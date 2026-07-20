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
#include "bnx2x_init.h"
#include "bnx2x_hw.h"

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
 * Read multi-function configuration location
 *
 * @v bnx2x		bnx2x device
 * @v offset		Offset within mf_cfg region
 * @ret value		Value
 */
static uint32_t bnx2x_mf_cfg_readl ( struct bnx2x_nic *bnx2x,
				     uint32_t offset ) {

	return bnx2x_readl ( bnx2x, ( bnx2x->mf_cfg_base + offset ) );
}

/**
 * Issue MCP mailbox command and await response
 *
 * @v bnx2x		bnx2x device
 * @v command		Command code (DRV_MSG_CODE_xxx)
 * @v param		Command parameter
 * @ret response	Response code (FW_MSG_CODE_xxx), or 0 on timeout
 */
static uint32_t bnx2x_fw_command ( struct bnx2x_nic *bnx2x, uint32_t command,
				   uint32_t param ) {
	uint32_t func_mb = BNX2X_SHMEM_FUNC_MB ( bnx2x->fw_mb_idx );
	uint32_t seq;
	uint32_t response = 0;
	unsigned int i;

	/* Compose and post command with next sequence number */
	bnx2x->fw_seq = ( ( bnx2x->fw_seq + 1 ) & DRV_MSG_SEQ_NUMBER_MASK );
	seq = bnx2x->fw_seq;
	bnx2x_shmem_writel ( bnx2x, param,
			     ( func_mb + BNX2X_FUNC_MB_DRV_MB_PARAM ) );
	bnx2x_shmem_writel ( bnx2x, ( command | seq ),
			     ( func_mb + BNX2X_FUNC_MB_DRV_MB_HEADER ) );
	DBGC2 ( bnx2x, "BNX2X %p MCP command %08x param %08x\n",
		bnx2x, ( command | seq ), param );

	/* Wait for firmware to echo our sequence number */
	for ( i = 0 ; i < BNX2X_MCP_TIMEOUT_TICKS ; i++ ) {
		mdelay ( 10 );
		response = bnx2x_shmem_readl ( bnx2x,
				( func_mb + BNX2X_FUNC_MB_FW_MB_HEADER ) );
		if ( ( response & FW_MSG_SEQ_NUMBER_MASK ) == seq ) {
			DBGC2 ( bnx2x, "BNX2X %p MCP response %08x\n",
				bnx2x, response );
			return ( response & FW_MSG_CODE_MASK );
		}
	}

	DBGC ( bnx2x, "BNX2X %p MCP command %08x timed out (last response "
	       "%08x)\n", bnx2x, ( command | seq ), response );
	return 0;
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

	/* Firmware mailbox index: port + vn * (ports per path) */
	bnx2x->fw_mb_idx = ( bnx2x->port + ( ( bnx2x->pfid >> 1 ) *
					     ( bnx2x->port4mode ? 2 : 1 ) ) );
	DBGC ( bnx2x, "BNX2X %p pf %d pfid %d port %d path %d fw_mb %d "
	       "(%d-port mode)\n", bnx2x, bnx2x->pf_num, bnx2x->pfid,
	       bnx2x->port, bnx2x->path, bnx2x->fw_mb_idx,
	       ( bnx2x->port4mode ? 4 : 2 ) );

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

	DBGC ( bnx2x, "BNX2X %p shmem %08x shmem2 %08x bc %d.%d.%d "
	       "hw_config %08x\n", bnx2x, bnx2x->shmem_base,
	       bnx2x->shmem2_base, ( ( bnx2x->bc_rev >> 16 ) & 0xff ),
	       ( ( bnx2x->bc_rev >> 8 ) & 0xff ), ( bnx2x->bc_rev & 0xff ),
	       bnx2x_shmem_readl ( bnx2x, BNX2X_SHMEM_HW_CONFIG ) );

	return 0;
}

