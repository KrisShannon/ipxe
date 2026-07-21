/*
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Slowpath infrastructure: default status block, event queue,
 * slowpath queue and ramrod posting.
 *
 * Ported from the Linux bnx2x driver,
 * drivers/net/ethernet/broadcom/bnx2x/ (Linux v6.6): primarily
 * bnx2x_init_def_sb / bnx2x_init_sp_ring / bnx2x_init_eq_ring /
 * bnx2x_pf_init / bnx2x_sp_post / bnx2x_eq_int in bnx2x_main.c and
 * bnx2x_func_send_start in bnx2x_sp.c:
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
 * bnx2x_hsi.h with a host offsetof tool (see CLAUDE.md).  Storm RAM
 * offsets come from the IRO table embedded in the firmware image.
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
#include <ipxe/netdevice.h>
#include "bnx2x.h"
#include "bnx2x_init.h"
#include "bnx2x_hw.h"
#include "bnx2x_sp.h"

/**
 * Compute an IRO-based storm RAM offset
 *
 * @v idx		IRO table index
 * @v m1_mult		Multiplier applied to m1
 * @v m2_mult		Multiplier applied to m2
 * @ret offset		Offset within the storm BAR window
 */
uint32_t bnx2x_iro_offset ( unsigned int idx, unsigned int m1_mult,
				   unsigned int m2_mult ) {
	const struct bnx2x_iro *iro = bnx2x_iro ( idx );

	return ( iro->base + ( m1_mult * iro->m1 ) + ( m2_mult * iro->m2 ) );
}

/**
 * Write an 8-bit value to a storm RAM location
 */
static void bnx2x_storm_writeb ( struct bnx2x_nic *bnx2x, uint32_t bar,
				 uint32_t offset, uint8_t value ) {

	writeb ( value, ( bnx2x->regs + bar + offset ) );
}

/**
 * Write a 16-bit value to a storm RAM location
 */
static void bnx2x_storm_writew ( struct bnx2x_nic *bnx2x, uint32_t bar,
				 uint32_t offset, uint16_t value ) {

	writew ( value, ( bnx2x->regs + bar + offset ) );
}

/**
 * Fill a storm RAM region with a value (dword granularity)
 */
void bnx2x_storm_fill ( struct bnx2x_nic *bnx2x, uint32_t bar,
			       uint32_t offset, size_t len, uint32_t fill ) {
	size_t i;

	for ( i = 0 ; i < len ; i += 4 )
		bnx2x_writel ( bnx2x, fill, ( bar + offset + i ) );
}

/**
 * Write a structure to storm RAM as a sequence of dwords
 */
void bnx2x_storm_memcpy ( struct bnx2x_nic *bnx2x, uint32_t bar,
				 uint32_t offset, const void *data,
				 size_t len ) {
	const uint32_t *dwords = data;
	size_t i;

	for ( i = 0 ; i < ( len / 4 ) ; i++ ) {
		bnx2x_writel ( bnx2x, dwords[i],
			       ( bar + offset + ( i * 4 ) ) );
	}
}

