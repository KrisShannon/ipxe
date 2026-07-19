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

/** Mock BMC LAN channel number */
#define MOCK_LAN_CHANNEL 2

/** Highest mock BMC LAN parameter number */
#define MOCK_LAN_PARAM_MAX 20

/** Mock BMC LAN parameter storage */
static uint8_t mock_lan[ MOCK_LAN_PARAM_MAX + 1 ][8] = {
	[3] = { 192, 168, 10, 20 },
	[4] = { 0x02 },
	[5] = { 0x00, 0x25, 0x90, 0xab, 0xcd, 0xef },
	[6] = { 255, 255, 255, 0 },
	[12] = { 192, 168, 10, 1 },
	[20] = { 0x00, 0x00 },
};

/** Mock BMC LAN parameter lengths */
static uint8_t mock_lan_len[ MOCK_LAN_PARAM_MAX + 1 ] = {
	[0] = 1, [3] = 4, [4] = 1, [5] = 6, [6] = 4, [12] = 4, [20] = 2,
};

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

	} else if ( ( netfn == IPMI_NETFN_APP ) && ( data_len >= 1 ) &&
		    ( request->command == IPMI_GET_CHANNEL_INFO ) ) {

		struct ipmi_channel_info info;

		memset ( &info, 0, sizeof ( info ) );
		info.channel = request->data[0];
		if ( info.channel == MOCK_LAN_CHANNEL ) {
			info.medium = IPMI_MEDIUM_LAN;
		} else if ( info.channel <= 1 ) {
			info.medium = 0x01; /* IPMB */
		} else {
			response->code = 0xcc; /* Invalid data field */
			return len;
		}
		memcpy ( response->data, &info, sizeof ( info ) );
		len += sizeof ( info );

	} else if ( ( netfn == IPMI_NETFN_TRANSPORT ) && ( data_len >= 4 ) &&
		    ( request->command == IPMI_GET_LAN_CONFIG ) ) {

		unsigned int param = request->data[1];

		if ( ( request->data[0] != MOCK_LAN_CHANNEL ) ||
		     ( param > MOCK_LAN_PARAM_MAX ) ||
		     ( ! mock_lan_len[param] ) ) {
			response->code = 0x80; /* Parameter not supported */
			return len;
		}
		response->data[0] = 0x11; /* Parameter revision */
		memcpy ( &response->data[1], mock_lan[param],
			 mock_lan_len[param] );
		len += ( 1 + mock_lan_len[param] );

	} else if ( ( netfn == IPMI_NETFN_TRANSPORT ) && ( data_len >= 2 ) &&
		    ( request->command == IPMI_SET_LAN_CONFIG ) ) {

		unsigned int param = request->data[1];
		size_t param_len = ( data_len - 2 );

		if ( ( request->data[0] != MOCK_LAN_CHANNEL ) ||
		     ( param > MOCK_LAN_PARAM_MAX ) ||
		     ( ! mock_lan_len[param] ) ||
		     ( param_len != mock_lan_len[param] ) ) {
			response->code = 0x80; /* Parameter not supported */
			return len;
		}
		memcpy ( mock_lan[param], &request->data[2], param_len );

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
 * Report a named setting fetch test result
 *
 * @v name		Qualified setting name (e.g. "dcmi/key")
 * @v expected		Expected value, or NULL if fetching should fail
 * @v file		Test code file
 * @v line		Test code line
 */
static void fetchf_name_okx ( const char *name, const char *expected,
			      const char *file, unsigned int line ) {
	char tmp[ strlen ( name ) + 1 /* NUL */ ];
	char value[ DCMI_ASSET_TAG_MAX + 1 /* NUL */ ];
	struct settings *settings;
	struct setting setting;
	int len;

	/* Look up setting via its qualified name */
	strcpy ( tmp, name );
	okx ( parse_setting_name ( tmp, find_child_settings, &settings,
				   &setting ) == 0, file, line );

	/* Fetch value */
	len = fetchf_setting ( settings, &setting, NULL, NULL, value,
			       sizeof ( value ) );
	if ( expected ) {
		okx ( len == ( int ) strlen ( expected ), file, line );
		okx ( strcmp ( value, expected ) == 0, file, line );
	} else {
		okx ( len < 0, file, line );
	}
}
#define fetchf_name_ok( name, expected ) \
	fetchf_name_okx ( name, expected, __FILE__, __LINE__ )
#define dcmi_key_ok( name, expected ) \
	fetchf_name_okx ( name, expected, __FILE__, __LINE__ )

/**
 * Report a named setting store test result
 *
 * @v name		Qualified setting name (e.g. "bmc/ipaddr")
 * @v value		Formatted value to store
 * @v file		Test code file
 * @v line		Test code line
 */
static void storef_name_okx ( const char *name, const char *value,
			      const char *file, unsigned int line ) {
	char tmp[ strlen ( name ) + 1 /* NUL */ ];
	struct settings *settings;
	struct setting setting;

	/* Look up setting via its qualified name */
	strcpy ( tmp, name );
	okx ( parse_setting_name ( tmp, find_child_settings, &settings,
				   &setting ) == 0, file, line );

	/* Store value */
	okx ( storef_setting ( settings, &setting, value ) == 0, file, line );
}
#define storef_name_ok( name, value ) \
	storef_name_okx ( name, value, __FILE__, __LINE__ )

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

	/* Verify key=value lookups within the asset tag */
	dcmi_ok ( settings, "env=prod mgmt_vlan=1234 vlan=99 empty= last=x" );
	dcmi_key_ok ( "dcmi/mgmt_vlan", "1234" );
	dcmi_key_ok ( "dcmi/env", "prod" );
	dcmi_key_ok ( "dcmi/vlan", "99" );
	dcmi_key_ok ( "dcmi/empty", "" );
	dcmi_key_ok ( "dcmi/last", "x" );
	dcmi_key_ok ( "dcmi/missing", NULL );
	dcmi_key_ok ( "dcmi/mgmt", NULL );
	dcmi_key_ok ( "dcmi/prod", NULL );

	/* Verify clearing the tag */
	ok ( delete_setting ( settings, &dcmi_assettag_setting ) == 0 );
	ok ( mock_tag_len == 0 );
	len = fetch_setting ( settings, &dcmi_assettag_setting, NULL, NULL,
			      raw, sizeof ( raw ) );
	ok ( len == 0 );
	dcmi_key_ok ( "dcmi/mgmt_vlan", NULL );
}

