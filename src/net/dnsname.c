/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * Portions copyright (C) 2004 Anselm M. Hoffmeister
 * <stockholm@users.sourceforge.net>.
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

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/dns.h>

/** @file
 *
 * DNS name encoding and decoding (RFC1035)
 *
 */

/**
 * Encode a DNS name using RFC1035 encoding
 *
 * @v string		DNS name as a string
 * @v name		DNS name to fill in
 * @ret len		Length of DNS name, or negative error
 */
int dns_encode ( const char *string, struct dns_name *name ) {
	uint8_t *start = ( name->data + name->offset );
	uint8_t *end = ( name->data + name->len );
	uint8_t *dst = start;
	size_t len = 0;
	char c;

	/* Encode name */
	while ( ( c = *(string++) ) ) {

		/* Handle '.' separators */
		if ( c == '.' ) {

			/* Reject consecutive '.' */
			if ( ( len == 0 ) && ( dst != start ) )
				return -EINVAL;

			/* Terminate if this is the trailing '.' */
			if ( *string == '\0' )
				break;

			/* Reject initial non-terminating '.' */
			if ( len == 0 )
				return -EINVAL;

			/* Reset length */
			len = 0;

		} else {

			/* Increment length */
			len++;

			/* Check for overflow */
			if ( len > DNS_MAX_LABEL_LEN )
				return -EINVAL;
		}

		/* Copy byte, update length */
		if ( ++dst < end ) {
			*dst = c;
			dst[-len] = len;
		}
	}

	/* Add terminating root marker */
	if ( len )
		dst++;
	if ( dst < end )
		*dst = '\0';
	dst++;

	return ( dst - start );
}

/**
 * Find start of valid label within an RFC1035-encoded DNS name
 *
 * @v name		DNS name
 * @v offset		Current offset
 * @ret offset		Offset of label, or negative error
 */
static int dns_label ( struct dns_name *name, size_t offset ) {
	const uint8_t *byte;
	const uint16_t *word;
	size_t len;
	size_t ptr;

	while ( 1 ) {

		/* Fail if we have overrun the DNS name */
		if ( ( offset + sizeof ( *byte) ) > name->len )
			return -EINVAL;
		byte = ( name->data + offset );

		/* Follow compression pointer, if applicable */
		if ( DNS_IS_COMPRESSED ( *byte ) ) {

			/* Fail if we have overrun the DNS name */
			if ( ( offset + sizeof ( *word ) ) > name->len )
				return -EINVAL;
			word = ( name->data + offset );

			/* Extract pointer to new offset */
			ptr = DNS_COMPRESSED_OFFSET ( ntohs ( *word ) );

			/* Fail if pointer does not point backwards.
			 * (This guarantees termination of the
			 * function.)
			 */
			if ( ptr >= offset )
				return -EINVAL;

			/* Continue from new offset */
			offset = ptr;
			continue;
		}

		/* Fail if we have overrun the DNS name */
		len = *byte;
		if ( ( offset + sizeof ( *byte ) + len ) > name->len )
			return -EINVAL;

		/* We have a valid label */
		return offset;
	}
}

/**
 * Decode RFC1035-encoded DNS name
 *
 * @v name		DNS name
 * @v data		Output buffer
 * @v len		Length of output buffer
 * @ret len		Length of decoded DNS name, or negative error
 */
int dns_decode ( struct dns_name *name, char *data, size_t len ) {
	unsigned int recursion_limit = name->len; /* Generous upper bound */
	int offset = name->offset;
	const uint8_t *label;
	size_t decoded_len = 0;
	size_t label_len;
	size_t copy_len;

	while ( recursion_limit-- ) {

		/* Find valid DNS label */
		offset = dns_label ( name, offset );
		if ( offset < 0 )
			return offset;

		/* Terminate if we have reached the root */
		label = ( name->data + offset );
		label_len = *(label++);
		if ( label_len == 0 ) {
			if ( decoded_len < len )
				*data = '\0';
			return decoded_len;
		}

		/* Prepend '.' if applicable */
		if ( decoded_len && ( decoded_len++ < len ) )
			*(data++) = '.';

		/* Copy label to output buffer */
		copy_len = ( ( decoded_len < len ) ? ( len - decoded_len ) : 0);
		if ( copy_len > label_len )
			copy_len = label_len;
		memcpy ( data, label, copy_len );
		data += copy_len;
		decoded_len += label_len;

		/* Move to next label */
		offset += ( sizeof ( *label ) + label_len );
	}

	/* Recursion limit exceeded */
	return -EINVAL;
}

