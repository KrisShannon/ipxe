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
#define BNX2X_SHMEM_FUNC_MB( func ) \
	( 0x0684 + ( (func) * BNX2X_SHMEM_FUNC_MB_STRIDE ) )

/** Timeout waiting for MCP shmem validity signature (in 10ms ticks) */
#define BNX2X_SHMEM_TIMEOUT_TICKS	500

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
};

#endif /* _BNX2X_H */
