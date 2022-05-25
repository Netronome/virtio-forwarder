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

#define __MODULE__ "virtio_forwarder_main"
#include "log.h"
#include "cmdline.h"
#include "virtio_vhostuser.h"
#include "dpdk_eal.h"
#include "ovsdb_mon.h"
#include "file_mon.h"
#include "vrelay_version.h"
#include "zmq_config.h"
#include "zmq_port_control.h"
#include "zmq_server.h"
#include "zmq_stats.h"
#include "zmq_core_sched.h"
#include "rte_version.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdbool.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <bsd/string.h>

#define DEFAULT_MASTER_LCORE 0
#define DEFAULT_LOG_LEVEL 6
#define str(s) str__(s)
#define str__(s) #s

#define DEFAULT_PID_PATH "/var/run"
#define DEFAULT_HUGE_PATH "/mnt/huge"
#define DEFAULT_OVSDB_SOCK "/usr/local/var/run/openvswitch/db.sock"
#define DEFAULT_ZMQ_CONFIG_EP "ipc:///var/run/virtio-forwarder/config"
#define DEFAULT_ZMQ_PORT_CONTROL_EP "ipc:///var/run/virtio-forwarder/port_control"
#define DEFAULT_ZMQ_STATS_EP "ipc:///var/run/virtio-forwarder/stats"
#define DEFAULT_ZMQ_CORE_SCHED_EP "ipc:///var/run/virtio-forwarder/core_sched"

#define DEFAULT_VHOSTUSER_USERNAME "libvirt-qemu"
#define DEFAULT_VHOSTUSER_GROUPNAME "kvm"
#define DEFAULT_VHOSTUSER_PATH "/tmp"  /* /var/run/virtio-forwarder */
#define DEFAULT_VHOSTUSER_SOCKNAME "virtio-forwarder%u.sock"
#define DAEMONNAME "virtio-forwarder"

static bool running = true;
static bool must_stop = false;
static char pidpath[100];
static char pidfile[128];
static int remove_pidfile = 1;
static bool daemonize = true;
static bool log_syslog = true;
static bool use_zmq_config = false;
static bool use_zmq_port_control = false;
static bool use_zmq_stats = false;
static bool use_zmq_core_sched = false;
static bool use_ipc = false;
static bool show_version = false;
static pthread_t main_thread;
static const char *daemonname = DAEMONNAME;
static struct dpdk_conf dpdk_cfg = {0};
static struct ovsdb_mon_conf ovsdb_cfg = {.ovsdb_sock_path=""};
static char zmq_config_ep[1024] = {'\0'};
static char zmq_port_control_ep[1024] = {'\0'};
static char zmq_stats_ep[1024] = {'\0'};
static char zmq_core_sched_ep[1024] = {'\0'};
struct virtio_vhostuser_conf vhost_conf = { 0 };
static bool create_vhostuser_sockets = true;

static void sig_handler(int s)
{
	if (running && pthread_equal(pthread_self(), main_thread)) {
		log_debug("Got signal %d", s);
		must_stop = true;
	}
}

static void
__backtrace_signal_handler(int sig, siginfo_t *info __attribute__((unused)),
				void *ptr __attribute__((unused)))
{
	void *bt_array[20];
	unsigned int n;
	size_t bt_size = backtrace(bt_array, sizeof(bt_array) / sizeof(void *));

	log_critical("Signal: %d", sig);
	if (bt_size > 0) {
		char **bt_strings = backtrace_symbols(bt_array, bt_size);
		log_critical("-- backtrace --");
		if (bt_strings == NULL) {
			for (n = 0; n < bt_size; ++n)
				log_critical("%p", bt_array[n]);
		} else {
			for (n = 0; n < bt_size; ++n)
				log_critical("%s", bt_strings[n]);
			free(bt_strings);
		}
	} else {
		log_critical("-- no backtrace available --");
	}
	exit(1);
}

