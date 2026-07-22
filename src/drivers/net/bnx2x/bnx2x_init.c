/*
 * Broadcom/QLogic NetXtreme II 10/20-Gigabit Ethernet (bnx2x) driver
 *
 * Table-driven hardware initialisation: the init-ops interpreter.
 *
 * Derived from the Linux bnx2x driver,
 * drivers/net/ethernet/broadcom/bnx2x/ (Linux v6.6), primarily
 * bnx2x_init_ops.h:
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

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ipxe/malloc.h>
#include <ipxe/deflate.h>
#include <ipxe/netdevice.h>
#include "bnx2x.h"
#include "bnx2x_init.h"
#include "bnx2x_fw.h"

/** @file
 *
 * bnx2x init-ops interpreter
 *
 * The storm firmware file carries, besides the storm processor code
 * itself, a table-driven description of the chip initialisation:
 * "init ops" (opcodes referencing data in "init data"), indexed per
 * (block, phase) by "init ops offsets".  This file interprets those
 * tables.  All writes are performed as individual register writes
 * (the Linux driver's !dmae_ready mode); DMAE acceleration can be
 * added later if init time turns out to matter.
 *
 */

/* SEM INT_TABLE and PRAM base addresses (from Linux bnx2x_reg.h),
 * used to select the correct firmware blob for OP_ZP by target
 * address.
 */
#define TSEM_REG_INT_TABLE	0x180400
#define TSEM_REG_PRAM		0x1c0000
#define CSEM_REG_INT_TABLE	0x200400
#define CSEM_REG_PRAM		0x240000
#define XSEM_REG_INT_TABLE	0x280400
#define XSEM_REG_PRAM		0x2c0000
#define USEM_REG_INT_TABLE	0x300400
#define USEM_REG_PRAM		0x340000

/** INT_TABLE address window size */
#define SEM_INT_TABLE_SIZE	0x400

/** PRAM address window size */
#define SEM_PRAM_SIZE		0x40000

/** Decompressor state (single-threaded; shared across devices) */
static struct deflate bnx2x_deflate;

/**
 * Get IRO table entry
 *
 * @v idx		IRO index
 * @ret iro		IRO table entry
 */
const struct bnx2x_iro * bnx2x_iro ( unsigned int idx ) {

	return &bnx2x_iro_arr[idx];
}

/**
 * Report firmware information
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_fw_info ( struct bnx2x_nic *bnx2x ) {

	DBGC ( bnx2x, "BNX2X %p storm firmware %d.%d.%d.%d (%zd init ops, "
	       "%zd dwords data, %zd IROs)\n", bnx2x, BNX2X_FW_MAJOR,
	       BNX2X_FW_MINOR, BNX2X_FW_REV, BNX2X_FW_ENG,
	       ( sizeof ( bnx2x_init_ops ) /
		 ( 2 * sizeof ( bnx2x_init_ops[0] ) ) ),
	       ( sizeof ( bnx2x_init_data ) /
		 sizeof ( bnx2x_init_data[0] ) ),
	       ( sizeof ( bnx2x_iro_arr ) / sizeof ( bnx2x_iro_arr[0] ) ) );
}

/**
 * Compute init mode flags
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_set_modes_bitmap ( struct bnx2x_nic *bnx2x ) {
	uint32_t flags = 0;
	unsigned int rev = ( ( bnx2x->chip_id >> 12 ) & 0xf );

	flags |= MODE_ASIC;
	flags |= ( bnx2x->port4mode ? MODE_PORT4 : MODE_PORT2 );

	/* All chips supported by this driver are E3 */
	flags |= MODE_E3;
	if ( rev == 0 ) {
		flags |= MODE_E3_A0;
	} else {
		flags |= ( MODE_E3_B0 | MODE_COS3 );
	}

	if ( ( bnx2x->mf_mode == BNX2X_MF_SI ) ||
	     ( bnx2x->mf_mode == BNX2X_MF_SD ) ) {
		flags |= MODE_MF;
		flags |= ( ( bnx2x->mf_mode == BNX2X_MF_SD ) ?
			   MODE_MF_SD : MODE_MF_SI );
	} else {
		flags |= MODE_SF;
	}

	flags |= MODE_LITTLE_ENDIAN;

	bnx2x->init_mode_flags = flags;
	DBGC2 ( bnx2x, "BNX2X %p init mode flags %08x\n", bnx2x, flags );
}