/**
 * Initialise the default (slowpath) status block
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_init_def_sb ( struct bnx2x_nic *bnx2x ) {
	physaddr_t sp_sb_phys = ( virt_to_bus ( bnx2x->def_sb ) +
				  BNX2X_DEF_SB_SP_SB_OFFSET );
	uint32_t sb_data[4];
	unsigned int pf = bnx2x->pfid;

	/* Record the IGU status block id in the attention block and
	 * point the IGU attention message block at it
	 */
	( ( uint8_t * ) bnx2x->def_sb )[0x08] = bnx2x->igu_dsb_id;
	bnx2x_writel ( bnx2x, virt_to_bus ( bnx2x->def_sb ) & 0xffffffffUL,
		       IGU_REG_ATTN_MSG_ADDR_L );
	bnx2x_writel ( bnx2x,
		       ( ( uint64_t ) virt_to_bus ( bnx2x->def_sb ) ) >> 32,
		       IGU_REG_ATTN_MSG_ADDR_H );

	/* Disable and zero the slowpath status block state */
	memset ( sb_data, 0, sizeof ( sb_data ) );
	sb_data[3] = 0xff0000;	/* p_func.vf_id = 0xff (invalid) */
	bnx2x_storm_memcpy ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 146, pf, 0 ),
			     sb_data, sizeof ( sb_data ) );
	bnx2x_storm_fill ( bnx2x, BAR_CSTRORM_INTMEM,
			   bnx2x_iro_offset ( 145, pf, 0 ),
			   bnx2x_iro ( 145 )->size, 0 );
	bnx2x_storm_fill ( bnx2x, BAR_CSTRORM_INTMEM,
			   bnx2x_iro_offset ( 148, pf, 0 ),
			   bnx2x_iro ( 148 )->size, 0 );

	/* Fill in hc_sp_status_block_data:
	 * word0/1: host_sb_addr lo/hi
	 * word2:   igu_sb_id | igu_seg_id<<8 | state<<16 (LE byte order:
	 *          igu_sb_id, igu_seg_id, state, rsrv)
	 * word3:   pci_entity: pf_id, vnic_id, vf_id=0xff, vf_valid=0
	 */
	sb_data[0] = ( sp_sb_phys & 0xffffffffUL );
	sb_data[1] = ( ( ( uint64_t ) sp_sb_phys ) >> 32 );
	sb_data[2] = ( bnx2x->igu_dsb_id | ( IGU_SEG_ACCESS_DEF << 8 ) |
		       ( 1 /* SB_ENABLED */ << 16 ) );
	sb_data[3] = ( pf | ( ( bnx2x->pfid >> 1 ) << 8 ) | ( 0xff << 16 ) );
	bnx2x_storm_memcpy ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 146, pf, 0 ),
			     sb_data, sizeof ( sb_data ) );

	/* Enable the default status block in the IGU */
	bnx2x_igu_ack_sb ( bnx2x, bnx2x->igu_dsb_id, IGU_SEG_ACCESS_DEF, 0,
			   0 /* IGU_INT_ENABLE */, 0 );
}

/**
 * Initialise the slowpath queue
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_init_sp_ring ( struct bnx2x_nic *bnx2x ) {
	physaddr_t spq_phys = virt_to_bus ( bnx2x->spq );
	unsigned int func = bnx2x->pfid;

	bnx2x->spq_prod_idx = 0;

	/* Enable the function in all four storms */
	bnx2x_storm_writeb ( bnx2x, BAR_XSTRORM_INTMEM,
			     bnx2x_iro_offset ( 48, func, 0 ), func );
	bnx2x_storm_writeb ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 154, func, 0 ), func );
	bnx2x_storm_writeb ( bnx2x, BAR_TSTRORM_INTMEM,
			     bnx2x_iro_offset ( 108, func, 0 ), func );
	bnx2x_storm_writeb ( bnx2x, BAR_USTRORM_INTMEM,
			     bnx2x_iro_offset ( 183, func, 0 ), func );
	bnx2x_storm_writeb ( bnx2x, BAR_XSTRORM_INTMEM,
			     bnx2x_iro_offset ( 47, func, 0 ), 1 );
	bnx2x_storm_writeb ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 153, func, 0 ), 1 );
	bnx2x_storm_writeb ( bnx2x, BAR_TSTRORM_INTMEM,
			     bnx2x_iro_offset ( 107, func, 0 ), 1 );
	bnx2x_storm_writeb ( bnx2x, BAR_USTRORM_INTMEM,
			     bnx2x_iro_offset ( 182, func, 0 ), 1 );

	/* SPQ page base and initial producer (via the XSEM fast
	 * memory window, as Linux does at this stage)
	 */
	bnx2x_writel ( bnx2x, ( spq_phys & 0xffffffffUL ),
		       ( XSEM_REG_FAST_MEMORY +
			 bnx2x_iro_offset ( 30, func, 0 ) ) );
	bnx2x_writel ( bnx2x, ( ( ( uint64_t ) spq_phys ) >> 32 ),
		       ( XSEM_REG_FAST_MEMORY +
			 bnx2x_iro_offset ( 30, func, 0 ) + 4 ) );
	bnx2x_writel ( bnx2x, bnx2x->spq_prod_idx,
		       ( XSEM_REG_FAST_MEMORY +
			 bnx2x_iro_offset ( 31, func, 0 ) ) );
}

