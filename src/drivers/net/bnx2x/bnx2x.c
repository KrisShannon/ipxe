/*
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Derived from the Linux bnx2x driver,
 * drivers/net/ethernet/broadcom/bnx2x/ (Linux v6.6):
 *
 *   Copyright (c) 2007-2013 Broadcom Corporation
 *   Copyright (c) 2014 QLogic Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

FILE_LICENCE ( GPL2_ONLY );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ipxe/pci.h>
#include <ipxe/io.h>
#include <ipxe/timer.h>
#include <ipxe/iobuf.h>
#include <ipxe/if_ether.h>
#include <ipxe/ethernet.h>
#include <ipxe/netdevice.h>
#include "bnx2x.h"

/** @file
 *
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Currently a probe-only skeleton: identifies the chip, locates the
 * MCP shared memory region, extracts the port MAC address and reports
 * MCP-maintained link state.  No datapath yet.
 *
 */

/**
 * Read GRC register
 *
 * @v bnx2x		bnx2x device
 * @v offset		Register offset within BAR0
 * @ret value		Register value
 */
static uint32_t bnx2x_readl ( struct bnx2x_nic *bnx2x, uint32_t offset ) {

	return readl ( bnx2x->regs + offset );
}

/**
 * Read MCP shared memory location
 *
 * @v bnx2x		bnx2x device
 * @v offset		Offset within shared memory
 * @ret value		Value
 */
static uint32_t bnx2x_shmem_readl ( struct bnx2x_nic *bnx2x,
				    uint32_t offset ) {

	return bnx2x_readl ( bnx2x, ( bnx2x->shmem_base + offset ) );
}

/**
 * Identify chip
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_identify ( struct bnx2x_nic *bnx2x ) {
	uint32_t val;
	uint32_t id;

	/* Check that the register window is responding */
	val = bnx2x_readl ( bnx2x, MISC_REG_CHIP_NUM );
	if ( val == 0xffffffff ) {
		DBGC ( bnx2x, "BNX2X %p register window not responding\n",
		       bnx2x );
		return -EIO;
	}

	/* Compose chip id: num:16-31, rev:12-15, metal:4-11, bond:0-3 */
	id = ( ( val & 0xffff ) << 16 );
	val = bnx2x_readl ( bnx2x, MISC_REG_CHIP_REV );
	id |= ( ( val & 0xf ) << 12 );
	val = bnx2x_readl ( bnx2x, ( PCICFG_OFFSET + PCI_ID_VAL3 ) );
	id |= ( ( ( val >> 24 ) & 0xf ) << 4 );
	val = bnx2x_readl ( bnx2x, MISC_REG_BOND_ID );
	id |= ( val & 0xf );

	/* BCM57811 reports the 57810 chip number; fix up if needed */
	val = bnx2x_readl ( bnx2x, MISC_REG_CHIP_TYPE );
	if ( val & MISC_REG_CHIP_TYPE_57811_MASK ) {
		if ( BNX2X_CHIP_NUM ( id ) == BNX2X_CHIP_NUM_57810 ) {
			id = ( ( BNX2X_CHIP_NUM_57811 << 16 ) |
			       ( id & 0x0000ffff ) | 0x1 );
		} else if ( BNX2X_CHIP_NUM ( id ) ==
			    BNX2X_CHIP_NUM_57810_MF ) {
			id = ( ( BNX2X_CHIP_NUM_57811_MF << 16 ) |
			       ( id & 0x0000ffff ) | 0x1 );
		}
	}
	bnx2x->chip_id = id;
	DBGC ( bnx2x, "BNX2X %p chip id %08x\n", bnx2x, bnx2x->chip_id );

	return 0;
}

