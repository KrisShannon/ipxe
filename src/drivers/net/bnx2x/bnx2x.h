#ifndef _BNX2X_H
#define _BNX2X_H

/** @file
 *
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Register addresses, shared-memory offsets and hardware constants in
 * this file are derived from the Linux bnx2x driver (GPLv2),
 * drivers/net/ethernet/broadcom/bnx2x/ as of Linux v6.6:
 * bnx2x_reg.h, bnx2x_hsi.h, bnx2x.h.  Shared-memory offsets were
 * computed as offsetof(struct shmem_region, ...) against the v6.6
 * bnx2x_hsi.h structure definitions; see CLAUDE.md at the repository
 * root for the procedure to re-verify them.
 *
 */

FILE_LICENCE ( GPL2_ONLY );

#include <stdint.h>
#include <ipxe/if_ether.h>

/*
 * Chip numbers (chip_id bits 31:16).  E3 family only; E1x/E2 are not
 * targeted by this driver.
 */
#define BNX2X_CHIP_NUM_57800		0x168a
#define BNX2X_CHIP_NUM_57800_MF		0x16a5
#define BNX2X_CHIP_NUM_57810		0x168e
#define BNX2X_CHIP_NUM_57810_MF		0x16ae
#define BNX2X_CHIP_NUM_57811		0x163d
#define BNX2X_CHIP_NUM_57811_MF		0x163e
#define BNX2X_CHIP_NUM_57840_4_10	0x16a1
#define BNX2X_CHIP_NUM_57840_2_20	0x16a2
#define BNX2X_CHIP_NUM_57840_MF		0x16a4

/** Extract chip number from composed chip id */
#define BNX2X_CHIP_NUM( chip_id )	( (chip_id) >> 16 )

/** BAR0 register window size (8MB on E2/E3) */
#define BNX2X_BAR0_SIZE			0x800000

/*
 * GRC (BAR0) register addresses
 */
#define MISC_REG_BOND_ID		0xa400
#define MISC_REG_CHIP_NUM		0xa408
#define MISC_REG_CHIP_REV		0xa40c
#define MISC_REG_CHIP_TYPE		0xac60
#define MISC_REG_CHIP_TYPE_57811_MASK	0x00000002
#define MISC_REG_SHARED_MEM_ADDR	0xa2b4
#define MISC_REG_GENERIC_CR_0		0xa460
#define MISC_REG_GENERIC_CR_1		0xa464
#define MISC_REG_PORT4MODE_EN		0xa750
#define MISC_REG_PORT4MODE_EN_OVWR	0xa720
#define MCP_REG_MCPR_NVM_CFG4		0x8642c
#define MCPR_NVM_CFG4_FLASH_SIZE	0x00000007

/** PCI configuration space image within GRC space */
#define PCICFG_OFFSET			0x2000
#define PCI_ID_VAL3			0x43c

/*
 * PCI configuration space registers
 */
#define PCICFG_GRC_ADDRESS		0x78
#define PCICFG_VENDOR_ID_OFFSET		0x00
#define PCICFG_ME_REGISTER		0x98
#define ME_REG_ABS_PF_NUM_SHIFT		16
#define ME_REG_ABS_PF_NUM		( 0x7 << ME_REG_ABS_PF_NUM_SHIFT )

/*
 * MCP shared memory ("shmem") layout.  Offsets are relative to the
 * per-path shmem base address read from MISC_REG_SHARED_MEM_ADDR.
 */

/** Validity map (one u32 per port) */
#define BNX2X_SHMEM_VALIDITY( port )	( 0x0000 + ( (port) * 4 ) )
#define SHR_MEM_VALIDITY_PCI_CFG	0x00100000
#define SHR_MEM_VALIDITY_MB		0x00200000
#define SHR_MEM_VALIDITY_DEV_INFO	0x00400000

/** Bootcode revision (dev_info.bc_rev) */
#define BNX2X_SHMEM_BC_REV		0x0008

/** Shared hardware configuration (dev_info.shared_hw_config.config) */
#define BNX2X_SHMEM_HW_CONFIG		0x001c

/** Port hardware configuration stride (sizeof(struct port_hw_cfg)) */
#define BNX2X_SHMEM_PORT_HW_CFG_STRIDE	0x190

/** Port MAC address (dev_info.port_hw_config[port].mac_upper/lower) */
#define BNX2X_SHMEM_MAC_UPPER( port ) \
	( 0x0044 + ( (port) * BNX2X_SHMEM_PORT_HW_CFG_STRIDE ) )
