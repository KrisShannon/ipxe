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
