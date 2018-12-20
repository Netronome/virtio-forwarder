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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for pipe2() */
#endif

#include "zmq_server.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zmq.h>
#include <poll.h>

#define __MODULE__ "zmq_server"
#include "log.h"
#include "zmq_service.h"

static int const linger_ms	 = 1000;	 /**< ZMQ_LINGER time (ms). */
static int const rcvtimeo_ms = 1000;	 /**< ZMQ receive timeout (ms). */
static int const sndtimeo_ms = 1000;	 /**< ZMQ send timeout (ms). */
static int const poll_ms = 1000;		 /**< Shutdown check interval (ms). */

/** Error reporting helper. */
static char const *
errno_str_en(char *buffer, size_t len, int en)
{
	snprintf(buffer, len, "%s (%i)", zmq_strerror(en), en);
	return buffer;
}

static inline char const *
errno_str(char *buffer, size_t len)
{
	return errno_str_en(buffer, len, zmq_errno());
}

/** Buffer size for use with errno_str()/errno_str_en(). */
#define ERRNO_BUFFER_SIZE 256

/**
 * Set @a *fd to -1, then close it if it was not previously -1.
 * Return false on error.
 *
 * @param[in] fd_description
 *	   Human-readable name of @a *fd (for error messages).
 */
static bool
close_fd(int *fd, char const *fd_description)
{
	int f = *fd;
	*fd = -1;

	if (f == -1) {
		return true;
	}

	if (close(f)) {
		char err[ERRNO_BUFFER_SIZE];
		log_error("close(%s): %s", fd_description, errno_str(err, sizeof err));
		return false;
	}

	return true;
}

/**
 * Set @a *zmq_s to NULL, then close it if it was not previously NULL.
 * Return false on error.
 */
static bool
close_zmq_socket(void **zmq_s)
{
	void *s = *zmq_s;
	*zmq_s = NULL;

	if (!s) {
		return true;
	}

	if (zmq_close(s) == -1) {
		char err[ERRNO_BUFFER_SIZE];
		log_error("zmq_close(): %s", errno_str(err, sizeof err));
		return false;
	}

	return true;
}

/**
 * Set @a *zmq_ctx to NULL, then destroy it if it was not previously NULL.
 * Return false on error.
 */
static bool
destroy_zmq_ctx(void **zmq_ctx)
{
	void *ctx = *zmq_ctx;
	*zmq_ctx = NULL;

	if (!ctx) {
		return true;
	}

	while (zmq_ctx_destroy(ctx) == -1) {
		char err[ERRNO_BUFFER_SIZE];
		if (zmq_errno() == EINTR) {
			log_warning("zmq_ctx_destroy(): EINTR");
			continue;
		}
		log_error("zmq_ctx_destroy(): %s", errno_str(err, sizeof err));
		return false;
	}

	return true;
}

/** Sends an event via pipe. */
static bool
send_event(
	int fd, char const *fd_description, char const *service_name)
{
	static uint8_t const byte = 0;
	for (;;) {
		ssize_t n = write(fd, &byte, sizeof byte);
		if (n < 0) {
			char err[ERRNO_BUFFER_SIZE];
			if (errno == EINTR) {
				log_warning("write(%s): EINTR", fd_description);
				continue;
			}
			else if (errno == EPIPE) {
				/* Thread shut down before we sent it the event (e.g., due to
				 * ZMQ error).
				 */
				log_warning(
					"write(%s): EPIPE: %s thread had already shut down",
					fd_description, service_name
				);
			}
			else {
				/* The write end of the quit pipe is technically in
				 * non-blocking mode, but if we get EAGAIN or EWOULDBLOCK
				 * something is seriously wrong (since there should be
				 * little to no data in the pipe at any time).
				 */
				log_critical(
					"write(%s): %s", fd_description, errno_str(err, sizeof err)
				);
				return false;
			}
		}

		/* Successfully sent the event. */
		assert(n != 0);
		return true;
	}
}

/** Receives an event from a pipe. */
static bool
receive_event(int fd, char const *fd_description, ssize_t *n_read)
{
	uint8_t byte;
	*n_read = read(fd, &byte, sizeof byte);
	if (*n_read >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
		return true;
	}

	char err[ERRNO_BUFFER_SIZE];
	log_error("read(%s): %s", fd_description, errno_str(err, sizeof err));
	return false;
}