#define BNX2X_SHMEM_MAC_LOWER( port ) \
	( 0x0048 + ( (port) * BNX2X_SHMEM_PORT_HW_CFG_STRIDE ) )

/** Per-port driver/MCP mailbox (struct drv_port_mb, link_status first) */
#define BNX2X_SHMEM_PORT_MB_STRIDE	0x10
#define BNX2X_SHMEM_LINK_STATUS( port ) \
	( 0x0664 + ( (port) * BNX2X_SHMEM_PORT_MB_STRIDE ) )
#define LINK_STATUS_LINK_UP		0x00000001

/** Per-function driver/MCP mailbox (struct drv_func_mb) */
#define BNX2X_SHMEM_FUNC_MB_STRIDE	0x2c
#define BNX2X_SHMEM_FUNC_MB( fw_mb_idx ) \
	( 0x0684 + ( (fw_mb_idx) * BNX2X_SHMEM_FUNC_MB_STRIDE ) )

/* Member offsets within struct drv_func_mb */
#define BNX2X_FUNC_MB_DRV_MB_HEADER	0x00
#define BNX2X_FUNC_MB_DRV_MB_PARAM	0x04
#define BNX2X_FUNC_MB_FW_MB_HEADER	0x08
#define BNX2X_FUNC_MB_FW_MB_PARAM	0x0c
#define BNX2X_FUNC_MB_DRV_PULSE_MB	0x10
#define BNX2X_FUNC_MB_MCP_PULSE_MB	0x14
#define BNX2X_FUNC_MB_DRV_STATUS	0x20

/* Driver-to-MCP mailbox message codes (drv_mb_header) */
#define DRV_MSG_CODE_MASK		0xffff0000
#define DRV_MSG_CODE_LOAD_REQ		0x10000000
#define DRV_MSG_CODE_LOAD_DONE		0x11000000
#define DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS	0x20010000
#define DRV_MSG_CODE_UNLOAD_REQ_WOL_MCP	0x20020000
#define DRV_MSG_CODE_UNLOAD_DONE	0x21000000
#define DRV_MSG_SEQ_NUMBER_MASK		0x0000ffff

/* LOAD_REQ parameter (drv_mb_param) */
#define DRV_MSG_CODE_LOAD_REQ_WITH_LFA	0x0000100a

/* UNLOAD_DONE parameter (drv_mb_param) */
#define DRV_MSG_CODE_UNLOAD_SKIP_LINK_RESET 0x00000002

/* MCP-to-driver mailbox response codes (fw_mb_header) */
#define FW_MSG_CODE_MASK		0xffff0000
#define FW_MSG_CODE_DRV_LOAD_COMMON	0x10100000
#define FW_MSG_CODE_DRV_LOAD_PORT	0x10110000
#define FW_MSG_CODE_DRV_LOAD_FUNCTION	0x10120000
#define FW_MSG_CODE_DRV_LOAD_COMMON_CHIP 0x10130000
#define FW_MSG_CODE_DRV_LOAD_REFUSED	0x10200000
#define FW_MSG_CODE_DRV_LOAD_DONE	0x11100000
#define FW_MSG_CODE_DRV_UNLOAD_COMMON	0x20100000
#define FW_MSG_CODE_DRV_UNLOAD_PORT	0x20110000
#define FW_MSG_CODE_DRV_UNLOAD_FUNCTION	0x20120000
#define FW_MSG_CODE_DRV_UNLOAD_DONE	0x21100000
#define FW_MSG_SEQ_NUMBER_MASK		0x0000ffff

/* Driver pulse (drv_pulse_mb) */
#define DRV_PULSE_SEQ_MASK		0x00007fff
#define DRV_PULSE_ALWAYS_ALIVE		0x00008000

/** Shared feature configuration (dev_info.shared_feature_config.config) */
#define BNX2X_SHMEM_FEAT_CONFIG		0x0354
#define SHARED_FEAT_CFG_FORCE_SF_MODE_MASK		0x00000700
#define SHARED_FEAT_CFG_FORCE_SF_MODE_MF_ALLOWED	0x00000000
#define SHARED_FEAT_CFG_FORCE_SF_MODE_FORCED_SF		0x00000100
#define SHARED_FEAT_CFG_FORCE_SF_MODE_SPIO4		0x00000200
#define SHARED_FEAT_CFG_FORCE_SF_MODE_SWITCH_INDEPT	0x00000300
#define SHARED_FEAT_CFG_FORCE_SF_MODE_AFEX_MODE		0x00000400
#define SHARED_FEAT_CFG_FORCE_SF_MODE_BD_MODE		0x00000500
#define SHARED_FEAT_CFG_FORCE_SF_MODE_UFP_MODE		0x00000600
#define SHARED_FEAT_CFG_FORCE_SF_MODE_EXTENDED_MODE	0x00000700

