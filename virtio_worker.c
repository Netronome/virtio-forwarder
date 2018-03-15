/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2016-2017 Netronome.
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
 *     * Neither the name of Netronome nor the names of its
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

#include "virtio_worker.h"
#define __MODULE__ "virtio_worker"
#include "log.h"
#include <rte_version.h>
#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
#include <rte_vhost.h>
#else
#include <rte_virtio_net.h>
#endif
#include "rte_errno.h"
#include "rte_mbuf.h"
#include "dpdk_eal.h"
#include "rte_ethdev.h"
#include "stats_dump.h"
#include "cpuinfo.h"
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <sys/time.h>
#include <assert.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_sctp.h>
#include <rte_arp.h>
#include <rte_jhash.h>
#include <rte_spinlock.h>
#include <rte_cycles.h>
#if RTE_VERSION < RTE_VERSION_NUM(16,7,0,0)
#include <numaif.h>
#endif
#include <rte_eth_bond.h>

/* TODO: make MTU configurable (also impacts hugepage allocation in dpdk_eal.c) */
#define JUMBO_IP_MTU (9000)
#define DEFAULT_IP_MTU (2100)
#define L2_OVERHEAD (14 + 4 + 4)
#define VF_RX_OFFSET (32)
#define JUMBO_MBUF_SIZE (JUMBO_IP_MTU + L2_OVERHEAD + RTE_PKTMBUF_HEADROOM + VF_RX_OFFSET)
#define DEFAULT_MBUF_SIZE (DEFAULT_IP_MTU + L2_OVERHEAD + RTE_PKTMBUF_HEADROOM + VF_RX_OFFSET)
#define MAX_MULTIQUEUE_PAIRS (32)
#define MAX_CPUS 64
#define BURST_LEN 32

/*
 * Type declarations
 */
#if RTE_VERSION >= RTE_VERSION_NUM(17,11,0,0)
typedef uint16_t dpdk_port_t;
#else
typedef uint8_t dpdk_port_t;
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
enum {VIRTIO_RXQ, VIRTIO_TXQ, VIRTIO_QNUM};
#endif

typedef struct {
	union {
		struct {
			pthread_t thread_id;
			int cpu;
			bool initialized;
			bool running;
			bool must_stop;
			volatile bool need_update;
			uint64_t active_relays;
		};
		uint8_t _buf[RTE_CACHE_LINE_SIZE];
	};
} worker_thread_t  __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

typedef enum {
	VIRTIO_UNINIT,
	VIRTIO_READY,
	VIRTIO_REMOVING1,
	VIRTIO_REMOVING2
} vio_state_t;

typedef enum {
	DPDK_UNINIT,
	DPDK_ADDED,
	DPDK_READY,
	DPDK_REMOVING1,
	DPDK_REMOVING2
} dpdk_state_t;

struct relay_virtio {
	int vio2vf_cpu;
	unsigned max_queue_pairs;
	uint64_t rx_q_bitmap;
	uint64_t tx_q_bitmap;
	unsigned rx_q_active;
	uint8_t rx_q_lut[MAX_MULTIQUEUE_PAIRS];
	unsigned tx_q_rr; /* round robin state of tx queue processing for multi-queue, 0 <= tx_q_rr < max_queue_pairs. */
	bool pow2queues;
	volatile vio_state_t state;
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	int vio_dev;
#else
	void *vio_dev;
#endif
	struct rte_mempool *mempool;
	int mempool_socket_id;
	struct rte_mbuf *tx_pkts[BURST_LEN];
	unsigned tx_pkts_avail, tx_pkts_used;
	rte_spinlock_t sl;
} __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

struct relay_dpdk {
	int vf2vio_cpu;
	volatile dpdk_state_t state;
	bool is_bond;
	dpdk_port_t dpdk_port;
	char pci_dbdf[20];
	struct rte_mbuf *rx_pkts[BURST_LEN];
	unsigned rx_pkts_avail, rx_pkts_used;
	rte_spinlock_t sl;
} __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

struct relay_stats {
	/* VM to VF */
	uint64_t vio_rx; /* packets received from virtio */
	uint64_t vio_rx_bytes; /* bytes received from virtio */
	uint64_t dpdk_tx; /* packets sent to the VF */
	uint64_t dpdk_tx_bytes; /* bytes sent to the VF */
	uint64_t dpdk_drop_full; /* packets from virtio dropped because VF queue full */
	uint64_t dpdk_drop_unavail; /* packets from virtio dropped because VF not ready */
	/* VF to VM */
	uint64_t dpdk_rx; /* packets received from the VF */
	uint64_t dpdk_rx_bytes; /* bytes received from the VF */
	uint64_t vio_tx; /* packets sent to virtio */
	uint64_t vio_tx_bytes; /* bytes sent to virtio */
	uint64_t vio_drop_full; /* packets from VF dropped because virtio queue full */
	uint64_t vio_drop_unavail; /* packets from VF dropped because virtio not avail */
} __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));


typedef struct {
	union {
		struct {
			unsigned id;
			#ifdef VIRTIO_ECHO
			struct rte_ring *echo_ring;
			#endif
			struct relay_virtio vio;
			struct relay_dpdk dpdk;
			struct relay_stats stats;
			unsigned use_jumbo:1;
		};
		uint8_t _buf[RTE_CACHE_LINE_SIZE];
	};
} vio_vf_relay_t __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

typedef struct {
	uint64_t virtio_rx;
	uint64_t virtio_rx_bytes;
	uint64_t dpdk_tx;
	uint64_t dpdk_tx_bytes;
	uint64_t dpdk_rx;
	uint64_t dpdk_rx_bytes;
	uint64_t virtio_tx;
	uint64_t virtio_tx_bytes;
	uint64_t time_prev;
} relay_prev_counters_t;

static worker_thread_t worker_threads[MAX_CPUS];
static uint64_t worker_core_bitmap;
static struct virtio_vhostuser_conf vio_worker_conf;
static vio_vf_relay_t virtio_vf_relays[MAX_RELAYS];
static relay_prev_counters_t relay_prev_counters[MAX_RELAYS];

static bool have_worker_on_node(int node)
{
	cpuinfo_t *c = get_cpuinfo();

	for (int i=0; i<MAX_CPUS; ++i) {
		if ((int)c->cpus[i].numanode != node)
			continue;
		if ((1ULL<<i) & worker_core_bitmap)
			return true;
	}

	return false;
}

/*
 * Get the least busy worker CPU based on the number of relays it is servicing.
 * VF-to-virtio is assumed approx 20% more CPU intensive.
 * Does not account for packet rates.
 */
static int naive_get_idlest_worker(int node)
{
	int min, min_index;
	int cpu_workers[MAX_CPUS] = {0};
	bool worker_on_node = false;
	cpuinfo_t *c = get_cpuinfo();

	for (unsigned w=0; w<MAX_RELAYS; ++w) {
		vio_vf_relay_t *relay = &virtio_vf_relays[w];
		if ((relay->dpdk.state == DPDK_READY ||
				relay->dpdk.state == DPDK_ADDED) &&
				relay->dpdk.vf2vio_cpu >= 0)
			cpu_workers[relay->dpdk.vf2vio_cpu] += 12;
		if (relay->vio.state == VIRTIO_READY &&
				relay->vio.vio2vf_cpu >= 0)
			cpu_workers[relay->vio.vio2vf_cpu] += 10;
	}

	worker_on_node = have_worker_on_node(node);
	min = MAX_RELAYS * 100;
	min_index = -1;
	for (unsigned cpu=0; cpu<MAX_CPUS; ++cpu) {
		if (node != SOCKET_ID_ANY && worker_on_node) {
			if ((int)c->cpus[cpu].numanode != node)
				continue;
		}
		if (((1ULL<<cpu) & worker_core_bitmap) == 0)
			continue;
		worker_thread_t *worker = &worker_threads[cpu];
		if (worker->initialized) {
			if (cpu_workers[cpu] < min || min_index == -1) {
				min = cpu_workers[cpu];
				min_index = cpu;
			}
		}
	}

	return min_index;
}

static void find_vf2virtio_cpu(vio_vf_relay_t *relay)
{
	int idlest_cpu;
	int conf_cpu = vio_worker_conf.relay_cpus[relay->id].vf2vio_cpu;

	if (conf_cpu != -1) {
		relay->dpdk.vf2vio_cpu = conf_cpu;
		log_debug("Using CPU %u for relay %u vf2virtio",
			relay->dpdk.vf2vio_cpu, relay->id);
		return;
	}

	if (relay->dpdk.vf2vio_cpu != -1) {
		worker_threads[relay->dpdk.vf2vio_cpu].need_update = true;
		relay->dpdk.vf2vio_cpu = -1;
	}
	idlest_cpu = naive_get_idlest_worker(relay->vio.mempool_socket_id);
	assert(idlest_cpu >= 0);
	relay->dpdk.vf2vio_cpu = idlest_cpu;
	log_debug("Found CPU %u for relay %u vf2virtio",
		relay->dpdk.vf2vio_cpu, relay->id);
}

/**
 * Log a warning message if the UIO driver appears to be set up incorrectly on
 * @a pci_dbdf).
 */
static void check_uio_driver_setup(const char *pci_dbdf)
{
	char buf[64];
	char linkname[128];
	int len;

	/* Attempt to find device driver associated with PCI device. */
	snprintf(buf, 64, "/sys/bus/pci/devices/%s/driver", pci_dbdf);
	if ((len=readlink(buf, linkname, 128)) > 0) {
		if (len==128)
			len = 127;
		linkname[len] = 0;
		char *s = strrchr(linkname, '/');
		if (s && (len - (s - linkname) > 1)) {
			s += 1;
			if (strstr(s, "uio") == 0)
				log_warning("PCI '%s' should be assigned to a UIO driver for use with virtio-forwarder, but is assigned to '%s'",
					pci_dbdf, s);
			else
				log_warning("PCI '%s' seems to be assigned to a UIO driver ('%s')",
					pci_dbdf, s);
		}
	} else
		log_warning("PCI '%s' does not seem to be assigned to any UIO driver",
			pci_dbdf);
}

