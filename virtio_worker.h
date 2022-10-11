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

#ifndef _VIRTIO_WORKER_THREAD
#define _VIRTIO_WORKER_THREAD

#include "virtio_vhostuser.h"
#include <stdio.h>
#include <rte_version.h>
#include <stdbool.h>
#include <stdint.h>
#include <rte_ethdev.h>

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
#define NUM_PKTMBUF_POOL 4096

/**
 * Required size of char[] buffer for virtio worker internal state debug
 * string.
 */
#define VIRTIO_WORKER_INTERNAL_STATE_CCH_MAX 24

/* Shared compat definitions. */
#if RTE_VERSION_NUM(17, 11, 0, 0) <= RTE_VERSION
typedef uint16_t dpdk_port_t;
#else
typedef uint8_t dpdk_port_t;
#endif

/* Shared array of vhost_user socket names, indexed by virtio-relay ID */
extern char *relay_ifname_map[MAX_RELAYS];

extern struct virtio_vhostuser_conf g_vio_worker_conf;

/*
 * Type declarations
 */
#if RTE_VERSION_NUM(17, 5, 0, 0) <= RTE_VERSION
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

/** Statistics for an individual worker. */
struct virtio_worker_stats
{
	/* String representation of relay->virtio.state. */
	char virtio_internal_state[VIRTIO_WORKER_INTERNAL_STATE_CCH_MAX];

	/* True if relay->virtio.state is VIRTIO_READY. */
	bool virtio2vf_active;

	/* The following fields are only valid when virtio2vf_active is true. */
	char vhost_socket_name[128];
	int virtio2vf_cpu;
	uint64_t virtio_rx;
	uint64_t virtio_rx_bytes;
	uint64_t dpdk_tx;
	uint64_t dpdk_tx_bytes;
	uint64_t dpdk_drop_full;
	uint64_t dpdk_drop_unavail;
	/* Rates. */
	float virtio_rx_rate;
	float virtio_rx_byte_rate;
	float dpdk_tx_rate;
	float dpdk_tx_byte_rate;

	/**/

	/* String representation of relay->dpdk.state. */
	char dpdk_internal_state[VIRTIO_WORKER_INTERNAL_STATE_CCH_MAX];

	/* True if relay->dpdk.state is DPDK_ADDED or DPDK_READY. */
	bool vf2virtio_active;

	/* The following fields are only valid when vf2virtio_active is true. */
	char pci_dbdf[RTE_ETH_NAME_MAX_LEN];
	int vf2virtio_cpu;
	uint64_t dpdk_rx;
	uint64_t dpdk_rx_bytes;
	uint64_t virtio_tx;
	uint64_t virtio_tx_bytes;
	uint64_t virtio_drop_full;
	uint64_t virtio_drop_unavail;
	/* Rates. */
	float dpdk_rx_rate;
	float dpdk_rx_byte_rate;
	float virtio_tx_rate;
	float virtio_tx_byte_rate;

	/**/

	/* True if virtio2vf_active and vf2virtio_active are both true. */
	bool active;

	/* NUMA node where the relay's memory pool is allocated. */
	unsigned socket_id;
};

/* Structure describing the virtio side of a relay */
struct relay_virtio {
	int vio2vf_cpu;
	unsigned max_queue_pairs;
	uint64_t rx_q_bitmap;
	volatile uint64_t tx_q_bitmap;
	unsigned rx_q_active;
	uint8_t rx_q_lut[MAX_MULTIQUEUE_PAIRS];
	unsigned tx_q_rr; /* round robin state of tx queue processing for multi-queue, 0 <= tx_q_rr < max_queue_pairs. */
	bool pow2queues;
	volatile vio_state_t state;
#if RTE_VERSION_NUM(16, 7, 0, 0) <= RTE_VERSION
	int vio_dev;
#else
	void *vio_dev;
#endif
	struct rte_mempool *mempool;
	int mempool_socket_id;
	struct rte_mbuf *tx_pkts[BURST_LEN];
	unsigned tx_pkts_avail, tx_pkts_used;
	rte_spinlock_t sl;
	volatile bool lm_pending;
} __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

/* Structure describing the DPDK/VF side of a relay */
struct relay_dpdk {
	int vf2vio_cpu;
	volatile dpdk_state_t state;
	bool is_bond;
	unsigned num_slaves;
	dpdk_port_t dpdk_port;
	char pci_dbdf[20];
	struct rte_mbuf *rx_pkts[BURST_LEN];
	unsigned rx_pkts_avail, rx_pkts_used;
	rte_spinlock_t sl;
} __attribute__ ((aligned (RTE_CACHE_LINE_SIZE)));

/* Per relay statistics */
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

/* Main relay type definition. Combines the virtio and VF side structs among other things */
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

