# iPXE bnx2x driver vs Linux kernel bnx2x driver

Status: written 2026-07-21, against Linux v6.6
(`drivers/net/ethernet/broadcom/bnx2x/`) — the reference tree the port
was made from.  Branch-only document (RFC preparation material; not
intended for upstream inclusion as-is).

## Scope and size

| | Linux v6.6 | iPXE |
|---|---|---|
| Hand-written code | ~81,900 lines (.c/.h) | ~5,700 lines |
| Generated firmware data | loaded at runtime via `request_firmware()` | `bnx2x_fw.h`, ~25,500 lines / 1.8MB (to be replaced by a build-time fetch) |
| Chips supported | E1, E1H, E2, E3 (57710–57840, incl. VFs) | E3 A0/B0 only: 57800, 57810, 57811, 57840 (PF only) |
| Tested hardware | (everything QLogic ever shipped) | BCM57810 (2×10G), BCM57840 Dell rNDC (4×10G, chip num 0x168d) |
| Interrupt model | MSI-X/MSI/INTx + NAPI | none — strictly polled |
| Queues | up to 16 RSS queues × 3 CoS, TPA/GRO, SGE pages | 1 RX + 1 TX (CoS 0), single buffer per packet |
| Link management | full PHY library (bnx2x_link.c, 14k lines) | MFW-maintained link (LFA), XMAC bring-up at 10G only |

The iPXE driver is a *boot firmware* driver: bring one port up, run
DHCP/TFTP/HTTP (optionally over software VLAN and with the passive LACP
responder), hand the chip to the OS cleanly.  Everything below is
grouped by *why* the corresponding Linux code was not ported.

## A. Not portable — relies on kernel infrastructure iPXE does not have

These are not "missing"; the concepts do not exist in iPXE's
single-threaded, polled, single-user network stack.

- **Interrupts and NAPI** (large parts of `bnx2x_main.c`/`bnx2x_cmn.c`):
  MSI-X vector management, IGU interrupt enable/ack paths, attention
  (AEU) interrupt processing, slowpath task offload to workqueues.
  iPXE polls: the IGU is forced into normal (non-BC) mode, status
  blocks are read from host memory on every `poll()`, and completions
  are acked with IGU_INT_NOP/no-update.  The default status block's
  *attention* half is configured but attention bits are never serviced
  (Linux uses them for link events, parity errors, MCP events — see
  section B for each).
- **Multi-queue / RSS** : one CPU, no SMP, no flows to steer.  Single
  client (cid 0), client id = IGU SB id.  The RSS engine is left
  unconfigured.
- **Stateless offloads**: TSO/GSO, TX checksum offload, RX checksum
  validation, GRO and the hardware TPA aggregation engine (with its
  SGE page rings).  The iPXE network stack has no offload concept —
  every packet is one contiguous buffer, checksums are computed in
  software.  TX therefore always posts exactly 2 BDs (start + parse)
  and RX always uses single 2KB buffers.
- **Hardware VLAN acceleration**: deliberately unused even though the
  chip supports it.  iPXE's `vcreate` implements VLANs in the network
  stack (tag insertion/stripping in software), which is exactly what
  this port exists to enable; the RX filter is set to ANY_VLAN and
  tagged frames pass through whole.
- **ethtool / debugfs / register dump** (`bnx2x_ethtool.c`,
  `bnx2x_dump.h`, 6k lines): no equivalent facility.  A small host-side
  BAR0 dump tool (`debug/bnx2xdump.c`, branch-only) covers the
  diagnostic need during development.
- **DCB/DCBX** (`bnx2x_dcb.c`): kernel netlink API plus ETS/PFC
  negotiation with the management firmware.  iPXE runs with pause and
  PFC disabled and does not participate; the MFW negotiates DCBX on its
  own regardless (as it does when no driver is loaded).
- **SR-IOV** (`bnx2x_sriov.c`, `bnx2x_vfpf.c`, 5.5k lines): kernel PCI
  VF infrastructure.  VF PCI IDs are deliberately not claimed.
- **PTP/IEEE-1588** clock support: kernel timekeeping infrastructure.
- **cnic / iSCSI / FCoE** storage personalities: the ULP framework,
  extra CIDs and their ramrod flows are kernel-only concepts.
- **Power management**: suspend/resume, runtime PM, WoL configuration
  (we always request UNLOAD_REQ_WOL_DIS — a boot loader has no
  business arming wake sources).

## B. Portable in principle, but wrong trade-off for boot firmware