/*
 * Secondary shared memory (shmem2) layout: offsets from shmem2 base
 */
#define BNX2X_SHMEM2_SIZE		0x0000
#define BNX2X_SHMEM2_MF_CFG_ADDR	0x0010

/*
 * Multi-function configuration (mf_cfg) layout: offsets from mf_cfg
 * base.  Legacy (no shmem2 mf_cfg_addr) location is directly after
 * the E1H_FUNC_MAX func_mb array at the end of shmem.
 */
#define BNX2X_MF_CFG_LEGACY_OFFSET \
	( 0x0684 + ( 8 /* E1H_FUNC_MAX */ * BNX2X_SHMEM_FUNC_MB_STRIDE ) )
#define BNX2X_MF_CFG_FUNC_STRIDE	0x18
#define BNX2X_MF_CFG_FUNC_CONFIG( func ) \
	( 0x0024 + ( (func) * BNX2X_MF_CFG_FUNC_STRIDE ) )
#define BNX2X_MF_CFG_FUNC_MAC_UPPER( func ) \
	( 0x0028 + ( (func) * BNX2X_MF_CFG_FUNC_STRIDE ) )
#define BNX2X_MF_CFG_FUNC_MAC_LOWER( func ) \
	( 0x002c + ( (func) * BNX2X_MF_CFG_FUNC_STRIDE ) )
#define BNX2X_MF_CFG_FUNC_E1HOV_TAG( func ) \
	( 0x0030 + ( (func) * BNX2X_MF_CFG_FUNC_STRIDE ) )
#define FUNC_MF_CFG_UPPERMAC_DEFAULT	0x0000ffff
#define FUNC_MF_CFG_LOWERMAC_DEFAULT	0xffffffff
#define FUNC_MF_CFG_E1HOV_TAG_MASK	0x0000ffff
#define FUNC_MF_CFG_E1HOV_TAG_DEFAULT	0x0000ffff

/** Multi-function modes */
enum bnx2x_mf_mode {
	/** Single function */
	BNX2X_MF_NONE = 0,
	/** Switch-dependent (outer VLAN tagged on the wire) */
	BNX2X_MF_SD,
	/** Switch-independent (NPAR; MAC-based demux, untagged) */
	BNX2X_MF_SI,
	/** Some other mode (AFEX etc.) that we do not support */
	BNX2X_MF_UNSUPPORTED,
};

/** Timeout waiting for MCP shmem validity signature (in 10ms ticks) */
#define BNX2X_SHMEM_TIMEOUT_TICKS	500

/** Timeout waiting for an MCP mailbox response (in 10ms ticks) */
#define BNX2X_MCP_TIMEOUT_TICKS		500

/** A bnx2x network card */
struct bnx2x_nic {
	/** Registers (BAR0) */
	void *regs;
	/** PCI device */
	struct pci_device *pci;
	/** Composed chip id (num:16-31 rev:12-15 metal:4-11 bond:0-3) */
	uint32_t chip_id;
	/** Absolute physical function number (from ME register) */
	unsigned int pf_num;
	/** Physical function id within path */
	unsigned int pfid;
	/** Port number within path (0 or 1) */
	unsigned int port;
	/** PCIe path (0 or 1) */
	unsigned int path;
	/** Chip is in 4-port mode */
	int port4mode;
	/** MCP shared memory base (BAR0 offset), 0 if MCP inactive */
	uint32_t shmem_base;
	/** MCP shared memory 2 base (BAR0 offset) */
	uint32_t shmem2_base;
	/** Bootcode revision */
	uint32_t bc_rev;
	/** MAC address as read from shmem */
	uint8_t hw_addr[ETH_ALEN];
	/** Firmware mailbox index (into shmem func_mb array) */
	unsigned int fw_mb_idx;
	/** Firmware mailbox sequence number */
	uint32_t fw_seq;
	/** Multi-function configuration base (BAR0 offset), 0 if none */
	uint32_t mf_cfg_base;
	/** Multi-function mode */
	enum bnx2x_mf_mode mf_mode;
	/** Outer VLAN tag (switch-dependent MF mode only) */
	unsigned int mf_ov;
	/** MCP load response code (while device is open) */
	uint32_t load_code;
};

#endif /* _BNX2X_H */
