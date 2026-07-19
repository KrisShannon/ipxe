/*
 * Copyright (C) 2026 Kris Shannon <kris@shannon.id.au>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

/** @file
 *
 * IPMI Keyboard Controller Style (KCS) system interface
 *
 * The KCS interface is the most common IPMI system interface, and
 * comprises a data register and a status/command register (typically
 * at I/O ports 0xca2 and 0xca3).  The interface is discovered via the
 * SMBIOS IPMI device information structure.
 *
 */

#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <byteswap.h>
#include <ipxe/io.h>
#include <ipxe/smbios.h>
#include <ipxe/ipmi.h>

/** Data register offset */
#define KCS_DATA 0

/** Status/command register offset */
#define KCS_STATUS 1

/** Output buffer (BMC to host) full */
#define KCS_STATUS_OBF 0x01

/** Input buffer (host to BMC) full */
#define KCS_STATUS_IBF 0x02

/** Extract interface state from status */
#define KCS_STATE( status ) ( ( (status) >> 6 ) & 0x3 )

/** Interface states */
#define KCS_STATE_IDLE 0x0
#define KCS_STATE_READ 0x1
#define KCS_STATE_WRITE 0x2
#define KCS_STATE_ERROR 0x3

/** Control codes (written to the command register) */
#define KCS_WRITE_START 0x61
#define KCS_WRITE_END 0x62

/** Control codes (written to the data register) */
#define KCS_READ 0x68

/** Timeout for status register flag changes (in microseconds)
 *
 * The specification allows the BMC up to five seconds, but any
 * healthy BMC will respond within a few milliseconds.
 */
#define KCS_TIMEOUT_US ( 2 * 1000 * 1000 )

/** Delay between status register polls (in microseconds) */
#define KCS_POLL_DELAY_US 10

/** A KCS system interface */
struct ipmi_kcs {
	/** Register base address */
	void *base;
	/** Register spacing */
	size_t spacing;
};

/** The KCS system interface */
static struct ipmi_kcs ipmi_kcs;

/**
 * Read KCS register
 *
 * @v kcs		KCS system interface
 * @v offset		Register offset
 * @ret data		Data
 */
static uint8_t kcs_read ( struct ipmi_kcs *kcs, unsigned int offset ) {

	return ioread8 ( kcs->base + ( offset * kcs->spacing ) );
}

/**
 * Write KCS register
 *
 * @v kcs		KCS system interface
 * @v offset		Register offset
 * @v data		Data
 */
static void kcs_write ( struct ipmi_kcs *kcs, unsigned int offset,
			uint8_t data ) {

	iowrite8 ( data, ( kcs->base + ( offset * kcs->spacing ) ) );
}

/**
 * Wait for BMC to accept input (IBF clear)
 *
 * @v kcs		KCS system interface
 * @ret status		Status register value, or negative error
 */
static int kcs_wait_input ( struct ipmi_kcs *kcs ) {
	uint8_t status;
	unsigned int i;

	for ( i = 0 ; i < ( KCS_TIMEOUT_US / KCS_POLL_DELAY_US ) ; i++ ) {
		status = kcs_read ( kcs, KCS_STATUS );
		if ( ! ( status & KCS_STATUS_IBF ) )
			return status;
		udelay ( KCS_POLL_DELAY_US );
	}
	DBGC ( kcs, "KCS %p timed out waiting for input buffer\n", kcs );
	return -ETIMEDOUT;
}

/**
 * Wait for BMC to provide output (OBF set)
 *
 * @v kcs		KCS system interface
 * @ret status		Status register value, or negative error
 */
static int kcs_wait_output ( struct ipmi_kcs *kcs ) {
	uint8_t status;
	unsigned int i;

	for ( i = 0 ; i < ( KCS_TIMEOUT_US / KCS_POLL_DELAY_US ) ; i++ ) {
		status = kcs_read ( kcs, KCS_STATUS );
		if ( status & KCS_STATUS_OBF )
			return status;
		udelay ( KCS_POLL_DELAY_US );
	}
	DBGC ( kcs, "KCS %p timed out waiting for output buffer\n", kcs );
	return -ETIMEDOUT;
}

