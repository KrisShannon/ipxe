/*
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Hardware initialisation sequences (common / port / function).
 *
 * Ported from the Linux bnx2x driver,
 * drivers/net/ethernet/broadcom/bnx2x/ (Linux v6.6), primarily
 * bnx2x_main.c bnx2x_init_hw_{common,port,func}() and the PXP/ILT/QM
 * helpers in bnx2x_init_ops.h:
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
 *
 * Deliberate simplifications relative to Linux (see CLAUDE.md):
 * E2/E3-only paths (no E1/E1H), single-function mode assumed for MF
 * specifics that do not apply (AFEX), no CNIC/SR-IOV, no DMAE (all
 * writes are individual register writes), FLR cleanup and parity
 * enable omitted, PHY common init relies on MFW link-flap avoidance.
 */

FILE_LICENCE ( GPL2_ONLY );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ipxe/pci.h>
#include <ipxe/timer.h>
#include <ipxe/malloc.h>
#include <ipxe/iobuf.h>
#include <ipxe/netdevice.h>
#include "bnx2x.h"
#include "bnx2x_init.h"
#include "bnx2x_hw.h"

/**
 * Poll a register until it reaches an expected value
 *
 * @v bnx2x		bnx2x device
 * @v reg		Register address
 * @v expected		Expected value
 * @v timeout_ms	Timeout in milliseconds
 * @ret value		Last value read
 */
static uint32_t bnx2x_reg_poll ( struct bnx2x_nic *bnx2x, uint32_t reg,
				 uint32_t expected,
				 unsigned int timeout_ms ) {
	uint32_t val;

	do {
		val = bnx2x_readl ( bnx2x, reg );
		if ( val == expected )
			break;
		mdelay ( 1 );
	} while ( timeout_ms-- );

	return val;
}

/**
 * Write a 64-bit value as two consecutive dwords
 *
 * @v bnx2x		bnx2x device
 * @v reg		Register address
 * @v val_lo		Low dword
 * @v val_hi		High dword
 */
static void bnx2x_wr_64 ( struct bnx2x_nic *bnx2x, uint32_t reg,
			  uint32_t val_lo, uint32_t val_hi ) {

	bnx2x_writel ( bnx2x, val_lo, reg );
	bnx2x_writel ( bnx2x, val_hi, ( reg + 4 ) );
}

/**
 * Get hardware lock control register for this function
 *
 * @v bnx2x		bnx2x device
 * @ret reg		Lock control register address
 */
static uint32_t bnx2x_hw_lock_reg ( struct bnx2x_nic *bnx2x ) {

	return ( ( bnx2x->pfid <= 5 ) ?
		 ( MISC_REG_DRIVER_CONTROL_1 + ( bnx2x->pfid * 8 ) ) :
		 ( MISC_REG_DRIVER_CONTROL_7 + ( ( bnx2x->pfid - 6 ) * 8 ) ) );
}

/**
 * Acquire hardware lock
 *
 * @v bnx2x		bnx2x device
 * @v resource		Resource number
 * @ret rc		Return status code
 */
static int bnx2x_acquire_hw_lock ( struct bnx2x_nic *bnx2x,
				   unsigned int resource ) {
	uint32_t lock_reg = bnx2x_hw_lock_reg ( bnx2x );
	uint32_t resource_bit = ( 1 << resource );
	uint32_t lock_status;
	unsigned int i;

	/* Try for 5 seconds every 5ms */
	for ( i = 0 ; i < 1000 ; i++ ) {
		bnx2x_writel ( bnx2x, resource_bit, ( lock_reg + 4 ) );
		lock_status = bnx2x_readl ( bnx2x, lock_reg );
		if ( lock_status & resource_bit )
			return 0;
		mdelay ( 5 );
	}

	DBGC ( bnx2x, "BNX2X %p could not acquire hw lock %d\n",
	       bnx2x, resource );
	return -ETIMEDOUT;
}

/**
 * Release hardware lock
 *
 * @v bnx2x		bnx2x device
 * @v resource		Resource number
 */
static void bnx2x_release_hw_lock ( struct bnx2x_nic *bnx2x,
				    unsigned int resource ) {
	uint32_t lock_reg = bnx2x_hw_lock_reg ( bnx2x );

	bnx2x_writel ( bnx2x, ( 1 << resource ), lock_reg );
}

/**
 * Pretend to be another function for subsequent register accesses
 *
 * @v bnx2x		bnx2x device
 * @v pretend_func	Absolute function number to pretend to be
 */
static void bnx2x_pretend_func ( struct bnx2x_nic *bnx2x,
				 unsigned int pretend_func ) {
	uint32_t pretend_reg;

	pretend_reg = ( PXP2_REG_PGL_PRETEND_FUNC_F0 +
			( bnx2x->pf_num * ( PXP2_REG_PGL_PRETEND_FUNC_F1 -
					    PXP2_REG_PGL_PRETEND_FUNC_F0 ) ) );
	bnx2x_writel ( bnx2x, pretend_func, pretend_reg );
	bnx2x_readl ( bnx2x, pretend_reg );
}

/**
 * Disable the (pretended) physical function
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_pf_disable ( struct bnx2x_nic *bnx2x ) {
	uint32_t val;

	val = bnx2x_readl ( bnx2x, IGU_REG_PF_CONFIGURATION );
	val &= ~IGU_PF_CONF_FUNC_EN;
	bnx2x_writel ( bnx2x, val, IGU_REG_PF_CONFIGURATION );
	bnx2x_writel ( bnx2x, 0, PGLUE_B_REG_INTERNAL_PFID_ENABLE_MASTER );
	bnx2x_writel ( bnx2x, 0, CFC_REG_WEAK_ENABLE_PF );
}

/**
 * Bring the chip blocks into reset
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_reset_common ( struct bnx2x_nic *bnx2x ) {
	uint32_t val = 0x1400;

	bnx2x_writel ( bnx2x, 0xd3ffff7f,
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR ) );

	/* E3: also reset the MSTAT blocks */
	val |= ( MISC_REGISTERS_RESET_REG_2_MSTAT0 |
		 MISC_REGISTERS_RESET_REG_2_MSTAT1 );
	bnx2x_writel ( bnx2x, val,
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_2_CLEAR ) );
}

/****************************************************************************
 * PXP arbiter configuration (from Linux bnx2x_init_ops.h)
 ****************************************************************************/

#define NUM_WR_Q	13
#define NUM_RD_Q	29
#define MAX_RD_ORD	3
#define MAX_WR_ORD	2

/** Configuration for one arbiter queue */
struct bnx2x_arb_line {
	uint32_t l;
	uint32_t add;
	uint32_t ubound;
};

/** Register addresses for one arbiter queue */
struct bnx2x_arb_addr {
	uint32_t l;
	uint32_t add;
	uint32_t ubound;
};

static const struct bnx2x_arb_line read_arb_data[NUM_RD_Q][MAX_RD_ORD + 1] = {
	{ {8, 64, 25}, {16, 64, 25}, {32, 64, 25}, {64, 64, 41} },
	{ {4, 8,  4},  {4,  8,  4},  {4,  8,  4},  {4,  8,  4}  },
	{ {4, 3,  3},  {4,  3,  3},  {4,  3,  3},  {4,  3,  3}  },
	{ {8, 3,  6},  {16, 3,  11}, {16, 3,  11}, {16, 3,  11} },
	{ {8, 64, 25}, {16, 64, 25}, {32, 64, 25}, {64, 64, 41} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {64, 3,  41} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {64, 3,  41} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {64, 3,  41} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {64, 3,  41} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 64, 6},  {16, 64, 11}, {32, 64, 21}, {32, 64, 21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 3,  6},  {16, 3,  11}, {32, 3,  21}, {32, 3,  21} },
	{ {8, 64, 25}, {16, 64, 41}, {32, 64, 81}, {64, 64, 120} }
};

static const struct bnx2x_arb_line write_arb_data[NUM_WR_Q][MAX_WR_ORD + 1] = {
	{ {4, 6,  3},  {4,  6,  3},  {4,  6,  3} },
	{ {4, 2,  3},  {4,  2,  3},  {4,  2,  3} },
	{ {8, 2,  6},  {16, 2,  11}, {16, 2,  11} },
	{ {8, 2,  6},  {16, 2,  11}, {32, 2,  21} },
	{ {8, 2,  6},  {16, 2,  11}, {32, 2,  21} },
	{ {8, 2,  6},  {16, 2,  11}, {32, 2,  21} },
	{ {8, 64, 25}, {16, 64, 25}, {32, 64, 25} },
	{ {8, 2,  6},  {16, 2,  11}, {16, 2,  11} },
	{ {8, 2,  6},  {16, 2,  11}, {16, 2,  11} },
	{ {8, 9,  6},  {16, 9,  11}, {32, 9,  21} },
	{ {8, 47, 19}, {16, 47, 19}, {32, 47, 21} },
	{ {8, 9,  6},  {16, 9,  11}, {16, 9,  11} },
	{ {8, 64, 25}, {16, 64, 41}, {32, 64, 81} }
};