/**
 * @brief Gets statistics for the individual worker @a virtio_id.
 * @param tic_period Pointer to the processor tic period in seconds
 */
void
virtio_forwarder_get_stats(unsigned virtio_id, struct virtio_worker_stats *stats,
			const float *tic_period);

/**
 * @brief Reset the rate statistics for all relays.
 * @param delay_ms Time in milliseconds to wait after resetting the counters.
 */
void reset_all_rate_stats(unsigned delay_ms);

/**
 * @brief Get index of the first idle relay.
 */
int virtio_get_free_relay_id(char **socket_map);

/**
 * @brief Check whether relay has the given device assigned.
 */
bool virtio_relay_has_device(unsigned id, const char *dev);

/**
 * @brief Set the migration pending flag on the relay.
 */
void virtio_set_lm_pending(int relay_id);

/**
 * @brief Entrypoint to initialize relays and start virtio-forwarder workers.
 */
int virtio_forwarders_initialize(void);

/**
 * @brief Gracefully shuts down worker threads
 */
void virtio_forwarders_shutdown(void);

/**
 * @brief Adds a virtio_net interface to a specific virtio-forwarder relay
 */
#if RTE_VERSION_NUM(16, 7, 0, 0) <= RTE_VERSION
int virtio_forwarder_add_virtio(int virtio_net, unsigned id);
#else
int virtio_forwarder_add_virtio(void *virtio_net, unsigned id);
#endif

/**
 * @brief Remove a virtio_net interface from a virtio-forwarder relay
 */
void virtio_forwarder_remove_virtio(unsigned id);

/**
 * @brief Removes all virtio_net and VF side instances from all relays
 */
void virtio_forwarders_remove_all(void);

/**
 * @brief Callback function helper to process a state change on a vring
 */
void
virtio_forwarder_vring_state_change(unsigned id, unsigned queue_id, int enable);

/**
 * @brief Add an SR-IOV VF to the virtio-forwarder using DPDK hotplug
 * @param pci_dbdf PCI domain:bus:device.function address string, e.g. "0000:05:0f.5"
 * @param virtio_id VF number from 0-59, maps 1:1 to a virtio instance
 * @param conditional True to report success on multiple identical adds.
 * @return 0 if success, non-zero on failure
 */
int
virtio_forwarder_add_vf2(const char *pci_dbdf, unsigned virtio_id,
			bool conditional);

/**
 * @brief Like virtio_forwarder_add_vf2(), but always performs an unconditional add.
 */
int virtio_forwarder_add_vf(const char *pci_dbdf, unsigned virtio_id);

/**
 * @brief Remove an SR-IOV VF from the virtio-forwarder using DPDK hotplug
 * @param pci_dbdf PCI domain:bus:device.function address string, e.g. "0000:05:0f.5"
 * @param virtio_id VF number from 0-59, maps 1:1 to a virtio instance
 * @param conditional True to report success on multiple identical adds.
 * @return 0 if success, non-zero on failure
 */
int
virtio_forwarder_remove_vf2(const char *pci_dbdf, unsigned virtio_id,
			bool conditional);

/**
 * @brief Add bond to virtio-forwarder
 * @param slave_dbdfs list od PCI domain:bus:device.function address strings
 * @param name name of new link bonding device
 * @param mode mode to initialize bonding device in
 * @param num_slaves number of slave device addresses contained in slave_dbdfs
 * @param virtio_id virtio instance to which the bond must be connected
 */
int virtio_forwarder_bond_add(char slave_dbdfs[MAX_NUM_BOND_SLAVES][RTE_ETH_NAME_MAX_LEN],
			unsigned num_slaves, const char *name, uint8_t mode,
			unsigned virtio_id);

/**
 * @brief Change the cpus that service a relay.
 * @param relay_number Relay instance to be altered.
 * @param new_virtio2vf_cpu New cpu in the virtio to VF direction.
 * @param new_vf2virtio_cpu New cpu in the VF to virtio direction.
 * @return 0 if success, non-zero on failure
 */
int
migrate_relay_cpus(int relay_number, int new_virtio2vf_cpu,
			int new_vf2virtio_cpu);

/**
 * @brief Get the worker core mask.
 * @return The worker core mask.
 */
uint64_t get_eal_core_map(void);

/**
 * @brief Like virtio_forwarder_remove_vf2(), but always performs an unconditional
 * remove.
 */
int virtio_forwarder_remove_vf(const char *pci_dbdf, unsigned virtio_id);

void virtio_forwarders_print_stats(int ptmfd);

/**
 * @brief Helper function to get a relay instance given a relay_id
 * @param id relay ID associated with the relay instance
 * @return A pointer to the correct relay (vio_vf_relay_t)
 */
vio_vf_relay_t * get_relay_from_id(unsigned id);

#endif // _VIRTIO_WORKER_THREAD