/**
 * Detect multi-function mode
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_detect_mf ( struct bnx2x_nic *bnx2x ) {
	unsigned int func = bnx2x->pf_num;
	uint32_t shmem2_size;
	uint32_t feat;
	uint32_t val;

	/* Locate multi-function configuration region */
	bnx2x->mf_cfg_base = ( bnx2x->shmem_base +
			       BNX2X_MF_CFG_LEGACY_OFFSET );
	if ( bnx2x->shmem2_base ) {
		shmem2_size = bnx2x_readl ( bnx2x, ( bnx2x->shmem2_base +
						     BNX2X_SHMEM2_SIZE ) );
		if ( shmem2_size > BNX2X_SHMEM2_MF_CFG_ADDR ) {
			bnx2x->mf_cfg_base =
				bnx2x_readl ( bnx2x, ( bnx2x->shmem2_base +
						BNX2X_SHMEM2_MF_CFG_ADDR ) );
		}
	}
	if ( ! bnx2x->mf_cfg_base ) {
		DBGC ( bnx2x, "BNX2X %p no mf_cfg region\n", bnx2x );
		bnx2x->mf_mode = BNX2X_MF_NONE;
		return 0;
	}

	/* Determine forced single/multi function mode */
	feat = ( bnx2x_shmem_readl ( bnx2x, BNX2X_SHMEM_FEAT_CONFIG ) &
		 SHARED_FEAT_CFG_FORCE_SF_MODE_MASK );
	switch ( feat ) {
	case SHARED_FEAT_CFG_FORCE_SF_MODE_SWITCH_INDEPT:
		/* NPAR: valid only if the function has a legal MAC */
		val = bnx2x_mf_cfg_readl ( bnx2x,
				BNX2X_MF_CFG_FUNC_MAC_UPPER ( func ) );
		bnx2x->mf_mode = ( ( val != FUNC_MF_CFG_UPPERMAC_DEFAULT ) ?
				   BNX2X_MF_SI : BNX2X_MF_NONE );
		break;
	case SHARED_FEAT_CFG_FORCE_SF_MODE_MF_ALLOWED:
	case SHARED_FEAT_CFG_FORCE_SF_MODE_SPIO4:
		/* Switch-dependent: valid only if func 0 has an outer
		 * VLAN configured
		 */
		val = ( bnx2x_mf_cfg_readl ( bnx2x,
				BNX2X_MF_CFG_FUNC_E1HOV_TAG ( 0 ) ) &
			FUNC_MF_CFG_E1HOV_TAG_MASK );
		bnx2x->mf_mode = ( ( val != FUNC_MF_CFG_E1HOV_TAG_DEFAULT ) ?
				   BNX2X_MF_SD : BNX2X_MF_NONE );
		break;
	case SHARED_FEAT_CFG_FORCE_SF_MODE_BD_MODE:
	case SHARED_FEAT_CFG_FORCE_SF_MODE_UFP_MODE:
		bnx2x->mf_mode = BNX2X_MF_SD;
		break;
	case SHARED_FEAT_CFG_FORCE_SF_MODE_FORCED_SF:
		bnx2x->mf_mode = BNX2X_MF_NONE;
		break;
	default:
		/* AFEX and extended modes are not supported; treat as
		 * single-function but complain
		 */
		DBGC ( bnx2x, "BNX2X %p unsupported MF mode %08x\n",
		       bnx2x, feat );
		bnx2x->mf_mode = BNX2X_MF_UNSUPPORTED;
		break;
	}

	/* Record outer VLAN for switch-dependent mode */
	if ( bnx2x->mf_mode == BNX2X_MF_SD ) {
		val = ( bnx2x_mf_cfg_readl ( bnx2x,
				BNX2X_MF_CFG_FUNC_E1HOV_TAG ( func ) ) &
			FUNC_MF_CFG_E1HOV_TAG_MASK );
		if ( val != FUNC_MF_CFG_E1HOV_TAG_DEFAULT ) {
			bnx2x->mf_ov = val;
		} else {
			DBGC ( bnx2x, "BNX2X %p MF-SD without outer VLAN\n",
			       bnx2x );
			return -EINVAL;
		}
	}

	DBGC ( bnx2x, "BNX2X %p %s mode (feat %08x mf_cfg %08x ov %d)\n",
	       bnx2x,
	       ( ( bnx2x->mf_mode == BNX2X_MF_NONE ) ? "single-function" :
		 ( bnx2x->mf_mode == BNX2X_MF_SI ) ? "MF switch-independent" :
		 ( bnx2x->mf_mode == BNX2X_MF_SD ) ? "MF switch-dependent" :
		 "MF UNSUPPORTED" ),
	       feat, bnx2x->mf_cfg_base, bnx2x->mf_ov );

	return 0;
}

/**
 * Set MAC address from upper/lower register values
 *
 * @v hw_addr		MAC address to fill in
 * @v upper		Upper 16 bits
 * @v lower		Lower 32 bits
 */