/**
 * Discard any pending output (clear OBF)
 *
 * @v kcs		KCS system interface
 */
static void kcs_discard ( struct ipmi_kcs *kcs ) {

	if ( kcs_read ( kcs, KCS_STATUS ) & KCS_STATUS_OBF )
		kcs_read ( kcs, KCS_DATA );
}

/**
 * Write byte and wait for it to be consumed
 *
 * @v kcs		KCS system interface
 * @v offset		Register offset
 * @v data		Data
 * @ret status		Status register value, or negative error
 */
static int kcs_put ( struct ipmi_kcs *kcs, unsigned int offset,
		     uint8_t data ) {
	int status;

	kcs_write ( kcs, offset, data );
	if ( ( status = kcs_wait_input ( kcs ) ) < 0 )
		return status;
	if ( KCS_STATE ( status ) != KCS_STATE_WRITE ) {
		DBGC ( kcs, "KCS %p unexpected state %#02x during write\n",
		       kcs, status );
		return -EPROTO;
	}
	kcs_discard ( kcs );
	return status;
}

/**
 * Transmit message
 *
 * @v kcs		KCS system interface
 * @v data		Data
 * @v len		Length of data
 * @ret rc		Return status code
 */
static int kcs_transmit ( struct ipmi_kcs *kcs, const uint8_t *data,
			  size_t len ) {
	unsigned int i;
	int status;

	/* Sanity check */
	if ( ! len )
		return -EINVAL;

	/* Start transaction */
	if ( ( status = kcs_wait_input ( kcs ) ) < 0 )
		return status;
	kcs_discard ( kcs );
	if ( ( status = kcs_put ( kcs, KCS_STATUS, KCS_WRITE_START ) ) < 0 )
		return status;

	/* Write all data bytes except the last */
	for ( i = 0 ; i < ( len - 1 ) ; i++ ) {
		if ( ( status = kcs_put ( kcs, KCS_DATA, data[i] ) ) < 0 )
			return status;
	}

	/* End transaction and write the final data byte */
	if ( ( status = kcs_put ( kcs, KCS_STATUS, KCS_WRITE_END ) ) < 0 )
		return status;
	kcs_write ( kcs, KCS_DATA, data[ len - 1 ] );

	return 0;
}

/**
 * Receive message
 *
 * @v kcs		KCS system interface
 * @v data		Data buffer
 * @v max		Length of data buffer
 * @ret len		Length of received message, or negative error
 */
static int kcs_receive ( struct ipmi_kcs *kcs, uint8_t *data, size_t max ) {
	size_t len = 0;
	int status;

	while ( 1 ) {

		/* Wait for interface to advance to the next byte */
		if ( ( status = kcs_wait_input ( kcs ) ) < 0 )
			return status;

		/* Read byte, or terminate transaction, as applicable */
		switch ( KCS_STATE ( status ) ) {
		case KCS_STATE_READ:
			if ( ( status = kcs_wait_output ( kcs ) ) < 0 )
				return status;
			if ( len >= max ) {
				DBGC ( kcs, "KCS %p overlength response\n",
				       kcs );
				return -EMSGSIZE;
			}
			data[len++] = kcs_read ( kcs, KCS_DATA );
			kcs_write ( kcs, KCS_DATA, KCS_READ );
			break;
		case KCS_STATE_IDLE:
			if ( ( status = kcs_wait_output ( kcs ) ) < 0 )
				return status;
			kcs_read ( kcs, KCS_DATA );
			return len;
		default:
			DBGC ( kcs, "KCS %p unexpected state %#02x during "
			       "read\n", kcs, status );
			return -EPROTO;
		}
	}
}

/**
 * Transfer IPMI message
 *
 * @v request		IPMI request
 * @v request_len	Length of IPMI request
 * @v response		IPMI response buffer
 * @v response_max	Length of IPMI response buffer
 * @ret response_len	Length of IPMI response, or negative error
 */