static const struct bnx2x_arb_addr read_arb_addr[NUM_RD_Q - 1] = {
	{ PXP2_REG_RQ_BW_RD_L0, PXP2_REG_RQ_BW_RD_ADD0,
	  PXP2_REG_RQ_BW_RD_UBOUND0 },
	{ PXP2_REG_PSWRQ_BW_L1, PXP2_REG_PSWRQ_BW_ADD1,
	  PXP2_REG_PSWRQ_BW_UB1 },
	{ PXP2_REG_PSWRQ_BW_L2, PXP2_REG_PSWRQ_BW_ADD2,
	  PXP2_REG_PSWRQ_BW_UB2 },
	{ PXP2_REG_PSWRQ_BW_L3, PXP2_REG_PSWRQ_BW_ADD3,
	  PXP2_REG_PSWRQ_BW_UB3 },
	{ PXP2_REG_RQ_BW_RD_L4, PXP2_REG_RQ_BW_RD_ADD4,
	  PXP2_REG_RQ_BW_RD_UBOUND4 },
	{ PXP2_REG_RQ_BW_RD_L5, PXP2_REG_RQ_BW_RD_ADD5,
	  PXP2_REG_RQ_BW_RD_UBOUND5 },
	{ PXP2_REG_PSWRQ_BW_L6, PXP2_REG_PSWRQ_BW_ADD6,
	  PXP2_REG_PSWRQ_BW_UB6 },
	{ PXP2_REG_PSWRQ_BW_L7, PXP2_REG_PSWRQ_BW_ADD7,
	  PXP2_REG_PSWRQ_BW_UB7 },
	{ PXP2_REG_PSWRQ_BW_L8, PXP2_REG_PSWRQ_BW_ADD8,
	  PXP2_REG_PSWRQ_BW_UB8 },
	{ PXP2_REG_PSWRQ_BW_L9, PXP2_REG_PSWRQ_BW_ADD9,
	  PXP2_REG_PSWRQ_BW_UB9 },
	{ PXP2_REG_PSWRQ_BW_L10, PXP2_REG_PSWRQ_BW_ADD10,
	  PXP2_REG_PSWRQ_BW_UB10 },
	{ PXP2_REG_PSWRQ_BW_L11, PXP2_REG_PSWRQ_BW_ADD11,
	  PXP2_REG_PSWRQ_BW_UB11 },
	{ PXP2_REG_RQ_BW_RD_L12, PXP2_REG_RQ_BW_RD_ADD12,
	  PXP2_REG_RQ_BW_RD_UBOUND12 },
	{ PXP2_REG_RQ_BW_RD_L13, PXP2_REG_RQ_BW_RD_ADD13,
	  PXP2_REG_RQ_BW_RD_UBOUND13 },
	{ PXP2_REG_RQ_BW_RD_L14, PXP2_REG_RQ_BW_RD_ADD14,
	  PXP2_REG_RQ_BW_RD_UBOUND14 },
	{ PXP2_REG_RQ_BW_RD_L15, PXP2_REG_RQ_BW_RD_ADD15,
	  PXP2_REG_RQ_BW_RD_UBOUND15 },
	{ PXP2_REG_RQ_BW_RD_L16, PXP2_REG_RQ_BW_RD_ADD16,
	  PXP2_REG_RQ_BW_RD_UBOUND16 },
	{ PXP2_REG_RQ_BW_RD_L17, PXP2_REG_RQ_BW_RD_ADD17,
	  PXP2_REG_RQ_BW_RD_UBOUND17 },
	{ PXP2_REG_RQ_BW_RD_L18, PXP2_REG_RQ_BW_RD_ADD18,
	  PXP2_REG_RQ_BW_RD_UBOUND18 },
	{ PXP2_REG_RQ_BW_RD_L19, PXP2_REG_RQ_BW_RD_ADD19,
	  PXP2_REG_RQ_BW_RD_UBOUND19 },
	{ PXP2_REG_RQ_BW_RD_L20, PXP2_REG_RQ_BW_RD_ADD20,
	  PXP2_REG_RQ_BW_RD_UBOUND20 },
	{ PXP2_REG_RQ_BW_RD_L22, PXP2_REG_RQ_BW_RD_ADD22,
	  PXP2_REG_RQ_BW_RD_UBOUND22 },
	{ PXP2_REG_RQ_BW_RD_L23, PXP2_REG_RQ_BW_RD_ADD23,
	  PXP2_REG_RQ_BW_RD_UBOUND23 },
	{ PXP2_REG_RQ_BW_RD_L24, PXP2_REG_RQ_BW_RD_ADD24,
	  PXP2_REG_RQ_BW_RD_UBOUND24 },
	{ PXP2_REG_RQ_BW_RD_L25, PXP2_REG_RQ_BW_RD_ADD25,
	  PXP2_REG_RQ_BW_RD_UBOUND25 },
	{ PXP2_REG_RQ_BW_RD_L26, PXP2_REG_RQ_BW_RD_ADD26,
	  PXP2_REG_RQ_BW_RD_UBOUND26 },
	{ PXP2_REG_RQ_BW_RD_L27, PXP2_REG_RQ_BW_RD_ADD27,
	  PXP2_REG_RQ_BW_RD_UBOUND27 },
	{ PXP2_REG_PSWRQ_BW_L28, PXP2_REG_PSWRQ_BW_ADD28,
	  PXP2_REG_PSWRQ_BW_UB28 }
};

static const struct bnx2x_arb_addr write_arb_addr[NUM_WR_Q - 1] = {
	{ PXP2_REG_PSWRQ_BW_L1, PXP2_REG_PSWRQ_BW_ADD1,
	  PXP2_REG_PSWRQ_BW_UB1 },
	{ PXP2_REG_PSWRQ_BW_L2, PXP2_REG_PSWRQ_BW_ADD2,
	  PXP2_REG_PSWRQ_BW_UB2 },
	{ PXP2_REG_PSWRQ_BW_L3, PXP2_REG_PSWRQ_BW_ADD3,
	  PXP2_REG_PSWRQ_BW_UB3 },
	{ PXP2_REG_PSWRQ_BW_L6, PXP2_REG_PSWRQ_BW_ADD6,
	  PXP2_REG_PSWRQ_BW_UB6 },
	{ PXP2_REG_PSWRQ_BW_L7, PXP2_REG_PSWRQ_BW_ADD7,
	  PXP2_REG_PSWRQ_BW_UB7 },
	{ PXP2_REG_PSWRQ_BW_L8, PXP2_REG_PSWRQ_BW_ADD8,
	  PXP2_REG_PSWRQ_BW_UB8 },
	{ PXP2_REG_PSWRQ_BW_L9, PXP2_REG_PSWRQ_BW_ADD9,
	  PXP2_REG_PSWRQ_BW_UB9 },
	{ PXP2_REG_PSWRQ_BW_L10, PXP2_REG_PSWRQ_BW_ADD10,
	  PXP2_REG_PSWRQ_BW_UB10 },
	{ PXP2_REG_PSWRQ_BW_L11, PXP2_REG_PSWRQ_BW_ADD11,
	  PXP2_REG_PSWRQ_BW_UB11 },
	{ PXP2_REG_PSWRQ_BW_L28, PXP2_REG_PSWRQ_BW_ADD28,
	  PXP2_REG_PSWRQ_BW_UB28 },
	{ PXP2_REG_RQ_BW_WR_L29, PXP2_REG_RQ_BW_WR_ADD29,
	  PXP2_REG_RQ_BW_WR_UBOUND29 },
	{ PXP2_REG_RQ_BW_WR_L30, PXP2_REG_RQ_BW_WR_ADD30,
	  PXP2_REG_RQ_BW_WR_UBOUND30 }
};