static void bnx2x_set_mac_buf ( uint8_t *hw_addr, uint32_t upper,
				uint32_t lower ) {

	hw_addr[0] = ( upper >> 8 );
	hw_addr[1] = ( upper >> 0 );
	hw_addr[2] = ( lower >> 24 );
	hw_addr[3] = ( lower >> 16 );
	hw_addr[4] = ( lower >> 8 );
	hw_addr[5] = ( lower >> 0 );
}

/**
 * Fetch MAC address from MCP shared memory
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_fetch_mac ( struct bnx2x_nic *bnx2x ) {
	unsigned int func = bnx2x->pf_num;
	uint32_t upper;
	uint32_t lower;

	/* In multi-function modes the per-function MAC comes from the
	 * mf_cfg region; otherwise use the port MAC.
	 */
	if ( ( bnx2x->mf_mode == BNX2X_MF_SI ) ||
	     ( bnx2x->mf_mode == BNX2X_MF_SD ) ) {
		upper = bnx2x_mf_cfg_readl ( bnx2x,
				BNX2X_MF_CFG_FUNC_MAC_UPPER ( func ) );
		lower = bnx2x_mf_cfg_readl ( bnx2x,
				BNX2X_MF_CFG_FUNC_MAC_LOWER ( func ) );
		if ( ( upper != FUNC_MF_CFG_UPPERMAC_DEFAULT ) &&
		     ( lower != FUNC_MF_CFG_LOWERMAC_DEFAULT ) ) {
			bnx2x_set_mac_buf ( bnx2x->hw_addr, upper, lower );
			DBGC ( bnx2x, "BNX2X %p func %d MF MAC %s\n", bnx2x,
			       func, eth_ntoa ( bnx2x->hw_addr ) );
			return 0;
		}
		DBGC ( bnx2x, "BNX2X %p func %d has no valid MF MAC; "
		       "falling back to port MAC\n", bnx2x, func );
	}

	/* Read port MAC address */
	upper = bnx2x_shmem_readl ( bnx2x,
				    BNX2X_SHMEM_MAC_UPPER ( bnx2x->port ) );
	lower = bnx2x_shmem_readl ( bnx2x,
				    BNX2X_SHMEM_MAC_LOWER ( bnx2x->port ) );
	bnx2x_set_mac_buf ( bnx2x->hw_addr, upper, lower );
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
			unsigned int speed =
				LINK_STATUS_SPEED_AND_DUPLEX ( link_status );
			DBGC ( bnx2x, "BNX2X %p link up at %s (status "
			       "%08x)\n", bnx2x,
			       ( ( speed ==
				   LINK_STATUS_SPEED_AND_DUPLEX_20GXFD ) ?
				 "20G" :
				 ( speed ==
				   LINK_STATUS_SPEED_AND_DUPLEX_10GXFD ) ?
				 "10G" :
				 ( speed ==
				   LINK_STATUS_SPEED_AND_DUPLEX_1000XFD ) ?
				 "1G" : "other speed" ),
			       link_status );
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
 * Unload driver instance via MCP
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_mcp_unload ( struct bnx2x_nic *bnx2x ) {
	uint32_t response;

	/* Request unload */
	response = bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_UNLOAD_REQ_WOL_DIS,
				      0 );
	if ( ! response )
		return -EBUSY;
	DBGC ( bnx2x, "BNX2X %p MCP unload level %08x\n", bnx2x, response );

	/* Complete unload, leaving the link untouched so that the
	 * MFW-maintained link (which we rely upon) stays up.
	 */
	response = bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_UNLOAD_DONE,
				      DRV_MSG_CODE_UNLOAD_SKIP_LINK_RESET );
	if ( ! response )
		return -EBUSY;

	bnx2x->load_code = 0;
	return 0;
}