/**
 * Return true if two PCI address strings represent the same address.
 *
 * Currently this just uses strcmp(). This should work in cases of interest to
 * vRouter (and wherever it was working before, as this is what the original
 * code did).
 *
 * In the future, we should probably compare the address components
 * numerically.
 */
static bool pci_dbdf_equal(const char *dbdf1, const char *dbdf2)
{
	return strcmp(dbdf1, dbdf2) == 0;
}

/**
 * Return true if a conditional VF add should be treated a no-op, rather than
 * an error.
 */
static bool
conditional_add_ok(vio_vf_relay_t *relay, const char *pci_dbdf,
			unsigned virtio_id)
{
	return (relay->dpdk.state == DPDK_READY ||
			relay->dpdk.state == DPDK_ADDED)
			&& relay->id == virtio_id
			&& pci_dbdf_equal(relay->dpdk.pci_dbdf, pci_dbdf);
}

static void build_tx_conf(struct rte_eth_txconf *tx_conf)
{
	memset(tx_conf, 0, sizeof(struct rte_eth_txconf));
	tx_conf->tx_free_thresh = 16;
	tx_conf->txq_flags = ETH_TXQ_FLAGS_NOOFFLOADS |
				ETH_TXQ_FLAGS_NOMULTMEMP |
				ETH_TXQ_FLAGS_NOMULTSEGS;
}

static void build_rx_conf(struct rte_eth_rxconf *rx_conf)
{
	memset(rx_conf, 0, sizeof(struct rte_eth_rxconf));
	rx_conf->rx_drop_en = 1;
	rx_conf->rx_free_thresh = 16;
}

static int cleanup_eth_dev(dpdk_port_t port_id)
{
	int err;
	char detach_dbdf[RTE_ETH_NAME_MAX_LEN];

	rte_eth_dev_stop(port_id);
	rte_eth_dev_close(port_id);
	err = rte_eth_dev_detach(port_id, detach_dbdf);
	if (err != 0) {
		log_warning("rte_eth_dev_detach(%hhu) failed with error %i",
			port_id, err);
	}

	return err;
}

static int start_eth_dev(dpdk_port_t port_id)
{
	int err;

	err = rte_eth_dev_start(port_id);
	if (err != 0)
		log_error("rte_eth_dev_start(%hhu) failed with error %i",
			port_id, err);

	return err;
}

static int dev_queue_configure(const char *name, dpdk_port_t port_id,
			unsigned virtio_id, vio_vf_relay_t *relay, bool is_bond)
{
	int err;
	struct rte_eth_conf eth_conf = {0};
	struct rte_eth_rxconf rx_conf;
	struct rte_eth_txconf tx_conf;

	log_info("Adding DPDK port %hhu ('%s') to virtio ('%u')",
		port_id, name, virtio_id);

	eth_conf.rxmode.jumbo_frame = 1;
	eth_conf.rxmode.max_rx_pkt_len = relay->use_jumbo ? JUMBO_MBUF_SIZE :
						DEFAULT_MBUF_SIZE;
	err = rte_eth_dev_configure(port_id, 1, 1, &eth_conf);
	if (err != 0) {
		log_error("rte_eth_dev_configure(%hhu, 1, 1) failed with error %i",
			port_id, err);
		return 4;
	}

	err = rte_eth_dev_set_mtu(port_id, relay->use_jumbo ?
				JUMBO_IP_MTU : DEFAULT_IP_MTU);
	if (!is_bond && err != 0) {
		log_error("rte_eth_dev_set_mtu failed with error %i", err);
		return 4;
	}

	/*
	 * Per <http://dpdk.org/doc/api/rte__ethdev_8h.html>, we must setup the
	 * TX queue before setting up the RX queue.
	 */
	build_tx_conf(&tx_conf);
	err = rte_eth_tx_queue_setup(port_id, 0, 1024,
				relay->vio.mempool_socket_id, &tx_conf);
	if (err != 0) {
		log_error("rte_eth_tx_queue_setup(%hhu, 0, 1024) failed with error %i",
			port_id, err);
		return 6;
	}

	build_rx_conf(&rx_conf);
	err = rte_eth_rx_queue_setup(port_id, 0, 1024,
				relay->vio.mempool_socket_id, &rx_conf,
				relay->vio.mempool);
	if (err != 0) {
		log_error("rte_eth_rx_queue_setup(%hhu, 0, 1024) failed with error %i",
			port_id, err);
		return 5;
	}

	return 0;
}

static int init_vf(const char *pci_dbdf, dpdk_port_t *port_id,
			unsigned virtio_id, vio_vf_relay_t *relay)
{
	int err;

	err = rte_eth_dev_attach(pci_dbdf, port_id);
	if (err != 0) {
		/* err is always -1 on error, so no useful additional info.
		 * Print it anyway for consistency with other error messages. */
		log_error("rte_eth_dev_attach('%s') failed with error %i",
			pci_dbdf, err);
		check_uio_driver_setup(pci_dbdf);
		return 2;
	}

	err = dev_queue_configure(pci_dbdf, *port_id, virtio_id, relay, false);
	if (err) {
		cleanup_eth_dev(*port_id);
		return err;
	}

	return 0;
}

static int configure_dev_with_virtio(const char *pci_dbdf, dpdk_port_t port_id,
			unsigned virtio_id, vio_vf_relay_t *relay, bool is_bond)
{
	int dpdk_state;

	if (relay->vio.state == VIRTIO_READY) {
		log_debug("Starting device for relay %u", virtio_id);
		if (start_eth_dev(port_id) != 0)
			return 1;
		dpdk_state = DPDK_READY;
	} else {
		log_debug("Not starting device for relay %u since virtio not connected",
			virtio_id);
		rte_eth_dev_stop(port_id); /* stop the VF explicitly in case it was still running from a previous process. */
		dpdk_state = DPDK_ADDED;
	}

	/* Success */
	log_info("Added dpdk port %hhu to relay", port_id);
	relay->dpdk.dpdk_port = port_id;
	strncpy(relay->dpdk.pci_dbdf, pci_dbdf, 20);
	relay->id = virtio_id;
	find_vf2virtio_cpu(relay);
	relay->dpdk.state = dpdk_state;
	relay->dpdk.is_bond = is_bond;
	__sync_synchronize();
	worker_threads[relay->dpdk.vf2vio_cpu].need_update = true;

	return 0;
}

int
virtio_forwarder_add_vf2(const char *pci_dbdf, unsigned virtio_id, bool conditional)
{
#ifdef VIRTIO_ECHO
	log_warning("No effect adding dpdk interface '%s' to relay %u in VIRTIO_ECHO mode!",
		pci_dbdf, virtio_id);
	return 0;
#else
	dpdk_port_t port_id;
	int err;

	log_debug("Got virtio_forwarder_add_vf2('%s', %u, conditional=%s)",
		pci_dbdf, virtio_id, conditional ? "true" : "false");

	if (virtio_id >= MAX_RELAYS) {
		log_error("Tried to add PCI '%s' to invalid virtio ID %u! (valid range is 0..%u)",
			pci_dbdf, virtio_id, MAX_RELAYS - 1);
		return 1;
	}

	vio_vf_relay_t *relay= &virtio_vf_relays[virtio_id];

	if (relay->dpdk.state != DPDK_UNINIT) {
		/* VF already active */
		if (conditional && conditional_add_ok(relay, pci_dbdf, virtio_id)) {
			log_info("No action required; VF was already properly configured");
			return 0;
		} else {
			log_error("Tried to add DPDK port to already initialized entity!");
			return 8;
		}
	}

	/* New VF */
	err = init_vf(pci_dbdf, &port_id, virtio_id, relay);
	if (err)
		return err;

	/* Successfully added VF. */
	err = configure_dev_with_virtio(pci_dbdf, port_id, virtio_id, relay, false);
	if (err)
		cleanup_eth_dev(port_id);

	return err;
#endif /* VIRTIO_ECHO */
}

int virtio_forwarder_add_vf(const char *pci_dbdf, unsigned virtio_id)
{
	/* Original code used unconditional adds/removes. */
	return virtio_forwarder_add_vf2(pci_dbdf, virtio_id, false);
}

static void format_slave_dbdfs(char slave_dbdfs[MAX_NUM_BOND_SLAVES][RTE_ETH_NAME_MAX_LEN],
			unsigned num_slaves, char *p)
{
	size_t idx = 0;

	for (unsigned i=0; i<num_slaves; i++) {
		strcpy(&p[idx], slave_dbdfs[i]);
		idx += strlen(slave_dbdfs[i]);
		strcpy(&p[idx], " ");
		idx ++;
	}
}