static int sig_segv_abrt_backtrace(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = __backtrace_signal_handler;
	action.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &action, NULL) < 0)
		return -errno;
	if (sigaction(SIGABRT, &action, NULL) < 0)
		return -errno;

	return 0;
}

static int create_daemon_file(const char *filename, int *must_remove)
{
	int fd = open(filename, O_RDWR | O_CREAT, 0644);
	char pid[10];

	if (fd < 0) {
		*must_remove = 1;
		return -errno;
	}
	if (lockf(fd, F_TLOCK, 0) < 0) {
		*must_remove = 0;
		return -errno;
	}
	snprintf(pid, 10, "%u\n", getpid());
	if (write(fd, pid, strlen(pid)) < 0) {
		*must_remove = 1;
		return -errno;
	}
	fsync(fd);

	return 0;
}

static int
cmdline_set_cpus(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	unsigned len;

	if (strncmp(arg, "0x", 2)==0) {
		/* Look for valid hex bitmap. */
		arg+=2;
		len=strlen(arg);
		if (len!=strspn(arg, "0123456789abcdefABCDEF")) {
			fprintf(stderr, "Invalid hex CPU bitmap '0x%s'\n!", arg);
			return -1;
		}
		char *eptr=0;
		vhost_conf.worker_core_bitmap=strtoull(arg, &eptr, 16);
		if (*eptr) {
			fprintf(stderr, "Invalid hex CPU bitmap '0x%s'\n!", arg);
			return -1;
		}
		return 0;
	} else {
		/* Look for valid comma separated list of integers. */
		len=strlen(arg);
		if (len!=strspn(arg, "0123456789,")) {
			fprintf(stderr, "Invalid comma separated list of CPUs '%s'\n!",
				arg);
			return -1;
		}
		vhost_conf.worker_core_bitmap=0;
		char *buf=strdup(arg);
		char *s=strtok(buf, ",");
		while (s) {
			char *eptr=0;
			unsigned long l=strtoul(s, &eptr, 10);
			if (*eptr || l>=64) {
				fprintf(stderr, "Invalid CPU value in list '%s' (%lu)\n!", s, l);
				vhost_conf.worker_core_bitmap=0;
				break;
			}
			vhost_conf.worker_core_bitmap|=(1ULL<<l);
			s=strtok(NULL, ",");
		}
		free(buf);
		if (vhost_conf.worker_core_bitmap!=0)
			return 0;
	}

	return -1;
}

static int
cmdline_set_master_lcore(void *opaque __attribute__((unused)),
				const char *arg __attribute__((unused)),
				int opt_index __attribute__((unused)))
{
	unsigned master_lcore = (unsigned)atoi(arg);

	dpdk_cfg.master_lcore = master_lcore;

	return 0;
}

static int
cmdline_set_nodaemon(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	if (arg == NULL || strcmp(arg, "log_syslog") != 0)
		log_syslog = false;
	daemonize = false;

	return 0;
}

static int
cmdline_set_ipc(void *opaque __attribute__((unused)),
		const char *arg __attribute__((unused)),
		int opt_index __attribute__((unused)))
{
	use_ipc = true;

	return 0;
}

static int
cmdline_set_loglevel(void *opaque __attribute__((unused)),
			const char *arg,
			int opt_index __attribute__((unused)))
{
	int level=atoi(arg);

	set_log_level(level);
	dpdk_cfg.log_level = level;

	return 0;
}

static int
cmdline_set_pid(void *opaque __attribute__((unused)),
		const char *arg,
		int opt_index __attribute__((unused)))
{
	struct stat fstat;

	if (stat(arg, &fstat) == 0) {
		if (!S_ISDIR(fstat.st_mode)) {
			fprintf(stderr, "Provided pidfile path ('%s') is not a valid path\n",
				arg);
			return 1;
		}
		strlcpy(pidpath, arg, 100);
	} else {
		fprintf(stderr, "Error accessing provided pidfile path '%s': %m\n",
			arg);
		return 1;
	}

	return 0;
}