/**
 * Configure the PXP arbiter
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_init_pxp ( struct bnx2x_nic *bnx2x ) {
	uint16_t devctl = 0;
	unsigned int r_order, w_order;
	unsigned int cap;
	uint32_t val;
	unsigned int i;

	/* Derive read/write orders from PCIe max payload/read request */
	cap = pci_find_capability ( bnx2x->pci, PCI_CAP_ID_EXP );
	if ( cap )
		pci_read_config_word ( bnx2x->pci, ( cap + PCI_EXP_DEVCTL ),
				       &devctl );
	w_order = ( ( devctl & 0x00e0 ) >> 5 );
	r_order = ( ( devctl & 0x7000 ) >> 12 );
	if ( r_order > MAX_RD_ORD )
		r_order = MAX_RD_ORD;
	if ( w_order > MAX_WR_ORD )
		w_order = MAX_WR_ORD;
	DBGC ( bnx2x, "BNX2X %p PXP arbiter read order %d write order %d\n",
	       bnx2x, r_order, w_order );

	for ( i = 0 ; i < ( NUM_RD_Q - 1 ) ; i++ ) {
		bnx2x_writel ( bnx2x, read_arb_data[i][r_order].l,
			       read_arb_addr[i].l );
		bnx2x_writel ( bnx2x, read_arb_data[i][r_order].add,
			       read_arb_addr[i].add );
		bnx2x_writel ( bnx2x, read_arb_data[i][r_order].ubound,
			       read_arb_addr[i].ubound );
	}

	for ( i = 0 ; i < ( NUM_WR_Q - 1 ) ; i++ ) {
		if ( ( write_arb_addr[i].l == PXP2_REG_RQ_BW_WR_L29 ) ||
		     ( write_arb_addr[i].l == PXP2_REG_RQ_BW_WR_L30 ) ) {
			bnx2x_writel ( bnx2x, write_arb_data[i][w_order].l,
				       write_arb_addr[i].l );
			bnx2x_writel ( bnx2x, write_arb_data[i][w_order].add,
				       write_arb_addr[i].add );
			bnx2x_writel ( bnx2x,
				       write_arb_data[i][w_order].ubound,
				       write_arb_addr[i].ubound );
		} else {
			val = bnx2x_readl ( bnx2x, write_arb_addr[i].l );
			bnx2x_writel ( bnx2x,
				       ( val |
					 ( write_arb_data[i][w_order].l
					   << 10 ) ),
				       write_arb_addr[i].l );
			val = bnx2x_readl ( bnx2x, write_arb_addr[i].add );
			bnx2x_writel ( bnx2x,
				       ( val |
					 ( write_arb_data[i][w_order].add
					   << 10 ) ),
				       write_arb_addr[i].add );
			val = bnx2x_readl ( bnx2x, write_arb_addr[i].ubound );
			bnx2x_writel ( bnx2x,
				       ( val |
					 ( write_arb_data[i][w_order].ubound
					   << 7 ) ),
				       write_arb_addr[i].ubound );
		}
	}

	val = write_arb_data[NUM_WR_Q - 1][w_order].add;
	val += ( write_arb_data[NUM_WR_Q - 1][w_order].ubound << 10 );
	val += ( write_arb_data[NUM_WR_Q - 1][w_order].l << 17 );
	bnx2x_writel ( bnx2x, val, PXP2_REG_PSWRQ_BW_RD );

	val = read_arb_data[NUM_RD_Q - 1][r_order].add;
	val += ( read_arb_data[NUM_RD_Q - 1][r_order].ubound << 10 );
	val += ( read_arb_data[NUM_RD_Q - 1][r_order].l << 17 );
	bnx2x_writel ( bnx2x, val, PXP2_REG_PSWRQ_BW_WR );

	bnx2x_writel ( bnx2x, w_order, PXP2_REG_RQ_WR_MBS0 );
	bnx2x_writel ( bnx2x, w_order, PXP2_REG_RQ_WR_MBS1 );
	bnx2x_writel ( bnx2x, r_order, PXP2_REG_RQ_RD_MBS0 );
	bnx2x_writel ( bnx2x, r_order, PXP2_REG_RQ_RD_MBS1 );

	/* E3 */
	bnx2x_writel ( bnx2x, ( 0x4 << w_order ), PXP2_REG_WR_USDMDP_TH );

	/* MPS thresholds (E2/E3 can use the optimal value) */
	val = w_order;
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_DMAE_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_HC_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_USDM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_CSDM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_TSDM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_XSDM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_QM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_TM_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_SRC_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_DBG_MPS );
	bnx2x_writel ( bnx2x, val, PXP2_REG_WR_CDU_MPS );

	/* Validate number of tags supported by device */
	val = ( bnx2x_readl ( bnx2x, 0x2980 ) & 0xff );
	if ( val <= 0x20 )
		bnx2x_writel ( bnx2x, 0x20, PXP2_REG_PGL_TAGS_LIMIT );
}

/**
 * Configure PXP endianness (little-endian host)
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_set_endianity ( struct bnx2x_nic *bnx2x ) {

	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_QM_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_TM_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_SRC_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_CDU_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_DBG_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_HC_ENDIAN_M );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RD_QM_SWAP_MODE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RD_TM_SWAP_MODE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RD_SRC_SWAP_MODE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RD_CDURD_SWAP_MODE );
}

/****************************************************************************
 * ILT configuration
 *
 * Fixed geometry: within this function's 384-line ILT window, line 0
 * is the single 32kB CDU context page and lines 1-16 are the sixteen
 * 4kB QM pages (for qm_cid_count=1024).
 ****************************************************************************/

/**
 * Write one ILT line
 *
 * @v bnx2x		bnx2x device
 * @v abs_idx		Absolute ILT line index
 * @v phys		Physical address (0 to clear, valid bit still set)
 */
static void bnx2x_ilt_line_wr ( struct bnx2x_nic *bnx2x, unsigned int abs_idx,
				physaddr_t phys ) {
	uint32_t reg = ( PXP2_REG_RQ_ONCHIP_AT_B0 + ( abs_idx * 8 ) );

	bnx2x_wr_64 ( bnx2x, reg, ILT_ADDR1 ( phys ), ILT_ADDR2 ( phys ) );
}

/**
 * Program ILT page sizes (common phase)
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_ilt_init_page_size ( struct bnx2x_nic *bnx2x ) {

	/* ILOG2(page_size >> 12): CDU 32kB -> 3, QM/SRC/TM 4kB -> 0 */
	bnx2x_writel ( bnx2x, 3, PXP2_REG_RQ_CDU_P_SIZE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_QM_P_SIZE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_SRC_P_SIZE );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_TM_P_SIZE );
}

/**
 * E2/E3 timers-block workaround: mark the entire ILT valid (zero
 * addresses) and point the pretended vnic-3 function's TM client at
 * the whole range, so that stray timer scans cannot cause an
 * unrecoverable translation error.
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_ilt_timers_workaround ( struct bnx2x_nic *bnx2x ) {
	unsigned int i;

	bnx2x_pretend_func ( bnx2x, ( bnx2x->path + 6 ) );

	for ( i = 0 ; i < ILT_NUM_PAGE_ENTRIES ; i++ )
		bnx2x_ilt_line_wr ( bnx2x, i, 0 );

	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_TM_FIRST_ILT );
	bnx2x_writel ( bnx2x, ( ILT_NUM_PAGE_ENTRIES - 1 ),
		       PXP2_REG_RQ_TM_LAST_ILT );

	bnx2x_pretend_func ( bnx2x, bnx2x->pf_num );
}

/**
 * Program this function's ILT lines and client boundaries
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_ilt_init_func ( struct bnx2x_nic *bnx2x ) {
	unsigned int start_line = FUNC_ILT_BASE ( bnx2x->pfid );
	physaddr_t cdu_phys = virt_to_bus ( bnx2x->cdu_context );
	physaddr_t qm_phys = virt_to_bus ( bnx2x->qm_mem );
	unsigned int i;

	/* CDU: line 0 */
	bnx2x_ilt_line_wr ( bnx2x, start_line, cdu_phys );
	bnx2x_writel ( bnx2x, start_line, PXP2_REG_RQ_CDU_FIRST_ILT );
	bnx2x_writel ( bnx2x, start_line, PXP2_REG_RQ_CDU_LAST_ILT );

	/* QM: lines 1..16 */
	for ( i = 0 ; i < BNX2X_QM_PAGES ; i++ ) {
		bnx2x_ilt_line_wr ( bnx2x, ( start_line + 1 + i ),
				    ( qm_phys + ( i * BNX2X_QM_PAGE_SIZE ) ) );
	}
	bnx2x_writel ( bnx2x, ( start_line + 1 ), PXP2_REG_RQ_QM_FIRST_ILT );
	bnx2x_writel ( bnx2x, ( start_line + BNX2X_QM_PAGES ),
		       PXP2_REG_RQ_QM_LAST_ILT );
}

/****************************************************************************
 * QM initialisation
 ****************************************************************************/

/**
 * Initialise the QM queue pointer table (common phase)
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_qm_init_ptr_table ( struct bnx2x_nic *bnx2x ) {
	unsigned int i;

	for ( i = 0 ; i < ( 4 * BNX2X_QM_QUEUES_PER_FUNC ) ; i++ ) {
		bnx2x_writel ( bnx2x,
			       ( BNX2X_QM_CID_COUNT * 4 *
				 ( i % BNX2X_QM_QUEUES_PER_FUNC ) ),
			       ( QM_REG_BASEADDR + ( i * 4 ) ) );
		bnx2x_wr_64 ( bnx2x, ( QM_REG_PTRTBL + ( i * 8 ) ), 0, 0 );
	}
}

/****************************************************************************
 * IGU
 ****************************************************************************/

