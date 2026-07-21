# iPXE native bnx2x driver port

**This file is the living project charter and lab notebook for this branch.
Keep it up to date as work progresses — new sessions (possibly on different
machines, including one attached to real hardware over serial console) must be
able to resume from this document alone.**

## Goal

Produce a workable native iPXE driver for the Broadcom/QLogic NetXtreme II
10/20G family (Linux driver `bnx2x`), focusing specifically on:

- **BCM57810** — `pci:v000014E4d0000168E...` (2×10G)
- **BCM57840** — `pci:v000014E4d000016A1sv00001028sd00001F7A...` (Dell rNDC,
  4×10G)

Motivation: these NICs must PXE/EFI-boot on switch ports configured for
**VLAN over LACP**. The UEFI firmware SNP/UNDI path does not deliver LACPDUs
or VLAN-tagged frames to iPXE, so `snponly.efi`/`vcreate` and iPXE's passive
LACP responder (`src/net/eth_slow.c`) cannot work through it (confirmed
experimentally by the user; they fell back to BMC virtual ISOs). A native
driver gives iPXE raw control of the MAC, making both `vcreate` VLANs and the
LACP responder functional. See `src/interface/efi/efi_pci.c` (`efipci_driver`)
for how native drivers take the PCI device away from the firmware driver under
EFI.

Primary deliverable: `bin-x86_64-efi/ipxe.efi` containing a working `bnx2x`
driver (probe → link → RX/TX → DHCP/TFTP/HTTP boot over `vcreate`d VLAN with
LACP responder active).

## Current status (update this section every session!)

