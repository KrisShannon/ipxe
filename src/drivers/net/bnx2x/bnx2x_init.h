#ifndef _BNX2X_INIT_H
#define _BNX2X_INIT_H

/** @file
 *
 * bnx2x table-driven hardware initialisation
 *
 * Structures and constants for interpreting the init-ops tables
 * shipped inside the storm firmware file.  Derived from the Linux
 * bnx2x driver (GPLv2), drivers/net/ethernet/broadcom/bnx2x/
 * bnx2x_init.h and bnx2x_init_ops.h as of Linux v6.6.
 *
 */

FILE_LICENCE ( GPL2_ONLY );

#include <stdint.h>

struct bnx2x_nic;

/** Init operation types (high byte of the first op word) */
enum {
	OP_RD = 0x1,	/* read a single register */
	OP_WR,		/* write a single register */
	OP_SW,		/* copy a string to the device */
	OP_ZR,		/* clear memory */
	OP_ZP,		/* unzip then copy with DMAE */
	OP_WR_64,	/* write 64 bit pattern */
	OP_WB,		/* copy a string using DMAE */
	OP_WB_ZR,	/* clear a string using DMAE or indirect-wr */
	/* Skip the following ops if all of the init modes don't match */
	OP_IF_MODE_OR,
	/* Skip the following ops if any of the init modes don't match */
	OP_IF_MODE_AND,
	OP_MAX
};

/** Init operation stages */
enum {
	STAGE_START,
	STAGE_END,
};

/** Init phases */
enum {
	PHASE_COMMON,
	PHASE_PORT0,
	PHASE_PORT1,
	PHASE_PF0,
	PHASE_PF1,
	PHASE_PF2,
	PHASE_PF3,
	PHASE_PF4,
	PHASE_PF5,
	PHASE_PF6,
	PHASE_PF7,
	NUM_OF_INIT_PHASES
};

/** Init modes */
enum {
	MODE_ASIC		= 0x00000001,
	MODE_FPGA		= 0x00000002,
	MODE_EMUL		= 0x00000004,
	MODE_E2			= 0x00000008,
	MODE_E3			= 0x00000010,
	MODE_PORT2		= 0x00000020,
	MODE_PORT4		= 0x00000040,
	MODE_SF			= 0x00000080,
	MODE_MF			= 0x00000100,
	MODE_MF_SD		= 0x00000200,
	MODE_MF_SI		= 0x00000400,
	MODE_MF_AFEX		= 0x00000800,
	MODE_E3_A0		= 0x00001000,
	MODE_E3_B0		= 0x00002000,
	MODE_COS3		= 0x00004000,
	MODE_COS6		= 0x00008000,
	MODE_LITTLE_ENDIAN	= 0x00010000,
	MODE_BIG_ENDIAN		= 0x00020000,
};

/** Init blocks */
enum {
	BLOCK_ATC,
	BLOCK_BRB1,
	BLOCK_CCM,
	BLOCK_CDU,
	BLOCK_CFC,
	BLOCK_CSDM,
	BLOCK_CSEM,
	BLOCK_DBG,
	BLOCK_DMAE,
	BLOCK_DORQ,
	BLOCK_HC,
	BLOCK_IGU,
	BLOCK_MISC,
	BLOCK_NIG,
	BLOCK_PBF,
	BLOCK_PGLUE_B,
	BLOCK_PRS,
	BLOCK_PXP2,
	BLOCK_PXP,
	BLOCK_QM,
	BLOCK_SRC,
	BLOCK_TCM,
	BLOCK_TM,
	BLOCK_TSDM,
	BLOCK_TSEM,
	BLOCK_UCM,
	BLOCK_UPB,
	BLOCK_USDM,
	BLOCK_USEM,
	BLOCK_XCM,
	BLOCK_XPB,
	BLOCK_XSDM,
	BLOCK_XSEM,
	BLOCK_MISC_AEU,
	NUM_OF_INIT_BLOCKS
};

/** Index of start/end of a specific block stage in the ops array */
#define BLOCK_OPS_IDX( block, stage, end ) \
	( ( 2 * ( ( (block) * NUM_OF_INIT_PHASES ) + (stage) ) ) + (end) )

/** An init relative offset (IRO) table entry */
struct bnx2x_iro {
	/** Base offset within storm RAM */
	uint32_t base;
	/** First multiplier */
	uint16_t m1;
	/** Second multiplier */
	uint16_t m2;
	/** Third multiplier */
	uint16_t m3;
	/** Size */
	uint16_t size;
};

/** Decompression scratch buffer size (matches Linux FW_BUF_SIZE) */
#define BNX2X_FW_BUF_SIZE	0x8000

extern void bnx2x_set_modes_bitmap ( struct bnx2x_nic *bnx2x );
extern int bnx2x_gunzip_init ( struct bnx2x_nic *bnx2x );
extern void bnx2x_gunzip_end ( struct bnx2x_nic *bnx2x );
extern void bnx2x_init_block ( struct bnx2x_nic *bnx2x, unsigned int block,
			       unsigned int stage );
extern const struct bnx2x_iro * bnx2x_iro ( unsigned int idx );
extern void bnx2x_fw_info ( struct bnx2x_nic *bnx2x );

#endif /* _BNX2X_INIT_H */
