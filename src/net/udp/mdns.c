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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/refcnt.h>
#include <ipxe/iobuf.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/resolv.h>
#include <ipxe/retry.h>
#include <ipxe/timer.h>
#include <ipxe/netdevice.h>
#include <ipxe/tcpip.h>
#include <ipxe/socket.h>
#include <ipxe/in.h>
#include <ipxe/job.h>
#include <ipxe/dns.h>

/** @file
 *
 * Multicast DNS protocol
 *
 * iPXE operates as a one-shot multicast DNS resolver as described in
 * RFC 6762 section 5.1.  Queries are sent to the mDNS multicast
 * address from an ephemeral source port, and responders will
 * therefore reply via conventional unicast directly to that port
 * (RFC 6762 section 6.7).  This allows iPXE to avoid ever needing to
 * receive multicast packets.
 */

/* Disambiguate the various error causes */
#define ENXIO_NO_RESPONSE __einfo_error ( EINFO_ENXIO_NO_RESPONSE )
#define EINFO_ENXIO_NO_RESPONSE \
	__einfo_uniqify ( EINFO_ENXIO, 0x01, "No response from mDNS responders" )

/** Multicast DNS port */
#define MDNS_PORT 5353

/** Multicast DNS cache-flush bit (RFC 6762 section 10.2) */
#define MDNS_CLASS_FLUSH 0x8000

/** Minimum retransmission interval
 *
 * One-shot queries must be spaced at least one second apart (RFC
 * 6762 section 5.1).
 */
#define MDNS_MIN_TIMEOUT ( 1 * TICKS_PER_SEC )

/** Maximum timeout
 *
 * This gives approximately three query attempts before giving up.
 */
#define MDNS_MAX_TIMEOUT ( 4 * TICKS_PER_SEC )

/** Multicast DNS IPv4 socket address (224.0.0.251:5353) */
static struct sockaddr_in mdns_addr_ipv4 = {
	.sin_family = AF_INET,
	.sin_port = htons ( MDNS_PORT ),
	.sin_addr.s_addr = htonl ( 0xe00000fbUL ),
};

/** Multicast DNS IPv6 socket address ([ff02::fb]:5353) */
static struct sockaddr_in6 mdns_addr_ipv6 = {
	.sin6_family = AF_INET6,
	.sin6_port = htons ( MDNS_PORT ),
	.sin6_addr.s6_addr = { 0xff, 0x02, [15] = 0xfb },
};

/** Multicast DNS destination socket addresses */
static struct sockaddr *mdns_addr[] = {
	( struct sockaddr * ) &mdns_addr_ipv4,
	( struct sockaddr * ) &mdns_addr_ipv6,
};

/** A multicast DNS request */
struct mdns_request {
	/** Reference counter */
	struct refcnt refcnt;
	/** Name resolution interface */
	struct interface resolv;
	/** Data transfer interface */
	struct interface socket;
	/** Retry timer */
	struct retry_timer timer;

	/** Socket address to fill in with resolved address */
	union {
		struct sockaddr sa;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} address;
	/** Buffer for query */
	struct {
		/** Query header */
		struct dns_header query;
		/** Name buffer */
		char name[DNS_MAX_NAME_LEN];
		/** Space for question */
		struct dns_question padding;
	} __attribute__ (( packed )) buf;
	/** Query name */
	struct dns_name name;
	/** Question within query */
	struct dns_question *question;
	/** Length of query */
	size_t len;
	/** Transmission counter */
	unsigned int count;
};

/**
 * Transcribe mDNS name (for debugging)
 *
 * @v name		DNS name
 * @ret string		Transcribed DNS name
 */
static const char * mdns_name ( struct dns_name *name ) {
	static char buf[256];
	int len;

	len = dns_decode ( name, buf, ( sizeof ( buf ) - 1 /* NUL */ ) );
	return ( ( len < 0 ) ? "<INVALID>" : buf );
}

/**
 * Mark mDNS request as complete
 *
 * @v mdns		mDNS request
 * @v rc		Return status code
 */