int virtio_forwarder_bond_add(char slave_dbdfs[MAX_NUM_BOND_SLAVES][RTE_ETH_NAME_MAX_LEN],
			unsigned num_slaves, const char *name, uint8_t mode,
			unsigned virtio_id)
{
	char p[RTE_ETH_NAME_MAX_LEN * MAX_NUM_BOND_SLAVES];
	dpdk_port_t port_id, slave_port_ids[MAX_NUM_BOND_SLAVES], tmp;
	int err, rc, socket_id;

	format_slave_dbdfs(slave_dbdfs, num_slaves, p);
	log_debug("Got virtio_forwarder_bond_add(<%s>, %u, %s, %u, %u)", p,
		num_slaves, name, mode, virtio_id);

	if (virtio_id >= MAX_RELAYS) {
		log_error("Tried to add bond '%s' to invalid virtio ID %u! (valid range is 0..%u)",
			name, virtio_id, MAX_RELAYS - 1);
		return 1;
	}
	vio_vf_relay_t *relay= &virtio_vf_relays[virtio_id];

	if (relay->dpdk.state != DPDK_UNINIT) {
		/* VF/bond already active on virtio instance. */
		log_error("Tried to add DPDK port to already initialized entity!");
		return 8;
	}

	/* XXX: DPDK defines SOCKET_ID_ANY as -1, but the bonding API accepts
	 * an unsigned... Until they fix it use 0 for unspecified sockets. */
	socket_id = relay->vio.mempool_socket_id == SOCKET_ID_ANY ?
					0 : relay->vio.mempool_socket_id;
	err = rte_eth_bond_create(name, mode, socket_id);
	if (err < 0) {
		log_error("rte_eth_bond_create(%s, %u, %d) failed with error %d",
			name, mode, socket_id, err);
		return 2;
	} else {
		port_id = err;
	}

	/* Error if a slave is already attached to DPDK. */
	for (unsigned i=0; i<num_slaves; ++i) {
		if (rte_eth_dev_get_port_by_name(slave_dbdfs[i], &tmp) == 0) {
			log_warning("The specified bond slave ('%s') is already in use with port id %u.",
				slave_dbdfs[i], tmp);
			rc = 3;
			goto error_bond_deconfigure;
		}
	}

	/* Configure bond. */
	err = dev_queue_configure(name, port_id, virtio_id, relay, true);
	if (err) {
		log_error("Bond configuration failed. Tearing down...");
		rc = 4;
		goto error_bond_deconfigure;
	}

	/* Add slaves to bond. XXX: The queue configure which takes place in
	 * init_vf may be redundant, as it is done in any case when the bond
	 * is started - check whether dev attach is adequate here. */
	for (unsigned i=0; i<num_slaves; ++i) {
		/* Setup slave interface. */
		err = init_vf(slave_dbdfs[i], &slave_port_ids[i], virtio_id, relay);
		if (err) {
			rc = 5;
			goto error_slaves_deconfigure;
		}

		/* Add slave to bond. */
		rte_eth_bond_slave_add(port_id, slave_port_ids[i]);
	}

	/* Successfully configured bond. */
	err = configure_dev_with_virtio(name, port_id, virtio_id, relay, true);
	if (err) {
		rc = 6;
		goto error_slaves_deconfigure;
	}

	return 0;

error_slaves_deconfigure:
	/* Deconfigure the other slaves that may have been initialized. */
	for (unsigned i=0; i<num_slaves; ++i)
		cleanup_eth_dev(slave_port_ids[i]);

error_bond_deconfigure:
	err = rte_eth_bond_free(name);
	if (err)
		log_warning("During error recovery, rte_eth_bond_free(%s) failed with error %i",
			name, err);

	return rc;
}

static int stop_vm2vf_thread(vio_vf_relay_t *relay)
{
	if (relay->dpdk.vf2vio_cpu >= 0) {
		unsigned retries = 0;
		relay->dpdk.state = DPDK_REMOVING1;
		while (relay->dpdk.state != DPDK_UNINIT && retries++ < 20)
			usleep(50000);
		if (relay->dpdk.state != DPDK_UNINIT) {
			log_error("Timeout waiting for DPDK port %hhu ('%s') to be released from relay thread!",
				relay->dpdk.dpdk_port, relay->dpdk.pci_dbdf);
			return 5;
		}
	}

	return 0;
}

static int detach_slaves(vio_vf_relay_t *relay)
{
	dpdk_port_t port_id = relay->dpdk.dpdk_port;
	uint16_t slaves[MAX_NUM_BOND_SLAVES] = {0};
	uint16_t n_slaves;
	int err;

	if (!relay->dpdk.is_bond) {
		log_warning("detach_slaves called on non-bond packet relay!");
		return 0;
	}

	err = rte_eth_bond_slaves_get(port_id, slaves, MAX_NUM_BOND_SLAVES) ;
	if (err < 0) {
		log_error("Error retrieving slave information for bond %s",
			relay->dpdk.pci_dbdf);
		return 1;
	} else {
		n_slaves = err;
	}
	for (unsigned i=0; i<n_slaves; ++i) {
		if (rte_eth_bond_slave_remove(port_id, slaves[i]))
			log_warning("Error removing slave %u from device %s",
				i, relay->dpdk.pci_dbdf);
	}
	for (unsigned i=0; i<n_slaves; ++i)
		cleanup_eth_dev(slaves[i]);

	return 0;
}

static int detach_device(vio_vf_relay_t *relay)
{
	dpdk_port_t port_id = relay->dpdk.dpdk_port;
	char *pci_dbdf = relay->dpdk.pci_dbdf;
	int err, tmpidx;

	err = stop_vm2vf_thread(relay);
	if (err)
		return err;

	/* Update workers. */
	tmpidx = relay->dpdk.vf2vio_cpu;
	relay->dpdk.vf2vio_cpu = -1;
	__sync_synchronize();
	worker_threads[tmpidx].need_update = true;

	/* Detach VF. */
	log_debug("Stopping PCI '%s' device (port %hhu)", pci_dbdf, port_id);
	if (relay->dpdk.is_bond) {
		detach_slaves(relay);
		rte_eth_dev_stop(port_id);
		rte_eth_dev_close(port_id);
		err = rte_eth_bond_free(relay->dpdk.pci_dbdf);
	} else {
		err = cleanup_eth_dev(port_id);
	}
	if (err == 0) {
		log_debug("Removed PCI '%s' device as port %hhu",
			pci_dbdf, port_id);
	} else {
		return 2;
	}
	log_info("removing DPDK port %hhu ('%s') from virtio %u", port_id,
		pci_dbdf, relay->id);
	relay->dpdk.dpdk_port = -1;
	relay->dpdk.pci_dbdf[0] = 0;
	log_info("Removed dpdk port %hhu from virtio", port_id);

	return err;
}

int
virtio_forwarder_remove_vf2(const char *pci_dbdf, unsigned virtio_id,
			bool conditional)
{
#ifndef VIRTIO_ECHO
	int err = 0;

	log_debug("Got virtio_forwarder_remove_vf2('%s', %u, conditional=%s)",
		pci_dbdf, virtio_id, conditional ? "true" : "false");

	if (virtio_id >= MAX_RELAYS) {
		log_error("Tried to remove PCI '%s' from invalid virtio ID %u!",
			pci_dbdf, virtio_id);
		return 1;
	}

	vio_vf_relay_t *relay = &virtio_vf_relays[virtio_id];
	if (relay->dpdk.state == DPDK_READY || relay->dpdk.state == DPDK_ADDED) {
		if (pci_dbdf_equal(pci_dbdf, relay->dpdk.pci_dbdf)) {
			err = detach_device(relay);
			if (err)
				return err;
		} else {
			log_error("Tried to remove DPDK with non-matching PCI addr!");
			return 3;
		}
	} else if (conditional) {
		log_info("No action required; VF was already removed");
		return 0;
	} else {
		log_error("Tried to remove DPDK port from un-initialized entity!");
		return 4;
	}
#else
	log_warning("No effect adding dpdk interface '%s' to relay %u in VIRTIO_ECHO mode!",
		pci_dbdf, virtio_id);
#endif

	return err;
}

int virtio_forwarder_remove_vf(const char *pci_dbdf, unsigned virtio_id)
{
	/* Original code used unconditional adds/removes. */
	return virtio_forwarder_remove_vf2(pci_dbdf, virtio_id, false);
}

#if RTE_VERSION < RTE_VERSION_NUM(16,7,0,0)
static inline uint16_t __attribute__((always_inline))
vring_available_entries(struct virtio_net *dev, uint16_t queue_id)
{
	struct vhost_virtqueue *vq = dev->virtqueue[queue_id];

	if (!vq->enabled)
		return 0;

	return *(volatile uint16_t *)&vq->avail->idx - vq->last_used_idx;
}
#endif

static inline uint32_t calc_eth_header_hash(struct ether_hdr *eth_hdr)
{
	struct udp_hdr *udp_h;
	struct ipv4_hdr *ipv4_h;
	uint32_t buf[16];
	uint32_t hashwords=0;

	if (likely(eth_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4))) {
		ipv4_h = (struct ipv4_hdr *)(eth_hdr + 1);
		buf[hashwords++] = ipv4_h->src_addr;
		buf[hashwords++] = ipv4_h->dst_addr;
		buf[hashwords++] = ipv4_h->next_proto_id;
		int ipv4_hdrlen = (ipv4_h->version_ihl & IPV4_HDR_IHL_MASK) *
					IPV4_IHL_MULTIPLIER;
		if (likely(ipv4_h->next_proto_id == IPPROTO_TCP ||
				ipv4_h->next_proto_id == IPPROTO_UDP ||
				ipv4_h->next_proto_id == IPPROTO_SCTP)) {
			udp_h = (struct udp_hdr *)((unsigned char *)ipv4_h +
					ipv4_hdrlen);
			buf[hashwords++] = (udp_h->dst_port<<16) +
						udp_h->src_port;
		}
	} else if (eth_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv6)) {
		struct ipv6_hdr *ipv6_h;
		uint32_t *p;
		ipv6_h = (struct ipv6_hdr *)(eth_hdr + 1);
		p = (uint32_t*)(ipv6_h->src_addr);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = *(p++);
		buf[hashwords++] = ipv6_h->proto;
	} else {
		/* Non-IPv4 ethernet. */
		buf[hashwords++] = *((uint32_t*)(eth_hdr)+0);
		buf[hashwords++] = *((uint32_t*)(eth_hdr)+1);
		buf[hashwords++] = *((uint32_t*)(eth_hdr)+2);
		buf[hashwords++] = eth_hdr->ether_type;
	}

	return rte_jhash_32b(buf, hashwords, 0xdeadbee5);
}

