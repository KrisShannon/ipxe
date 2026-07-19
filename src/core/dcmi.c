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
 * Data Center Manageability Interface
 *
 * The DCMI asset tag is stored by the BMC and is exposed as the
 * (writable) setting "dcmi/assettag".
 *
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ipxe/settings.h>
#include <ipxe/init.h>
#include <ipxe/ipmi.h>
#include <ipxe/dcmi.h>

/** DCMI settings scope */
static const struct settings_scope dcmi_settings_scope;

/** DCMI asset tag setting */
const struct setting dcmi_assettag_setting __setting ( SETTING_HOST_EXTRA,
						       assettag ) = {
	.name = "assettag",
	.description = "DCMI asset tag",
	.type = &setting_type_string,
	.scope = &dcmi_settings_scope,
};

/**
 * Read DCMI asset tag
 *
 * @v data		Buffer to fill with asset tag
 * @v max		Length of buffer
 * @ret len		Length of asset tag, or negative error
 */
static int dcmi_asset_tag_read ( void *data, size_t max ) {
	struct dcmi_get_asset_tag_request request;
	union {
		struct dcmi_get_asset_tag_response response;
		uint8_t bytes[ sizeof ( struct dcmi_get_asset_tag_response ) +
			       DCMI_ASSET_TAG_CHUNK ];
	} rsp;
	size_t offset = 0;
	size_t total = 0;
	size_t frag;
	size_t copy;
	int len;

	do {
		/* Read next chunk of asset tag */
		request.group = DCMI_GROUP_ID;
		request.offset = offset;
		request.count = DCMI_ASSET_TAG_CHUNK;
		if ( total && ( ( total - offset ) < request.count ) )
			request.count = ( total - offset );
		len = ipmi_message ( IPMI_NETFN_GROUP, DCMI_GET_ASSET_TAG,
				     &request, sizeof ( request ),
				     &rsp, sizeof ( rsp ) );
		if ( len < 0 )
			return len;

		/* Validate response */
		if ( ( ( ( size_t ) len ) < sizeof ( rsp.response ) ) ||
		     ( rsp.response.group != DCMI_GROUP_ID ) )
			return -EPROTO;
		total = rsp.response.total;
		if ( total > DCMI_ASSET_TAG_MAX )
			total = DCMI_ASSET_TAG_MAX;

		/* Copy data */
		frag = ( len - sizeof ( rsp.response ) );
		if ( frag > ( total - offset ) )
			frag = ( total - offset );
		copy = frag;
		if ( offset >= max ) {
			copy = 0;
		} else if ( copy > ( max - offset ) ) {
			copy = ( max - offset );
		}
		memcpy ( ( data + offset ), rsp.response.data, copy );
		offset += frag;

		/* Avoid an endless loop if the BMC returns no data */
		if ( ( offset < total ) && ! frag )
			return -EPROTO;

	} while ( offset < total );

	return total;
}

/**
 * Write DCMI asset tag
 *
 * @v data		Asset tag
 * @v len		Length of asset tag
 * @ret rc		Return status code
 */
static int dcmi_asset_tag_write ( const void *data, size_t len ) {
	union {
		struct dcmi_set_asset_tag_request request;
		uint8_t bytes[ sizeof ( struct dcmi_set_asset_tag_request ) +
			       DCMI_ASSET_TAG_CHUNK ];
	} req;
	struct dcmi_set_asset_tag_response response;
	size_t offset = 0;
	size_t frag;
	int rsp_len;

	/* Sanity check */
	if ( len > DCMI_ASSET_TAG_MAX )
		return -ENOSPC;

	/* Write asset tag in chunks
	 *
	 * A zero-length write at offset zero clears the asset tag.
	 */
	do {
		frag = ( len - offset );
		if ( frag > DCMI_ASSET_TAG_CHUNK )
			frag = DCMI_ASSET_TAG_CHUNK;
		req.request.group = DCMI_GROUP_ID;
		req.request.offset = offset;
		req.request.count = frag;
		if ( frag )
			memcpy ( req.request.data, ( data + offset ), frag );
		rsp_len = ipmi_message ( IPMI_NETFN_GROUP, DCMI_SET_ASSET_TAG,
					 &req, ( sizeof ( req.request ) +
						 frag ),
					 &response, sizeof ( response ) );
		if ( rsp_len < 0 )
			return rsp_len;
		if ( ( ( ( size_t ) rsp_len ) < sizeof ( response ) ) ||
		     ( response.group != DCMI_GROUP_ID ) )
			return -EPROTO;
		offset += frag;
	} while ( offset < len );

	return 0;
}