/**
 * Initialise the event queue
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_init_eq_ring ( struct bnx2x_nic *bnx2x ) {
	physaddr_t eq_phys = virt_to_bus ( bnx2x->eq_ring );
	uint32_t *last_elem;
	uint32_t eq_data[4];
	unsigned int pf = bnx2x->pfid;

	/* Last element of the (single) page points back to the page
	 * (struct regpair: lo dword first, then hi)
	 */
	last_elem = ( bnx2x->eq_ring +
		      ( ( BNX2X_EQ_DESC_CNT - 1 ) * BNX2X_EQ_ELEM_SIZE ) );
	last_elem[0] = ( eq_phys & 0xffffffffUL );		/* lo */
	last_elem[1] = ( ( ( uint64_t ) eq_phys ) >> 32 );	/* hi */

	bnx2x->eq_cons = 0;
	bnx2x->eq_prod = BNX2X_EQ_DESC_CNT;

	/* event_ring_data: base_addr lo/hi, then
	 * producer(u16)|sb_id(u8)<<16|index_id(u8)<<24, reserved
	 */
	eq_data[0] = ( eq_phys & 0xffffffffUL );
	eq_data[1] = ( ( ( uint64_t ) eq_phys ) >> 32 );
	eq_data[2] = ( bnx2x->eq_prod | ( 0xde /* HC_SP_SB_ID */ << 16 ) |
		       ( BNX2X_HC_SP_INDEX_EQ_CONS << 24 ) );
	eq_data[3] = 0;
	bnx2x_storm_memcpy ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 157, ( pf >> 1 ),
						( pf & 1 ) ),
			     eq_data, sizeof ( eq_data ) );
}

/**
 * Update the event queue producer
 *
 * @v bnx2x		bnx2x device
 */
static void bnx2x_update_eq_prod ( struct bnx2x_nic *bnx2x ) {
	unsigned int pf = bnx2x->pfid;

	bnx2x_storm_writew ( bnx2x, BAR_CSTRORM_INTMEM,
			     bnx2x_iro_offset ( 158, ( pf >> 1 ),
						( pf & 1 ) ),
			     bnx2x->eq_prod );
}

/**
 * Post a slowpath element (ramrod)
 *
 * @v bnx2x		bnx2x device
 * @v command		Ramrod command id
 * @v cid		Connection id
 * @v data_phys		Physical address of ramrod data (or 0)
 * @v conn_type		Connection type (ETH/NONE)
 */
void bnx2x_sp_post ( struct bnx2x_nic *bnx2x, unsigned int command,
			    unsigned int cid, physaddr_t data_phys,
			    unsigned int conn_type ) {
	uint32_t *spe = ( bnx2x->spq +
			  ( ( bnx2x->spq_prod_idx % BNX2X_SPQ_DESC_CNT ) *
			    BNX2X_SPQ_ELEM_SIZE ) );
	uint32_t hw_cid;
	uint16_t type;

	/* struct eth_spe: hdr.conn_and_cmd_data (le32), hdr.type
	 * (le16 + le16 reserved), data.update_data_addr as a regpair
	 * (lo dword first, then hi)
	 */
	hw_cid = ( ( bnx2x->port << 23 ) | ( ( bnx2x->pfid >> 1 ) << 17 ) |
		   cid );
	type = ( ( conn_type & 0xff ) | ( bnx2x->pfid << 8 ) );
	spe[0] = cpu_to_le32 ( ( command << 24 ) | hw_cid );
	spe[1] = cpu_to_le32 ( type );
	spe[2] = cpu_to_le32 ( data_phys & 0xffffffffUL );
	spe[3] = cpu_to_le32 ( ( ( uint64_t ) data_phys ) >> 32 );
	wmb();

	/* Advance producer (wraps with the single page) */
	bnx2x->spq_prod_idx = ( ( bnx2x->spq_prod_idx + 1 ) %
				BNX2X_SPQ_DESC_CNT );

	DBGC2 ( bnx2x, "BNX2X %p ramrod cmd %d cid %d type %d prod %d\n",
		bnx2x, command, cid, conn_type, bnx2x->spq_prod_idx );

	bnx2x_storm_writew ( bnx2x, BAR_XSTRORM_INTMEM,
			     bnx2x_iro_offset ( 31, bnx2x->pfid, 0 ),
			     bnx2x->spq_prod_idx );
}

