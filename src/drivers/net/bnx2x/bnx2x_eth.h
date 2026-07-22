#ifndef _BNX2X_ETH_H
#define _BNX2X_ETH_H

/** @file
 *
 * bnx2x Ethernet datapath
 *
 */

FILE_LICENCE ( GPL2_ONLY );

#include <stdint.h>

struct bnx2x_nic;
struct net_device;
struct io_buffer;

extern int bnx2x_eth_open ( struct net_device *netdev );
extern void bnx2x_eth_close ( struct net_device *netdev );
extern void bnx2x_eth_free ( struct bnx2x_nic *bnx2x );
extern int bnx2x_eth_transmit ( struct net_device *netdev,
				struct io_buffer *iobuf );
extern void bnx2x_eth_poll ( struct net_device *netdev );

#endif /* _BNX2X_ETH_H */
