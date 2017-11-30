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

#include "zmq_core_sched.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "virtioforwarder.pb-c.h"

#define __MODULE__ "zmq_core_sched"
#include "log.h"
#include "virtio_worker.h"
#include "zmq_service.h"

uint32_t eal_cores_buffer[64]; /* Buffer to store core query response. */

/** Converts CoreSchedRequest.Op to string. */
static char const *
Virtioforwarder__CoreSchedRequest__Op__to_str(
	Virtioforwarder__CoreSchedRequest__Op op, char *ans, size_t len)
{
	switch (op)
	{
	case VIRTIOFORWARDER__CORE_SCHED_REQUEST__OP__UPDATE:
		return "UPDATE";
	case VIRTIOFORWARDER__CORE_SCHED_REQUEST__OP__GET_EAL_CORES:
		return "GET_EAL_CORES";
	default:
		snprintf(ans, len, "Virtioforwarder__CoreSchedRequest__Op(%i)", op);
		return ans;
	}
}
#define Virtioforwarder__CoreSchedRequest__Op_CCH_MAX 64

/** Converts CoreSchedRequest to string. */
static char const *
Virtioforwarder__CoreSchedRequest__to_str(
	Virtioforwarder__CoreSchedRequest const *pc, char *ans, size_t len)
{
	char op_str[Virtioforwarder__CoreSchedRequest__Op_CCH_MAX];

	snprintf(ans, len, "[op=%s]",
		Virtioforwarder__CoreSchedRequest__Op__to_str(
			pc->op, op_str, sizeof op_str
		)
	);

	return ans;
}
#define Virtioforwarder__CoreSchedRequest__CCH_MAX 128

/** Range checks CoreSchedRequest fields. */
static bool
validate_CoreSchedRequest(Virtioforwarder__CoreSchedRequest const *pc)
{
	/* Check for valid relays. */
	if (pc->n_relay_cpu_map) {
		for (size_t i=0; i<pc->n_relay_cpu_map; ++i) {
			Virtioforwarder__CoreSchedRequest__RelayCPU *cpu = pc->relay_cpu_map[i];
			if (cpu->relay_number > MAX_RELAYS) {
				log_warning("Bad core scheduler configuration: relay_number=%d > MAX_RELAYS= %d",
							 cpu->relay_number, MAX_RELAYS);
				return false;
			}
		}
	}

	return true;
}

/** Sets detailed response status. */
static void
handle_CoreSchedRequest_set_error_code(
	Virtioforwarder__CoreSchedResponse *response, char const *error_code_source,
	int32_t error_code)
{
	if (!error_code) {
		response->status = VIRTIOFORWARDER__CORE_SCHED_RESPONSE__STATUS__OK;
	} else {
		assert(
			response->status ==
			VIRTIOFORWARDER__CORE_SCHED_RESPONSE__STATUS__ERROR
		);
		/* The double cast is to suppress -Wcast-qual, on the assumption that
		 * the response serializer will not actually attempt to write to this
		 * memory.
		 */
		response->error_code_source = (char *)(intptr_t) error_code_source;
		response->error_code = error_code;
		response->has_error_code = true;
	}
}

/** Handles a CoreSchedRequest. */
static size_t
handle_CoreSchedRequest(
	struct zmq_service *service __attribute__((unused)),
	uint8_t const *request_buffer, size_t cb_request, uint8_t *response_buffer,
	size_t cb_response_buffer)
{
	Virtioforwarder__CoreSchedResponse response;
	virtioforwarder__core_sched_response__init(&response);
	response.status = VIRTIOFORWARDER__CORE_SCHED_RESPONSE__STATUS__ERROR;

	Virtioforwarder__CoreSchedRequest *pc =
	virtioforwarder__core_sched_request__unpack(
		NULL,
		cb_request,
		request_buffer
	);

	if (!pc) {
		goto pack_response;
	}

	bool valid = validate_CoreSchedRequest(pc);
	if (valid) {
		char pc_str[Virtioforwarder__CoreSchedRequest__CCH_MAX];
		log_info("CoreSchedRequest: %s",
			Virtioforwarder__CoreSchedRequest__to_str(pc, pc_str, sizeof pc_str)
		);

		switch (pc->op)
		{
		/* Trigger relay cpu migration. */
		case VIRTIOFORWARDER__CORE_SCHED_REQUEST__OP__UPDATE:
			if (pc->n_relay_cpu_map) {
				log_info("Updating %zu relay CPU affinities.", pc->n_relay_cpu_map);
				for (size_t i=0; i<pc->n_relay_cpu_map; ++i) {
					Virtioforwarder__CoreSchedRequest__RelayCPU *cpu = pc->relay_cpu_map[i];
					log_info("Setting relay %d's virtio2vf_cpu=%d and vf2virtio_cpu=%d",
							 cpu->relay_number, cpu->virtio2vf_cpu, cpu->vf2virtio_cpu);
					handle_CoreSchedRequest_set_error_code(
						&response, "migrate_relay_cpus()",
						migrate_relay_cpus(cpu->relay_number, cpu->virtio2vf_cpu, cpu->vf2virtio_cpu)
					);
				}
			}
			break;

		case VIRTIOFORWARDER__CORE_SCHED_REQUEST__OP__GET_EAL_CORES:
			response.n_eal_cores = 0;
			uint64_t eal_bitmap = get_eal_core_map();
			unsigned i = 0;
			while (eal_bitmap) {
				unsigned w = __builtin_ffsll(eal_bitmap)-1;
				assert(w<64);
				eal_bitmap &= (~(1ULL << w));

				response.n_eal_cores += 1;
				eal_cores_buffer[i++] = w;
			}
			if (response.n_eal_cores) {
				response.eal_cores = eal_cores_buffer;
				response.status = VIRTIOFORWARDER__CORE_SCHED_RESPONSE__STATUS__OK;
			}
			else {
				handle_CoreSchedRequest_set_error_code(&response, "worker core request", 1);
			}
			break;

		default:
			log_critical("unhandled CoreSchedRequest.Op %i", pc->op);
			break;
		}
	}
	else {
		log_warning("CoreSchedRequest: invalid field value(s)");
	}

	virtioforwarder__core_sched_request__free_unpacked(
		pc, NULL
	);

pack_response:;
	size_t cb_response =
		virtioforwarder__core_sched_response__get_packed_size(&response);
	assert(cb_response <= cb_response_buffer);
	return virtioforwarder__core_sched_response__pack(
		&response, response_buffer
	);
}

/** Destructor for core scheduler service. */
static void
core_sched_free(struct zmq_service *service)
{
	log_debug("Freeing ZeroMQ core scheduler service: %p", service);
	zmq_service_free(service);
}

/** Constructor for core scheduler service. */
static int
core_sched_setup(
	struct zmq_service *service, void *setup_arg __attribute__((unused)))
{
	service->handle_request = &handle_CoreSchedRequest;
	service->destructor = &core_sched_free;
	service->max_request_cb = 1024;
	service->max_response_cb = 256;
	return 0;
}

struct zmq_service *
zmq_core_sched_service_alloc(void)
{
	struct zmq_service *ans;

	ans = zmq_service_alloc(__MODULE__, &core_sched_setup, NULL);
	if (ans)
		log_debug("Allocated ZeroMQ core scheduler service: %p", ans);
	else
		log_critical("Failed to allocate ZeroMQ core scheduler service");

	return ans;
}