/**
 * Allocate decompression scratch buffer
 *
 * @v bnx2x		bnx2x device
 * @ret rc		Return status code
 */
int bnx2x_gunzip_init ( struct bnx2x_nic *bnx2x ) {

	bnx2x->gunzip_buf = malloc_phys ( BNX2X_FW_BUF_SIZE,
					  BNX2X_FW_BUF_SIZE );
	if ( ! bnx2x->gunzip_buf )
		return -ENOMEM;
	return 0;
}

/**
 * Free decompression scratch buffer
 *
 * @v bnx2x		bnx2x device
 */
void bnx2x_gunzip_end ( struct bnx2x_nic *bnx2x ) {

	free_phys ( bnx2x->gunzip_buf, BNX2X_FW_BUF_SIZE );
	bnx2x->gunzip_buf = NULL;
}

/**
 * Decompress a gzipped firmware blob into the scratch buffer
 *
 * @v bnx2x		bnx2x device
 * @v zbuf		Compressed data (gzip format)
 * @v len		Length of compressed data
 * @ret rc		Return status code
 */
static int bnx2x_gunzip ( struct bnx2x_nic *bnx2x, const uint8_t *zbuf,
			  size_t len ) {
	struct deflate_chunk out;
	unsigned int n = 10;
	int rc;

	/* Check gzip header (magic, deflate method) */
	if ( ( len < n ) || ( zbuf[0] != 0x1f ) || ( zbuf[1] != 0x8b ) ||
	     ( zbuf[2] != 0x08 ) ) {
		DBGC ( bnx2x, "BNX2X %p bad gzip header\n", bnx2x );
		return -EINVAL;
	}

	/* Skip optional file name */
	if ( zbuf[3] & 0x08 ) {
		while ( ( n < len ) && ( zbuf[n++] != 0 ) ) {}
	}

	/* Inflate raw deflate stream into scratch buffer */
	deflate_init ( &bnx2x_deflate, DEFLATE_RAW );
	deflate_chunk_init ( &out, bnx2x->gunzip_buf, 0, BNX2X_FW_BUF_SIZE );
	if ( ( rc = deflate_inflate ( &bnx2x_deflate, ( zbuf + n ),
				      ( len - n ), &out ) ) != 0 ) {
		DBGC ( bnx2x, "BNX2X %p decompression error: %s\n",
		       bnx2x, strerror ( rc ) );
		return rc;
	}
	if ( ! deflate_finished ( &bnx2x_deflate ) ) {
		DBGC ( bnx2x, "BNX2X %p decompression truncated\n", bnx2x );
		return -EINVAL;
	}
	if ( out.offset & 0x3 ) {
		DBGC ( bnx2x, "BNX2X %p decompressed length %zd not dword "
		       "aligned\n", bnx2x, out.offset );
		return -EINVAL;
	}
	bnx2x->gunzip_outlen = ( out.offset >> 2 );

	return 0;
}

/**
 * Write a string of dwords to consecutive registers
 *
 * @v bnx2x		bnx2x device
 * @v addr		Register address
 * @v data		Data
 * @v len		Length (in dwords)
 */
static void bnx2x_init_str_wr ( struct bnx2x_nic *bnx2x, uint32_t addr,
				const uint32_t *data, uint32_t len ) {
	uint32_t i;

	for ( i = 0 ; i < len ; i++ )
		bnx2x_writel ( bnx2x, data[i], ( addr + ( i * 4 ) ) );
}

/**
 * Write the scratch buffer to consecutive registers
 *
 * @v bnx2x		bnx2x device
 * @v addr		Register address
 * @v len		Length (in dwords)
 */
static void bnx2x_write_big_buf ( struct bnx2x_nic *bnx2x, uint32_t addr,
				  uint32_t len ) {

	/* The Linux driver uses DMAE here when available; we always
	 * use individual writes (its !dmae_ready mode, which is valid
	 * on E2/E3 where the PXP root complex handles BIOS ZLR).
	 */
	bnx2x_init_str_wr ( bnx2x, addr, bnx2x->gunzip_buf, len );
}

/**
 * Fill a device memory region with a value
 *
 * @v bnx2x		bnx2x device
 * @v addr		Register address
 * @v fill		Fill byte value
 * @v len		Length (in dwords)
 */