static int
cmdline_set_huge_dir(void *opaque __attribute__((unused)),
			const char *arg,
			int opt_index __attribute__((unused)))
{
	struct stat fstat;

	if (stat(arg, &fstat) == 0) {
		if (!S_ISDIR(fstat.st_mode)) {
			fprintf(stderr, "Provided hugepage path ('%s') is not a valid path\n",
				arg);
			return 1;
		}
		snprintf(dpdk_cfg.huge_dir, 32, "%s", arg);
	} else {
		fprintf(stderr, "Error accessing provided hugepage path '%s': %m\n",
			arg);
		return 1;
	}

	return 0;
}

static int
cmdline_set_ovsdb_path(void *opaque __attribute__((unused)),
			const char *arg,
			int opt_index __attribute__((unused)))
{
	snprintf(ovsdb_cfg.ovsdb_sock_path, 128, "%s", arg);

	return 0;
}

static int
cmdline_set_zmq_config_ep(void *opaque __attribute__((unused)),
				const char *arg,
				int opt_index __attribute__((unused)))
{
	if (arg)
		strlcpy(zmq_config_ep, arg, sizeof zmq_config_ep);
	use_zmq_config = true;

	return 0;
}

static int
cmdline_set_zmq_port_control_ep(void *opaque __attribute__((unused)),
				const char *arg,
				int opt_index __attribute__((unused)))
{
	if (arg)
		strlcpy(zmq_port_control_ep, arg, sizeof zmq_port_control_ep);
	use_zmq_port_control = true;

	return 0;
}

static int
cmdline_set_zmq_stats_ep(void *opaque __attribute__((unused)),
				const char *arg,
				int opt_index __attribute__((unused)))
{
	if (arg)
		strlcpy(zmq_stats_ep, arg, sizeof zmq_stats_ep);
	use_zmq_stats = true;

	return 0;
}

static int
cmdline_set_zmq_core_sched_ep(void *opaque __attribute__((unused)),
				const char *arg,
				int opt_index __attribute__((unused)))
{
	if (arg)
		strlcpy(zmq_core_sched_ep, arg, sizeof zmq_core_sched_ep);
	use_zmq_stats = true;
	use_zmq_core_sched = true;

	return 0;
}

static int
cmdline_set_vhost_username(void *opaque __attribute__((unused)),
				const char *arg __attribute__((unused)),
				int opt_index __attribute__((unused)))
{
	if (arg)
		snprintf(vhost_conf.vhost_username, 32, "%s", arg);
	else
		vhost_conf.vhost_username[0] = 0;

	return 0;
}

static int
cmdline_set_vhost_groupname(void *opaque __attribute__((unused)),
				const char *arg __attribute__((unused)),
				int opt_index __attribute__((unused)))
{
	if (arg)
		snprintf(vhost_conf.vhost_groupname, 32, "%s", arg);
	else
		vhost_conf.vhost_groupname[0] = 0;

	return 0;
}

static int
cmdline_set_vhost_path(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	snprintf(vhost_conf.socket_path, 128, "%s", arg);
	return 0;
}

static int
cmdline_set_vhost_socket(void *opaque __attribute__((unused)),
				const char *arg __attribute__((unused)),
				int opt_index __attribute__((unused)))
{
	snprintf(vhost_conf.socket_name, 32, "%s", arg);

	return 0;
}

