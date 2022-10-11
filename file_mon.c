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

#include <stdio.h>
#include <rte_version.h>
#if RTE_VERSION_NUM(17, 5, 0, 0) <= RTE_VERSION
#include <rte_vhost.h>
#else
#include <rte_virtio_net.h>
#endif
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "file_mon.h"
#define __MODULE__ "file_mon"
#include "log.h"
#include "rte_ethdev.h"
#include "virtio_worker.h"
#include "sriov.h"

#define MATCH_TOKEN(test,keyword) (strncmp(test, keyword, sizeof(*keyword))==0)

static pthread_t file_mon_thread;
static int running;
static int must_stop;

static void scan_file(FILE *f)
{
	char buf[128];
	char netdev[17];
	char *args;

	memset(buf, 0, 128);
	if (!fread(buf, 1, 128, f))
		goto close_file;

	if (MATCH_TOKEN(buf, "add_port ")) {
		args = buf + 8;
		log_debug("Got cmd to add port with args '%s'", args);
		if (sscanf(args, "%16s", netdev) == 1) {
			struct sriov_info vfinfo;
			if (get_VF_PCIE_from_netdev(netdev, &vfinfo) == 0) {
				log_info("Adding netdev '%s' to virtio %u",
					netdev, (unsigned)vfinfo.vf);
				#ifndef VIRTIO_ECHO
				virtio_forwarder_add_vf(vfinfo.dbdf,
						vfinfo.vf /*virtio_id*/);
				#endif
			}
		}
	} else if (MATCH_TOKEN(buf, "del_port ")) {
		args = buf + 8;
		log_debug("Got cmd to remove port with args '%s'", args);
		if (sscanf(args, "%16s", netdev) == 1) {
			struct sriov_info vfinfo;
			if (get_VF_PCIE_from_netdev(netdev, &vfinfo) == 0) {
				log_info("removing netdev '%s' from virtio %u",
					netdev, (unsigned)vfinfo.vf);
				#ifndef VIRTIO_ECHO
				virtio_forwarder_remove_vf(vfinfo.dbdf,
						vfinfo.vf /*virtio_id*/);
				#endif
			}
		}
	}

close_file:
	fclose(f);
}

static void* file_mon_threadmain(void *arg __attribute__((unused)))
{
	log_debug("Starting file_mon thread");
	must_stop = 0;
	running = 1;
	while (running && !must_stop) {
		struct stat statbuf;

		/* Quick'n'dirty "IPC via cmds in text file"). */
		usleep(1000);
		if (stat("/tmp/vio_cmd", &statbuf) == 0) {
			FILE *f = fopen("/tmp/vio_cmd", "r");
			if (f)
				scan_file(f);
			unlink("/tmp/vio_cmd");
		}
	}
	running = 0;
	log_debug("file_mon thread ending");

	return 0;
}

int file_mon_start(void)
{
	log_debug("Creating file_mon thread");
	pthread_create(&file_mon_thread, 0, file_mon_threadmain, 0);

	return 0;
}

void file_mon_stop(void)
{
	struct timespec ts;
	struct timeval tv;

	must_stop=1;
	gettimeofday(&tv, 0);
	tv.tv_sec+=2;
	ts.tv_sec=tv.tv_sec;
	ts.tv_nsec=tv.tv_usec*1000;
	log_debug("Stopping file_mon thread");
	if (pthread_timedjoin_np(file_mon_thread, 0, &ts) != 0) {
		log_debug("Timeout waiting for file_mon_thread, cancelling thread...");
		pthread_cancel(file_mon_thread);
	}
	pthread_join(file_mon_thread, 0);
	log_debug("Stopped file_mon thread");
}
