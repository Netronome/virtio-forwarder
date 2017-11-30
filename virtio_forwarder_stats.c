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

/* A short program to illustrate how to read the debug counters from
   virtio-forwarder */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#define MAXPTS 255

int main(void)
{
	FILE* ptsfile;
	char ptsfname[MAXPTS];
	int rc;
	char* retptr;
	int ptsfd;
	char junk;

	ptsfile = fopen("/var/run/virtioforwarder.pts", "r");
	if (ptsfile == NULL) {
		printf("Cannot find Debug PTS\n");
		return 1;
	}
	retptr = fgets(ptsfname, MAXPTS, ptsfile);
	if (retptr == NULL) {
		perror("Locating Debug PTS");
		fclose(ptsfile);
		return 1;
	}
	fclose(ptsfile);
	ptsfd = open(ptsfname, O_RDWR | O_NOCTTY);
	if (ptsfd < 0) {
		perror("Opening Debug PTS");
		return 1;
	}

	junk = 42;
	rc = write(ptsfd, &junk, 1);
	if (rc < 0) {
		perror("Writing to Debug PTS");
		return 1;
	}
	while (junk != 0) {
		rc = read(ptsfd, &junk, 1);
		if (rc != 1) {
			perror("Reading from Debug PTS");
			return 1;
		}
		printf("%c", junk);
	}
	fflush(stdout);
	close(ptsfd);
	return 0;
}
