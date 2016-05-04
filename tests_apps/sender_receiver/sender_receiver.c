#include <getopt.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include <rte_mbuf.h>
#include <rte_memzone.h>
#include <rte_memcpy.h>
#include <rte_ethdev.h>
#include <rte_config.h>
#include <rte_memzone.h>
#include <rte_mempool.h>

#define RTE_LOGTYPE_APP         RTE_LOGTYPE_USER1

#define PKT_SIZE 64
#define MBUF_SIZE (PKT_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)

#define BURST_SIZE 32

/* function prototypes */
void send_receive_loop(void);
void init(char * port1, char * port2);
void crtl_c_handler(int s);
inline int send_packets(struct rte_mbuf ** packets);

volatile sig_atomic_t stop;
volatile sig_atomic_t pause_;
void crtl_c_handler(int s);

/* Per-port statistics struct */
struct port_statistics {
	uint32_t tx;
	uint32_t rx;
	uint32_t tx_retries;
	uint32_t alloc_fails;
} __rte_cache_aligned;

//Allocation methods
#define ALLOC 		1	/* allocate and deallocate packets */
#define NO_ALLOC 	2	/* send the same packets always*/

//Sending methods
#define RING 		1	/* send packets to rte_rings */
#define ETHERNET	2	/* send packets to network devices */

#define ALLOC_METHOD NO_ALLOC
#define SEND_MODE ETHERNET

struct rte_mempool * packets_pool = NULL;

struct port_statistics stats;

#if SEND_MODE == RING
struct rte_ring *rx_ring = NULL;
struct rte_ring *tx_ring = NULL;

#elif SEND_MODE == ETHERNET

uint8_t txport;
uint8_t rxport;

struct rte_eth_dev_info dev_info;
/* TODO: verify this setup */
static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split = 0,
		.hw_ip_checksum = 0,
		.hw_vlan_filter = 0,
		.jumbo_frame = 0,
		.hw_strip_crc = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static uint16_t nb_rxd = 128;
static uint16_t nb_txd = 512;
#endif

int main(int argc, char *argv[])
{
	int retval = 0;

	if ((retval = rte_eal_init(argc, argv)) < 0)
		return -1;

	argc -= retval;
	argv +=  retval;

	if(argc < 2)
	{
		RTE_LOG(INFO, APP, "usage: -- tx rx\n");
		return 0;
	}

	init(argv[1], argv[2]);

	RTE_LOG(INFO, APP, "Finished Process Init.\n");

/* Print information about all the flags! */

	send_receive_loop();	//Forward packets...

	RTE_LOG(INFO, APP, "Done\n");
	return 0;
}

#if SEND_MODE == RING
void init(char * tx, char * rx)
{
	char ring_tx[RTE_RING_NAMESIZE];
	char ring_rx[RTE_RING_NAMESIZE];

	/* be aware that ring name is in ovs point of view */
	sprintf(ring_rx, "%s_tx", rx);
	sprintf(ring_tx, "%s_rx", tx);

	if ((rx_ring = rte_ring_lookup(ring_rx)) == NULL)
	{
		rte_exit(EXIT_FAILURE, "Cannot find RX ring: %s\n", ring_rx);
	}

	if ((tx_ring = rte_ring_lookup(ring_tx)) == NULL)
	{
		rte_exit(EXIT_FAILURE, "Cannot find TX ring: %s\n", ring_tx);
	}
}
#elif SEND_MODE == ETHERNET

