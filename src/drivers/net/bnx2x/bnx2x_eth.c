/*
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Ethernet datapath: fastpath status block, client setup, MAC
 * classification, RX/TX rings and polling.
 *
 * Ported from the Linux bnx2x driver,
 * drivers/net/ethernet/broadcom/bnx2x/ (Linux v6.6): bnx2x_init_sb,
 * bnx2x_q_fill_init_* / bnx2x_set_one_mac_e2 / rx-mode fills from
 * bnx2x_sp.c, ring management from bnx2x_cmn.c/h, context validation
 * from bnx2x_cmn.c:
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
 * All structure layouts were computed against the Linux v6.6
 * bnx2x_hsi.h with a host offsetof tool (see CLAUDE.md).
 */

FILE_LICENCE ( GPL2_ONLY );
FILE_SECBOOT ( PERMITTED );

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <byteswap.h>
#include <ipxe/timer.h>
#include <ipxe/malloc.h>
#include <ipxe/iobuf.h>
#include <ipxe/netdevice.h>
#include <ipxe/ethernet.h>
#include <ipxe/if_ether.h>
#include "bnx2x.h"
#include "bnx2x_init.h"
#include "bnx2x_hw.h"
#include "bnx2x_sp.h"
#include "bnx2x_eth.h"

/** Leading connection id */
#define BNX2X_ETH_CID		0

/** RX buffer size handed to the firmware */
#define BNX2X_RX_BUF_SIZE	2048

/** RX buffer allocation size (headroom for placement offset) */
#define BNX2X_RX_IOB_SIZE	( BNX2X_RX_BUF_SIZE + 128 )

/** Number of RX buffers kept posted */
#define BNX2X_RX_FILL		8

/** Ring geometry (single page each) */
#define BNX2X_RX_BD_CNT		512	/* 8-byte BDs; last 2 next-page */
#define BNX2X_RX_BD_USABLE	( BNX2X_RX_BD_CNT - 2 )
#define BNX2X_RCQ_CNT		64	/* 64-byte CQEs; last 1 next-page */
#define BNX2X_RCQ_USABLE	( BNX2X_RCQ_CNT - 1 )
#define BNX2X_TX_BD_CNT		256	/* 16-byte BDs; last 1 next-page */
#define BNX2X_TX_BD_USABLE	( BNX2X_TX_BD_CNT - 1 )

/** Client (function-relative) id: BP_L_ID = vn << 2 */
#define BNX2X_CL_ID( bnx2x )	( ( (bnx2x)->pfid >> 1 ) << 2 )

/**
 * Advance an RX BD producer/consumer index, skipping next-page slots
 */
static unsigned int bnx2x_next_rx_idx ( unsigned int idx ) {

	return ( ( ( idx & ( BNX2X_RX_BD_CNT - 1 ) ) ==
		   ( BNX2X_RX_BD_USABLE - 1 ) ) ? ( idx + 3 ) : ( idx + 1 ) );
}

/**
 * Advance an RX CQ index, skipping the next-page slot
 */
static unsigned int bnx2x_next_rcq_idx ( unsigned int idx ) {

	return ( ( ( idx & ( BNX2X_RCQ_USABLE ) ) ==
		   ( BNX2X_RCQ_USABLE - 1 ) ) ? ( idx + 2 ) : ( idx + 1 ) );
}

/**
 * Advance a TX BD index, skipping the next-page slot
 */
static unsigned int bnx2x_next_tx_idx ( unsigned int idx ) {

	return ( ( ( idx & ( BNX2X_TX_BD_USABLE ) ) ==
		   ( BNX2X_TX_BD_USABLE - 1 ) ) ? ( idx + 2 ) : ( idx + 1 ) );
}

/**
 * CRC-8 for CDU context validation (ported verbatim from Linux
 * bnx2x_reg.h calc_crc8())
 */