static void mdns_done ( struct mdns_request *mdns, int rc ) {

	/* Stop the retry timer */
	stop_timer ( &mdns->timer );

	/* Shut down interfaces */
	intf_shutdown ( &mdns->socket, rc );
	intf_shutdown ( &mdns->resolv, rc );
}

/**
 * Mark mDNS request as resolved and complete
 *
 * @v mdns		mDNS request
 */
static void mdns_resolved ( struct mdns_request *mdns ) {

	DBGC ( mdns, "MDNS %p found address %s\n",
	       mdns, sock_ntoa ( &mdns->address.sa ) );

	/* Return resolved address */
	resolv_done ( &mdns->resolv, &mdns->address.sa );

	/* Mark operation as complete */
	mdns_done ( mdns, 0 );
}

/**
 * Send mDNS query
 *
 * @v mdns		mDNS request
 * @ret rc		Return status code
 */
static int mdns_send_packet ( struct mdns_request *mdns ) {
	struct dns_header *query = &mdns->buf.query;
	struct net_device *netdev;
	struct xfer_metadata meta;
	union {
		struct sockaddr sa;
		struct sockaddr_tcpip st;
	} dest;
	unsigned int sent = 0;
	unsigned int i;
	int rc = -ENETUNREACH;

	/* Start retransmission timer */
	start_timer ( &mdns->timer );

	/* Alternate query type between A and AAAA, so that both
	 * IPv4-only and IPv6-only responders will eventually be
	 * found.  (Responses of either type are accepted regardless
	 * of the current query type.)
	 */
	mdns->question->qtype = ( ( mdns->count++ & 1 ) ?
				  htons ( DNS_TYPE_AAAA ) :
				  htons ( DNS_TYPE_A ) );

	/* Construct metadata */
	memset ( &meta, 0, sizeof ( meta ) );
	meta.dest = &dest.sa;

	/* Send query on each open network device, to each mDNS
	 * multicast address.  Multicast routing requires an explicit
	 * scope ID.  Transmission via an unconfigured address family
	 * will fail harmlessly.
	 */
	DBGC ( mdns, "MDNS %p sending query ID %#04x for %s\n", mdns,
	       ntohs ( query->id ), mdns_name ( &mdns->name ) );
	for_each_netdev ( netdev ) {
		if ( ! netdev_is_open ( netdev ) )
			continue;
		for ( i = 0 ; i < ( sizeof ( mdns_addr ) /
				    sizeof ( mdns_addr[0] ) ) ; i++ ) {
			memcpy ( &dest.sa, mdns_addr[i], sizeof ( dest.sa ) );
			dest.st.st_scope_id = netdev->scope_id;
			if ( ( rc = xfer_deliver_raw_meta ( &mdns->socket,
							    query, mdns->len,
							    &meta ) ) == 0 ) {
				sent++;
			} else {
				DBGC2 ( mdns, "MDNS %p could not send via %s "
					"to %s: %s\n", mdns, netdev->name,
					sock_ntoa ( mdns_addr[i] ),
					strerror ( rc ) );
			}
		}
	}

	return ( sent ? 0 : rc );
}

/**
 * Handle mDNS (re)transmission timer expiry
 *
 * @v timer		Retry timer
 * @v fail		Failure indicator
 */
static void mdns_timer_expired ( struct retry_timer *timer, int fail ) {
	struct mdns_request *mdns =
		container_of ( timer, struct mdns_request, timer );

	/* Give up if no responder has answered */
	if ( fail ) {
		mdns_done ( mdns, -ENXIO_NO_RESPONSE );
		return;
	}

	/* Send mDNS query */
	mdns_send_packet ( mdns );
}

/**
 * Receive new data
 *
 * @v mdns		mDNS request
 * @v iobuf		I/O buffer
 * @v meta		Data transfer metadata
 * @ret rc		Return status code
 */
