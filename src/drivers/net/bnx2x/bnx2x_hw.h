#ifndef _BNX2X_HW_H
#define _BNX2X_HW_H

/** @file
 *
 * bnx2x hardware initialisation register definitions
 *
 * Register addresses and bit definitions extracted verbatim from the
 * Linux bnx2x driver (GPLv2), drivers/net/ethernet/broadcom/bnx2x/
 * {bnx2x_reg.h, bnx2x.h, bnx2x_hsi.h} as of Linux v6.6, by a script
 * matching the exact #define names used by the ported init code.
 *
 */

FILE_LICENCE ( GPL2_ONLY );

struct bnx2x_nic;

#define GRCBASE_MISC                                     0x00A000
#define GRCBASE_UPB                                      0x0C1000
#define MISC_REGISTERS_RESET_REG_1_SET                   0x584
#define MISC_REGISTERS_RESET_REG_1_CLEAR                 0x588
#define MISC_REGISTERS_RESET_REG_2_SET                   0x594
#define MISC_REGISTERS_RESET_REG_2_CLEAR                 0x598
#define MISC_REGISTERS_RESET_REG_2_MSTAT0                (0x1<<24)
#define MISC_REGISTERS_RESET_REG_2_MSTAT1                (0x1<<25)
#define MISC_REG_DRIVER_CONTROL_1                        0xa510
#define MISC_REG_DRIVER_CONTROL_7                        0xa3c8
#define HW_LOCK_RESOURCE_RESET                           5
#define HW_LOCK_MAX_RESOURCE_VALUE                       31
#define PGLUE_B_REG_INTERNAL_PFID_ENABLE_MASTER          0x942c
#define PGLUE_B_REG_WAS_ERROR_PF_7_0_CLR                 0x9470
#define CFC_REG_WEAK_ENABLE_PF                           0x104124
#define PXP2_REG_PGL_PRETEND_FUNC_F0                     0x120674
#define PXP2_REG_PGL_PRETEND_FUNC_F1                     0x120678
#define PXP_REG_PXP_INT_MASK_0                           0x103074
#define PXP_REG_PXP_INT_MASK_1                           0x103084
#define PXP2_REG_RQ_CFG_DONE                             0x1201b4
#define PXP2_REG_RD_INIT_DONE                            0x120370
#define PXP2_REG_RQ_DISABLE_INPUTS                       0x120330
#define PXP2_REG_RD_DISABLE_INPUTS                       0x120374
#define PXP2_REG_RQ_DRAM_ALIGN                           0x1205b0
#define PXP2_REG_RQ_DRAM_ALIGN_RD                        0x12092c
#define PXP2_REG_RQ_DRAM_ALIGN_SEL                       0x120930
#define PXP2_REG_RQ_WR_MBS0                              0x12015c
#define PXP2_REG_RQ_WR_MBS1                              0x120164
#define PXP2_REG_RQ_RD_MBS0                              0x120160
#define PXP2_REG_RQ_RD_MBS1                              0x120168
#define PXP2_REG_WR_USDMDP_TH                            0x120348
#define PXP2_REG_WR_DMAE_MPS                             0x1205ec
#define PXP2_REG_WR_HC_MPS                               0x1205c8
#define PXP2_REG_WR_USDM_MPS                             0x1205cc
#define PXP2_REG_WR_CSDM_MPS                             0x1205d0
#define PXP2_REG_WR_TSDM_MPS                             0x1205d4
#define PXP2_REG_WR_XSDM_MPS                             0x1205d8
#define PXP2_REG_WR_QM_MPS                               0x1205dc
#define PXP2_REG_WR_TM_MPS                               0x1205e0
#define PXP2_REG_WR_SRC_MPS                              0x1205e4
#define PXP2_REG_WR_DBG_MPS                              0x1205e8
#define PXP2_REG_WR_CDU_MPS                              0x1205f0
#define PXP2_REG_PGL_TAGS_LIMIT                          0x1205a8
#define PXP2_REG_PSWRQ_BW_RD                             0x120324
#define PXP2_REG_PSWRQ_BW_WR                             0x120328
#define PXP2_REG_RQ_QM_ENDIAN_M                          0x120194
#define PXP2_REG_RQ_TM_ENDIAN_M                          0x120198
#define PXP2_REG_RQ_SRC_ENDIAN_M                         0x12019c
#define PXP2_REG_RQ_CDU_ENDIAN_M                         0x1201a0
#define PXP2_REG_RQ_DBG_ENDIAN_M                         0x1201a4
#define PXP2_REG_RQ_HC_ENDIAN_M                          0x1201a8
#define PXP2_REG_RD_QM_SWAP_MODE                         0x1203f8
#define PXP2_REG_RD_TM_SWAP_MODE                         0x1203fc
#define PXP2_REG_RD_SRC_SWAP_MODE                        0x120400
#define PXP2_REG_RD_CDURD_SWAP_MODE                      0x120404
#define PXP2_REG_RQ_ONCHIP_AT_B0                         0x128000
#define PXP2_REG_RQ_CDU_FIRST_ILT                        0x12061c
#define PXP2_REG_RQ_CDU_LAST_ILT                         0x120620
#define PXP2_REG_RQ_QM_FIRST_ILT                         0x120634
#define PXP2_REG_RQ_QM_LAST_ILT                          0x120638
#define PXP2_REG_RQ_SRC_FIRST_ILT                        0x12063c
#define PXP2_REG_RQ_SRC_LAST_ILT                         0x120640
#define PXP2_REG_RQ_TM_FIRST_ILT                         0x120644
#define PXP2_REG_RQ_TM_LAST_ILT                          0x120648
#define PXP2_REG_RQ_CDU_P_SIZE                           0x120018
#define PXP2_REG_RQ_QM_P_SIZE                            0x120050
#define PXP2_REG_RQ_SRC_P_SIZE                           0x12006c
#define PXP2_REG_RQ_TM_P_SIZE                            0x120034
#define PXP2_REG_PXP2_INT_MASK_0                         0x120578
#define PXP2_REG_PXP2_INT_STS_CLR_0                      0x120570
#define PXP2_PXP2_INT_MASK_0_REG_PGL_CPL_AFT             (0x1<<19)
#define PXP2_PXP2_INT_MASK_0_REG_PGL_CPL_OF              (0x1<<20)
#define PXP2_PXP2_INT_MASK_0_REG_PGL_PCIE_ATTN           (0x1<<22)
#define PXP2_PXP2_INT_MASK_0_REG_PGL_READ_BLOCKED        (0x1<<23)
#define PXP2_PXP2_INT_MASK_0_REG_PGL_WRITE_BLOCKED       (0x1<<24)
#define ATC_REG_ATC_INIT_DONE                            0x1100bc
#define DMAE_REG_DMAE_INT_MASK                           0x102054
#define QM_REG_SOFT_RESET                                0x168428
#define QM_REG_CONNNUM_0                                 0x168020
#define QM_REG_PF_EN                                     0x16e70c
#define QM_REG_BASEADDR                                  0x168900
#define QM_REG_PTRTBL                                    0x168a00
#define QM_REG_QM_INT_MASK                               0x168444
#define TM_REG_TM_INT_MASK                               0x1640fc
#define DORQ_REG_DORQ_INT_MASK                           0x170180
#define DORQ_REG_MODE_ACT                                0x170008
#define BRB1_REG_BRB1_INT_MASK                           0x60128
#define BRB1_REG_MAC_GUARANTIED_0                        0x601e8
#define BRB1_REG_MAC_GUARANTIED_1                        0x60240
#define PRS_REG_A_PRSU_20                                0x40134
#define PRS_REG_E1HOV_MODE                               0x401c8
#define PRS_REG_NIC_MODE                                 0x40138
#define PRS_REG_HDRS_AFTER_BASIC_PORT_0                  0x40270
#define PRS_REG_HDRS_AFTER_BASIC_PORT_1                  0x40290
#define TSDM_REG_TSDM_INT_MASK_0                         0x4229c
#define TSDM_REG_TSDM_INT_MASK_1                         0x422ac
#define CSDM_REG_CSDM_INT_MASK_0                         0xc229c
#define CSDM_REG_CSDM_INT_MASK_1                         0xc22ac
#define USDM_REG_USDM_INT_MASK_0                         0xc42a0
#define USDM_REG_USDM_INT_MASK_1                         0xc42b0
#define XSDM_REG_XSDM_INT_MASK_0                         0x16629c
#define XSDM_REG_XSDM_INT_MASK_1                         0x1662ac
#define TCM_REG_TCM_INT_MASK                             0x501dc
#define CCM_REG_CCM_INT_MASK                             0xd01e4
#define UCM_REG_UCM_INT_MASK                             0xe01d4
#define XCM_REG_XCM_INT_MASK                             0x202b4
#define TSEM_REG_FAST_MEMORY                             0x1a0000
#define XSEM_REG_FAST_MEMORY                             0x2a0000
#define VFC_REG_MEMORIES_RST                             0x1943c
#define VFC_MEMORIES_RST_REG_CAM_RST                     (0x1<<0)
#define VFC_MEMORIES_RST_REG_RAM_RST                     (0x1<<1)
#define TSEM_REG_PASSIVE_BUFFER                          0x181000
#define CSEM_REG_PASSIVE_BUFFER                          0x202000
#define USEM_REG_PASSIVE_BUFFER                          0x302000
#define XSEM_REG_PASSIVE_BUFFER                          0x282000
#define TSEM_REG_TSEM_INT_MASK_1                         0x180110
#define TSEM_REG_VFPF_ERR_NUM                            0x180380
#define USEM_REG_VFPF_ERR_NUM                            0x300380
#define CSEM_REG_VFPF_ERR_NUM                            0x200380
#define XSEM_REG_VFPF_ERR_NUM                            0x280380
#define SRC_REG_SOFT_RST                                 0x4049c
#define CDU_REG_CDU_GLOBAL_PARAMS                        0x101020
#define CDU_REG_CDU_INT_MASK                             0x10103c
#define CFC_REG_INIT_REG                                 0x10404c
#define CFC_REG_CFC_INT_MASK                             0x104108
#define CFC_REG_DEBUG0                                   0x104050
#define CFC_REG_LL_INIT_DONE                             0x104074
#define CFC_REG_AC_INIT_DONE                             0x104078
#define CFC_REG_CAM_INIT_DONE                            0x10407c
#define PB_REG_PB_INT_MASK                               0x28
#define PBF_REG_PBF_INT_MASK                             0x1401d4
#define PBF_REG_DISABLE_PF                               0x1402e8
#define NIG_REG_MASK_INTERRUPT_PORT0                     0x10330
#define NIG_REG_LLH_MF_MODE                              0x16024
#define NIG_REG_LLH1_MF_MODE                             0x18614
#define NIG_REG_P0_HDRS_AFTER_BASIC                      0x18038
#define NIG_REG_P1_HDRS_AFTER_BASIC                      0x1818c
#define NIG_REG_LLH0_CLS_TYPE                            0x16080
#define NIG_REG_LLH1_CLS_TYPE                            0x16084
#define NIG_REG_LLFC_ENABLE_0                            0x16208
#define NIG_REG_LLFC_OUT_EN_0                            0x160c8
#define NIG_REG_PAUSE_ENABLE_0                           0x160c0
#define NIG_REG_LLH0_BRB1_DRV_MASK_MF                    0x16048
#define MISC_REG_AEU_MASK_ATTN_FUNC_0                    0xa060
#define MISC_REG_AEU_ENABLE4_NIG_0                       0xa0f8
#define MISC_REG_AEU_ENABLE4_NIG_1                       0xa198
#define MISC_REG_AEU_ENABLE4_PXP_0                       0xa108
#define MISC_REG_AEU_ENABLE4_PXP_1                       0xa1a8
#define AEU_INPUTS_ATTN_BITS_MCP_LATCHED_SCPAD_PARITY    (0x1U<<31)
#define MISC_REG_AEU_GENERAL_ATTN_12                     0xa030
#define MISC_REG_SPIO_EVENT_EN                           0xa2b8
#define MISC_SPIO_SPIO5                                  0x20
#define AEU_INPUTS_ATTN_BITS_SPIO5                       (0x1<<15)
#define MISC_REG_AEU_ENABLE1_FUNC_0_OUT_0                0xa06c
#define MISC_REG_AEU_ENABLE1_FUNC_1_OUT_0                0xa10c
#define IGU_REG_BLOCK_CONFIGURATION                      0x130000
#define IGU_BLOCK_CONFIGURATION_REG_BACKWARD_COMP_EN     (0x1<<1)
#define IGU_REG_RESET_MEMORIES                           0x130158
#define IGU_REG_MAPPING_MEMORY                           0x131000
#define IGU_REG_MAPPING_MEMORY_SIZE                      136
#define IGU_REG_MAPPING_MEMORY_VALID                     (1<<0)
#define IGU_REG_PROD_CONS_MEMORY                         0x132000
#define IGU_REG_PF_CONFIGURATION                         0x130154
#define IGU_PF_CONF_FUNC_EN                              (0x1<<0)  /* function enable	      */
#define IGU_PF_CONF_SINGLE_ISR_EN                        (0x1<<4)  /* single ISR mode enable */
#define IGU_REG_SB_INT_BEFORE_MASK_LSB                   0x13015c
#define IGU_REG_SB_INT_BEFORE_MASK_MSB                   0x130160
#define IGU_REG_SB_MASK_LSB                              0x130164
#define IGU_REG_SB_MASK_MSB                              0x130168
#define IGU_REG_PBA_STATUS_LSB                           0x130138
#define IGU_REG_PBA_STATUS_MSB                           0x13013c
#define IGU_REG_LEADING_EDGE_LATCH                       0x130134
#define IGU_REG_TRAILING_EDGE_LATCH                      0x130104
#define IGU_REG_COMMAND_REG_32LSB_DATA                   0x130124
#define IGU_REG_COMMAND_REG_CTRL                         0x13012c
#define IGU_REG_CSTORM_TYPE_0_SB_CLEANUP                 0x130200
#define IGU_REGULAR_CLEANUP_TYPE_SHIFT                   28
#define IGU_REGULAR_CLEANUP_SET                          (0x1<<30)
#define IGU_REGULAR_BCLEANUP                             (0x1<<31)
#define IGU_CTRL_REG_ADDRESS_SHIFT                       0
#define IGU_CTRL_REG_FID_SHIFT                           12
#define IGU_CTRL_REG_TYPE_SHIFT                          20
#define IGU_REGULAR_SB_INDEX_SHIFT                       0
#define IGU_REGULAR_SEGMENT_ACCESS_SHIFT                 21
#define IGU_REGULAR_BUPDATE_SHIFT                        24
#define IGU_REGULAR_ENABLE_INT_SHIFT                     25
#define IGU_FID_ENCODE_IS_PF                             (0x1<<6)
#define IGU_FID_ENCODE_IS_PF_SHIFT                       6
#define IGU_FID_PF_NUM_MASK                              (0x7)
#define IGU_FID_VF_NUM_MASK                              (0x3f)
#define BAR_IGU_INTMEM                                   0x440000
#define IGU_CMD_INT_ACK_BASE                             0x0400
#define IGU_CMD_E2_PROD_UPD_BASE                         0x0500
#define IGU_NORM_BASE_DSB_PROD                           136
#define IGU_NORM_NDSB_NUM_SEGS                           1
#define IGU_NORM_DSB_NUM_SEGS                            2
#define PXPCS_TL_CONTROL_5                               0x814
#define PXPCS_TL_CONTROL_5_ERR_UNSPPORT1                 (1 << 18)   /*WC*/
#define PXPCS_TL_CONTROL_5_ERR_UNSPPORT                  (1 << 8)    /*WC*/
#define PXPCS_TL_FUNC345_STAT                            0x854
#define PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT4              (1 << 28) /* Unsupported Request Error Status in function4, if  set, generate pcie_err_attn output when this error is seen. WC */
#define PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT3              (1 << 18) /* Unsupported Request Error Status in function3, if  set, generate pcie_err_attn output when this error is seen. WC */
#define PXPCS_TL_FUNC345_STAT_ERR_UNSPPORT2              (1 << 8) /* Unsupported Request Error Status for Function 2, if  set, generate pcie_err_attn output when this error is seen. WC */
#define PXPCS_TL_FUNC678_STAT                            0x85C
#define PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT7              (1 << 28) /* Unsupported Request Error Status in function7, if  set, generate pcie_err_attn output when this error is seen. WC */
#define PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT6              (1 << 18) /* Unsupported Request Error Status in function6, if  set, generate pcie_err_attn output when this error is seen. WC */
#define PXPCS_TL_FUNC678_STAT_ERR_UNSPPORT5              (1 << 8) /* Unsupported Request Error Status for Function 5, if  set, generate pcie_err_attn output when this error is seen. WC */