static uint8_t bnx2x_calc_crc8 ( uint32_t data, uint8_t crc ) {
	uint8_t D[32];
	uint8_t NewCRC[8];
	uint8_t C[8];
	uint8_t crc_res;
	uint8_t i;

	for ( i = 0 ; i < 32 ; i++ ) {
		D[i] = ( uint8_t ) ( data & 1 );
		data = ( data >> 1 );
	}
	for ( i = 0 ; i < 8 ; i++ ) {
		C[i] = ( crc & 1 );
		crc = ( crc >> 1 );
	}

	NewCRC[0] = D[31] ^ D[30] ^ D[28] ^ D[23] ^ D[21] ^ D[19] ^ D[18] ^
		    D[16] ^ D[14] ^ D[12] ^ D[8] ^ D[7] ^ D[6] ^ D[0] ^ C[4] ^
		    C[6] ^ C[7];
	NewCRC[1] = D[30] ^ D[29] ^ D[28] ^ D[24] ^ D[23] ^ D[22] ^ D[21] ^
		    D[20] ^ D[18] ^ D[17] ^ D[16] ^ D[15] ^ D[14] ^ D[13] ^
		    D[12] ^ D[9] ^ D[6] ^ D[1] ^ D[0] ^ C[0] ^ C[4] ^ C[5] ^
		    C[6];
	NewCRC[2] = D[29] ^ D[28] ^ D[25] ^ D[24] ^ D[22] ^ D[17] ^ D[15] ^
		    D[13] ^ D[12] ^ D[10] ^ D[8] ^ D[6] ^ D[2] ^ D[1] ^ D[0] ^
		    C[0] ^ C[1] ^ C[4] ^ C[5];
	NewCRC[3] = D[30] ^ D[29] ^ D[26] ^ D[25] ^ D[23] ^ D[18] ^ D[16] ^
		    D[14] ^ D[13] ^ D[11] ^ D[9] ^ D[7] ^ D[3] ^ D[2] ^ D[1] ^
		    C[1] ^ C[2] ^ C[5] ^ C[6];
	NewCRC[4] = D[31] ^ D[30] ^ D[27] ^ D[26] ^ D[24] ^ D[19] ^ D[17] ^
		    D[15] ^ D[14] ^ D[12] ^ D[10] ^ D[8] ^ D[4] ^ D[3] ^
		    D[2] ^ C[0] ^ C[2] ^ C[3] ^ C[6] ^ C[7];
	NewCRC[5] = D[31] ^ D[28] ^ D[27] ^ D[25] ^ D[20] ^ D[18] ^ D[16] ^
		    D[15] ^ D[13] ^ D[11] ^ D[9] ^ D[5] ^ D[4] ^ D[3] ^ C[1] ^
		    C[3] ^ C[4] ^ C[7];
	NewCRC[6] = D[29] ^ D[28] ^ D[26] ^ D[21] ^ D[19] ^ D[17] ^ D[16] ^
		    D[14] ^ D[12] ^ D[10] ^ D[6] ^ D[5] ^ D[4] ^ C[2] ^ C[4] ^
		    C[5];
	NewCRC[7] = D[30] ^ D[29] ^ D[27] ^ D[22] ^ D[20] ^ D[18] ^ D[17] ^
		    D[15] ^ D[13] ^ D[11] ^ D[7] ^ D[6] ^ D[5] ^ C[3] ^ C[5] ^
		    C[6];

	crc_res = 0;
	for ( i = 0 ; i < 8 ; i++ )
		crc_res |= ( NewCRC[i] << i );

	return crc_res;
}

/**
 * Set CDU context validation bytes for the leading connection
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_set_ctx_validation ( struct bnx2x_nic *bnx2x ) {
	uint8_t *cxt = bnx2x->cdu_context;	/* cid 0: first 1kB entry */
	uint32_t hw_cid = ( ( bnx2x->port << 23 ) |
			    ( ( bnx2x->pfid >> 1 ) << 17 ) | BNX2X_ETH_CID );
	uint32_t valid_data;

	/* CDU_RSRVD_VALUE_TYPE_A(HW_CID, region, ETH_CONNECTION_TYPE):
	 * ustorm: region 4 (UCM_AG); xstorm: region 2 (XCM_AG)
	 */
	valid_data = ( ( hw_cid << 8 ) | ( ( 4 & 0xf ) << 4 ) |
		       ( ETH_CONNECTION_TYPE & 0xf ) );
	cxt[0x227] = ( 0x80 | ( bnx2x_calc_crc8 ( valid_data, 0xff ) &
				0x7f ) );
	valid_data = ( ( hw_cid << 8 ) | ( ( 2 & 0xf ) << 4 ) |
		       ( ETH_CONNECTION_TYPE & 0xf ) );
	cxt[0x147] = ( 0x80 | ( bnx2x_calc_crc8 ( valid_data, 0xff ) &
				0x7f ) );
}