/** DCMI self-test */
struct self_test dcmi_test __self_test = {
	.name = "dcmi",
	.exec = dcmi_test_exec,
};

/**
 * Perform BMC LAN settings self-tests
 *
 */
static void bmc_test_exec ( void ) {
	struct settings *settings;

	/* Locate settings block (registered at initialisation) */
	settings = find_settings ( "bmc" );
	ok ( settings != NULL );
	if ( ! settings )
		return;

	/* Verify fetching of named parameters */
	fetchf_name_ok ( "bmc/ipaddr", "192.168.10.20" );
	fetchf_name_ok ( "bmc/subnet", "255.255.255.0" );
	fetchf_name_ok ( "bmc/defgw", "192.168.10.1" );
	fetchf_name_ok ( "bmc/macaddr", "00:25:90:ab:cd:ef" );
	fetchf_name_ok ( "bmc/ipsrc", "2" );

	/* Verify storing of named parameters */
	storef_name_ok ( "bmc/ipaddr", "10.1.2.3" );
	ok ( memcmp ( mock_lan[3], "\x0a\x01\x02\x03", 4 ) == 0 );
	fetchf_name_ok ( "bmc/ipaddr", "10.1.2.3" );
	storef_name_ok ( "bmc/ipsrc", "1" );
	ok ( mock_lan[4][0] == 0x01 );

	/* Verify numeric parameter access */
	fetchf_name_ok ( "bmc/12:ipv4", "192.168.10.1" );
	storef_name_ok ( "bmc/12:ipv4", "10.1.2.254" );
	fetchf_name_ok ( "bmc/defgw", "10.1.2.254" );

	/* Verify VLAN ID translation */
	fetchf_name_ok ( "bmc/vlanid", "0" );
	storef_name_ok ( "bmc/vlanid", "1234" );
	ok ( mock_lan[20][0] == 0xd2 );
	ok ( mock_lan[20][1] == 0x84 );
	fetchf_name_ok ( "bmc/vlanid", "1234" );
	storef_name_ok ( "bmc/vlanid", "0" );
	ok ( mock_lan[20][0] == 0x00 );
	ok ( mock_lan[20][1] == 0x00 );
	fetchf_name_ok ( "bmc/vlanid", "0" );

	/* Verify unknown and unsupported parameters */
	fetchf_name_ok ( "bmc/bogus", NULL );
	fetchf_name_ok ( "bmc/7", NULL );
}

/** BMC LAN settings self-test */
struct self_test bmc_test __self_test = {
	.name = "bmc",
	.exec = bmc_test_exec,
};

/* Drag in objects under test that are not referenced by symbol */
REQUIRING_SYMBOL ( dcmi_test );
REQUIRE_OBJECT ( ipmi_lan );
