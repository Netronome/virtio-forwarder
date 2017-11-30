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

#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <sys/select.h>
#include <rte_version.h>
#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
#include <rte_vhost.h>
#else
#include <rte_virtio_net.h>
#endif
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <signal.h>
#include <stdarg.h>

#include "stats_dump.h"
#define __MODULE__ "stats_dump"
#include "log.h"
#include "rte_ethdev.h"
#include "virtio_worker.h"

#define STATS_MAX_WRITE 65535

static char stats_header[] = "Virtio-forwarder\n";

/* Creates a pseudo terminal and sleeps until a connection is made. Dumps stats,
then goes to sleep again. */

static int tty_setraw(int fd)
{
	struct termios t;

	if (tcgetattr(fd, &t) < 0)
		return -1;

	cfmakeraw(&t);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;

	if (tcsetattr(fd, TCSANOW, &t) < 0)
		return -1;

	return 0;
}

static int init_ptm(void)
{
	int ptmfd;
	int rc;
	FILE* ptsfile;

	ptmfd = posix_openpt(O_RDWR|O_NOCTTY);
	if (ptmfd < 0) {
		log_error("Error opening pty.");
		return -1;
	}

	rc = grantpt(ptmfd);
	if (rc != 0) {
		log_error("Error granting pty permissions.");
		goto err;
	}

	rc = unlockpt(ptmfd);
	if (rc != 0) {
		log_error("Error unlocking pty.");
		goto err;
	}

	rc = tty_setraw(ptmfd);
	if (rc != 0) {
		log_error("Error setting pty RAW.");
		goto err;
	}

	ptsfile = fopen("/var/run/virtioforwarder.pts", "w");
	if (ptsfile == NULL) {
		log_error("Error publishing pty.");
		goto err;
	}
	fprintf(ptsfile, "%s", ptsname(ptmfd));
	fclose(ptsfile);

	return ptmfd;
err:
	close(ptmfd);
	return rc;
}

static pthread_t stats_dump_thread;
static int running;
static int must_stop;
static void* stats_dump_threadmain(void *arg __attribute__((unused))) {

	int ptmfd;
	int rc;
	fd_set read_fds;
	struct timeval sel_timeout;
	char junk;

	log_debug("Starting stats_dump thread");
	must_stop=0;
	running=1;

	ptmfd = init_ptm();
	while (running && !must_stop) {
		if (ptmfd < 0) {
			sleep(1);
			ptmfd = init_ptm();
			continue;
		}

		FD_ZERO(&read_fds);
		FD_SET(ptmfd, &read_fds);
		sel_timeout.tv_sec=0;
		sel_timeout.tv_usec=100000; //100ms
		rc = select(ptmfd+1, &read_fds, 0, 0, &sel_timeout);
		if (rc == 0)
			continue;
		if (rc < 0) {
			log_error("Error reading pty: %m");
			sleep(1);
			ptmfd = init_ptm();
			continue;
		}
		if (!FD_ISSET(ptmfd, &read_fds)) {
			log_warning("Unexpected return from select with nothing to read!");
			continue;
		}
		rc = read(ptmfd, &junk, 1);
		if (rc > 0) {
			tcflush(ptmfd, TCIFLUSH);
			/* Stats dump starts here. */
			/* CPU intensive tasks may be done here. */
			print_stat(ptmfd, "%s", stats_header);

			virtio_forwarders_print_stats(ptmfd);

			/* End with a NULL character */
			junk = 0;
			rc = write(ptmfd, &junk, 1);
			if (rc < 0) {
				log_warning("Error on write.");
			}
			tcdrain(ptmfd);
		} else {
			/* Detected a disconnect or EOF */
			close(ptmfd);
			ptmfd = init_ptm();
		}
	}
	running=0;
	log_debug("stats_dump thread ending");
	return 0;
}

int stats_dump_start(void) {
	log_debug("Creating stats_dump thread");
	pthread_create(&stats_dump_thread, 0, stats_dump_threadmain, 0);
	return 0;
}

void stats_dump_stop(void) {
	struct timespec ts;
	struct timeval tv;

	must_stop=1;
	gettimeofday(&tv, 0);
	tv.tv_sec+=2;
	ts.tv_sec=tv.tv_sec;
	ts.tv_nsec=tv.tv_usec*1000;
	log_debug("Stopping stats_dump thread");
	if (pthread_timedjoin_np(stats_dump_thread, 0, &ts) != 0) {
		log_debug("Timeout waiting for stats_dump_thread, cancelling thread...");
		pthread_cancel(stats_dump_thread);
	}
	pthread_join(stats_dump_thread, 0);
	log_debug("Stopped stats_dump thread");
}

void print_stat(int ptmfd, const char *format, ...)
{
	va_list args;
	char buf[STATS_MAX_WRITE];
	int rc;

	va_start(args, format);
	vsnprintf(buf, STATS_MAX_WRITE, format, args);
	rc = write(ptmfd, buf, strlen(buf));
	if (rc < 0) {
		log_warning("Error on write.");
	}
	va_end(args);
}