/**
 * Initialise the fastpath status block
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_init_fp_sb ( struct bnx2x_nic *bnx2x ) {
	physaddr_t sb_phys = virt_to_bus ( bnx2x->fp_sb );
	unsigned int sb = bnx2x->igu_base_sb;	/* fw_sb_id == igu_sb_id */
	uint32_t sb_data[16];

	/* Disable and zero the status block */
	memset ( sb_data, 0, sizeof ( sb_data ) );
	sb_data[10] = 0xff0000;	/* p_func.vf_id = 0xff */
	bnx2x_storm_memcpy ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 137, sb, 0 ),
			     sb_data, sizeof ( sb_data ) );
	bnx2x_storm_fill ( bnx2x, BAR_CSTRORM_INTMEM,
			   bnx2x_iro_offset ( 136, sb, 0 ),
			   bnx2x_iro ( 136 )->size, 0 );
	bnx2x_storm_fill ( bnx2x, BAR_CSTRORM_INTMEM,
			   bnx2x_iro_offset ( 141, sb, 0 ),
			   bnx2x_iro ( 141 )->size, 0 );

	/* hc_status_block_data_e2:
	 * words 0-3: hc_index_data[8] {u8 timeout, u8 flags}:
	 *   index 1 (RX CQ): SM_RX(0), HC_ENABLED
	 *   index 5 (TX CQ): SM_TX(1), HC_ENABLED
	 * words 4-15: hc_sb_data: host_sb_addr lo/hi,
	 *   state_machine[2] {flags,timer 0xff,igu_sb,seg NORM(0)} +
	 *   time_to_expire 0xffffffff, p_func, same_igu_sb_1b/state
	 */
	memset ( sb_data, 0, sizeof ( sb_data ) );
	sb_data[0] = ( 0x02 << 24 );		/* index1.flags: HC_EN|SM0 */
	sb_data[2] = ( 0x03 << 24 );		/* index5.flags: HC_EN|SM1 */
	sb_data[4] = ( sb_phys & 0xffffffffUL );
	sb_data[5] = ( ( ( uint64_t ) sb_phys ) >> 32 );
	sb_data[6] = ( ( 0xff << 8 ) | ( sb << 16 ) );	/* RX SM */
	sb_data[7] = 0xffffffff;
	sb_data[8] = ( ( 0xff << 8 ) | ( sb << 16 ) );	/* TX SM */
	sb_data[9] = 0xffffffff;
	sb_data[10] = ( bnx2x->pfid | ( ( bnx2x->pfid >> 1 ) << 8 ) |
			( 0xff << 16 ) );	/* pf, vnic, vf_id 0xff */
	sb_data[11] = ( 1 | ( 1 << 16 ) );	/* same_igu_sb_1b, ENABLED */
	bnx2x_storm_memcpy ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 137, sb, 0 ),
			     sb_data, sizeof ( sb_data ) );
}

/**
 * Write the RX producers to USTORM
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_update_rx_prods ( struct bnx2x_nic *bnx2x ) {
	uint32_t prods[2];

	/* struct ustorm_eth_rx_producers (LE): {u16 cqe_prod, u16
	 * bd_prod}, {u16 sge_prod, u16 reserved}
	 */
	prods[0] = ( ( bnx2x->rx_cq_prod & 0xffff ) |
		     ( ( bnx2x->rx_bd_prod & 0xffff ) << 16 ) );
	prods[1] = 0;
	wmb();
	bnx2x_storm_memcpy ( bnx2x, BAR_USTRORM_INTMEM,
			     bnx2x_iro_offset ( 217, BNX2X_CL_ID ( bnx2x ),
						0 ),
			     prods, sizeof ( prods ) );
}