static void bnx2x_init_fill ( struct bnx2x_nic *bnx2x, uint32_t addr,
			      int fill, uint32_t len ) {
	uint32_t buf_len = ( ( ( len * 4 ) > BNX2X_FW_BUF_SIZE ) ?
			     BNX2X_FW_BUF_SIZE : ( len * 4 ) );
	uint32_t buf_len32 = ( buf_len / 4 );
	uint32_t cur_len;
	uint32_t i;

	memset ( bnx2x->gunzip_buf, ( fill & 0xff ), buf_len );

	for ( i = 0 ; i < len ; i += buf_len32 ) {
		cur_len = ( ( buf_len32 < ( len - i ) ) ?
			    buf_len32 : ( len - i ) );
		bnx2x_write_big_buf ( bnx2x, ( addr + ( i * 4 ) ), cur_len );
	}
}

/**
 * Write a repeating 64-bit pattern to a device memory region
 *
 * @v bnx2x		bnx2x device
 * @v addr		Register address
 * @v data		Pattern (two dwords: low, high)
 * @v len64		Length (in 64-bit units)
 */
static void bnx2x_init_wr_64 ( struct bnx2x_nic *bnx2x, uint32_t addr,
			       const uint32_t *data, uint32_t len64 ) {
	uint32_t buf_len32 = ( BNX2X_FW_BUF_SIZE / 4 );
	uint32_t len = ( len64 * 2 );
	uint64_t data64;
	uint32_t cur_len;
	uint32_t i;

	/* 64 bit value is in a blob: first low dword, then high dword */
	data64 = ( ( ( ( uint64_t ) data[1] ) << 32 ) | data[0] );

	if ( len64 > ( BNX2X_FW_BUF_SIZE / 8 ) )
		len64 = ( BNX2X_FW_BUF_SIZE / 8 );
	for ( i = 0 ; i < len64 ; i++ ) {
		( ( uint64_t * ) bnx2x->gunzip_buf )[i] = data64;
	}

	for ( i = 0 ; i < len ; i += buf_len32 ) {
		cur_len = ( ( buf_len32 < ( len - i ) ) ?
			    buf_len32 : ( len - i ) );
		bnx2x_write_big_buf ( bnx2x, ( addr + ( i * 4 ) ), cur_len );
	}
}

/**
 * Select the firmware blob corresponding to a target address
 *
 * @v addr		Target address
 * @ret data		Firmware blob, or NULL
 *
 * The PRAM and INT_TABLE writes are split into multiple operations,
 * so the target address may lie anywhere within the region.
 */
static const uint8_t * bnx2x_sel_blob ( uint32_t addr ) {

	if ( ( addr >= TSEM_REG_INT_TABLE ) &&
	     ( addr <= ( TSEM_REG_INT_TABLE + SEM_INT_TABLE_SIZE ) ) )
		return bnx2x_tsem_int_table;
	if ( ( addr >= CSEM_REG_INT_TABLE ) &&
	     ( addr <= ( CSEM_REG_INT_TABLE + SEM_INT_TABLE_SIZE ) ) )
		return bnx2x_csem_int_table;
	if ( ( addr >= USEM_REG_INT_TABLE ) &&
	     ( addr <= ( USEM_REG_INT_TABLE + SEM_INT_TABLE_SIZE ) ) )
		return bnx2x_usem_int_table;
	if ( ( addr >= XSEM_REG_INT_TABLE ) &&
	     ( addr <= ( XSEM_REG_INT_TABLE + SEM_INT_TABLE_SIZE ) ) )
		return bnx2x_xsem_int_table;
	if ( ( addr >= TSEM_REG_PRAM ) &&
	     ( addr <= ( TSEM_REG_PRAM + SEM_PRAM_SIZE ) ) )
		return bnx2x_tsem_pram;
	if ( ( addr >= CSEM_REG_PRAM ) &&
	     ( addr <= ( CSEM_REG_PRAM + SEM_PRAM_SIZE ) ) )
		return bnx2x_csem_pram;
	if ( ( addr >= USEM_REG_PRAM ) &&
	     ( addr <= ( USEM_REG_PRAM + SEM_PRAM_SIZE ) ) )
		return bnx2x_usem_pram;
	if ( ( addr >= XSEM_REG_PRAM ) &&
	     ( addr <= ( XSEM_REG_PRAM + SEM_PRAM_SIZE ) ) )
		return bnx2x_xsem_pram;

	return NULL;
}

