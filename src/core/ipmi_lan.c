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
 * BMC LAN configuration settings
 *
 * The BMC's LAN configuration parameters (for the first LAN channel)
 * are exposed as the settings block "bmc".  The common parameters are
 * available as named settings (e.g. "bmc/ipaddr"), and any parameter
 * may be accessed numerically (e.g. "bmc/3:ipv4" or "bmc/194:hex").
 *
 * Note that the deliberately distinct setting names avoid any
 * collision with the core network settings ("ip", "netmask", etc.),
 * which would otherwise become ambiguous in unqualified lookups.
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ipxe/settings.h>
#include <ipxe/init.h>
#include <ipxe/ipmi.h>

/** Maximum length of a LAN configuration parameter */
#define BMC_LAN_PARAM_MAX 30

/** BMC LAN settings scope */
static const struct settings_scope bmc_settings_scope;

/** BMC LAN channel number, or negative error */
static int bmc_channel;

/** IP address setting */
const struct setting bmc_ipaddr_setting __setting ( SETTING_HOST_EXTRA,
						    ipaddr ) = {
	.name = "ipaddr",
	.description = "BMC IP address",
	.tag = IPMI_LAN_PARAM_IP,
	.type = &setting_type_ipv4,
	.scope = &bmc_settings_scope,
};

/** Subnet mask setting */
const struct setting bmc_subnet_setting __setting ( SETTING_HOST_EXTRA,
						    subnet ) = {
	.name = "subnet",
	.description = "BMC subnet mask",
	.tag = IPMI_LAN_PARAM_NETMASK,
	.type = &setting_type_ipv4,
	.scope = &bmc_settings_scope,
};

/** Default gateway setting */
const struct setting bmc_defgw_setting __setting ( SETTING_HOST_EXTRA,
						   defgw ) = {
	.name = "defgw",
	.description = "BMC default gateway",
	.tag = IPMI_LAN_PARAM_GATEWAY,
	.type = &setting_type_ipv4,
	.scope = &bmc_settings_scope,
};

/** MAC address setting */
const struct setting bmc_macaddr_setting __setting ( SETTING_HOST_EXTRA,
						     macaddr ) = {
	.name = "macaddr",
	.description = "BMC MAC address",
	.tag = IPMI_LAN_PARAM_MAC,
	.type = &setting_type_hex,
	.scope = &bmc_settings_scope,
};

/** IP address source setting */
const struct setting bmc_ipsrc_setting __setting ( SETTING_HOST_EXTRA,
						   ipsrc ) = {
	.name = "ipsrc",
	.description = "BMC IP address source",
	.tag = IPMI_LAN_PARAM_IP_SOURCE,
	.type = &setting_type_int8,
	.scope = &bmc_settings_scope,
};

/** VLAN ID setting */
const struct setting bmc_vlanid_setting __setting ( SETTING_HOST_EXTRA,
						    vlanid ) = {
	.name = "vlanid",
	.description = "BMC VLAN ID",
	.tag = IPMI_LAN_PARAM_VLAN,
	.type = &setting_type_int16,
	.scope = &bmc_settings_scope,
};

/**
 * Identify BMC LAN channel
 *
 * @ret channel		Channel number, or negative error
 */
static int bmc_lan_channel ( void ) {
	struct ipmi_channel_info info;
	uint8_t channel;
	int len;

	/* Use cached channel number, if available */
	if ( bmc_channel )
		return bmc_channel;

	/* Scan for the first LAN channel */
	for ( channel = 1 ; channel <= IPMI_CHANNEL_MAX ; channel++ ) {
		len = ipmi_message ( IPMI_NETFN_APP, IPMI_GET_CHANNEL_INFO,
				     &channel, sizeof ( channel ),
				     &info, sizeof ( info ) );
		if ( len < 0 )
			continue;
		if ( ( ( size_t ) len ) < sizeof ( info ) )
			continue;
		if ( ( info.medium & 0x7f ) == IPMI_MEDIUM_LAN ) {
			bmc_channel = info.channel;
			DBGC ( &bmc_channel, "BMC LAN using channel %d\n",
			       bmc_channel );
			return bmc_channel;
		}
	}

	DBGC ( &bmc_channel, "BMC LAN found no LAN channel\n" );
	bmc_channel = -ENODEV;
	return bmc_channel;
}