/** Initializes ZMQ and starts listening on socket. */
static bool
init_zmq(
	char const *zmq_ep, struct zmq_service *service, void **zmq_ctx,
	void **zmq_s)
{
	*zmq_ctx = *zmq_s = NULL;

	*zmq_ctx = zmq_ctx_new();
	if (!*zmq_ctx) {
		log_error("zmq_ctx_new(): unspecified error");
		return false;
	}

	*zmq_s = zmq_socket(*zmq_ctx, ZMQ_REP);
	if (!*zmq_s) {
		char err[ERRNO_BUFFER_SIZE];
		log_error("zmq_socket(): %s", errno_str(err, sizeof err));
		return false;
	}

	int64_t maxmsgsize = service->max_request_cb;
	if (maxmsgsize < service->max_response_cb) {
		maxmsgsize = service->max_response_cb;
	}

	const struct {
		int name;
		char const *name_str;
		void const *value;
		size_t len;
	} opts[] = {
		{ ZMQ_LINGER,	  "ZMQ_LINGER",		&linger_ms,   sizeof linger_ms	 },
		{ ZMQ_SNDTIMEO,   "ZMQ_SNDTIMEO",	&sndtimeo_ms, sizeof sndtimeo_ms },
		{ ZMQ_RCVTIMEO,   "ZMQ_RCVTIMEO",	&rcvtimeo_ms, sizeof rcvtimeo_ms },
		{ ZMQ_MAXMSGSIZE, "ZMQ_MAXMSGSIZE", &maxmsgsize,  sizeof maxmsgsize  },
	};

	for (size_t i = 0; i < sizeof opts / sizeof *opts; ++i)
	{
		if (zmq_setsockopt(
			*zmq_s, opts[i].name, opts[i].value, opts[i].len) == -1)
		{
			char err[ERRNO_BUFFER_SIZE];
			log_error(
				"zmq_setsockopt(%s): %s", opts[i].name_str,
				errno_str(err, sizeof err)
			);
			return false;
		}
	}

	if (zmq_bind(*zmq_s, zmq_ep) == -1) {
		char err[ERRNO_BUFFER_SIZE];
		log_error("zmq_bind(): %s: %s", zmq_ep, errno_str(err, sizeof err));
		return false;
	}

	return true;
}

/** Server thread main loop. */
static intptr_t
zmq_thread_loop(
	char const *zmq_ep, struct zmq_service *service, int send_ready_fd,
	int quit_fd)
{
	void *zmq_ctx = NULL, *zmq_s = NULL;

	if (!init_zmq(zmq_ep, service, &zmq_ctx, &zmq_s)) {
		goto error_exit;
	}

	/* Send ready event to main thread. */
	if (!send_event(send_ready_fd, "send_ready_fd", "main")) {
		goto error_exit;
	}

	for (;;) {
		/* Check if we are supposed to quit. */
		ssize_t n_read;
		if (!receive_event(quit_fd, "quit_fd", &n_read)) {
			goto error_exit;
		}
		else if (n_read >= 0) {
			/* 0 is end-of-file, which still means that we should quit. */
			break;
		}

		/* Wait a bit for a ZMQ message (so we can loop around and check the
		 * quit flag again if there is none).
		 */
		zmq_pollitem_t pollitem = {
			.socket = zmq_s,
			.fd		= -1,
			.events = ZMQ_POLLIN,
		};
		if (zmq_poll(&pollitem, 1, poll_ms) == -1) {
			char err[ERRNO_BUFFER_SIZE];
			if (errno == EINTR) {
				log_warning("zmq_poll(): EINTR");
				continue;
			}
			log_error("zmq_recv(): %s", errno_str(err, sizeof err));
			goto error_exit;
		}

		if (!pollitem.revents) {
			/* No message. */
			continue;
		}

		/* Receive a request. */
		size_t const cb_request_buffer = service->max_request_cb;
		uint8_t request_buffer[cb_request_buffer];
		memset(request_buffer, 0, cb_request_buffer);

		int cb_request = zmq_recv(zmq_s, request_buffer, cb_request_buffer, 0);
		if (cb_request == -1) {
			char err[ERRNO_BUFFER_SIZE];
			if (errno == EINTR) {
				log_warning("zmq_recv(): EINTR");
				continue;
			}
			log_error("zmq_recv(): %s", errno_str(err, sizeof err));
			goto error_exit;
		} else {
			/* Negative return values other than -1 are not documented. */
			assert(cb_request >= 0);
		}

		/* ZMQ_MAXMSGSIZE socket option is supposed to prevent message
		 * truncation with fixed sized buffer (by disconnecting clients that
		 * send more than the permitted amount of data).
		 */
		assert((size_t) cb_request <= cb_request_buffer);

		/* Handle the request. */
		size_t const cb_response_buffer = service->max_response_cb;
		uint8_t response_buffer[cb_response_buffer];
		memset(response_buffer, 0, cb_response_buffer);
		size_t cb_response = service->handle_request(
			service,
			request_buffer, (size_t) cb_request, response_buffer,
			cb_response_buffer
		);

		/* Send the response. */
		for (;;) {
			int cb_sent = zmq_send(zmq_s, response_buffer, cb_response, 0);
			if (cb_sent == -1) {
				char err[ERRNO_BUFFER_SIZE];
				if (errno == EINTR) {
					log_warning("zmq_send(): EINTR");
					continue;
				}
				log_error("zmq_send(): %s", errno_str(err, sizeof err));
				goto error_exit;
			}
			break;
		}
	}

	/* Got quit event. */
	if (!close_zmq_socket(&zmq_s)) {
		goto error_exit;
	}
	if (!destroy_zmq_ctx(&zmq_ctx)) {
		goto error_exit;
	}
	if (!close_fd(&quit_fd, "quit_fd")) {
		goto error_exit;
	}
	if (!close_fd(&send_ready_fd, "send_ready_fd")) {
		goto error_exit;
	}

	return 0;

error_exit:
	close_zmq_socket(&zmq_s);
	destroy_zmq_ctx(&zmq_ctx);
	close_fd(&quit_fd, "quit_fd");
	close_fd(&send_ready_fd, "send_ready_fd");

	return 1;
}

