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

#define MAX_NUM_BOND_SLAVES 8

int virtio_forwarders_initialize(const struct virtio_vhostuser_conf *conf);
void virtio_forwarders_shutdown(void);

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
int virtio_forwarder_add_virtio(int virtio_net, unsigned id);
#else
int virtio_forwarder_add_virtio(void *virtio_net, unsigned id);
#endif

void virtio_forwarder_remove_virtio(unsigned id);

void virtio_forwarders_remove_all(void);

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
migrate_relay_cpus (int relay_number, int new_virtio2vf_cpu,
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
 * Required size of char[] buffer for virtio worker internal state debug
 * string.
 */
#define VIRTIO_WORKER_INTERNAL_STATE_CCH_MAX 24

/** Statistics for an individual worker. */
struct virtio_worker_stats
{
	/* String representation of relay->virtio.state. */
	char virtio_internal_state[VIRTIO_WORKER_INTERNAL_STATE_CCH_MAX];

	/* True if relay->virtio.state is VIRTIO_READY. */
	bool virtio2vf_active;

	/* The following fields are only valid when virtio2vf_active is true. */
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

#endif // _VIRTIO_WORKER_THREAD