/**
 * Get next event queue index, skipping the next-page element
 */
static unsigned int bnx2x_next_eq_idx ( unsigned int idx ) {

	return ( ( ( idx & ( BNX2X_EQ_DESC_CNT - 1 ) ) ==
		   ( BNX2X_EQ_DESC_CNT - 2 ) ) ? ( idx + 2 ) : ( idx + 1 ) );
}

/**
 * Wait for a slowpath (event queue) completion
 *
 * @v bnx2x		bnx2x device
 * @v opcode		Expected event ring opcode
 * @ret rc		Return status code
 */
int bnx2x_sp_wait_comp ( struct bnx2x_nic *bnx2x, unsigned int opcode ) {
	uint16_t *eq_cons_sb = ( bnx2x->def_sb + BNX2X_DEF_SB_SP_SB_OFFSET +
				 ( BNX2X_HC_SP_INDEX_EQ_CONS * 2 ) );
	uint16_t *running_index = ( bnx2x->def_sb +
				    BNX2X_DEF_SB_SP_SB_OFFSET + 0x20 );
	uint32_t *elem;
	unsigned int hw_cons;
	unsigned int i;
	int found = 0;

	/* Wait up to 5 seconds for the EQ consumer index to advance */
	for ( i = 0 ; i < 5000 ; i++ ) {
		hw_cons = le16_to_cpu ( *eq_cons_sb );
		if ( ( hw_cons & ( BNX2X_EQ_DESC_CNT - 1 ) ) ==
		     ( BNX2X_EQ_DESC_CNT - 1 ) )
			hw_cons++;
		if ( hw_cons != bnx2x->eq_cons )
			break;
		mdelay ( 1 );
	}
	if ( hw_cons == bnx2x->eq_cons ) {
		DBGC ( bnx2x, "BNX2X %p timed out waiting for completion "
		       "(opcode %d)\n", bnx2x, opcode );
		return -ETIMEDOUT;
	}

	/* Consume all pending events */
	while ( bnx2x->eq_cons != hw_cons ) {
		elem = ( bnx2x->eq_ring +
			 ( ( bnx2x->eq_cons & ( BNX2X_EQ_DESC_CNT - 1 ) ) *
			   BNX2X_EQ_ELEM_SIZE ) );
		DBGC2 ( bnx2x, "BNX2X %p EQ event opcode %d error %d\n",
			bnx2x, ( elem[0] & 0xff ),
			( ( elem[0] >> 8 ) & 0xff ) );
		if ( ( elem[0] & 0xff ) == opcode )
			found = 1;
		bnx2x->eq_cons = bnx2x_next_eq_idx ( bnx2x->eq_cons );
		bnx2x->eq_prod = bnx2x_next_eq_idx ( bnx2x->eq_prod );
	}

	/* Return the consumed elements to the firmware and
	 * acknowledge the default status block
	 */
	bnx2x_update_eq_prod ( bnx2x );
	bnx2x_igu_ack_sb ( bnx2x, bnx2x->igu_dsb_id, IGU_SEG_ACCESS_DEF,
			   le16_to_cpu ( *running_index ), IGU_INT_NOP, 1 );

	if ( ! found ) {
		DBGC ( bnx2x, "BNX2X %p expected EQ opcode %d not seen\n",
		       bnx2x, opcode );
		return -EIO;
	}
	return 0;
}

