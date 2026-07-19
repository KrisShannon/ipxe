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
 * Intelligent Platform Management Interface
 *
 * This provides access to a local baseboard management controller via
 * an IPMI system interface, for issuing IPMI commands from the host
 * to its own BMC.  There is no support for (and no intention to ever
 * support) remote IPMI.
 *
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ipxe/ipmi.h>

/** Number of attempts for a busy BMC */
#define IPMI_MAX_ATTEMPTS 3

/** Delay between attempts for a busy BMC (in milliseconds) */
#define IPMI_RETRY_DELAY_MS 10

/** Active IPMI system interface transport */
static struct ipmi_transport *ipmi;

/**
 * Issue IPMI message
 *
 * @v netfn		Network function
 * @v command		Command
 * @v data		Request data
 * @v len		Length of request data
 * @v response		Response data buffer
 * @v response_max	Length of response data buffer
 * @ret len		Length of response data, or negative error
 *
 * The actual length of the response data will be returned even if the
 * buffer was too small.
 */
int ipmi_message ( unsigned int netfn, unsigned int command,
		   const void *data, size_t len, void *response,
		   size_t response_max ) {
	union {
		struct ipmi_request request;
		uint8_t bytes[ sizeof ( struct ipmi_request ) + IPMI_MAX_DATA ];
	} tx;
	union {
		struct ipmi_response response;
		uint8_t bytes[ sizeof ( struct ipmi_response ) +
			       IPMI_MAX_DATA ];
	} rx;
	unsigned int attempt;
	size_t rx_data_len;
	int rx_len;
	int rc;

	/* Open system interface, if applicable */
	if ( ( rc = ipmi_open() ) != 0 )
		return rc;

	/* Sanity check */
	if ( len > IPMI_MAX_DATA )
		return -EMSGSIZE;

	/* Construct request */
	tx.request.netfn_lun = IPMI_NETFN_LUN ( netfn, 0 );
	tx.request.command = command;
	memcpy ( tx.request.data, data, len );

	/* Issue request, retrying if the BMC is busy */
	for ( attempt = 1 ; ; attempt++ ) {

		/* Transfer message */
		rx_len = ipmi->xfer ( &tx.request,
				      ( sizeof ( tx.request ) + len ),
				      &rx.response, sizeof ( rx ) );
		if ( rx_len < 0 ) {
			rc = rx_len;
			DBGC ( &ipmi, "IPMI could not issue netfn %#02x "
			       "command %#02x: %s\n", netfn, command,
			       strerror ( rc ) );
			return rc;
		}

		/* Validate response */
		if ( ( ( ( size_t ) rx_len ) < sizeof ( rx.response ) ) ||
		     ( IPMI_NETFN ( rx.response.netfn_lun ) !=
		       ( netfn | 1 ) ) ||
		     ( rx.response.command != command ) ) {
			DBGC ( &ipmi, "IPMI received malformed response to "
			       "netfn %#02x command %#02x:\n", netfn,
			       command );
			DBGC_HDA ( &ipmi, 0, rx.bytes, rx_len );
			return -EPROTO;
		}

		/* Retry if, and only if, the BMC reports itself busy */
		if ( rx.response.code != IPMI_CC_BUSY )
			break;
		if ( attempt >= IPMI_MAX_ATTEMPTS )
			break;
		mdelay ( IPMI_RETRY_DELAY_MS );
	}

	/* Check completion code */
	if ( rx.response.code != IPMI_CC_OK ) {
		DBGC ( &ipmi, "IPMI netfn %#02x command %#02x failed with "
		       "completion code %#02x\n", netfn, command,
		       rx.response.code );
		switch ( rx.response.code ) {
		case IPMI_CC_INVALID_COMMAND:
			return -ENOTSUP;
		case IPMI_CC_TIMEOUT:
			return -ETIMEDOUT;
		case IPMI_CC_BUSY:
			return -EBUSY;
		default:
			return -EIO;
		}
	}

	/* Copy response data */
	rx_data_len = ( rx_len - sizeof ( rx.response ) );
	if ( response_max > rx_data_len )
		response_max = rx_data_len;
	memcpy ( response, rx.response.data, response_max );

	return rx_data_len;
}

/**
 * Open IPMI system interface
 *
 * @ret rc		Return status code
 */
int ipmi_open ( void ) {
	struct ipmi_transport *transport;
	struct ipmi_device_id id;
	int len;
	int rc = -ENODEV;

	/* Do nothing if an interface is already open */
	if ( ipmi )
		return 0;

	/* Open the first available transport */
	for_each_table_entry ( transport, IPMI_TRANSPORTS ) {
		if ( ( rc = transport->open() ) == 0 ) {
			ipmi = transport;
			break;
		}
	}
	if ( ! ipmi ) {
		DBGC ( &ipmi, "IPMI found no usable system interface\n" );
		return rc;
	}

	/* Check that the BMC is responsive */
	len = ipmi_message ( IPMI_NETFN_APP, IPMI_GET_DEVICE_ID, NULL, 0,
			     &id, sizeof ( id ) );
	if ( len < 0 ) {
		rc = len;
		DBGC ( &ipmi, "IPMI %s could not get device ID: %s\n",
		       ipmi->name, strerror ( rc ) );
		ipmi = NULL;
		return rc;
	}
	if ( ( ( size_t ) len ) < sizeof ( id ) ) {
		DBGC ( &ipmi, "IPMI %s underlength device ID\n", ipmi->name );
		ipmi = NULL;
		return -EPROTO;
	}
	DBGC ( &ipmi, "IPMI %s found IPMI %x.%x BMC (device %#02x firmware "
	       "%d.%02x)\n", ipmi->name, ( id.version & 0x0f ),
	       ( id.version >> 4 ), id.id, ( id.fw_major & 0x7f ),
	       id.fw_minor );

	return 0;
}