/**
 * Check applicability of DCMI setting
 *
 * @v settings		Settings block
 * @v setting		Setting
 * @ret applies		Setting applies within this settings block
 */
static int dcmi_applies ( struct settings *settings __unused,
			  const struct setting *setting ) {

	return ( setting->scope == &dcmi_settings_scope );
}

/**
 * Fetch value of a key within the asset tag
 *
 * @v key		Key name
 * @v data		Buffer to fill with value
 * @v len		Length of buffer
 * @ret len		Length of value, or negative error
 *
 * Treat the asset tag as a list of space-separated "key=value"
 * entries, and return the value corresponding to the specified key.
 */
static int dcmi_key_read ( const char *key, void *data, size_t len ) {
	char tag[ DCMI_ASSET_TAG_MAX + 1 /* NUL */ ];
	size_t key_len = strlen ( key );
	size_t value_len;
	char *token;
	char *next;
	char *value;
	int tag_len;

	/* Read asset tag */
	tag_len = dcmi_asset_tag_read ( tag, DCMI_ASSET_TAG_MAX );
	if ( tag_len < 0 )
		return tag_len;
	tag[tag_len] = '\0';

	/* Search for "<key>=" at the start of a space-separated token */
	for ( token = tag ; token ; token = next ) {
		next = strchr ( token, ' ' );
		if ( next )
			*(next++) = '\0';
		if ( ( strncmp ( token, key, key_len ) == 0 ) &&
		     ( token[key_len] == '=' ) ) {
			value = &token[ key_len + 1 ];
			value_len = strlen ( value );
			if ( len > value_len )
				len = value_len;
			memcpy ( data, value, len );
			return value_len;
		}
	}

	return -ENOENT;
}

/**
 * Fetch value of DCMI setting
 *
 * @v settings		Settings block
 * @v setting		Setting to fetch
 * @v data		Buffer to fill with setting data
 * @v len		Length of buffer
 * @ret len		Length of setting data, or negative error
 */
static int dcmi_fetch ( struct settings *settings __unused,
			struct setting *setting, void *data, size_t len ) {

	/* Handle known settings */
	if ( setting_cmp ( setting, &dcmi_assettag_setting ) == 0 )
		return dcmi_asset_tag_read ( data, len );

	/* Treat any other named setting as a key within the asset tag */
	if ( setting->name && setting->name[0] && ( ! setting->tag ) )
		return dcmi_key_read ( setting->name, data, len );

	return -ENOENT;
}

/**
 * Store value of DCMI setting
 *
 * @v settings		Settings block
 * @v setting		Setting to store
 * @v data		Setting data, or NULL to clear setting
 * @v len		Length of setting data
 * @ret rc		Return status code
 */
static int dcmi_store ( struct settings *settings __unused,
			const struct setting *setting, const void *data,
			size_t len ) {

	/* Handle known settings */
	if ( setting_cmp ( setting, &dcmi_assettag_setting ) == 0 )
		return dcmi_asset_tag_write ( data, ( data ? len : 0 ) );

	return -ENOTSUP;
}

/** DCMI settings operations */
static struct settings_operations dcmi_settings_operations = {
	.applies = dcmi_applies,
	.store = dcmi_store,
	.fetch = dcmi_fetch,
};

/** DCMI settings */
static struct settings dcmi_settings = {
	.refcnt = NULL,
	.siblings = LIST_HEAD_INIT ( dcmi_settings.siblings ),
	.children = LIST_HEAD_INIT ( dcmi_settings.children ),
	.op = &dcmi_settings_operations,
	.default_scope = &dcmi_settings_scope,
};

/** Initialise DCMI settings */
static void dcmi_init ( void ) {
	struct settings *settings = &dcmi_settings;
	int rc;

	/* Do nothing unless an IPMI system interface exists */
	if ( ( rc = ipmi_open() ) != 0 ) {
		DBGC ( settings, "DCMI has no IPMI interface: %s\n",
		       strerror ( rc ) );
		return;
	}

	/* Register settings */
	if ( ( rc = register_settings ( settings, NULL, "dcmi" ) ) != 0 ) {
		DBGC ( settings, "DCMI could not register settings: %s\n",
		       strerror ( rc ) );
		return;
	}
}

/** DCMI settings initialiser */
struct init_fn dcmi_init_fn __init_fn ( INIT_NORMAL ) = {
	.name = "dcmi",
	.initialise = dcmi_init,
};
