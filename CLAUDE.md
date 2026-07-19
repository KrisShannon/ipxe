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
| `func_mb[func]` (`struct drv_func_mb`, size 0x2c) | `0x0684 + func*0x2c` |

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

## Roadmap

1. **Phase 1 — skeleton (DONE, needs HW validation)**: probe, chip id, shmem,
   MAC, link status readout. Deliverable: `ifstat` shows `net0` with correct
   MAC on real hardware; debug log shows sane chip id / bc_rev / shmem values.
2. **Phase 2 — MCP handshake & MF awareness**: `DRV_MSG_CODE_LOAD_REQ` /
   `LOAD_DONE` sequence via `func_mb` (mailbox protocol incl. sequence numbers
   in `drv_mb_header`), previous-unload recovery (`bnx2x_prev_unload` logic),
   MF mode detect + per-function MAC from mf_cfg, proper `UNLOAD_REQ` on
   remove. Without a clean unload the MFW can leave the function in a bad
   state for the OS driver — be careful, test with warm reboots.
3. **Phase 3 — hardware init + storm firmware**: embed/parse fw blob, port the
   init-ops interpreter, common/port/function init sequences (Linux
   `bnx2x_init_hw_{common,port,func}`), PXP arbiter/ILT setup for a minimal
   single-queue configuration, interrupt-free (polled) status block.
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

Debug output goes to the console (serial included under EFI). `DEBUG=bnx2x:3`
for extra-verbose (`DBGC2`) output.

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

## Session/workflow notes

- Branch: `claude/efi-network-snp-vlan-lacp-m81oui` (do not push elsewhere).
- Keep commits small and buildable; `make bin-x86_64-efi/ipxe.efi` must pass
  before every commit.
- Update the **Current status** section (dated) before ending any session.