void init(char * tx, char * rx)
{
	int ret;

	/** first port **/

	/* XXX: is there a better way to get the port id based on the name? */
	txport = atoi(tx);

	/* TODO: verify memory pool creation options */
	packets_pool = rte_pktmbuf_pool_create("packets", 256*1024, 32,
		0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	//packets_pool = rte_mempool_create("packets",
	//									256*1024,
	//									MBUF_SIZE,
	//									32,	/*cache size */
	//									sizeof(struct rte_pktmbuf_pool_private),
	//									rte_pktmbuf_pool_init, NULL,
	//									rte_pktmbuf_init, NULL,
	//									rte_socket_id(), 0);

	if(packets_pool == NULL)
	{
		rte_exit(EXIT_FAILURE, "Cannot find memory pool\n");
	}

	rte_eth_dev_info_get(txport, &dev_info);

	ret = rte_eth_dev_configure(txport, 1, 1, &port_conf);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device\n");

	ret = rte_eth_tx_queue_setup(txport, 0, nb_txd,
								SOCKET_ID_ANY/*rte_eth_dev_socket_id(portid1)*/, NULL);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device tx queue\n");

	ret = rte_eth_rx_queue_setup(txport, 0, nb_rxd,
			rte_eth_dev_socket_id(txport), NULL, packets_pool);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device rx queue\n");

	ret = rte_eth_dev_start(txport);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot start device\n");

	rte_eth_promiscuous_enable(txport);

	/** second port **/

	/* XXX: is there a better way to get the port id based on the name? */
	rxport = atoi(rx);

	if(packets_pool == NULL)
	{
		rte_exit(EXIT_FAILURE, "Cannot find memory pool\n");
	}

	rte_eth_dev_info_get(rxport, &dev_info);

	ret = rte_eth_dev_configure(rxport, 1, 1, &port_conf);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device\n");

	ret = rte_eth_tx_queue_setup(rxport, 0, nb_txd,
								SOCKET_ID_ANY/*rte_eth_dev_socket_id(portid2)*/, NULL);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device tx queue\n");

	ret = rte_eth_rx_queue_setup(rxport, 0, nb_rxd,
			rte_eth_dev_socket_id(rxport), NULL, packets_pool);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device rx queue\n");

	ret = rte_eth_dev_start(rxport);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot start device\n");

	rte_eth_promiscuous_enable(rxport);

}
#endif

/* send packets */
inline int send_packets(struct rte_mbuf ** packets)
{
	int i = 0;
#if SEND_MODE == RING
	#ifdef SEND_FULL_BURST
	int ntosend = BURST_SIZE;
	do
	{
		#ifdef CALC_TX_TRIES
		stats.tx_retries++;
		#endif

		i += rte_ring_sp_enqueue_burst(tx_ring, (void **) &packets[i], ntosend - i);
		if(unlikely(stop))
			break;
	} while(unlikely(i < ntosend));
	return BURST_SIZE;
	#else
	int sent = i = rte_ring_enqueue_burst(tx_ring, (void **) &packets[0], BURST_SIZE);
	if (unlikely(i < BURST_SIZE)) {
		do {
			rte_pktmbuf_free(packets[i]);
		} while (++i < BURST_SIZE);
	}
	return sent;
	#endif

#elif SEND_MODE == ETHERNET
	#ifdef SEND_FULL_BURST
	int ntosend = BURST_SIZE;
	do
	{
		#ifdef CALC_TX_TRIES
		stats.tx_retries++;
		#endif

		i += rte_eth_tx_burst(txport, 0, &packets[i], ntosend - i);
		if(unlikely(stop))
			break;
	} while(unlikely(i < ntosend));
	return BURST_SIZE;
	#else
	int sent = i = rte_eth_tx_burst(txport, 0, &packets[0], BURST_SIZE);
	if (unlikely(i < BURST_SIZE)) {
		do {
			rte_pktmbuf_free(packets[i]);
		} while (++i < BURST_SIZE);
	}
	return sent;
	#endif
#endif
}

void send_receive_loop(void)
{
	struct rte_mbuf * packets_array[BURST_SIZE] = {0};

	char pkt[PKT_SIZE] = {0};

	int i;
	int nreceived;
	int retval = 0;
	(void) retval;
	srand(time(NULL));

	//Initializate packet contents
	for(i = 0; i < PKT_SIZE; i++)
		pkt[i] = 0xCC;

/* prealloc packets */
#if ALLOC_METHOD == NO_ALLOC
	int n;
	do
	{
		n = rte_mempool_get_bulk(packets_pool, (void **) packets_array, BURST_SIZE);
	} while(n != 0 && !stop);

	for(i = 0; i < BURST_SIZE; i++)
	{
		rte_memcpy(rte_pktmbuf_mtod(packets_array[i], void *), pkt, PKT_SIZE);
		packets_array[i]->next = NULL;
		packets_array[i]->pkt_len = PKT_SIZE;
		packets_array[i]->data_len = PKT_SIZE;
	}
#endif

	signal (SIGINT,crtl_c_handler);

	while(likely(!stop))
	{
		while(pause_);

		/* send packets */
#if ALLOC_METHOD == ALLOC
		int n;
		/* get BURST_SIZE free slots */
		do {
			n = rte_mempool_get_bulk(packets_pool, (void **) packets_array, BURST_SIZE);
			if(unlikely(n != 0))
				stats.alloc_fails++;
		} while(n != 0 && !stop);

		//Copy data to the buffers
		for(i = 0; i < BURST_SIZE; i++)
		{
			/* XXX: is this a valid aprroach? */
			rte_mbuf_refcnt_set(packets_array[i], 1);

			rte_memcpy(rte_pktmbuf_mtod(packets_array[i], void *), pkt, PKT_SIZE);
			packets_array[i]->next = NULL;
			packets_array[i]->pkt_len = PKT_SIZE;
			packets_array[i]->data_len = PKT_SIZE;

	#ifdef CALC_CHECKSUM
				for(kk = 0; kk < 8; kk++) /** XXX: HARDCODED value**/
					checksum += ((uint64_t *)packets_array[i]->buf_addr)[kk];
	#endif
		}
#endif
		stats.tx += send_packets(packets_array);

		/* receive packets */

#if SEND_MODE == RING
		nreceived = rte_ring_sc_dequeue_burst(rx_ring, (void **) packets_array, BURST_SIZE);
#elif SEND_MODE == ETHERNET
		nreceived = rte_eth_rx_burst(rxport, 0, packets_array, BURST_SIZE);
#endif
#ifdef CALC_CHECKSUM
		for(i = 0; i < nreceived; i++)
			for(kk = 0; kk < PKT_LEN/8; kk++)
				checksum += ((uint64_t *)packets_array[i]->buf_addr)[kk];
#endif

#if ALLOC_METHOD == ALLOC
		if(likely(nreceived > 0))
			rte_mempool_mp_put_bulk(packets_pool, (void **) packets_array, nreceived);
#endif

		stats.rx += nreceived;
	}
}

void crtl_c_handler(int s)
{
	(void) s; /* Avoid compile warning */
	printf("Requesting stop.\n");
	stop = 1;
}
