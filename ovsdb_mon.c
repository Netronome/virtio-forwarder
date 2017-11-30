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
#include <errno.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>

#include "ovsdb_mon.h"
#define __MODULE__ "ovsdb_mon"
#include "log.h"
#include "rte_ethdev.h"
#include "virtio_worker.h"
#include "sriov.h"

#define MAX_READ 65535

/*
 * Monitor ovsdb for ports added with virtio_forwarder value in external_ids column,
 * e.g.
 * ovs-vsctl add-port br-virtio eth1 \
 * -- set interface eth1 external_ids:virtio_forwarder=1
 */

struct port_config {
	int virtio_enabled;
	long int virtio_id;
};

static struct ovsdb_mon_conf ovsdb_conf;
static const char tok_initial[] = "initial";
static const char tok_insert[] = "insert";
static const char tok_delete[] = "delete";
static const char tok_old[] = "old";
static const char tok_new[] = "new";
static const char tok_relay_id[] = "virtio_relay";
static const char tok_forwarder[] = "virtio_forwarder";
static pthread_t ovsdb_mon_thread;
static int running;
static int must_stop;

static int extract_field(const char* buf, int start, char* field)
{
	char* end;
	size_t dstptr;
	size_t srcptr;

	buf = buf + start;
	if (buf[0] == '"') {
		/* Quoted string. */
		srcptr = 1;
		dstptr = 0;
		while (srcptr < strlen(buf)) {
			if (buf[srcptr] == '"') {
				if (buf[srcptr + 1] == '"') {
					field[dstptr++] = buf[srcptr++];
					srcptr++;
				} else if (buf[srcptr + 1] == '\0' ||
						buf[srcptr + 1] == ',') {
					srcptr++;
					break;
				} else {
					log_error("Warning, truncating badly quoted string\n");
					while(srcptr < strlen(buf) &&
							buf[srcptr] != '\0' &&
							buf[srcptr] != ',') {
						srcptr++;
					}
					return start + srcptr + 1;
				}
			} else {
				field[dstptr++] = buf[srcptr++];
			}
		}
		field[dstptr] = '\0';
		return start + srcptr + 1;
	} else {
		/* Unquoted string. */
		end = strchr(buf, ',');
		if (!end)
			end = strchr(buf, '\0');
		memcpy(field, buf, end - buf);
		field[end - buf] = '\0';
		return start + (end - buf) + 1;
	}
}

static void unquote(char* str)
{
	size_t dstptr;
	size_t srcptr;
	char tmp[MAX_READ];
	if (str[0] == '"') {
		strcpy(tmp, str);
		srcptr = 1;
		dstptr = 0;
		while (srcptr < strlen(tmp)) {
			if (tmp[srcptr] == '"') {
				if (tmp[srcptr + 1] == '\0') {
					/*String done!*/
					break;
				} else {
					log_error("Warning, truncating badly quoted string\n");
					break;
				}
			} else if (tmp[srcptr] == '\\') {
				if (tmp[srcptr + 1] == '\0') {
					log_error("Warning, badly quoted string\n");
					break;
				} else {
					/*Escaped character*/
					srcptr++;
				}
				str[dstptr++] = tmp[srcptr++];
			} else {
				str[dstptr++] = tmp[srcptr++];
			}
		}
		str[dstptr] = '\0';
	}
}


enum map_parse_state {
	STATE_START,
	STATE_KEY,
	STATE_VAL,
	STATE_EQ,
	STATE_SEP,
	STATE_DONE
};