static int
cmdline_set_vf_cpu(void *opaque __attribute__((unused)),
			const char *arg,
			int opt_index __attribute__((unused)))
{
	unsigned virtio,cpu1,cpu2;

	if (strchr(arg, ',') == 0) {
		if (sscanf(arg, "%u:%u", &virtio, &cpu1) != 2) {
			fprintf(stderr, "Invalid virtio CPU specifier '%s', format: <virtio>:<cpu>[,<cpu>]\n",
				arg);
			return 1;
		}
		cpu2=cpu1;
	} else {
		if (sscanf(arg, "%u:%u,%u", &virtio, &cpu1, &cpu2) != 3) {
			fprintf(stderr, "Invalid virtio CPU specifier '%s', format: <virtio>:<cpu>[,<cpu>]\n",
				arg);
			return 1;
		}
	}
	if (virtio > MAX_RELAYS) {
		fprintf(stderr, "Invalid virtio %u specified, must be 0-%u!\n",
			virtio, MAX_RELAYS);
		return 1;
	}
	if (cpu1 >= 64 || cpu2 >= 64) {
		fprintf(stderr, "Invalid CPU for virtio %u specified, must be 0-63!\n",
			virtio);
		return 1;
	}
	vhost_conf.relay_cpus[virtio].vf2vio_cpu = cpu1;
	vhost_conf.relay_cpus[virtio].vio2vf_cpu = cpu2;

	return 0;
}

static int cmdline_set_vf_cpus(void *opaque, const char *arg, int opt_index)
{
	char *input, *saveptr, *tok;
	int rc;

	if (!(input = strdup(arg))) {
		fprintf(stderr, "%s: strdup: %m\n", __func__);
		return 1;
	}

	tok = strtok_r(input, ";", &saveptr);
	rc = 0;
	while (tok) {
		if ((rc = cmdline_set_vf_cpu(opaque, tok, opt_index)))
			break;

		tok = strtok_r(NULL, ";", &saveptr);
	}
	free(input);

	return rc;
}

static int
cmdline_show_version(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	show_version = true;

	return 0;
}

static int
cmdline_enable_jumbo(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	dpdk_cfg.use_jumbo = 1;
	vhost_conf.use_jumbo = 1;

	return 0;
}

static int
cmdline_enable_mrgbuf(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	vhost_conf.use_rx_mrgbuf = 1;

	return 0;
}

static int
cmdline_add_static_vf(void *opaque __attribute__((unused)),
			const char *arg,
			int opt_index __attribute__((unused)))
{
	unsigned num_entries;
	int *virtio_id;
	char *p, *dbdf;
	char testpci[50];
	struct stat fstat;

	num_entries = vhost_conf.static_relay_conf.num_static_entries;
	if (num_entries >= MAX_RELAYS) {
		fprintf(stderr, "Maximum relay entries (%u) already reached, cannot add more!\n",
			MAX_RELAYS);
		return 1;
	}
	virtio_id = &vhost_conf.static_relay_conf.static_relays[num_entries].virtio_id;
	dbdf = vhost_conf.static_relay_conf.static_relays[num_entries].pci_dbdf;
	p = strchr(arg, '=');
	if (p)
		*p = 0;
	if (p == 0 || sscanf(arg, "%20s", dbdf) != 1 ||
			sscanf(p+1, "%u", virtio_id) != 1) {
		fprintf(stderr, "Invalid static VF entry '%s', format is <PCI>=<virtio_id>, where <PCI> is in domain:bus:device.function format, and <virtio_id> is an integer\n",
			arg);
		return 1;
	}
	snprintf(testpci, 50, "/sys/bus/pci/devices/%s", dbdf);
	if (stat(testpci, &fstat) != 0) {
		fprintf(stderr, "Invalid PCI device '%s'!\n", dbdf);
		return 1;
	}
	if (*virtio_id >= MAX_RELAYS) {
		fprintf(stderr, "Invalid virtio ID %u, valid range is 0..%u!\n",
			*virtio_id, MAX_RELAYS - 1);
		return 1;
	}
	vhost_conf.static_relay_conf.num_static_entries++;

	return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
static int
cmdline_enable_vhostclient(void *opaque __attribute__((unused)),
				const char *arg __attribute__((unused)),
				int opt_index __attribute__((unused)))
{
	vhost_conf.vhost_client = 1;

	return 0;
}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(16,11,0,0)
static int
cmdline_enable_zerocopy(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	vhost_conf.zerocopy = 1;

	return 0;
}
#endif

static int
cmdline_enable_tso(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	vhost_conf.enable_tso = 1;
	dpdk_cfg.enable_tso = 1;

	return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
static int
cmdline_enable_dynamic_sockets(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	create_vhostuser_sockets = false;

	return 0;
}
#endif

static int
cmdline_enable_same_numa(void *opaque __attribute__((unused)),
			const char *arg __attribute__((unused)),
			int opt_index __attribute__((unused)))
{
	dpdk_cfg.enable_same_numa = 1;

	return 0;
}

static int configure_signals(void)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGSEGV);
	sigaddset(&sigset, SIGABRT);
	sigaddset(&sigset, SIGUSR1);
	sigaddset(&sigset, SIGUSR2);
	sigaddset(&sigset, SIGHUP);
	main_thread = pthread_self();
	pthread_sigmask(SIG_UNBLOCK, &sigset, 0);
	if (signal(SIGINT, sig_handler) == SIG_ERR) {
		return 1;
	}
	if (signal(SIGTERM, sig_handler) == SIG_ERR) {
		return 1;
	}
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		return 1;
	}
	sig_segv_abrt_backtrace();

	return 0;
}

