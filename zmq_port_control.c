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

#include "zmq_port_control.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "virtioforwarder.pb-c.h"

#define __MODULE__ "zmq_port_control"
#include "log.h"
#include "sriov.h"
#include "virtio_worker.h"
#include "zmq_service.h"

/** Converts PortControlRequest.Op to string. */
static char const *
Virtioforwarder__PortControlRequest__Op__to_str(
	Virtioforwarder__PortControlRequest__Op op, char *ans, size_t len)
{
	switch (op)
	{
	case VIRTIOFORWARDER__PORT_CONTROL_REQUEST__OP__ADD:
		return "ADD";

	case VIRTIOFORWARDER__PORT_CONTROL_REQUEST__OP__REMOVE:
		return "REMOVE";

	default:
		snprintf(ans, len, "Virtioforwarder__PortControlRequest__Op(%i)", op);
		return ans;
	}
}
#define Virtioforwarder__PortControlRequest__Op_CCH_MAX 64

/** Converts PortControlRequest.PciAddress to string. */
static char const *
Virtioforwarder__PortControlRequest__PciAddress__to_str(
	Virtioforwarder__PortControlRequest__PciAddress const *addr, char *ans,
	size_t len)
{
	snprintf(ans, len, "%04X:%02X:%02X.%X", addr->domain, addr->bus,
		addr->slot, addr->function);
	return ans;
}
#define Virtioforwarder__PortControlRequest__PciAddress_CCH_MAX 16

/** Converts PortControlRequest to string. */
static char const *
Virtioforwarder__PortControlRequest__to_str(
	Virtioforwarder__PortControlRequest const *pc, char *ans, size_t len)
{
	char op_str[Virtioforwarder__PortControlRequest__Op_CCH_MAX];
	char pci_addr_str[Virtioforwarder__PortControlRequest__PciAddress_CCH_MAX];

	snprintf(ans, len, "[op=%s, pci_addr=%s, vf=%u]",
		Virtioforwarder__PortControlRequest__Op__to_str(
			pc->op, op_str, sizeof op_str
		),
		Virtioforwarder__PortControlRequest__PciAddress__to_str(
			pc->pci_addr, pci_addr_str, sizeof pci_addr_str
		),
		pc->vf
	);

	return ans;
}
#define Virtioforwarder__PortControlRequest__CCH_MAX 128

/** Range checks PortControlRequest fields. */
static bool
validate_PortControlRequest(Virtioforwarder__PortControlRequest const *pc)
{
	/* Assuming that these are all unsigned. */
	return pc->pci_addr->domain   <= 0xFFFF
		&& pc->pci_addr->bus	  <= 0xFF
		&& pc->pci_addr->slot	  <= 0xFF
		&& pc->pci_addr->function <= 0xF
		&& pc->vf <= 0x7F
	;
}

/** Sets detailed response status. */
static void
handle_PortControlRequest_set_error_code(
	Virtioforwarder__PortControlResponse *response, char const *error_code_source,
	int32_t error_code)
{
	if (!error_code) {
		response->status = VIRTIOFORWARDER__PORT_CONTROL_RESPONSE__STATUS__OK;
	} else {
		assert(
			response->status ==
			VIRTIOFORWARDER__PORT_CONTROL_RESPONSE__STATUS__ERROR
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

/** Handles a PortControlRequest. */
static size_t
handle_PortControlRequest(
	struct zmq_service *service __attribute__((unused)),
	uint8_t const *request_buffer, size_t cb_request, uint8_t *response_buffer,
	size_t cb_response_buffer)
{
	Virtioforwarder__PortControlResponse response;
	virtioforwarder__port_control_response__init(&response);
	response.status = VIRTIOFORWARDER__PORT_CONTROL_RESPONSE__STATUS__ERROR;

	Virtioforwarder__PortControlRequest *pc =
	virtioforwarder__port_control_request__unpack(
		NULL,
		cb_request,
		request_buffer
	);

	if (!pc) {
		goto pack_response;
	}

	bool valid = validate_PortControlRequest(pc);
	if (valid) {
		char pc_str[Virtioforwarder__PortControlRequest__CCH_MAX];
		log_info("PortControlRequest: %s",
			Virtioforwarder__PortControlRequest__to_str(pc, pc_str, sizeof pc_str)
		);

		struct sriov_info vfinfo;
		Virtioforwarder__PortControlRequest__PciAddress__to_str(
			pc->pci_addr, vfinfo.dbdf, sizeof vfinfo.dbdf
		);
		vfinfo.vf = pc->vf;

		bool conditional;
		if (pc->has_conditional) {
			conditional = pc->conditional;
		} else {
			conditional = true;
		}

		switch (pc->op)
		{
		case VIRTIOFORWARDER__PORT_CONTROL_REQUEST__OP__ADD:
			handle_PortControlRequest_set_error_code(
				&response, "virtio_forwarder_add_vf2()",
				virtio_forwarder_add_vf2(vfinfo.dbdf, vfinfo.vf, conditional)
			);
			break;

		case VIRTIOFORWARDER__PORT_CONTROL_REQUEST__OP__REMOVE:
			handle_PortControlRequest_set_error_code(
				&response, "virtio_forwarder_remove_vf2()",
				virtio_forwarder_remove_vf2(vfinfo.dbdf, vfinfo.vf, conditional)
			);
			break;

		default:
			log_critical("unhandled PortControlRequest.Op %i", pc->op);
			break;
		}
	}
	else {
		log_warning("PortControlRequest: invalid field value(s)");
	}

	virtioforwarder__port_control_request__free_unpacked(
		pc, NULL
	);

pack_response:;
	size_t cb_response =
		virtioforwarder__port_control_response__get_packed_size(&response);
	assert(cb_response <= cb_response_buffer);
	return virtioforwarder__port_control_response__pack(
		&response, response_buffer
	);
}

/** Destructor for port control service. */
static void
port_control_free(struct zmq_service *service)
{
	log_debug("Freeing ZeroMQ port control service: %p", service);
	zmq_service_free(service);
}

/** Constructor for port control service. */
static int
port_control_setup(
	struct zmq_service *service, void *setup_arg __attribute__((unused)))
{
	service->handle_request = &handle_PortControlRequest;
	service->destructor = &port_control_free;
	service->max_request_cb = 64;
	service->max_response_cb = 256;
	return 0;
}

struct zmq_service *
zmq_port_control_service_alloc(void)
{
	struct zmq_service *ans;

	ans = zmq_service_alloc(__MODULE__, &port_control_setup, NULL);
	if (ans)
		log_debug("Allocated ZeroMQ port control service: %p", ans);
	else
		log_critical("Failed to allocate ZeroMQ port control service");

	return ans;
}
