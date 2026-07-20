/*
 * Host-side diagnostic register dump for bnx2x devices (debug aid,
 * not part of the iPXE build; remove before upstreaming).
 *
 * Dumps the same registers as the iPXE driver's RXDIAG output, but
 * from a running Linux system - run it while the in-kernel bnx2x
 * driver has the interface up and receiving, to capture the state of
 * a WORKING datapath for comparison.
 *
 * Build:   gcc -O2 -o bnx2xdump hostdump.c
 * Run:     sudo ./bnx2xdump /sys/bus/pci/devices/0000:01:00.0/resource0
 *          (use the PCI address of the port under test, e.g. from
 *          "ethtool -i <ifname>"; port 1 functions use the same
 *          registers at the port-1 addresses - this tool assumes
 *          port 0, matching the iPXE diagnostics)
 *
 * NOTE: reading MSTAT counters is CLEAR-ON-READ and will disturb the
 * in-kernel driver's statistics slightly; harmless for debugging.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BAR0_SIZE 0x800000

static volatile uint32_t *bar;

static uint32_t rd ( uint32_t off ) {
	return bar[off / 4];
}

int main ( int argc, char **argv ) {
	unsigned int i;
	int fd;

	if ( argc != 2 ) {
		fprintf ( stderr, "usage: %s <pci resource0 path>\n",
			  argv[0] );
		return 1;
	}
	fd = open ( argv[1], O_RDWR | O_SYNC );
	if ( fd < 0 ) {
		perror ( "open" );
		return 1;
	}
	bar = mmap ( NULL, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0 );
	if ( bar == MAP_FAILED ) {
		perror ( "mmap" );
		return 1;
	}

	printf ( "RXDIAG gates drv_mask %08x mf %08x not_mcp %08x mf_mode "
		 "%08x cls %08x func_en %08x hdrs %08x mac_in %08x "
		 "mac_out %08x\n",
		 rd ( 0x10244 ), rd ( 0x16048 ), rd ( 0x1025c ),
		 rd ( 0x16024 ), rd ( 0x16080 ), rd ( 0x160fc ),
		 rd ( 0x18038 ), rd ( 0x185ac ), rd ( 0x185b0 ) );
	printf ( "RXDIAG ifs brb0_out %d prs_req_in %d prs_eop_out %d "
		 "drain %d llh_fifo_empty %08x eop_empty %08x rmp_empty "
		 "%08x brb_occ0 %d xmac_ctrl %08x\n",
		 rd ( 0x100f8 ), rd ( 0x100b8 ), rd ( 0x10104 ),
		 rd ( 0x10060 ), rd ( 0x10548 ), rd ( 0x104ec ),
		 rd ( 0x10530 ), rd ( 0x60094 ), rd ( 0x163000 ) );
	printf ( "RXDIAG sts rx_macfifo_empty %08x int0 %08x int1 %08x "
		 "prty0 %08x prty1 %08x\n",
		 rd ( 0x18570 ), rd ( 0x103b0 ), rd ( 0x103c0 ),
		 rd ( 0x183bc ), rd ( 0x183cc ) );
	printf ( "RXDIAG misc port_swap %d strap_ovr %d\n",
		 rd ( 0x10394 ), rd ( 0x10398 ) );
	printf ( "RXDIAG storm prs_int %08x prs_dead %d tcm_int %08x "
		 "tsdm_int %08x tsdm_en1 %08x cfc_int %08x cfc_err %08x "
		 "lcids arr %d alloc %d leave %d inside_pf %d\n",
		 rd ( 0x40188 ), rd ( 0x40130 ), rd ( 0x501d0 ),
		 rd ( 0x42290 ), rd ( 0x42238 ), rd ( 0x1040fc ),
		 rd ( 0x10403c ), rd ( 0x104004 ), rd ( 0x104020 ),
		 rd ( 0x104018 ), rd ( 0x104120 ) );
	printf ( "RXDIAG legacy emac0_en %d emac0_in %d bmac0_in %d "
		 "bmac0_out %d bmac0_regs_out %d egress_emac0_out %d "
		 "no_crc %d\n",
		 rd ( 0x1003c ), rd ( 0x100a4 ), rd ( 0x100ac ),
		 rd ( 0x100e0 ), rd ( 0x100e8 ), rd ( 0x10120 ),
		 rd ( 0x10044 ) );
	printf ( "RXDIAG prs_packets %d (CLEAR-ON-READ - run twice with "
		 "traffic between)\n", rd ( 0x40124 ) );
	printf ( "RXDIAG xmac0" );
	for ( i = 0 ; i < 128 ; i++ )
		printf ( " %08x", rd ( 0x163000 + ( i * 4 ) ) );
	printf ( "\n" );
	printf ( "RXDIAG nig_p0_18000" );
	for ( i = 0 ; i < 64 ; i++ )
		printf ( " %08x", rd ( 0x18000 + ( i * 4 ) ) );
	printf ( "\n" );
	printf ( "RXDIAG nig_p0_18400" );
	for ( i = 0 ; i < 128 ; i++ )
		printf ( " %08x", rd ( 0x18400 + ( i * 4 ) ) );
	printf ( "\n" );
	printf ( "RXDIAG nig_100xx" );
	for ( i = 0 ; i < 128 ; i++ )
		printf ( " %08x", rd ( 0x10000 + ( i * 4 ) ) );
	printf ( "\n" );
	printf ( "RXDIAG misc_a700" );
	for ( i = 0 ; i < 64 ; i++ )
		printf ( " %08x", rd ( 0xa700 + ( i * 4 ) ) );
	printf ( "\n" );
	printf ( "RXDIAG misc_a900" );
	for ( i = 0 ; i < 64 ; i++ )
		printf ( " %08x", rd ( 0xa900 + ( i * 4 ) ) );
	printf ( "\n" );

	munmap ( ( void * ) bar, BAR0_SIZE );
	close ( fd );
	return 0;
}
