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

#include "dpdk_eal.h"
#include "argv.h"
#define __MODULE__ "dpdk_eal"
#include "log.h"
#include "rte_errno.h"
#include "rte_mbuf.h"
#include "cpuinfo.h"

#include <getopt.h>
#include <rte_eal.h>
#include <rte_version.h>

int dpdk_eal_initialize(const struct dpdk_conf *conf)
{
	int argc = 0;
	char **argv = NULL;
	int ret;
	char buf[32];
	unsigned i;
	int l;
	cpuinfo_t *cpu = get_cpuinfo();
	uint32_t socket_bitmap = 0;
	uint32_t numa_bitmap = 0;

	if (!cpu)
		return -1;
	if ((conf->core_bitmap & cpu->cpubitmap) != conf->core_bitmap) {
		log_error("Specified CPU bitmap 0x%"PRIx64" contains CPUs outside of currently connected CPUs (0x%"PRIx64")",
			conf->core_bitmap, cpu->cpubitmap);
		return -1;
	}
	if (!((1<<conf->master_lcore) & cpu->cpubitmap)) {
		log_error("Master lcore is not a valid CPU: %u not in (0x%"PRIx64")",
			conf->master_lcore, cpu->cpubitmap);
		return -1;
	}
	if (conf->core_bitmap & (1<<conf->master_lcore)) {
		log_error("Worker CPU bitmap 0x%"PRIx64" may not contain the master lcore %u",
			conf->core_bitmap, conf->master_lcore);
		return -1;
	}

	for (i=0; i<MAX_CPUS; ++i) {
		if ((1ULL<<i) & conf->core_bitmap) {
			socket_bitmap |= (1<<cpu->cpus[i].socket);
			numa_bitmap |= (1<<cpu->cpus[i].numanode);
		}
	}

	add_arg(&argv, &argc, "virtio-forwarder");
	add_arg(&argv, &argc, "--master-lcore");
	snprintf(buf, 32, "%u", conf->master_lcore);
	add_arg(&argv, &argc, buf);
	add_arg(&argv, &argc, "--log-level");
	snprintf(buf, 32, "%u", conf->log_level);
	add_arg(&argv, &argc, buf);
	add_arg(&argv, &argc, "--huge-dir");
	add_arg(&argv, &argc, conf->huge_dir);
	add_arg(&argv, &argc, "--file-prefix");
	add_arg(&argv, &argc, "virtio-forwarder_");
	/* Unlink the hugepages after mapping to ensure they're gone when app exits. */
	add_arg(&argv, &argc, "--huge-unlink");
#if RTE_VERSION < RTE_VERSION_NUM(17,11,0,0)
	/* Prevent scanning all PCI devices at startup, instead only use hotplug API. */
	add_arg(&argv, &argc, "--no-pci");
#else
	/* DPDK's --no-pci behaviour changed in 17.11. With the flag passed, the
	 * EAL never populates its device lists which causes subsequent hotplugs
	 * to fail. When it is not passed, EAL attaches to all uio devices,
	 * which we do not want that either. The 'solution' is to omit the flag
	 * in order for DPDK to discover devices and to also add a bogus
	 * whitelist address such that DPDK will only auto-attach to it.
	 */
	add_arg(&argv, &argc, "-w");
	add_arg(&argv, &argc, "ffff:ff:ff.f");
#endif
	add_arg(&argv, &argc, "-c"); /* Core mask. */
	snprintf(buf, 32, "%llx", (long long)conf->core_bitmap | (1<<conf->master_lcore));
	add_arg(&argv, &argc, buf);
	add_arg(&argv, &argc, "-n"); /* Mem channels. */
	add_arg(&argv, &argc, "4");
	add_arg(&argv, &argc, "--socket-mem"); /* Memory to allocate per NUMA node (MiB). */
	l=0;
	for (i=0; i<cpu->numnodes; i++) {
		if (i >= 1)
			l += snprintf(buf + l, 32 - l, ",");
		if ((1<<i) & numa_bitmap)
			l += snprintf(buf + l, 32 - l,
				(conf->use_jumbo || conf->enable_tso) ?
				"2750" : "768");
		else
			l += snprintf(buf + l, 32 - l, "0");
	}
	add_arg(&argv, &argc, buf);
	optind = 0;

	log_info("DPDK version: %s", rte_version());
	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		return ret;
	}
	log_debug("DPDK RTE_MAX_ETHPORTS=%u", RTE_MAX_ETHPORTS);
	if (RTE_MAX_ETHPORTS < 60) {
		log_warning("Maximum relays is %u, increase RTE_MAX_ETHPORTS in DPDK if more are required",
			RTE_MAX_ETHPORTS);
	}

	return 0;
}

void dpdk_eal_finalize(void)
{}
