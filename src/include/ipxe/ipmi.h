#ifndef _IPXE_IPMI_H
#define _IPXE_IPMI_H

/** @file
 *
 * Intelligent Platform Management Interface
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <stddef.h>
#include <ipxe/tables.h>

/** An IPMI request */
struct ipmi_request {
	/** Network function and LUN */
	uint8_t netfn_lun;
	/** Command */
	uint8_t command;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** An IPMI response */
struct ipmi_response {
	/** Network function and LUN */
	uint8_t netfn_lun;
	/** Command */
	uint8_t command;
	/** Completion code */
	uint8_t code;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** Construct network function and LUN byte */
#define IPMI_NETFN_LUN( netfn, lun ) ( ( (netfn) << 2 ) | (lun) )

/** Extract network function */
#define IPMI_NETFN( netfn_lun ) ( (netfn_lun) >> 2 )

/** Application network function */
#define IPMI_NETFN_APP 0x06

/** Group extension network function */
#define IPMI_NETFN_GROUP 0x2c

/** Get Device ID command */
#define IPMI_GET_DEVICE_ID 0x01

/** Get Device ID response */
struct ipmi_device_id {
	/** Device ID */
	uint8_t id;
	/** Device revision */
	uint8_t revision;
	/** Firmware major revision */
	uint8_t fw_major;
	/** Firmware minor revision (BCD) */
	uint8_t fw_minor;
	/** IPMI specification version (BCD) */
	uint8_t version;
	/** Additional device support */
	uint8_t support;
	/** Manufacturer ID */
	uint8_t mfg[3];
	/** Product ID */
	uint8_t product[2];
} __attribute__ (( packed ));

/** Command completed normally */
#define IPMI_CC_OK 0x00

/** Node busy */
#define IPMI_CC_BUSY 0xc0

/** Invalid command */
#define IPMI_CC_INVALID_COMMAND 0xc1

/** Timeout while processing command */
#define IPMI_CC_TIMEOUT 0xc3

/** Maximum length of IPMI message data
 *
 * The KCS system interface is specified to allow message lengths of
 * at least 40 bytes.  All messages that we send or expect to receive
 * are substantially shorter than this.
 */
#define IPMI_MAX_DATA 32

/** An IPMI system interface transport */
struct ipmi_transport {
	/** Name */
	const char *name;
	/**
	 * Open transport
	 *
	 * @ret rc		Return status code
	 */
	int ( * open ) ( void );
	/**
	 * Transfer message
	 *
	 * @v request		IPMI request
	 * @v request_len	Length of IPMI request
	 * @v response		IPMI response buffer
	 * @v response_max	Length of IPMI response buffer
	 * @ret response_len	Length of IPMI response, or negative error
	 */
	int ( * xfer ) ( const struct ipmi_request *request,
			 size_t request_len, struct ipmi_response *response,
			 size_t response_max );
};

/** IPMI system interface transport table */
#define IPMI_TRANSPORTS __table ( struct ipmi_transport, "ipmi_transports" )

/** Declare an IPMI system interface transport */
#define __ipmi_transport __table_entry ( IPMI_TRANSPORTS, 01 )

extern int ipmi_open ( void );
extern int ipmi_message ( unsigned int netfn, unsigned int command,
			  const void *data, size_t len, void *response,
			  size_t response_max );

#endif /* _IPXE_IPMI_H */