/**
 * Read LAN configuration parameter
 *
 * @v param		Parameter number
 * @v data		Buffer to fill with parameter data
 * @v len		Length of buffer
 * @ret len		Length of parameter data, or negative error
 */
static int bmc_lan_read ( unsigned int param, void *data, size_t len ) {
	uint8_t request[4];
	uint8_t response[ 1 /* revision */ + BMC_LAN_PARAM_MAX ];
	size_t rsp_data_len;
	int channel;
	int rsp_len;

	/* Identify channel */
	if ( ( channel = bmc_lan_channel() ) < 0 )
		return channel;

	/* Issue Get LAN Configuration Parameters */
	request[0] = channel;
	request[1] = param;
	request[2] = 0; /* Set selector */
	request[3] = 0; /* Block selector */
	rsp_len = ipmi_message ( IPMI_NETFN_TRANSPORT, IPMI_GET_LAN_CONFIG,
				 request, sizeof ( request ),
				 response, sizeof ( response ) );
	if ( rsp_len < 0 )
		return rsp_len;
	if ( rsp_len < 1 /* revision */ )
		return -EPROTO;

	/* Copy parameter data (which follows the parameter revision) */
	rsp_data_len = ( rsp_len - 1 );
	if ( len > rsp_data_len )
		len = rsp_data_len;
	memcpy ( data, &response[1], len );

	return rsp_data_len;
}

/**
 * Write LAN configuration parameter
 *
 * @v param		Parameter number
 * @v data		Parameter data
 * @v len		Length of parameter data
 * @ret rc		Return status code
 */
static int bmc_lan_write ( unsigned int param, const void *data, size_t len ) {
	uint8_t request[ 2 /* channel and parameter */ + BMC_LAN_PARAM_MAX ];
	uint8_t progress[3];
	int channel;
	int rc;

	/* Sanity check */
	if ( len > BMC_LAN_PARAM_MAX )
		return -EMSGSIZE;

	/* Identify channel */
	if ( ( channel = bmc_lan_channel() ) < 0 )
		return channel;

	/* Signal set in progress (ignoring errors, since this
	 * parameter is optional for the BMC to implement)
	 */
	progress[0] = channel;
	progress[1] = IPMI_LAN_PARAM_SET_IN_PROGRESS;
	progress[2] = IPMI_LAN_SET_IN_PROGRESS;
	ipmi_message ( IPMI_NETFN_TRANSPORT, IPMI_SET_LAN_CONFIG,
		       progress, sizeof ( progress ), NULL, 0 );

	/* Issue Set LAN Configuration Parameters */
	request[0] = channel;
	request[1] = param;
	memcpy ( &request[2], data, len );
	rc = ipmi_message ( IPMI_NETFN_TRANSPORT, IPMI_SET_LAN_CONFIG,
			    request, ( 2 + len ), NULL, 0 );

	/* Signal set complete (ignoring errors, as above) */
	progress[2] = IPMI_LAN_SET_COMPLETE;
	ipmi_message ( IPMI_NETFN_TRANSPORT, IPMI_SET_LAN_CONFIG,
		       progress, sizeof ( progress ), NULL, 0 );

	return ( ( rc < 0 ) ? rc : 0 );
}

/**
 * Check applicability of BMC LAN setting
 *
 * @v settings		Settings block
 * @v setting		Setting
 * @ret applies		Setting applies within this settings block
 */
static int bmc_applies ( struct settings *settings __unused,
			 const struct setting *setting ) {

	return ( setting->scope == &bmc_settings_scope );
}

/**
 * Fetch value of BMC LAN setting
 *
 * @v settings		Settings block
 * @v setting		Setting to fetch
 * @v data		Buffer to fill with setting data
 * @v len		Length of buffer
 * @ret len		Length of setting data, or negative error
 */