/**
 * Send the FUNCTION_START ramrod
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
static int bnx2x_func_start ( struct bnx2x_nic *bnx2x ) {
	uint8_t *rdata = bnx2x->sp_data;

	/* struct function_start_data: function_mode +0x00 (0 = single
	 * function), sd_vlan_tag le16 +0x02, path_id +0x06,
	 * network_cos_mode +0x07 (STATIC_COS=1 on E2/E3), dmae_cmd_id
	 * +0x08 (13 = FW DMAE channel), inner_rss +0x13,
	 * sd_vlan_eth_type le16 +0x1c (0x8100)
	 */
	memset ( rdata, 0, 0x30 );
	rdata[0x02] = 0;	/* sd_vlan_tag (le16) = 0 */
	rdata[0x06] = bnx2x->path;
	rdata[0x07] = 1;	/* STATIC_COS */
	rdata[0x08] = 13;	/* BNX2X_FW_DMAE_C */
	rdata[0x13] = 1;	/* inner_rss */
	rdata[0x1c] = 0x00;	/* sd_vlan_eth_type = 0x8100 (le16) */
	rdata[0x1d] = 0x81;
	wmb();

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_COMMON_FUNCTION_START, 0,
			virt_to_bus ( rdata ), NONE_CONNECTION_TYPE );
	return bnx2x_sp_wait_comp ( bnx2x, EVENT_RING_OPCODE_FUNCTION_START );
}

/**
 * Send the FUNCTION_STOP ramrod
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
int bnx2x_func_stop ( struct bnx2x_nic *bnx2x ) {

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_COMMON_FUNCTION_STOP, 0, 0,
			NONE_CONNECTION_TYPE );
	return bnx2x_sp_wait_comp ( bnx2x, EVENT_RING_OPCODE_FUNCTION_STOP );
}

/**
 * Free slowpath memory
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_sp_free ( struct bnx2x_nic *bnx2x ) {

	if ( bnx2x->sp_data ) {
		free_phys ( bnx2x->sp_data, BNX2X_SP_DATA_SIZE );
		bnx2x->sp_data = NULL;
	}
	if ( bnx2x->spq ) {
		free_phys ( bnx2x->spq, BNX2X_PAGE_SIZE );
		bnx2x->spq = NULL;
	}
	if ( bnx2x->eq_ring ) {
		free_phys ( bnx2x->eq_ring, BNX2X_PAGE_SIZE );
		bnx2x->eq_ring = NULL;
	}
	if ( bnx2x->def_sb ) {
		free_phys ( bnx2x->def_sb, BNX2X_DEF_SB_SIZE );
		bnx2x->def_sb = NULL;
	}
}

/**
 * Initialise the slowpath channel and start the function
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
/**
 * Query and dump the storm firmware statistics
 *
 * @v bnx2x		bnx2x device
 *
 * Posts a STAT_QUERY ramrod asking the firmware to DMA its per-port
 * TSTORM statistics (MAC/filter/BRB discard counters) and our
 * queue's per-storm statistics (received/discarded packet counts by
 * reason) to a host buffer, then prints them.  This names the fate
 * of RX frames at the storm level, which no directly-readable
 * register exposes.
 *
 * Buffer layout (one page): the query request (header + 2 entries)
 * at +0x000; completion counters at +0x800 (pre-filled with 0xff;
 * the firmware writes back the request's drv_stats_counter, i.e. 0);
 * per-port TSTORM statistics at +0x840; per-queue statistics at
 * +0x880 (TSTORM at +0x00, USTORM at +0x38, XSTORM at +0x70 within).
 */