/**
 * Determine IGU configuration (normal mode assumed/enforced)
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
int bnx2x_igu_info ( struct bnx2x_nic *bnx2x ) {
	uint32_t val;
	unsigned int igu_sb_id;
	unsigned int fid;
	unsigned int vec;
	int rc;

	if ( ( rc = bnx2x_acquire_hw_lock ( bnx2x,
					    HW_LOCK_RESOURCE_RESET ) ) != 0 )
		return rc;

	/* Force IGU normal (non-backward-compatible) mode if needed */
	val = bnx2x_readl ( bnx2x, IGU_REG_BLOCK_CONFIGURATION );
	if ( val & IGU_BLOCK_CONFIGURATION_REG_BACKWARD_COMP_EN ) {
		unsigned int tout = 5000;

		DBGC ( bnx2x, "BNX2X %p forcing IGU normal mode\n", bnx2x );
		val &= ~IGU_BLOCK_CONFIGURATION_REG_BACKWARD_COMP_EN;
		bnx2x_writel ( bnx2x, val, IGU_REG_BLOCK_CONFIGURATION );
		bnx2x_writel ( bnx2x, 0x7f, IGU_REG_RESET_MEMORIES );
		while ( tout-- &&
			bnx2x_readl ( bnx2x, IGU_REG_RESET_MEMORIES ) ) {
			mdelay ( 1 );
		}
		if ( bnx2x_readl ( bnx2x, IGU_REG_RESET_MEMORIES ) ) {
			DBGC ( bnx2x, "BNX2X %p failed to force IGU normal "
			       "mode\n", bnx2x );
			bnx2x_release_hw_lock ( bnx2x,
						HW_LOCK_RESOURCE_RESET );
			return -EIO;
		}
	}

	/* Read the IGU CAM to find this function's status blocks */
	bnx2x->igu_dsb_id = ~0U;
	bnx2x->igu_base_sb = ~0U;
	bnx2x->igu_sb_cnt = 0;
	for ( igu_sb_id = 0 ; igu_sb_id < IGU_REG_MAPPING_MEMORY_SIZE ;
	      igu_sb_id++ ) {
		val = bnx2x_readl ( bnx2x, ( IGU_REG_MAPPING_MEMORY +
					     ( igu_sb_id * 4 ) ) );
		if ( ! ( val & IGU_REG_MAPPING_MEMORY_VALID ) )
			continue;
		fid = ( ( val & IGU_REG_MAPPING_MEMORY_FID_MASK ) >>
			IGU_REG_MAPPING_MEMORY_FID_SHIFT );
		if ( ! ( fid & IGU_FID_ENCODE_IS_PF ) )
			continue;
		if ( ( fid & IGU_FID_PF_NUM_MASK ) != bnx2x->pfid )
			continue;
		vec = ( ( val & IGU_REG_MAPPING_MEMORY_VECTOR_MASK ) >>
			IGU_REG_MAPPING_MEMORY_VECTOR_SHIFT );
		if ( vec == 0 ) {
			bnx2x->igu_dsb_id = igu_sb_id;
		} else {
			if ( bnx2x->igu_base_sb == ~0U )
				bnx2x->igu_base_sb = igu_sb_id;
			bnx2x->igu_sb_cnt++;
		}
	}

	bnx2x_release_hw_lock ( bnx2x, HW_LOCK_RESOURCE_RESET );

	if ( ( bnx2x->igu_dsb_id == ~0U ) || ( bnx2x->igu_sb_cnt == 0 ) ) {
		DBGC ( bnx2x, "BNX2X %p IGU CAM configuration error\n",
		       bnx2x );
		return -EINVAL;
	}
	DBGC ( bnx2x, "BNX2X %p IGU dsb %d base sb %d cnt %d\n", bnx2x,
	       bnx2x->igu_dsb_id, bnx2x->igu_base_sb, bnx2x->igu_sb_cnt );

	return 0;
}

/**
 * Acknowledge a status block via the IGU (normal mode)
 *
 * @v bnx2x		bnx2x device
 * @v igu_sb_id		IGU status block id
 * @v segment		IGU segment
 * @v index		Status block index value
 * @v op		Interrupt operation (IGU_INT_xxx)
 * @v update		Update flag
 */
void bnx2x_igu_ack_sb ( struct bnx2x_nic *bnx2x, unsigned int igu_sb_id,
			unsigned int segment, uint16_t index, unsigned int op,
			unsigned int update ) {
	uint32_t igu_addr = ( BAR_IGU_INTMEM +
			      ( ( IGU_CMD_INT_ACK_BASE + igu_sb_id ) * 8 ) );
	uint32_t cmd_data;

	cmd_data = ( ( index << IGU_REGULAR_SB_INDEX_SHIFT ) |
		     ( segment << IGU_REGULAR_SEGMENT_ACCESS_SHIFT ) |
		     ( update << IGU_REGULAR_BUPDATE_SHIFT ) |
		     ( op << IGU_REGULAR_ENABLE_INT_SHIFT ) );
	bnx2x_writel ( bnx2x, cmd_data, igu_addr );
}

/**
 * Clean up a status block's IGU state
 *
 * @v bnx2x		bnx2x device
 * @v idu_sb_id		IGU status block id
 */
static void bnx2x_igu_clear_sb ( struct bnx2x_nic *bnx2x,
				 unsigned int idu_sb_id ) {
	uint32_t igu_addr_ack = ( IGU_REG_CSTORM_TYPE_0_SB_CLEANUP +
				  ( ( idu_sb_id / 32 ) * 4 ) );
	uint32_t sb_bit = ( 1 << ( idu_sb_id % 32 ) );
	uint32_t func_encode = ( bnx2x->pfid |
				 ( 1 << IGU_FID_ENCODE_IS_PF_SHIFT ) );
	uint32_t addr_encode = ( IGU_CMD_E2_PROD_UPD_BASE + idu_sb_id );
	uint32_t data, ctl;
	unsigned int cnt = 100;

	data = ( ( IGU_USE_REGISTER_cstorm_type_0_sb_cleanup
		   << IGU_REGULAR_CLEANUP_TYPE_SHIFT ) |
		 IGU_REGULAR_CLEANUP_SET | IGU_REGULAR_BCLEANUP );
	ctl = ( ( addr_encode << IGU_CTRL_REG_ADDRESS_SHIFT ) |
		( func_encode << IGU_CTRL_REG_FID_SHIFT ) |
		( IGU_CTRL_CMD_TYPE_WR << IGU_CTRL_REG_TYPE_SHIFT ) );

	bnx2x_writel ( bnx2x, data, IGU_REG_COMMAND_REG_32LSB_DATA );
	bnx2x_writel ( bnx2x, ctl, IGU_REG_COMMAND_REG_CTRL );

	/* Wait for cleanup to finish */
	while ( ! ( bnx2x_readl ( bnx2x, igu_addr_ack ) & sb_bit ) &&
		--cnt ) {
		mdelay ( 20 );
	}
	if ( ! ( bnx2x_readl ( bnx2x, igu_addr_ack ) & sb_bit ) ) {
		DBGC ( bnx2x, "BNX2X %p IGU cleanup of sb %d did not "
		       "complete\n", bnx2x, idu_sb_id );
	}
}

/****************************************************************************
 * Attention masking
 ****************************************************************************/

/**
 * Enable/mask block attentions (common phase)
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_enable_blocks_attention ( struct bnx2x_nic *bnx2x ) {
	uint32_t val;

	bnx2x_writel ( bnx2x, 0, PXP_REG_PXP_INT_MASK_0 );
	bnx2x_writel ( bnx2x, 0x40, PXP_REG_PXP_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, DORQ_REG_DORQ_INT_MASK );
	bnx2x_writel ( bnx2x, 0, CFC_REG_CFC_INT_MASK );
	/* Read-length errors in BRB are legal for the parser */
	bnx2x_writel ( bnx2x, 0xfc00, BRB1_REG_BRB1_INT_MASK );
	bnx2x_writel ( bnx2x, 0, QM_REG_QM_INT_MASK );
	bnx2x_writel ( bnx2x, 0, TM_REG_TM_INT_MASK );
	bnx2x_writel ( bnx2x, 0, XSDM_REG_XSDM_INT_MASK_0 );
	bnx2x_writel ( bnx2x, 0, XSDM_REG_XSDM_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, XCM_REG_XCM_INT_MASK );
	bnx2x_writel ( bnx2x, 0, USDM_REG_USDM_INT_MASK_0 );
	bnx2x_writel ( bnx2x, 0, USDM_REG_USDM_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, UCM_REG_UCM_INT_MASK );
	bnx2x_writel ( bnx2x, 0, ( GRCBASE_UPB + PB_REG_PB_INT_MASK ) );
	bnx2x_writel ( bnx2x, 0, CSDM_REG_CSDM_INT_MASK_0 );
	bnx2x_writel ( bnx2x, 0, CSDM_REG_CSDM_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, CCM_REG_CCM_INT_MASK );

	val = ( PXP2_PXP2_INT_MASK_0_REG_PGL_CPL_AFT |
		PXP2_PXP2_INT_MASK_0_REG_PGL_CPL_OF |
		PXP2_PXP2_INT_MASK_0_REG_PGL_PCIE_ATTN |
		PXP2_PXP2_INT_MASK_0_REG_PGL_READ_BLOCKED |
		PXP2_PXP2_INT_MASK_0_REG_PGL_WRITE_BLOCKED );
	bnx2x_writel ( bnx2x, val, PXP2_REG_PXP2_INT_MASK_0 );

	bnx2x_writel ( bnx2x, 0, TSDM_REG_TSDM_INT_MASK_0 );
	bnx2x_writel ( bnx2x, 0, TSDM_REG_TSDM_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, TCM_REG_TCM_INT_MASK );
	/* Enable VFC attentions: bits 11 and 12, bits 31:13 reserved */
	bnx2x_writel ( bnx2x, 0x07ff, TSEM_REG_TSEM_INT_MASK_1 );
	bnx2x_writel ( bnx2x, 0, CDU_REG_CDU_INT_MASK );
	bnx2x_writel ( bnx2x, 0, DMAE_REG_DMAE_INT_MASK );
	/* PBF bits 3 and 4 masked */
	bnx2x_writel ( bnx2x, 0x18, PBF_REG_PBF_INT_MASK );
}