/**
 * Post an RX buffer
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_post_rx_buffer ( struct bnx2x_nic *bnx2x ) {
	struct io_buffer *iobuf;
	unsigned int slot = ( bnx2x->rx_bd_prod & ( BNX2X_RX_BD_CNT - 1 ) );
	uint32_t *bd = ( bnx2x->rx_bd_ring + ( slot * 8 ) );
	physaddr_t phys;

	iobuf = alloc_iob ( BNX2X_RX_IOB_SIZE );
	if ( ! iobuf )
		return -ENOMEM;
	phys = virt_to_bus ( iobuf->data );
	bd[0] = cpu_to_le32 ( phys & 0xffffffffUL );
	bd[1] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );

	bnx2x->rx_iobuf[bnx2x->rx_ring_head % BNX2X_RX_FILL] = iobuf;
	bnx2x->rx_ring_head++;
	bnx2x->rx_bd_prod = bnx2x_next_rx_idx ( bnx2x->rx_bd_prod );
	bnx2x->rx_cq_prod = bnx2x_next_rcq_idx ( bnx2x->rx_cq_prod );

	return 0;
}

/**
 * Wait for a ramrod completion CQE on the RX completion queue
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_wait_ramrod_cqe ( struct bnx2x_nic *bnx2x ) {
	uint16_t *rx_cons_sb = ( bnx2x->fp_sb + ( 1 * 2 ) ); /* index 1 */
	uint32_t *cqe;
	unsigned int hw_cons;
	unsigned int slot;
	unsigned int i;

	for ( i = 0 ; i < 5000 ; i++ ) {
		hw_cons = le16_to_cpu ( *rx_cons_sb );
		if ( ( hw_cons & BNX2X_RCQ_USABLE ) == BNX2X_RCQ_USABLE )
			hw_cons++;
		if ( hw_cons != ( bnx2x->rx_cq_cons & 0xffff ) )
			break;
		mdelay ( 1 );
	}
	if ( hw_cons == ( bnx2x->rx_cq_cons & 0xffff ) ) {
		DBGC ( bnx2x, "BNX2X %p ramrod CQE timeout\n", bnx2x );
		return -ETIMEDOUT;
	}

	slot = ( bnx2x->rx_cq_cons & ( BNX2X_RCQ_CNT - 1 ) );
	cqe = ( bnx2x->rx_cq_ring + ( slot * 64 ) );
	DBGC2 ( bnx2x, "BNX2X %p ramrod CQE flags %02x\n", bnx2x,
		( cqe[0] & 0xff ) );
	if ( ( cqe[0] & 0x3 ) != 1 ) {	/* RX_ETH_CQE_TYPE_ETH_RAMROD */
		DBGC ( bnx2x, "BNX2X %p unexpected CQE type %02x\n",
		       bnx2x, ( cqe[0] & 0xff ) );
	}
	memset ( cqe, 0, 64 );
	bnx2x->rx_cq_cons = bnx2x_next_rcq_idx ( bnx2x->rx_cq_cons );
	bnx2x->rx_cq_prod = bnx2x_next_rcq_idx ( bnx2x->rx_cq_prod );
	bnx2x_update_rx_prods ( bnx2x );

	return 0;
}

/**
 * Send the CLIENT_SETUP ramrod
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_client_setup ( struct bnx2x_nic *bnx2x ) {
	uint8_t *d = bnx2x->sp_data;
	unsigned int cl_id = BNX2X_CL_ID ( bnx2x );
	unsigned int sb = bnx2x->igu_base_sb;
	physaddr_t bd_phys = virt_to_bus ( bnx2x->rx_bd_ring );
	physaddr_t cq_phys = virt_to_bus ( bnx2x->rx_cq_ring );
	physaddr_t tx_phys = virt_to_bus ( bnx2x->tx_ring );

	/* client_init_ramrod_data: general @0x00, rx @0x10, tx @0x60 */
	memset ( d, 0, 0x78 );

	/* general */
	d[0x00] = cl_id;			/* client_id */
	d[0x01] = 0;		/* statistics_counter_id (disabled=0) */
	d[0x02] = 0;				/* statistics_en_flg */
	d[0x04] = 1;				/* activate_flg */
	d[0x05] = cl_id;			/* sp_client_id */
	d[0x06] = ( ETH_MAX_MTU & 0xff );	/* mtu (le16) = 1500 */
	d[0x07] = ( ETH_MAX_MTU >> 8 );
	d[0x09] = bnx2x->pfid;			/* func_id */
	d[0x0c] = 2;				/* fp_hsi_ver (VER_2) */

	/* rx */
	d[0x13] = 6;		/* cache_line_alignment_log_size (64) */
	d[0x16] = cl_id;			/* client_qzone_id */
	d[0x1b] = 0;			/* inner_vlan_removal off (VLANs!) */
	d[0x1d] = sb;				/* status_block_id */
	d[0x1e] = 1;			/* rx_sb_index_number (RX_CQ_CONS) */
	d[0x1f] = 1;		/* dont_verify_rings_pause_thr_flg */
	d[0x22] = ( BNX2X_RX_BUF_SIZE & 0xff );	/* max_bytes_on_bd le16 */
	d[0x23] = ( BNX2X_RX_BUF_SIZE >> 8 );
	/* bd_page_base @0x28 (regpair lo/hi) */
	* ( ( uint32_t * ) &d[0x28] ) = cpu_to_le32 ( bd_phys & 0xffffffffUL );
	* ( ( uint32_t * ) &d[0x2c] ) =
		cpu_to_le32 ( ( ( uint64_t ) bd_phys ) >> 32 );
	/* cqe_page_base @0x38 */
	* ( ( uint32_t * ) &d[0x38] ) = cpu_to_le32 ( cq_phys & 0xffffffffUL );
	* ( ( uint32_t * ) &d[0x3c] ) =
		cpu_to_le32 ( ( ( uint64_t ) cq_phys ) >> 32 );
	d[0x40] = 1;				/* is_leading_rss */
	/* state @0x44 (le16): start in DROP_ALL (UCAST|MCAST) as Linux */
	d[0x44] = ( 1 | ( 1 << 3 ) );

	/* tx @0x60 */
	d[0x61] = sb;				/* tx_status_block_id */
	d[0x62] = 5;		/* tx_sb_index_number (TX_CQ_CONS_COS0) */
	d[0x63] = cl_id;			/* tss_leading_client_id */
	/* tx_bd_page_base @0x68 */
	* ( ( uint32_t * ) &d[0x68] ) = cpu_to_le32 ( tx_phys & 0xffffffffUL );
	* ( ( uint32_t * ) &d[0x6c] ) =
		cpu_to_le32 ( ( ( uint64_t ) tx_phys ) >> 32 );
	wmb();

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_CLIENT_SETUP, BNX2X_ETH_CID,
			virt_to_bus ( d ), ETH_CONNECTION_TYPE );
	return bnx2x_wait_ramrod_cqe ( bnx2x );
}