static int mdns_xfer_deliver ( struct mdns_request *mdns,
			       struct io_buffer *iobuf,
			       struct xfer_metadata *meta __unused ) {
	struct dns_header *response = iobuf->data;
	struct dns_name buf;
	union dns_rr *rr;
	unsigned int qdcount;
	unsigned int class;
	int offset;
	size_t next_offset;
	size_t rdlength;
	int rc;

	/* Sanity check */
	if ( iob_len ( iobuf ) < sizeof ( *response ) ) {
		DBGC ( mdns, "MDNS %p received underlength packet length "
		       "%zd\n", mdns, iob_len ( iobuf ) );
		rc = -EINVAL;
		goto done;
	}

	/* Check response ID matches query ID */
	if ( response->id != mdns->buf.query.id ) {
		DBGC ( mdns, "MDNS %p received unexpected response ID %#04x "
		       "(wanted %#04x)\n", mdns, ntohs ( response->id ),
		       ntohs ( mdns->buf.query.id ) );
		rc = -EINVAL;
		goto done;
	}

	/* Check that this is a response */
	if ( ! ( response->flags & htons ( DNS_FLAG_QR ) ) ) {
		DBGC ( mdns, "MDNS %p received non-response\n", mdns );
		rc = -EINVAL;
		goto done;
	}
	DBGC ( mdns, "MDNS %p received response ID %#04x\n",
	       mdns, ntohs ( response->id ) );

	/* Responders SHOULD echo the question in a legacy unicast
	 * response (RFC 6762 section 6.7): allow it to be present or
	 * absent.
	 */
	qdcount = ntohs ( response->qdcount );
	if ( qdcount > 1 ) {
		DBGC ( mdns, "MDNS %p received response with %d questions\n",
		       mdns, qdcount );
		rc = -EINVAL;
		goto done;
	}

	/* Verify and skip question section, if present */
	buf.data = iobuf->data;
	buf.len = iob_len ( iobuf );
	buf.offset = sizeof ( *response );
	if ( qdcount ) {
		if ( dns_compare ( &buf, &mdns->name ) != 0 ) {
			DBGC ( mdns, "MDNS %p received response for %s\n",
			       mdns, mdns_name ( &buf ) );
			rc = -EINVAL;
			goto done;
		}
		offset = dns_skip ( &buf );
		if ( offset < 0 ) {
			rc = offset;
			DBGC ( mdns, "MDNS %p received response with "
			       "malformed question: %s\n",
			       mdns, strerror ( rc ) );
			goto done;
		}
		buf.offset = ( offset + sizeof ( struct dns_question ) );
	}

	/* Search through response for a usable answer */
	for ( ; buf.offset < buf.len ; buf.offset = next_offset ) {

		/* Check for valid name */
		offset = dns_skip ( &buf );
		if ( offset < 0 ) {
			rc = offset;
			DBGC ( mdns, "MDNS %p received response with "
			       "malformed answer: %s\n",
			       mdns, strerror ( rc ) );
			goto done;
		}

		/* Check for sufficient space for resource record */
		rr = ( buf.data + offset );
		if ( ( offset + sizeof ( rr->common ) ) > buf.len ) {
			DBGC ( mdns, "MDNS %p received response with "
			       "underlength RR\n", mdns );
			rc = -EINVAL;
			goto done;
		}
		rdlength = ntohs ( rr->common.rdlength );
		next_offset = ( offset + sizeof ( rr->common ) + rdlength );
		if ( next_offset > buf.len ) {
			DBGC ( mdns, "MDNS %p received response with "
			       "underlength RR\n", mdns );
			rc = -EINVAL;
			goto done;
		}

		/* Skip non-matching names */
		if ( dns_compare ( &buf, &mdns->name ) != 0 ) {
			DBGC2 ( mdns, "MDNS %p ignoring response for %s\n",
				mdns, mdns_name ( &buf ) );
			continue;
		}

		/* Skip non-Internet classes, masking out the
		 * cache-flush bit (RFC 6762 section 10.2)
		 */
		class = ( ntohs ( rr->common.class ) & ~MDNS_CLASS_FLUSH );
		if ( class != DNS_CLASS_IN ) {
			DBGC2 ( mdns, "MDNS %p ignoring response class %d\n",
				mdns, class );
			continue;
		}

		/* Handle answer */
		switch ( rr->common.type ) {

		case htons ( DNS_TYPE_AAAA ):

			/* Found the target AAAA record */
			if ( rdlength <
			     sizeof ( mdns->address.sin6.sin6_addr ) ) {
				DBGC ( mdns, "MDNS %p received response with "
				       "underlength AAAA\n", mdns );
				rc = -EINVAL;
				goto done;
			}
			mdns->address.sin6.sin6_family = AF_INET6;
			memcpy ( &mdns->address.sin6.sin6_addr,
				 &rr->aaaa.in6_addr,
				 sizeof ( mdns->address.sin6.sin6_addr ) );
			mdns_resolved ( mdns );
			rc = 0;
			goto done;

		case htons ( DNS_TYPE_A ):

			/* Found the target A record */
			if ( rdlength <
			     sizeof ( mdns->address.sin.sin_addr ) ) {
				DBGC ( mdns, "MDNS %p received response with "
				       "underlength A\n", mdns );
				rc = -EINVAL;
				goto done;
			}
			mdns->address.sin.sin_family = AF_INET;
			mdns->address.sin.sin_addr = rr->a.in_addr;
			mdns_resolved ( mdns );
			rc = 0;
			goto done;

		default:
			DBGC2 ( mdns, "MDNS %p ignoring record type %d\n",
				mdns, ntohs ( rr->common.type ) );
			break;
		}
	}

	/* Ignore responses containing no usable answer.  The
	 * retransmission timer is still running, and a usable
	 * response may yet arrive.
	 */
	rc = 0;

 done:
	/* Free I/O buffer */
	free_iob ( iobuf );
	return rc;
}