/****************************************************************************
 * Common / port / function initialisation
 ****************************************************************************/

/**
 * Initialise hardware at the COMMON phase
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_init_hw_common ( struct bnx2x_nic *bnx2x ) {
	unsigned int abs_func;
	unsigned int i;
	uint32_t val;
	int rc;

	DBGC ( bnx2x, "BNX2X %p common init\n", bnx2x );

	/* Take the RESET lock while resetting the chip */
	if ( ( rc = bnx2x_acquire_hw_lock ( bnx2x,
					    HW_LOCK_RESOURCE_RESET ) ) != 0 )
		return rc;
	bnx2x_reset_common ( bnx2x );
	bnx2x_writel ( bnx2x, 0xffffffff,
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET ) );
	bnx2x_writel ( bnx2x, ( 0xfffc |
				MISC_REGISTERS_RESET_REG_2_MSTAT0 |
				MISC_REGISTERS_RESET_REG_2_MSTAT1 ),
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_2_SET ) );
	bnx2x_release_hw_lock ( bnx2x, HW_LOCK_RESOURCE_RESET );

	bnx2x_init_block ( bnx2x, BLOCK_MISC, PHASE_COMMON );

	/* Disable PF master-enable for all functions on this path,
	 * then re-enable for ourselves
	 */
	for ( abs_func = bnx2x->path ; abs_func < ( E2_FUNC_MAX * 2 ) ;
	      abs_func += 2 ) {
		if ( abs_func == bnx2x->pf_num ) {
			bnx2x_writel ( bnx2x, 1,
				PGLUE_B_REG_INTERNAL_PFID_ENABLE_MASTER );
			continue;
		}
		bnx2x_pretend_func ( bnx2x, abs_func );
		bnx2x_pf_disable ( bnx2x );
		bnx2x_pretend_func ( bnx2x, bnx2x->pf_num );
	}

	bnx2x_init_block ( bnx2x, BLOCK_PXP, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_PXP2, PHASE_COMMON );
	bnx2x_init_pxp ( bnx2x );
	bnx2x_set_endianity ( bnx2x );
	bnx2x_ilt_init_page_size ( bnx2x );

	/* Let the HW do its magic */
	mdelay ( 100 );
	val = bnx2x_readl ( bnx2x, PXP2_REG_RQ_CFG_DONE );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p PXP2 RQ_CFG failed\n", bnx2x );
		return -EBUSY;
	}
	val = bnx2x_readl ( bnx2x, PXP2_REG_RD_INIT_DONE );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p PXP2 RD_INIT failed\n", bnx2x );
		return -EBUSY;
	}

	/* Timers bug workaround: zero+valid the whole ILT and give
	 * vnic-3 of this path a full-range TM window
	 */
	bnx2x_ilt_timers_workaround ( bnx2x );
	bnx2x_writel ( bnx2x, BNX2X_PXP_DRAM_ALIGN, PXP2_REG_RQ_DRAM_ALIGN );
	bnx2x_writel ( bnx2x, BNX2X_PXP_DRAM_ALIGN,
		       PXP2_REG_RQ_DRAM_ALIGN_RD );
	bnx2x_writel ( bnx2x, 1, PXP2_REG_RQ_DRAM_ALIGN_SEL );

	bnx2x_writel ( bnx2x, 0, PXP2_REG_RQ_DISABLE_INPUTS );
	bnx2x_writel ( bnx2x, 0, PXP2_REG_RD_DISABLE_INPUTS );

	bnx2x_init_block ( bnx2x, BLOCK_PGLUE_B, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_ATC, PHASE_COMMON );
	mdelay ( 200 );
	val = bnx2x_readl ( bnx2x, ATC_REG_ATC_INIT_DONE );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p ATC_INIT failed\n", bnx2x );
		return -EBUSY;
	}

	bnx2x_init_block ( bnx2x, BLOCK_DMAE, PHASE_COMMON );

	/* Clean the DMAE memory (Linux writes 8 zero dwords to the
	 * start of TSEM PRAM via DMAE as a self-test; plain writes
	 * here)
	 */
	for ( i = 0 ; i < 8 ; i++ )
		bnx2x_writel ( bnx2x, 0, ( 0x1c0000 /* TSEM_REG_PRAM */ +
					   ( i * 4 ) ) );

	bnx2x_init_block ( bnx2x, BLOCK_TCM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_UCM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_CCM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_XCM, PHASE_COMMON );

	/* Read SEM passive buffers */
	bnx2x_readl ( bnx2x, XSEM_REG_PASSIVE_BUFFER );
	bnx2x_readl ( bnx2x, ( XSEM_REG_PASSIVE_BUFFER + 4 ) );
	bnx2x_readl ( bnx2x, ( XSEM_REG_PASSIVE_BUFFER + 8 ) );
	bnx2x_readl ( bnx2x, CSEM_REG_PASSIVE_BUFFER );
	bnx2x_readl ( bnx2x, ( CSEM_REG_PASSIVE_BUFFER + 4 ) );
	bnx2x_readl ( bnx2x, ( CSEM_REG_PASSIVE_BUFFER + 8 ) );
	bnx2x_readl ( bnx2x, TSEM_REG_PASSIVE_BUFFER );
	bnx2x_readl ( bnx2x, ( TSEM_REG_PASSIVE_BUFFER + 4 ) );
	bnx2x_readl ( bnx2x, ( TSEM_REG_PASSIVE_BUFFER + 8 ) );
	bnx2x_readl ( bnx2x, USEM_REG_PASSIVE_BUFFER );
	bnx2x_readl ( bnx2x, ( USEM_REG_PASSIVE_BUFFER + 4 ) );
	bnx2x_readl ( bnx2x, ( USEM_REG_PASSIVE_BUFFER + 8 ) );

	bnx2x_init_block ( bnx2x, BLOCK_QM, PHASE_COMMON );
	bnx2x_qm_init_ptr_table ( bnx2x );

	/* Soft reset pulse */
	bnx2x_writel ( bnx2x, 1, QM_REG_SOFT_RESET );
	bnx2x_writel ( bnx2x, 0, QM_REG_SOFT_RESET );

	bnx2x_init_block ( bnx2x, BLOCK_DORQ, PHASE_COMMON );
	bnx2x_writel ( bnx2x, 0, DORQ_REG_DORQ_INT_MASK );

	bnx2x_init_block ( bnx2x, BLOCK_BRB1, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_PRS, PHASE_COMMON );
	bnx2x_writel ( bnx2x, 0xf, PRS_REG_A_PRSU_20 );
	bnx2x_writel ( bnx2x, 0, PRS_REG_E1HOV_MODE );
	/* (E3 B0: PRS_REG_HDRS_AFTER_BASIC is per-port) */

	bnx2x_init_block ( bnx2x, BLOCK_TSDM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_CSDM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_USDM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_XSDM, PHASE_COMMON );

	/* Reset VFC memories */
	bnx2x_writel ( bnx2x, ( VFC_MEMORIES_RST_REG_CAM_RST |
				VFC_MEMORIES_RST_REG_RAM_RST ),
		       ( TSEM_REG_FAST_MEMORY + VFC_REG_MEMORIES_RST ) );
	bnx2x_writel ( bnx2x, ( VFC_MEMORIES_RST_REG_CAM_RST |
				VFC_MEMORIES_RST_REG_RAM_RST ),
		       ( XSEM_REG_FAST_MEMORY + VFC_REG_MEMORIES_RST ) );
	mdelay ( 20 );

	/* SEM block init loads the storm firmware (OP_ZP unzip of
	 * INT_TABLE and PRAM sections)
	 */
	bnx2x_init_block ( bnx2x, BLOCK_TSEM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_USEM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_CSEM, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_XSEM, PHASE_COMMON );

	/* Sync SEMI RTC */
	bnx2x_writel ( bnx2x, 0x80000000,
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_CLEAR ) );
	bnx2x_writel ( bnx2x, 0x80000000,
		       ( GRCBASE_MISC + MISC_REGISTERS_RESET_REG_1_SET ) );

	bnx2x_init_block ( bnx2x, BLOCK_UPB, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_XPB, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_PBF, PHASE_COMMON );
	/* (E3 B0: PBF_REG_HDRS_AFTER_BASIC is handled by init tables) */

	bnx2x_writel ( bnx2x, 1, SRC_REG_SOFT_RST );
	bnx2x_init_block ( bnx2x, BLOCK_SRC, PHASE_COMMON );
	bnx2x_writel ( bnx2x, 0, SRC_REG_SOFT_RST );

	bnx2x_init_block ( bnx2x, BLOCK_CDU, PHASE_COMMON );
	/* (4 << 24) + (0 << 12) + 1024: CDU global params */
	bnx2x_writel ( bnx2x, ( ( 4 << 24 ) + ( 0 << 12 ) + 1024 ),
		       CDU_REG_CDU_GLOBAL_PARAMS );

	bnx2x_init_block ( bnx2x, BLOCK_CFC, PHASE_COMMON );
	bnx2x_writel ( bnx2x, 0x7ff, CFC_REG_INIT_REG );
	bnx2x_writel ( bnx2x, 0, CFC_REG_CFC_INT_MASK );
	/* Set the thresholds to prevent CFC/CDU race */
	bnx2x_writel ( bnx2x, 0x20020000, CFC_REG_DEBUG0 );

	bnx2x_init_block ( bnx2x, BLOCK_HC, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_IGU, PHASE_COMMON );
	bnx2x_init_block ( bnx2x, BLOCK_MISC_AEU, PHASE_COMMON );

	/* Reset PCIE errors for debug */
	bnx2x_writel ( bnx2x, 0xffffffff, 0x2814 );
	bnx2x_writel ( bnx2x, 0xffffffff, 0x3820 );

	bnx2x_writel ( bnx2x, ( PXPCS_TL_CONTROL_5_ERR_UNSPPORT1 |
				PXPCS_TL_CONTROL_5_ERR_UNSPPORT ),
		       ( PCICFG_OFFSET + PXPCS_TL_CONTROL_5 ) );
	bnx2x_writel ( bnx2x, ( PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT4 |
				PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT3 |
				PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT2 ),
		       ( PCICFG_OFFSET + PXPCS_TL_FUNC345_STAT ) );
	bnx2x_writel ( bnx2x, ( PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT7 |
				PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT6 |
				PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT5 ),
		       ( PCICFG_OFFSET + PXPCS_TL_FUNC678_STAT ) );

	bnx2x_init_block ( bnx2x, BLOCK_NIG, PHASE_COMMON );
	/* (E3: NIG_REG_LLH_MF_MODE is per-port) */

	/* Finish CFC init */
	val = bnx2x_reg_poll ( bnx2x, CFC_REG_LL_INIT_DONE, 1, 100 );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p CFC LL_INIT failed\n", bnx2x );
		return -EBUSY;
	}
	val = bnx2x_reg_poll ( bnx2x, CFC_REG_AC_INIT_DONE, 1, 100 );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p CFC AC_INIT failed\n", bnx2x );
		return -EBUSY;
	}
	val = bnx2x_reg_poll ( bnx2x, CFC_REG_CAM_INIT_DONE, 1, 100 );
	if ( val != 1 ) {
		DBGC ( bnx2x, "BNX2X %p CFC CAM_INIT failed\n", bnx2x );
		return -EBUSY;
	}
	bnx2x_writel ( bnx2x, 0, CFC_REG_DEBUG0 );

	/* Fan failure detection: only the shared-config "enabled"
	 * setting is honoured; the PHY-type-based detection applies
	 * to PHYs not used on our target boards.  Report if a board
	 * ever asks for it.
	 */
	val = ( bnx2x_shmem_readl ( bnx2x, BNX2X_SHMEM_HW_CONFIG2 ) &
		SHARED_HW_CFG_FAN_FAILURE_MASK );
	if ( val == SHARED_HW_CFG_FAN_FAILURE_ENABLED ) {
		DBGC ( bnx2x, "BNX2X %p WARNING: board requests fan failure "
		       "detection (not implemented)\n", bnx2x );
	}

	/* Clear PXP2 attentions */
	bnx2x_readl ( bnx2x, PXP2_REG_PXP2_INT_STS_CLR_0 );

	bnx2x_enable_blocks_attention ( bnx2x );
	/* (Parity enable deliberately omitted) */

	return 0;
}