/**
 * Add our MAC address via the CLASSIFICATION_RULES ramrod
 *
 * @v bnx2x		bnx2x device
 * @v netdev		Network device (for current MAC)
 * @ret rc		Return status code
 */
static int bnx2x_set_mac ( struct bnx2x_nic *bnx2x,
			   struct net_device *netdev ) {
	uint8_t *d = bnx2x->sp_data;
	uint8_t *mac = netdev->ll_addr;

	/* eth_classify_rules_ramrod_data: header {rule_cnt @0x00,
	 * echo @0x04}, rules[0] @0x08 (mac rule 0x10 bytes):
	 * header {cmd_general_data, func_id, client_id}, reserved,
	 * inner_mac, mac_lsb, mac_mid, mac_msb
	 */
	memset ( d, 0, 0x18 );
	d[0x00] = 1;				/* rule_cnt */
	/* rule header: RX_CMD|TX_CMD|IS_ADD, opcode MAC (0) */
	d[0x08] = ( 0x01 | 0x02 | 0x10 );
	d[0x09] = bnx2x->pfid;			/* func_id */
	d[0x0a] = BNX2X_CL_ID ( bnx2x );	/* client_id */
	/* mac_lsb/mid/msb (le16 each) */
	d[0x10] = mac[5];
	d[0x11] = mac[4];
	d[0x12] = mac[3];
	d[0x13] = mac[2];
	d[0x14] = mac[1];
	d[0x15] = mac[0];
	wmb();

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_CLASSIFICATION_RULES,
			BNX2X_ETH_CID, virt_to_bus ( d ),
			ETH_CONNECTION_TYPE );
	return bnx2x_sp_wait_comp ( bnx2x,
				    EVENT_RING_OPCODE_CLASSIFICATION_RULES );
}

/**
 * Set the RX filter mode via the FILTER_RULES ramrod
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_set_rx_mode ( struct bnx2x_nic *bnx2x ) {
	uint8_t *d = bnx2x->sp_data;
	unsigned int i;

	/* eth_filter_rules_ramrod_data: header {rule_cnt @0x00, echo
	 * @0x04}, rules[] @0x08, each 0x10: {cmd_general_data,
	 * func_id, client_id, rsv, state le16 @0x04, ...}.
	 * Two rules: RX path and TX path.  State: accept matched
	 * unicast + all-multicast + broadcast + any VLAN.
	 */
	memset ( d, 0, 0x28 );
	d[0x00] = 2;				/* rule_cnt */
	for ( i = 0 ; i < 2 ; i++ ) {
		uint8_t *rule = &d[ 0x08 + ( i * 0x10 ) ];
		rule[0x00] = ( i ? 0x02 : 0x01 );	/* TX_CMD : RX_CMD */
		rule[0x01] = bnx2x->pfid;
		rule[0x02] = BNX2X_CL_ID ( bnx2x );
		/* MCAST_ACCEPT_ALL | BCAST_ACCEPT_ALL | ACCEPT_ANY_VLAN */
		rule[0x04] = ( ( 1 << 4 ) | ( 1 << 5 ) | ( 1 << 6 ) );
	}
	wmb();

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_FILTER_RULES, BNX2X_ETH_CID,
			virt_to_bus ( d ), ETH_CONNECTION_TYPE );
	return bnx2x_sp_wait_comp ( bnx2x, EVENT_RING_OPCODE_FILTERS_RULES );
}