/**
 * Handle socket closure
 *
 * @v mdns		mDNS request
 * @v rc		Reason for close
 */
static void mdns_xfer_close ( struct mdns_request *mdns, int rc ) {

	if ( ! rc )
		rc = -ECONNABORTED;

	mdns_done ( mdns, rc );
}

/**
 * Report job progress
 *
 * @v mdns		mDNS request
 * @v progress		Progress report to fill in
 * @ret ongoing_rc	Ongoing job status code (if known)
 */
static int mdns_progress ( struct mdns_request *mdns,
			   struct job_progress *progress ) {
	int len;

	/* Show current question as progress message */
	len = dns_decode ( &mdns->name, progress->message,
			   ( sizeof ( progress->message ) - 1 /* NUL */ ) );
	if ( len < 0 ) {
		/* Ignore undecodable names */
		progress->message[0] = '\0';
	}

	return 0;
}

/** mDNS socket interface operations */
static struct interface_operation mdns_socket_operations[] = {
	INTF_OP ( xfer_deliver, struct mdns_request *, mdns_xfer_deliver ),
	INTF_OP ( intf_close, struct mdns_request *, mdns_xfer_close ),
};

/** mDNS socket interface descriptor */
static struct interface_descriptor mdns_socket_desc =
	INTF_DESC ( struct mdns_request, socket, mdns_socket_operations );

/** mDNS resolver interface operations */
static struct interface_operation mdns_resolv_op[] = {
	INTF_OP ( job_progress, struct mdns_request *, mdns_progress ),
	INTF_OP ( intf_close, struct mdns_request *, mdns_done ),
};

/** mDNS resolver interface descriptor */
static struct interface_descriptor mdns_resolv_desc =
	INTF_DESC ( struct mdns_request, resolv, mdns_resolv_op );

/**
 * Check for a name within the mDNS .local domain
 *
 * @v name		Name to check
 * @ret is_local	Name is within the .local domain
 */
