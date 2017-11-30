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

#include "zmq_service.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define __MODULE__ "zmq_service"
#include "log.h"

struct zmq_service *
zmq_service_alloc(
	char const *name,
	int (*setup)(struct zmq_service *service, void *setup_arg),
	void *setup_arg)
{
	struct zmq_service *ans;
	int err;

	assert(strlen(name) < ZMQ_SERVICE_NAME_CCH_MAX);
	ans = calloc(1, sizeof(struct zmq_service));
	if (!ans)
		return NULL;

	strncpy(ans->name, name, ZMQ_SERVICE_NAME_CCH_MAX);

	if ((err = setup(ans, setup_arg)) < 0) {
		log_critical(
			"%s: setup() failed: %s", ans->name, strerror(-err)
		);
		free(ans);
		return NULL;
	}

	log_debug("%s: Request buffer size: %uB", ans->name, ans->max_request_cb);
	assert(ans->max_request_cb);
	log_debug("%s: Response buffer size: %uB", ans->name, ans->max_response_cb);
	assert(ans->max_response_cb);

	return ans;
}

void
zmq_service_release(struct zmq_service *service)
{
	/* No reference counting yet. */
	if (service->destructor)
		service->destructor(service);
}

void
zmq_service_free(struct zmq_service *service)
{
	free(service);
}