/* PXP2 read/write arbiter registers */
#define PXP2_REG_PSWRQ_BW_ADD1                   0x1201c0
#define PXP2_REG_PSWRQ_BW_ADD10                  0x1201e4
#define PXP2_REG_PSWRQ_BW_ADD11                  0x1201e8
#define PXP2_REG_PSWRQ_BW_ADD2                   0x1201c4
#define PXP2_REG_PSWRQ_BW_ADD28                  0x120228
#define PXP2_REG_PSWRQ_BW_ADD3                   0x1201c8
#define PXP2_REG_PSWRQ_BW_ADD6                   0x1201d4
#define PXP2_REG_PSWRQ_BW_ADD7                   0x1201d8
#define PXP2_REG_PSWRQ_BW_ADD8                   0x1201dc
#define PXP2_REG_PSWRQ_BW_ADD9                   0x1201e0
#define PXP2_REG_PSWRQ_BW_CREDIT                 0x12032c
#define PXP2_REG_PSWRQ_BW_L1                     0x1202b0
#define PXP2_REG_PSWRQ_BW_L10                    0x1202d4
#define PXP2_REG_PSWRQ_BW_L11                    0x1202d8
#define PXP2_REG_PSWRQ_BW_L2                     0x1202b4
#define PXP2_REG_PSWRQ_BW_L28                    0x120318
#define PXP2_REG_PSWRQ_BW_L3                     0x1202b8
#define PXP2_REG_PSWRQ_BW_L6                     0x1202c4
#define PXP2_REG_PSWRQ_BW_L7                     0x1202c8
#define PXP2_REG_PSWRQ_BW_L8                     0x1202cc
#define PXP2_REG_PSWRQ_BW_L9                     0x1202d0
#define PXP2_REG_PSWRQ_BW_RD                     0x120324
#define PXP2_REG_PSWRQ_BW_UB1                    0x120238
#define PXP2_REG_PSWRQ_BW_UB10                   0x12025c
#define PXP2_REG_PSWRQ_BW_UB11                   0x120260
#define PXP2_REG_PSWRQ_BW_UB2                    0x12023c
#define PXP2_REG_PSWRQ_BW_UB28                   0x1202a0
#define PXP2_REG_PSWRQ_BW_UB3                    0x120240
#define PXP2_REG_PSWRQ_BW_UB6                    0x12024c
#define PXP2_REG_PSWRQ_BW_UB7                    0x120250
#define PXP2_REG_PSWRQ_BW_UB8                    0x120254
#define PXP2_REG_PSWRQ_BW_UB9                    0x120258
#define PXP2_REG_PSWRQ_BW_WR                     0x120328
#define PXP2_REG_RQ_BW_RD_ADD0                   0x1201bc
#define PXP2_REG_RQ_BW_RD_ADD12                  0x1201ec
#define PXP2_REG_RQ_BW_RD_ADD13                  0x1201f0
#define PXP2_REG_RQ_BW_RD_ADD14                  0x1201f4
#define PXP2_REG_RQ_BW_RD_ADD15                  0x1201f8
#define PXP2_REG_RQ_BW_RD_ADD16                  0x1201fc
#define PXP2_REG_RQ_BW_RD_ADD17                  0x120200
#define PXP2_REG_RQ_BW_RD_ADD18                  0x120204
#define PXP2_REG_RQ_BW_RD_ADD19                  0x120208
#define PXP2_REG_RQ_BW_RD_ADD20                  0x12020c
#define PXP2_REG_RQ_BW_RD_ADD22                  0x120210
#define PXP2_REG_RQ_BW_RD_ADD23                  0x120214
#define PXP2_REG_RQ_BW_RD_ADD24                  0x120218
#define PXP2_REG_RQ_BW_RD_ADD25                  0x12021c
#define PXP2_REG_RQ_BW_RD_ADD26                  0x120220
#define PXP2_REG_RQ_BW_RD_ADD27                  0x120224
#define PXP2_REG_RQ_BW_RD_ADD4                   0x1201cc
#define PXP2_REG_RQ_BW_RD_ADD5                   0x1201d0
#define PXP2_REG_RQ_BW_RD_L0                     0x1202ac
#define PXP2_REG_RQ_BW_RD_L12                    0x1202dc
#define PXP2_REG_RQ_BW_RD_L13                    0x1202e0
#define PXP2_REG_RQ_BW_RD_L14                    0x1202e4
#define PXP2_REG_RQ_BW_RD_L15                    0x1202e8
#define PXP2_REG_RQ_BW_RD_L16                    0x1202ec
#define PXP2_REG_RQ_BW_RD_L17                    0x1202f0
#define PXP2_REG_RQ_BW_RD_L18                    0x1202f4
#define PXP2_REG_RQ_BW_RD_L19                    0x1202f8
#define PXP2_REG_RQ_BW_RD_L20                    0x1202fc
#define PXP2_REG_RQ_BW_RD_L22                    0x120300
#define PXP2_REG_RQ_BW_RD_L23                    0x120304
#define PXP2_REG_RQ_BW_RD_L24                    0x120308
#define PXP2_REG_RQ_BW_RD_L25                    0x12030c
#define PXP2_REG_RQ_BW_RD_L26                    0x120310
#define PXP2_REG_RQ_BW_RD_L27                    0x120314
#define PXP2_REG_RQ_BW_RD_L4                     0x1202bc
#define PXP2_REG_RQ_BW_RD_L5                     0x1202c0
#define PXP2_REG_RQ_BW_RD_UBOUND0                0x120234
#define PXP2_REG_RQ_BW_RD_UBOUND12               0x120264
#define PXP2_REG_RQ_BW_RD_UBOUND13               0x120268
#define PXP2_REG_RQ_BW_RD_UBOUND14               0x12026c
#define PXP2_REG_RQ_BW_RD_UBOUND15               0x120270
#define PXP2_REG_RQ_BW_RD_UBOUND16               0x120274
#define PXP2_REG_RQ_BW_RD_UBOUND17               0x120278
#define PXP2_REG_RQ_BW_RD_UBOUND18               0x12027c
#define PXP2_REG_RQ_BW_RD_UBOUND19               0x120280
#define PXP2_REG_RQ_BW_RD_UBOUND20               0x120284
#define PXP2_REG_RQ_BW_RD_UBOUND22               0x120288
#define PXP2_REG_RQ_BW_RD_UBOUND23               0x12028c
#define PXP2_REG_RQ_BW_RD_UBOUND24               0x120290
#define PXP2_REG_RQ_BW_RD_UBOUND25               0x120294
#define PXP2_REG_RQ_BW_RD_UBOUND26               0x120298
#define PXP2_REG_RQ_BW_RD_UBOUND27               0x12029c
#define PXP2_REG_RQ_BW_RD_UBOUND4                0x120244
#define PXP2_REG_RQ_BW_RD_UBOUND5                0x120248
#define PXP2_REG_RQ_BW_WR_ADD29                  0x12022c
#define PXP2_REG_RQ_BW_WR_ADD30                  0x120230
#define PXP2_REG_RQ_BW_WR_L29                    0x12031c
#define PXP2_REG_RQ_BW_WR_L30                    0x120320
#define PXP2_REG_RQ_BW_WR_UBOUND29               0x1202a4
#define PXP2_REG_RQ_BW_WR_UBOUND30               0x1202a8