static int ipmi_kcs_xfer ( const struct ipmi_request *request,
			   size_t request_len, struct ipmi_response *response,
			   size_t response_max ) {
	struct ipmi_kcs *kcs = &ipmi_kcs;
	int len;
	int rc;

	/* Transmit request */
	DBGC2 ( kcs, "KCS %p transmitting:\n", kcs );
	DBGC2_HDA ( kcs, 0, request, request_len );
	if ( ( rc = kcs_transmit ( kcs, ( ( const uint8_t * ) request ),
				   request_len ) ) != 0 )
		return rc;

	/* Receive response */
	len = kcs_receive ( kcs, ( ( uint8_t * ) response ), response_max );
	if ( len < 0 )
		return len;
	DBGC2 ( kcs, "KCS %p received:\n", kcs );
	DBGC2_HDA ( kcs, 0, response, len );

	return len;
}

/**
 * Open KCS system interface
 *
 * @ret rc		Return status code
 */
static int ipmi_kcs_open ( void ) {
	struct ipmi_kcs *kcs = &ipmi_kcs;
	const struct smbios_ipmi_information *info;
	const struct smbios_header *header;
	unsigned long address;
	uint8_t modifier;
	uint8_t status;

	/* Do nothing if interface is already open */
	if ( kcs->base )
		return 0;

	/* Locate SMBIOS IPMI device information */
	header = smbios_structure ( SMBIOS_TYPE_IPMI_INFORMATION, 0 );
	if ( ! header ) {
		DBGC ( kcs, "KCS %p found no SMBIOS IPMI device "
		       "information\n", kcs );
		return -ENODEV;
	}
	info = container_of ( header, struct smbios_ipmi_information, header );
	if ( header->len < ( offsetof ( typeof ( *info ), base_address ) +
			     sizeof ( info->base_address ) ) ) {
		DBGC ( kcs, "KCS %p underlength SMBIOS IPMI device "
		       "information\n", kcs );
		return -ENODEV;
	}

	/* Check interface type */
	if ( info->interface != SMBIOS_IPMI_INTERFACE_KCS ) {
		DBGC ( kcs, "KCS %p unsupported IPMI interface type %#02x\n",
		       kcs, info->interface );
		return -ENOTSUP;
	}

	/* Parse base address and modifier */
	modifier = ( ( header->len > offsetof ( typeof ( *info ), modifier ) )
		     ? info->modifier : 0 );
	address = ( ( le64_to_cpu ( info->base_address ) &
		      ~( ( uint64_t ) SMBIOS_IPMI_BASE_IO ) ) |
		    SMBIOS_IPMI_LSB ( modifier ) );
	kcs->spacing = SMBIOS_IPMI_SPACING ( modifier );
	if ( info->base_address & SMBIOS_IPMI_BASE_IO ) {
		kcs->base = ( ( void * ) ( intptr_t ) address );
		DBGC ( kcs, "KCS %p at I/O port %#04lx (spacing %zd)\n",
		       kcs, address, kcs->spacing );
	} else {
		kcs->base = ioremap ( address, ( kcs->spacing + 1 ) );
		if ( ! kcs->base ) {
			DBGC ( kcs, "KCS %p could not map %#08lx\n",
			       kcs, address );
			return -ENOMEM;
		}
		DBGC ( kcs, "KCS %p at memory address %#08lx (spacing "
		       "%zd)\n", kcs, address, kcs->spacing );
	}

	/* Check that the interface seems to be present */
	status = kcs_read ( kcs, KCS_STATUS );
	if ( status == 0xff ) {
		DBGC ( kcs, "KCS %p interface seems to be absent\n", kcs );
		if ( ! ( info->base_address & SMBIOS_IPMI_BASE_IO ) )
			iounmap ( kcs->base );
		kcs->base = NULL;
		return -ENODEV;
	}

	return 0;
}

/** KCS system interface transport */
struct ipmi_transport ipmi_kcs_transport __ipmi_transport = {
	.name = "KCS",
	.open = ipmi_kcs_open,
	.xfer = ipmi_kcs_xfer,
};