/**
 * Initialise hardware at the PORT phase
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_init_hw_port ( struct bnx2x_nic *bnx2x ) {
	unsigned int port = bnx2x->port;
	unsigned int init_phase = ( port ? PHASE_PORT1 : PHASE_PORT0 );
	uint32_t reg;
	uint32_t val;

	DBGC ( bnx2x, "BNX2X %p port init (port %d)\n", bnx2x, port );

	bnx2x_writel ( bnx2x, 0,
		       ( NIG_REG_MASK_INTERRUPT_PORT0 + ( port * 4 ) ) );

	bnx2x_init_block ( bnx2x, BLOCK_MISC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PXP, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PXP2, init_phase );

	/* Timers bug workaround: re-enable master before any DMAE-ish
	 * accesses
	 */
	bnx2x_writel ( bnx2x, 1, PGLUE_B_REG_INTERNAL_PFID_ENABLE_MASTER );

	bnx2x_init_block ( bnx2x, BLOCK_ATC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_DMAE, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PGLUE_B, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_QM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_TCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_UCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XCM, init_phase );

	/* QM connection count */
	bnx2x_writel ( bnx2x, ( ( BNX2X_QM_CID_COUNT / 16 ) - 1 ),
		       ( QM_REG_CONNNUM_0 + ( port * 4 ) ) );

	bnx2x_init_block ( bnx2x, BLOCK_DORQ, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_BRB1, init_phase );

	if ( bnx2x->port4mode ) {
		bnx2x_writel ( bnx2x, 40,
			       ( port ? BRB1_REG_MAC_GUARANTIED_1 :
				 BRB1_REG_MAC_GUARANTIED_0 ) );
	}

	bnx2x_init_block ( bnx2x, BLOCK_PRS, init_phase );
	/* E3 B0, no outer VLAN: L2 headers that may follow ethernet */
	bnx2x_writel ( bnx2x, 6,
		       ( port ? PRS_REG_HDRS_AFTER_BASIC_PORT_1 :
			 PRS_REG_HDRS_AFTER_BASIC_PORT_0 ) );

	bnx2x_init_block ( bnx2x, BLOCK_TSDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CSDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_USDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XSDM, init_phase );

	bnx2x_init_block ( bnx2x, BLOCK_TSEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_USEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CSEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XSEM, init_phase );

	bnx2x_init_block ( bnx2x, BLOCK_UPB, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XPB, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PBF, init_phase );

	bnx2x_init_block ( bnx2x, BLOCK_CDU, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CFC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_HC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_IGU, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_MISC_AEU, init_phase );

	/* AEU attention mask: SF bits 0-2 plus DCBX (bit 4) */
	bnx2x_writel ( bnx2x, ( 0x7 | 0x10 ),
		       ( MISC_REG_AEU_MASK_ATTN_FUNC_0 + ( port * 4 ) ) );

	/* SCPAD_PARITY should NOT trigger close-the-gates */
	reg = ( port ? MISC_REG_AEU_ENABLE4_NIG_1 :
		MISC_REG_AEU_ENABLE4_NIG_0 );
	bnx2x_writel ( bnx2x,
		       ( bnx2x_readl ( bnx2x, reg ) &
			 ~AEU_INPUTS_ATTN_BITS_MCP_LATCHED_SCPAD_PARITY ),
		       reg );
	reg = ( port ? MISC_REG_AEU_ENABLE4_PXP_1 :
		MISC_REG_AEU_ENABLE4_PXP_0 );
	bnx2x_writel ( bnx2x,
		       ( bnx2x_readl ( bnx2x, reg ) &
			 ~AEU_INPUTS_ATTN_BITS_MCP_LATCHED_SCPAD_PARITY ),
		       reg );

	bnx2x_init_block ( bnx2x, BLOCK_NIG, init_phase );

	/* L2 headers after basic ethernet (no outer VLAN in SF) */
	bnx2x_writel ( bnx2x, 6,
		       ( port ? NIG_REG_P1_HDRS_AFTER_BASIC :
			 NIG_REG_P0_HDRS_AFTER_BASIC ) );
	/* E3: LLH MF mode is per-port; single-function here */
	bnx2x_writel ( bnx2x, 0,
		       ( port ? NIG_REG_LLH1_MF_MODE : NIG_REG_LLH_MF_MODE ) );

	/* 0x2: disable mf_ov (single-function) */
	bnx2x_writel ( bnx2x, 0x2,
		       ( NIG_REG_LLH0_BRB1_DRV_MASK_MF + ( port * 4 ) ) );
	/* LLH classification type: 0 in single-function mode */
	bnx2x_writel ( bnx2x, 0,
		       ( port ? NIG_REG_LLH1_CLS_TYPE :
			 NIG_REG_LLH0_CLS_TYPE ) );
	bnx2x_writel ( bnx2x, 0, ( NIG_REG_LLFC_ENABLE_0 + ( port * 4 ) ) );
	bnx2x_writel ( bnx2x, 0, ( NIG_REG_LLFC_OUT_EN_0 + ( port * 4 ) ) );
	bnx2x_writel ( bnx2x, 1, ( NIG_REG_PAUSE_ENABLE_0 + ( port * 4 ) ) );

	/* If SPIO5 is set to generate interrupts, enable it for this
	 * port
	 */
	val = bnx2x_readl ( bnx2x, MISC_REG_SPIO_EVENT_EN );
	if ( val & MISC_SPIO_SPIO5 ) {
		reg = ( port ? MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0 :
			MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0 );
		val = bnx2x_readl ( bnx2x, reg );
		val |= AEU_INPUTS_ATTN_BITS_SPIO5;
		bnx2x_writel ( bnx2x, val, reg );
	}

	return 0;
}