static void parse_map(const char* map, struct port_config* port_config)
{
	enum map_parse_state parser_state;
	char key[MAX_READ];
	char val[MAX_READ];

	int srcptr;
	int dstptr;

	char *endptr;

	parser_state = STATE_START;
	port_config->virtio_enabled = 0;
	port_config->virtio_id = 0;

	srcptr = 0;
	while(parser_state != STATE_DONE) {
		/* Sink whitespace. */
		while (isspace(map[srcptr]))
			srcptr++;

		switch(parser_state) {
		case STATE_START:
			if (map[srcptr] != '{') {
				log_error("Warning, expected '{'");
				srcptr++;
				while (map[srcptr] != '{' && map[srcptr] != '\0')
					srcptr++;
			}
			if (map[srcptr] == '{') {
				srcptr++;
				parser_state = STATE_KEY;
			} else {
				parser_state = STATE_DONE;
			}
			break;

		case STATE_KEY:
			if (map[srcptr] == '}') {
				parser_state = STATE_DONE;
				break;
			} else if (map[srcptr] == '"') {
				/* Quoted string. */
				srcptr++;
				dstptr = 0;
				key[dstptr] = '\0';
				while (parser_state == STATE_KEY) {
					if (map[srcptr] == '\0') {
						log_error("Warning, badly quoted string.");
						parser_state = STATE_DONE;
						break;
					}
					if (map[srcptr] == '"') {
						/* String done! */
						srcptr++;
						key[dstptr] = '\0';
						parser_state = STATE_EQ;
						break;
					} else if (map[srcptr] == '\\') {
						/* Escaped character. */
						srcptr++;
					}
					key[dstptr++] = map[srcptr++];
				}
			} else {
				dstptr = 0;
				while (!isspace(map[srcptr]) &&
					   map[srcptr] != '\0' &&
					   map[srcptr] != '=') {
					key[dstptr++] = map[srcptr++];
				}
				key[dstptr] = '\0';
				parser_state = STATE_EQ;
			}
			break;

		case STATE_VAL:
			if (map[srcptr] == '"') {
				/*quoted string*/
				srcptr++;
				dstptr = 0;
				val[dstptr] = '\0';
				while (parser_state == STATE_VAL) {
					if (map[srcptr] == '\0') {
						log_error("Warning, badly quoted string.");
						parser_state = STATE_DONE;
						break;
					}
					if (map[srcptr] == '"') {
						/*String done!*/
						srcptr++;
						val[dstptr] = '\0';
						parser_state = STATE_SEP;
						break;
					} else if (map[srcptr] == '\\') {
						/*Escaped character*/
						srcptr++;
					}
					val[dstptr++] = map[srcptr++];
				}
			} else {
				dstptr = 0;
				while (!isspace(map[srcptr]) &&
					   map[srcptr] != '\0' &&
					   map[srcptr] != ',' &&
					   map[srcptr] != '}') {
					val[dstptr++] = map[srcptr++];
				}
				val[dstptr] = '\0';
				parser_state = STATE_SEP;
			}
			if (strcmp(tok_forwarder, key) == 0 ||
					strcmp(tok_relay_id, key) == 0) {
				port_config->virtio_enabled = 1;
				errno = 0;
				port_config->virtio_id = strtol(val, &endptr, 0);
				if ((errno == ERANGE &&
						(port_config->virtio_id ==
						LONG_MAX ||
						port_config->virtio_id ==
						LONG_MIN)) ||
						(errno != 0 &&
						port_config->virtio_id == 0)) {
					log_error("Error getting relay id: %m");
					port_config->virtio_enabled = 0;
				}
				if (endptr == val) {
					log_error("Empty virtio_id\n");
					port_config->virtio_enabled = 0;
				}
				if (*endptr != '\0')	/* Not necessarily an error... */
					log_warning("Warning, trailing characters on virtio_id\n");
			}
			break;

		case STATE_EQ:
			if (map[srcptr] != '=') {
				log_error("Warning, expected '='");
				srcptr++;
				while (map[srcptr] != '=' && map[srcptr] != '\0') {
					srcptr++;
				}
			}
			if (map[srcptr] == '=') {
				srcptr++;
				parser_state = STATE_VAL;
			} else {
				parser_state = STATE_DONE;
			}
			break;

		case STATE_SEP:
			if (map[srcptr] != ',' && map[srcptr] != '}') {
				log_error("Warning, expected ',' or '}'");
				srcptr++;
				while (map[srcptr] != ',' &&
					   map[srcptr] != '\0' &&
					   map[srcptr] != '}' ) {
					srcptr++;
				}
			}
			if (map[srcptr] == ',') {
				srcptr++;
				parser_state = STATE_KEY;
			} else {
				parser_state = STATE_DONE;
			}
			break;

		case STATE_DONE:
			break;
		}
	}
}