/** Context information for server thread. */
struct zmq_thread_context {
	/** IPC endpoint. */
	char const *zmq_ep;

	/** Service running on @a zmq_ep. */
	struct zmq_service *service;

	/** Filehandle used to send thread ready event. */
	int send_ready_fd;

	/** Filehandle used to receive quit event. */
	int quit_fd;
};

static void *
zmq_thread(void *ptr)
{
	struct zmq_thread_context *thread_context = ptr;

	log_debug(
		"Starting %s thread (endpoint: %s)",
		thread_context->service->name,
		thread_context->zmq_ep
	);

	intptr_t rc = zmq_thread_loop(
		thread_context->zmq_ep, thread_context->service,
		thread_context->send_ready_fd, thread_context->quit_fd
	);

	log_debug(
		"Graceful %s thread shutdown, exit status: %"PRIiPTR,
		thread_context->service->name, rc
	);

	return (void *) rc;
}

/** Context information required to shut down a zmq_server once it starts. */
struct zmq_server {
	pthread_t thr;	  /**< Server thread. */
	int ready_fd;	  /**< Filehandle used to receive thread ready event. */
	int send_quit_fd; /**< Filehandle used to send quit event. */

	struct zmq_thread_context thread_context;
};

/**
 * Waits for an event from a pipe. Signals a timeout if the event takes too
 * long to arrive.
 */
static bool
wait_for_thread_ready(
	int fd, char const *fd_description, char const *service_name)
{
	static const time_t READY_TIMEOUT_MSEC = 2000;
	struct pollfd fds;

	fds.fd = fd;
	fds.events = POLLIN;
	for (;;) {
		int rc = poll(&fds, 1, READY_TIMEOUT_MSEC);
		if (rc == -1) {
			char err[ERRNO_BUFFER_SIZE];
			if (errno == EINTR) {
				log_warning("poll(%s): EINTR", fd_description);
				continue;
			}
			log_critical(
				"poll(%s): %s", fd_description, errno_str(err, sizeof err)
			);
			return false;
		}
		else if (!rc) {
			goto error_exit;
		}
		else {
			break;
		}
	}

	ssize_t n_read;
	if (!receive_event(fd, fd_description, &n_read)) {
		goto error_exit;
	}
	else if (n_read == 0) {
		/* n_read == 0 means that the thread closed the write end of the pipe,
		 * which counts as a "not ready" event.
		 */
		log_error("%s thread shut down abruptly", service_name);
		return false;
	}

	log_debug("%s thread is ready", service_name);
	return true;

error_exit:
	log_error(
		"%s thread failed to mark itself ready within %i ms",
		service_name, (int) READY_TIMEOUT_MSEC
	);
	return false;
}