/**
 * Initialise hardware at the FUNCTION phase
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_init_hw_func ( struct bnx2x_nic *bnx2x ) {
	unsigned int func = bnx2x->pfid;
	unsigned int init_phase = ( PHASE_PF0 + func );
	unsigned int dsb_idx;
	unsigned int prod_offset;
	unsigned int sb_idx;
	unsigned int i;

	DBGC ( bnx2x, "BNX2X %p function init (func %d)\n", bnx2x, func );

	/* (Linux performs FLR cleanup here; we always follow a full
	 * common-phase chip reset, after which there is nothing to
	 * clean, so it is omitted.)
	 */

	bnx2x_init_block ( bnx2x, BLOCK_PXP, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PXP2, init_phase );

	/* Program this function's ILT */
	bnx2x_ilt_init_func ( bnx2x );

	/* NIC operation mode (no iSCSI searcher) */
	bnx2x_writel ( bnx2x, 1, PRS_REG_NIC_MODE );

	/* Timers workaround: wait 20ms after ILT update before
	 * enabling master, so no PXP-internal requests hold stale ILT
	 * addresses
	 */
	mdelay ( 20 );
	bnx2x_writel ( bnx2x, 1, PGLUE_B_REG_INTERNAL_PFID_ENABLE_MASTER );
	/* Enable the function in IGU: single ISR mode (polled) */
	bnx2x_writel ( bnx2x, ( IGU_PF_CONF_FUNC_EN |
				IGU_PF_CONF_SINGLE_ISR_EN ),
		       IGU_REG_PF_CONFIGURATION );

	bnx2x_init_block ( bnx2x, BLOCK_PGLUE_B, init_phase );

	/* Clear PGLUE was-error indication for this function */
	bnx2x_writel ( bnx2x, ( 1 << bnx2x->pf_num ),
		       PGLUE_B_REG_WAS_ERROR_PF_7_0_CLR );

	bnx2x_init_block ( bnx2x, BLOCK_ATC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_DMAE, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_NIG, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_SRC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_MISC, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_TCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_UCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XCM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_TSEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_USEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CSEM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XSEM, init_phase );

	bnx2x_writel ( bnx2x, 1, QM_REG_PF_EN );

	/* VF/PF error identification (no VFs: 64 + pfid) */
	bnx2x_writel ( bnx2x, ( 64 + func ), TSEM_REG_VFPF_ERR_NUM );
	bnx2x_writel ( bnx2x, ( 64 + func ), USEM_REG_VFPF_ERR_NUM );
	bnx2x_writel ( bnx2x, ( 64 + func ), CSEM_REG_VFPF_ERR_NUM );
	bnx2x_writel ( bnx2x, ( 64 + func ), XSEM_REG_VFPF_ERR_NUM );

	bnx2x_init_block ( bnx2x, BLOCK_QM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_TM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_DORQ, init_phase );
	/* No doorbell power management */
	bnx2x_writel ( bnx2x, 1, DORQ_REG_MODE_ACT );

	bnx2x_init_block ( bnx2x, BLOCK_BRB1, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PRS, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_TSDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CSDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_USDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XSDM, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_UPB, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_XPB, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_PBF, init_phase );
	bnx2x_writel ( bnx2x, 0, PBF_REG_DISABLE_PF );

	bnx2x_init_block ( bnx2x, BLOCK_CDU, init_phase );
	bnx2x_init_block ( bnx2x, BLOCK_CFC, init_phase );
	bnx2x_writel ( bnx2x, 1, CFC_REG_WEAK_ENABLE_PF );

	bnx2x_init_block ( bnx2x, BLOCK_MISC_AEU, init_phase );

	/* IGU function init */
	bnx2x_writel ( bnx2x, 0,
		       ( MISC_REG_AEU_GENERAL_ATTN_12 + ( func * 4 ) ) );
	bnx2x_writel ( bnx2x, 0, IGU_REG_LEADING_EDGE_LATCH );
	bnx2x_writel ( bnx2x, 0, IGU_REG_TRAILING_EDGE_LATCH );

	bnx2x_init_block ( bnx2x, BLOCK_IGU, init_phase );

	/* Zero the producer memory and clean up all of this
	 * function's status blocks (non-default and default)
	 */
	for ( sb_idx = 0 ; sb_idx < bnx2x->igu_sb_cnt ; sb_idx++ ) {
		prod_offset = ( ( bnx2x->igu_base_sb + sb_idx ) *
				IGU_NORM_NDSB_NUM_SEGS );
		for ( i = 0 ; i < IGU_NORM_NDSB_NUM_SEGS ; i++ ) {
			bnx2x_writel ( bnx2x, 0,
				       ( IGU_REG_PROD_CONS_MEMORY +
					 ( ( prod_offset + i ) * 4 ) ) );
		}
		bnx2x_igu_ack_sb ( bnx2x, ( bnx2x->igu_base_sb + sb_idx ),
				   IGU_SEG_ACCESS_DEF, 0, IGU_INT_NOP, 1 );
		bnx2x_igu_clear_sb ( bnx2x, ( bnx2x->igu_base_sb + sb_idx ) );
	}

	/* Default status block */
	dsb_idx = ( bnx2x->port4mode ? func : ( bnx2x->pfid >> 1 ) );
	prod_offset = ( IGU_NORM_BASE_DSB_PROD + dsb_idx );
	for ( i = 0 ; i < ( IGU_NORM_DSB_NUM_SEGS * 4 ) ; i += 4 ) {
		bnx2x_writel ( bnx2x, 0,
			       ( IGU_REG_PROD_CONS_MEMORY +
				 ( ( prod_offset + i ) * 4 ) ) );
	}
	bnx2x_igu_ack_sb ( bnx2x, bnx2x->igu_dsb_id, IGU_SEG_ACCESS_DEF, 0,
			   IGU_INT_NOP, 1 );
	bnx2x_igu_ack_sb ( bnx2x, bnx2x->igu_dsb_id, IGU_SEG_ACCESS_ATTN, 0,
			   IGU_INT_NOP, 1 );
	bnx2x_igu_clear_sb ( bnx2x, bnx2x->igu_dsb_id );

	bnx2x_writel ( bnx2x, 0, IGU_REG_SB_INT_BEFORE_MASK_LSB );
	bnx2x_writel ( bnx2x, 0, IGU_REG_SB_INT_BEFORE_MASK_MSB );
	bnx2x_writel ( bnx2x, 0, IGU_REG_SB_MASK_LSB );
	bnx2x_writel ( bnx2x, 0, IGU_REG_SB_MASK_MSB );
	bnx2x_writel ( bnx2x, 0, IGU_REG_PBA_STATUS_LSB );
	bnx2x_writel ( bnx2x, 0, IGU_REG_PBA_STATUS_MSB );

	/* Reset PCIE errors for debug */
	bnx2x_writel ( bnx2x, 0xffffffff, 0x2114 );
	bnx2x_writel ( bnx2x, 0xffffffff, 0x2120 );

	return 0;
}

/**
 * Initialise and enable the XMAC (E3 10/20G MAC)
 *
 * @v bnx2x		bnx2x device
 * @v mac		MAC address (for pause frame source address)
 *
 * Ported from Linux bnx2x_link.c bnx2x_xmac_init() /
 * bnx2x_xmac_enable() / bnx2x_update_pfc_xmac() /
 * bnx2x_set_xumac_nig(), for the MFW-maintained 10G link case with
 * flow control and PFC disabled.  The PHY itself is never touched.
 */