/**
 * Determine physical function, port and path
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_identify_function ( struct bnx2x_nic *bnx2x ) {
	uint32_t me;
	uint32_t val;

	/* Read absolute function number from the ME register.  The
	 * PCI bus/device/function may be arbitrary (e.g. behind
	 * virtualisation), so the chip provides its own view.
	 */
	pci_read_config_dword ( bnx2x->pci, PCICFG_ME_REGISTER, &me );
	bnx2x->pf_num = ( ( me & ME_REG_ABS_PF_NUM ) >>
			  ME_REG_ABS_PF_NUM_SHIFT );

	/* Determine 2-port vs 4-port mode */
	val = bnx2x_readl ( bnx2x, MISC_REG_PORT4MODE_EN_OVWR );
	if ( ( val & 1 ) == 0 ) {
		val = bnx2x_readl ( bnx2x, MISC_REG_PORT4MODE_EN );
	} else {
		val = ( ( val >> 1 ) & 1 );
	}
	bnx2x->port4mode = ( val & 1 );

	/* Derive function id within path, port and path */
	if ( bnx2x->port4mode ) {
		bnx2x->pfid = ( bnx2x->pf_num >> 1 );
	} else {
		bnx2x->pfid = ( bnx2x->pf_num & 0x6 );
	}
	bnx2x->port = ( bnx2x->pfid & 1 );
	bnx2x->path = ( bnx2x->pf_num & 1 );
	DBGC ( bnx2x, "BNX2X %p pf %d pfid %d port %d path %d (%d-port "
	       "mode)\n", bnx2x, bnx2x->pf_num, bnx2x->pfid, bnx2x->port,
	       bnx2x->path, ( bnx2x->port4mode ? 4 : 2 ) );

	return 0;
}

/**
 * Locate MCP shared memory
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_init_shmem ( struct bnx2x_nic *bnx2x ) {
	uint32_t validity;
	unsigned int i;

	/* Wait for the management firmware to publish a valid shared
	 * memory region.
	 */
	for ( i = 0 ; i < BNX2X_SHMEM_TIMEOUT_TICKS ; i++ ) {
		bnx2x->shmem_base =
			bnx2x_readl ( bnx2x, MISC_REG_SHARED_MEM_ADDR );
		if ( bnx2x->shmem_base ) {
			validity = bnx2x_shmem_readl ( bnx2x,
				BNX2X_SHMEM_VALIDITY ( bnx2x->port ) );
			if ( ( validity & ( SHR_MEM_VALIDITY_MB |
					    SHR_MEM_VALIDITY_DEV_INFO ) ) ==
			     ( SHR_MEM_VALIDITY_MB |
			       SHR_MEM_VALIDITY_DEV_INFO ) ) {
				break;
			}
		}
		mdelay ( 10 );
	}
	if ( i == BNX2X_SHMEM_TIMEOUT_TICKS ) {
		DBGC ( bnx2x, "BNX2X %p MCP shared memory not valid "
		       "(base %08x)\n", bnx2x, bnx2x->shmem_base );
		return -ETIMEDOUT;
	}

	/* Locate secondary shared memory region (per path) */
	bnx2x->shmem2_base =
		bnx2x_readl ( bnx2x, ( bnx2x->path ? MISC_REG_GENERIC_CR_1 :
				       MISC_REG_GENERIC_CR_0 ) );

	/* Record bootcode version */
	bnx2x->bc_rev = ( bnx2x_shmem_readl ( bnx2x,
					      BNX2X_SHMEM_BC_REV ) >> 8 );

	DBGC ( bnx2x, "BNX2X %p shmem %08x shmem2 %08x bc %x.%x.%x "
	       "hw_config %08x\n", bnx2x, bnx2x->shmem_base,
	       bnx2x->shmem2_base, ( ( bnx2x->bc_rev >> 16 ) & 0xff ),
	       ( ( bnx2x->bc_rev >> 8 ) & 0xff ), ( bnx2x->bc_rev & 0xff ),
	       bnx2x_shmem_readl ( bnx2x, BNX2X_SHMEM_HW_CONFIG ) );

	return 0;
}