/**
 * Open the Ethernet datapath
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
int bnx2x_eth_open ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;
	uint32_t *next;
	physaddr_t phys;
	unsigned int i;
	int rc;

	/* Allocate rings and fastpath status block */
	bnx2x->fp_sb = malloc_phys ( 0x40, 0x40 );
	bnx2x->rx_bd_ring = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	bnx2x->rx_cq_ring = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	bnx2x->tx_ring = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	if ( ! ( bnx2x->fp_sb && bnx2x->rx_bd_ring && bnx2x->rx_cq_ring &&
		 bnx2x->tx_ring ) ) {
		rc = -ENOMEM;
		goto err;
	}
	memset ( bnx2x->fp_sb, 0, 0x40 );
	memset ( bnx2x->rx_bd_ring, 0, BNX2X_PAGE_SIZE );
	memset ( bnx2x->rx_cq_ring, 0, BNX2X_PAGE_SIZE );
	memset ( bnx2x->tx_ring, 0, BNX2X_PAGE_SIZE );
	memset ( bnx2x->rx_iobuf, 0, sizeof ( bnx2x->rx_iobuf ) );
	memset ( bnx2x->tx_iobuf, 0, sizeof ( bnx2x->tx_iobuf ) );
	bnx2x->rx_bd_prod = 0;
	bnx2x->rx_cq_prod = 0;
	bnx2x->rx_cq_cons = 0;
	bnx2x->rx_ring_head = 0;
	bnx2x->rx_ring_tail = 0;
	bnx2x->tx_bd_prod = 0;
	bnx2x->tx_pkt_prod = 0;
	bnx2x->tx_pkt_cons = 0;
	bnx2x->tx_db_prod = 0;

	/* Next-page pointers (rings loop back to themselves) */
	phys = virt_to_bus ( bnx2x->rx_bd_ring );
	next = ( bnx2x->rx_bd_ring + ( BNX2X_RX_BD_USABLE * 8 ) );
	next[0] = cpu_to_le32 ( phys & 0xffffffffUL );		/* addr_lo */
	next[1] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );
	next[2] = cpu_to_le32 ( phys & 0xffffffffUL );
	next[3] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );
	phys = virt_to_bus ( bnx2x->rx_cq_ring );
	next = ( bnx2x->rx_cq_ring + ( BNX2X_RCQ_USABLE * 64 ) );
	next[0] = cpu_to_le32 ( phys & 0xffffffffUL );		/* addr_lo */
	next[1] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );
	phys = virt_to_bus ( bnx2x->tx_ring );
	next = ( bnx2x->tx_ring + ( BNX2X_TX_BD_USABLE * 16 ) );
	next[0] = cpu_to_le32 ( phys & 0xffffffffUL );		/* addr_lo */
	next[1] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );

	/* Initialise fastpath status block and context */
	bnx2x_init_fp_sb ( bnx2x );
	bnx2x_set_ctx_validation ( bnx2x );

	/* Post RX buffers and publish producers before creating the
	 * client (mirrors the Linux ordering: the client-setup
	 * completion arrives as a CQE on this very queue)
	 */
	for ( i = 0 ; i < BNX2X_RX_FILL ; i++ ) {
		if ( ( rc = bnx2x_post_rx_buffer ( bnx2x ) ) != 0 )
			goto err;
	}
	bnx2x_update_rx_prods ( bnx2x );

	/* Set up the client */
	if ( ( rc = bnx2x_client_setup ( bnx2x ) ) != 0 )
		goto err;
	DBGC ( bnx2x, "BNX2X %p client ready\n", bnx2x );

	/* Program MAC address and RX filters */
	if ( ( rc = bnx2x_set_mac ( bnx2x, netdev ) ) != 0 )
		goto err_halt;
	if ( ( rc = bnx2x_set_rx_mode ( bnx2x ) ) != 0 )
		goto err_halt;

	/* Bring up the MAC (the MFW maintains the PHY link, but the
	 * MAC block is always the driver's responsibility)
	 */
	bnx2x_xmac_enable ( bnx2x, netdev->ll_addr );

	/* Inject NIG loopback debug packets: tests NIG->BRB->PRS->
	 * storm placement with no wire traffic (results in the LBTEST
	 * debug line; the packets may also surface as RX completions)
	 */
	bnx2x_lb_test ( bnx2x );

	DBGC ( bnx2x, "BNX2X %p datapath up\n", bnx2x );
	return 0;

 err_halt:
	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_HALT, BNX2X_ETH_CID, 0,
			ETH_CONNECTION_TYPE );
	bnx2x_wait_ramrod_cqe ( bnx2x );
 err:
	bnx2x_eth_free ( bnx2x );
	return rc;
}