- **2026-07-21 (ax)**: **Source cleanup round 1 (comments + macro
  consistency), user-requested.** (1) Comments: removed every
  CLAUDE.md reference from driver sources (offsetof-method now
  described inline), fixed the badly stale bnx2x.c @file comment
  ("probe-only skeleton, no datapath yet"!), updated the stale
  link-status-validity TBD (validated long ago), reworded
  "sessions" → "opens"/"runs", generalised the rNDC-BMC note,
  clarified the inner-vlan-removal comment. Environment-specific
  hardware facts (Dell rNDC 0x168d chip-number quirk) kept — they
  are board facts, not workflow. (2) Macros: BNX2X_CL_ID +new
  BNX2X_HW_CID moved/added to bnx2x.h and now used in
  bnx2x_sp.c (sp_post hw_cid, stats cl_id) and bnx2x_hw.c (rx_diag
  qzone) which had the formulas inlined; rx_iobuf[120]/tx_iobuf[16]
  now sized by BNX2X_RX_FILL/BNX2X_TX_MAX_PENDING (both defined in
  bnx2x.h before the struct; eth.c copy removed) with a new
  build_assert(BNX2X_RX_FILL <= BNX2X_RCQ_USABLE); new
  BNX2X_RX_BD_SIZE/BNX2X_RCQ_CQE_SIZE/BNX2X_TX_BD_SIZE/
  BNX2X_FP_SB_SIZE/BNX2X_DB_STRIDE/BNX2X_HC_INDEX_* replace the
  scattered 8/64/16/0x40/*8/1/5 literals; TX doorbell write
  deduplicated into bnx2x_tx_doorbell() (transmit + stall re-kick).
  Both build variants pass. NO functional change intended — a
  quick soak/imgfetch re-check on next hardware session is enough.

- **2026-07-21 (aw)**: Wrote
  `src/drivers/net/bnx2x/LINUX_COMPARISON.md` (branch-only, RFC prep):
  full accounting of what the iPXE driver omits/changes vs Linux v6.6
  bnx2x and why, grouped as (A) impossible in iPXE (interrupts/NAPI,
  RSS, offloads, ethtool, DCB, SR-IOV, PTP, cnic), (B) wrong trade-off
  for boot firmware (14k-line PHY library vs MFW/LFA link, DMAE, ecore
  object framework, stats machine, parity recovery, NVRAM), (C)
  deliberate behavioral deltas (simplified prev_unload + eager RX
  quiesce, forced-SF only, full teardown per close, explicit 16-bit
  masking, ring sizes, E3-only), (D) iPXE-only additions. Known
  functional gaps worth remembering: 1G links dead (no UMAC), 20G
  mode unconfigured, SD/NPAR outer-tag TX missing, no jumbo.

- **2026-07-21 (av)**: **GOAL PATH + OS HANDOFF BOTH VALIDATED.**
  Clarification from the user: the (au) 629MB imgfetch runs were
  already over net0-602-over-LACP (nothing was reachable via
  net4/net5) — so the sustained-load check on the GOAL PATH is
  done. Then: vmlinuz+initrd fetched and BOOTED from iPXE over the
  same path; the initramfs (which currently expects a local ISO)
  dropped to its debug shell, where the user manually built bond0 →
  vlan602 → static IP on the KERNEL's own bnx2x driver and fetched
  the same ISO in 5.6s. That is the long-outstanding phase-2
  "OS driver works after our load/unload" check, in its strongest
  form (same-boot handoff, not even a warm reboot): iPXE hooks
  ExitBootServices, so our full close path (quiesce → HALT/
  TERMINATE/CFC_DEL → MCP unload) ran when the kernel took over,
  and Linux bnx2x then probed/bonded/tagged the port cleanly.
  REMAINING VALIDATION: just a DHCP soak to confirm no regression
  from the (as)/(at) datapath fixes. Then the cleanup/upstream
  list: bnx2x_fw.h 1.8MB → build-time fetch like other drivers,
  licence/FILE_SECBOOT review, decide fate of debug/bnx2xdump.c +
  src/debug.ipxe (branch-only), ipxe-devel RFC; optionally raise
  the EFI-watchdog starvation topic upstream.

- **2026-07-21 (au)**: **SUSTAINED-TRANSFER BUGS FIXED AND VALIDATED:
  imgfetch of the production 629MB ISO (659,554,304 bytes — the very
  image that was being served via BMC virtual media because iPXE
  wasn't usable) completed in 5.7s (~110MB/s) with md5sum verified
  clean.** That transfer crossed the 16-bit RX-CQ wrap ~7 times and
  traversed the TX BD page ~1,800 times, so the (as) doorbell
  counting and (at) consumer-bump wrap fixes are both confirmed on
  hardware. Datapath robustness work DONE pending two remaining
  transfer checks: (1) same imgfetch over net0-602-over-LACP
  (`ifopen net0 ; sleep 20` first), (2) a DHCP soak to confirm no
  regression. Then back to the (ap) cleanup list: warm-reboot-to-
  OS-driver check (user test, last phase-2 leftover); upstream
  trim: bnx2x_fw.h 1.8MB → build-time fetch like other drivers,
  licence/FILE_SECBOOT review, decide fate of debug/bnx2xdump.c +
  src/debug.ipxe (branch-only), then ipxe-devel RFC; optionally
  raise the EFI-watchdog starvation topic upstream.

- **2026-07-21 (at)**: **(as) doorbell fix CONFIRMED ON HW (no dup
  ACKs, sailed past the old 40MB wall at full speed) — then a NEW
  crash at ~93MB: instant GPF ⇒ UEFI restart, and 93,264,233 bytes
  ≈ 64,400 segments is EXACTLY the 16-bit wrap of the RX CQ slot
  counter (64,409 CQEs @mss 1448 + 1-per-63 page skips + open-time
  ramrods/ARP/DHCP ≈ 65,535). ROOT CAUSE (inspection): the
  next-page bump after reading the SB CQ index —
  `if ((hw_cons & 63) == 63) hw_cons++` — with hw_cons an
  `unsigned int`: when the fw index reads exactly 0xFFFF (a
  page-boundary value, so the bump fires) it becomes 0x10000,
  which `(rx_cq_cons & 0xffff)` can NEVER equal ⇒ the poll loop
  consumes the CQ ring forever, recycling NULL/stale rx_iobufs
  into netdev_rx ⇒ GPF within microseconds. Linux is immune only
  because its hw_comp_cons is a u16 (the ++ wraps to 0).** FIX:
  bump is now `hw_cons = (hw_cons + 1) & 0xffff` in
  bnx2x_eth_poll AND bnx2x_wait_ramrod_cqe; same latent pattern
  fixed in bnx2x_sp_wait_comp (EQ side; unreachable in practice
  but same class — eq_cons comparisons now masked too). Whether a
  run dies at the wrap is per-lap roulette (only if a poll READS
  the SB while the accumulated index == 0xFFFF); >100MB crosses
  the wrap at least once, so the previous imgfetch tests were all
  short of it or got lucky. HW test unchanged from (as): >100MB
  imgfetch plain port + net0-602-over-LACP + a DHCP soak. A
  transfer >190MB crosses the wrap TWICE — even better. TX-STALL
  tripwire from (ar) still armed.

- **2026-07-21 (as)**: **TX-death ROOT CAUSE FOUND BY INSPECTION
  (same session as (ar), before any HW run): the TX doorbell
  producer must count the ring's NEXT-PAGE element.** Linux
  bnx2x_start_xmit, right before the doorbell: "now send a tx
  doorbell, counting the next BD if the packet contains or ends
  with it" — `if (TX_BD_POFF(bd_prod) < nbd) nbd++`. We always did
  tx_db_prod += 2, so each traversal of the 256-slot TX BD page
  (every ~127 packets) left our doorbell value one BD further
  behind the firmware's slot-counting cursor; after enough drift
  the firmware stops processing TX doorbells entirely ⇒ the
  permanent TX silence at ~40MB (needs thousands of TX ACKs to
  accumulate — soaks/open-close/LACP never came close, which is
  why every earlier test passed). FIX: tx_db_prod += 3 when
  (tx_bd_prod & 255) < 2 after advancing past both BDs (poff 0/1 ⇔
  the two BDs contained or ended at slot 255), else += 2 — exact
  port of the Linux condition (nbd always 2 for us). The (ar)
  instrumentation stays in as a tripwire: if the theory is right
  the >100MB imgfetch now completes with ZERO "TX STALL" lines;
  any "TX STALL" line appearing means a second cause exists —
  paste it. HW test: imgfetch >100MB on plain port AND on
  net0-602-over-LACP (usual `ifopen net0 ; sleep 20` first), then
  confirm the DHCP soak still passes. Normal build is fine for the
  transfer test; `DEBUG=bnx2x_eth` (level 1, NOT :3) if you want
  the tripwire visible.

- **2026-07-21 (ar)**: **NEW BUG from re-test of (aq): TX dies
  PERMANENTLY mid-imgfetch (~40MB): after a dup-ACK burst iPXE goes
  totally silent — server exhausts its 2MB window unanswered, iPXE
  eventually reports connection timeout but not even the FIN/RST
  goes out.** Total TX silence = every transmit rejected = the
  BNX2X_TX_MAX_PENDING(16) gate returning ENOBUFS forever = TX
  completions stopped being harvested. THIS COMMIT instruments the
  stall path (no fix yet — cause unknown): (1) new
  bnx2x_tx_stall(), called on every ENOBUFS: re-rings the TX
  doorbell with the current tx_db_prod (idempotent absolute-value
  write — cheap lost-doorbell insurance; if the stall now
  self-heals, lost doorbell was the cause); (2) if the ring-full
  condition persists >1s, a ONE-SHOT DBGC dump (level 1 — visible
  with plain `DEBUG=bnx2x_eth`, deliberately NOT :3 which floods
  serial per-packet and perturbs timing): "TX STALL" lines with pkt
  prod/cons/db_prod, all 8 fp_sb index words + running index, and
  RX ring state; (3) "TX STALL recovered" one-shot when the gate
  passes again. READING THE DUMP: sb[5] != pkt cons ⇒ completions
  arrived but our harvest is broken (poll bug); sb[5] == pkt cons
  with prod-cons==16 ⇒ firmware stopped completing (doorbell lost
  or TX storm wedge — and if the re-kick heals it: doorbell);
  sb[] all frozen vs rx lines advancing ⇒ SB DMA died. HW test:
  rebuild with `DEBUG=bnx2x_eth` (level 1 ONLY), rerun the >100MB
  imgfetch on the plain port; paste any "TX STALL" lines. If no
  stall lines but transfer still dies, TX attempts stopped
  reaching the driver (netdev/TCP layer) — different hunt.

- **2026-07-20 (aq)**: **Sustained-RX bug found by imgfetch test:
  ~27MiB at full speed, then escalating TCP DUP-ACK storms, then
  permanent stall ⇒ connection timeout.** Two causes, both fixed
  (awaiting HW re-test): (1) FATAL: poll refilled the ring once per
  consumed CQE only — if alloc_iob failed (heap pressure while TCP
  buffers out-of-order segments during loss recovery), that ring
  slot was lost FOREVER; the ring shrank until RX died permanently.
  Refill is now a top-up loop on every poll (head-tail vs FILL) —
  self-healing after transient allocation failures. (2) CAPACITY:
  48 buffers cannot absorb a TCP window burst at 10G given iPXE
  polling latency ⇒ chronic overflow drops ⇒ the DUP-ACK storms
  that triggered (1). CQ ring is now 2 pages (126 usable CQEs,
  next-page pointer chains page0→page1→page0), BNX2X_RX_FILL 48→120
  (~261KB of the 4MB heap — core/malloc.c HEAP_SIZE is 4MB, so
  fine). RCQ index arithmetic reworked to per-page masks
  (BNX2X_RCQ_PER_PAGE 64; skip when (idx&63)==62; hw_cons bump when
  (hw&63)==63; slot = idx & 127). rx_iobuf[] sized to 120. RX fill
  threshold decision (ap) superseded by this. HW test: imgfetch of
  a large (>100MB) image on both a plain port and net0-602-over-
  LACP; watch for DUP-ACK storms (some early-window drops are
  normal, sustained storms are not) and verify completion + soak
  still passes. If throughput still poor, next lever is a 2-page BD
  ring + FILL ~250.

- **2026-07-20 (ap)**: **Bisect round 2 passed (single benign
  'discarding CQE' again; DHCP fine after all cycles) ⇒ init-step
  bisection COMPLETE**: final init sequence = Linux xmac_enable +
  CTRL=0+20ms cycle + XON toggle (both prev_unload-canonical,
  kept) + RX quiesce at close. **TX len-18 mystery SOLVED by code
  inspection: they are EAPOL-Start frames from iPXE's 802.1X
  supplicant (net/eapol.c: 3 × 18-byte frames per open, 2s apart —
  eth 14 + eapol 4). Harmless/expected; not driver traffic.**
  THIS COMMIT: stats_query_dump + rx_diag calls now gated behind
  `if ( DBG_EXTRA )` — compiled out of normal builds (linker GCs
  the functions), still available with the usual :3 DEBUG string.
  DECISION: BNX2X_RX_FILL stays 48 (~105KB heap) — robustness over
  memory for boot firmware; threshold hunt not worth the hardware
  time. REMAINING: warm-reboot-to-OS-driver check (user test, last
  phase-2 leftover); upstream trim: bnx2x_fw.h 1.8MB → build-time
  fetch like other drivers, licence/FILE_SECBOOT review, decide
  fate of debug/bnx2xdump.c + src/debug.ipxe (branch-only), then
  ipxe-devel RFC; optionally raise the EFI-watchdog starvation
  topic upstream.

- **2026-07-20 (ao)**: **FULL VALIDATION GAUNTLET PASSED**: 5×DHCP
  soak net4-4001 + 5×DHCP soak net0-602 (over LACP!) + 20×open-all/
  close-all + both soaks again — grep for 'unexpected CQE'/'timed
  out'/'BRB failed to drain'/'discarding CQE' found ONE benign
  "discarding CQE type 0 while awaiting ramrod" (the skip logic
  working as designed, once in 120 opens). TWO OPERATIONAL FINDINGS
  (user-diagnosed): (1) the mystery reboots were the **EFI Boot
  Watchdog** (NOT the BIOS OS watchdog; cannot be disabled in Dell
  setup): iPXE re-arms it every 10s via a process on the event loop
  (src/interface/efi/efi_watchdog.c, watch with
  DEBUG=efi_watchdog:3), but ifopen/ifclose don't run the event
  loop, so a tight script loop >5min gets killed by firmware —
  scripts must `sleep 1` between operations (sleep runs the event
  loop). Possibly worth an upstream discussion (long non-polling
  commands starve the watchdog). (2) DHCP over the LACP port needs
  bundle-sync time before the DHCP backoff starts: use
  `ifopen net0 ; sleep 20 ; ifconf -c dhcp net0-602`.
  THIS COMMIT (bisect round 1): removed the two disproven-premise
  experimental init writes — sibling XMAC enable (sibling was
  already enabled when measured) and legacy NIG_REG_EMAC0_IN_EN=1
  (no observed effect when introduced). KEPT (Linux-canonical):
  CTRL=0+20ms cycle and XON toggle in xmac_enable, both quiesce
  steps at close. Re-test at whatever depth is convenient (a soak +
  a few open-alls should do; full gauntlet if paranoid). Remaining
  cleanup: RXDIAG/STATS scaffolding decision, RX fill threshold
  bisect, warm-reboot-to-OS check, TX len-18 mystery, upstream trim
  (bnx2x_fw.h 1.8MB → build-time fetch).

- **2026-07-20 (an)**: **cl_id change regression found+fixed: the
  HALT ramrod carries the CLIENT ID in its SPE data field.** First
  single-port DHCP after the cl_id fix worked, but close failed:
  HALT/TERMINATE CQE timeouts then CFC_DEL/FUNC_STOP EQ timeouts.
  Linux bnx2x_q_send_halt posts HALT with data_lo = cl_id; we
  always passed 0, which silently matched while cl_id was 0 and
  broke the moment cl_id became igu_base_sb (=1 for pf0). Both
  HALT call sites now pass BNX2X_CL_ID as data. Also fixed the
  RXDIAG ustorm-prods readback to use qzone = igu_base_sb (was
  reading qzone 0 = all zeros since the cl_id change). Re-test
  sequence unchanged: cold boot → soak → open-all/close-all →
  soak.

- **2026-07-20 (am)**: **All-6-ports-open bug ROOT-CAUSED: client id
  collision between the two ports of a path.** Evidence: close
  failures grouped by PATH (net0+net2 = path0: net0 HALT timeout,
  net2 CFC_DEL/FUNC_STOP timeout; net1+net3 = path1: both wedged;
  57810s on their own paths closed clean), plus endless packet-CQE
  stream on reopen (chip wedged) ending in a crash/UEFI restart.
  Cause: BNX2X_CL_ID was (pfid>>1)<<2 = vn<<2 = 0 for ALL FOUR rNDC
  functions — but cl_id is also the QZONE id indexing the USTORM RX
  producers in PER-PATH storm RAM (IRO[217] + qzone*0x20), so both
  ports of a path clobbered each other's producers when open
  simultaneously. Linux E2+ rule (bnx2x_fp_cl_id): "Client ID must
  equal the IGU SB ID" (chip-wide unique). FIX: BNX2X_CL_ID =
  igu_base_sb (pf0=1 etc.); stats query index updated to match.
  Also added a runaway guard in wait_ramrod_cqe (max 2*RCQ_CNT
  discards ⇒ -EIO) so a corrupt completion queue can no longer hang
  iPXE until the UEFI watchdog restarts it. NOTE for retest: cl_id
  change affects single-port operation too (pf0 now cl_id 1, not
  0) — rerun the basic soak FIRST, then open-all/close-all, then
  soak again. COLD BOOT required after the previous wedge.

- **2026-07-20 (al)**: **RX quiesce validated (soak→smoke→cycle→soak
  passed); two new findings from all-6-ports-at-once testing.** (1)
  "unexpected CQE type 00" at HALT/TERMINATE on every port: packet
  CQEs queued ahead of the ramrod completion (frames arriving since
  the last poll + frames pushed through by the quiesce BRB-drain)
  were mistaken for the completion, desyncing rx_cq_cons — on
  net2/net3 this cascaded into CFC_DEL/FUNC_STOP EQ timeouts
  (TERMINATE mis-accounted ⇒ fw won't delete the connection). FIX
  (this commit): bnx2x_wait_ramrod_cqe now consumes+discards packet
  CQEs (freeing their iobufs, advancing tail) until the real ramrod
  CQE (type 1) arrives or 5s elapse. (2) SERVER SUDDENLY REBOOTED
  during testing: cause was the BIOS "OS Watchdog Timer" (iDRAC log
  showed it) — iPXE never pets it. **Disable the OS Watchdog in
  BIOS on iPXE test systems** (user has done so). Re-test: the
  debug.ipxe menu sequence incl. open-all-6/close-all-6, then soak
  again. Remaining bisect list unchanged (CTRL cycle, XON toggle,
  sibling enable, EMAC0_IN_EN), plus the rest of (ai)'s cleanup
  items.

- **2026-07-20 (ak)**: **Repeated open/close wedges RX without the
  NIG reset — root-caused to our own close path; RX quiesce added
  (awaiting HW re-test).** User sequence: DHCP soak PASSED → smoke
  +cycle tests (16 open/closes) → DHCP soak FAILED at cycle 0 with:
  CLIENT_SETUP completion garbled ("ramrod CQE flags 00"), partial
  RX then jam (tstorm counted 13, then BRB stuck at 11 blocks,
  INGRESS_EOP_PORT0 fifo non-empty), STATS query answered by
  t/xstorm but u/cstorm counters stayed 0xffff (USTORM wedged),
  every subsequent CQE/EQ completion timed out. Diagnosis: since
  (m) our ifclose left the MAC enabled and LLH gates open — fine
  while every open re-reset the NIG, but WITHOUT the NIG reset the
  frames still flowing after HALT/TERMINATE leave the NIG↔BRB
  handshake desynced (BRB gets table-soft-reset under a live NIG on
  the next open) ⇒ exactly the prev_unload hazard Linux guards
  against. FIX: bnx2x_xmac_quiesce() (bnx2x_hw.c), called FIRST in
  eth_close: closes LLH gates (set_rx_filter(0)), XON toggle +
  XMAC CTRL=0 + 20ms (prev_unload_close_mac equivalent), polls
  BRB1_REG_NUM_OF_FULL_BLOCKS→0 (≤1s) while the storms are still
  alive to drain. Note net4/57810 RMP dump now shows all-zero MFW
  rules even without our NIG reset (only 0x1023c/40 = 0x04000000)
  — the DHCP-steal concern may be rNDC-only; first soak PASSING
  without the NIG reset confirms DHCP works on net4. HW re-test:
  same sequence (soak → smoke → cycle → soak, cold boot start);
  everything should now pass. If the wedge persists, options:
  restore RST_NIG per-open, or investigate residual NIG state
  (INGRESS_EOP fifo) at open. User's src/debug.ipxe (committed to
  branch) provides a menu for all these tests.

- **2026-07-20 (aj)**: **Cleanup round 1 + NIG-reset removal
  experiment (awaiting HW test).** Changes: (1) reset_common mask
  reverted to Linux 0xd3ffff7f — the NIG is NO LONGER reset, so the
  MFW RMP steering rules survive (incl. UDP 67/68/547 DHCP-steal
  rules and the mgmt-MAC rule; BMC inband mgmt keeps working).
  Risk to verify on HW: DHCPv4 over VLAN (net4-4001) might break if
  the MFW steals port-67/68 replies — the RMP dump in RXDIAG shows
  the active rules again; IPv6/SLAAC should be unaffected (ICMPv6).
  If DHCP breaks: options are (a) restore the NIG reset behind a
  build option, (b) surgically clear just the DEST_UDP/TCP RMP
  rules (0x10214/0x10218/0x10220 region) at ifopen — gentler than a
  full NIG reset, MFW MAC rule kept. (2) Probes deleted: XMACPRE
  dump, bnx2x_lb_test (NIG debug injection), bnx2x_mac_lb_test
  (XMAC loopback) + all call sites. KEPT for this bisect round:
  RXDIAG dumps + STATS query at close, CTRL=0+20ms cycle, XON
  toggle, sibling XMAC enable, EMAC0_IN_EN=1 (bisect those next,
  one per boot, AFTER the NIG-reset verdict). Test scripts for
  embedding added below ("Test scripts"). HW test: cold boot,
  usual DEBUG string; (a) ifopen net0 + arping → RX>0 expected
  (gates are driver-owned, reset not needed for plain RX);
  (b) `vcreate --tag 4001 net4` + `ifconf -c dhcp net4-4001` — THE
  critical check (RMP DHCP-steal); (c) ipv6 over net0-602 + LACP as
  in (ai). Paste STATS+RXDIAG (rmp dump now nonzero again) if
  anything fails.

- **2026-07-20 (ai)**: **PROJECT GOAL ACHIEVED: `ifconf -c ipv6
  net0-602` completes over VLAN 602 over an ACTIVE LACP bundle on
  the 57840 rNDC.** The eth_slow passive responder keeps the switch
  port-channel bundled (visible as periodic TX len-124 LACPDUs).
  Caveat discovered: building with `eth_slow:3` debug makes LACP
  flap — serial console logging stalls the poll loop past the
  fast-LACP ~3s response window; do NOT enable eth_slow debug over
  serial on a live LACP port. NEXT (cleanup pass, in order):
  (1) strip diag scaffolding: LBTEST/MACLB functions + calls,
  RXDIAG dumps, STATS dump at close (maybe keep behind DBGC2?),
  XMACPRE dump, debug/bnx2xdump.c;
  (2) bisect the experimental init steps one boot each — candidates
  to REMOVE if RX still works without them: forced XMAC hard reset
  (aa/u), CTRL=0+20ms cycle (t), XON PFC_CTRL_HI toggle (s),
  sibling XMAC enable (aa), EMAC0_IN_EN=1 (x); DECIDE on the NIG
  reset (v): keeping it wipes the MFW RMP DHCP-steal rules (good
  for DHCP!) but kills BMC inband mgmt on these ports while iPXE
  runs — on this system BMC uses the dedicated iDRAC port, and a
  reboot restores MFW config, so KEEP unless upstream objects;
  (3) find the real RX fill threshold (bisect 8..48; keep margin);
  (4) warm-reboot-to-OS-driver check (still outstanding from
  phase 2!); open/close/open cycles; both 57810 ports + all four
  rNDC ports smoke test;
  (5) investigate the TX len-18 header-only frames seen on some
  opens (upper-layer frame? mis-built iobuf?);
  (6) size/trim for upstream: FILE_SECBOOT review, licence headers,
  bnx2x_fw.h size (1.8MB header!) — consider fetching fw at build
  time like other drivers, then ipxe-devel RFC.

- **2026-07-20 (ah)**: **PHASE 6a COMPLETE: vcreate VLANs work on
  both cards.** `ifconf -c ipv6 net0-602` → ok (SLAAC over tagged
  VLAN 602 = tagged RS out, tagged RA in, on the 57840 rNDC);
  `ifconf -c dhcp net4-4001` → ok (full DHCPv4 exchange over tagged
  VLAN 4001 on the 57810). This is the capability the vendor
  SNP/UNDI could never provide. REMAINING: (b) LACP — re-enable
  LACP on the switch port (add back to port-channel), ifopen net0,
  wait 30-90s, check switch port-channel status (iPXE's passive
  eth_slow responder should bring the link into the bundle; build
  with eth_slow:3 in DEBUG to watch LACPDUs); then vcreate+dhcp
  over the bundled port = the original goal scenario. Then the
  cleanup pass listed in (ag).

- **2026-07-20 (ag)**: **RX WORKS ON BOTH CHIPS.** ifstat RX:14
  (net0/57840) and RX:15 (net4/57810); STATS shows tstorm accepting
  6/5/3 and ustorm no_buff 0 — the BNX2X_RX_FILL 8→48 change was
  the fix: the storm firmware has a minimum-free-RX-buffer
  threshold somewhere above 8, and every frame was dropped
  no_buff_discard until the ring was properly stocked. (The RXE 1
  "Operation not supported" per port was the MACLB self-test frame
  — experimental ethertype rejected by the net stack after
  SUCCESSFUL delivery; MACLB now disabled in eth_open, function
  kept __unused for regression use.) PHASES 4+5 COMPLETE. **PHASE 6
  NOW: (a) `ifopen net0` + `vcreate --tag 602 net0` + `dhcp
  net0-602` on the tagged-VLAN-602 switch port — note our NIG reset
  wipes the MFW RMP rules incl. the UDP 67/68 DHCP-steal rules, so
  DHCP replies should reach the host; (b) re-enable LACP on the
  switch port (add to port-channel), ifopen net0, wait, check
  switch port-channel status — iPXE's eth_slow LACP responder
  should bring the channel up (we accept all-multicast so LACPDUs
  on 01:80:c2:00:00:02 are delivered; verify with DEBUG add
  eth_slow if needed); then vcreate+dhcp over the LACP port.**
  After phase 6 validates: cleanup pass — strip diag scaffolding
  (LBTEST/MACLB/RXDIAG/STATS dump/XMACPRE), re-test which
  experimental init steps are actually needed (forced XMAC reset,
  CTRL cycle, XON toggle, sibling XMAC enable, EMAC0_IN_EN, NIG
  reset — keep NIG reset? it conveniently kills the MFW DHCP-steal
  RMP rules, but breaks BMC inband mgmt via these ports), find the
  real fill threshold (bisect 8..48), consider CQ ring of 2 pages +
  deeper BD ring, TX len-18 mystery frames, warm-reboot-to-OS
  check, then trim for upstream.

- **2026-07-20 (af)**: **STATS_QUERY WORKED AND NAMED THE FAULT:
  TSTORM accepts every frame for our client (net0: ucast 6/bcast 5/
  mcast 3 = exactly the test traffic; net4 long run: 6560 frames),
  ZERO classification discards, and USTORM drops every single one
  with no_buff_discard (counts match 1:1).** The whole MAC/NIG/PRS
  hunt was chasing phantoms — those counters are simply
  dead/clear-on-read on E3; the ingress pipeline has worked all
  along. USTORM believes there are no RX buffers. Verified correct
  via offsets tool + IRO dump: client_init offsets ALL correct
  (general/rx/tx @0x00/0x10/0x60, state @rx+0x34, max_bytes
  @rx+0x12, bd/cqe_page @rx+0x18/0x28...), prods struct order
  (LE: cqe_prod low16 | bd_prod high16 = our 0x00080009 = cqe 9 bd
  8 CORRECT), IRO[217] = base 0x6000 m1 0x20 size 8 (canonical
  USTORM qzone; qzone=cl_id=0 for pf0 on both test NICs), IRO[213]
  agg data base 0xa000 size 0x2000. Remaining delta vs Linux:
  Linux fills the whole ~500-buffer BD ring; we posted only 8.
  Theory: fw has a minimum-free-buffer/batching threshold > 8 ⇒
  permanent no_buff. THIS BUILD: BNX2X_RX_FILL 8 → 48 (rx_iobuf[]
  sized to match; 48 < 63 usable CQEs of the single-page CQ ring).
  If RX works: phase 6 time (also re-lower later to find the real
  threshold + grow CQ to 2 pages if we want deeper rings). Also:
  hostdump.c moved to debug/bnx2xdump.c (it broke the iPXE build
  inside SRCDIRS). Linux-side mmap of resource0 failed with EINVAL
  under lockdown LSM — alternatives: boot params lockdown=none (SB
  off), or use `ethtool -d <if> raw on > regs.bin` (bnx2x get_regs
  curated dump) — but with no_buff identified we likely don't need
  the Linux capture. Test: usual DEBUG string, ifopen net0, arping
  → **expect ifstat RX>0 at last**; then vcreate+dhcp, then LACP.

- **2026-07-20 (ae)**: **STATS_QUERY ramrod ported + Linux-side
  comparison tool added (no custom kernel needed).** (1)
  bnx2x_stats_query_dump() in bnx2x_sp.c, called at ifclose before
  rx_diag: posts RAMROD_CMD_ID_COMMON_STAT_QUERY (=6, cid 0, NONE
  type, data = one page: header@0 {cmd_num=2, drv_stats_counter=0,
  counters addr regpair} + 2×16B query entries {kind,index,funcID,
  addr regpair}; PORT query kind=1 → reply@+0x840, QUEUE query
  kind=0 index=cl_id → reply@+0x880), waits for EQ opcode 5
  (EVENT_RING_OPCODE_STAT_QUERY) then polls the completion counters
  block (@+0x800, pre-filled 0xff; fw echoes drv_stats_counter=0
  per storm: x/t/u/c at dwords 0/2/4/6). New debug lines: "STATS
  counters/port/tstorm(q)/ustorm(q)". Reply layouts (hsi): tstorm
  per-port {mac_discard, mac_filter_discard, brb_truncate_discard,
  mf_tag_discard, packet_drop}; per-queue = tstorm{ucast/bcast/
  mcast bytes+pkts, checksum/too_big/ttl0/no_buff discards}@0 +
  ustorm{*_no_buff_pkts @dw20-22}@0x38 + xstorm{*_pkts_sent,
  error_drop @dw34-37}@0x70. (2) CLIENT_SETUP now ENABLES
  statistics (general data: statistics_counter_id=cl_id, en_flg=1,
  zero_flg=1 — offsets 1/2/8; activate_flg@4 confirmed already
  set). (3) NEW: src/drivers/net/bnx2x/hostdump.c — userspace tool
  (gcc -O2 -o bnx2xdump hostdump.c; sudo ./bnx2xdump /sys/bus/pci/
  devices/<addr>/resource0) that mmaps BAR0 on a RUNNING LINUX
  system and prints the same RXDIAG lines + wide XMAC/NIG/MISC
  windows — run it against a port with working Linux RX (ethtool -i
  for the PCI addr; port-0 functions only: 01:00.0 = net0-equiv,
  03:00.0 = 57810) to capture a known-good state to diff against
  our output. CAUTION: MSTAT/PRS counters are clear-on-read; may
  briefly perturb kernel stats. Remove hostdump.c before upstream.
  Reading the STATS output: tstorm(q) rcv_*_pkts>0 ⇒ frames DID
  reach TSTORM for our client (then look at u/xstorm + no_buff);
  all zero + port counters zero ⇒ storm firmware never saw them ⇒
  back to PRS→TCM. mac_filter_discard>0 ⇒ classification drop.
  Test: usual DEBUG string, cold boot, ifopen net0, arping,
  ifclose (STATS lines appear before RXDIAG); same on net4.

- **2026-07-20 (ad)**: **AGG_DATA/IGU_MODE fix did NOT change
  behaviour. But the clean-pipeline 57810 run reproduced the real
  trickle: prs_packets 1 at close with NO runts injected — one
  genuine MAC-side frame (MACLB frame or the single wire unicast)
  reached the parser, then nothing, and it produced no CQE with
  prods untouched and zero errors anywhere. 57840: still prs 0
  always.** Reproducible pattern: 2-port passes exactly ~1 frame
  then stalls; 4-port passes 0. Signature = single-buffer handoff
  then permanent stall somewhere in PRS→TCM/TSTORM→USTORM: the
  frame is consumed, never placed (no CQE, no drop counter we can
  see, no error, prods frozen). Ideas standing: (a) client state
  machine — CLIENT_SETUP put client in DROP_ALL-ish initial state
  and FILTER_RULES may not have taken effect on the right
  client/path (silent TSTORM drop wouldn't stall PRS though); (b)
  something in the TSTORM→USTORM placement handshake never
  completes, wedging the one-frame pipeline. **NEXT STEP (new
  session): port the STATS_QUERY ramrod (bnx2x_stats.c machinery:
  stats_query_header/cmd_group, DMAE-back per-queue+per-port
  TSTORM/USTORM stats) to see rcv_*_pkts vs *_discard counters —
  that names the storm-side fate of the frame(s). Also re-audit
  client_init_ramrod_data byte offsets (esp. rx state flags,
  cache_line_log, sb index ids) and FILTER_RULES echo/cid handling
  against Linux bnx2x_q_fill_init_* one more time with fresh eyes;
  and consider testing with UCAST_ACCEPT_ALL added to filter state
  to bypass classification.** 4-port extra deadness (0 vs 1 frame)
  remains unexplained on top of the common stall.

- **2026-07-20 (ac)**: **Clean-pipeline run: prs 0 on BOTH chips ⇒
  the earlier "trickle" was almost certainly the LBTEST runts
  echoing in the counter, NOT MAC frames. Storm/CFC diag all clean
  (no latched errors; lcids inside_pf 1 = our ETH connection). So:
  no MAC-side frame has EVER reached the parser on either chip.**
  MACLB still fails everywhere. THEN: found a genuinely missing
  init step — Linux bnx2x_init_internal_common (nic_init path, run
  on every COMMON load) which we never ported: (1) zeroes
  USTORM_AGG_DATA (IRO[213], ~size dwords) with the comment "Zero
  this manually as its initialization is currently missing in the
  initTool" — the fw init tables leave USTORM aggregation data as
  GARBAGE; uninitialised agg state can wedge USTORM RX placement,
  which would backpressure TSTORM→PRS→NIG→MAC on ALL chips
  identically (fits every symptom, incl. why ramrod CQEs still
  work); (2) writes the CSTORM IGU mode byte (IRO[161].base,
  REG_WR8) = HC_IGU_NBC_MODE (1) since we force the IGU to normal
  (non-BC) mode at init. BOTH now ported into bnx2x_sp_init()
  (before def-SB setup; bnx2x_sp.c), defines in bnx2x_sp.h. Also
  checked bnx2x_pf_init/bnx2x_func_init for other gaps: func_cfg is
  E1x-only, vf_to_pf/func_en/SPQ we already do, cmng is TX rate
  shaping only (skippable), IGU stats zeroing cosmetic ⇒ nothing
  else missing there. Web search for XMAC/Broadcom register docs:
  no public spec found. Test: cold boot, usual DEBUG string, ifopen
  net0 (MACLB auto) + arping + ifclose; MACLB idx1 advancing and/or
  ifstat RX>0 = FIXED (then vcreate+dhcp+phase 6!); if still dead,
  next step is porting the STATS_QUERY ramrod for TSTORM drop
  counters, and asking on ipxe-devel/Broadcom for the XMAC→NIG
  system-side spec.

- **2026-07-20 (ab)**: **Fault relocated: the MAC→NIG→BRB→PRS path
  is (at least partly) ALIVE on BOTH chips — the stall is at the
  parser→storm handoff.** Evidence: 57840 sibling-enable run showed
  prs_packets 2 at close (FIRST EVER MAC-side frames at the parser
  on the 4-port chip; sibling ctrl read 3 = was already enabled, so
  the sibling theory's premise was wrong but the run moved the
  needle anyway); 57810 runs showed 1 then 0. Pattern = a trickle
  (0-2 frames) passes, then nothing: classic credit/backpressure
  stall — the parser hands each frame to TCM/TSTORM with per-CID
  activity counting through the CFC; if that handshake sticks, the
  parser accepts its internal buffer's worth and backpressures the
  whole ingress path forever (which is what all the MAC-boundary
  symptoms were). MACLB failing on the 57810 even with the fixed
  source MAC fits too (its frame queued behind the stalled LBTEST
  runts?). Checked and CLEARED: CFC_REG_DEBUG0=0 after CFC polls is
  faithful to Linux (7458); TCM_REG_PRS_IFEN=1 and TCM_CFC_IFEN=1
  from tables; PRS_REG_NIC_MODE=1 written in func init; CFC search
  credit untouched by tables (default). This commit: (1) new
  "RXDIAG storm" line — PRS_INT_STS, PRS_NUM_OF_DEAD_CYCLES,
  TCM_INT_STS, TSDM_INT_STS_0, TSDM_ENABLE_IN1, CFC_INT_STS,
  CFC_ERROR_VECTOR, CFC NUM_LCIDS_ARRIVING/ALLOC/LEAVING/INSIDE_PF
  — a latched error or stuck LCID counter names the guilty block;
  (2) LBTEST injection DISABLED (the 16-byte runts are malformed
  and may themselves plug the parser ahead of legitimate frames —
  MACLB must test a clean pipeline). Test: cold boot, usual DEBUG
  string, ifopen net0 (MACLB auto), arping, ifclose; also same on
  net4 if convenient. KEY LINES: MACLB + "RXDIAG storm" +
  prs_packets. If MACLB now passes without the runts ahead of it ⇒
  LBTEST was the plug all along (post-(t) runs anyway) and wire RX
  may just work; if storm line shows CFC/TCM errors ⇒ attack that
  block's config (CDU context validation / activity counters).

- **2026-07-20 (aa)**: **57810 CROSS-CHECK IS THE BREAKTHROUGH: on
  the 2-port chip ONE frame reached the parser (RXDIAG prs_packets
  1, after LBTEST's clear-on-read) — almost certainly the MACLB
  loopback frame, dropped before the CQE by storm self-source-MAC
  pruning (our test frame used our own MAC as source). On the
  57840, prs has NEVER counted a MAC-side frame.** ⇒ MAC→NIG works
  in 2-port (Single Port) mode and is dead in 4-port (Dual Port)
  mode. New prime theory: MISC_REG_XMAC_CORE_PORT_MODE description
  says it is a strap for the "XMAC_MP core" — ONE multi-port MAC
  core per path whose system side in Dual Port Mode (=1) is a
  time-multiplexed interface shared by both ports; the mux may only
  run when BOTH port instances are enabled. Vendor epoch: all four
  UEFI driver instances active (all XMACs enabled) → RX worked.
  iPXE disconnects every vendor instance (each Stop() disables its
  MAC), we enable only our own port ⇒ mux dead ⇒ explains every
  observation incl. TX working (NIG-side muxing) and all state
  combinations failing. This build: (1) MACLB source MAC changed to
  NOT be our own (last byte ^1) so storm self-source pruning cannot
  mask a working path — on the 57810 MACLB should now produce a
  real RX CQE (idx1 advance, ifstat RX 1); (2) 57840 4-port: after
  enabling our XMAC, minimally configure + enable the SIBLING XMAC
  of the path (other port's instance; logs "sibling XMAC ctrl was
  X" — vendor value informative: 0 would confirm UEFI Stop()
  disabled it). Tests (cold boot, usual DEBUG string): (a) ifopen
  net4 / ifclose net4 — expect MACLB idx1 to ADVANCE now; (b)
  ifopen net0 + arping + ifclose — if sibling theory right, MACLB
  passes and wire RX works ⇒ phase 6. Paste MACLB + sibling +
  RXDIAG lines for both.

- **2026-07-20 (z)**: **XMAC FULLY EXONERATED: MACLB fails even on
  verbatim vendor XMAC state (no reset, all registers vendor's own,
  wide 0x00-0x1fc window recorded and identical).** Every reset
  combination has now failed identically: vendor XMAC+vendor NIG
  (runs ≤t), reset XMAC+vendor NIG (u), reset XMAC+reset NIG (v-x),
  vendor XMAC+reset NIG (y). The XMAC↔NIG boundary never passes an
  RX frame for us in ANY state while TX always works and the vendor
  UEFI driver's RX worked. Wide-dump extras (vendor values, FYI):
  +0x88/8c = 01-80-C2-00-00-01 pause DA, +0xdc = 01380000
  (EEE-timer-ish), +0xe0..0xfc = port MAC ×4, +0x100 = 000a93dc.
  LBTEST prs=2 exactly this run (clean). PORT4MODE straps are NOT
  touched by init tables (sim-verified) so vendor values persist.
  **NEXT TEST (no rebuild needed — current build): run the SAME
  procedure on the 57810 (2-port mode, different board path, full
  Linux-identical XMAC bring-up since the 4-port skip doesn't apply
  there): cold boot, ifopen net4 (or net5; the two 57810 ports),
  MACLB line appears automatically with no wire traffic needed,
  ifclose, paste everything.** MACLB passes on 57810 ⇒ fault is
  4-port/rNDC-specific (CORE_PORT_MODE semantics, 4-port XLGMII
  muxing) ⇒ attack that. MACLB fails on 57810 too ⇒ our common/port
  init breaks XMAC→NIG RX on ALL E3 ⇒ re-diff init_hw_common/port
  against Linux yet again with fresh eyes (esp. blocks between MISC
  and NIG), and consider porting bnx2x's full first-load ordering
  (bnx2x_nic_load sequence) more faithfully. Also record which
  switch port the 57810s are cabled to for a wire test.

- **2026-07-20 (y)**: **XMACPRE vs our config diff came back CLEAN —
  the vendor XMAC registers (0x00-0x7c) are IDENTICAL to ours**
  (incl. unnamed MODE=0x40 @+0x08, RX_CTRL=0x40c @+0x30,
  RX_VLAN_TAG=0x81008100/3 @+0x48, 0x8808 @+0x78; only diff:
  RX_LSS_CTRL we write 3, vendor 0 — benign direction). Also
  learned: **MSTAT counters are CLEAR-ON-READ** (mstat_rx 5->1
  across MACLB = 5 cleared, 1 = the loopback frame); mstat1_rx=4 is
  path0-port1's own switch chatter (its MSTAT got reset by common
  init), NOT our frames. MACLB still failed after our reset with
  vendor-identical config ⇒ either the difference hides ABOVE +0x7c
  (EEE_CTRL @+0xd8 which we zero, or other regs 0x80-0x1fc), or the
  XMAC registers are innocent and our FORCED HARD RESET breaks
  something unrestorable (e.g. XMAC↔WC/NIG attach done by firmware
  at power-up). This commit: (1) restore the Linux 4-port skip (do
  NOT reset the XMAC when already out of reset → run on verbatim
  vendor state), (2) widen XMACPRE + RXDIAG xmac dumps to 128 dwords
  (0x00-0x1fc). Decision matrix: MACLB passes now ⇒ villain was our
  reset + some register above 0x7c (diff the wide dumps, restore it,
  and if none differs: never reset the XMAC, rely on CTRL-cycle);
  wire RX too ⇒ DONE, phase 6. MACLB still fails on verbatim vendor
  XMAC ⇒ XMAC fully exonerated ⇒ hunt shifts to the NIG P0 RX
  interface / system-side clocking (MISC_REG_XMAC_CORE_PORT_MODE
  semantics, NIG P0-region unnamed regs 0x185xx, or ask Broadcom
  docs). Test: cold boot, usual DEBUG string, ifopen net0, arping,
  ifclose; paste XMACPRE + MACLB + RXDIAG (both wide dumps).

- **2026-07-20 (x)**: **MACLB verdict: XMAC system side broken
  generally — even the XMAC's own line-looped frame (mstat_rx 0->1,
  counted at the same tap as wire frames) never reaches the NIG
  (idx1 1->1). port_swap 0, MSTAT1 all zero, p1_macfifo empty ⇒ no
  crossed port.** So the fault is inside the XMAC between line RX
  (counts fine) and system-side output (dead), with TX crossing the
  same boundary fine. Prime suspects now: XMAC registers NO bnx2x
  driver ever writes, sitting at raw silicon defaults since we hard
  reset the block. The XMAC is Broadcom switch IP; its real register
  map (offsets match bnx2x_reg.h exactly) includes XMAC_MODE (+0x08)
  and XMAC_RX_CTRL (+0x30), RX SA (+0x38), RX_VLAN_TAG (+0x48) —
  all unnamed in bnx2x_reg.h. Under the vendor UEFI driver these
  held working values; a full Linux bnx2x load never resets the
  XMAC on this board (4-port skip) so it inherits them; WE reset
  the block and never restore the unknowns. This round (commit):
  (1) "XMACPRE" dump — the full 32-dword XMAC window BEFORE our
  reset: **on a cold boot this is the vendor driver's working
  config, the reference to diff against**; (2) "RXDIAG xmac" dump —
  same window at close (our config); (3) "RXDIAG legacy" line —
  NIG_EMAC0_EN/EMAC0_IN_EN/BMAC0_IN/OUT/REGS/EGRESS_EMAC0_OUT/
  NO_CRC (legacy gates, also silicon-default since our NIG reset);
  (4) experiment: NIG_REG_EMAC0_IN_EN=1 written in xmac_enable
  (candidate second series gate; harmless — EMAC is in reset).
  **Test MUST be a cold boot** (so XMACPRE captures vendor state):
  usual DEBUG string, ifopen net0, arping/bcast, ifclose. Paste
  XMACPRE + MACLB + all RXDIAG lines. Next action: diff XMACPRE vs
  "RXDIAG xmac", identify the RX-side register(s) we fail to set
  (started for the diff: dwords 0-1 CTRL, 8-9 =+0x20 TX_CTRL,
  10-11 =+0x28 TX SA, 12-13 =+0x30 RX_CTRL!, 14-15 =+0x38 RX SA,
  16 =+0x40 RX_MAX_SIZE, 18 =+0x48 RX_VLAN_TAG?, 20 =+0x50
  RX_LSS_CTRL, 26 =+0x68 PAUSE_CTRL, 28/29 =+0x70/74 PFC_CTRL/HI),
  then write them from xmac_enable and drop the probes.

- **2026-07-20 (w)**: **NIG reset did NOT fix wire RX either — but it
  proves the wedge theory wrong and narrows the fault to the
  XMAC↔NIG interface itself: freshly-reset NIG (RMP dump now all
  zeros = reset provably took effect) + freshly-reset XMAC + perfect
  config, LB injection reaches PRS, wire frames still counted at MAC
  line side (12 grpok incl. the arpings) and never enter the P0 RX
  MACFIFO.** New probes in this commit (no fix attempt): (1)
  bnx2x_mac_lb_test() in bnx2x_eth.c — at open, enables XMAC
  LINE_LOCAL_LPBK (CTRL bit2), TXes one self-addressed 60-byte frame
  (ethertype 0x88b5) through the normal ring (iobuf manually put on
  netdev->tx_queue since netdev_tx refuses mid-open), waits ≤100ms
  for fp_sb index1 to move, restores CTRL; logs "MACLB idx1 a->b
  mstat_rx x->y tx_gtpkt z". Line-local loopback returns TX at the
  MAC line side = same tap where MSTAT counts wire frames. (2) RXDIAG
  "misc" line: NIG_REG_PORT_SWAP (0x10394), STRAP_OVERRIDE (0x10398),
  MSTAT1 tx/rx/pok counters, P1_RX_MACFIFO_EMPTY (0x1858c) — checks
  the crossed-RX-mux theory (frames landing at NIG P1/LLH1 whose
  gates are closed).
  Interpretation: MACLB idx1 advances ⇒ whole XMAC-RX→host path good
  ⇒ wire loss is WC→XMAC coupling or line-side frame marking (weird,
  since MSTAT counts them as good — then compare mstat_rx delta for
  the loopback frame vs wire frames). MACLB dead + mstat_rx
  incremented ⇒ XMAC system side broken generally → attack
  CORE_PORT_MODE/system-side config next. mstat1 counters nonzero ⇒
  crossing to port 1 → open LLH1/P1 gates + maybe enable XMAC1.
  Test: usual DEBUG string, ifopen net0 (MACLB runs automatically),
  wire arping as before, ifclose, paste MACLB + all RXDIAG lines.

- **2026-07-20 (v)**: **Forced XMAC reset did NOT help; switch-port
  bounce did NOT help ⇒ new best theory: the NIG P0 LLH input state
  machine is WEDGED (mid-packet cut during the UNDI→iPXE handover /
  our init-over-live-NIG), and survives everything because the NIG
  is the one block never reset (Linux excludes RST_NIG only to keep
  the MCP/BMC path alive).** Supporting logic: under the vendor UEFI
  driver RX WORKED (SNP delivered untagged frames — the original
  problem was only VLAN/LACP), so XMAC→NIG was fine before we took
  over; LB LLH (separate input machine) works (LBTEST prs 5 again);
  every register/config readback is perfect; MAC-side resets/edges
  change nothing. xmac_lss=1 this run = latched local-fault from our
  hard reset + the port bounce; informational only. FIX in this
  commit (one line): bnx2x_reset_common now clears 0xd3ffffff (adds
  bit 7 RST_NIG) so the NIG is fully reset and then rebuilt by the
  SAME init tables we already run (Linux's parity-recovery
  process_kill does exactly this: REG_1 0xffffffff incl. NIG with
  MCP alive, then normal init). Consequence: MFW's NIG config (RMP
  steering etc.) is wiped while iPXE owns the NIC — BMC inband
  management via these ports would break until MFW reconfigures;
  user's BMC is on the dedicated iDRAC port, acceptable. The forced
  XMAC reset + CTRL cycle + XON toggle from (t)/(u) are left in for
  now — once RX works, revisit and strip the unnecessary ones one at
  a time. Test: usual DEBUG string; cold boot recommended (clean
  epoch), ifopen net0, arping/bcast-ping from VLAN 602 host,
  ifclose; success = ifstat RX>0. If RX works: immediately also test
  `vcreate --tag 602 net0` + `dhcp net0-602` (watch for the MFW
  UDP-67/68 RMP steal — should be gone now since RMP rules are wiped
  by the NIG reset!) and then phase 6 LACP. If still dead: next
  probes are LLH1-gate-open test (crossed RX mux theory) and XMAC
  line-local loopback (CTRL bit2) to bisect line-vs-system RX.

- **2026-07-20 (u)**: **BISECTION COMPLETE: NIG/BRB/PRS proven good
  (LBTEST prs_packets 4), wire RX still dead ⇒ blockage is INSIDE
  the XMAC→LLH hop.** CTRL off/on cycle did not help either. Also
  learned: PRS_REG_NUM_OF_PACKETS appears CLEAR-ON-READ (read 4 at
  LBTEST, 0 at close) — earlier zero readings remain valid (each was
  the first read after a traffic window). LBTEST counted 4 for 2
  injected packets (maybe per-beat counting or E2/E3 debug format
  framing — irrelevant, packets flowed). The injected multicasts did
  NOT surface as RX CQEs (fp_sb idx1 stayed 1, ifstat RX 0) — 16-byte
  runts with garbage protocol presumably dropped at/after PRS;
  acceptable. Remaining theory: XMAC core state inherited from the
  vendor UEFI epoch (we, like Linux LFA, skip the hard reset in
  4-port mode when already out of reset) leaves the MAC system side
  detached from the NIG; Linux first-load recovers because non-LFA
  loads run the FULL PHY init (warpcore retrain + link flap) which
  resyncs WC↔XMAC↔NIG. New experiment (this commit): FORCE the XMAC
  hard reset + CORE_PORT_MODE/PHY_PORT_MODE + soft reset on every
  xmac_enable (drop the 4-port skip — no other driver instance can
  own the path's other port under iPXE; may briefly disrupt BMC
  sideband if it rides this port). Expect new debug line "XMAC out
  of reset (4-port): resetting anyway".
  **Test (two stages in ONE run): (1) usual ifopen net0 +
  arping/bcast-ping from VLAN 602 host; if RX still 0, then (2)
  WITH net0 still open, bounce the switch port (shutdown / no
  shutdown) and re-try the arping.** If RX starts working only
  after the bounce ⇒ the missing piece is link-establishment-time
  WC↔XMAC RX sync, and the likely production fix is: at ifopen do
  UNLOAD_DONE *without* SKIP_LINK_RESET first (MFW resets and
  re-trains the PHY itself while no driver is loaded) before
  LOAD_REQ+LFA — no bnx2x_link.c port needed. If RX works
  immediately after (1) ⇒ forced XMAC reset was the fix. Collect
  LBTEST + RXDIAG + ifstat as usual, plus note WHEN (if at all) RX
  came alive relative to the bounce.

- **2026-07-20 (t)**: **XON toggle did NOT fix RX. Two new probes in
  one build: XMAC CTRL off/on cycle (fix candidate 2) + NIG debug
  packet injection (bisection test).** Latest run: 18 good frames at
  MAC (5 uca/8 mca/5 bca, grpok 18), prs still 0; new sts line clean
  (rx_macfifo_empty 1, prty 0/0; int0=1 is bit0 address-error, almost
  certainly self-inflicted by our diag reading undocumented
  0x1023c/0x10240 — ignore). Switch counters this time confirm ALL 5
  iPXE TX frames arrived (incl. 3×18-byte frames padded to 64 —
  NOTE: "TX n len 18" = tagged eth header ONLY, no payload; iob_len
  really is 18; unexplained, investigate later — maybe an upper-layer
  frame we mis-handle, but they do reach the switch).
  New reasoning: on this system the UEFI epoch leaves XMAC out of
  reset AND enabled (CTRL=3), so the 4-port skip means we never
  reset it, and writing CTRL=3 over CTRL=3 gives NO rising edge on
  RX_EN — the MAC may never re-attach its system side to the NIG we
  reconfigured underneath it. Linux gets this edge implicitly:
  prev_unload_close_mac writes CTRL=0 (+20ms) on every load, then
  restores. Fix: bnx2x_xmac_enable now writes CTRL=0 + 20ms delay
  right after the (possibly skipped) reset block, before config;
  final CTRL=3 write provides the rising edge. ALSO added
  bnx2x_lb_test() (bnx2x_hw.c, called at end of eth_open before
  "datapath up"): injects 2×16-byte multicast debug packets straight
  into the NIG loopback LLH via NIG_REG_DEBUG_PACKET_LB 0x10800
  (Linux bnx2x_lb_pckt format: SOP beat {0x55555555,0x55555555,0x20},
  EOP beat {0x09000000,0x55555555,0x10}), then logs "LBTEST
  prs_packets N brb_full N lb_eop_empty X". Reading: LBTEST prs=2 &
  wire-RX still dead ⇒ NIG->BRB->PRS(+maybe storm->host if ifstat
  RX:2) proven good, blockage is MAC->LLH0 hop specifically. LBTEST
  prs=0 ⇒ NIG ingress core broken generally (look at LB vs P0 LLH
  differences, then BRB). If CTRL-cycle fix works: wire RX>0, done —
  proceed to phase 6. Same DEBUG string; ifopen net0, arping/bcast
  ping from VLAN 602 host, ifclose, paste LBTEST + all RXDIAG lines.

- **2026-07-20 (s)**: **PRIME SUSPECT FOUND: latched NIG XOFF —
  fix implemented (PFC_CTRL_HI XON toggle), awaiting hardware
  test.** The guaranteed-traffic test settled it: 119 good frames at
  the MAC (36 uca / 49 bca / 34 mca, grpok 119, no pause/control
  frames) and STILL prs_packets 0 with every gate/enable/FIFO status
  perfect ⇒ frames die at the MAC→NIG boundary before the LLH FIFO.
  Root cause theory: the NIG per-port RX flow-control state is a
  LATCH, and the NIG is never reset (reset_common's 0xd3ffff7f
  deliberately excludes RST_NIG bit 7 to keep the MCP/BMC path
  alive). The vendor UEFI driver epoch (or a BRB soft-reset around
  the live NIG) left port 0 latched XOFF; an XOFF'd port drops all
  ingress silently. Linux clears this on EVERY load in
  bnx2x_prev_unload_close_mac (E3 branch): rising-edge toggle of
  XMAC_REG_PFC_CTRL_HI bit 1 = "Send an indication to change the
  state in the NIG back to XON" (also done in bnx2x_set_xmac_rxtx).
  We only ever wrote the constant 0x2 — no guaranteed edge. Fix in
  bnx2x_xmac_enable: after pause/PFC config, write PFC_CTRL_HI 0x0
  then 0x2 to force the rising edge, before enabling TX/RX. Also
  added "RXDIAG sts" line: P0_RX_MACFIFO_EMPTY (0x18570), NIG
  INT_STS_0/1 (0x103b0/0x103c0), NIG_PRTY_STS_0/1 (0x183bc/0x183cc).
  Side findings from the RMP dump: MFW rules = dest-MAC
  4c:76:25:ba:93:db (management MAC, port MAC minus 1) + UDP ports
  67/68/547 (DHCP/DHCPv6 → the MFW may steal DHCP replies for its
  own management DHCP! watch for this if DHCP still fails once
  ARP/ping RX works) + unknown regs 0x1023c/0x10240 = 0x0601c04a.
  Switch-side test observations: switch received only 6 of 18 iPXE
  TX frames (the untagged IPv6 RS multicasts) — tagged TX (vcreate
  602 DHCP discovers) never counted by the switch: check whether
  VLAN 602 is still a tagged member of the test port (it may have
  been configured on the old port-channel only). Switch also reports
  "PHY XS TX Status: Down" for the port — unexplained; revisit if
  problems remain after the XON fix. Test: same DEBUG string, ifopen
  net0, arping/broadcast-ping from a host on the port's VLAN,
  ifclose; success = ifstat RX>0 / prs_packets>0.

- **2026-07-20 (r)**: **Readback perfect — new working theory: the
  datapath is probably FINE and the test had no deliverable
  traffic.** Hardware readback showed every gate/enable at its
  expected value (drv_mask 3f, mf 3, not_mcp 1, mf_mode 0, cls 0,
  func_en 0, hdrs 6, mac_in/out 1, brb0_out/prs_req_in/prs_eop_out 1,
  drain 0, llh/eop/rmp FIFOs empty, brb_occ 0, xmac_ctrl 3).
  Crucially `rx_gruca 0 rx_grbca 0`: in the whole window NOT ONE
  unicast or broadcast frame reached the MAC — the only ingress was
  13 multicasts, consistent with link-local LLDP/CDP-class frames on
  a quiet trunk port. Those match the MFW's RMP steering rules
  (NIG_REG_LLH0_DEST_MAC_* etc., MCP-owned; not_mcp=1 only routes
  NON-matching frames to the BRB — reg.h: "send to BRB1 if no match
  on any of RMP rules"), so the MCP legitimately eats them: same on
  Linux (bnx2x users never see peer LLDP; MFW consumes it for DCBX).
  ⇒ prs_packets 0 may simply mean "nothing deliverable ever
  arrived". Diag extended (same commit): mstat rx_grxpf/grxcf/grpok
  + raw dump of RMP rules 0x101c0-0x10240 appended to the mstat
  RXDIAG line. **Next hardware test must guarantee deliverable
  traffic**: (a) `vcreate --tag <vlan> net0` + `dhcp net0-<vlan>` on
  the trunk port — the DHCP OFFER itself is the deliverable frame;
  and/or (b) from another host on that VLAN arping/ping-flood the
  iPXE MAC/broadcast so tagged unicast+broadcast definitely arrive.
  Success criteria: ifstat RX>0, or RXDIAG rx_gruca/grbca>0 with
  prs_packets>0 (frames passing NIG) even if something later still
  drops them. If gruca/grbca count but prs stays 0 → real NIG
  problem after all (then dump RMP rules tell us what the MFW
  claims). If DHCP completes: phase 6 (VLAN+LACP) begins.

- **2026-07-20 (q)**: **BREAKTHROUGH: XMAC RX works — frames die
  between MAC and BRB.** Corrected MSTAT diag on hardware shows
  `tx_gtpkt 14 rx_grpkt 7 rx_grmca 7 rx_grfcs 0 xmac_lss 0` with
  `prs_packets 0` and `brb_full_blocks 0`: the MAC receives clean
  multicasts (switch LACP/LLDP), nothing reaches BRB/parser. NIG
  STAT0 counters (incl. brb_discard) are dead on E3 — don't trust
  their zeros. Ruled out by **host-side init-table simulation**
  (`scratchpad/initsim.c`, replays bnx2x_fw.h init ops with our exact
  block/phase sequence + mode flags 0x160d1): the NIG init tables DO
  set every ingress interface enable (BRB0_OUT_EN=1 PRS_REQ_IN_EN=1
  PRS_EOP_OUT_EN=1 XCM0_OUT_EN=1 BRB0_PAUSE_IN_EN=1, LLH0_XCM_MASK=4,
  P0_HDRS_AFTER_BASIC=6 even from the table); PORT4-vs-PORT2 diff
  shows only QM/PBF TX-side differences. Also ruled out by line-level
  audit: update_pfc path is DCBX-only (not run by Linux either in
  plain SF), LLH0_FUNC_EN is MF-only. Remaining suspects: (a) our
  LLH gate writes (DRV_MASK 0x3f / NOT_MCP 1) not sticking or being
  re-programmed by the MFW for BMC/NC-SI sharing (rNDC!), (b) XMAC
  system-side (XLGMII→NIG) stalled so frames count in MSTAT (line
  side) but never exit the MAC. Added full readback diag to
  bnx2x_rx_diag: "RXDIAG gates ..." (drv_mask/mf/not_mcp/mf_mode/
  cls/func_en/hdrs/mac_in/mac_out) and "RXDIAG ifs ..." (brb0_out/
  prs_req_in/prs_eop_out/drain/llh_fifo_empty/eop_empty/rmp_empty/
  brb_occ0/xmac_ctrl). Port-0 addresses only. Expected good values:
  drv_mask 3f, mf 3, not_mcp 1, mf_mode 0, cls 0, func_en 0, hdrs 6,
  mac_in/out 1, brb0_out/prs_req_in/prs_eop_out 1, drain 0,
  xmac_ctrl bits0-1 = 3. Test exactly as before (same DEBUG string;
  ifopen, traffic, ifclose) and paste all five RXDIAG lines.

- **2026-07-20 (p)**: **MSTAT diag offsets corrected before hardware
  test** — the first MSTAT diag commit read the wrong registers. In
  HARDWARE the MSTAT RX counters start at base+0x200
  (`MSTAT_REG_RX_STAT_GR64_LO` in bnx2x_reg.h); the packed
  `struct mstat_stats` (TX 27 pairs = 0xd8 then RX) is only the DMAE
  *destination* layout, not the register map. Corrected reads:
  tx_gtpkt = +0x038 (TX entry 7), rx_grpkt/grfcs/gruca/grmca/grbca =
  +0x250/0x258/0x260/0x268/0x270 (RX entries 10-14),
  XMAC_REG_RX_LSS_STATUS = xmac_base+0x58 (confirmed from
  bnx2x_reg.h). Also re-audited the whole RX enable path against
  Linux line by line: bnx2x_phy_init LFA path = set_rx_filter(1)
  [0x3f/0x3/1 — matches ours] → bnx2x_avoid_link_flap = MSTAT
  reset-toggle (stats-zeroing only; our MSTAT is out of reset via
  common init 0xfffc|MSTAT0|MSTAT1 restore) + bnx2x_xmac_enable
  [matches ours incl. update_pfc values 0x18000/0xffff8000/0x2 and
  set_xumac_nig] + NIG drain 0. init_hw_port NIG section
  (HDRS_AFTER_BASIC=6, LLH_MF_MODE=0, DRV_MASK_MF=0x2 at port-init
  then 0x3 at rx_filter, CLS_TYPE=0, LLFC off, PAUSE_ENABLE=1) and
  BRB1 4-port MAC_GUARANTIED=40 all match Linux. ⇒ No config delta
  found on the enable path; the MSTAT numbers must decide. Test as
  before: full DEBUG string, ifopen net0, wait ~10 s with inbound
  broadcast traffic on the port, ifclose net0, capture the RXDIAG
  lines. Reading: rx_grpkt==0 ⇒ XMAC RX silent (check xmac_lss fault
  bits, then warpcore↔XMAC RX coupling); rx_grpkt>0 & prs 0 ⇒ NIG
  eats frames between MAC and BRB (hunt LLH masks); "usem/xmac rx
  flush" from note (l) does NOT exist in bnx2x_link.c (searched).

- **2026-07-20 (o)**: RX still dead after opening NIG→BRB gates.
  Added `bnx2x_rx_diag()` (bnx2x_hw.c), dumped on every ifclose:
  NIG_REG_STAT0_BRB_DISCARD/TRUNCATE, BRB1_REG_NUM_OF_FULL_BLOCKS,
  PRS_REG_NUM_OF_PACKETS, NIG_REG_STAT0_EGRESS_MAC_PKT0, USTORM rx
  prods readback (IRO[217]) and raw fp_sb index words. Interpretation:
  all-zero ⇒ frames never leave XMAC (look at warpcore/XMAC RX, MSTAT
  counters next); discard>0 ⇒ NIG dropping (gates/masks); prs>0 with
  no CQE ⇒ storm classification/USTORM placement (check prods/qzone,
  client state, TSTORM drop stats). Port-0 counters only.

- **2026-07-20 (n)**: **XMAC TX confirmed on hardware (switch RX
  counters increase) — RX still zero → found the missing NIG→BRB
  ingress gate.** Linux `bnx2x_set_rx_filter(params, 1)` (bnx2x_link.c,
  called on link-up) opens: `NIG_REG_LLH0_BRB1_DRV_MASK`(+port*4) =
  0x3f (per-class ingress enables; RESET DEFAULT 0 = all ingress
  blocked!), `NIG_REG_LLH0_BRB1_DRV_MASK_MF` = 0x3 (tagged+untagged),
  `NIG_REG_LLH0/1_BRB1_NOT_MCP` = 1 (route ingress to host BRB, not
  the management CPU), plus `NIG_REG_EGRESS_DRAIN0_MODE` = 0. All now
  written at the end of bnx2x_xmac_enable(). TX-works/RX-dead with
  clean CQE machinery = check these gates first. NOT yet
  hardware-tested.

- **2026-07-20 (m)**: **Phase 5 XMAC bring-up implemented** (in
  `bnx2x_hw.c` → no DEBUG string change). `bnx2x_xmac_enable()` runs
  at the end of datapath bring-up: hard reset via RESET_REG_2 XMAC
  bit (skipped if 57840+4-port and already out of reset — block is
  shared per path), CORE_PORT_MODE 1/0 + PHY_PORT_MODE 3 (10G),
  XMAC_SOFT reset cycle, NIG egress routed to XMAC
  (NIG_REG_EGRESS_EMAC0_PORT=0), idle-fault detection disabled +
  latched faults cleared (unmanaged warpcore), RX_MAX_SIZE 0x2710,
  TX_CTRL 0xc800 (CRC append), pause/PFC disabled (0x18000/
  0xffff8000/0x2), SA from MAC, EEE off, CTRL=TX_EN|RX_EN, NIG
  P0/P1_MAC_IN_EN=OUT_EN=1 PAUSE_OUT_EN=0. Expect ifopen log line
  "XMAC enabled (port 0, 4-port mode)". **NOT yet hardware-tested.**
  Test: ifopen → TX should now appear on the switch; `dhcp net0`
  (access port) / `vcreate --tag 602 net0` + `dhcp net0-602`; LACP.
  MAC left enabled at close (chip is fully reset on next open; OS
  driver does its own MAC init).

- **2026-07-20 (l)**: **Phase 4b hardware test: all control paths work,
  but no packets reach the wire in either direction ⇒ the MAC (XMAC)
  is not initialised — phase 5 is mandatory, not optional.** Evidence:
  full open sequence clean ("function started/client ready/datapath
  up", all ramrod completions incl. teardown HALT/TERMINATE/CFC_DEL);
  TX completions return (TX:34 TXE:0) so PBF consumes frames, but the
  switch sees ZERO packets from iPXE; switch sends many packets (incl.
  LACP) and iPXE sees RX:0. Both directions die at the MAC boundary.
  Diagnosis: (a) `port_mb.link_status` reflects only the MFW-owned PHY
  link — the E3 XMAC MAC block is configured by the DRIVER in
  bnx2x_link.c on every nic_load (LFA skips PHY init only, NOT MAC
  bring-up); we never ported it. (b) Our bnx2x_reset_common puts XMAC
  into reset (REG_1_CLEAR 0xd3ffff7f + REG_2_CLEAR incl. nothing? and
  REG_2_SET restores only 0xfffc|MSTAT0|MSTAT1 — the XMAC reset bits
  in RESET_REG_2 are ABOVE bit 15 and are never re-set) ⇒ XMAC likely
  held in reset entirely.
  **Next session (phase 5, minimal MAC bring-up), port from Linux
  bnx2x_link.c:**
  - `bnx2x_xmac_init`: RESET_REG_2 CLEAR/SET of
    MISC_REGISTERS_RESET_REG_2_XMAC and _XMAC_SOFT (get bit values from
    bnx2x_reg.h!), XMAC core config incl. **4-port mode handling**
    (57840 rNDC!), xmac_base = GRCBASE_XMAC0/XMAC1 by port.
  - `bnx2x_xmac_enable`: XMAC_REG_CTRL TX_EN|RX_EN,
    XMAC_REG_RX_MAX_SIZE, pause off; skip PFC/stats.
  - `bnx2x_set_xumac_nig`: NIG_REG_P0_MAC_IN_EN/OUT_EN (+port offset)
    and pause-enable gates.
  - Call at end of datapath bring-up (speed from link_status = 10G
    fixed; no PHY touch). Also check bnx2x_link.c for E3 "usem/xmac
    rx flush" or BRB/NIG enable steps adjacent to xmac_enable in
    bnx2x_avoid_link_flap / bnx2x_link_update — mirror whatever the
    LFA path does after a chip reset.
  - Fetch: bnx2x_link.c + bnx2x_link.h into scratchpad reference dir.
  ifstat evidence archived: TX counts rise with TXE=0 while switch RX
  counters stay 0 — remember this signature means "MAC dead".

- **2026-07-20 (k)**: **Phase 4b datapath implemented** (`bnx2x_eth.c`).
  ifopen after function-start now: maps BAR2 doorbells (probe; 4K, cid0
  @ offset 0, db_size 8) → fp SB (0x40) config in CSTORM IRO[136/137/
  141] (index1=RX CQ→SM_RX+HC_EN, index5=TX COS0→SM_TX+HC_EN, SM
  timer 0xff/expire ~0, same_igu_sb_1b, fw_sb_id=igu_base_sb) → CDU
  ctx validation bytes for cid0 (crc8 @ +0x147 X-AG, +0x227 U-AG) →
  rings (1 page each: RX BD 512×8B [510 usable, 2 next-ptrs], CQ
  64×64B [63 usable, 1 next-page], TX 256×16B [255 usable]; all
  self-looping) → post 8 RX bufs (2048B max_bytes, alloc 2176) +
  USTORM prods IRO[217] (cqe_prod|bd_prod<<16, sge 0) → CLIENT_SETUP
  ramrod (client_init data by computed offsets: mtu 1500, fp_hsi_ver
  2, inner-vlan-removal OFF, dont_verify_pause 1, state DROP_ALL
  initial; **completion = RAMROD CQE on own CQ**, type bits0-1==1) →
  CLASSIFICATION_RULES MAC add (RX|TX|IS_ADD hdr 0x13; EQ opcode 15)
  → FILTER_RULES rx-mode (2 rules RX+TX: MCAST_ALL|BCAST_ALL|
  ANY_VLAN, matched-ucast; EQ opcode 16). TX: start_bd {addr, nbd=2,
  len<<16, pkt_prod|0x10<<16|0x01<<24} + zero parse_bd_e2, doorbell
  raw = 0x02|db_prod<<16 (db_prod += nbd). Poll: TX pkt cons = fp SB
  idx5; RX: CQ cons vs idx1 (with 63-skip adjust), fastpath CQE:
  pad=byte3, len=word2>>16, refill returns CQE credit. Close: HALT →
  TERMINATE (CQ completions) → CFC_DEL (EQ op 3) → func stop →
  unload. cl_id=qzone=(pfid>>1)<<2, cid 0, HW_CID port<<23|vn<<17.
  **NOT yet hardware-tested.** Build:
  `DEBUG=bnx2x:3,bnx2x_init:3,bnx2x_hw:3,bnx2x_sp:3,bnx2x_eth:3`
  (bnx2x_eth is NEW in the debug list!). Test: ifopen net0 → expect
  "function started", "client ready", "datapath up"; then `dhcp
  net0` on an access port should complete! Watch: ramrod CQE timeout
  (client setup), MAC/filter EQ timeouts, TX doorbell (does idx5
  advance = first TX completion), RX (background IPv6 RS now real).
  Then vcreate + LACP (phase 6). Known-unverified: FW handling of
  pre-tagged VLAN TX frames (software VLAN), mtu 1500 vs tagged RX.

- **2026-07-20 (j)**: **Phase 4a VALIDATED ON HARDWARE** (after the
  regpair fix): cold boot and warm-boot reruns both clean. The test
  build lacked `bnx2x_sp:3` in DEBUG so the sp lines were invisible,
  but success is implied conclusively: no NMI, ifopen returned
  success (a FUNCTION_START completion timeout would have failed the
  open), net0 went [open] with background TX attempts, ifclose clean.
  ⇒ The full ramrod round trip works: SPE fetch, function_start_data
  fetch, EQ completion DMA, def-SB index DMA, EQ prod update, IGU ack.
  **First confirmed chip→host DMA.** Next hardware test: use the full
  DEBUG string (bnx2x:3,bnx2x_init:3,bnx2x_hw:3,bnx2x_sp:3) to see
  "function started" + EQ event lines explicitly. **Next: phase 4b
  datapath** — needs BAR2 doorbell mapping at probe, fastpath SB,
  ETH_CLIENT_SETUP (client_init_ramrod_data w/ CDU context validation
  for cid 0), CLASSIFICATION_RULES (MAC), FILTER_RULES (rx mode:
  ucast+bcast+all-mcast for LACP), RX BD/CQ rings + USTORM producers,
  TX BD chain + doorbell, poll loop on fp SB indices. Design pack
  below has all layouts.
- **2026-07-20 (i)**: **Phase 4a first hardware test → NMI crash →
  root cause found and fixed.** Symptom: immediately after LOAD_DONE
  on ifopen, host NMI ("A system restart is required", crash IP in
  Metronome.efi = during our mdelay poll loop); Dell Lifecycle log
  shows "PCI parity error ... bus 0 device 5 function 0/2" = the
  Intel IIO root-complex global-error functions. Diagnosis: the SPE's
  data pointer was written hi/lo-transposed (`struct regpair` is
  {lo, hi} in memory — we wrote hi first), so the storm firmware's
  very first upstream DMA (fetching function_start_data) went to
  address ~(lo<<32), i.e. an 8-EB address → master abort/poisoned
  completion → IIO parity error → NMI. The EQ next-page element had
  the same transposition (would have bitten at first EQ wrap).
  Both fixed; all other 64-bit address writes audited (sp_sb host
  addr, EQ base, SPQ page base, IGU attn addr, ILT lines — all
  correct). **Lesson: every `regpair` in HSI structures is lo-first;
  double-check every hi/lo fill against the Linux assignment order.**
  Retest 4a after a COLD POWER CYCLE (the NMI/parity state and a
  possibly-wedged PGLUE was_error want a clean slate; note the
  was-error clear in func init should handle it, but don't confuse
  debugging with residue from the crash).
- **2026-07-20 (h)**: **Phase 4a slowpath infrastructure implemented**
  (`bnx2x_sp.c`/`bnx2x_sp.h`). ifopen now, after LOAD_DONE: allocates
  def SB (0x40)/EQ page/SPQ page/ramrod buffer → configures the def SB
  in CSTORM (IRO[145/146/148]: zeroed+SB_ENABLED data with sp_sb host
  address, igu_dsb_id, IGU_SEG_ACCESS_DEF, pf/vnic, vf_id 0xff; attn
  block id + IGU_REG_ATTN_MSG_ADDR_L/H set) → func_en=1 + vf_to_pf=pfid
  in all four storms (IRO X47/48 C153/154 T107/108 U182/183) → SPQ page
  base+prod=0 via XSEM_REG_FAST_MEMORY+IRO[30/31] → EQ ring (256×16B,
  last elem = self next-page ptr, prod=256, event_ring_data w/ sb_id
  0xde index 7 via IRO[157], prod updates via REG_WR16 IRO[158]) →
  **FUNCTION_START ramrod posted and completion polled** via sp_sb
  index_values[7] + EQ consume + IGU ack(running_index, NOP, update).
  SPE format: word0=le32(cmd<<24 | port<<23|vn<<17|cid), word1=
  le32(conn_type|pfid<<8), word2/3=data hi/lo; prod (u16) to
  BAR_XSTRORM_INTMEM+IRO[31]. function_start_data: SF mode, path_id,
  STATIC_COS, dmae_cmd_id 13, inner_rss 1, sd_vlan_eth_type 0x8100.
  ifclose: FUNCTION_STOP ramrod → free → MCP unload → hw free.
  **NOT yet hardware-tested.** Test with
  `DEBUG=bnx2x:3,bnx2x_init:3,bnx2x_hw:3,bnx2x_sp:3`: ifopen should
  log "function started"; watch for sp completion timeout (would mean
  EQ/def-SB DMA not working — first real DMA test of the storm fw!),
  and EQ event opcode mismatches. ifclose should log nothing new
  (FUNCTION_STOP completes silently at :3 via DBGC2 EQ event line).
  Next: 4b datapath (fp SB, client setup via ETH_CLIENT_SETUP +
  CLASSIFICATION_RULES/FILTER_RULES, RX/CQ + TX rings, BAR2 doorbell).
- **2026-07-20 (g)**: **Phase 3 VALIDATED ON HARDWARE.** Full
  common+port+function init ran to completion on the 57840 rNDC pf0:
  all block init tables executed (op ranges logged and sane), storm
  firmware decompressed+loaded into all four SEMs, no done-poll
  failures (PXP2 CFG/RD_INIT, ATC, CFC LL/AC/CAM all passed silently),
  MCP accepted LOAD_DONE, **link stayed up at 10G throughout**, and
  ifclose/ifopen repeated the whole COMMON_CHIP cycle successfully
  (fw_seq resumed at 0006). IGU CAM scan: dsb 0, base sb 1, cnt 63 —
  63 non-default SBs for pf0 as MFW-provisioned. PXP arbiter: read
  order 3 (MRRS 4096?) write order 1 (MPS 256). Open now takes a few
  seconds (PRAM via single writes) — acceptable. Warm-reboot-to-OS
  check still outstanding. **Next: phase 4 datapath** — status blocks
  (def SB via CSTORM IROs), SPQ + EQ for ramrods, function-start,
  client setup, RX/TX rings + doorbells (needs BAR2 doorbell map).
- **2026-07-20 (f)**: **Phase 3 hardware init sequences ported**
  (`bnx2x_hw.c`/`bnx2x_hw.h`, ~1100 lines): `ifopen` now runs, between
  LOAD_REQ and LOAD_DONE: IGU mode check (+force-normal) + CAM scan →
  common init (reset_common, per-path PF master-disable via pretend,
  PXP arbiter from PCIe MPS/MRRS, endianness, ILT page sizes, E2/E3
  timers workaround [whole-ILT zero+valid under pretend path+6, TM
  full range for vnic3], PXP2/ATC done-polls, all block init-tables
  incl. **SEM storm firmware load via OP_ZP**, QM ptr table + soft
  reset, VFC memory reset, CFC init-done polls, attention masking) →
  port init (per-port blocks, BRB guaranteed for 4-port, PRS/NIG hdr
  config for SF, AEU masks, pause enable) → func init (ILT lines:
  1×32K CDU ctx page + 16×4K QM pages [qm_cid_count=1024], NIC mode,
  IGU function enable single-ISR, per-func blocks, producer memory
  zeroing + SB cleanup commands for all CAM-discovered SBs + DSB).
  Close = MCP unload + memory free (chip left initialised-but-idle;
  next open does full COMMON_CHIP init again after our unload drops
  load count to zero). **Deliberate omissions** (documented in file
  header): FLR cleanup (we always follow a full chip reset — matters
  only if a second port is opened without one, cold boot covers it),
  parity enable, fan-failure PHY-type detection (warns if a board
  requests it), PHY common init (LFA assumed; Linux itself skips it
  when shmem2 has lfa_host_addr, true for bc 7.12.4/7.14.18 — verify).
  Register constants script-extracted verbatim from Linux v6.6
  headers into bnx2x_hw.h. **NOT yet hardware-tested.** Test:
  `ifopen net0` under `DEBUG=bnx2x:3,bnx2x_init:3,bnx2x_hw:3`
  (open takes seconds now: ~250 KB PRAM via single register writes).
  Watch for: PXP2 CFG/RD_INIT/ATC/CFC done-poll failures, MCP
  load-done timeout (would suggest storm fw unhappy), link staying
  up, ifclose clean, warm reboot to OS driver, and open/close/open
  again (second open re-runs COMMON_CHIP — checks re-init after our
  own unload). Next: phase 4 datapath (status blocks, SPQ ramrods,
  client setup, TX/RX rings).
- **2026-07-20 (e)**: Phase-3 infrastructure validated on hardware: probe
  shows `storm firmware 7.13.21.0 (1804 init ops, 5471 dwords data, 387
  IROs)` on all six functions. Init mode flags confirmed: rNDC 57840 =
  `000160d1` (ASIC|PORT4|E3|E3_B0|COS3|SF|LE), 57810 = `000160b1` (PORT2
  variant) — **both chips are E3 B0 silicon**. Lesson recorded: DEBUG
  makeflag must list every driver object (see Build & test).
- **2026-07-20 (d)**: Phase 3 infrastructure: storm firmware
  `bnx2x-e2-7.13.21.0.fw` (sha256 7ee27cf1...) embedded via generator
  `src/drivers/net/bnx2x/bnx2x_mkfw.py` → committed `bnx2x_fw.h` (init_data
  LE-swapped, init_ops decoded to (op<<24|offset, data) u32 pairs,
  init_ops_offsets u16, IRO decoded to structs, SEM int-table/pram kept as
  raw gzip streams). Init-ops interpreter ported (`bnx2x_init.c`): OP_RD/WR/
  SW/WB/ZR/WB_ZR/ZP/WR_64/IF_MODE_AND/IF_MODE_OR, always-!dmae mode (plain
  register writes; valid on E2/E3), gunzip via iPXE deflate (DEFLATE_RAW
  after gzip header skip, 32 KB scratch buffer via malloc_phys), init mode
  flags (ASIC|PORT2/4|E3|E3_A0-or-B0+COS3|SF/MF|LITTLE_ENDIAN). Probe now
  logs fw version + table sizes. **Hardware init sequences NOT yet ported**
  — bnx2x_init_hw_{common,port,func} orchestration (reset_common, block
  ordering, PXP arbiter/ILT, IGU) is the next chunk; until then ifopen still
  does only the MCP handshake. Note: OP_ZP data_len=compressed bytes (high
  u16 of data word), data_off=dword offset into SEM gzip blob (low u16);
  IF_MODE cmd_offset lives in the 24-bit addr field.
- **2026-07-20 (c)**: **Phase 2 validated on real hardware** (log excerpt
  below). Answers to the phase-2 hardware questions:
  - (a) **Link survives the load/unload handshake**: `ifstat` shows
    `Link:up` after repeated `ifopen`/`ifclose` cycles (LFA + no PHY
    touch + UNLOAD_DONE with SKIP_LINK_RESET all behaving).
  - (b) **Cold-boot load level is `common+chip` (0x10130000)** on the
    57840 rNDC, and again on re-open after a full unload (our unload
    drops the MCP load count back to zero, so every open gets
    COMMON_CHIP — phase 3 must therefore always run the full
    common+port+function init sequence).
  - (c) **Both cards are in forced single-function mode** (`feat
    00000100` = FORCE_SF): no NPAR on this system, port MACs are the
    right MACs, no outer-VLAN complications for the datapath. mf_cfg
    base resolves via shmem2 to 003c73b4. (MF MAC path in our code is
    thus unexercised — revisit only if a test system with NPAR shows up.)
  - (d) **Warm reboot into the OS bnx2x driver after our load/unload
    cycle: STILL UNTESTED** — do this check next hardware session.
  - fw_seq resume confirmed working (0000 on cold boot, 0006 on second
    open; MCP echoes each sequence number correctly).
  - Unload-request on a never-loaded function returns UNLOAD_COMMON
    (0x20100000) — recovery handshake is safe to run unconditionally.
  - Note: ~3 TX attempts (ENOTSUP, harmless) appear after each ifopen —
    iPXE background traffic (likely IPv6 router solicitation retries);
    a useful early smoke test for the phase-4 TX path.
- **2026-07-20 (b)**: **Phase 1 validated on real hardware.** Boot of the
  phase-1 build on the target system probed all six functions successfully
  (4× BCM57840 rNDC + 2× BCM57810), correct per-port MACs, all links up.
  Raw log preserved below; key findings:
  - **rNDC 57840 reports chip num `0x168d`** (`CHIP_NUM_57840_OBSOLETE`,
    chip id `168d1010`), *not* 16a1. Any future chip-number checks (E3
    detection for init!) must include 168d/16ab. Defines added to bnx2x.h.
  - 57810 (chip id `168e1000`) presents its two ports as **pf0→path0 and
    pf1→path1, both port 0** with per-path shmem (bases `003c6c80` /
    `003c7640`) — matches Linux `pfid = pf_num & 6`, `path = pf_num & 1`
    logic and yields correct distinct MACs. On these chips the "path" is
    what separates the two network ports; same shmem bases are shared with
    the 57840 functions on the same path.
  - 57840 rNDC: pf0-3 → (path,port) = (0,0),(1,0),(0,1),(1,1), 4-port mode
    detected correctly, MACs ...93:dc/df, ...99:56/59.
  - **`port_mb.link_status` is valid with no driver loaded**: all functions
    show `0x40900275` = link up, speed/duplex code 10 = 10G FD, autoneg
    enabled+complete. MFW-maintained link confirmed ⇒ phase 5 strategy
    (skip bnx2x_link.c, use MFW link) is viable.
  - Bootcode: rNDC bc 7.12.4, 57810 bc 7.14.18 (printed hex in phase-1
    build — now fixed to decimal to match ethtool's "bc X.Y.Z").
  - MF mode questions remain open (this boot predates phase-2 code; the
    phase-2 hardware question list below still stands).
- **2026-07-20**: Phase 2 (MCP handshake) implemented. `bnx2x_fw_command()`
  mailbox with sequence numbers; `ifopen` now performs: fw_seq resume from
  `drv_mb_header` → simplified previous-unload (UNLOAD_REQ_WOL_DIS +
  UNLOAD_DONE w/ SKIP_LINK_RESET — the full `bnx2x_prev_unload` hw cleanup
  [pglue errors, hw locks, UNDI MAC close, FLR] is **not** ported yet) →
  LOAD_REQ WITH_LFA → LOAD_DONE → write DRV_PULSE_ALWAYS_ALIVE. `ifclose`
  does UNLOAD_REQ_WOL_DIS + UNLOAD_DONE(SKIP_LINK_RESET). MF detection at
  probe: mf_cfg base via shmem2 `mf_cfg_addr` (fallback legacy), forced-SF
  mode switch (SI/SD/BD/UFP/forced-SF), per-function MAC from mf_cfg in MF
  modes, `mf_ov` recorded for SD. **Hardware questions for next session:**
  (a) does link_status stay up across LOAD_REQ(WITH_LFA)→LOAD_DONE with no
  PHY init? (b) what load level does MCP return on cold boot (expect
  COMMON_CHIP)? (c) MF mode + MAC on the Dell rNDC 57840? (d) does a
  warm-reboot into the OS bnx2x driver still work after our load/unload
  cycle? Test: `ifopen net0` / `ifclose net0` under `DEBUG=bnx2x:3`.
- **2026-07-19**: Project started. Branch `claude/efi-network-snp-vlan-lacp-m81oui`
  created off `origin/master` (9d6b360). Driver skeleton added under
  `src/drivers/net/bnx2x/`: PCI probe, BAR0 mapping, chip identification,
  PF/port/path discovery, MCP shared-memory (shmem) discovery + validity wait,
  MAC address extraction from shmem `port_hw_config`, link-status readout from
  shmem `port_mb` (validity on real HW TBD — MFW maintains link when no driver
  is loaded, but confirm). No datapath yet: `transmit` fails with ENOTSUP,
  `poll` only refreshes link state. Compiles into `bin-x86_64-efi/ipxe.efi`.
  **Next milestone: verify probe output on real hardware, then start MCP
  LOAD_REQ handshake + storm firmware loading.**

## Hardware background (knowledge base)

- Family: NetXtreme II 5771x/578xx, "Everest" architecture. E1/E1H = 57710/11
  (not targeted), E2 = 57712, **E3 = 57800/57810/57811/57840 (our targets)**.
- The chip's fastpath is implemented by four on-die "storm" RISC processors
  (T/C/U/X-storm) that require **firmware download at driver load**. Without
  storm firmware there is no DMA datapath. This is the single biggest chunk of
  work (see roadmap phase 3).
- A separate **management firmware (MCP/MFW, "bootcode")** runs permanently
  from NVRAM. It owns the PHY/link when no driver is loaded and exposes a
  mailbox + shared memory ("shmem") region inside BAR0. Drivers coordinate
  with it via `DRV_MSG_CODE_LOAD_REQ`/`LOAD_DONE` etc. in `func_mb`.
- BAR0 is an 8 MB register window (we map all of it); BAR1 is the doorbell
  BAR (needed later for TX doorbells).
- 57840 rNDC caveat: Dell ships these in NPAR/multi-function configurations.
  In MF mode the per-function MAC lives in the **mf_cfg** region (via shmem2),
  not `port_hw_config` — skeleton currently reads the port MAC only and logs
  `hw_config`/`mf_cfg` info for diagnosis. Handle MF properly in phase 2.
- 57840 is 4-port: chips have 2 PCIe "paths" × 2 ports. Discovery logic
  (mirrors Linux `bnx2x_get_common_hwinfo`):
  - `pf_num` from PCI config dword 0x98 (ME register), bits 18:16
  - 4-port mode detect: `MISC_REG_PORT4MODE_EN_OVWR` (0xa720) else
    `MISC_REG_PORT4MODE_EN` (0xa750)
  - 4-port: `pfid = pf_num >> 1`; 2-port: `pfid = pf_num & 0x6`
  - `port = pfid & 1`, `path = pf_num & 1`; shmem is per-path.

### Key registers (from Linux v6.6 `bnx2x_reg.h`, GPLv2)

| Register | Address | Purpose |
|---|---|---|
| `MISC_REG_CHIP_NUM` | `0xa408` | chip id bits 16-31 |
| `MISC_REG_CHIP_REV` | `0xa40c` | rev bits 12-15 |
| `MISC_REG_BOND_ID` | `0xa400` | bond id bits 0-3 |
| `MISC_REG_CHIP_TYPE` | `0xac60` | bit 1 ⇒ actually 57811 |
| `MISC_REG_SHARED_MEM_ADDR` | `0xa2b4` | shmem base (BAR0 offset, per path) |
| `MISC_REG_GENERIC_CR_0/1` | `0xa460/0xa464` | shmem2 base (path 0/1) |
| `MISC_REG_PORT4MODE_EN(_OVWR)` | `0xa750/0xa720` | 2 vs 4 port mode |
| `PCICFG_OFFSET + PCI_ID_VAL3` | `0x2000+0x43c` | metal rev (bits 27:24) |
| `MCP_REG_MCPR_NVM_CFG4` | `0x8642c` | flash size |
| cfg-space `PCICFG_GRC_ADDRESS` | `0x78` | write 0 to clean indirect access |
| cfg-space `PCICFG_ME_REGISTER` | `0x98` | absolute PF number (bits 18:16) |

### Shmem layout offsets (computed from Linux v6.6 `bnx2x_hsi.h` with a host
program taking `offsetof(struct shmem_region, ...)` — see "Reference sources")

| Item | Offset from shmem base |
|---|---|
| `validity_map[port]` (u32 × 2) | `0x0000` |
| — `SHR_MEM_VALIDITY_MB` | bit `0x00200000` |
| — `SHR_MEM_VALIDITY_DEV_INFO` | bit `0x00400000` |
| `dev_info.bc_rev` | `0x0008` |
| `dev_info.shared_hw_config.config` | `0x001c` |
| `dev_info.port_hw_config[port].mac_upper` | `0x0044 + port*0x190` |
| `dev_info.port_hw_config[port].mac_lower` | `0x0048 + port*0x190` |
| `port_mb[port]` (`struct drv_port_mb`, link_status first) | `0x0664 + port*0x10` |
| `func_mb[fw_mb_idx]` (`struct drv_func_mb`, size 0x2c) | `0x0684 + idx*0x2c` |
| `dev_info.shared_feature_config.config` | `0x0354` |

`drv_func_mb` members: drv_mb_header +0x00, drv_mb_param +0x04,
fw_mb_header +0x08, fw_mb_param +0x0c, drv_pulse_mb +0x10, mcp_pulse_mb
+0x14, drv_status +0x20. **Mailbox index is NOT the PF number**:
`fw_mb_idx = port + vn * (4port ? 2 : 1)` where `vn = pfid >> 1`
(Linux `BP_FW_MB_IDX`). Mailbox protocol: write param, then
`command | ++seq` (seq 16-bit, resumed from drv_mb_header at load) to
drv_mb_header; poll fw_mb_header until low 16 bits echo seq (≤5 s);
response is high 16 bits. All message codes are in our `bnx2x.h`.

shmem2 layout (from shmem2 base): `size` +0x00, `mf_cfg_addr` +0x10.
mf_cfg layout (base from `mf_cfg_addr`, or legacy shmem+0x0684+8*0x2c):
`func_mf_config[abs_func]` at 0x24 + func*0x18, members config +0x00,
mac_upper +0x04, mac_lower +0x08, e1hov_tag +0x0c.

MAC byte order (Linux `bnx2x_set_mac_buf`): `mac[0]=upper>>8, mac[1]=upper,
mac[2]=lower>>24, mac[3]=lower>>16, mac[4]=lower>>8, mac[5]=lower`.

### Storm firmware strategy (phase 3 — the hard part)

- Linux loads `bnx2x/bnx2x-e2-7.13.21.0.fw` (~250 KB) from linux-firmware for
  both E2 **and E3** chips (57810/57840 use the *e2* blob). The blob is
  redistributable (see linux-firmware `WHENCE`).
- iPXE precedent for embedded firmware: `src/drivers/net/bnx2_fw.h` (bnx2
  driver). Plan: convert the .fw container (header defined in Linux
  `bnx2x_fw_file_hdr.h`) into a C header, or embed the raw .fw and parse the
  container at runtime (sections: init_ops, init_data, TSEM/CSEM/USEM/XSEM
  code, IRO array).
- The init procedure is table-driven: `bnx2x_init_ops.h` interprets init_ops
  opcodes (write/fill/copy blocks) against init_data. Porting that interpreter
  is much smaller than it looks (~1-2 KLOC) and avoids hand-coding thousands
  of register writes.
- IRO (init relative offsets) array from the fw file is needed to compute
  storm RAM offsets for status blocks etc.

### Licensing

- Driver is derived from the Linux `bnx2x` driver (GPLv2 only) ⇒ iPXE files
  are marked `FILE_LICENCE ( GPL2_ONLY )` (same as tg3). Cite provenance in
  file headers.
- Firmware blob: redistributable per linux-firmware WHENCE; keep it in a
  separate file with its own licence note when we get there.

## Phase 4 implementation notes (design pack — extracted 2026-07-20)

All structure sizes/offsets below were computed with the offsetof host
tool against Linux v6.6 `bnx2x_hsi.h`; constants from `bnx2x_hsi.h` /
`bnx2x_fw_defs.h` / `bnx2x.h`. Storm RAM accesses go through BAR0 at
`BAR_{U,C,X,T}STRORM_INTMEM` = `0x400000/0x410000/0x420000/0x430000`
plus IRO-derived offsets (IRO array is already embedded/decoded;
`bnx2x_iro(n)`).

**Memory objects (all page-aligned DMA, alloc at open):**
- Default SB: `struct host_sp_status_block` size 0x38 (atten block
  0x10, then `hc_sp_status_block` sp_sb: index_values[16] u16 @+0x00,
  running_index @+0x20). Poll `index_values[HC_SP_INDEX_EQ_CONS=7]`
  for EQ, `[HC_SP_INDEX_ETH_DEF_CONS=3]` for eth def consumer.
- Fastpath SB: `host_hc_status_block_e2` size 0x40
  (`hc_status_block_e2`: index_values[8] u16 @0x00, running_index
  @0x10). RX CQ cons = index 1 (`HC_INDEX_ETH_RX_CQ_CONS`), TX CQ
  cons COS0 = index 5.
- EQ: 1 page of 256 × `union event_ring_elem` (0x10 each); msg
  opcode field selects `event_ring_opcode` (FUNCTION_START=1,
  FUNCTION_STOP=2, CFC_DEL=3, SET_MAC=14, CLASSIFICATION_RULES=15,
  FILTERS_RULES=16). Last element = next-page pointer (Linux uses
  NUM_EQ_PAGES=1, EQ_DESC_CNT_PAGE=256, usable 255).
- SPQ: 1 page, 256 × `struct eth_spe` (0x10: spe_hdr conn_and_cmd
  @0x00, type @0x04, then data hi/lo regpair).
- Ramrod data buffer (a few hundred bytes, one at a time):
  `function_start_data` 0x30, `client_init_ramrod_data` 0x78
  (general @0x00, rx @0x10, tx @0x60), `eth_classify_rules_ramrod_data`
  0x108 (header 0x8 + rules; mac cmd entry 0x10).

**SB/EQ/SPQ setup (bnx2x_init_def_sb / init_eq_ring / init_sp_ring):**
- SP SB config: write zeroed-then-filled `hc_sp_status_block_data`
  (size 0x10: host_sb_addr lo/hi @0x00, version/pf_id/vnic fields
  @0x0c u32) as u32s to `BAR_CSTRORM_INTMEM +
  CSTORM_SP_STATUS_BLOCK_DATA_OFFSET(pf)` = IRO[146]; SB itself
  zeroed at CSTORM_SP_STATUS_BLOCK_OFFSET(pf) = IRO[145] (+SYNC block
  IRO[148] zeroed too). pf here = pfid.
- Fastpath SB (fw_sb_id = igu_base_sb? For E2 fw sb id == igu sb id):
  `hc_status_block_data_e2` size 0x40 = hc_index_data[8] (2 bytes:
  flags/timeout) @0x00 + `hc_sb_data` common @0x10 (host_sb_addr
  @+0x00, ..., p_func @+0x18, same_igu_sb_1b @+0x1c, state last) →
  written to CSTORM_STATUS_BLOCK_DATA_OFFSET(sb) = IRO[137]; SB
  zeroed at IRO[136], sync block IRO[141].
- EQ: `event_ring_data` (0x10; base_addr @0x00) to BAR_CSTRORM +
  CSTORM_EVENT_RING_DATA_OFFSET(pf)=IRO[157] (note pf>>1/pf&1 m1/m2
  split); producer (=NUM usable, init 1?) to
  CSTORM_EVENT_RING_PROD_OFFSET(pf)=IRO[158]. Linux sets prod to
  NUM_EQ_DESC-1? (check bnx2x_init_eq_ring: bp->eq_prod =
  NUM_EQ_DESC? verify at impl time in bnx2x_main.c).
- SPQ: page base to XSTORM_SPQ_PAGE_BASE_OFFSET(func)=IRO[30] (wr64),
  prod to XSTORM_SPQ_PROD_OFFSET(func)=IRO[31] (u16/u32?). SPE post
  (bnx2x_sp_post): hdr.conn_and_cmd_data = cpu_to_le32((command <<
  SPE_HDR_CMD_ID_SHIFT=24) | HW_CID(cid)); HW_CID(bp,cid) =
  (BP_PORT<<23 | BP_VN<<17 | cid) — verify macro; hdr.type =
  le16((conn_type << SPE_HDR_CONN_TYPE_SHIFT=0) | (function <<
  SPE_HDR_FUNCTION_ID_SHIFT=8)); data regpair hi/lo = ramrod buffer
  phys. After writing SPE, ++prod and REG_WR16 prod to
  XSTORM_SPQ_PROD_OFFSET.
- Ramrod cmd IDs: common (NONE_CONNECTION_TYPE=8): FUNCTION_START=1,
  FUNCTION_STOP=2, CFC_DEL=4. eth (ETH_CONNECTION_TYPE=0):
  CLIENT_SETUP=1, HALT=2, TX_QUEUE_SETUP=4, TERMINATE=7,
  CLASSIFICATION_RULES=9, FILTER_RULES=10, MULTICAST_RULES=11,
  SET_MAC=13.
- Completion: poll sp_sb index_values[7] (EQ cons); EQ element:
  `event_ring_msg` { u8 opcode; ... data }. After consuming, write
  new EQ prod to CSTORM_EVENT_RING_PROD_OFFSET and ack IGU (dsb, op
  IGU_INT_NOP, update 1) — likely optional when polling.

**function_start_data (0x30)**: function_mode=0 (SF), sd_vlan_tag=0,
path_id=path, network_cos_mode=0 (OVERRIDE_COS?)/static — check Linux
bnx2x_func_send_start; gate en / tunnels zero.

**Datapath (4b):**
- RX BD ring: `eth_rx_bd` 8 bytes {addr_lo,addr_hi? actually
  addr hi/lo pair}; page of 512 minus last 2 = next-page pointers.
  CQ: `union eth_rx_cqe` 0x40 (!, E2 = 64 bytes incl. padding? size
  computed 0x40); NUM pages ≥1, last element next-page. RX producers
  (bd_prod, cqe_prod, sge_prod as `ustorm_eth_rx_producers` 0x8)
  written as u32s to BAR_USTRORM + USTORM_RX_PRODS_E2_OFFSET(qzone) =
  IRO[217], qzone = client id for our SF single queue.
- TX: chain of 0x10 BDs: `eth_tx_start_bd` + `eth_tx_parse_bd_e2`
  per packet (nbd=2+frags), last BD of page = next pointer. Doorbell:
  BAR2 (map at probe! `pci_bar_start(pci, PCI_BASE_ADDRESS_2)`),
  offset = cid * (1<<BNX2X_DB_SHIFT=3)?? — Linux BNX2X_DB_SHIFT 3 but
  doorbell offset macro is `BXE`-style `DOORBELL(bp, cid, val)
  REG_WR_RELAXED(bp, bp->doorbells + (bp->db_size * cid), val)` with
  db_size = 1<<7? **verify DOORBELL macro + db_size at impl time**
  (bnx2x.h line ~768: db_size = (1 << BNX2X_DB_SHIFT) where SHIFT=7
  in some trees, 3 in others — read the v6.6 source carefully).
  Doorbell value: `struct doorbell_set_prod` (header + zeros + prod).
- Client setup: fill client_init_ramrod_data: general (client_id,
  statistics off, sb id, sp_client flags), rx (BD/CQE page addrs,
  buffer size, cache line log, status block index HC_INDEX 1,
  approx-mcast off, vmqueue mode off), tx (page addr, sb index 5) →
  SPE ETH_CLIENT_SETUP cid 0. Then classification MAC add via
  CLASSIFICATION_RULES ramrod (classify_rules_ramrod_data: header
  {rule_cnt=1}, mac rule {header {cmd ADD, client, func}, mac hi/mid/
  lo, vlan? }) and FILTER_RULES for rx-mode (accept unicast+broadcast
  +all-multicast? for LACP we need the slow-protocols multicast —
  accept-all-multicast simplest).
- CIDs: leading eth connection cid 0; HW cid via HW_CID macro. The
  CDU context for cid 0 must be filled? Linux writes eth context via
  bnx2x_set_ctx_validation + storm setup in queue setup ramrod using
  cxt (cdu_context page): set validation bytes via
  bnx2x_set_ctx_validation(bp, cxt, cid) — port from bnx2x_cmn.c
  (uses CDU_RSRVD_VALUE macros) — small.

**Linux functions to port for 4a/4b** (bnx2x_main.c/bnx2x_cmn.c
unless noted): bnx2x_init_def_sb, bnx2x_init_eq_ring,
bnx2x_init_sp_ring, bnx2x_sp_post, bnx2x_eq_int (skeleton),
bnx2x_func_send_start (bnx2x_sp.c, just the data fill),
bnx2x_set_ctx_validation (cmn), rx/tx ring init from bnx2x_cmn.c
(bnx2x_init_rx_rings/tx_rings, next-page pointer layout),
bnx2x_update_rx_prod, and the queue-setup/classification data fills
from bnx2x_sp.c (bnx2x_q_fill_init_*, bnx2x_set_one_mac_e2 — port the
*data structure fills* directly, skip the object framework).

## Roadmap

1. **Phase 1 — skeleton (DONE, needs HW validation)**: probe, chip id, shmem,
   MAC, link status readout. Deliverable: `ifstat` shows `net0` with correct
   MAC on real hardware; debug log shows sane chip id / bc_rev / shmem values.
2. **Phase 2 — MCP handshake & MF awareness (CODE DONE, needs HW
   validation)**: implemented as described in the status entry above.
   Deliberately deferred pieces of Linux `bnx2x_prev_unload` (port later if
   hardware testing shows they're needed): `bnx2x_clean_pglue_errors`, hw
   lock/NVRAM-arb release (`MISC_REG_DRIVER_CONTROL_x`, `MCPR_NVM_SW_ARB`),
   MCP access-lock release, UNDI-residue detection + MAC close +
   `bnx2x_prev_unload_common` chip reset, FLR path, path marking.
3. **Phase 3 — hardware init + storm firmware (CODE DONE, needs HW
   validation)**: fw blob embedded + interpreter + full
   common/port/function init sequences ported (see status entries).
   Polled status block setup itself is part of phase 4 (it belongs to
   nic_init/datapath in Linux terms).
4. **Phase 4 — datapath**: one TX queue + one RX queue, client setup via
   ramrods on the slowpath channel (SPQ), leading connection only. Polled RX
   completion queue. This mirrors what other iPXE drivers do with vastly less
   fabric; study FreeBSD `bxe` (simpler than Linux) for the minimal ramrod
   sequence: function start → client setup → set MAC.
5. **Phase 5 — link management**: initially rely on MFW-maintained link
   (query via `port_mb.link_status` / shmem2 `link_status` fields) instead of
   porting the enormous `bnx2x_link.c` PHY library. Only if that proves
   insufficient, port the minimal 578xx-KR/DAC bits.
6. **Phase 6 — the actual goal**: test `vcreate` VLAN + LACP responder on the
   target switch config; then trim code size, review `FILE_SECBOOT`, upstream
   discussion.

## Repo integration points

- Driver lives in `src/drivers/net/bnx2x/` (`bnx2x.c`, `bnx2x.h`).
- Registered in `src/Makefile` via `SRCDIRS += drivers/net/bnx2x`.
- Error-file id `ERRFILE_bnx2x` in `src/include/ipxe/errfile.h`.
- PCI IDs claimed: 14e4:168a/16a5 (57800), 168e/16ae (57810), 163d/163e
  (57811), 16a1/16a2/16a4 (57840). VF ids intentionally not claimed.

## Build & test

```sh
cd src
make -j$(nproc) bin-x86_64-efi/ipxe.efi          # normal build
make -j$(nproc) bin-x86_64-efi/ipxe.efi DEBUG=bnx2x   # driver debug output
```

**DEBUG flags: every driver source file is a separate iPXE debug object and
must be listed explicitly.** The canonical set for hardware testing is
currently:

```
DEBUG=bnx2x:3,bnx2x_init:3,bnx2x_hw:3,bnx2x_sp:3,bnx2x_eth:3
```

(Extend this list whenever a new .c file is added to the driver — and call
the full string out in test instructions every time.) `:3` enables the
extra-verbose `DBGC2` output; plain `DEBUG=bnx2x,bnx2x_init` gives the
one-line-per-event `DBGC` output. Debug output goes to the console (serial
included under EFI).

For interactive hardware testing, embed a script so iPXE drops to the shell
instead of autobooting (and rebooting on failure). An embedded script runs
*instead of* autoboot, and the `goto` loop stops `exit` from falling out of
iPXE back to the firmware:

```sh
printf '#!ipxe\n:sh\nshell\ngoto sh\n' > shell.ipxe
make -j$(nproc) bin-x86_64-efi/ipxe.efi DEBUG=bnx2x:3 EMBED=shell.ipxe
util/genfsimg -o ipxe.iso bin-x86_64-efi/ipxe.efi
```

On hardware (serial console session):
1. Boot the built `ipxe.efi` (via BMC virtual media ISO is fine —
   `util/genfsimg -o ipxe.iso bin-x86_64-efi/ipxe.efi` builds a bootable image).
2. At the iPXE prompt: `ifstat` — expect `net0`/`net1`(+) with the same MACs
   Linux reports for the bnx2x ports.
3. Record the DEBUG=bnx2x probe output (chip id, pf_num, port mode, shmem
   base, bc_rev, hw_config, link_status raw value) into this file's status
   section — several TBDs above depend on those values.

## Reference sources (not committed; re-fetch as needed)

Linux v6.6 `drivers/net/ethernet/broadcom/bnx2x/` — fetch with:

```sh
for f in bnx2x.h bnx2x_hsi.h bnx2x_main.c bnx2x_reg.h bnx2x_fw_defs.h \
         bnx2x_mfw_req.h bnx2x_init.h bnx2x_init_ops.h bnx2x_sp.c bnx2x_sp.h \
         bnx2x_cmn.c bnx2x_fw_file_hdr.h; do
  curl -sfL -O "https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/net/ethernet/broadcom/bnx2x/$f"
done
```

Shmem offsets were computed by compiling a tiny host program against
`bnx2x_hsi.h` (typedef `u8/u16/u32/u64` + `__le16/32/64` to stdint types,
compile with `-D__LITTLE_ENDIAN`) and printing `offsetof(struct shmem_region,
...)` values. Re-verify offsets that way rather than hand-counting.

Other useful references:
- FreeBSD `sys/dev/bxe/` — same hardware, more self-contained than Linux.
- iPXE driver model examples: `src/drivers/net/bnxt/` (Broadcom, DMA rings),
  `src/drivers/net/intel.c` (clean minimal ring driver).
- iPXE porting doc: https://ipxe.org/dev/drivers

## Test scripts (EMBED these for unattended checks)

Build with `EMBED=<script>.ipxe` (an embedded script replaces
autoboot; the final `shell` keeps the console usable afterwards).

Open/close cycle test (checks re-init after our own unload; the
second and later opens re-run the full COMMON_CHIP init):

```
#!ipxe
set n:int32 0
:loop
echo === cycle ${n} ===
ifopen net0 || goto fail
ifstat net0
ifclose net0
inc n
iseq ${n} 10 || goto loop
echo PASSED 10 open/close cycles
shell
:fail
echo FAILED at cycle ${n}
shell
```

All-ports smoke test (probe/open/close every function; expects all
six links up):

```
#!ipxe
set idx:int32 0
:portloop
ifopen net${idx} || goto fail
ifstat net${idx}
ifclose net${idx}
inc idx
iseq ${idx} 6 || goto portloop
echo ALL PORTS PASSED
shell
:fail
echo PORT net${idx} FAILED
shell
```

VLAN DHCP soak test (repeated tagged DHCP on the 57810; adjust
tag/interface to the switch config):

```
#!ipxe
ifopen net4
vcreate --tag 4001 net4
set n:int32 0
:loop
ifconf -c dhcp net4-4001 || goto fail
ifclose net4-4001
inc n
iseq ${n} 5 || goto loop
echo PASSED 5 tagged DHCP cycles
shell
:fail
echo DHCP FAILED at cycle ${n}
shell
```

## Hardware test logs

### 2026-07-20 — phase-1 build, first boot on target system

```
BNX2X 0x70f53470 chip id 168d1010
BNX2X 0x70f53470 pf 0 pfid 0 port 0 path 0 (4-port mode)
BNX2X 0x70f53470 shmem 003c6c80 shmem2 003c3e1c bc 7.c.4 hw_config 00010111
BNX2X 0x70f53470 port 0 MAC 4c:76:25:ba:93:dc
BNX2X 0x70f53470 link up (status 40900275)
BNX2X 0x70f54130 chip id 168d1010
BNX2X 0x70f54130 pf 1 pfid 0 port 0 path 1 (4-port mode)
BNX2X 0x70f54130 shmem 003c7640 shmem2 003c4020 bc 7.c.4 hw_config 00010111
BNX2X 0x70f54130 port 0 MAC 4c:76:25:ba:93:df
BNX2X 0x70f54130 link up (status 40900275)
BNX2X 0x70f54df0 chip id 168d1010
BNX2X 0x70f54df0 pf 2 pfid 1 port 1 path 0 (4-port mode)
BNX2X 0x70f54df0 shmem 003c6c80 shmem2 003c3e1c bc 7.c.4 hw_config 00010111
BNX2X 0x70f54df0 port 1 MAC 4c:76:25:ba:99:56
BNX2X 0x70f54df0 link up (status 40900275)
BNX2X 0x70f582d0 chip id 168d1010
BNX2X 0x70f582d0 pf 3 pfid 1 port 1 path 1 (4-port mode)
BNX2X 0x70f582d0 shmem 003c7640 shmem2 003c4020 bc 7.c.4 hw_config 00010111
BNX2X 0x70f582d0 port 1 MAC 4c:76:25:ba:99:59
BNX2X 0x70f582d0 link up (status 40900275)
BNX2X 0x70f58f30 chip id 168e1000
BNX2X 0x70f58f30 pf 0 pfid 0 port 0 path 0 (2-port mode)
BNX2X 0x70f58f30 shmem 003c6c80 shmem2 003c41dc bc 7.e.12 hw_config 00010101
BNX2X 0x70f58f30 port 0 MAC 4c:76:25:ba:93:e0
BNX2X 0x70f58f30 link up (status 40900275)
BNX2X 0x70f5c410 chip id 168e1000
BNX2X 0x70f5c410 pf 1 pfid 0 port 0 path 1 (2-port mode)
BNX2X 0x70f5c410 shmem 003c7640 shmem2 003c4428 bc 7.e.12 hw_config 00010101
BNX2X 0x70f5c410 port 0 MAC 4c:76:25:ba:93:e3
BNX2X 0x70f5c410 link up (status 40900275)
```

(`bc` values are hex in this build: 7.c.4 = 7.12.4, 7.e.12 = 7.14.18.
`40900275` = link up, 10G FD, AN enabled+complete.)

### 2026-07-20 — phase-2 build, MCP handshake on 57840 rNDC pf0 (net0)

```
iPXE> ifopen net0
BNX2X 0x70f54470 initial fw_seq 0000
BNX2X 0x70f54470 MCP command 20010001 param 00000000
BNX2X 0x70f54470 MCP response 20100001
BNX2X 0x70f54470 MCP unload level 20100000
BNX2X 0x70f54470 MCP command 21000002 param 00000002
BNX2X 0x70f54470 MCP response 21100002
BNX2X 0x70f54470 MCP command 10000003 param 0000100a
BNX2X 0x70f54470 MCP response 10130003
BNX2X 0x70f54470 MCP load level 10130000 (common+chip)
BNX2X 0x70f54470 MCP command 11000004 param 00000000
BNX2X 0x70f54470 MCP response 11100004
iPXE> ifclose net0
BNX2X 0x70f54470 MCP command 20010005 param 00000000
BNX2X 0x70f54470 MCP response 20100005
BNX2X 0x70f54470 MCP unload level 20100000
BNX2X 0x70f54470 MCP command 21000006 param 00000002
BNX2X 0x70f54470 MCP response 21100006
```

(Second open resumed fw_seq at 0006 and again got COMMON_CHIP. Link
stayed up throughout; probe showed forced-SF on all six functions,
mf_cfg base 003c73b4 via shmem2.)

## Session/workflow notes

- Branch: `claude/efi-network-snp-vlan-lacp-m81oui` (do not push elsewhere).
- Keep commits small and buildable; `make bin-x86_64-efi/ipxe.efi` must pass
  before every commit.
- Update the **Current status** section (dated) before ending any session.

### 2026-07-20 — eth_slow:3 LACP responder output (for the record)

Captured during the LACP flap investigation; with serial debug output
enabled the responses were delayed enough that the switch's fast-LACP
(~3s) timeout expired and the bundle flapped. Without eth_slow debug
the bundle is stable. Typical exchange:

```
SLOW net0 RX LACP actor (8000,f8:b1:56:65:b4:d2,0008,8000,025b) [AFGScdlX]
SLOW net0 RX LACP partner (ffff,4c:76:25:ba:93:dc,0001,ff,0001) [aFGsCDlx]
SLOW net0 RX LACP collector 0000 (0 us)
SLOW net0 LACP partner is down
SLOW net0 TX LACP actor (ffff,4c:76:25:ba:93:dc,0001,ff,0001) [aFGSCDlx]
SLOW net0 TX LACP partner (8000,f8:b1:56:65:b4:d2,0008,8000,025b) [AFGScdlX]
SLOW net0 TX LACP collector 0000 (0 us)
```

(Switch actor f8:b1:56:65:b4:d2, our port answering with matching
partner info; len-124 TX frames on the wire are these LACPDUs.)
