/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 64

volatile int tot, tmpc, txcnt;
int T = 10000;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;

	if (port >= rte_eth_dev_count())
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

inline void printMac(unsigned char * d) {
	int i;
	for (i=0;i<6;++i)
		printf("\x1b[33m%02x \x1b[0m", d[i]);
}

inline void printIP(unsigned char * d) {
	printf("\x1b[33m%3d.%3d.%3d.%3d \x1b[0m",*d,*(d+1),*(d+2),*(d+3));
}

#define black_list_cnt 2

unsigned char black_list[black_list_cnt][4] = {{113,244,103,5}, {202,128,166,7}};

inline int IPcmp(unsigned char * x, unsigned char * y) {
	return !(*x==*y&&*(x+1)==*(y+1)&&*(x+2)==*(y+2)&&*(x+3)==*(y+3));
}

uint16_t _FireWall(struct rte_mbuf * b[], uint16_t n) {
	uint16_t i, j, k;
	k = 0;
	for (i=0;i<n;++i) {
		unsigned char * d = (unsigned char *)b[i];
		d = d+256+14;
		int flag = 1;
		for (j=0;j<black_list_cnt&&flag;++j) {
			if (IPcmp(d+12,(unsigned char *)black_list[j])==0) {
				flag = 0;
				printIP(d+12);
				printf(" in black list.\n");
			}
		}
		if (flag) {
			memcpy(b[k++],b[i],sizeof(struct rte_mbuf));
		}
	}
	return k;
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static __attribute__((noreturn)) void
lcore_main(void)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	for (port = 0; port < nb_ports; port++)
		if (rte_eth_dev_socket_id(port) > 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());

	/* Run until the application is quit or killed. */
	for (;;) {
		/*
		 * Receive packets on a port and forward them on the paired
		 * port. The mapping is 0 -> 1, 1 -> 0, 2 -> 3, 3 -> 2, etc.
		 */
		for (port = 0; port < nb_ports; port++) {

			/* Get burst of RX packets, from first port of pair. */
			struct rte_mbuf *bufs[BURST_SIZE];
			uint16_t nb_rx = rte_eth_rx_burst(port, 0,
					bufs, BURST_SIZE);
			
			int k = rand()%10;
			if (!k&&nb_rx>0) {
				printf("\n[%u] recv: %u\n",port, nb_rx);
				int i;
				//struct ether_hdr *eth = rte_pktmbuf_mtod((struct rte_mbuf *)(((unsigned char *)(bufs[0]))+128), struct ether_hdr *);
				unsigned char * d = ((unsigned char *)(bufs[0]))+256;
				puts("\nPkt:\n");
				printf("Dst MAC: ");
				printMac(d);
				printf("Src MAC: ");
				printMac(d+6);
				unsigned char * ipd = d+14;
				printf("IPv%d len: %d ",*ipd>>4, (*(ipd+2)<<8)|(*(ipd+3)));
				printf("src IP: ");
				printIP(ipd+12);
				printf("dst IP: ");
				printIP(ipd+16);
				printf("protocol: %d, serv.: %x", *(ipd + 9), *(ipd + 1));
				printf("\n\x1b[0m");
				for (i = 0; i < 34; ++i) printf("\x1b[33m%02x \x1b[0m",d[i]);
				printf("\n\x1b[0m");
			}
			
			if (unlikely(nb_rx == 0))
				continue;
			else {
				tot+=nb_rx;
				tmpc+=nb_rx;
			}

			/* Send burst of TX packets, to second port of pair. */
			printf("\ntest: resend: %u\n", clock());
			uint16_t nb_tx_test = rte_eth_tx_burst(port, 0,
					bufs, nb_rx);
			int h_send = nb_tx_test;
			while (h_send<nb_rx) {
					nb_tx_test = rte_eth_tx_burst(port, 0,
					bufs + h_send, nb_rx - h_send);
					h_send+=nb_tx_test;
			}
			printf("test: after %u resend: %u\n", nb_tx_test, clock());
			unsigned int st = clock();
			printf("\nbefore resend: %u\n", st);
			int ci;
			for (ci = 0;ci<T;++ci) {
				int i;
				for (i = 0; i < nb_rx; ++i) {
					unsigned char * d = ((unsigned char *)bufs[i]) + 256;
					*(d+14+1) = rand()%256;
				}
			}
			h_send = nb_rx;
			nb_rx = _FireWall(bufs, nb_rx);
			printf("%d Pkts have been dropped.\n", h_send - nb_rx);
			unsigned int en = clock();
			printf("modified: %u %u\n", en - st, en);
			uint16_t nb_tx = rte_eth_tx_burst(port, 0,
					bufs, nb_rx);
			h_send = nb_tx;
			while (h_send<nb_rx) {
					nb_tx = rte_eth_tx_burst(port, 0,
					bufs+h_send, nb_rx-h_send);
				h_send+=nb_tx;
			}
			printf("after %u resend: %u\n", nb_tx, clock());
			txcnt+=nb_tx;

			/* Free any unsent packets. */
			if (unlikely(0 < nb_rx)) {
				uint16_t buf;
				for (buf = 0; buf < nb_rx; buf++)
					rte_pktmbuf_free(bufs[buf]);
			}
		}
	}
}

void * disp_loop(void * x) {
	int ts=0, ss=0;
	tot=0; txcnt = 0;
	for (;;) {
		tmpc=0;
		usleep(1000);
		ts=1000;
		ss+=1000;
		printf("\r\x1b[?25lreal time: \x1b[34m%10.2f\x1b[0m Pkts/s,\t avg: \x1b[34m%10.2f\x1b[0m Pkts/s total recv: \x1b[34m%d\x1b[0m Pkts, resend: %d", (float)tmpc/ts*1000000, (float)tot/ss*1000000, tot, txcnt);
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint8_t portid;
	
	if (argc>0) T=atoi(argv[1]);
	strcpy(argv[1],argv[0]); // unsafe, maybe make some mistakes
	--argc;
	argv++;
	printf("[%s] mod %d times.\n",argv[0],T);

	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	//if (nb_ports < 2 || (nb_ports & 1))
	//	rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	printf("socket_id: %u ports: %u\n", rte_socket_id(), nb_ports);
	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	//if (rte_lcore_count() > 1)
	//	printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the master core only. */
	pthread_t disp;
	int sig = 1;
	pthread_create(&disp, NULL, disp_loop, &sig);
	lcore_main();
	pthread_join(disp, NULL);

	return 0;
}
