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

#include "zmq_stats.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "virtioforwarder.pb-c.h"

#define __MODULE__ "zmq_stats"
#include "log.h"
#include "sriov.h"
#include "virtio_worker.h"
#include "zmq_service.h"
#include <rte_cycles.h>

static float stats_timer_period;

/**
 * In the future, the memory used to construct stats responses could be
 * dynamically allocated. For the initial implementation, pre-allocating the
 * maximum required space, on the stack, on each request seems reasonable.
 */
struct stats_response_buffer
{
	/* Storage for stats coming out of virtio_worker.c. */
	struct virtio_worker_stats worker_stats[MAX_RELAYS];

	/* Storage for bits and pieces of a Protocol Buffers stats response. */
	Virtioforwarder__RelayState relay_state[MAX_RELAYS];
	Virtioforwarder__RelayState__CPU cpu[MAX_RELAYS];
	Virtioforwarder__RelayState__VF vf[MAX_RELAYS];
	Virtioforwarder__RelayState__VFtoVM vf_to_vm[MAX_RELAYS];
	Virtioforwarder__RelayState__VMtoVF vm_to_vf[MAX_RELAYS];

	/*
	 * Protocol Buffers wants an array of pointers to Virtioforwarder__RelayState,
	 * not an actual array of Virtioforwarder__RelayState.
	 */
	Virtioforwarder__RelayState *relay_state_ptrs[MAX_RELAYS];
};

/**
 * Perform query for a specific @a relay, store the result in @a b, and return
 * an updated value for the next free output position @a j.
 */
static size_t
relay_query(
	uint32_t relay, bool include_inactive, size_t j,
	struct stats_response_buffer *b)
{
	if (relay >= MAX_RELAYS) {
		/* Invalid relay number. */
		return j;
	}

	/* Query virtio_worker.c for the relay information. */
	struct virtio_worker_stats *s = b->worker_stats + j;
	virtio_forwarder_get_stats(relay, s, &stats_timer_period);

	if (!include_inactive && !s->active) {
		/* Exclude inactive relay. */
		return j;
	}

	/* Present the result in a form Protocol Buffers can understand. */
	Virtioforwarder__RelayState *relay_state = b->relay_state + j;
	virtioforwarder__relay_state__init(relay_state);

	Virtioforwarder__RelayState__CPU *cpu = b->cpu + j;
	virtioforwarder__relay_state__cpu__init(cpu);
	Virtioforwarder__RelayState__VF *vf = b->vf + j;
	virtioforwarder__relay_state__vf__init(vf);

	Virtioforwarder__RelayState__VMtoVF *vm_to_vf = b->vm_to_vf + j;
	virtioforwarder__relay_state__vmto_vf__init(vm_to_vf);
	relay_state->vm_to_vf = vm_to_vf;
	vm_to_vf->active = s->virtio2vf_active;
	vm_to_vf->internal_state = s->virtio_internal_state;
	if (vm_to_vf->active) {
		cpu->has_vm_to_vf = true;
		cpu->vm_to_vf = s->virtio2vf_cpu;

		vm_to_vf->has_pkts_rx_from_vm = true;
		vm_to_vf->pkts_rx_from_vm = s->virtio_rx;
		vm_to_vf->has_bytes_rx_from_vm = true;
		vm_to_vf->bytes_rx_from_vm = s->virtio_rx_bytes;
		vm_to_vf->has_pkts_tx_to_vf = true;
		vm_to_vf->pkts_tx_to_vf = s->dpdk_tx;
		vm_to_vf->has_bytes_tx_to_vf = true;
		vm_to_vf->bytes_tx_to_vf = s->dpdk_tx_bytes;
		vm_to_vf->has_pkts_dropped_vf_queue_full = true;
		vm_to_vf->pkts_dropped_vf_queue_full = s->dpdk_drop_full;
		vm_to_vf->has_pkts_dropped_vf_not_connected = true;
		vm_to_vf->pkts_dropped_vf_not_connected = s->dpdk_drop_unavail;
		/* Rates. */
		vm_to_vf->pkt_rate_rx_from_vm = s->virtio_rx_rate;
		vm_to_vf->byte_rate_rx_from_vm = s->virtio_rx_byte_rate;
		vm_to_vf->pkt_rate_tx_to_vf = s->dpdk_tx_rate;
		vm_to_vf->byte_rate_tx_to_vf = s->dpdk_tx_byte_rate;
	}

