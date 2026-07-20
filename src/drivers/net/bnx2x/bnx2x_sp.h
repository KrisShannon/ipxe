#ifndef _BNX2X_SP_H
#define _BNX2X_SP_H

/** @file
 *
 * bnx2x slowpath channel definitions
 *
 * Constants from the Linux bnx2x driver (GPLv2) bnx2x_hsi.h /
 * bnx2x_fw_defs.h / bnx2x.h as of Linux v6.6.  Structure layouts
 * computed via host offsetof tool (see CLAUDE.md).
 *
 */

FILE_LICENCE ( GPL2_ONLY );

#include <stdint.h>

struct bnx2x_nic;

/** DMA page size used for rings */
#define BNX2X_PAGE_SIZE			4096

/** Default status block: allocation size (struct is 0x38, sp_sb at
 * +0x10 with index_values[16] and running_index at +0x20)
 */
#define BNX2X_DEF_SB_SIZE		0x40
#define BNX2X_DEF_SB_SP_SB_OFFSET	0x10

/** Slowpath status block index used for the event queue consumer */
#define BNX2X_HC_SP_INDEX_EQ_CONS	7

/** IGU operation modes (enum igu_mode) */
#define HC_IGU_BC_MODE			0
#define HC_IGU_NBC_MODE			1

/** Event queue geometry: one page of 16-byte elements */
#define BNX2X_EQ_ELEM_SIZE		16
#define BNX2X_EQ_DESC_CNT		( BNX2X_PAGE_SIZE /	\
					  BNX2X_EQ_ELEM_SIZE )

/** Slowpath queue geometry: one page of 16-byte elements */
#define BNX2X_SPQ_ELEM_SIZE		16
#define BNX2X_SPQ_DESC_CNT		( BNX2X_PAGE_SIZE /	\
					  BNX2X_SPQ_ELEM_SIZE )

/** Ramrod data buffer size */
#define BNX2X_SP_DATA_SIZE		0x200

/* Common (NONE_CONNECTION_TYPE) ramrod command ids */
#define RAMROD_CMD_ID_COMMON_FUNCTION_START	1
#define RAMROD_CMD_ID_COMMON_FUNCTION_STOP	2
#define RAMROD_CMD_ID_COMMON_CFC_DEL		4

/* Ethernet (ETH_CONNECTION_TYPE) ramrod command ids */
#define RAMROD_CMD_ID_ETH_CLIENT_SETUP		1
#define RAMROD_CMD_ID_ETH_HALT			2
#define RAMROD_CMD_ID_ETH_TERMINATE		7
#define RAMROD_CMD_ID_ETH_CLASSIFICATION_RULES	9
#define RAMROD_CMD_ID_ETH_FILTER_RULES		10

/* Connection types */
#define ETH_CONNECTION_TYPE		0
#define NONE_CONNECTION_TYPE		8

/* Event ring opcodes */
#define EVENT_RING_OPCODE_FUNCTION_START	1
#define EVENT_RING_OPCODE_FUNCTION_STOP		2
#define EVENT_RING_OPCODE_CFC_DEL		3
#define EVENT_RING_OPCODE_SET_MAC		14
#define EVENT_RING_OPCODE_CLASSIFICATION_RULES	15
#define EVENT_RING_OPCODE_FILTERS_RULES		16

extern int bnx2x_sp_init ( struct bnx2x_nic *bnx2x );
extern void bnx2x_sp_free ( struct bnx2x_nic *bnx2x );
extern int bnx2x_func_stop ( struct bnx2x_nic *bnx2x );
extern int bnx2x_sp_wait_comp ( struct bnx2x_nic *bnx2x,
				unsigned int opcode );
extern uint32_t bnx2x_iro_offset ( unsigned int idx, unsigned int m1_mult,
				   unsigned int m2_mult );
extern void bnx2x_storm_fill ( struct bnx2x_nic *bnx2x, uint32_t bar,
			       uint32_t offset, size_t len, uint32_t fill );
extern void bnx2x_storm_memcpy ( struct bnx2x_nic *bnx2x, uint32_t bar,
				 uint32_t offset, const void *data,
				 size_t len );
extern void bnx2x_sp_post ( struct bnx2x_nic *bnx2x, unsigned int command,
			    unsigned int cid, physaddr_t data_phys,
			    unsigned int conn_type );

#endif /* _BNX2X_SP_H */