/**
 * Compare DNS names for equality
 *
 * @v first		First DNS name
 * @v second		Second DNS name
 * @ret rc		Return status code
 */
int dns_compare ( struct dns_name *first, struct dns_name *second ) {
	unsigned int recursion_limit = first->len; /* Generous upper bound */
	int first_offset = first->offset;
	int second_offset = second->offset;
	const uint8_t *first_label;
	const uint8_t *second_label;
	size_t label_len;
	size_t len;

	while ( recursion_limit-- ) {

		/* Find valid DNS labels */
		first_offset = dns_label ( first, first_offset );
		if ( first_offset < 0 )
			return first_offset;
		second_offset = dns_label ( second, second_offset );
		if ( second_offset < 0 )
			return second_offset;

		/* Compare label lengths */
		first_label = ( first->data + first_offset );
		second_label = ( second->data + second_offset );
		label_len = *(first_label++);
		if ( label_len != *(second_label++) )
			return -ENOENT;
		len = ( sizeof ( *first_label ) + label_len );

		/* Terminate if we have reached the root */
		if ( label_len == 0 )
			return 0;

		/* Compare label contents (case-insensitively) */
		while ( label_len-- ) {
			if ( tolower ( *(first_label++) ) !=
			     tolower ( *(second_label++) ) )
				return -ENOENT;
		}

		/* Move to next labels */
		first_offset += len;
		second_offset += len;
	}

	/* Recursion limit exceeded */
	return -EINVAL;
}

/**
 * Copy a DNS name
 *
 * @v src		Source DNS name
 * @v dst		Destination DNS name
 * @ret len		Length of copied DNS name, or negative error
 */
int dns_copy ( struct dns_name *src, struct dns_name *dst ) {
	unsigned int recursion_limit = src->len; /* Generous upper bound */
	int src_offset = src->offset;
	size_t dst_offset = dst->offset;
	const uint8_t *label;
	size_t label_len;
	size_t copy_len;
	size_t len;

	while ( recursion_limit-- ) {

		/* Find valid DNS label */
		src_offset = dns_label ( src, src_offset );
		if ( src_offset < 0 )
			return src_offset;

		/* Copy as an uncompressed label */
		label = ( src->data + src_offset );
		label_len = *label;
		len = ( sizeof ( *label ) + label_len );
		copy_len = ( ( dst_offset < dst->len ) ?
			     ( dst->len - dst_offset ) : 0 );
		if ( copy_len > len )
			copy_len = len;
		memcpy ( ( dst->data + dst_offset ), label, copy_len );
		src_offset += len;
		dst_offset += len;

		/* Terminate if we have reached the root */
		if ( label_len == 0 )
			return ( dst_offset - dst->offset );
	}

	/* Recursion limit exceeded */
	return -EINVAL;
}

/**
 * Skip RFC1035-encoded DNS name
 *
 * @v name		DNS name
 * @ret offset		Offset to next name, or negative error
 */
int dns_skip ( struct dns_name *name ) {
	unsigned int recursion_limit = name->len; /* Generous upper bound */
	int offset = name->offset;
	int prev_offset;
	const uint8_t *label;
	size_t label_len;

	while ( recursion_limit-- ) {

		/* Find valid DNS label */
		prev_offset = offset;
		offset = dns_label ( name, prev_offset );
		if ( offset < 0 )
			return offset;

		/* Terminate if we have reached a compression pointer */
		if ( offset != prev_offset )
			return ( prev_offset + sizeof ( uint16_t ) );

		/* Skip this label */
		label = ( name->data + offset );
		label_len = *label;
		offset += ( sizeof ( *label ) + label_len );

		/* Terminate if we have reached the root */
		if ( label_len == 0 )
			return offset;
	}

	/* Recursion limit exceeded */
	return -EINVAL;
}