/**
 * Free datapath memory (buffers and rings)
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_eth_free ( struct bnx2x_nic *bnx2x ) {
	unsigned int i;

	for ( i = 0 ; i < BNX2X_RX_FILL ; i++ ) {
		if ( bnx2x->rx_iobuf[i] ) {
			free_iob ( bnx2x->rx_iobuf[i] );
			bnx2x->rx_iobuf[i] = NULL;
		}
	}
	if ( bnx2x->tx_ring ) {
		free_phys ( bnx2x->tx_ring, BNX2X_PAGE_SIZE );
		bnx2x->tx_ring = NULL;
	}
	if ( bnx2x->rx_cq_ring ) {
		free_phys ( bnx2x->rx_cq_ring, BNX2X_PAGE_SIZE );
		bnx2x->rx_cq_ring = NULL;
	}
	if ( bnx2x->rx_bd_ring ) {
		free_phys ( bnx2x->rx_bd_ring, BNX2X_PAGE_SIZE );
		bnx2x->rx_bd_ring = NULL;
	}
	if ( bnx2x->fp_sb ) {
		free_phys ( bnx2x->fp_sb, 0x40 );
		bnx2x->fp_sb = NULL;
	}
}

/**
 * Close the Ethernet datapath (halt, terminate and delete the
 * connection)
 *
 * @v netdev		Network device
 */
void bnx2x_eth_close ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;

	/* Dump RX-path diagnostics before tearing down */
	bnx2x_rx_diag ( bnx2x );

	/* Halt the client (completion on the RX CQ) */
	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_HALT, BNX2X_ETH_CID, 0,
			ETH_CONNECTION_TYPE );
	bnx2x_wait_ramrod_cqe ( bnx2x );

	/* Terminate the connection (completion on the RX CQ) */
	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_ETH_TERMINATE, BNX2X_ETH_CID, 0,
			ETH_CONNECTION_TYPE );
	bnx2x_wait_ramrod_cqe ( bnx2x );

	/* Delete the CFC element (completion on the EQ) */
	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_COMMON_CFC_DEL, BNX2X_ETH_CID, 0,
			NONE_CONNECTION_TYPE );
	bnx2x_sp_wait_comp ( bnx2x, EVENT_RING_OPCODE_CFC_DEL );

	bnx2x_eth_free ( bnx2x );
}

/**
 * Transmit a packet
 *
 * @v netdev		Network device
 * @v iobuf		I/O buffer
 * @ret rc		Return status code
 */
int bnx2x_eth_transmit ( struct net_device *netdev,
			 struct io_buffer *iobuf ) {
	struct bnx2x_nic *bnx2x = netdev->priv;
	unsigned int slot;
	uint32_t *bd;
	physaddr_t phys;
	uint32_t db;

	/* Limit outstanding transmissions */
	if ( ( bnx2x->tx_pkt_prod - bnx2x->tx_pkt_cons ) >=
	     BNX2X_TX_MAX_PENDING )
		return -ENOBUFS;

	/* Start BD */
	slot = ( bnx2x->tx_bd_prod & ( BNX2X_TX_BD_CNT - 1 ) );
	bd = ( bnx2x->tx_ring + ( slot * 16 ) );
	phys = virt_to_bus ( iobuf->data );
	bd[0] = cpu_to_le32 ( phys & 0xffffffffUL );		/* addr_lo */
	bd[1] = cpu_to_le32 ( ( ( uint64_t ) phys ) >> 32 );	/* addr_hi */
	/* nbd = 2 (start + parse), nbytes */
	bd[2] = cpu_to_le32 ( 2 | ( iob_len ( iobuf ) << 16 ) );
	/* vlan_or_ethertype = pkt_prod (fw sanity counter in non-MF),
	 * bd_flags = START_BD, general_data = 1 header BD
	 */
	bd[3] = cpu_to_le32 ( ( bnx2x->tx_pkt_prod & 0xffff ) |
			      ( 0x10 << 16 ) | ( 0x01 << 24 ) );
	bnx2x->tx_bd_prod = bnx2x_next_tx_idx ( bnx2x->tx_bd_prod );

	/* Parse BD (E2): all zeros for plain L2 */
	slot = ( bnx2x->tx_bd_prod & ( BNX2X_TX_BD_CNT - 1 ) );
	bd = ( bnx2x->tx_ring + ( slot * 16 ) );
	bd[0] = 0;
	bd[1] = 0;
	bd[2] = 0;
	bd[3] = 0;
	bnx2x->tx_bd_prod = bnx2x_next_tx_idx ( bnx2x->tx_bd_prod );

	/* Record buffer and ring the doorbell */
	bnx2x->tx_iobuf[bnx2x->tx_pkt_prod % BNX2X_TX_MAX_PENDING] = iobuf;
	bnx2x->tx_pkt_prod++;
	bnx2x->tx_db_prod += 2;
	wmb();
	/* doorbell_set_prod: header (DB_TYPE) | prod << 16 */
	db = ( 0x02 | ( ( bnx2x->tx_db_prod & 0xffff ) << 16 ) );
	writel ( db, ( bnx2x->doorbells + ( BNX2X_ETH_CID * 8 ) ) );

	DBGC2 ( bnx2x, "BNX2X %p TX %d len %zd\n", bnx2x,
		( bnx2x->tx_pkt_prod - 1 ), iob_len ( iobuf ) );
	return 0;
}