	Virtioforwarder__RelayState__VFtoVM *vf_to_vm = b->vf_to_vm + j;
	virtioforwarder__relay_state__vfto_vm__init(vf_to_vm);
	relay_state->vf_to_vm = vf_to_vm;
	vf_to_vm->active = s->vf2virtio_active;
	vf_to_vm->internal_state = s->dpdk_internal_state;
	if (vf_to_vm->active) {
		cpu->has_vf_to_vm = true;
		cpu->vf_to_vm = s->vf2virtio_cpu;

		vf_to_vm->has_pkts_rx_from_vf = true;
		vf_to_vm->pkts_rx_from_vf = s->dpdk_rx;
		vf_to_vm->has_bytes_rx_from_vf = true;
		vf_to_vm->bytes_rx_from_vf = s->dpdk_rx_bytes;
		vf_to_vm->has_pkts_tx_to_vm = true;
		vf_to_vm->pkts_tx_to_vm = s->virtio_tx;
		vf_to_vm->has_bytes_tx_to_vm = true;
		vf_to_vm->bytes_tx_to_vm = s->virtio_tx_bytes;
		vf_to_vm->has_pkts_dropped_vm_queue_full = true;
		vf_to_vm->pkts_dropped_vm_queue_full = s->virtio_drop_full;
		vf_to_vm->has_pkts_dropped_vm_not_connected = true;
		vf_to_vm->pkts_dropped_vm_not_connected = s->virtio_drop_unavail;
		/* Rates. */
		vf_to_vm->pkt_rate_rx_from_vf = s->dpdk_rx_rate;
		vf_to_vm->byte_rate_rx_from_vf = s->dpdk_rx_byte_rate;
		vf_to_vm->pkt_rate_tx_to_vm = s->virtio_tx_rate;
		vf_to_vm->byte_rate_tx_to_vm = s->virtio_tx_byte_rate;
	}

	/* Populate relay_state vf and cpu if they have any interesting contents. */
	if (cpu->has_vm_to_vf || cpu->has_vf_to_vm) {
		relay_state->cpu = cpu;
	}
	if (vf_to_vm->active) {
		vf->pci_addr_str = s->pci_dbdf;
		relay_state->vf = vf;
	}

	/* The following fields are always valid. */
	relay_state->id = relay;
	relay_state->active = s->active;
	relay_state->socket_id = s->socket_id;
	/* TBA: relay_state->ident = ... */

	b->relay_state_ptrs[j] = relay_state;
	return j + 1;
}

/** Handles a StatsRequest. */
static size_t
handle_StatsRequest(
	struct zmq_service *service __attribute__((unused)),
	uint8_t const *request_buffer, size_t cb_request, uint8_t *response_buffer,
	size_t cb_response_buffer)
{
	Virtioforwarder__StatsResponse response;
	virtioforwarder__stats_response__init(&response);

	Virtioforwarder__StatsRequest *pc =
	virtioforwarder__stats_request__unpack(
		NULL,
		cb_request,
		request_buffer
	);

	if (!pc) {
		response.status = VIRTIOFORWARDER__STATS_RESPONSE__STATUS__EBADR;
		goto pack_response;
	}
	else if (pc->n_relay > MAX_RELAYS) {
		response.status = VIRTIOFORWARDER__STATS_RESPONSE__STATUS__E2BIG;
		goto pack_response;
	}

	bool include_inactive = !pc->has_include_inactive || pc->include_inactive;
	unsigned delay = pc->delay; /* Defaults to 0. */
	reset_all_rate_stats(delay);

	/* Construct a response consumable by protoc-c generated code. */
	struct stats_response_buffer b;
	memset(&b, 0, sizeof(b));

	if (pc->n_relay) {
		/* Specific relays query. */
		for (size_t i = 0; i < pc->n_relay; ++i) {
			response.n_relay = relay_query(
				pc->relay[i], include_inactive, response.n_relay, &b
			);
		}
	}
	else {
		/* All relays query. */
		for (size_t i = 0; i < MAX_RELAYS; ++i) {
			response.n_relay = relay_query(
				(uint32_t) i, include_inactive, response.n_relay, &b
			);
		}
		if (include_inactive) {
			/* Loop [0, MAX_RELAYS) should not yield any invalid relays. */
			assert(response.n_relay == MAX_RELAYS);
		}
	}

	if (response.n_relay) {
		response.relay = b.relay_state_ptrs;
	}

pack_response:;
	if (pc) {
		virtioforwarder__stats_request__free_unpacked(
			pc, NULL
		);
	}

	size_t cb_response =
		virtioforwarder__stats_response__get_packed_size(&response);
	assert(cb_response <= cb_response_buffer);
	return virtioforwarder__stats_response__pack(
		&response, response_buffer
	);
}

/** Destructor for stats service. */
static void
stats_free(struct zmq_service *service)
{
	log_debug("Freeing ZeroMQ stats service: %p", service);
	zmq_service_free(service);
}

/** Constructor for stats service. */
static int
stats_setup(
	struct zmq_service *service, void *setup_arg __attribute__((unused)))
{
	service->handle_request = &handle_StatsRequest;
	service->destructor = &stats_free;
	service->max_request_cb = 512;
	service->max_response_cb = sizeof(struct stats_response_buffer);
	return 0;
}

struct zmq_service *
zmq_stats_service_alloc(void)
{
	struct zmq_service *ans;

	ans = zmq_service_alloc(__MODULE__, &stats_setup, NULL);
	if (ans) {
		log_debug("Allocated ZeroMQ stats service: %p", ans);
		log_debug(
			"Overhead per request: %zuB", sizeof(struct stats_response_buffer)
		);
		stats_timer_period = 1.0f/rte_get_timer_hz();
	}
	else {
		log_critical("Failed to allocate ZeroMQ stats service");
	}

	return ans;
}