/**
 * Decompress and write a zipped blob to a device memory region
 *
 * @v bnx2x		bnx2x device
 * @v addr		Register address
 * @v len		Compressed length (in bytes)
 * @v blob_off		Offset into the blob (in dwords)
 */
static void bnx2x_init_wr_zp ( struct bnx2x_nic *bnx2x, uint32_t addr,
			       uint32_t len, uint32_t blob_off ) {
	const uint8_t *data;

	data = bnx2x_sel_blob ( addr );
	if ( ! data ) {
		DBGC ( bnx2x, "BNX2X %p no blob for OP_ZP address %08x\n",
		       bnx2x, addr );
		return;
	}
	data += ( blob_off * 4 );

	if ( bnx2x_gunzip ( bnx2x, data, len ) != 0 )
		return;

	bnx2x_write_big_buf ( bnx2x, addr, bnx2x->gunzip_outlen );
}

/**
 * Run the init operations for one (block, stage) pair
 *
 * @v bnx2x		bnx2x device
 * @v block		Block (BLOCK_xxx)
 * @v stage		Stage (PHASE_xxx)
 */
void bnx2x_init_block ( struct bnx2x_nic *bnx2x, unsigned int block,
			unsigned int stage ) {
	uint16_t op_start = bnx2x_init_ops_offsets[
		BLOCK_OPS_IDX ( block, stage, STAGE_START ) ];
	uint16_t op_end = bnx2x_init_ops_offsets[
		BLOCK_OPS_IDX ( block, stage, STAGE_END ) ];
	const uint32_t *data;
	uint32_t op_idx, op_word, raw_data;
	unsigned int op_type;
	uint32_t addr, len;

	/* If empty block */
	if ( op_start == op_end )
		return;

	DBGC2 ( bnx2x, "BNX2X %p init block %d stage %d ops [%d,%d)\n",
		bnx2x, block, stage, op_start, op_end );

	for ( op_idx = op_start ; op_idx < op_end ; op_idx++ ) {

		op_word = bnx2x_init_ops[ 2 * op_idx ];
		raw_data = bnx2x_init_ops[ ( 2 * op_idx ) + 1 ];
		op_type = ( op_word >> 24 );
		addr = ( op_word & 0xffffff );

		/* Data used by OP_SW, OP_WB and OP_WR_64: length in
		 * dwords (high 16 bits) at offset in init_data (low
		 * 16 bits).
		 */
		len = ( raw_data >> 16 );
		data = ( bnx2x_init_data + ( raw_data & 0xffff ) );

		switch ( op_type ) {
		case OP_RD:
			bnx2x_readl ( bnx2x, addr );
			break;
		case OP_WR:
			bnx2x_writel ( bnx2x, raw_data, addr );
			break;
		case OP_SW:
			bnx2x_init_str_wr ( bnx2x, addr, data, len );
			break;
		case OP_WB:
			/* No DMAE: plain string write (E2/E3 safe) */
			bnx2x_init_str_wr ( bnx2x, addr, data, len );
			break;
		case OP_ZR:
			bnx2x_init_fill ( bnx2x, addr, 0, raw_data );
			break;
		case OP_WB_ZR:
			bnx2x_init_fill ( bnx2x, addr, 0, raw_data );
			break;
		case OP_ZP:
			bnx2x_init_wr_zp ( bnx2x, addr, len,
					   ( raw_data & 0xffff ) );
			break;
		case OP_WR_64:
			bnx2x_init_wr_64 ( bnx2x, addr, data, len );
			break;
		case OP_IF_MODE_AND:
			/* If any of the flags doesn't match, skip the
			 * conditional block.
			 */
			if ( ( bnx2x->init_mode_flags & raw_data ) !=
			     raw_data )
				op_idx += addr;
			break;
		case OP_IF_MODE_OR:
			/* If none of the flags match, skip the
			 * conditional block.
			 */
			if ( ( bnx2x->init_mode_flags & raw_data ) == 0 )
				op_idx += addr;
			break;
		default:
			DBGC ( bnx2x, "BNX2X %p unknown init op %02x at "
			       "index %d\n", bnx2x, op_type, op_idx );
			break;
		}
	}
}
