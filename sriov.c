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

#include "sriov.h"
#define __MODULE__ "sriov"
#include "log.h"

#include <bsd/string.h>
#include <string.h>
#include <stdio.h>

/*
 * Look for string like "bus-info: NFP 0 VF0.40 0000:05:0d.0" in output of
 * ethtool -i
 * TODO: improve robustness, e.g. perhaps read entry from
 * /sys/module/<driver>/control/ instead of ethtool or try to obtain from sysfs.
 */
int get_VF_PCIE_from_netdev(const char *netdev, struct sriov_info *info) {
	char cmd[128];
	char buf[2048];
	int count;
	char *bus;
	char _vf[9];
	char _dbdf[21];
	uint8_t nicbus, vfnum;
	FILE *stdout;

	snprintf(cmd, 128, "/sbin/ethtool -i %s", netdev);
	stdout = popen(cmd, "r");
	if (stdout == NULL) {
		log_error("Could not run '%s'!", cmd);
		return -1;
	}
	count = fread(buf, 1, 2048, stdout);
	pclose(stdout);
	if (count <= 0) {
		log_error("Could not read stdout from '%s'!", cmd);
		return -1;
	}
	bus = strstr(buf, "bus-info: NFP 0 ");
	if (bus == NULL) {
		log_error("Could not find bus-info in cmd '%s' output!", cmd);
		return -1;
	}
	bus += 16;
	if (sscanf(bus, "%8s %20s", _vf, _dbdf) != 2) {
		log_error("Could not extract domain:bus:device.function from cmd '%s'!", cmd);
		return -1;
	}
	strlcpy(info->dbdf, _dbdf, 20);
	log_debug("Got PCI addr '%s' for netdev '%s'", info->dbdf, netdev);
	if (sscanf(_vf, "VF%hhu.%hhu", &nicbus, &vfnum) != 2) {
		log_error("Could not extract VF number from cmd '%s'!", cmd);
		return -1;
	}
	info->vf = vfnum;
	log_debug("Got VF num %hhu for netdev '%s'", info->vf, netdev);

	return 0;
}