static int mdns_is_local ( const char *name ) {
	static const char local[] = ".local";
	size_t len = strlen ( name );

	/* Ignore a single trailing dot, if present */
	if ( len && ( name[ len - 1 ] == '.' ) )
		len--;

	/* Check for a ".local" suffix (case-insensitively, as per
	 * RFC 6762 section 3)
	 */
	if ( len < ( sizeof ( local ) - 1 /* NUL */ ) )
		return 0;
	return ( strncasecmp ( ( name + len - ( sizeof ( local ) - 1 ) ),
			       local, ( sizeof ( local ) - 1 ) ) == 0 );
}

/**
 * Resolve name using mDNS
 *
 * @v resolv		Name resolution interface
 * @v name		Name to resolve
 * @v sa		Socket address to fill in
 * @ret rc		Return status code
 */
static int mdns_resolv ( struct interface *resolv, const char *name,
			 struct sockaddr *sa ) {
	struct mdns_request *mdns;
	struct dns_header *query;
	int name_len;
	int rc;

	/* Decline names outside of the .local domain */
	if ( ! mdns_is_local ( name ) ) {
		DBG ( "MDNS not attempting to resolve \"%s\": "
		      "not a .local name\n", name );
		return -ENOTSUP;
	}

	/* Allocate mDNS structure */
	mdns = zalloc ( sizeof ( *mdns ) );
	if ( ! mdns ) {
		rc = -ENOMEM;
		goto err_alloc_mdns;
	}
	ref_init ( &mdns->refcnt, NULL );
	intf_init ( &mdns->resolv, &mdns_resolv_desc, &mdns->refcnt );
	intf_init ( &mdns->socket, &mdns_socket_desc, &mdns->refcnt );
	timer_init ( &mdns->timer, mdns_timer_expired, &mdns->refcnt );
	set_timer_limits ( &mdns->timer, MDNS_MIN_TIMEOUT, MDNS_MAX_TIMEOUT );
	memcpy ( &mdns->address.sa, sa, sizeof ( mdns->address.sa ) );

	/* Construct query.  A one-shot query uses a random non-zero
	 * ID (RFC 6762 section 8.3 requires an mDNS responder to echo
	 * it in the response), and does not set the QU bit since
	 * legacy unicast responses are already guaranteed by our use
	 * of an ephemeral source port.
	 */
	query = &mdns->buf.query;
	do {
		query->id = random();
	} while ( ! query->id );
	query->qdcount = htons ( 1 );
	mdns->name.data = &mdns->buf;
	mdns->name.offset = offsetof ( typeof ( mdns->buf ), name );
	mdns->name.len = offsetof ( typeof ( mdns->buf ), padding );
	name_len = dns_encode ( name, &mdns->name );
	if ( name_len < 0 ) {
		rc = name_len;
		goto err_encode;
	}
	if ( ( mdns->name.offset + name_len ) > mdns->name.len ) {
		DBGC ( mdns, "MDNS %p name is too long\n", mdns );
		rc = -EINVAL;
		goto err_encode;
	}
	mdns->question = ( ( ( void * ) &mdns->buf ) + mdns->name.offset +
			   name_len );
	mdns->question->qclass = htons ( DNS_CLASS_IN );
	mdns->len = ( mdns->name.offset + name_len +
		      sizeof ( *(mdns->question) ) );

	/* Open UDP connection */
	if ( ( rc = xfer_open_socket ( &mdns->socket, SOCK_DGRAM,
				       NULL, NULL ) ) != 0 ) {
		DBGC ( mdns, "MDNS %p could not open socket: %s\n",
		       mdns, strerror ( rc ) );
		goto err_open_socket;
	}

	/* Start timer to trigger first packet */
	start_timer_nodelay ( &mdns->timer );

	/* Attach parent interface, mortalise self, and return */
	intf_plug_plug ( &mdns->resolv, resolv );
	ref_put ( &mdns->refcnt );
	return 0;

 err_open_socket:
 err_encode:
	ref_put ( &mdns->refcnt );
 err_alloc_mdns:
	return rc;
}

/** Multicast DNS resolver */
struct resolver mdns_resolver __resolver ( RESOLV_MDNS ) = {
	.name = "mDNS",
	.resolv = mdns_resolv,
};
