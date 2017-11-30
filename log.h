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

#ifndef __LOG_H
#define __LOG_H

#ifndef __MODULE__
#error "__MODULE__ must be defined prior to including log.h!"
#endif

#ifdef	__cplusplus
extern "C" {
#endif

#define LOG_OPT_SYSLOG (1<<0)		/* Log to syslog LOCAL0 facility */
#define LOG_OPT_STDOUT (1<<1)		/* Log to stdout */
#define LOG_OPT_DETAIL (1<<2)		/* Include file,line,function,loglevel detail */
#define LOG_OPT_TIMESTAMP (1<<3)	/* Add date/time to stdout log */

/**
 * Initializes logging system.
 *
 * @param ident   Identifier used for syslog
 * @param options Options bitmap, combination of LOG_OPT_*
 */
void open_log(const char *ident, int options);
void close_log(void);

#define LOG_LVL_EMERG		0		/* System is unusable */
#define LOG_LVL_ALERT		1		/* Action must be taken immediately */
#define LOG_LVL_CRIT		2		/* Critical conditions */
#define LOG_LVL_ERR		3		/* Error conditions */
#define LOG_LVL_WARNING		4		/* Warning conditions */
#define LOG_LVL_NOTICE		5		/* Normal but significant condition */
#define LOG_LVL_INFO		6		/* Informational */
#define LOG_LVL_DEBUG		7		/* Debug-level messages */


/**
 * Set the log threshold, all log messages of equal or greater severity will be accepted.
 *
 * @param level  Log level (see LOG_LVL_*)
 */
void set_log_level(int level);
int get_log_level(void);

void
log_print(int level, const char *file, int line, const char *func,
		const char *fmsg, ...) __attribute__((format(printf, 5, 6)));

#define log_emergency(...) do { log_print(LOG_LVL_EMERG, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_alert(...) do { log_print(LOG_LVL_ALERT, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__) }; while (0)
#define log_critical(...) do { log_print(LOG_LVL_CRIT, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_error(...) do { log_print(LOG_LVL_ERR, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_warning(...) do { log_print(LOG_LVL_WARNING, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_notice(...) do { log_print(LOG_LVL_NOTICE, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_info(...) do { log_print(LOG_LVL_INFO, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)
#define log_debug(...) do { log_print(LOG_LVL_DEBUG, __MODULE__, __LINE__, __FUNCTION__, __VA_ARGS__); } while (0)

#define log_once(x) do { static int _logged=0; if (!_logged) { _logged=1; x; } } while (0);

#ifdef	__cplusplus
}
#endif

#endif /* __LOG_H */
