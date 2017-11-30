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

#include "cpuinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <glob.h>

#define STRMATCH(test,strptr) (strncmp(test, strptr, strlen(test))==0)

static cpuinfo_t cpuinfo;
static int parsed=0;

static int parse_cpuinfo(void)
{
	FILE *cpuinfofile=fopen("/proc/cpuinfo","r");
	char *lineptr=NULL;
	size_t n=0;
	int curprocessor=-1;

	memset(&cpuinfo, 0, sizeof(cpuinfo));
	while (getline(&lineptr, &n, cpuinfofile)>0) {
		if (STRMATCH("processor", lineptr)) {
			int val = atoi(strchr(lineptr + 8, ':') + 1);
			if (val >= MAX_CPUS)
				break;
			cpuinfo.cpubitmap |= (1ULL<<val);
			cpuinfo.cpus[val].cpunum = val;
			cpuinfo.cpus[val].alive = 1;
			curprocessor = val;
			++cpuinfo.numcpus;
		} else if (STRMATCH("physical id", lineptr)) {
			int val=atoi(strchr(lineptr + 8, ':') + 1);
			cpuinfo.cpus[curprocessor].socket = val;
			if ((unsigned)(val + 1) > cpuinfo.numsockets)
			cpuinfo.numsockets = val + 1;
		} else if (STRMATCH("core id", lineptr)) {
			int val = atoi(strchr(lineptr + 8, ':') + 1);
			cpuinfo.cpus[curprocessor].corenum = val;
		}
		free(lineptr);
		lineptr = NULL;
		n = 0;
	}
	if (lineptr)
		free(lineptr);

	fclose(cpuinfofile);

	return 0;
}

/* See
 * https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-system-cpu
 */
static int parse_sysfs(void)
{
	glob_t globbuf;
	unsigned i;
	unsigned numabit=0;
	unsigned numnodes=0;
	int ofs=sizeof("/sys/devices/system/node/node")-1;

	glob("/sys/devices/system/node/node[0-9]*/cpu[0-9]*", 0, NULL, &globbuf);
	for (i = 0; i < globbuf.gl_pathc; ++i) {
		const char *s = globbuf.gl_pathv[i]+ofs;
		int node = atoi(s);
		int cpu;
		if (((1<<node) & numabit) == 0) {
			numabit |= (1<<node);
			++numnodes;
		}
		cpu = atoi(strchr(s, '/') + 4);
		if (cpu < MAX_CPUS)
			cpuinfo.cpus[cpu].numanode = node;
	}
	globfree(&globbuf);
	cpuinfo.numnodes = numnodes;

	return 0;
}

cpuinfo_t *get_cpuinfo(void)
{
	if (!parsed) {
		if (parse_cpuinfo() != 0)
			return 0;
		if (parse_sysfs() != 0)
			return 0;
		parsed = 1;
	}

	return &cpuinfo;
}

/* gcc -D_CPUINFO_UNITTEST_ cpuinfo.c -o c */
#ifdef _CPUINFO_UNITTEST_
#include <assert.h>

int main()
{
	int i;

	cpuinfo_t *c = get_cpuinfo();
	assert(c != 0);
	printf("Num CPUs: %u (0x%016llX), num sockets=%u, num nodes=%u\n",
		c->numcpus, (long long)c->cpubitmap, c->numsockets, c->numnodes);
	for (i = 0; i < c->numcpus; ++i)
		printf("CPU%d: core=%u, socket=%u, numanode=%u\n",
			i, c->cpus[i].corenum, c->cpus[i].socket,
			c->cpus[i].numanode);

	return 0;
}
#endif