static int is_ovspidfile(const struct dirent *d)
{
	if (strncmp(d->d_name, "vovsdbc", 7) == 0)
		return (strcmp(d->d_name + strlen(d->d_name) - 4, ".pid") ==
			0 ? 1 : 0);

	return 0;
}

static void clean_old(void)
{
	struct dirent **namelist;
	int n;

	n = scandir("/tmp", &namelist, is_ovspidfile, alphasort);
	if (n > 0) {
		while (n--) {
			char buf[128];
			log_debug("Found old pidfile: '%s'",
				namelist[n]->d_name);
			snprintf(buf, 128, "/usr/bin/pkill -F /tmp/%s ovsdb-client &>/dev/null",
				namelist[n]->d_name);
			if (system(buf) == -1)
				log_debug("Error killing stale ovsdb-client");
			unlink(namelist[n]->d_name);
			free(namelist[n]);
		}
		free(namelist);
	}
}

static int check_ovsdb_sock(void)
{
	struct stat statbuf;

	if (stat(ovsdb_conf.ovsdb_sock_path, &statbuf) == 0) {
		if ((statbuf.st_mode&(S_IFSOCK | S_IRWXU)) !=
				(S_IFSOCK | S_IRWXU)) {
			log_error("'%s' is not a user read/write/executable socket path!",
				ovsdb_conf.ovsdb_sock_path);
			return 1;
		}
	} else {
		log_error("Could not open ovsdb socket path '%s'!",
			ovsdb_conf.ovsdb_sock_path);
		return 1;
	}

	return 0;
}

static FILE*
setup_ovsdb_mon(const char *cmd, const char *pidfile, pid_t *ovsdbclient_pid,
		int *cmd_sock)
{
	FILE *cmd_stdout;
	char pid[16];
	int bytes;
	int readpid;

	cmd_stdout = popen(cmd, "r");
	if (cmd_stdout == NULL) {
		log_error("Error connecting to ovsdb!");
		return NULL;
	}

	sleep(1);
	readpid = open(pidfile, O_RDONLY);
	if (readpid < 0) {
		log_error("Error opening ovsdb-client PID file '%s'!", pidfile);
		pclose(cmd_stdout);
		return NULL;
	}

	if ((bytes=read(readpid, pid, 16))<=0) {
		close(readpid);
		log_error("Error reading ovsdb-client PID file!");
		pclose(cmd_stdout);
		return NULL;
	}
	close(readpid);
	*ovsdbclient_pid=(pid_t)strtol(pid, 0, 10);
	*cmd_sock = fileno(cmd_stdout);
	log_debug("ovsdb-client subprocess running with PID %u",
		(unsigned)*ovsdbclient_pid);

	return cmd_stdout;
}

