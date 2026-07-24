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
- **Prerequisite mux fix**: the resolver mux used to abort the whole
  resolution when a resolver's `resolv()` method failed
  *synchronously* (only async failures advanced to the next entry;
  NUMERIC dodges this by reporting failure via a one-shot process).
  Fixed in `resolv.c` to advance to the next resolver instead, so
  `mdns_resolv()` can decline synchronously without the
  process-deferral boilerplate.

### New module: `src/net/udp/mdns.c`

Modelled directly on `src/net/udp/dns.c`, but much simpler (no
nameserver list, no search list, no recursion-desired):

- **Socket**: `xfer_open_socket()` / UDP with NULL peer (like
  dns.c), ephemeral local port.  Each transmission sends the query
  via `meta.dest` to 224.0.0.251:5353 (AF_INET) and ff02::fb:5353
  (AF_INET6), on *every open netdev*.  **Important discovery**:
  multicast routing (`ipv4_route()`) matches the destination's scope
  ID against `netdev->scope_id`, and scope 0 matches nothing — the
  scope ID (`st_scope_id`) MUST be set explicitly per netdev.  Since
  a netdev walk is thus mandatory anyway, querying all open netdevs
  costs nothing extra.  Transmission via an unconfigured address
  family fails harmlessly (send succeeds if any tx worked).
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
- End-to-end (no qemu needed): build the userspace binary
  `bin-x86_64-linux/tap.linux` with `EMBED=<script>` (script does
  `ifopen net0` / static `set net0/ip` / `nslookup addr
  testhost.local`) and `#define NSLOOKUP_CMD` in
  `config/local/general.h`; run it against a Python AF_PACKET
  responder on a persistent tap device which answers the one-shot
  query with a legacy unicast response.  `DEBUG=mdns,resolv` shows
  the full flow.  Verified 2026-07-24: A record, AAAA record (via
  qtype alternation), cache-flush bit masking, timeout fall-through
  to DNS, and instant decline of non-.local names.
- Against real avahi (also no qemu): `apt-get install avahi-daemon`,
  `ip tuntap add avahitap0 mode tap` + `ip addr add 192.168.77.1/24`
  + `ip link set up`, configure `/etc/avahi/avahi-daemon.conf` with
  `host-name=testhost`, `allow-interfaces=avahitap0`,
  `enable-dbus=no`, run `avahi-daemon --no-drop-root --daemonize`,
  then run the embedded-script `tap.linux` with
  `--net tap,if=avahitap0`.  avahi answers the one-shot query ~120µs
  after it is sent (unicast from :5353, ID and question echoed,
  1 answer).  Container caveat: this sandbox's kernel has IPv6
  disabled (avahi logs "Failed to create IPv6 socket"), so IPv6
  transport was verified with the Python responder instead
  (hand-built IPv6 legacy unicast response incl. mandatory UDPv6
  checksum), with iPXE running IPv6-only (no IPv4 configured).
- Still outstanding on real hardware/qemu: `dhcp`-driven config and
  a host-kernel-IPv6 network with dual-stack avahi.

### Future work (explicit non-goals for phase 1)

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

- **2026-07-24 (e)**: Commit 3 (avahi interop testing) done — **no
  code changes needed**; everything passed against real avahi 0.8:
  basic A resolution (answered on the *first* one-shot query),
  full `chain http://testhost.local:8080/...` HTTP fetch (CHAINOK,
  port preserved through resolv_done), uppercase `TESTHOST.LOCAL`,
  trailing-dot `testhost.local.`, nonexistent-name clean failure
  (3 sends, ~7s, falls through to DNS).  Wire capture confirms
  textbook RFC 6762 §6.7 behaviour: query QM from ephemeral port on
  both transports; avahi replies unicast from :5353 with ID+question
  echoed.  IPv6: container kernel has IPv6 *disabled* (avahi can't
  even open an IPv6 socket) — iPXE's ff02::fb queries were verified
  well-formed on the wire (tcpdump parses them, udp sum ok), and the
  IPv6 RX path was verified with the layer-2 Python responder:
  A-over-IPv6 and AAAA-over-IPv6 (alternation) both resolve with
  iPXE running IPv6-only.  IPv4-unconfigured runtime degrades
  gracefully ("Network unreachable" per family, retries continue).
  Remaining for real hardware: dhcp-driven flow, kernel-IPv6
  dual-stack avahi.  Test artefacts (responder.py harness) live in
  the session scratchpad; the method is documented under Testing.
- **2026-07-24 (d)**: Commit 2 done (plus a prep commit).  Two
  design corrections discovered en route, both folded back into the
  design notes above: (1) the resolver mux aborted the whole
  resolution on a *synchronous* resolver failure — fixed in
  `core/resolv.c` (own commit) so mDNS can decline non-.local names
  cheaply; (2) multicast tx requires an explicit scope ID
  (`ipv4_route()` matches `netdev->scope_id`, scope 0 matches
  nothing), so mdns.c sends on every open netdev, which also
  removed the single-NIC limitation from the future-work list.
  Implementation details: qtype alternates A/AAAA across
  retransmissions (accepts either answer type) to cover IPv4-only
  and IPv6-only responders without probing the local stack; timer
  min 1s / max 4s gives 3 sends (t=0,1s,3s), gives up ~7s.  Verified
  end-to-end with a tap + Python-responder harness (see Testing):
  A, AAAA, cache-flush masking, timeout fall-through, non-.local
  instant decline all pass; EFI build OK; `tests.linux` all pass.
  Next: commit 3 follow-ups — real-network avahi testing (qemu),
  IPv6-only/dual-stack builds, and consider whether `.local` should
  also skip the DNS fallback.
- **2026-07-24 (c)**: Commit 1 done: name codec moved verbatim from
  dns.c to new `src/net/dnsname.c` (plus `ERRFILE_dnsname` in
  `errfile.h` — iPXE requires a per-file error identifier; forgetting
  it fails the build with `missing_errfile_declaration`).  Verified:
  EFI build OK, `tests.linux` passes (`dns` 86/86).  Next: commit 2
  (mdns.c resolver itself).
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
