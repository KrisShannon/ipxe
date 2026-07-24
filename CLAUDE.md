# iPXE mDNS enhancement

**Living charter/lab notebook for branch `claude/mdns`.  Keep
the dated status section updated every session so any new session can
resume from this document alone.**

## Goal

Design and implement mDNS client and configuration for iPXE.

## Design notes

### Approach: one-shot ("legacy") mDNS querier

iPXE needs one-shot boot-time name resolution, not a continuous
responder/cache.  RFC 6762 explicitly supports this mode:

- **§5.1 one-shot queries**: a resolver may send a query to
  224.0.0.251:5353 (IPv4) / [ff02::fb]:5353 (IPv6) from an
  *ephemeral* source port.
- **§6.7 legacy unicast responses**: because the source port is not
  5353, responders MUST reply via conventional *unicast* back to the
  querier's source address and port.

This is the key simplification.  iPXE never needs to receive
multicast for mDNS: replies come back as ordinary unicast UDP and are
demuxed by the existing `udp_demux()` (match on local port) exactly
like unicast DNS replies.  We therefore avoid any dependency on NIC
multicast RX filters and need no IGMP/MLD (iPXE has neither).

The transmit path already works: `ipv4_route()` handles multicast
destinations via scope ID (`src/net/ipv4.c:321`), and `ipv6_route()`
routes link-local multicast such as ff02::fb (`src/net/ipv6.c:351`).
The link layer maps multicast IP to multicast MAC on tx.

### Integration point: the resolver table

`src/core/resolv.c` multiplexes name resolution over a linker table
of `struct resolver` entries (`RESOLVERS`), tried in order until one
succeeds.  Current entries: `NUMERIC` at order `01`, `DNS` at
`RESOLV_NORMAL` = `02` (`src/net/udp/dns.c:1057`).

- Add `#define RESOLV_MDNS 015` to `include/ipxe/resolv.h`.  Table
  order tokens sort lexically, so `"015"` lands between `"01"` and
  `"02"`: mDNS runs before DNS.
- `mdns_resolv()` immediately returns an error unless the name ends
  in `.local` (case-insensitive, per RFC 6762 §3), so non-mDNS names
  fall through to DNS at zero cost.  Conversely, if mDNS fails on a
  `.local` name, the mux still falls through to unicast DNS as a
  pragmatic fallback.

### New module: `src/net/udp/mdns.c`

Modelled directly on `src/net/udp/dns.c`, but much simpler (no
nameserver list, no search list, no recursion-desired):

- **Socket**: `xfer_open_socket()` / UDP to peer 224.0.0.251:5353
  (AF_INET) or ff02::fb:5353 (AF_INET6, when IPv6 is compiled in),
  ephemeral local port.  `sin_scope_id`/`sin6_scope_id` left at 0,
  which routes via the first open netdev — fine for the typical
  single-NIC boot case (see Future work).
- **Query**: standard DNS wire format; reuse the RFC 1035 name codec
  from dns.c (`dns_encode()` etc, prototyped in `include/ipxe/dns.h`).
  Random non-zero ID (legacy queries use and check real IDs, unlike
  fully-compliant mDNS which uses 0).  QCLASS = IN without the QU
  bit: §6.7 already guarantees unicast responses to legacy queries.
  QTYPE A and/or AAAA following the same address-family preference
  logic as dns.c.
- **Retransmission**: `retry` timer as in dns.c; intervals of at
  least one second (RFC 6762 §5.1), exponential backoff, give up
  after ~3 attempts with `-ENXIO` so the mux can try the next
  resolver.
- **Response validation**: QR bit set, matching ID, matching
  question, answer RR name/type matches.  Mask the top bit of the RR
  class (mDNS cache-flush bit, RFC 6762 §10.2) before comparing
  against `DNS_CLASS_IN`.  Take the first matching A/AAAA record and
  call `resolv_done()`.  CNAME chasing is not supported (essentially
  unused in mDNS); ignore non-matching RRs.

### Refactoring prerequisite

`dns_encode()`, `dns_decode()`, `dns_compare()`, `dns_copy()` and
`dns_skip()` are declared in `include/ipxe/dns.h` but *defined* in
`dns.c`, which also registers the DNS resolver.  Linking mdns.c
against them would drag in the whole unicast DNS resolver even when
`DNS_RESOLVER` is disabled.  First commit: factor the name codec out
into a new `src/net/dnsname.c` (no functional change; existing
`tests/dns_test.c` covers the codec and keeps us honest).

### Configuration

- **Build option**: `#define MDNS_RESOLVER` in `config/general.h`
  next to `DNS_RESOLVER` (line 254), enabled by default, with the
  matching `REQUIRE_OBJECT ( mdns )` hook in `config/config.c`
  (pattern at line 152).  Independent of `DNS_RESOLVER` thanks to the
  dnsname.c split.
- **Runtime**: no new settings needed for phase 1.  mDNS is scoped to
  `.local` names only, so it cannot perturb existing configurations.

### Security notes

mDNS is inherently unauthenticated and link-local; any host on the
segment can answer.  This is no worse than iPXE's existing trust in
DHCP-supplied DNS servers on the same segment.  Responses are bounds-
checked with the same parsing discipline as dns.c; `FILE_SECBOOT (
PERMITTED )` as for dns.c.

### Testing

- Unit: existing `tests/dns_test.c` exercises the (relocated) name
  codec; add an `mdns_test.c` only if response-parsing helpers grow
  beyond what dns.c already covers.
- Manual: qemu guest on a bridged/tap network with `avahi-daemon` on
  the host; in iPXE: `dhcp`, then `nslookup addr somehost.local` and
  `show addr`, plus a full `chain http://somehost.local/...` fetch.
  Test with IPv4-only, IPv6-only, and dual-stack builds.

### Future work (explicit non-goals for phase 1)

- Querying on all open netdevs (currently first-netdev via scope 0).
- Continuous-mode mDNS (bind :5353, join group, cache, cache-flush
  handling) — would need netdev multicast join support.
- DNS-SD service discovery (PTR/SRV/TXT, e.g. `_https._tcp.local`)
  for boot-server discovery; would build naturally on this module.

### Commit plan

1. `[dns] Split RFC1035 name codec out into dnsname.c` (pure move).
2. `[mdns] Add one-shot multicast DNS resolver` (mdns.c, resolv.h
   priority, config plumbing).
3. Follow-ups as needed from testing.

## Status

- **2026-07-24 (b)**: Design written (one-shot legacy mDNS querier per
  RFC 6762 §5.1/§6.7; new resolver at order 015; dnsname.c codec
  split; `MDNS_RESOLVER` build option).  No code yet — next step is
  commit 1 of the commit plan (dnsname.c split).
- **2026-07-24 (a)**: Initial commit with just this CLAUDE.md file

## Build & test

```sh
cd src
make -j$(nproc) bin-x86_64-efi/ipxe.efi
util/genfsimg -o ipxe.iso bin-x86_64-efi/ipxe.efi   # for BMC virtual media
```

## Workflow

- Branch: `claude/mdns` (based on `origin/master`; do not push
  elsewhere).  Keep commits small and buildable.