static void virtiofwd_shutdown(void)
{
	if (remove_pidfile)
		unlink(pidfile);
	close_log();
}

cmdline_opt_t opts[] = {
	{ "cpus", 'C', CMDLINE_PARAM_FLAG_MANDATORY, cmdline_set_cpus, 1, "CPUs to use for worker threads (either comma separated integers or hex bitmap starting with 0x)" },
	{ "master-lcore", 'M', 0, cmdline_set_master_lcore, 1, "Set master lcore (default: " str(DEFAULT_MASTER_LCORE) ")" },
	{ "nodaemon", 'n', 0, cmdline_set_nodaemon, 2, "Don't daemonize. Optionally set to 'log_syslog' to switch from the default console output (default: run as daemon)" },
	{ "loglevel", 'l', 0, cmdline_set_loglevel, 1, "Set log threshold 0-7 (least to most verbose) (default: " str(DEFAULT_LOG_LEVEL) ")" },
	{ "pid-path", 'p', 0, cmdline_set_pid, 1, "PID file ("DAEMONNAME".pid) will be written to this directory (default: "DEFAULT_PID_PATH")" },
	{ "huge-dir", 'H', 0, cmdline_set_huge_dir, 1, "The mount path to the hugepages (default: "DEFAULT_HUGE_PATH")" },
	{ "ovsdb-sock", 'O', 0, cmdline_set_ovsdb_path, 1, "OVSDB unix domain socket file (default: "DEFAULT_OVSDB_SOCK")" },
	{ "zmq-config-ep", 'Y', 0, cmdline_set_zmq_config_ep, 2, "Use ZeroMQ IPC on the given endpoint to respond to configuration queries (default: "DEFAULT_ZMQ_CONFIG_EP")" },
	{ "zmq-port-control-ep", 'Z', 0, cmdline_set_zmq_port_control_ep, 2, "Use ZeroMQ IPC on the given endpoint to add/remove VF&virtio instead of OVSDB (default: "DEFAULT_ZMQ_PORT_CONTROL_EP")" },
	{ "zmq-stats-ep", 'z', 0, cmdline_set_zmq_stats_ep, 2, "Use ZeroMQ IPC on the given endpoint to report stats instead of PTY (default: "DEFAULT_ZMQ_STATS_EP")" },
	{ "zmq-core-sched-ep", 's', 0, cmdline_set_zmq_core_sched_ep, 2, "Use ZeroMQ IPC on the given endpoint to enable CPU load balancing (default: "DEFAULT_ZMQ_CORE_SCHED_EP")" },
	{ "ipc", 'I', 0, cmdline_set_ipc, 0, "Use IPC to add/remove VF&virtio instead of OVSDB (default: use OVSDB monitoring)" },
	{ "vhost-username", 'u', 0, cmdline_set_vhost_username, 2, "vhost-user unix socket ownership username, omit value to inherit process username (default: "DEFAULT_VHOSTUSER_USERNAME")" },
	{ "vhost-groupname", 'g', 0, cmdline_set_vhost_groupname, 2, "vhost-user unix socket ownership groupname, omit value to inherit process groupname (default: "DEFAULT_VHOSTUSER_GROUPNAME")" },
	{ "vhost-path", 'V', 0, cmdline_set_vhost_path, 1, "vhost-user unix socket directory path (default: "DEFAULT_VHOSTUSER_PATH")" },
	{ "vhost-socket", 'S', 0, cmdline_set_vhost_socket, 1, "vhost-user unix socket file name, must contain exactly one %u to denote VirtIO ID (default: "DEFAULT_VHOSTUSER_SOCKNAME")" },
	{ "virtio-cpu", 'c', 0, cmdline_set_vf_cpus, 1, "Semicolon-delimited list of '<virtio>:<cpu>[,<cpu>]' strings specifying which CPU(s) to use for the specified virtio IDs. Can be specified more than once." },
	{ "enable-jumbo", 'J', 0, cmdline_enable_jumbo, 0, "Enable jumbo frame support for the relay (increases hugepage memory requirement)" },
	{ "enable-mrgbuf", 'R', 0, cmdline_enable_mrgbuf, 0, "Enable virtio RX buffer merging (can impact small packet performance)" },
	{ "add-pci-vf", 'P', 0, cmdline_add_static_vf, 1, "Add a static VF, <PCI>=<virtio_id>, e.g. 0000:05:08.1=1" },
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	{ "vhostuser-client", 'i', 0, cmdline_enable_vhostclient, 0, "Use vhostuser in client mode (default: server mode)" },
#endif
#if RTE_VERSION >= RTE_VERSION_NUM(16,11,0,0)
	{ "zero-copy", '0', 0, cmdline_enable_zerocopy, 0, "Use experimental zero-copy support (VM to NIC) (default: disabled)" },
#endif
	{ "enable-tso", 'T', 0, cmdline_enable_tso, 0, "Enable TCP Segmentation Offload (increases hugepage memory requirement, default: disabled)" },
#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
	{ "dynamic-sockets", 'd', 0, cmdline_enable_dynamic_sockets, 0, "Connect to sockets dynamically instead of creating the default sockets (default: disabled)" },
#endif
	{ "same-numa", 'a', 0, cmdline_enable_same_numa, 0, "No longer reserve hugapage on all numa, just reserve hugapage on numa that been used (default: disable)" },
	{ "version", 'v', CMDLINE_PARAM_FLAG_TERMINATE, cmdline_show_version, 0, "Show version number and exit" },
	{ 0, 0, 0, 0, 0, "\n\nVirtio-forwarder daemon: forward packets between SR-IOV VFs (serviced by DPDK) and VirtIO network backend.\n" }
};