- **The PHY library** (`bnx2x_link.c`, 14,065 lines — larger than our
  whole driver): warpcore/SerDes microcode interaction, KR/KR2
  autoneg + training, ~20 external PHY chip drivers, SFP module
  detection and power-up, cable detection, LED control, EEE.
  **Strategy instead**: rely on the management firmware's
  link-flap-avoidance (LFA) support — the MFW keeps the PHY trained
  and the link up while no driver is loaded, and we load with
  LOAD_REQ_WITH_LFA and never touch the PHY.  Validated on bc 7.12.4
  and 7.14.18 (both expose shmem2 `lfa_host_addr`).  Link state is
  read from shmem `port_mb.link_status`, which the MFW maintains
  (validated: correct with no driver loaded, and across our
  load/unload cycles).
  **Accepted limitations**: (1) only the XMAC is brought up, at the
  MFW-trained 10G speed — Linux selects the UMAC for ≤1G links and a
  different XMAC config for 20G (57840); a port that trains at 1G
  would carry no traffic under this driver today.  (2) boards/bootcodes
  without LFA (pre-7.x bootcode) would need real PHY init and are not
  supported.  Both are acceptable for the 10G DAC/backplane deployments
  this hardware lives in, and both are cheap to diagnose (`link_status`
  speed field).
- **DMAE engine**: Linux DMA-copies init data, storm RAM writes and
  statistics through the chip's DMAE block.  We use plain register
  writes everywhere (the init interpreter runs in always-!dmae mode,
  valid on E2/E3).  Cost: ~250KB of storm PRAM written dword-by-dword
  makes `ifopen` take a few seconds, once per boot.  Benefit: no DMAE
  command queue/completion machinery, and one less DMA master active
  during early bring-up.
- **The ecore object framework** (most of Linux `bnx2x_sp.c`, 6.5k
  lines): queue/vlan_mac/rx_mode/mcast/RSS *objects* with pending
  command registries, credit pools and per-object state machines.
  That framework exists to serialize concurrent reconfiguration
  requests arriving from interrupts, workqueues and user context.
  iPXE is single-threaded and posts one ramrod at a time, polling for
  its completion — so we ported only the ramrod *data structure fills*
  (client setup, classification rules, filter rules, function
  start/stop), roughly 300 lines instead of 6,500.
- **Multicast filtering** (`MULTICAST_RULES` ramrod, approx-match
  engine): we accept all multicast instead.  Required anyway for the
  LACP responder (01:80:c2:00:00:02) and IPv6 ND, and standard
  practice in iPXE drivers.
- **Statistics machine** (`bnx2x_stats.c`, 2k lines): periodic
  DMAE-collected MAC/storm statistics with a driver state machine.
  iPXE's netdev layer counts packets in software; the chip-side
  counters were only ever needed for bring-up diagnosis, so a one-shot
  STATS_QUERY ramrod dump exists behind `DBG_EXTRA` (compiled out of
  normal builds).
- **Parity/error attention handling and recovery** (`process_kill`,
  leader election, recovery state machine): Linux must recover a live
  chip without losing netdev state, coordinating across all functions.
  The boot-firmware answer is structural: every `ifopen` performs the
  full COMMON_CHIP reset + reinit anyway (our unload always drops the
  MCP load count to zero), and anything unrecoverable is solved by a
  reboot.  We did adopt the recovery-path *reset scope* (Linux
  `reset_common` mask 0xd3ffff7f) as our normal open.
- **Self-test / loopback framework**: temporary equivalents (XMAC
  line-loopback, NIG debug packet injection) were used during RX
  bring-up and then deleted; no production value in iPXE.
- **NVRAM/EEPROM/VPD access**: not needed to boot.  (Also keeps us
  well away from anything that could brick a card.)
- **Fan failure attention** (SPIO5 boards, e.g. some HP blades):
  Linux shuts the card down on fan failure attention.  We print a
  probe-time warning if the board requests the feature and otherwise
  rely on the MFW's own thermal monitoring.  Known, documented gap.
- **EEE**: explicitly disabled in our XMAC bring-up; energy saving is
  meaningless for a boot loader's minutes-long residence.

## C. Ported but deliberately different (behavioral deltas to disclose)