/**
 * Fetch MAC address from MCP shared memory
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_fetch_mac ( struct bnx2x_nic *bnx2x ) {
	uint32_t upper;
	uint32_t lower;

	/* Read port MAC address.  Note that in multi-function (NPAR)
	 * configurations the per-function MAC lives in the mf_cfg
	 * region instead; handling that is a TODO (see CLAUDE.md).
	 */
	upper = bnx2x_shmem_readl ( bnx2x,
				    BNX2X_SHMEM_MAC_UPPER ( bnx2x->port ) );
	lower = bnx2x_shmem_readl ( bnx2x,
				    BNX2X_SHMEM_MAC_LOWER ( bnx2x->port ) );
	bnx2x->hw_addr[0] = ( upper >> 8 );
	bnx2x->hw_addr[1] = ( upper >> 0 );
	bnx2x->hw_addr[2] = ( lower >> 24 );
	bnx2x->hw_addr[3] = ( lower >> 16 );
	bnx2x->hw_addr[4] = ( lower >> 8 );
	bnx2x->hw_addr[5] = ( lower >> 0 );
	DBGC ( bnx2x, "BNX2X %p port %d MAC %s\n", bnx2x, bnx2x->port,
	       eth_ntoa ( bnx2x->hw_addr ) );

	return 0;
}

/**
 * Check link state
 *
 * @v netdev		Network device
 */