void bnx2x_stats_query_dump ( struct bnx2x_nic *bnx2x ) {
	uint8_t *buf;
	uint32_t *req;
	uint32_t *cnt;
	uint32_t *port;
	uint32_t *queue;
	physaddr_t phys;
	unsigned int cl_id = bnx2x->igu_base_sb;	/* = client id */
	unsigned int i;

	buf = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	if ( ! buf )
		return;
	memset ( buf, 0, BNX2X_PAGE_SIZE );
	req = ( ( uint32_t * ) buf );
	cnt = ( ( uint32_t * ) ( buf + 0x800 ) );
	port = ( ( uint32_t * ) ( buf + 0x840 ) );
	queue = ( ( uint32_t * ) ( buf + 0x880 ) );
	phys = virt_to_bus ( buf );

	/* stats_query_header: cmd_num, drv_stats_counter=0, then the
	 * completion counters address (regpair lo/hi)
	 */
	buf[0x00] = 2;				/* cmd_num */
	req[2] = cpu_to_le32 ( ( phys + 0x800 ) & 0xffffffffUL );
	req[3] = cpu_to_le32 ( ( ( uint64_t ) ( phys + 0x800 ) ) >> 32 );
	memset ( ( buf + 0x800 ), 0xff, 0x20 );

	/* Query 0: per-port statistics (TSTORM discard counters) */
	buf[0x10] = 1;				/* kind = STATS_TYPE_PORT */
	buf[0x11] = bnx2x->port;		/* index (don't care) */
	buf[0x12] = bnx2x->pfid;		/* funcID le16 */
	req[6] = cpu_to_le32 ( ( phys + 0x840 ) & 0xffffffffUL );
	req[7] = cpu_to_le32 ( ( ( uint64_t ) ( phys + 0x840 ) ) >> 32 );

	/* Query 1: our queue's statistics */
	buf[0x20] = 0;				/* kind = STATS_TYPE_QUEUE */
	buf[0x21] = cl_id;			/* index = stats id */
	buf[0x22] = bnx2x->pfid;		/* funcID le16 */
	req[10] = cpu_to_le32 ( ( phys + 0x880 ) & 0xffffffffUL );
	req[11] = cpu_to_le32 ( ( ( uint64_t ) ( phys + 0x880 ) ) >> 32 );
	wmb();

	bnx2x_sp_post ( bnx2x, RAMROD_CMD_ID_COMMON_STAT_QUERY, 0, phys,
			NONE_CONNECTION_TYPE );
	bnx2x_sp_wait_comp ( bnx2x, EVENT_RING_OPCODE_STAT_QUERY );

	/* Wait for the firmware to write the completion counters
	 * (tstats/ustats echo drv_stats_counter = 0)
	 */
	for ( i = 0 ; i < 100 ; i++ ) {
		if ( ( ( le32_to_cpu ( cnt[2] ) & 0xffff ) != 0xffff ) &&
		     ( ( le32_to_cpu ( cnt[4] ) & 0xffff ) != 0xffff ) )
			break;
		mdelay ( 1 );
	}

	DBGC ( bnx2x, "BNX2X %p STATS counters x %04x t %04x u %04x c %04x\n",
	       bnx2x, ( le32_to_cpu ( cnt[0] ) & 0xffff ),
	       ( le32_to_cpu ( cnt[2] ) & 0xffff ),
	       ( le32_to_cpu ( cnt[4] ) & 0xffff ),
	       ( le32_to_cpu ( cnt[6] ) & 0xffff ) );
	DBGC ( bnx2x, "BNX2X %p STATS port mac_discard %d filter_discard %d "
	       "brb_trunc %d mf_tag %d pkt_drop %d\n", bnx2x,
	       le32_to_cpu ( port[0] ), le32_to_cpu ( port[1] ),
	       le32_to_cpu ( port[2] ), le32_to_cpu ( port[3] ),
	       le32_to_cpu ( port[4] ) );
	DBGC ( bnx2x, "BNX2X %p STATS tstorm(q) ucast %d csum_disc %d "
	       "bcast %d too_big %d mcast %d ttl0 %d no_buff %d\n", bnx2x,
	       le32_to_cpu ( queue[2] ),	/* rcv_ucast_pkts */
	       le32_to_cpu ( queue[3] ),	/* checksum_discard */
	       le32_to_cpu ( queue[6] ),	/* rcv_bcast_pkts */
	       le32_to_cpu ( queue[7] ),	/* pkts_too_big_discard */
	       le32_to_cpu ( queue[10] ),	/* rcv_mcast_pkts */
	       le32_to_cpu ( queue[11] ),	/* ttl0_discard */
	       ( le32_to_cpu ( queue[12] ) & 0xffff ) ); /* no_buff (le16) */
	DBGC ( bnx2x, "BNX2X %p STATS ustorm(q) no_buff u %d m %d b %d "
	       "xstorm(q) sent u %d m %d b %d err_drop %d\n", bnx2x,
	       le32_to_cpu ( queue[20] ),	/* ucast_no_buff_pkts */
	       le32_to_cpu ( queue[21] ),	/* mcast_no_buff_pkts */
	       le32_to_cpu ( queue[22] ),	/* bcast_no_buff_pkts */
	       le32_to_cpu ( queue[34] ),	/* ucast_pkts_sent */
	       le32_to_cpu ( queue[35] ),	/* mcast_pkts_sent */
	       le32_to_cpu ( queue[36] ),	/* bcast_pkts_sent */
	       le32_to_cpu ( queue[37] ) );	/* error_drop_pkts */

	free_phys ( buf, BNX2X_PAGE_SIZE );
}