static void
process_external_ids(char *action, char *name, struct port_config *port_config,
			int *virtio_id_change)
{
	struct sriov_info vfinfo;

	if (port_config->virtio_enabled && (strcmp(action, tok_insert) == 0 ||
			strcmp(action, tok_initial) == 0)) {
		if (get_VF_PCIE_from_netdev(name, &vfinfo) == 0) {
			#ifndef VIRTIO_ECHO
			log_info("Adding netdev '%s' (%s) to virtio %u",
				name, vfinfo.dbdf, vfinfo.vf);
			if (virtio_forwarder_add_vf2(vfinfo.dbdf, vfinfo.vf, 1) != 0)
				log_warning("Error adding netdev '%s' (%s) to virtio %u!",
					name, vfinfo.dbdf, vfinfo.vf);
			#endif
		} else {
			log_warning("Failed to retrieve VF PCI info for netdev '%s'!",
				name);
		}
	} else if (port_config->virtio_enabled &&
			strcmp(action, tok_delete) == 0) {
		struct sriov_info vfinfo;
		if (get_VF_PCIE_from_netdev(name, &vfinfo) == 0) {
			#ifndef VIRTIO_ECHO
			log_info("Removing netdev '%s' (%s) from virtio %u",
				name, vfinfo.dbdf, vfinfo.vf);
			if (virtio_forwarder_remove_vf2(vfinfo.dbdf, vfinfo.vf, 1) != 0)
				log_warning("Error removing netdev '%s' (%s) from virtio %u",
					name, vfinfo.dbdf, vfinfo.vf);
			#endif
		} else {
			log_warning("Failed to retrieve VF PCI info for netdev '%s'!",
				name);
		}
	} else if (strcmp(action, tok_old) == 0) {
		if (port_config->virtio_enabled)
			*virtio_id_change = port_config->virtio_id;
		else
			*virtio_id_change = -1;
	} else if (strcmp(action, tok_new) == 0) {
		#ifndef VIRTIO_ECHO
		struct sriov_info vfinfo;
		if (get_VF_PCIE_from_netdev(name, &vfinfo) == 0) {
			if (*virtio_id_change != -1) {
				log_info("Removing netdev '%s' (%s) from virtio %u",
					name, vfinfo.dbdf, vfinfo.vf);
				if (virtio_forwarder_remove_vf2(vfinfo.dbdf, vfinfo.vf, 1) != 0)
					log_warning("Error removing netdev '%s' (%s) from virtio %u",
						name, vfinfo.dbdf, vfinfo.vf);
				*virtio_id_change = -1;
			}
			if (port_config->virtio_enabled) {
				log_info("Adding netdev '%s' (%s) to virtio %u",
					name, vfinfo.dbdf, vfinfo.vf);
				if (virtio_forwarder_add_vf2(vfinfo.dbdf, vfinfo.vf, 1) != 0)
					log_info("Error adding netdev '%s' (%s) to virtio %u",
						name, vfinfo.dbdf, vfinfo.vf);
			}
		} else {
			log_warning("Failed to retrieve VF PCI info for netdev '%s'!",
				name);
		}
		#endif
	} else {
		log_debug("Ignoring token '%s'", action);
	}
}

static void parse_ovsdb_client(int cmd_sock)
{
	int remain;
	int len;
	ssize_t count;
	char buf[MAX_READ];
	char tmp[MAX_READ];
	char action[MAX_READ];
	char name[MAX_READ];
	char *nlpos;
	size_t pos;
	struct port_config port_config;
	int virtio_id_change = -1;

	remain = 0;
	count = read(cmd_sock, buf + remain, MAX_READ - remain - 1);
	while (remain + count > 0) {
		len = remain + count;
		buf[len] = '\0';
		remain = len;
		nlpos = strchr(buf, '\n');
		while (nlpos) {
			/* Parse a single line in the form:
			 * hash_nr,action,"""name""","{virtio_forwarder=""1""}" */
			nlpos[0] = '\0';
			if (strlen(buf)) {
				pos = 0;
				/* Parse hash */
				pos = extract_field(buf, pos, tmp);
				if (pos >= strlen(buf)) {
					log_error("Warning, truncated row.");
				}
				/* Parse action */
				pos = extract_field(buf, pos, action);
				if (pos >= strlen(buf)) {
					log_error("Warning, truncated row.");
				}
				/* Parse name */
				pos = extract_field(buf, pos, name);
				if (pos >= strlen(buf)) {
					log_error("Warning, truncated row.");
				}
				unquote(name);
				/* Parse external_ids */
				pos = extract_field(buf, pos, tmp);
				parse_map(tmp, &port_config);
				process_external_ids(action, name, &port_config,
						&virtio_id_change);

			}
			/* Copy the data of the next line to the
			 * beginning of buf. */
			len = strlen(nlpos + 1);
			memmove(buf, nlpos + 1, len + 1);
			remain = len;
			nlpos = strchr(buf, '\n');
		}
		if (remain == (MAX_READ - 1)) {
			log_error("Warning, ignoring long string!");
			count = read(cmd_sock, buf, 1);
			while (count > 0) {
				if (buf[0] == '\n') {
					remain = 0;
					break;
				}
				count = read(cmd_sock, buf, 1);
			}
			if (count < 1) {
				break;
			}
		}
		count = read(cmd_sock, buf + remain, MAX_READ - remain - 1);
		if (count < 1)
			break;
	}
}

