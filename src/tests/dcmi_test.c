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

/** @file
 *
 * DCMI self-tests
 *
 * These tests use a mock IPMI system interface transport which
 * emulates a BMC providing the DCMI asset tag commands.
 *
 */

/* Forcibly enable assertions */
#undef NDEBUG

#include <string.h>
#include <assert.h>
#include <ipxe/settings.h>
#include <ipxe/ipmi.h>
#include <ipxe/dcmi.h>
#include <ipxe/test.h>

/** Mock BMC asset tag storage */
static uint8_t mock_tag[DCMI_ASSET_TAG_MAX];

/** Mock BMC asset tag length */
static size_t mock_tag_len;

/**
 * Open mock transport
 *
 * @ret rc		Return status code
 */
static int mock_open ( void ) {
	return 0;
}

/**
 * Transfer IPMI message via mock transport
 *
 * @v request		IPMI request
 * @v request_len	Length of IPMI request
 * @v response		IPMI response buffer
 * @v response_max	Length of IPMI response buffer
 * @ret response_len	Length of IPMI response, or negative error
 */
static int mock_xfer ( const struct ipmi_request *request, size_t request_len,
		       struct ipmi_response *response, size_t response_max ) {
	static const struct ipmi_device_id mock_id = {
		.id = 0x22,
		.version = 0x02,
	};
	unsigned int netfn = IPMI_NETFN ( request->netfn_lun );
	size_t data_len = ( request_len - sizeof ( *request ) );
	size_t len = sizeof ( *response );
	size_t offset;
	size_t count;

	/* Construct response header */
	assert ( response_max >= ( sizeof ( *response ) + IPMI_MAX_DATA ) );
	response->netfn_lun = IPMI_NETFN_LUN ( ( netfn | 1 ), 0 );
	response->command = request->command;
	response->code = IPMI_CC_OK;

	/* Emulate a BMC supporting only Get Device ID and DCMI asset tags */
	if ( ( netfn == IPMI_NETFN_APP ) &&
	     ( request->command == IPMI_GET_DEVICE_ID ) ) {

		memcpy ( response->data, &mock_id, sizeof ( mock_id ) );
		len += sizeof ( mock_id );

	} else if ( ( netfn == IPMI_NETFN_GROUP ) && ( data_len >= 3 ) &&
		    ( request->data[0] == DCMI_GROUP_ID ) &&
		    ( request->command == DCMI_GET_ASSET_TAG ) ) {

		offset = request->data[1];
		count = request->data[2];
		if ( offset > mock_tag_len )
			offset = mock_tag_len;
		if ( count > ( mock_tag_len - offset ) )
			count = ( mock_tag_len - offset );
		response->data[0] = DCMI_GROUP_ID;
		response->data[1] = mock_tag_len;
		memcpy ( &response->data[2], &mock_tag[offset], count );
		len += ( 2 + count );

	} else if ( ( netfn == IPMI_NETFN_GROUP ) && ( data_len >= 3 ) &&
		    ( request->data[0] == DCMI_GROUP_ID ) &&
		    ( request->command == DCMI_SET_ASSET_TAG ) ) {

		offset = request->data[1];
		count = request->data[2];
		if ( ( ( offset + count ) > sizeof ( mock_tag ) ) ||
		     ( count != ( data_len - 3 ) ) ) {
			response->code = 0xc9; /* Parameter out of range */
		} else {
			memcpy ( &mock_tag[offset], &request->data[3], count );
			mock_tag_len = ( offset + count );
		}
		response->data[0] = DCMI_GROUP_ID;
		response->data[1] = mock_tag_len;
		len += 2;

	} else {

		response->code = IPMI_CC_INVALID_COMMAND;
	}

	return len;
}

/** Mock IPMI system interface transport
 *
 * Use an explicit table entry order to ensure that the mock transport
 * is opened in preference to any real transport included in the test
 * build.
 */
struct ipmi_transport mock_transport __table_entry ( IPMI_TRANSPORTS, 00 ) = {
	.name = "MOCK",
	.open = mock_open,
	.xfer = mock_xfer,
};

/**
 * Report an asset tag store and fetch test result
 *
 * @v settings		Settings block
 * @v tag		Asset tag
 * @v file		Test code file
 * @v line		Test code line
 */
static void dcmi_okx ( struct settings *settings, const char *tag,
		       const char *file, unsigned int line ) {
	char fetched[ DCMI_ASSET_TAG_MAX + 1 /* NUL */ ];
	int len;

	/* Store asset tag */
	okx ( storef_setting ( settings, &dcmi_assettag_setting, tag ) == 0,
	      file, line );
	okx ( mock_tag_len == strlen ( tag ), file, line );
	okx ( memcmp ( mock_tag, tag, mock_tag_len ) == 0, file, line );

	/* Fetch asset tag */
	len = fetchf_setting ( settings, &dcmi_assettag_setting, NULL, NULL,
			       fetched, sizeof ( fetched ) );
	okx ( len == ( int ) strlen ( tag ), file, line );
	okx ( strcmp ( fetched, tag ) == 0, file, line );
}
#define dcmi_ok( settings, tag ) \
	dcmi_okx ( settings, tag, __FILE__, __LINE__ )

/**
 * Perform DCMI self-tests
 *
 */
static void dcmi_test_exec ( void ) {
	struct settings *settings;
	char overlong[ DCMI_ASSET_TAG_MAX + 2 /* extra character and NUL */ ];
	uint8_t raw[DCMI_ASSET_TAG_MAX];
	int len;

	/* Locate settings block (registered at initialisation) */
	settings = find_settings ( "dcmi" );
	ok ( settings != NULL );
	if ( ! settings )
		return;

	/* Verify empty asset tag */
	len = fetch_setting ( settings, &dcmi_assettag_setting, NULL, NULL,
			      raw, sizeof ( raw ) );
	ok ( len == 0 );

	/* Verify tag shorter than a single chunk */
	dcmi_ok ( settings, "hello" );

	/* Verify tag of exactly one chunk */
	dcmi_ok ( settings, "0123456789abcdef" );

	/* Verify tag requiring multiple chunks */
	dcmi_ok ( settings, "datacenter-rack42-u07-node3-asset-999999" );

	/* Verify maximum length tag */
	dcmi_ok ( settings, "0123456789abcdef0123456789abcdef"
		  "0123456789abcdef0123456789abcdef" );

	/* Verify replacement with a shorter tag */
	dcmi_ok ( settings, "shorter" );

	/* Verify fetch into a short buffer */
	len = fetch_setting ( settings, &dcmi_assettag_setting, NULL, NULL,
			      raw, 3 );
	ok ( len == ( int ) strlen ( "shorter" ) );
	ok ( memcmp ( raw, "sho", 3 ) == 0 );

	/* Verify overlong tag is rejected */
	memset ( overlong, 'x', ( sizeof ( overlong ) - 1 ) );
	overlong[ sizeof ( overlong ) - 1 ] = '\0';
	ok ( storef_setting ( settings, &dcmi_assettag_setting,
			      overlong ) != 0 );
	ok ( mock_tag_len == strlen ( "shorter" ) );

	/* Verify clearing the tag */
	ok ( delete_setting ( settings, &dcmi_assettag_setting ) == 0 );
	ok ( mock_tag_len == 0 );
	len = fetch_setting ( settings, &dcmi_assettag_setting, NULL, NULL,
			      raw, sizeof ( raw ) );
	ok ( len == 0 );
}

/** DCMI self-test */
struct self_test dcmi_test __self_test = {
	.name = "dcmi",
	.exec = dcmi_test_exec,
};
