#ifndef _IPXE_DCMI_H
#define _IPXE_DCMI_H

/** @file
 *
 * Data Center Manageability Interface
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>

/** DCMI group extension identification */
#define DCMI_GROUP_ID 0xdc

/** Get Asset Tag command */
#define DCMI_GET_ASSET_TAG 0x06

/** Set Asset Tag command */
#define DCMI_SET_ASSET_TAG 0x08

/** Maximum length of an asset tag */
#define DCMI_ASSET_TAG_MAX 64

/** Maximum number of asset tag bytes per command */
#define DCMI_ASSET_TAG_CHUNK 16

/** DCMI Get Asset Tag request */
struct dcmi_get_asset_tag_request {
	/** Group extension identification */
	uint8_t group;
	/** Offset to read */
	uint8_t offset;
	/** Number of bytes to read */
	uint8_t count;
} __attribute__ (( packed ));

/** DCMI Get Asset Tag response */
struct dcmi_get_asset_tag_response {
	/** Group extension identification */
	uint8_t group;
	/** Total asset tag length */
	uint8_t total;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** DCMI Set Asset Tag request */
struct dcmi_set_asset_tag_request {
	/** Group extension identification */
	uint8_t group;
	/** Offset to write */
	uint8_t offset;
	/** Number of bytes to write */
	uint8_t count;
	/** Data */
	uint8_t data[0];
} __attribute__ (( packed ));

/** DCMI Set Asset Tag response */
struct dcmi_set_asset_tag_response {
	/** Group extension identification */
	uint8_t group;
	/** Total asset tag length */
	uint8_t total;
} __attribute__ (( packed ));

#endif /* _IPXE_DCMI_H */