static int bmc_fetch ( struct settings *settings __unused,
		       struct setting *setting, void *data, size_t len ) {
	uint8_t raw[2];
	unsigned int vlan;
	int raw_len;

	/* Refuse to fetch settings with no parameter number */
	if ( ! setting->tag )
		return -ENOENT;

	/* Translate the VLAN ID parameter, which encodes an enable
	 * bit alongside the (little-endian) VLAN ID
	 */
	if ( setting_cmp ( setting, &bmc_vlanid_setting ) == 0 ) {
		raw_len = bmc_lan_read ( setting->tag, raw, sizeof ( raw ) );
		if ( raw_len < 0 )
			return raw_len;
		if ( ( ( size_t ) raw_len ) < sizeof ( raw ) )
			return -EPROTO;
		vlan = ( ( raw[1] & IPMI_LAN_VLAN_ENABLED ) ?
			 ( ( ( raw[1] & 0x0f ) << 8 ) | raw[0] ) : 0 );
		raw[0] = ( vlan >> 8 );
		raw[1] = ( vlan >> 0 );
		if ( len > sizeof ( raw ) )
			len = sizeof ( raw );
		memcpy ( data, raw, len );
		return sizeof ( raw );
	}

	/* Fetch raw parameter */
	raw_len = bmc_lan_read ( setting->tag, data, len );

	/* Set default type */
	if ( ( raw_len >= 0 ) && ( ! setting->type ) )
		setting->type = &setting_type_hex;

	return raw_len;
}

/**
 * Store value of BMC LAN setting
 *
 * @v settings		Settings block
 * @v setting		Setting to store
 * @v data		Setting data, or NULL to clear setting
 * @v len		Length of setting data
 * @ret rc		Return status code
 */
static int bmc_store ( struct settings *settings __unused,
		       const struct setting *setting, const void *data,
		       size_t len ) {
	const uint8_t *bytes = data;
	uint8_t raw[2];
	unsigned int vlan;
	unsigned int i;

	/* Refuse to store settings with no parameter number */
	if ( ! setting->tag )
		return -ENOTSUP;

	/* Refuse to clear settings, except for the VLAN ID (for
	 * which clearing meaningfully disables the VLAN)
	 */
	if ( ! data ) {
		data = bytes = raw;
		len = 0;
	}

	/* Translate the VLAN ID parameter (as for fetching) */
	if ( setting_cmp ( setting, &bmc_vlanid_setting ) == 0 ) {
		vlan = 0;
		for ( i = 0 ; i < len ; i++ )
			vlan = ( ( vlan << 8 ) | bytes[i] );
		if ( vlan > 0xfff )
			return -ERANGE;
		raw[0] = ( vlan & 0xff );
		raw[1] = ( vlan ? ( IPMI_LAN_VLAN_ENABLED | ( vlan >> 8 ) )
			   : 0 );
		return bmc_lan_write ( setting->tag, raw, sizeof ( raw ) );
	}

	/* Refuse zero-length writes of other parameters */
	if ( ! len )
		return -ENOTSUP;

	/* Store raw parameter */
	return bmc_lan_write ( setting->tag, data, len );
}

/** BMC LAN settings operations */
static struct settings_operations bmc_settings_operations = {
	.applies = bmc_applies,
	.store = bmc_store,
	.fetch = bmc_fetch,
};

/** BMC LAN settings */
static struct settings bmc_settings = {
	.refcnt = NULL,
	.siblings = LIST_HEAD_INIT ( bmc_settings.siblings ),
	.children = LIST_HEAD_INIT ( bmc_settings.children ),
	.op = &bmc_settings_operations,
	.default_scope = &bmc_settings_scope,
};

/** Initialise BMC LAN settings */
static void bmc_init ( void ) {
	struct settings *settings = &bmc_settings;
	int rc;

	/* Do nothing unless an IPMI system interface exists */
	if ( ( rc = ipmi_open() ) != 0 ) {
		DBGC ( settings, "BMC LAN has no IPMI interface: %s\n",
		       strerror ( rc ) );
		return;
	}

	/* Register settings */
	if ( ( rc = register_settings ( settings, NULL, "bmc" ) ) != 0 ) {
		DBGC ( settings, "BMC LAN could not register settings: %s\n",
		       strerror ( rc ) );
		return;
	}
}

/** BMC LAN settings initialiser */
struct init_fn bmc_init_fn __init_fn ( INIT_NORMAL ) = {
	.name = "bmc",
	.initialise = bmc_init,
};
