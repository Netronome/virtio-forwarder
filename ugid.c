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

#include "ugid.h"
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

int get_uid(const char *uname, uid_t *uid)
{
	struct passwd pwd;
	struct passwd *res;
	char *tmpbuf;
	size_t tmpbufsize;
	int r;

	tmpbufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (tmpbufsize == (size_t)-1)
		tmpbufsize = 256;
	tmpbuf = calloc(1, tmpbufsize);
	if (tmpbuf == NULL)
		return -ENOMEM;
	r = getpwnam_r(uname, &pwd, tmpbuf, tmpbufsize, &res);
	if (res == 0) {
		if (r == 0) {
			r = -ENOENT;
			goto out;
		} else {
			goto out;
		}
	}
	*uid = pwd.pw_uid;
out:
	if (tmpbuf)
		free(tmpbuf);
	return r;
}

int get_gid(const char *gname, gid_t *gid)
{
	struct group grp;
	struct group *res;
	char *tmpbuf;
	size_t tmpbufsize;
	int r;

	tmpbufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
	if (tmpbufsize == (size_t)-1)
		tmpbufsize = 256;
	tmpbuf = calloc(1, tmpbufsize);
	if (tmpbuf == NULL)
		return -ENOMEM;
	r = getgrnam_r(gname, &grp, tmpbuf, tmpbufsize, &res);
	if (res == 0) {
		if (r == 0) {
			r = -ENOENT;
			goto out;
		} else {
			goto out;
		}
	}
	*gid = grp.gr_gid;
out:
	if (tmpbuf)
		free(tmpbuf);
	return r;
}
