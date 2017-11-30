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

#define __MODULE__ "log"
#include "log.h"
#include <syslog.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define BUFLEN 256

static int _loglevel = -1;
static int _options;

void open_log(const char *ident, int options)
{
	_options = options;
	if (_options & LOG_OPT_SYSLOG)
		openlog(ident, LOG_NDELAY | LOG_PID, LOG_LOCAL0);
	if (_loglevel == -1)
		set_log_level(LOG_LVL_INFO);
}

void close_log(void)
{
	if (_options & LOG_OPT_SYSLOG)
		closelog();
}

void set_log_level(int level)
{
	if (level > LOG_LVL_DEBUG)
		level = LOG_LVL_DEBUG;
	setlogmask(LOG_UPTO(level));
	_loglevel = level;
}

int get_log_level(void)
{
	return _loglevel;
}

void
log_print(int level, const char *file, int line, const char *func,
		const char *fmsg, ...)
{
	va_list ap;
	char buf[BUFLEN];
	int len = 0;
	static const char *months[12] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
		"Oct", "Nov", "Dec"
	};
	static char levelchar[LOG_LVL_DEBUG + 1] = {
		'x', //EMERG
		'!', //ALERT
		'^', //CRIT
		'*', //ERROR
		'#', //WARN
		'-', //NOTICE
		'?', //INFO
		'.', //DEBUG
	};

	if (level > _loglevel)
		return;

	va_start(ap, fmsg);
	if (_options & LOG_OPT_DETAIL)
		len += snprintf(buf + len, BUFLEN - len, "%s:%d(%s) %c ",
				file, line, func, level <= LOG_LVL_DEBUG ?
				levelchar[level] : 'x');
	len += vsnprintf(buf + len, BUFLEN - len, fmsg, ap);
	if (len >= BUFLEN - 2)
		len = BUFLEN - 2;
	va_end(ap);
	if (_options & LOG_OPT_SYSLOG)
		syslog(level, "%s", buf);
	if (_options & LOG_OPT_STDOUT) {
		buf[len] = '\n';
		buf[len + 1] = 0;
		if ((_options & (LOG_OPT_STDOUT | LOG_OPT_TIMESTAMP)) ==
				(LOG_OPT_STDOUT | LOG_OPT_TIMESTAMP)) {
			struct tm tms;
			time_t t = time(0);
			localtime_r(&t, &tms);
			fprintf(stdout, "%s %02d %02d:%02d:%02d ",
				months[tms.tm_mon], tms.tm_mday, tms.tm_hour,
				tms.tm_min, tms.tm_sec);
		}
		fprintf(stdout, "%s", buf);
		fflush(stdout);
	}
}
