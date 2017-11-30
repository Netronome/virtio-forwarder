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

#include <stddef.h>
#include <stdint.h>

/** Maximum length, including NUL terminator, of a ZeroMQ service name. */
#define ZMQ_SERVICE_NAME_CCH_MAX 32

/** A service exposed by a ZeroMQ server. */
struct zmq_service
{
	char name[ZMQ_SERVICE_NAME_CCH_MAX];

	/**
	 * Decode request from @a *request_buffer. Act on valid requests.
	 * Construct response and serialize into @a *response_buffer.
	 *
	 * @param[in] service
	 *	   ZeroMQ service object.
	 * @param[in] request_buffer
	 *	   A buffer containing encoded request data.
	 * @param[in] cb_request
	 *	   Size in bytes of encoded request data.
	 * @param[in] response_buffer
	 *	   Receives encoded response data.
	 * @param[in]
	 *	   Number of bytes available in @a *response_buffer.
	 *
	 * @return
	 *	   Size in bytes of encoded response data.
	 */
	size_t (*handle_request)(
		struct zmq_service *service,
		uint8_t const *request_buffer, size_t cb_request,
		uint8_t *response_buffer, size_t cb_response_buffer
	);

	/**
	 * Maximum size of a request, in bytes.
	 * @remarks
	 *	   This is limited to uint16_t because currently, the request buffer
	 *	   is allocated on the stack.
	 */
	uint16_t max_request_cb;

	/**
	 * Maximum size of a response, in bytes.
	 * @remarks
	 *	   This is limited to uint16_t because currently, the response buffer
	 *	   is allocated on the stack.
	 */
	uint16_t max_response_cb;

	/** Service-specific data. */
	void *priv;

	/** Tears down the service. */
	void (*destructor)(struct zmq_service *service);
};

/**
 * Creates a zmq_service.
 *
 * @param[in] name
 *	   Service name.
 * @param[in] setup
 *	   Service constructor. Returns a negative error code on failure.
 * @param[in] setup_arg
 *	   Opaque data passed to @a setup.
 *
 * @return
 *	   The zmq_service, or @c NULL on failure.
 */
struct zmq_service *
zmq_service_alloc(
	char const *name,
	int (*setup)(struct zmq_service *service, void *setup_arg),
	void *setup_arg
);

/**
 * Releases a zmq_service.
 *
 * When the last reference to the service drops, this will call the service
 * destructor, which should invoke zmq_service_free().
 */
void
zmq_service_release(struct zmq_service *service);

/**
 * Deallocates a zmq_service.
 *
 * Outside of service destructors, this should be called via
 * zmq_service_release().
 */
void
zmq_service_free(struct zmq_service *service);