/**
 * Poll for completed and received packets
 *
 * @v netdev		Network device
 */
void bnx2x_eth_poll ( struct net_device *netdev ) {
	struct bnx2x_nic *bnx2x = netdev->priv;
	uint16_t *tx_cons_sb = ( bnx2x->fp_sb + ( 5 * 2 ) ); /* index 5 */
	uint16_t *rx_cons_sb = ( bnx2x->fp_sb + ( 1 * 2 ) ); /* index 1 */
	struct io_buffer *iobuf;
	uint32_t *cqe;
	unsigned int hw_cons;
	unsigned int slot;
	unsigned int type;
	unsigned int len;
	unsigned int pad;
	int posted = 0;

	/* Complete transmissions */
	hw_cons = le16_to_cpu ( *tx_cons_sb );
	while ( ( bnx2x->tx_pkt_cons & 0xffff ) != hw_cons ) {
		iobuf = bnx2x->tx_iobuf[bnx2x->tx_pkt_cons %
					BNX2X_TX_MAX_PENDING];
		netdev_tx_complete ( netdev, iobuf );
		bnx2x->tx_pkt_cons++;
	}

	/* Process received packets */
	hw_cons = le16_to_cpu ( *rx_cons_sb );
	if ( ( hw_cons & BNX2X_RCQ_USABLE ) == BNX2X_RCQ_USABLE )
		hw_cons++;
	while ( ( bnx2x->rx_cq_cons & 0xffff ) != hw_cons ) {
		slot = ( bnx2x->rx_cq_cons & ( BNX2X_RCQ_CNT - 1 ) );
		cqe = ( bnx2x->rx_cq_ring + ( slot * 64 ) );
		type = ( cqe[0] & 0x3 );

		if ( type == 0 ) {	/* fast path CQE */
			iobuf = bnx2x->rx_iobuf[bnx2x->rx_ring_tail %
						BNX2X_RX_FILL];
			bnx2x->rx_iobuf[bnx2x->rx_ring_tail %
					BNX2X_RX_FILL] = NULL;
			bnx2x->rx_ring_tail++;
			pad = ( ( cqe[0] >> 24 ) & 0xff );
			len = ( ( cqe[2] >> 16 ) & 0xffff );
			if ( cqe[0] & 0x8 ) {	/* PHY_DECODE_ERR */
				netdev_rx_err ( netdev, iobuf, -EIO );
			} else {
				iob_reserve ( iobuf, pad );
				iob_put ( iobuf, len );
				netdev_rx ( netdev, iobuf );
			}
		} else {
			DBGC2 ( bnx2x, "BNX2X %p unexpected CQE %08x\n",
				bnx2x, cqe[0] );
		}
		memset ( cqe, 0, 64 );
		bnx2x->rx_cq_cons = bnx2x_next_rcq_idx ( bnx2x->rx_cq_cons );

		/* Refill: posting a new buffer advances both the BD
		 * producer and the CQ producer (returning the CQE
		 * slot we just consumed to the firmware)
		 */
		if ( bnx2x_post_rx_buffer ( bnx2x ) == 0 )
			posted = 1;
	}
	if ( posted )
		bnx2x_update_rx_prods ( bnx2x );
}