int main(int argc, char *argv[])
{
	struct zmq_service *config_service = NULL;
	struct zmq_server  *config_server  = NULL;
	struct zmq_service *stats_service = NULL;
	struct zmq_server  *stats_server  = NULL;
	struct zmq_service *port_control_service = NULL;
	struct zmq_server  *port_control_server  = NULL;
	struct zmq_service *core_sched_service = NULL;
	struct zmq_server  *core_sched_server  = NULL;
	int core_sched_shutdown_rc = 0;
	int vf_shutdown_rc = 0;
	int stats_shutdown_rc = 0;
	int config_shutdown_rc = 0;

	sprintf(pidpath, DEFAULT_PID_PATH);

	dpdk_cfg.log_level = DEFAULT_LOG_LEVEL;
	dpdk_cfg.master_lcore = DEFAULT_MASTER_LCORE;
	snprintf(dpdk_cfg.huge_dir, 32, DEFAULT_HUGE_PATH);

	snprintf(ovsdb_cfg.ovsdb_sock_path, 128, DEFAULT_OVSDB_SOCK);

	snprintf(zmq_config_ep, sizeof zmq_config_ep, DEFAULT_ZMQ_CONFIG_EP);
	snprintf(zmq_port_control_ep, sizeof zmq_port_control_ep,
			DEFAULT_ZMQ_PORT_CONTROL_EP);
	snprintf(zmq_stats_ep, sizeof zmq_stats_ep, DEFAULT_ZMQ_STATS_EP);
	snprintf(zmq_core_sched_ep, sizeof zmq_core_sched_ep,
			DEFAULT_ZMQ_CORE_SCHED_EP);

	snprintf(vhost_conf.vhost_username, 32, DEFAULT_VHOSTUSER_USERNAME);
	snprintf(vhost_conf.vhost_groupname, 32, DEFAULT_VHOSTUSER_GROUPNAME);
	snprintf(vhost_conf.socket_path, 128, DEFAULT_VHOSTUSER_PATH);
	snprintf(vhost_conf.socket_name, 32, "%s", DEFAULT_VHOSTUSER_SOCKNAME);
	for (int i=0; i<MAX_RELAYS; ++i) {
		vhost_conf.relay_cpus[i].vf2vio_cpu = -1;
		vhost_conf.relay_cpus[i].vio2vf_cpu = -1;
	}

	if (cmdline_parser(opts, 0, argc, argv, 0) != 0) {
		exit(1);
	}

	snprintf(pidfile, 128, "%s/%s.pid", pidpath, daemonname);

	if (show_version) {
		fprintf(stdout, "Virtio-forwarder version %s (%s)\n",
			virtio_forwarder_version(), rte_version());
		exit(0);
	}

	if (log_syslog)
		open_log(daemonname, LOG_OPT_SYSLOG);
	else
		open_log(daemonname, LOG_OPT_STDOUT | LOG_OPT_DETAIL |
			LOG_OPT_TIMESTAMP);

	for (int i=0; i<MAX_RELAYS; ++i) {
		int cpu = vhost_conf.relay_cpus[i].vf2vio_cpu;
		if (cpu == -1)
			continue;

		if (((1ULL<<cpu) & vhost_conf.worker_core_bitmap) == 0) {
			log_error("Invalid CPU %u specified for virtio %u (not in CPU worker list)!",
				cpu, i);
			exit(1);
		}
		cpu = vhost_conf.relay_cpus[i].vio2vf_cpu;
		if (((1ULL<<cpu) & vhost_conf.worker_core_bitmap) == 0) {
			log_error("Invalid CPU %u specified for virtio %u (not in CPU worker list)!",
				cpu, i);
			exit(1);
		}
	}

	if (daemonize) {
		if (daemon(1, 0) != 0) {
			log_critical("Could not daemonize: %s", strerror(errno));
			return 1;
		}
	}
	if (create_daemon_file(pidfile, &remove_pidfile) != 0) {
		log_critical("Could not create PID file: %m");
		return 1;
	}

	if (configure_signals()) {
		log_critical("Could not configure signals: %s", strerror(errno));
		return 1;
	}

	log_info("Starting %s %s", daemonname, virtio_forwarder_version());

	if (nice(-20) != -20)
		log_warning("Could not set process nice value to -20!");

	/* Initialize all threads here. */
	dpdk_cfg.core_bitmap = vhost_conf.worker_core_bitmap;
	if (dpdk_eal_initialize(&dpdk_cfg) != 0) {
		log_critical("Error with EAL initialization!");
		exit(-1);
	}
	if (virtio_vhostuser_start(&vhost_conf, create_vhostuser_sockets) != 0) {
		log_critical("Error with vhostuser initialization!");
		exit(-1);
	}

	/* Start the configuration query service. */
	if (use_zmq_config) {
		config_service = zmq_config_service_alloc(&vhost_conf);
		if (!config_service) {
			log_critical("Exiting: Error creating config service");
			exit(-1);
		}
		config_server = zmq_server_start(zmq_config_ep, config_service);
		if (!config_server) {
			log_critical("Exiting: Error starting ZeroMQ config server");
			exit(-1);
		}
	}

	/* Start the stats service. */
	if (use_zmq_stats) {
		stats_service = zmq_stats_service_alloc();
		if (!stats_service) {
			log_critical("Exiting: Error creating stats service");
			exit(-1);
		}
		stats_server = zmq_server_start(zmq_stats_ep, stats_service);
		if (!stats_server) {
			log_critical("Exiting: Error starting ZeroMQ stats server");
			exit(-1);
		}
	}

	/* Start the VF addition/removal service. */
	if (use_zmq_port_control) {
		port_control_service = zmq_port_control_service_alloc();
		if (!port_control_service) {
			log_critical("Exiting: Error creating port control service");
			exit(-1);
		}
		port_control_server = zmq_server_start(
			zmq_port_control_ep, port_control_service
		);
		if (!port_control_server) {
			log_critical("Exiting: Error starting ZeroMQ port control server");
			exit(-1);
		}
	} else if (use_ipc) {
		if (file_mon_start() != 0) {
			log_critical("Error with IPC initialization!");
			exit(-1);
		}
	} else {
		if (ovsdb_mon_start(&ovsdb_cfg) != 0) {
			log_critical("Error with OVSDB monitor initialization!");
			exit(-1);
		}
	}

	/* Start the core scheduler service. */
	if (use_zmq_core_sched) {
		core_sched_service = zmq_core_sched_service_alloc();
		if (!core_sched_service) {
			log_critical("Exiting: Error creating core scheduler service");
			exit(-1);
		}
		core_sched_server = zmq_server_start(
			zmq_core_sched_ep, core_sched_service
		);
		if (!core_sched_server) {
			log_critical("Exiting: Error starting ZeroMQ core scheduler server");
			exit(-1);
		}
	}

	prctl(PR_SET_NAME, daemonname, 0, 0, 0);

	running = true;
	while (running) {
		usleep(100000);
		if (must_stop) {
			running = false;
		}
	}

	/*
	 * Stop all threads here.
	 */

	/* Stop the core schedular service. */
	if (use_zmq_core_sched) {
		if (core_sched_server) {
			core_sched_shutdown_rc = zmq_server_stop(core_sched_server);
			core_sched_server = NULL;
		}
	}

	/* Stop the VF addition/removal service. */
	if (use_zmq_port_control) {
		if (port_control_server) {
			vf_shutdown_rc = zmq_server_stop(port_control_server);
			port_control_server = NULL;
		}
	} else if (use_ipc) {
		file_mon_stop();
	} else {
		ovsdb_mon_stop();
	}

	/* Stop the stats service. */
	if (use_zmq_stats) {
		if (stats_server) {
			stats_shutdown_rc = zmq_server_stop(stats_server);
			stats_server = NULL;
		}
	}

	/* Stop the config service. */
	if (use_zmq_config) {
		if (config_server) {
			config_shutdown_rc = zmq_server_stop(config_server);
			config_server = NULL;
		}
	}

	virtio_vhostuser_stop();
	dpdk_eal_finalize();
	log_info("Stopping %s", daemonname);
	virtiofwd_shutdown();

	if (core_sched_shutdown_rc)
		return core_sched_shutdown_rc;
	else if (vf_shutdown_rc)
		return vf_shutdown_rc;
	else if (stats_shutdown_rc)
		return stats_shutdown_rc;
	else if (config_shutdown_rc)
		return config_shutdown_rc;
	else
		return 0;
}