static void* ovsdb_mon_threadmain(void *arg __attribute__((unused)))
{
	char cmd[256];
	FILE* cmd_stdout = NULL;
	int cmd_sock = -1;
	fd_set readfds;
	struct timeval sel_timeout;
	static pid_t ovsdbclient_pid;
	char pidfile[32];
	bool ovsdb_conn_down = true;

	log_debug("Starting ovsdb_mon thread");

	snprintf(pidfile, 32, "/tmp/vovsdbc%u.pid", (unsigned)getpid());
	snprintf(cmd, 256, "ovsdb-client --pidfile=%s -f csv --no-headings "
		"monitor Interface name,external_ids 2>/dev/null",
		pidfile);

	must_stop = 0;
	running = 1;
	while (running && !must_stop) {
		int ret;

		/* Poll ovsdb. */
		while (ovsdb_conn_down) {
			if (check_ovsdb_sock() == 0) {
				clean_old();
				if ((cmd_stdout = setup_ovsdb_mon(cmd, pidfile,
						&ovsdbclient_pid, &cmd_sock))
						!= NULL) {
					ovsdb_conn_down = false;
					break;
				}
			}
			sleep(5);
		}
		if (check_ovsdb_sock()) {
			ovsdb_conn_down = true;
			pclose(cmd_stdout);
			continue;
		}

		/*
		 * Connection is up.
		 */
		FD_ZERO(&readfds);
		FD_SET(cmd_sock, &readfds);
		sel_timeout.tv_sec = 0;
		sel_timeout.tv_usec = 100000; /* 100ms */
		ret = select(cmd_sock+1, &readfds, 0, 0, &sel_timeout);
		if (ret == 0)
			continue;
		if (ret < 0) {
			log_error("Error reading ovsdb: %m");
			ovsdb_conn_down = true;
			pclose(cmd_stdout);
			continue;
		}
		if (!FD_ISSET(cmd_sock, &readfds)) {
			log_warning("Unexpectedly returned from select with nothing to read!");
			continue;
		}

		parse_ovsdb_client(cmd_sock);
	}
	log_debug("stopping ovsdb-client subprocess (PID %u)",
		(unsigned)ovsdbclient_pid);
	kill(ovsdbclient_pid, SIGTERM);
	usleep(100000);
	kill(ovsdbclient_pid, SIGKILL);
	unlink(pidfile);
	if (!ovsdb_conn_down)
		pclose(cmd_stdout);

	running = 0;
	log_debug("ovsdb_mon thread ending");

	return 0;
}

int ovsdb_mon_start(const struct ovsdb_mon_conf *conf)
{
	log_debug("Creating ovsdb_mon thread");
	ovsdb_conf = *conf;
	pthread_create(&ovsdb_mon_thread, 0, ovsdb_mon_threadmain, 0);

	return 0;
}

void ovsdb_mon_stop(void)
{
	struct timespec ts;
	struct timeval tv;

	must_stop = 1;
	gettimeofday(&tv, 0);
	tv.tv_sec += 2;
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = tv.tv_usec*1000;
	log_debug("Stopping ovsdb_mon thread");
	if (pthread_timedjoin_np(ovsdb_mon_thread, 0, &ts) != 0) {
		log_debug("Timeout waiting for ovsdb_mon_thread, cancelling thread...");
		pthread_cancel(ovsdb_mon_thread);
	}
	pthread_join(ovsdb_mon_thread, 0);
	log_debug("Stopped ovsdb_mon thread");
}
