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

struct zmq_server;
struct zmq_service;

/**
 * Start a ZeroMQ server.
 *
 * @param[in] ep
 *	   ZeroMQ IPC endpoint (e.g., ipc:///var/run/virtio-forwarder/port_control).
 * @param[in] service
 *	   A ZeroMQ service to attach to @a ep. In case of failure, the function
 *	   guarantees a call to zmq_service_release(service), so there is no need
 *	   to release a service once it has been passed into this function.
 *
 * @return
 *	   An object that can be passed to zmq_server_stop() to stop the server, or
 *	   @c NULL on failure.
 */
struct zmq_server *
zmq_server_start(char const *zmq_ep, struct zmq_service *service);

/**
 * Stop a ZeroMQ server and call zmq_service_release() on its attached service.
 *
 * @param[in] server
 *	   An object returned from zmq_server_start().
 *
 * @return
 *	   ZMQ server thread exit status.
 */
int
zmq_server_stop(struct zmq_server *server);