/* Additional constants used by the init code */
#define SHARED_HW_CFG_FAN_FAILURE_MASK		0x00180000
#define SHARED_HW_CFG_FAN_FAILURE_ENABLED	0x00100000
#define BNX2X_SHMEM_HW_CONFIG2			0x0020
#define BNX2X_SHMEM2_LFA_HOST_ADDR( port )	( 0x013c + ( (port) * 4 ) )
#define IGU_CTRL_CMD_TYPE_WR			1
#define IGU_SEG_ACCESS_DEF			1
#define IGU_SEG_ACCESS_ATTN			2
#define IGU_INT_NOP				2
#define ATTENTION_ID				4
#define E2_FUNC_MAX				4
#define IGU_USE_REGISTER_cstorm_type_0_sb_cleanup 2

/* ILT geometry (Linux bnx2x.h) */
#define ILT_NUM_PAGE_ENTRIES			3072
#define ILT_PER_FUNC				( ILT_NUM_PAGE_ENTRIES / 8 )
#define FUNC_ILT_BASE( func )			( (func) * ILT_PER_FUNC )
#define ILT_ADDR1( x )				( ( (uint32_t) ( ( ( uint64_t ) (x) ) >> 12 ) ) )
#define ILT_ADDR2( x )				( ( uint32_t ) ( ( 1 << 20 ) | ( ( ( uint64_t ) (x) ) >> 44 ) ) )