static inline void calc_mbuf_hash(struct rte_mbuf **pkts, uint16_t nb_pkts)
{
	uint16_t i;

	for (i=0; i<nb_pkts; ++i) {
		rte_prefetch0(rte_pktmbuf_mtod(pkts[i], void *));
	}
	for (i=0; i<nb_pkts; ++i) {
		struct ether_hdr *eth_hdr;

		eth_hdr = rte_pktmbuf_mtod(pkts[i], struct ether_hdr *);
		pkts[i]->hash.usr = calc_eth_header_hash(eth_hdr);
	}
}

#if defined(VIRTIO_RETRY_ENQUEUE)
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
static int
worker_vhost_enqueue_burst(int dev, uint16_t q, struct rte_mbuf **pkts,
				uint32_t count)
{
#else
static int
worker_vhost_enqueue_burst(struct virtio_net *dev, uint16_t q,
				struct rte_mbuf **pkts, uint32_t count)
{
#endif
	int retries=VIRTIO_RETRY_ENQUEUE;
	unsigned sent = 0;

	do {
		unsigned tx = rte_vhost_enqueue_burst(dev, q, pkts+sent, count);
		if (likely(tx)) {
			sent += tx;
			count -= tx;
		} else
			break;
	} while (count && (retries-- > 0));

	return sent;
}
#endif

int
migrate_relay_cpus(int relay_number, int new_virtio2vf_cpu,
			int new_vf2virtio_cpu)
{
	vio_vf_relay_t *relay = &virtio_vf_relays[relay_number];

	if ((((1ULL<< new_virtio2vf_cpu) | (1ULL<<new_vf2virtio_cpu)) &
			!worker_core_bitmap) != 0) {
		log_warning("Attempted to assign an invalid cpu when migrating relay %u.",
			relay->id);
		return -1;
	}
	worker_thread_t *thread;

	/* Move virtio2vf. */
	if (!worker_threads[new_virtio2vf_cpu].initialized) {
		log_warning("Cannot move to uninitialized cpu.");
	}
	else if (relay->vio.state != VIRTIO_READY) {
		log_warning("Will not attempt to alter virtio2vf cpu state before the VM has connected.");
	}
	else if (relay->vio.vio2vf_cpu != new_virtio2vf_cpu) {
		/* Update data structures. */
		unsigned old_lcore = relay->vio.vio2vf_cpu;
		relay->vio.vio2vf_cpu = new_virtio2vf_cpu;
		thread = &worker_threads[old_lcore];
		thread->need_update = true;
		thread = &worker_threads[new_virtio2vf_cpu];
		thread->need_update = true;
		/* thread->active_relays field will be updated in worker_func
		 * due to the need_update flag. */
		log_debug("Moved relay %u's virtio2vf cpu to %d.",
			relay->id, new_virtio2vf_cpu);
	}

	/* Move vf2virtio. */
	if (!worker_threads[new_vf2virtio_cpu].initialized) {
		log_warning("Cannot move to uninitialized cpu.");
	}
	else if (!(relay->dpdk.state == DPDK_ADDED ||
			relay->dpdk.state == DPDK_READY)) {
		log_warning("Will not attempt to alter vf2virtio cpu state before the VF has been initialized.");
	}
	else if (relay->dpdk.vf2vio_cpu != new_vf2virtio_cpu) {
		/* Update data structures. */
		unsigned old_lcore = relay->dpdk.vf2vio_cpu;
		relay->dpdk.vf2vio_cpu = new_vf2virtio_cpu;
		thread = &worker_threads[old_lcore];
		thread->need_update = true;
		thread = &worker_threads[new_vf2virtio_cpu];
		thread->need_update = true;
		/* thread->active_relays field will be updated in worker_func
		 * due to the need_update flag. */
		log_debug("Moved relay %u's vf2virtio cpu to %d.",
			relay->id, new_vf2virtio_cpu);
	}

	return 0;
}

static inline void update_thread(worker_thread_t *thread)
{
	uint64_t active_relays = 0;

	thread->need_update = false;
	__sync_synchronize();
	for (int w=0; w<MAX_RELAYS; ++w) {
		vio_vf_relay_t *relay = &virtio_vf_relays[w];
		if ((relay->dpdk.state != DPDK_UNINIT &&
				relay->dpdk.vf2vio_cpu == thread->cpu) ||
				(relay->vio.state != VIRTIO_UNINIT &&
				relay->vio.vio2vf_cpu == thread->cpu))
			active_relays |= (1ULL << w);
	}
	thread->active_relays = active_relays;
	log_debug("Worker %u got signal to update state, active_relays=0x%08llX",
		thread->cpu, (unsigned long long)active_relays);
}

static inline int virtio_rx(vio_vf_relay_t *relay)
{
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	int dev = (int)relay->vio.vio_dev;
#else
	struct virtio_net *dev = relay->vio.vio_dev;
#endif
	int rcvd;
	int try_rcv = BURST_LEN;
	struct rte_mbuf **pkts = relay->vio.tx_pkts;

	if (relay->vio.state != VIRTIO_READY)
		return -1;

	if (likely((1ULL<<(relay->vio.tx_q_rr)) & relay->vio.tx_q_bitmap))
		rcvd = rte_vhost_dequeue_burst(dev, relay->vio.tx_q_rr*2+1,
						relay->vio.mempool, pkts,
						try_rcv);
	else
		rcvd = 0;

	relay->vio.tx_pkts_avail = rcvd;
	relay->vio.tx_pkts_used = 0;
	do { /* Increment tx_q_rr to the next valid index. */
		++relay->vio.tx_q_rr;
		if (relay->vio.tx_q_rr >= relay->vio.max_queue_pairs)
			relay->vio.tx_q_rr = 0;
	} while (!((1ULL<<(relay->vio.tx_q_rr)) & relay->vio.tx_q_bitmap));

	/* Update rx stats. */
	if (rcvd) {
		int i;
		unsigned bytes = 0;
		for (i=0; i<rcvd; ++i)
			bytes += pkts[i]->pkt_len;
		relay->stats.vio_rx += rcvd;
		relay->stats.vio_rx_bytes += bytes;
	}

	return rcvd;
}

/*
 * Send a burst of output packets on a transmit queue of an Ethernet
 * device.
 */
static inline int dpdk_tx(vio_vf_relay_t *relay)
{
	int sent;
	struct rte_mbuf **pkts = relay->vio.tx_pkts;

#ifndef VIRTIO_ECHO
	if (relay->dpdk.state != DPDK_READY)
		return -1;

	pkts += relay->vio.tx_pkts_used;
	sent = rte_eth_tx_burst(relay->dpdk.dpdk_port, 0, pkts,
						relay->vio.tx_pkts_avail);
#else
	sent = rte_ring_enqueue_burst(relay->echo_ring,
					(void **)((void *)&pkts[0 + relay->vio.tx_pkts_used]),
					relay->vio.tx_pkts_avail);
#endif
	relay->vio.tx_pkts_avail -= sent;
	relay->vio.tx_pkts_used += sent; /* The first 'sent' mbuf pointers were successfully transmitted. */
	assert(relay->vio.tx_pkts_used <= BURST_LEN);

	/* Update tx stats. */
	if (sent) {
		unsigned bytes=0;
		for (int i=0; i<sent; ++i)
			bytes += pkts[i]->pkt_len;
		relay->stats.dpdk_tx+=sent;
		relay->stats.dpdk_tx_bytes+=bytes;
	}

	return sent;
}

static void worker_remove_vf(vio_vf_relay_t *relay)
{
	int rcvd;
	struct rte_mbuf **pkts;

	log_debug("Removing VF from worker");
	if (relay->dpdk.rx_pkts_avail) {
		log_debug("Freeing %u cached RX packets",
			relay->dpdk.rx_pkts_avail);
		rcvd = relay->dpdk.rx_pkts_avail;
		relay->stats.vio_drop_unavail += rcvd;
		pkts = relay->dpdk.rx_pkts + relay->dpdk.rx_pkts_used;
		while (rcvd) {
			--rcvd;
			rte_pktmbuf_free(pkts[rcvd]);
		}
		relay->dpdk.rx_pkts_avail = 0;
		relay->dpdk.rx_pkts_used = 0;
	}
	if (relay->vio.tx_pkts_avail) {
		log_debug("Freeing %u cached TX packets",
			relay->vio.tx_pkts_avail);
		rcvd = relay->vio.tx_pkts_avail;
		relay->stats.dpdk_drop_unavail += rcvd;
		pkts = relay->vio.tx_pkts + relay->vio.tx_pkts_used;
		while (rcvd) {
			--rcvd;
			rte_pktmbuf_free(pkts[rcvd]);
		}
		relay->vio.tx_pkts_avail = 0;
		relay->vio.tx_pkts_used = 0;
	}
	relay->dpdk.state = DPDK_UNINIT; /* Signal main thread. */
}

/*
 * Forward virtio->DPDK
 */
static inline int relay_vm2vf_traffic(vio_vf_relay_t *relay)
{
	int sent = 0;

	/* There are no buffered packets in the internal
	 * tx queue. Try to fetch packets from virtio
	 * into mbufs. */
	if (likely(relay->vio.tx_pkts_avail == 0))
		virtio_rx(relay);

	/* Send virtio to VF. */
	if (likely(relay->vio.tx_pkts_avail))
		sent = dpdk_tx(relay);

	if (sent == -1 && relay->vio.tx_pkts_avail) {
		/* DPDK not ready.
		 * Free buffered packets. */
		struct rte_mbuf **pkts = relay->vio.tx_pkts +
					relay->vio.tx_pkts_used;
		int avail = relay->vio.tx_pkts_avail;
		relay->stats.dpdk_drop_unavail += avail;
		while (avail > 0) {
			--avail;
			rte_pktmbuf_free(pkts[avail]);
		}
	}

	if (unlikely(relay->vio.state == VIRTIO_REMOVING1)) {
		relay->vio.state = VIRTIO_REMOVING2; /* Signal other thread. */
		if (relay->dpdk.vf2vio_cpu == -1)
			/* There is no other thread. */
			relay->vio.state = VIRTIO_UNINIT;
	}

	if (unlikely(relay->dpdk.state == DPDK_REMOVING2))
		worker_remove_vf(relay);

	return (relay->vio.state == VIRTIO_READY) ? 1 : 0;
}

static inline int dpdk_rx(vio_vf_relay_t *relay)
{
	int rcvd, try_rcv;
	struct rte_mbuf **pkts = relay->dpdk.rx_pkts;

#ifndef VIRTIO_ECHO
	if (relay->dpdk.state != DPDK_READY)
		return -1;
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	try_rcv = rte_vhost_avail_entries((int)relay->vio.vio_dev,
						VIRTIO_RXQ);
#else
	try_rcv = vring_available_entries((struct virtio_net *)relay->vio.vio_dev,
						VIRTIO_RXQ);
#endif
	if (try_rcv > BURST_LEN)
		try_rcv = BURST_LEN;
#ifndef VIRTIO_ECHO
	rcvd = rte_eth_rx_burst(relay->dpdk.dpdk_port, 0, pkts, try_rcv);
#else
	rcvd = rte_ring_dequeue_burst(relay->echo_ring, (void**)pkts,
					try_rcv);
#endif
	relay->dpdk.rx_pkts_avail = rcvd;
	relay->dpdk.rx_pkts_used = 0;

	if (relay->vio.rx_q_bitmap > 1)
		calc_mbuf_hash(pkts, rcvd); /* For multiqueue. */

	/* Update stats. */
	if (rcvd) {
		unsigned bytes=0;
		for (int i=0; i<rcvd; ++i)
			bytes += pkts[i]->pkt_len;
		relay->stats.dpdk_rx+=rcvd;
		relay->stats.dpdk_rx_bytes+=bytes;
	}

	return rcvd;
}

static inline int virtio_tx(vio_vf_relay_t *relay)
{
	bool multiqueue = (relay->vio.rx_q_bitmap > 1);
	int sent = 0;
	struct rte_mbuf **pkts = relay->dpdk.rx_pkts + relay->dpdk.rx_pkts_used;

	if (relay->vio.state != VIRTIO_READY)
		return -1;

	if (multiqueue) {
		unsigned i;
		unsigned q[relay->dpdk.rx_pkts_avail];
		unsigned _sent;

		/* Find the queue that is to be used for each available packet. */
		for (i=0; i<relay->dpdk.rx_pkts_avail; ++i) {
			unsigned h = pkts[i]->hash.usr;
			if (likely(relay->vio.pow2queues))
				h = h & (relay->vio.rx_q_active - 1); /* Retain lower bits of h. Similar to modulo -> result falls within num queues=rx_q_active. */
			else
				h = h % relay->vio.rx_q_active;
			/* Here, h is a number < rx_q_active, so it can be used
			 * to index the lookup table. */
			h = relay->vio.rx_q_lut[h];
			q[i] = h;
		}

		/* Search run-lengths of same q. */
		unsigned cur_q = q[0];
		unsigned runlen = 1;
		for (i=1; i<relay->dpdk.rx_pkts_avail; ++i) {
			if (q[i] == cur_q) {
				++runlen;
			} else {
#if defined(VIRTIO_RETRY_ENQUEUE)
				_sent = worker_vhost_enqueue_burst(
						relay->vio.vio_dev,
						cur_q*2, pkts+i-runlen, runlen);
#else
				_sent = rte_vhost_enqueue_burst(
						relay->vio.vio_dev,
						cur_q*2, pkts+i-runlen, runlen);
#endif
				sent += _sent;
				cur_q = q[i];
				runlen = 1;
				if (_sent != runlen) { /* Stop immediately when an error is encountered. Remaining packets will be processed in a subsequent run. */
				        runlen = 0;
				        break;
				}
			}
		}
#if defined(VIRTIO_RETRY_ENQUEUE)
		sent += worker_vhost_enqueue_burst(relay->vio.vio_dev,
						cur_q*2, pkts+i-runlen, runlen);
#else
		sent += rte_vhost_enqueue_burst(relay->vio.vio_dev,
						cur_q*2, pkts+i-runlen, runlen);
#endif
	} else {
		sent = rte_vhost_enqueue_burst(relay->vio.vio_dev,
						VIRTIO_RXQ, pkts,
						relay->dpdk.rx_pkts_avail);
	}
	relay->dpdk.rx_pkts_avail -= sent;
	relay->dpdk.rx_pkts_used += sent;
	assert(relay->dpdk.rx_pkts_used <= BURST_LEN);

	/* Update sent to VM stats. */
	if (sent) {
		unsigned bytes=0;
		for (int i=0; i<sent; ++i)
			bytes += pkts[i]->pkt_len;
		relay->stats.vio_tx_bytes += bytes;
		relay->stats.vio_tx += sent;
	}

	/* Free packets that have been enqueued. */
	while (sent) {
		--sent;
		rte_pktmbuf_free(pkts[sent]);
	}

	return sent;
}

static void worker_remove_virtio(vio_vf_relay_t *relay)
{
	int rcvd;
	struct rte_mbuf **pkts;

	log_debug("Removing virtio instance from worker");
	if (relay->dpdk.rx_pkts_avail) {
		log_debug("Freeing %u cached RX packets",
			relay->dpdk.rx_pkts_avail);
		rcvd = relay->dpdk.rx_pkts_avail;
		relay->stats.vio_drop_unavail += rcvd;
		pkts = relay->dpdk.rx_pkts + relay->dpdk.rx_pkts_used;
		while (rcvd) {
			--rcvd;
			rte_pktmbuf_free(pkts[rcvd]);
		}
		relay->dpdk.rx_pkts_avail = 0;
		relay->dpdk.rx_pkts_used = 0;
	}
	if (relay->vio.tx_pkts_avail) {
		log_debug("Freeing %u cached TX packets",
			relay->vio.tx_pkts_avail);
		rcvd = relay->vio.tx_pkts_avail;
		relay->stats.dpdk_drop_unavail += rcvd;
		pkts = relay->vio.tx_pkts + relay->vio.tx_pkts_used;
		while (rcvd) {
			--rcvd;
			rte_pktmbuf_free(pkts[rcvd]);
		}
		relay->vio.tx_pkts_avail = 0;
		relay->vio.tx_pkts_used = 0;
	}
	relay->vio.state = VIRTIO_UNINIT; /* Signal main thread. */
}

/*
 * Forward DPDK->virtio
 */
static inline int relay_vf2vm_traffic(vio_vf_relay_t *relay)
{
	int sent = 0;

	/* There are no buffered packets in the internal
	 * rx queue. Try to fetch packets from the VF
	 * into mbufs. */
	if (likely(relay->dpdk.rx_pkts_avail == 0))
		dpdk_rx(relay);

	/* Send dpdk to VM. */
	if (likely(relay->dpdk.rx_pkts_avail))
		sent = virtio_tx(relay);

	if (sent == -1 && relay->dpdk.rx_pkts_avail) {
		/* Virtio not ready.
		 * Free buffered packets. */
		struct rte_mbuf **pkts = relay->dpdk.rx_pkts +
					relay->dpdk.rx_pkts_used;
		int avail = relay->dpdk.rx_pkts_avail;
		relay->stats.vio_drop_unavail += avail;
		while (avail > 0) {
			--avail;
			rte_pktmbuf_free(pkts[avail]);
		}
	}

	if (unlikely(relay->vio.state == VIRTIO_REMOVING2))
		worker_remove_virtio(relay);

	if (unlikely(relay->dpdk.state == DPDK_REMOVING1)) {
		relay->dpdk.state = DPDK_REMOVING2; /* Signal other thread. */
		if (relay->vio.vio2vf_cpu == -1)
			/* There is no other thread. */
			relay->dpdk.state = DPDK_UNINIT;
	}

	return (relay->dpdk.state == DPDK_READY) ? 1 : 0;
}

static int worker_func(void *arg __attribute__((unused)))
{
	unsigned cpu = rte_lcore_id();
	worker_thread_t *this_thread = &worker_threads[cpu];

	this_thread->running=true;
	this_thread->must_stop=false;
	log_debug("New worker thread on CPU %u", this_thread->cpu);
	while (this_thread->running && !this_thread->must_stop) {
		int cpu_processed = 0;
		unsigned long long _active_relays = this_thread->active_relays;

		if (unlikely(this_thread->need_update))
			update_thread(this_thread);

		while (_active_relays) {
			unsigned w = __builtin_ffsll(_active_relays) - 1;
			assert(w < MAX_RELAYS);
			_active_relays &= (~(1ULL<<w));
			vio_vf_relay_t *relay = &virtio_vf_relays[w];

			/* Forward VM->VF. */
			if (relay->vio.vio2vf_cpu == this_thread->cpu &&
					likely(rte_spinlock_trylock(&relay->vio.sl))) {
				cpu_processed |= relay_vm2vf_traffic(relay);
				rte_spinlock_unlock(&relay->vio.sl);
			}

			/* Forward VF->VM. */
			if (relay->dpdk.vf2vio_cpu == this_thread->cpu &&
					likely(rte_spinlock_trylock(&relay->dpdk.sl))) {
				cpu_processed |= relay_vf2vm_traffic(relay);
				rte_spinlock_unlock(&relay->dpdk.sl);
			}

		}
		if (cpu_processed==0) {
		  usleep(1000);
		}
	}
	this_thread->running=false;
	log_debug("Worker thread on CPU %u ended", this_thread->cpu);
	return 0;
}

static struct rte_mempool *alloc_mempool(unsigned virtio_id, int socket_id)
{
	char buf[32];

	/* Alternate between these names to allow check before migration. */
	snprintf(buf, 32, "mempool_%u", virtio_id);
	if (rte_mempool_lookup(buf))
		snprintf(buf, 32, "mempoolx_%u", virtio_id);

	/* TODO: Increase memory for bonds. */
	return rte_pktmbuf_pool_create(buf, 4096-1, 32-1, 0,
					(vio_worker_conf.use_jumbo ||
					vio_worker_conf.enable_tso) ?
					JUMBO_MBUF_SIZE : DEFAULT_MBUF_SIZE,
					socket_id);
}

static void *init_static_vfs(void *ptr __attribute__((unused)))
{
	const struct virtio_vhostuser_conf *conf = &vio_worker_conf;

	for (unsigned w=0; w<conf->static_relay_conf.num_static_entries; ++w) {
		const char *dbdf = conf->static_relay_conf.static_relays[w].pci_dbdf;
		int vio_id = conf->static_relay_conf.static_relays[w].virtio_id;
		log_info("Adding static PCI VF '%s' to virtio %d", dbdf, vio_id);
		virtio_forwarder_add_vf(dbdf, vio_id);
	}

	return NULL;
}

int virtio_forwarders_initialize(const struct virtio_vhostuser_conf *conf)
{
	int cpu;
	unsigned w;
	cpuinfo_t *c = get_cpuinfo();
	pthread_t static_vfs_init;

	vio_worker_conf = *conf;
	memset(virtio_vf_relays, 0, sizeof(*virtio_vf_relays));

	/* Issue NUMA mismatch warnings. */
	for (w=0; w<MAX_RELAYS; ++w) {
		const struct relay_cpus *r_cpus = &conf->relay_cpus[w];
		if (r_cpus->vf2vio_cpu != -1 && r_cpus->vio2vf_cpu != -1 &&
				r_cpus->vf2vio_cpu != r_cpus->vio2vf_cpu) {
			if (c->cpus[r_cpus->vf2vio_cpu].socket
					!= c->cpus[r_cpus->vio2vf_cpu].socket)
				log_warning("CPUs specified for virtio %u (%u,%u) are on different sockets, this will negatively affect performance!",
					w, r_cpus->vf2vio_cpu,
					r_cpus->vio2vf_cpu);
			else if (c->cpus[r_cpus->vf2vio_cpu].corenum ==
					c->cpus[r_cpus->vio2vf_cpu].corenum)
				log_debug("CPUs specified for virtio %u (%u,%u) are on the same core",
					w, r_cpus->vf2vio_cpu,
					r_cpus->vio2vf_cpu);
		}
	}

	/* Initialize array of relays.*/
	for (w=0; w<MAX_RELAYS; ++w) {
		int socket_id;
		if (conf->relay_cpus[w].vio2vf_cpu != -1) {
			socket_id = c->cpus[conf->relay_cpus[w].vio2vf_cpu].numanode;
		} else {
			socket_id = SOCKET_ID_ANY;
#ifndef RTE_LIBRTE_VHOST_NUMA
			if (c->numsockets>1)
				log_warning("virtio %u has no specified CPU, NUMA memory allocation may be non-optimal on this multi-socket platform!", w);
#endif
		}
		virtio_vf_relays[w].dpdk.is_bond = false;
		virtio_vf_relays[w].vio.mempool_socket_id = socket_id;
		virtio_vf_relays[w].use_jumbo = conf->use_jumbo;
		virtio_vf_relays[w].vio.mempool=alloc_mempool(w, socket_id);
		if (!virtio_vf_relays[w].vio.mempool) {
		  log_critical("Could not alloc mempool for worker %u!", w);
		  return -1;
		}
		virtio_vf_relays[w].vio.vio2vf_cpu = -1;
		virtio_vf_relays[w].dpdk.vf2vio_cpu = -1;
		virtio_vf_relays[w].dpdk.rx_pkts_avail = 0;
		virtio_vf_relays[w].dpdk.rx_pkts_used = 0;
		virtio_vf_relays[w].vio.tx_pkts_avail = 0;
		virtio_vf_relays[w].vio.tx_pkts_used = 0;
		rte_spinlock_init(&virtio_vf_relays[w].vio.sl);
		rte_spinlock_init(&virtio_vf_relays[w].dpdk.sl);
	}

	/* Launch worker_func on all slaves. */
	memset(worker_threads, 0, sizeof(*worker_threads));
	worker_core_bitmap = conf->worker_core_bitmap;
	log_debug("Master running on core %u", rte_get_master_lcore());
	log_debug("Starting workers on CPU bitmap 0x%08llX",
		(unsigned long long)worker_core_bitmap);
	RTE_LCORE_FOREACH_SLAVE(cpu) {
		worker_thread_t *worker = &worker_threads[cpu];
		worker->cpu = cpu;
		worker->initialized = true;
	}
	rte_eal_mp_remote_launch(worker_func, NULL, SKIP_MASTER);

	/* Need to add static VFs from a separate thread: The memory required
	 * for initializing a netdev is reserved according to the socket of
	 * the calling thread. Using a pthread entails a don't care. When the
	 * master creates VFs, and there is no memory on the master's socket,
	 * DPDK will segfault. This manner of initialization is in keeping
	 * with the other methods used to add VFs (ovsdb and zmq).
	 */
	if (pthread_create(&static_vfs_init, NULL, init_static_vfs, NULL)) {
		log_error("Error adding static VFs!");
		return 0;
	}
	pthread_join(static_vfs_init, NULL);

	return 0;
}

void virtio_forwarders_shutdown(void)
{
	int cpu;

	for (cpu=0; cpu<MAX_CPUS; ++cpu) {
		if (((1ULL<<cpu) & worker_core_bitmap) == 0)
			continue;
		worker_thread_t *worker = &worker_threads[cpu];
		if (worker->initialized && worker->running) {
			log_debug("Stopping worker on CPU %u", worker->cpu);
			worker->must_stop=true;
		}
	}

	RTE_LCORE_FOREACH_SLAVE(cpu) {
		if (rte_eal_wait_lcore(cpu) < 0) {
			log_error("Error waiting for cpu %d", cpu);
			break;
		} else {
			log_debug("Worker on CPU %d stopped", cpu);
		}
	}
}

static void find_virtio2vf_cpu(vio_vf_relay_t *relay)
{
	int idlest_cpu;
	int conf_cpu = vio_worker_conf.relay_cpus[relay->id].vio2vf_cpu;

	if (conf_cpu != -1) {
		relay->vio.vio2vf_cpu = conf_cpu;
		log_debug("Using CPU %u for relay %u virtio2vf",
			relay->vio.vio2vf_cpu, relay->id);
		return;
	}

	if (relay->vio.vio2vf_cpu != -1) {
		worker_threads[relay->vio.vio2vf_cpu].need_update = true;
		relay->vio.vio2vf_cpu = -1;
	}
	idlest_cpu = naive_get_idlest_worker(relay->vio.mempool_socket_id);
	assert(idlest_cpu >= 0);
	relay->vio.vio2vf_cpu = idlest_cpu;
	log_debug("Found CPU %u for relay %u virtio2vf",
		relay->vio.vio2vf_cpu, relay->id);
}

/* Get NUMA information of guest. */
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
static int get_guest_numa(int virtionet)
{
	int guest_node = rte_vhost_get_numa_node(virtionet);

	if (guest_node == -1) {
#ifdef RTE_LIBRTE_VHOST_NUMA
		log_error("Failed to detect NUMA node of guest with vid %u",
			virtionet);
#else
		log_warning("Unable to determine NUMA node of guest. Configure DPDK with CONFIG_RTE_LIBRTE_VHOST_NUMA=y.");
#endif
		guest_node = SOCKET_ID_ANY;
	} else {
		log_debug("NUMA node of connecting guest = %d", guest_node);
	}

	return guest_node;
}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
static int migrate_numa(vio_vf_relay_t *relay, int newnode)
{
	uint8_t port_id = relay->dpdk.dpdk_port;
	struct rte_mempool *new_pool;

	log_info("Relay %u's mempool affinity requires change from %d to %d to align with the connecting guest.",
		relay->id, relay->vio.mempool_socket_id, newnode);

	if (relay->dpdk.state == DPDK_ADDED && relay->dpdk.is_bond) {
		log_info("Mempool migration not supported for bonded interfaces.");
		return 1;
	}

	/* Try to allocate a new mempool. */
	new_pool = alloc_mempool(relay->id, newnode);
	if (!new_pool) {
		log_error("Could not alloc mempool for worker %u on socket %d! Previous configuration will be retained",
			newnode, relay->id);
		return -1;
	}

	/**
	 * On success, stop the VF associated with this relay and reconfigure.
	 * See http://dpdk.org/doc/api/rte__ethdev_8h.html
	 */
	assert(relay->dpdk.state != DPDK_READY);
	if (relay->dpdk.state == DPDK_ADDED) {
		log_debug("Previously setup VF requires update. Stopping VF...");
		rte_eth_dev_stop(port_id);
	}

	/* Free old mempool. rte_mempool docs state that no other cores should
	 * use the  mempool while it is being freed. These locks are probably
	 * redundant because of the virtio and dpdk state at this point. */
	while (rte_spinlock_trylock(&relay->vio.sl) == 0);
	while (rte_spinlock_trylock(&relay->dpdk.sl) == 0);
	log_debug("Migrating mempool for relay %u...", relay->id);
	rte_mempool_free(relay->vio.mempool);
	relay->vio.mempool = new_pool;
	relay->vio.mempool_socket_id = newnode;
	rte_spinlock_unlock(&relay->vio.sl);
	rte_spinlock_unlock(&relay->dpdk.sl);

	/* Reconfigure VF if it was previously configured. */
	if (relay->dpdk.state == DPDK_ADDED) {
		int err;
		struct rte_eth_rxconf rx_conf;
		struct rte_eth_txconf tx_conf;

		log_info("Updating VF %s with the new NUMA configuration...",
			relay->dpdk.pci_dbdf);
		/* Reconfigure queues. */
		build_tx_conf(&tx_conf);
		err = rte_eth_tx_queue_setup(port_id, 0, 1024,
					relay->vio.mempool_socket_id, &tx_conf);
		if (err != 0) {
			log_error("rte_eth_tx_queue_setup(%hhu, 0, 1024) failed with error %i",
				port_id, err);
			rte_eth_dev_close(port_id);
			return -1;
		}
		build_rx_conf(&rx_conf);
		err = rte_eth_rx_queue_setup(port_id, 0, 1024,
					relay->vio.mempool_socket_id, &rx_conf,
					relay->vio.mempool);
		if (err != 0) {
			log_error("rte_eth_rx_queue_setup(%hhu, 0, 1024) failed with error %i",
				port_id, err);
			rte_eth_dev_close(port_id);
			return -1;
		}
		log_info("Successfully re-configured VF %s for relay %u",
			relay->dpdk.pci_dbdf, relay->id);
	}

	return 0;
}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
int virtio_forwarder_add_virtio(int virtionet, unsigned id)
#else
int virtio_forwarder_add_virtio(void *virtionet, unsigned id)
#endif
{
	vio_vf_relay_t *relay;

	if (id >= MAX_RELAYS) {
		log_error("Tried to add virtio with invalid ID '%u'", id);
		return -1;
	}

	relay = &virtio_vf_relays[id];
	if (relay->vio.state != VIRTIO_UNINIT) {
		/* Explicity disallow more than one connection per vhost-user
		 * socket for now (since we don't know how to identify VMs for
		 * routing). */
		log_error("Tried to add virtio with ID '%u' which is already initialized, ignoring",
			id);
		return -1;
	}

	log_debug("Adding relay for virtio id '%u' on CPU %u", id,
		relay->vio.vio2vf_cpu);
	relay->id = id;
	relay->vio.tx_q_rr = 0;
	relay->vio.vio_dev = virtionet;
#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
	relay->vio.max_queue_pairs = rte_vhost_get_vring_num(virtionet)/2;
#elif RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	relay->vio.max_queue_pairs = rte_vhost_get_queue_num(virtionet);
#else
	relay->vio.max_queue_pairs =
		((struct virtio_net *)(relay->vio.vio_dev))->virt_qp_nb;
#endif
	if (relay->vio.max_queue_pairs > MAX_MULTIQUEUE_PAIRS) {
		log_error("Tried to configure too many queues (%u), max is %u!",
			relay->vio.max_queue_pairs, MAX_MULTIQUEUE_PAIRS);
		return -1;
	}

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	/* Use guest numa to align mempool. */
	int newnode = get_guest_numa(virtionet);
	if (relay->vio.mempool_socket_id != newnode &&
			newnode != SOCKET_ID_ANY)
		migrate_numa(relay, newnode);
#endif

	find_virtio2vf_cpu(relay);
	if (((1ULL<<relay->vio.vio2vf_cpu) & worker_core_bitmap) == 0) {
		log_error("Invalid CPU %u for virtio-forwarder, no relay thread on that CPU!",
			relay->vio.vio2vf_cpu);
		return -1;
	}

#ifdef VIRTIO_ECHO
	char buf[32];
	snprintf(buf, 32, "echo_ring_%u", id);
	relay->echo_ring=rte_ring_create(buf, 2048,
					relay->vio.mempool_socket_id,
					RING_F_SP_ENQ|RING_F_SC_DEQ);
	if (relay->echo_ring==0) {
		log_error("Cannot create echo ring: %s",
			rte_strerror(rte_errno));
		return -1;
	}
	relay->dpdk.state = DPDK_READY;
	find_vf2virtio_cpu(relay);
	__sync_synchronize();
	worker_threads[relay->dpdk.vf2vio_cpu].need_update = true;
#endif

	relay->vio.state = VIRTIO_READY;
	__sync_synchronize();
	worker_threads[relay->vio.vio2vf_cpu].need_update = true;

	/* Start VF if already properly configured. */
	if (relay->dpdk.state == DPDK_ADDED) {
		int err;

		/* We now have NUMA info to use to possibly find a better CPU. */
		find_vf2virtio_cpu(relay);
		log_debug("Starting VF for relay %u", relay->id);
		err = start_eth_dev(relay->dpdk.dpdk_port);
		if (err != 0) {
			log_warning("start_eth_dev(port %u) failed with error %i ('%s')",
				relay->dpdk.dpdk_port, err, rte_strerror(-err));
		} else {
			relay->dpdk.state = DPDK_READY;
			__sync_synchronize();
			worker_threads[relay->dpdk.vf2vio_cpu].need_update = true;
		}
	}

	return 0;
}

void virtio_forwarder_remove_virtio(unsigned id)
{
	unsigned retries = 0;
	int tmpidx;
	vio_vf_relay_t *relay = &virtio_vf_relays[id];

	if (id >= MAX_RELAYS) {
		log_error("Tried to remove virtio with invalid ID '%u'", id);
		return;
	}

	if (virtio_vf_relays[id].vio.state != VIRTIO_READY)
		return;

	log_debug("Removing virtio-forwarder %u", id);
	relay->vio.state = VIRTIO_REMOVING1;
	while (relay->vio.state != VIRTIO_UNINIT && retries++ < 20)
		usleep(50000);
	if (retries >= 20)
		log_warning("Timeout waiting for thread to release virtio %u!",
			id);
	log_debug("Removed virtio-forwarder %u from CPU %u", id,
		relay->vio.vio2vf_cpu);

	tmpidx = relay->vio.vio2vf_cpu;
	relay->vio.vio2vf_cpu = -1;
	relay->vio.tx_q_bitmap = 0;
	relay->vio.rx_q_bitmap = 0;
	__sync_synchronize();
	worker_threads[tmpidx].need_update = true;

#if RTE_VERSION < RTE_VERSION_NUM(16,7,0,0)
	struct virtio_net *dev=relay->vio.vio_dev;
	log_debug("virtio-forwarder ended, final virtqueue state: txq avail idx=%hhu, txq avail last used=%hhu, txq used idx=%hhu, rxq avail idx=%hhu, rxq avail last used=%hhu, rxq used idx=%hhu",
		dev->virtqueue[VIRTIO_TXQ]->avail->idx,
		dev->virtqueue[VIRTIO_TXQ]->last_used_idx,
		dev->virtqueue[VIRTIO_TXQ]->used->idx,
		dev->virtqueue[VIRTIO_RXQ]->avail->idx,
		dev->virtqueue[VIRTIO_RXQ]->last_used_idx,
		dev->virtqueue[VIRTIO_RXQ]->used->idx );
#endif
	log_debug("stats: virtio_rx=%"PRIu64", dpdk_tx=%"PRIu64", dpdk_drop_full=%"PRIu64", dpdk_drop_unavail=%"PRIu64,
		relay->stats.vio_rx, relay->stats.dpdk_tx,
		relay->stats.dpdk_drop_full, relay->stats.dpdk_drop_unavail);
	log_debug("stats: dpdk_rx=%"PRIu64", virtio_tx=%"PRIu64", virtio_drop_full=%"PRIu64", virtio_drop_unavail=%"PRIu64,
		relay->stats.dpdk_rx, relay->stats.vio_tx,
		relay->stats.vio_drop_full, relay->stats.vio_drop_unavail);

#ifdef VIRTIO_ECHO
	rte_ring_free(relay->echo_ring);
	relay->dpdk.state=DPDK_UNINIT;
	tmpidx=relay->dpdk.vf2vio_cpu;
	relay->dpdk.vf2vio_cpu = -1;
	__sync_synchronize();
	worker_threads[tmpidx].need_update = true;
#endif

	/* Stop VF. */
	if (relay->dpdk.state == DPDK_READY) {
		log_debug("Stopping VF for relay %u", relay->id);
		rte_eth_dev_stop(relay->dpdk.dpdk_port);
		relay->dpdk.state = DPDK_ADDED;
		__sync_synchronize();
		worker_threads[relay->dpdk.vf2vio_cpu].need_update = true;
	}
}

void virtio_forwarder_vring_state_change(unsigned id, unsigned queue_id, int enable)
{
	vio_vf_relay_t *relay;
	unsigned shiftwidth;
	unsigned active;
	unsigned idx;
	unsigned bmp;

	if (id>=MAX_RELAYS) {
		log_error("Tried to change queuer state of virtio with invalid ID '%u'", id);
		return;
	}

	relay = &virtio_vf_relays[id];
	shiftwidth = (queue_id>>1); /* Divide to get queue pair. */
	if (enable) {
		if (queue_id & 1) /* Odd is TX. */
			relay->vio.tx_q_bitmap |= (1ULL<<shiftwidth);
		else
			relay->vio.rx_q_bitmap |= (1ULL<<shiftwidth);
	} else {
		if (queue_id & 1)
			relay->vio.tx_q_bitmap &= ~(1ULL<<shiftwidth);
		else
			relay->vio.rx_q_bitmap &= ~(1ULL<<shiftwidth);
	}

	active = __builtin_popcountl(relay->vio.rx_q_bitmap); /* nnz bits. */
	if ((active & (active - 1)) == 0)
		relay->vio.pow2queues = true;
	else
		relay->vio.pow2queues = false;

	/* Populate lookup table: Index 0-n-1 maps to queue number (usually 1:1,
	 * but not necessarily). */
	idx = 0;
	bmp = relay->vio.rx_q_bitmap;
	while (bmp) {
		unsigned val = (__builtin_ffsl(bmp) - 1);
		bmp &= ~(1ULL<<val);
		relay->vio.rx_q_lut[idx] = val;
		++idx;
	}
	relay->vio.rx_q_active = active;
	log_debug("vring state change on queue_id=%hu (enable=%d) on relay %d, rx_q_bitmap=0x%08"PRIx64", tx_q_bitmap=0x%08"PRIx64", rx_q_active=%u",
		queue_id, enable, id, relay->vio.rx_q_bitmap,
		relay->vio.tx_q_bitmap, relay->vio.rx_q_active);
}

void virtio_forwarders_remove_all(void)
{
	for (unsigned id=0; id<MAX_RELAYS; ++id) {
		virtio_forwarder_remove_virtio(id);
		vio_vf_relay_t *relay = &virtio_vf_relays[id];
		if (relay->dpdk.state == DPDK_READY ||
				relay->dpdk.state == DPDK_ADDED)
			virtio_forwarder_remove_vf(relay->dpdk.pci_dbdf, id);
	}
}

void virtio_forwarders_print_stats(int ptmfd)
{
	for (unsigned id=0; id<MAX_RELAYS; ++id) {
		if (virtio_vf_relays[id].vio.state != VIRTIO_READY &&
				virtio_vf_relays[id].dpdk.state != DPDK_READY)
			continue;

		vio_vf_relay_t *relay = &virtio_vf_relays[id];
		print_stat(ptmfd, "relay%u.virtio_rx=%"PRIu64"\n", id,
			relay->stats.vio_rx);
		print_stat(ptmfd, "relay%u.virtio_rx_bytes=%"PRIu64"\n",
			id, relay->stats.vio_rx_bytes);
		print_stat(ptmfd, "relay%u.dpdk_tx=%"PRIu64"\n", id,
			relay->stats.dpdk_tx);
		print_stat(ptmfd, "relay%u.dpdk_tx_bytes=%"PRIu64"\n", id,
			relay->stats.dpdk_tx_bytes);
		print_stat(ptmfd, "relay%u.dpdk_drop_full=%"PRIu64"\n", id,
			relay->stats.dpdk_drop_full);
		print_stat(ptmfd, "relay%u.dpdk_drop_unavail=%"PRIu64"\n", id,
			relay->stats.dpdk_drop_unavail);
		print_stat(ptmfd, "relay%u.dpdk_rx=%"PRIu64"\n", id,
			relay->stats.dpdk_rx);
		print_stat(ptmfd, "relay%u.dpdk_rx_bytes=%"PRIu64"\n", id,
			relay->stats.dpdk_rx_bytes);
		print_stat(ptmfd, "relay%u.virtio_tx=%"PRIu64"\n", id,
			relay->stats.vio_tx);
		print_stat(ptmfd, "relay%u.virtio_tx_bytes=%"PRIu64"\n", id,
			relay->stats.vio_tx_bytes);
		print_stat(ptmfd, "relay%u.virtio_drop_full=%"PRIu64"\n", id,
			relay->stats.vio_drop_full);
		print_stat(ptmfd, "relay%u.virtio_drop_unavail=%"PRIu64"\n", id,
			relay->stats.vio_drop_unavail);
	}
}

/** Converts virtio_state_t to debug string. */
static void
virtio_state_to_str(vio_state_t state, char *ans, size_t cch_max_ans)
{
	switch (state) {
	case VIRTIO_UNINIT:
		strncpy(ans, "VIRTIO_UNINIT", cch_max_ans);
		break;

	case VIRTIO_READY:
		strncpy(ans, "VIRTIO_READY", cch_max_ans);
		break;

	case VIRTIO_REMOVING1:
		strncpy(ans, "VIRTIO_REMOVING1", cch_max_ans);
		break;

	case VIRTIO_REMOVING2:
		strncpy(ans, "VIRTIO_REMOVING2", cch_max_ans);
		break;

	default:
		snprintf(ans, cch_max_ans, "0x%X", state);
		break;
	}
}

/** Converts dpdk_state_t to debug string. */
static void
dpdk_state_to_str(dpdk_state_t state, char *ans, size_t cch_max_ans)
{
	switch (state) {
	case DPDK_UNINIT:
		strncpy(ans, "DPDK_UNINIT", cch_max_ans);
		break;

	case DPDK_ADDED:
		strncpy(ans, "DPDK_ADDED", cch_max_ans);
		break;

	case DPDK_READY:
		strncpy(ans, "DPDK_READY", cch_max_ans);
		break;

	case DPDK_REMOVING1:
		strncpy(ans, "DPDK_REMOVING1", cch_max_ans);
		break;

	case DPDK_REMOVING2:
		strncpy(ans, "DPDK_REMOVING2", cch_max_ans);
		break;

	default:
		snprintf(ans, cch_max_ans, "0x%X", state);
		break;
	}
}

static void reset_rate_stats(unsigned id)
{
	vio_vf_relay_t const *r = virtio_vf_relays + id;
	relay_prev_counters_t *prev_stats = relay_prev_counters + id;
	/* VM2VF */
	prev_stats->virtio_rx = r->stats.vio_rx;
	prev_stats->virtio_rx_bytes = r->stats.vio_rx_bytes;
	prev_stats->dpdk_tx = r->stats.dpdk_tx;
	prev_stats->dpdk_tx_bytes = r->stats.dpdk_tx_bytes;
	/* VF2VM */
	prev_stats->dpdk_rx = r->stats.dpdk_rx;
	prev_stats->dpdk_rx_bytes = r->stats.dpdk_rx_bytes;
	prev_stats->virtio_tx = r->stats.vio_tx;
	prev_stats->virtio_tx_bytes = r->stats.vio_tx_bytes;
	/* Time. */
	prev_stats->time_prev = rte_get_timer_cycles();
}

void reset_all_rate_stats(unsigned delay_ms)
{
	for (unsigned id=0; id<MAX_RELAYS; ++id)
		reset_rate_stats(id);

	if (delay_ms > 0)
		rte_delay_ms(delay_ms);
}

void
virtio_forwarder_get_stats(unsigned virtio_id, struct virtio_worker_stats *stats,
			const float *tic_period)
{
	assert(virtio_id < MAX_RELAYS);
	vio_vf_relay_t const *r = virtio_vf_relays + virtio_id;
	relay_prev_counters_t *prev_stats = relay_prev_counters + virtio_id;

	memset(stats, 0, sizeof(struct virtio_worker_stats));

	/**/
	/* Calculate time since last call/reset. */
	float elapsed = (rte_get_timer_cycles() - prev_stats->time_prev) *
		(*tic_period);

	vio_state_t virtio_state = r->vio.state;
	virtio_state_to_str(virtio_state, stats->virtio_internal_state,
			sizeof(stats->virtio_internal_state));
	if (virtio_state == VIRTIO_READY) {
		stats->virtio2vf_active = true;
		stats->virtio2vf_cpu = r->vio.vio2vf_cpu;
		stats->virtio_rx = r->stats.vio_rx;
		stats->virtio_rx_bytes = r->stats.vio_rx_bytes;
		stats->dpdk_tx = r->stats.dpdk_tx;
		stats->dpdk_tx_bytes = r->stats.dpdk_tx_bytes;
		stats->dpdk_drop_full = r->stats.dpdk_drop_full;
		stats->dpdk_drop_unavail = r->stats.dpdk_drop_unavail;
		/* Rates. */
		stats->virtio_rx_rate = (stats->virtio_rx -
			prev_stats->virtio_rx) / elapsed;
		stats->virtio_rx_byte_rate = (stats->virtio_rx_bytes -
			prev_stats->virtio_rx_bytes) / elapsed;
		stats->dpdk_tx_rate = (stats->dpdk_tx - prev_stats->dpdk_tx) /
			elapsed;
		stats->dpdk_tx_byte_rate = (stats->dpdk_tx_bytes -
			prev_stats->dpdk_tx_bytes) / elapsed;
	}

	/**/

	dpdk_state_t dpdk_state = r->dpdk.state;
	dpdk_state_to_str(dpdk_state, stats->dpdk_internal_state,
			sizeof(stats->dpdk_internal_state));
	if (dpdk_state == DPDK_ADDED || dpdk_state == DPDK_READY) {
		strncpy(stats->pci_dbdf, r->dpdk.pci_dbdf,
			sizeof(stats->pci_dbdf));
		stats->vf2virtio_active = true;
		stats->vf2virtio_cpu = r->dpdk.vf2vio_cpu;
		stats->dpdk_rx = r->stats.dpdk_rx;
		stats->dpdk_rx_bytes = r->stats.dpdk_rx_bytes;
		stats->virtio_tx = r->stats.vio_tx;
		stats->virtio_tx_bytes = r->stats.vio_tx_bytes;
		stats->virtio_drop_full = r->stats.vio_drop_full;
		stats->virtio_drop_unavail = r->stats.vio_drop_unavail;
		/* Rates. */
		stats->dpdk_rx_rate = (stats->dpdk_rx - prev_stats->dpdk_rx) /
			elapsed;
		stats->dpdk_rx_byte_rate = (stats->dpdk_rx_bytes -
			prev_stats->dpdk_rx_bytes) / elapsed;
		stats->virtio_tx_rate = (stats->virtio_tx -
			prev_stats->virtio_tx) / elapsed;
		stats->virtio_tx_byte_rate = (stats->virtio_tx_bytes -
			prev_stats->virtio_tx_bytes) / elapsed;
	}
	reset_rate_stats(virtio_id);

	/* Note, this is slightly different than AND-ing together
	 * virtio2vf_active with vf2virtio_active, because vf2virtio_active is
	 * true when the state is DPDK_ADDED *or* DPDK_READY. */
	stats->active = (virtio_state == VIRTIO_READY && dpdk_state == DPDK_READY);
	stats->socket_id = r->vio.mempool_socket_id;
}

uint64_t get_eal_core_map(void)
{
	return worker_core_bitmap;
}