- **Previous-driver unload**: Linux `bnx2x_prev_unload` handles taking
  over from a crashed driver: UNDI-residue detection, MAC close, BRB
  drain, FLR, PGLUE error cleanup, hw-lock release.  We do only the
  MCP UNLOAD_REQ/UNLOAD_DONE recovery handshake at open.  Justified
  because (a) we always arrive from the vendor UEFI driver's orderly
  `Stop()` or from our own unload, and (b) every open runs the full
  common reset+init.  Validated across crashes/UEFI resets during
  development.  The close path adds an **RX quiesce**
  (`bnx2x_xmac_quiesce`: close LLH gates, XON toggle, XMAC CTRL=0
  +20ms, BRB drain) that Linux performs only inside the *next* load's
  prev_unload — iPXE open/close cycles within one boot need it done
  eagerly, since we keep the NIG (and its MFW/BMC configuration)
  un-reset exactly like Linux does.
- **Multi-function (NPAR) modes**: forced-single-function is fully
  supported and hardware-validated.  SD/SI/UFP/BD configs are parsed
  (per-function MAC from mf_cfg, `mf_ov` recorded) but the datapath has
  no outer-tag TX handling (`sd_vlan_tag` is posted as 0 in
  function-start; no OVLAN insertion in the TX BD), so SD mode must be
  considered unsupported until tested on NPAR hardware.
- **16-bit index conventions**: Linux keeps status-block-derived
  consumer indices in `u16` variables, making wraparound implicit.
  iPXE uses `unsigned int` counters with explicit `& 0xffff` masking
  and an explicitly wrapped next-page bump — two hardware-validated
  crash/hang classes (TX doorbell next-page counting, 0xffff consumer
  bump) trace to this difference and are now documented in the code.
- **Ramrod completion handling**: ETH ramrod completions arrive on the
  RX CQ; Linux consumes them from NAPI context interleaved with
  packets.  Our synchronous `wait_ramrod_cqe` must instead discard any
  packet CQEs queued ahead of the ramrod CQE (with a runaway cap), a
  situation Linux structurally cannot have.
- **Teardown scope**: `ifclose` fully tears down (HALT → TERMINATE →
  CFC_DEL → FUNCTION_STOP → MCP unload); Linux keeps the function
  loaded between `ifdown`/`ifup`.  Consequence: every iPXE open pays
  the full ~seconds-long chip init, and the MCP load count is zero
  whenever iPXE isn't actively using the port (friendlier to the OS
  handoff, which is hardware-validated including same-boot kexec-style
  handoff to the Linux driver).
- **Ring geometry**: RX 120 buffers / 2-page CQ vs Linux's ~500-buffer
  BD ring and larger CQ; TX 255-usable-BD single page vs Linux's
  multi-page rings.  Sized for iPXE's 4MB heap while staying above the
  storm firmware's minimum-free-buffer placement threshold (empirically
  >8; 48 sufficed, 120 gives TCP-burst headroom; sustained 110MB/s
  hardware-validated).
- **MTU**: fixed 1500 (+VLAN); no jumbo support plumbed (RX buffer
  size would allow ~1998 without layout changes).
- **Chip coverage**: E1/E1H (57710/57711) are a different HSI/init
  world (separate firmware blob, func_cfg init path, EMAC/BMAC MACs)
  and are not claimed.  E2 (57712) shares our firmware and most init
  paths but is unclaimed/untested — could be enabled later with
  PORT2-mode flags and a hardware test.  IGU bootcode-compatibility
  (BC) mode is not supported (we force normal mode, matching all
  modern bootcodes).

## D. iPXE-side additions with no Linux counterpart

- Embedded storm firmware as a generated C header
  (`bnx2x_mkfw.py` → `bnx2x_fw.h`) with the init-ops interpreter
  running from decoded tables; planned move to iPXE's standard
  build-time firmware fetch.
- TX-stall tripwire: one-shot state dump + idempotent doorbell re-ring
  if the TX ring stays full >1s (level-1 debug, compiled cheap).
- Probe-time chip identification accepts the Dell rNDC's
  pre-production chip number 0x168d (`CHIP_NUM_57840_OBSOLETE`) that
  Linux v6.6 also quirks.

## Bottom line

Roughly 93% of the Linux driver's line count serves capabilities that
are either impossible to express in iPXE (interrupts, offloads,
multi-queue, kernel APIs) or inappropriate for firmware whose job is a
few minutes of single-flow networking (PHY library vs MFW-owned link,
recovery machinery vs "reboot", object frameworks vs one ramrod at a
time).  The ported core — MCP handshake, table-driven hardware init
with embedded storm firmware, one polled queue pair, and a clean
teardown the OS driver demonstrably tolerates — is the complete
intersection of "what the chip requires" and "what iPXE can use".