/**
 * Request driver load from MCP
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_mcp_load_request ( struct bnx2x_nic *bnx2x ) {
	uint32_t func_mb = BNX2X_SHMEM_FUNC_MB ( bnx2x->fw_mb_idx );
	uint32_t load_code;
	int rc;

	/* Resume mailbox sequence numbering from current state */
	bnx2x->fw_seq =
		( bnx2x_shmem_readl ( bnx2x, ( func_mb +
					BNX2X_FUNC_MB_DRV_MB_HEADER ) ) &
		  DRV_MSG_SEQ_NUMBER_MASK );
	DBGC2 ( bnx2x, "BNX2X %p initial fw_seq %04x\n",
		bnx2x, bnx2x->fw_seq );

	/* Recover from any previous driver instance (vendor UNDI
	 * driver, OS driver after a warm reboot, or an interrupted
	 * iPXE session) by requesting and completing an unload.  This
	 * is the (heavily simplified) equivalent of the Linux
	 * driver's bnx2x_prev_unload(): we cannot yet perform the
	 * "common" hardware cleanup for a chip left running by an
	 * uncleanly-stopped previous driver, but the unload handshake
	 * alone resets the MCP's load counts for this function.
	 */
	if ( ( rc = bnx2x_mcp_unload ( bnx2x ) ) != 0 ) {
		DBGC ( bnx2x, "BNX2X %p previous-unload failed\n", bnx2x );
		return rc;
	}

	/* Request load, with link flap avoidance so that an
	 * MFW-maintained link stays up across the handshake.
	 */
	load_code = bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_LOAD_REQ,
				       DRV_MSG_CODE_LOAD_REQ_WITH_LFA );
	if ( ! load_code ) {
		DBGC ( bnx2x, "BNX2X %p MCP load request timed out\n",
		       bnx2x );
		return -EBUSY;
	}
	if ( load_code == FW_MSG_CODE_DRV_LOAD_REFUSED ) {
		DBGC ( bnx2x, "BNX2X %p MCP refused load request\n", bnx2x );
		return -EBUSY;
	}
	bnx2x->load_code = load_code;
	DBGC ( bnx2x, "BNX2X %p MCP load level %08x (%s)\n", bnx2x, load_code,
	       ( ( load_code == FW_MSG_CODE_DRV_LOAD_COMMON_CHIP ) ?
		 "common+chip" :
		 ( load_code == FW_MSG_CODE_DRV_LOAD_COMMON ) ? "common" :
		 ( load_code == FW_MSG_CODE_DRV_LOAD_PORT ) ? "port" :
		 ( load_code == FW_MSG_CODE_DRV_LOAD_FUNCTION ) ? "function" :
		 "unknown" ) );

	return 0;
}

/**
 * Complete driver load via MCP
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_mcp_load_done ( struct bnx2x_nic *bnx2x ) {
	uint32_t func_mb = BNX2X_SHMEM_FUNC_MB ( bnx2x->fw_mb_idx );
	uint32_t response;

	/* Complete load */
	response = bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_LOAD_DONE, 0 );
	if ( ! response ) {
		DBGC ( bnx2x, "BNX2X %p MCP load-done timed out\n", bnx2x );
		return -EBUSY;
	}

	/* Tell the MCP not to expect heartbeat pulses from us */
	bnx2x_shmem_writel ( bnx2x, DRV_PULSE_ALWAYS_ALIVE,
			     ( func_mb + BNX2X_FUNC_MB_DRV_PULSE_MB ) );

	return 0;
}

/**
 * Open network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int bnx2x_open ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;
	int rc;

	/* Determine IGU configuration */
	if ( ( rc = bnx2x_igu_info ( bnx2x ) ) != 0 )
		return rc;

	/* Request load from MCP */
	if ( ( rc = bnx2x_mcp_load_request ( bnx2x ) ) != 0 )
		return rc;

	/* Initialise hardware (including storm firmware download) */
	if ( ( rc = bnx2x_hw_init ( bnx2x ) ) != 0 )
		goto err_hw_init;

	/* Complete load */
	if ( ( rc = bnx2x_mcp_load_done ( bnx2x ) ) != 0 )
		goto err_load_done;

	/* No datapath yet; refresh link state */
	bnx2x_check_link ( netdev );

	return 0;

 err_load_done:
	bnx2x_hw_free ( bnx2x );
 err_hw_init:
	/* Abort the load in the MCP's eyes */
	bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_UNLOAD_REQ_WOL_MCP, 0 );
	bnx2x_fw_command ( bnx2x, DRV_MSG_CODE_UNLOAD_DONE,
			   DRV_MSG_CODE_UNLOAD_SKIP_LINK_RESET );
	return rc;
}

/**
 * Close network device
 *
 * @v netdev		Network device
 */
static void bnx2x_close ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;

	/* Perform MCP unload handshake */
	bnx2x_mcp_unload ( bnx2x );

	/* Free hardware init memory */
	bnx2x_hw_free ( bnx2x );
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

	/* Detect multi-function mode */
	if ( ( rc = bnx2x_detect_mf ( bnx2x ) ) != 0 )
		goto err_shmem;

	/* Compute init mode flags and report firmware identity */
	bnx2x_set_modes_bitmap ( bnx2x );
	bnx2x_fw_info ( bnx2x );

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