/* ILT client geometry for this driver: one 32kB CDU context page and
 * sixteen 4kB QM pages (qm_cid_count=1024), see bnx2x_hw.c
 */
#define BNX2X_CDU_PAGE_SIZE			0x8000
#define BNX2X_QM_PAGE_SIZE			0x1000
#define BNX2X_QM_PAGES				16
#define BNX2X_QM_CID_COUNT			1024
#define BNX2X_QM_QUEUES_PER_FUNC		16

/* Timers-workaround DRAM alignment value (BNX2X_RX_ALIGN_SHIFT(6) - 5) */
#define BNX2X_PXP_DRAM_ALIGN			1

#define IGU_REG_ATTN_MSG_ADDR_H			0x13011c
#define IGU_REG_ATTN_MSG_ADDR_L			0x130120

/* XMAC (E3 10/20G MAC) registers (from Linux bnx2x_reg.h) */
#define GRCBASE_XMAC0				0x163000
#define GRCBASE_XMAC1				0x163800
#define XMAC_REG_CTRL				0x00
#define XMAC_CTRL_REG_TX_EN			0x01
#define XMAC_CTRL_REG_RX_EN			0x02
#define XMAC_CTRL_REG_LINE_LOCAL_LPBK		0x04
#define XMAC_REG_TX_CTRL			0x20
#define XMAC_REG_CTRL_SA_LO			0x28
#define XMAC_REG_CTRL_SA_HI			0x2c
#define XMAC_REG_RX_MAX_SIZE			0x40
#define XMAC_REG_RX_LSS_CTRL			0x50
#define XMAC_RX_LSS_CTRL_REG_LOCAL_FAULT_DISABLE	0x01
#define XMAC_RX_LSS_CTRL_REG_REMOTE_FAULT_DISABLE	0x02
#define XMAC_REG_CLEAR_RX_LSS_STATUS		0x60
#define XMAC_REG_PAUSE_CTRL			0x68
#define XMAC_REG_PFC_CTRL			0x70
#define XMAC_REG_PFC_CTRL_HI			0x74
#define XMAC_REG_EEE_CTRL			0xd8
#define MISC_REG_RESET_REG_2			0xa590
#define MISC_REGISTERS_RESET_REG_2_XMAC		( 0x1 << 22 )
#define MISC_REGISTERS_RESET_REG_2_XMAC_SOFT	( 0x1 << 23 )
#define MISC_REG_XMAC_PHY_PORT_MODE		0xa960
#define MISC_REG_XMAC_CORE_PORT_MODE		0xa964
#define NIG_REG_EGRESS_EMAC0_PORT		0x10058
#define NIG_REG_EGRESS_DRAIN0_MODE		0x10060
#define NIG_REG_LLH0_BRB1_DRV_MASK		0x10244
#define NIG_REG_LLH0_BRB1_NOT_MCP		0x1025c
#define NIG_REG_LLH1_BRB1_NOT_MCP		0x102dc
#define NIG_REG_P0_MAC_IN_EN			0x185ac
#define NIG_REG_P0_MAC_OUT_EN			0x185b0
#define NIG_REG_P0_MAC_PAUSE_OUT_EN		0x185b4
#define NIG_REG_P1_MAC_IN_EN			0x185c0
#define NIG_REG_P1_MAC_OUT_EN			0x185c4
#define NIG_REG_P1_MAC_PAUSE_OUT_EN		0x185c8