void bnx2x_xmac_enable ( struct bnx2x_nic *bnx2x, const uint8_t *mac ) {
	uint32_t xmac_base = ( bnx2x->port ? GRCBASE_XMAC1 : GRCBASE_XMAC0 );
	unsigned int chip_num = BNX2X_CHIP_NUM ( bnx2x->chip_id );
	int is_57840 = ( ( chip_num == BNX2X_CHIP_NUM_57840_4_10 ) ||
			 ( chip_num == BNX2X_CHIP_NUM_57840_2_20 ) ||
			 ( chip_num == BNX2X_CHIP_NUM_57840_OBSOLETE ) );

	/* In 4-port mode the XMAC block is shared by both ports of
	 * the path: if it is already out of reset, the mode has been
	 * set and it must not be reset again
	 */
	if ( ! ( is_57840 && bnx2x->port4mode &&
		 ( bnx2x_readl ( bnx2x, MISC_REG_RESET_REG_2 ) &
		   MISC_REGISTERS_RESET_REG_2_XMAC ) ) ) {

		/* Hard reset */
		bnx2x_writel ( bnx2x, MISC_REGISTERS_RESET_REG_2_XMAC,
			       ( GRCBASE_MISC +
				 MISC_REGISTERS_RESET_REG_2_CLEAR ) );
		mdelay ( 1 );
		bnx2x_writel ( bnx2x, MISC_REGISTERS_RESET_REG_2_XMAC,
			       ( GRCBASE_MISC +
				 MISC_REGISTERS_RESET_REG_2_SET ) );

		if ( bnx2x->port4mode ) {
			/* Two ports per path, Warp Core in 10G mode */
			bnx2x_writel ( bnx2x, 1,
				       MISC_REG_XMAC_CORE_PORT_MODE );
			bnx2x_writel ( bnx2x, 3,
				       MISC_REG_XMAC_PHY_PORT_MODE );
		} else {
			/* One port per path at 10G */
			bnx2x_writel ( bnx2x, 0,
				       MISC_REG_XMAC_CORE_PORT_MODE );
			bnx2x_writel ( bnx2x, 3,
				       MISC_REG_XMAC_PHY_PORT_MODE );
		}

		/* Soft reset */
		bnx2x_writel ( bnx2x, MISC_REGISTERS_RESET_REG_2_XMAC_SOFT,
			       ( GRCBASE_MISC +
				 MISC_REGISTERS_RESET_REG_2_CLEAR ) );
		mdelay ( 1 );
		bnx2x_writel ( bnx2x, MISC_REGISTERS_RESET_REG_2_XMAC_SOFT,
			       ( GRCBASE_MISC +
				 MISC_REGISTERS_RESET_REG_2_SET ) );
	}

	/* Route NIG egress traffic to the XMAC (not the UMAC) */
	bnx2x_writel ( bnx2x, 0,
		       ( NIG_REG_EGRESS_EMAC0_PORT + ( bnx2x->port * 4 ) ) );

	/* Disable idle-based fault detection and clear latched fault
	 * state (we do not manage the warpcore, so mirror the Linux
	 * !FLAGS_TX_ERROR_CHECK path)
	 */
	bnx2x_writel ( bnx2x, ( XMAC_RX_LSS_CTRL_REG_LOCAL_FAULT_DISABLE |
				XMAC_RX_LSS_CTRL_REG_REMOTE_FAULT_DISABLE ),
		       ( xmac_base + XMAC_REG_RX_LSS_CTRL ) );
	bnx2x_writel ( bnx2x, 0,
		       ( xmac_base + XMAC_REG_CLEAR_RX_LSS_STATUS ) );
	bnx2x_writel ( bnx2x, 0x3,
		       ( xmac_base + XMAC_REG_CLEAR_RX_LSS_STATUS ) );

	/* Maximum RX packet size */
	bnx2x_writel ( bnx2x, 0x2710, ( xmac_base + XMAC_REG_RX_MAX_SIZE ) );

	/* CRC append for TX packets */
	bnx2x_writel ( bnx2x, 0xc800, ( xmac_base + XMAC_REG_TX_CTRL ) );

	/* Pause and PFC configuration: both disabled */
	bnx2x_writel ( bnx2x, 0x18000, ( xmac_base + XMAC_REG_PAUSE_CTRL ) );
	bnx2x_writel ( bnx2x, 0xffff8000, ( xmac_base + XMAC_REG_PFC_CTRL ) );
	bnx2x_writel ( bnx2x, 0x2, ( xmac_base + XMAC_REG_PFC_CTRL_HI ) );

	/* Source MAC for pause frames */
	bnx2x_writel ( bnx2x, ( ( mac[2] << 24 ) | ( mac[3] << 16 ) |
				( mac[4] << 8 ) | mac[5] ),
		       ( xmac_base + XMAC_REG_CTRL_SA_LO ) );
	bnx2x_writel ( bnx2x, ( ( mac[0] << 8 ) | mac[1] ),
		       ( xmac_base + XMAC_REG_CTRL_SA_HI ) );

	/* No EEE */
	bnx2x_writel ( bnx2x, 0, ( xmac_base + XMAC_REG_EEE_CTRL ) );

	/* Enable TX and RX */
	bnx2x_writel ( bnx2x, ( XMAC_CTRL_REG_TX_EN | XMAC_CTRL_REG_RX_EN ),
		       ( xmac_base + XMAC_REG_CTRL ) );

	/* Open the NIG-to-MAC gates (no pause output) */
	bnx2x_writel ( bnx2x, 1, ( bnx2x->port ? NIG_REG_P1_MAC_IN_EN :
				   NIG_REG_P0_MAC_IN_EN ) );
	bnx2x_writel ( bnx2x, 1, ( bnx2x->port ? NIG_REG_P1_MAC_OUT_EN :
				   NIG_REG_P0_MAC_OUT_EN ) );
	bnx2x_writel ( bnx2x, 0, ( bnx2x->port ? NIG_REG_P1_MAC_PAUSE_OUT_EN :
				   NIG_REG_P0_MAC_PAUSE_OUT_EN ) );

	DBGC ( bnx2x, "BNX2X %p XMAC enabled (port %d, %d-port mode)\n",
	       bnx2x, bnx2x->port, ( bnx2x->port4mode ? 4 : 2 ) );
}

/**
 * Allocate hardware init memory (CDU context, QM pages)
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_hw_alloc ( struct bnx2x_nic *bnx2x ) {

	bnx2x->cdu_context = malloc_phys ( BNX2X_CDU_PAGE_SIZE,
					   BNX2X_CDU_PAGE_SIZE );
	if ( ! bnx2x->cdu_context )
		goto err_cdu;
	memset ( bnx2x->cdu_context, 0, BNX2X_CDU_PAGE_SIZE );

	bnx2x->qm_mem = malloc_phys ( ( BNX2X_QM_PAGES *
					BNX2X_QM_PAGE_SIZE ),
				      BNX2X_QM_PAGE_SIZE );
	if ( ! bnx2x->qm_mem )
		goto err_qm;
	memset ( bnx2x->qm_mem, 0, ( BNX2X_QM_PAGES * BNX2X_QM_PAGE_SIZE ) );

	return 0;

 err_qm:
	free_phys ( bnx2x->cdu_context, BNX2X_CDU_PAGE_SIZE );
	bnx2x->cdu_context = NULL;
 err_cdu:
	return -ENOMEM;
}

/**
 * Free hardware init memory
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_hw_free ( struct bnx2x_nic *bnx2x ) {

	if ( bnx2x->qm_mem ) {
		free_phys ( bnx2x->qm_mem,
			    ( BNX2X_QM_PAGES * BNX2X_QM_PAGE_SIZE ) );
		bnx2x->qm_mem = NULL;
	}
	if ( bnx2x->cdu_context ) {
		free_phys ( bnx2x->cdu_context, BNX2X_CDU_PAGE_SIZE );
		bnx2x->cdu_context = NULL;
	}
}

/**
 * Initialise hardware according to the MCP load level
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
int bnx2x_hw_init ( struct bnx2x_nic *bnx2x ) {
	int rc;

	/* Allocate function memories and decompression buffer */
	if ( ( rc = bnx2x_hw_alloc ( bnx2x ) ) != 0 )
		return rc;
	if ( ( rc = bnx2x_gunzip_init ( bnx2x ) ) != 0 )
		goto err_gunzip;

	/* Run the applicable phases */
	switch ( bnx2x->load_code ) {
	case FW_MSG_CODE_DRV_LOAD_COMMON_CHIP:
	case FW_MSG_CODE_DRV_LOAD_COMMON:
		if ( ( rc = bnx2x_init_hw_common ( bnx2x ) ) != 0 )
			goto err_init;
		/* fall through */
	case FW_MSG_CODE_DRV_LOAD_PORT:
		if ( ( rc = bnx2x_init_hw_port ( bnx2x ) ) != 0 )
			goto err_init;
		/* fall through */
	case FW_MSG_CODE_DRV_LOAD_FUNCTION:
		if ( ( rc = bnx2x_init_hw_func ( bnx2x ) ) != 0 )
			goto err_init;
		break;
	default:
		DBGC ( bnx2x, "BNX2X %p unknown load code %08x\n",
		       bnx2x, bnx2x->load_code );
		rc = -EINVAL;
		goto err_init;
	}

	bnx2x_gunzip_end ( bnx2x );
	DBGC ( bnx2x, "BNX2X %p hardware initialised\n", bnx2x );
	return 0;

 err_init:
	bnx2x_gunzip_end ( bnx2x );
 err_gunzip:
	bnx2x_hw_free ( bnx2x );
	return rc;
}