struct zmq_server *
zmq_server_start(char const *zmq_ep, struct zmq_service *service)
{
	/* Pipes for ready and quit events. */
	int pipes[4] = {-1, -1, -1, -1};

	log_debug("Creating %s thread", service->name);

	struct zmq_server *server = calloc(1, sizeof(struct zmq_server));
	if (!server) {
		char err[ERRNO_BUFFER_SIZE];
		log_critical("calloc(): %s", errno_str(err, sizeof err));
		goto error_exit;
	}
	server->ready_fd = server->send_quit_fd = -1;

	/* Setup ready and quit events. */
	if (pipe2(pipes + 0, O_NONBLOCK) || pipe2(pipes + 2, O_NONBLOCK)) {
		char err[ERRNO_BUFFER_SIZE];
		log_critical("pipe2(): %s", errno_str(err, sizeof err));
		goto error_exit;
	}
	server->ready_fd = pipes[0];
	server->send_quit_fd = pipes[3];
	pipes[0] = pipes[3] = -1;

	/* Start server thread. */
	server->thread_context.send_ready_fd = pipes[1];
	server->thread_context.quit_fd = pipes[2];
	server->thread_context.zmq_ep = zmq_ep;
	server->thread_context.service = service;
	int en = pthread_create(
		&server->thr, NULL, &zmq_thread, &server->thread_context
	);
	if (en) {
		char err[ERRNO_BUFFER_SIZE];
		log_critical("pthread_create(): %s", errno_str_en(err, sizeof err, en));
		goto error_exit;
	}
	/* Once the thread has started, it is responsible for closing these. */
	pipes[1] = pipes[2] = -1;

	/* Wait for ready event from thread. This is useful to catch blatant
	 * startup errors, e.g., invalid @a *zmq_ep.
	 */
	if (!wait_for_thread_ready(server->ready_fd, "ready_fd", service->name)) {
		goto error_exit;
	}

	return server;

error_exit:
	if (server) {
		close_fd(&server->ready_fd, "ready_fd");
		close_fd(&server->send_quit_fd, "send_quit_fd");
		free(server);
		server = NULL;
	}

	close_fd(&pipes[0], "ready_fd");
	close_fd(&pipes[1], "send_ready_fd");
	close_fd(&pipes[2], "quit_fd");
	close_fd(&pipes[3], "send_quit_fd");

	log_critical("Failed to create %s thread", service->name);
	zmq_service_release(service);

	return NULL;
}

int
zmq_server_stop(struct zmq_server *server)
{
	char service_name[ZMQ_SERVICE_NAME_CCH_MAX];
	strncpy(
		service_name, server->thread_context.service->name,
		ZMQ_SERVICE_NAME_CCH_MAX
	);

	log_debug("Stopping %s thread", service_name);

	/* Send quit event to server thread. */
	if (!send_event(server->send_quit_fd, "send_quit_fd", service_name)) {
		goto error_exit;
	}

	/* TBD: file_mon.c has a timed join and pthread_cancel() here. */
	void *retval = NULL;
	int en = pthread_join(server->thr, &retval);
	if (en) {
		char err[ERRNO_BUFFER_SIZE];
		log_error("pthread_join(): %s", errno_str_en(err, sizeof err, en));
		goto error_exit;
	}

	if (!close_fd(&server->ready_fd, "ready_fd")) {
		goto error_exit;
	}
	if (!close_fd(&server->send_quit_fd, "send_quit_fd")) {
		goto error_exit;
	}

	zmq_service_release(server->thread_context.service);
	free(server);
	server = NULL;

	log_debug("Stopped %s thread", service_name);
	return (intptr_t) retval;

error_exit:
	close_fd(&server->ready_fd, "ready_fd");
	close_fd(&server->send_quit_fd, "send_quit_fd");

	zmq_service_release(server->thread_context.service);
	free(server);
	server = NULL;

	log_warning(
		"Stopped %s thread (with one or more cleanup(s) reporting an error)",
		service_name
	);
	return 1;
}
