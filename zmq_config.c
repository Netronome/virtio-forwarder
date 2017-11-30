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

#include "zmq_config.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtioforwarder.pb-c.h"

#define __MODULE__ "zmq_config"
#include "log.h"
#include "virtio_vhostuser.h"
#include "zmq_service.h"

/**
 * In the future, the memory used to construct config responses could be
 * dynamically allocated. For the initial implementation, pre-allocating the
 * maximum required space, on the stack, on each request seems reasonable.
 */
struct config_response_buffer
{
	Virtioforwarder__ConfigResponse__RelayCPU relay_cpu_map[MAX_RELAYS];

	/* Protocol Buffers wants an array of pointers, not an array of objects.*/
	Virtioforwarder__ConfigResponse__RelayCPU *relay_cpu_ptrs[MAX_RELAYS];
};

/** Service-specific data for config service. */
struct config_service_priv
{
	struct virtio_vhostuser_conf const *conf;
};

/** Perform query for relay_cpu_map. */
static size_t
relay_cpus_query(
	struct virtio_vhostuser_conf const *conf, struct config_response_buffer *b)
{
	size_t j = 0;

	for (size_t i = 0; i < MAX_RELAYS; ++i) {
		Virtioforwarder__ConfigResponse__RelayCPU *cpu = b->relay_cpu_map + i;
		virtioforwarder__config_response__relay_cpu__init(cpu);

		cpu->relay_number = i;
		cpu->vf2virtio_cpu = conf->relay_cpus[i].vf2vio_cpu;
		cpu->virtio2vf_cpu = conf->relay_cpus[i].vio2vf_cpu;

		if (cpu->vf2virtio_cpu != -1 && cpu->virtio2vf_cpu != -1)
			b->relay_cpu_ptrs[j++] = cpu;
	}

	return j;
}

/** Handles a ConfigRequest. */
static size_t
handle_ConfigRequest(
	struct zmq_service *service,
	uint8_t const *request_buffer, size_t cb_request, uint8_t *response_buffer,
	size_t cb_response_buffer)
{
	struct config_service_priv *priv = service->priv;

	Virtioforwarder__ConfigResponse response;
	virtioforwarder__config_response__init(&response);

	Virtioforwarder__ConfigRequest *pc =
	virtioforwarder__config_request__unpack(
		NULL,
		cb_request,
		request_buffer
	);

	if (!pc) {
		response.status = VIRTIOFORWARDER__CONFIG_RESPONSE__STATUS__EBADR;
		goto pack_response;
	}

	/* Construct a response consumable by protoc-c generated code. */
	struct config_response_buffer b;
	memset(&b, 0, sizeof(b));

	response.relay_cpu_map = b.relay_cpu_ptrs;
	response.n_relay_cpu_map = relay_cpus_query(priv->conf, &b);

pack_response:;
	if (pc) {
		virtioforwarder__config_request__free_unpacked(
			pc, NULL
		);
	}

	size_t cb_response =
		virtioforwarder__config_response__get_packed_size(&response);
	assert(cb_response <= cb_response_buffer);
	return virtioforwarder__config_response__pack(
		&response, response_buffer
	);
}

/** Destructor for config service. */
static void
config_free(struct zmq_service *service)
{
	log_debug("Freeing ZeroMQ config service: %p", service);

	if (service->priv) {
		free(service->priv);
		service->priv = NULL;
	}

	zmq_service_free(service);
}

/** Constructor for config service. */
static int
config_setup(struct zmq_service *service, void *setup_arg)
{
	service->handle_request = &handle_ConfigRequest;
	service->destructor = &config_free;
	service->max_request_cb = 512;
	service->max_response_cb = 4096;

	service->priv = calloc(1, sizeof(struct config_service_priv));
	if (!service->priv) {
		return -ENOMEM;
	}
	memcpy(service->priv, setup_arg, sizeof(struct config_service_priv));

	return 0;
}

struct zmq_service *
zmq_config_service_alloc(struct virtio_vhostuser_conf const *conf)
{
	struct zmq_service *ans;
	struct config_service_priv priv = {
		.conf = conf,
	};

	ans = zmq_service_alloc(__MODULE__, &config_setup, &priv);
	if (ans) {
		log_debug("Allocated ZeroMQ config service: %p", ans);
		log_debug(
			"Overhead per request: %zuB", sizeof(struct config_response_buffer)
		);
	}
	else {
		log_critical("Failed to allocate ZeroMQ config service");
	}

	return ans;
}