int bnx2x_sp_init ( struct bnx2x_nic *bnx2x ) {
	int rc;

	/* Allocate memory */
	bnx2x->def_sb = malloc_phys ( BNX2X_DEF_SB_SIZE, BNX2X_DEF_SB_SIZE );
	bnx2x->eq_ring = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	bnx2x->spq = malloc_phys ( BNX2X_PAGE_SIZE, BNX2X_PAGE_SIZE );
	bnx2x->sp_data = malloc_phys ( BNX2X_SP_DATA_SIZE,
				       BNX2X_SP_DATA_SIZE );
	if ( ! ( bnx2x->def_sb && bnx2x->eq_ring && bnx2x->spq &&
		 bnx2x->sp_data ) ) {
		rc = -ENOMEM;
		goto err;
	}
	memset ( bnx2x->def_sb, 0, BNX2X_DEF_SB_SIZE );
	memset ( bnx2x->eq_ring, 0, BNX2X_PAGE_SIZE );
	memset ( bnx2x->spq, 0, BNX2X_PAGE_SIZE );
	memset ( bnx2x->sp_data, 0, BNX2X_SP_DATA_SIZE );

	/* Internal storm RAM initialisation that the firmware init
	 * tables do not cover (Linux bnx2x_init_internal_common:
	 * "Zero this manually as its initialization is currently
	 * missing in the initTool").  Uninitialised USTORM
	 * aggregation data can wedge the RX placement path, which
	 * backpressures TSTORM, the parser and ultimately the whole
	 * MAC ingress.  The IGU mode byte tells CSTORM how status
	 * block acks work; our IGU is forced to normal (non
	 * backward-compatible) mode.
	 */
	bnx2x_storm_fill ( bnx2x, BAR_USTRORM_INTMEM,
			   bnx2x_iro ( 213 )->base, bnx2x_iro ( 213 )->size,
			   0 );
	writeb ( HC_IGU_NBC_MODE,
		 ( bnx2x->regs + BAR_CSTRORM_INTMEM +
		   bnx2x_iro ( 161 )->base ) );

	/* Set up default SB, SPQ and EQ */
	bnx2x_init_def_sb ( bnx2x );
	bnx2x_init_sp_ring ( bnx2x );
	bnx2x_init_eq_ring ( bnx2x );

	/* Start the function */
	if ( ( rc = bnx2x_func_start ( bnx2x ) ) != 0 ) {
		DBGC ( bnx2x, "BNX2X %p FUNCTION_START failed: %s\n",
		       bnx2x, strerror ( rc ) );
		goto err;
	}
	DBGC ( bnx2x, "BNX2X %p function started\n", bnx2x );

	return 0;

 err:
	bnx2x_sp_free ( bnx2x );
	return rc;
}