static void bnx2x_check_link ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;
	uint32_t link_status;

	/* Read MCP-maintained link status.  Validity of this field
	 * when no full driver has ever been loaded is still to be
	 * confirmed on real hardware (see CLAUDE.md).
	 */
	link_status = bnx2x_shmem_readl ( bnx2x,
			BNX2X_SHMEM_LINK_STATUS ( bnx2x->port ) );
	if ( link_status & LINK_STATUS_LINK_UP ) {
		if ( ! netdev_link_ok ( netdev ) ) {
			DBGC ( bnx2x, "BNX2X %p link up (status %08x)\n",
			       bnx2x, link_status );
			netdev_link_up ( netdev );
		}
	} else {
		if ( netdev_link_ok ( netdev ) ) {
			DBGC ( bnx2x, "BNX2X %p link down (status %08x)\n",
			       bnx2x, link_status );
			netdev_link_down ( netdev );
		}
	}
}

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int bnx2x_open ( struct net_device *netdev ) {

	/* No datapath yet; just refresh link state */
	bnx2x_check_link ( netdev );

	return 0;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void bnx2x_close ( struct net_device *netdev __unused ) {

	/* Nothing to do yet */
}

/**
 * Transmit packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
static int bnx2x_transmit ( struct net_device *netdev,
			    struct io_buffer *iobuf __unused ) {
	struct bnx2x_nic *bnx2x = netdev->priv;

	/* Datapath not yet implemented */
	DBGC2 ( bnx2x, "BNX2X %p transmit not yet implemented\n", bnx2x );
	return -ENOTSUP;
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
static void bnx2x_poll ( struct net_device *netdev ) {

	/* Datapath not yet implemented; just track link state */
	bnx2x_check_link ( netdev );
}

/** bnx2x network device operations */
static struct net_device_operations bnx2x_operations = {
	.open		= bnx2x_open,
	.close		= bnx2x_close,
	.transmit	= bnx2x_transmit,
	.poll		= bnx2x_poll,
};

/**
 * Probe PCI device
 *
 * @v pci		PCI device
 * @ret rc		Return status code
 */
static int bnx2x_probe ( struct pci_device *pci ) {
	struct net_device *netdev;
	struct bnx2x_nic *bnx2x;
	int rc;

	/* Allocate and initialise net device */
	netdev = alloc_etherdev ( sizeof ( *bnx2x ) );
	if ( ! netdev ) {
		rc = -ENOMEM;
		goto err_alloc;
	}
	netdev_init ( netdev, &bnx2x_operations );
	bnx2x = netdev->priv;
	pci_set_drvdata ( pci, netdev );
	netdev->dev = &pci->dev;
	memset ( bnx2x, 0, sizeof ( *bnx2x ) );
	bnx2x->pci = pci;

	/* Fix up PCI device */
	adjust_pci_device ( pci );

	/* Map registers */
	bnx2x->regs = pci_ioremap ( pci, pci->membase, BNX2X_BAR0_SIZE );
	if ( ! bnx2x->regs ) {
		rc = -ENODEV;
		goto err_ioremap;
	}

	/* Clean indirect addresses (as done by the Linux driver) */
	pci_write_config_dword ( pci, PCICFG_GRC_ADDRESS,
				 PCICFG_VENDOR_ID_OFFSET );

	/* Identify chip */
	if ( ( rc = bnx2x_identify ( bnx2x ) ) != 0 )
		goto err_identify;

	/* Determine function, port and path */
	if ( ( rc = bnx2x_identify_function ( bnx2x ) ) != 0 )
		goto err_identify;

	/* Locate MCP shared memory */
	if ( ( rc = bnx2x_init_shmem ( bnx2x ) ) != 0 )
		goto err_shmem;

	/* Fetch MAC address */
	if ( ( rc = bnx2x_fetch_mac ( bnx2x ) ) != 0 )
		goto err_mac;
	memcpy ( netdev->hw_addr, bnx2x->hw_addr, ETH_ALEN );

	/* Register network device */
	if ( ( rc = register_netdev ( netdev ) ) != 0 )
		goto err_register;

	/* Set initial link state */
	bnx2x_check_link ( netdev );

	return 0;

 err_register:
 err_mac:
 err_shmem:
 err_identify:
	iounmap ( bnx2x->regs );
 err_ioremap:
	netdev_nullify ( netdev );
	netdev_put ( netdev );
 err_alloc:
	return rc;
}

/**
 * Remove PCI device
 *
 * @v pci		PCI device
 */
static void bnx2x_remove ( struct pci_device *pci ) {
	struct net_device *netdev = pci_get_drvdata ( pci );
	struct bnx2x_nic *bnx2x = netdev->priv;

	/* Unregister network device */
	unregister_netdev ( netdev );

	/* Unmap registers */
	iounmap ( bnx2x->regs );

	/* Free network device */
	netdev_nullify ( netdev );
	netdev_put ( netdev );
}

/** bnx2x PCI device IDs */
static struct pci_device_id bnx2x_nics[] = {
	PCI_ROM ( 0x14e4, 0x168a, "bnx2x-57800", "Broadcom BCM57800", 0 ),
	PCI_ROM ( 0x14e4, 0x16a5, "bnx2x-57800-mf", "Broadcom BCM57800 MF",
		  0 ),
	PCI_ROM ( 0x14e4, 0x168e, "bnx2x-57810", "Broadcom BCM57810", 0 ),
	PCI_ROM ( 0x14e4, 0x16ae, "bnx2x-57810-mf", "Broadcom BCM57810 MF",
		  0 ),
	PCI_ROM ( 0x14e4, 0x163d, "bnx2x-57811", "Broadcom BCM57811", 0 ),
	PCI_ROM ( 0x14e4, 0x163e, "bnx2x-57811-mf", "Broadcom BCM57811 MF",
		  0 ),
	PCI_ROM ( 0x14e4, 0x16a1, "bnx2x-57840", "Broadcom BCM57840 4x10G",
		  0 ),
	PCI_ROM ( 0x14e4, 0x16a2, "bnx2x-57840-2-20",
		  "Broadcom BCM57840 2x20G", 0 ),
	PCI_ROM ( 0x14e4, 0x16a4, "bnx2x-57840-mf", "Broadcom BCM57840 MF",
		  0 ),
};

/** bnx2x PCI driver */
struct pci_driver bnx2x_driver __pci_driver = {
	.ids = bnx2x_nics,
	.id_count = ( sizeof ( bnx2x_nics ) / sizeof ( bnx2x_nics[0] ) ),
	.probe = bnx2x_probe,
	.remove = bnx2x_remove,
};