/* Storm internal memory BAR0 windows */
#define BAR_USTRORM_INTMEM			0x400000
#define BAR_CSTRORM_INTMEM			0x410000
#define BAR_XSTRORM_INTMEM			0x420000
#define BAR_TSTRORM_INTMEM			0x430000

/* IGU CAM mapping entry fields */
#define IGU_REG_MAPPING_MEMORY_VECTOR_MASK	(0x3F<<1)
#define IGU_REG_MAPPING_MEMORY_VECTOR_SHIFT	1
#define IGU_REG_MAPPING_MEMORY_FID_MASK		(0x7F<<7)
#define IGU_REG_MAPPING_MEMORY_FID_SHIFT	7

extern int bnx2x_hw_init ( struct bnx2x_nic *bnx2x );
extern void bnx2x_hw_free ( struct bnx2x_nic *bnx2x );
extern void bnx2x_xmac_enable ( struct bnx2x_nic *bnx2x,
				const uint8_t *mac );
extern void bnx2x_rx_diag ( struct bnx2x_nic *bnx2x );

#define NIG_REG_STAT0_BRB_TRUNCATE		0x105f8
#define NIG_REG_STAT0_BRB_DISCARD		0x105f0
#define NIG_REG_STAT0_EGRESS_MAC_PKT0		0x10750
#define BRB1_REG_NUM_OF_FULL_BLOCKS		0x60090
#define PRS_REG_NUM_OF_PACKETS			0x40124
extern int bnx2x_igu_info ( struct bnx2x_nic *bnx2x );
extern void bnx2x_igu_ack_sb ( struct bnx2x_nic *bnx2x,
			       unsigned int igu_sb_id, unsigned int segment,
			       uint16_t index, unsigned int op,
			       unsigned int update );

#endif /* _BNX2X_HW_H */
