# iPXE LACP link-wait enhancement

**Living charter/lab notebook for branch `claude/lacp-linkwait`.  Keep
the dated status section updated every session so any new session can
resume from this document alone.**

## Goal

Make booting over VLAN-over-LACP ports script-friendly: replace the
blind `sleep 20` between `ifopen` and `ifconf` (needed because the
switch takes time to bundle the port after iPXE's passive LACP
responder starts answering) with a positive wait for LACP
establishment:

- Record the link partner's most recent LACP state (and reception
  time) on the `net_device`, cleared on close and link-down.
- `iflinkwait --lacp [--timeout <ms>] <interface>` waits until the
  partner reports in-sync + collecting + distributing with current
  (non-stale) information.  Works on a VLAN device too (waits on its
  trunk).

Base: branch `bnx2x` of KrisShannon/ipxe (the cleaned native bnx2x
driver branch — the driver this feature is intended to pair with;
the feature itself is driver-agnostic).  Target usage:

```
ifopen net0
iflinkwait --lacp --timeout 30000 net0
ifconf -c dhcp net0-602
```

## Design notes

- `struct net_device` gains `lacp_state` (partner's LACP actor state
  byte ORed with NETDEV_LACP_VALID 0x100; 0 = nothing received) and
  `lacp_time` (currticks at reception).  Updated by the eth_slow LACP
  responder on every received LACPDU (before it rebuilds the buffer
  for the response — the RX actor TLV is overwritten in place!).
- Cleared in `netdev_link_err()` (rc != 0 path, i.e. any link-down)
  and `netdev_close()`.
- Establishment predicate (usr/ifmgmt.c iflacpwait_progress): link up,
  NETDEV_LACP_VALID set, partner state has IN_SYNC|COLLECTING|
  DISTRIBUTING all set, and information no older than 3× the
  requested interval (1s fast/30s slow — 802.1AX receive-machine
  timeout).  Ongoing/timeout error: "No LACP aggregation"
  (ENOTCONN_LACP, uniqify 0x02 under ENOTCONN; 0x01 is taken by
  netdevice.c's "Down").
- eth_slow already blocks the link (`netdev_link_block`) when a
  received LACPDU shows the partner NOT in sync — but nothing arrives
  at all in the window between ifopen and the switch's first LACPDU,
  so the link looks up and plain `iflinkwait` sails through; that gap
  is exactly what --lacp closes.
- New `vlan_trunk()` accessor in net/vlan.c (+ __weak NULL stub in
  netdevice.c, same pattern as vlan_tci) lets --lacp on `net0-602`
  wait on `net0` transparently.
- eth_slow.h was not self-contained (used ETH_ALEN without including
  if_ether.h) — fixed, needed by ifmgmt.c including it.

## Status

- **2026-07-22 (b)**: **HW-VALIDATED FIRST GO**: `time iflinkwait -l
  net0-602` (debug `eth_slow` level 1 — NOTE from Kris: level 1 is
  serial-safe, it was `:3` that flapped the bundle) released in
  13.2s, right when the switch's LACPDU flipped to [AFGSCDlx]
  (in-sync+collecting+distributing); the log shows the full 802.1AX
  progression (partner defaulted/expired → learns us → bundles) and
  the VLAN→trunk redirection working.  THEN added `ifconf --lacp`
  (`-l`): waits via iflacpwait with new LACP_WAIT_TIMEOUT (60s;
  plain link wait stays 15s, which would have missed the observed
  13.2s) before configuring.  `ifconf()` gained an `int lacp` param
  (callers updated: autoboot passes 0).  Target one-liner is now
  `ifconf --lacp -c dhcp net0-602` after `ifopen net0` — wait +
  configure in one command, no sleeps.  EFI+BIOS build; ifconf
  --lacp not yet HW-tested (test: replaces the iflinkwait line;
  also regression-check plain `ifconf -c dhcp net4-4001`).

- **2026-07-22 (a)**: Feature implemented as above; EFI + BIOS builds
  pass.  NOT yet hardware-tested.  HW test plan (on the LACP trunk
  port, normal build — do NOT enable eth_slow debug over serial, it
  flaps the bundle; see the bnx2x project's notes): (1) `ifopen net0`
  then `iflinkwait --lacp --timeout 60000 net0` — expect the wait to
  end within a few seconds of the switch bundling the port (compare
  against the old sleep-20 workaround), then `ifconf -c dhcp
  net0-602` immediately — should succeed with no manual sleep.
  (2) Same but `iflinkwait --lacp net0-602` (VLAN device → trunk
  redirection). (3) On a NON-LACP port: `iflinkwait --lacp --timeout
  5000 net4` — expect timeout with "No LACP aggregation".
  (4) Regression: plain `iflinkwait net0` unchanged.  Possible
  follow-ups: fold an automatic LACP wait into `ifconf`? (upstream
  question); upstream RFC alongside/independent of the bnx2x driver.

## Build & test

```sh
cd src
make -j$(nproc) bin-x86_64-efi/ipxe.efi
util/genfsimg -o ipxe.iso bin-x86_64-efi/ipxe.efi   # for BMC virtual media
```

No new debug objects; relevant existing DEBUG names: `eth_slow`
(NEVER `:3` over serial on a live LACP port), `netdevice`, `ifmgmt`.

## Workflow

- Branch: `claude/lacp-linkwait` (based on `origin/bnx2x`; do not push
  elsewhere).  Keep commits small and buildable.
- Note: `debug/bnx2xdump.c` still exists on the `bnx2x` base branch —
  possibly missed when the dev files were stripped; flagged to Kris.
