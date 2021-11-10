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

#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <bsd/string.h>
#include "virtio_vhostuser.h"
#define __MODULE__ "virtio_vhostuser"
#include "log.h"
#include "virtio_worker.h"
#include "ugid.h"
#include <rte_version.h>
#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
#include <rte_vhost.h>
#include <linux/virtio_net.h>
#else
#include <rte_virtio_net.h>
#endif

const char* const virtio_features[] = {
	"CSUM", // 0
	"GUEST_CSUM", //1
	"CTRL_GUEST_OFFLOADS", //2
	"3",//3
	"4",//4
	"MAC",//5
	"6",//6
	"GUEST_TSO4",//7
	"GUEST_TSO6",//8
	"GUEST_ECN",//9
	"GUEST_UFO",//10
	"HOST_TSO4",//11
	"HOST_TSO6",//12
	"HOST_ECN",//13
	"HOST_UFO",//14
	"MRG_RXBUF",//15
	"STATUS",//16
	"CTRL_VQ",//17
	"CTRL_RX",//18
	"CTRL_VLAN",//19
	"20",//20
	"GUEST_ANNOUNCE",//21
	"MQ",//22
	"CTRL_MAC_ADDR",//23
	"NOTIFY_ON_EMPTY",//24
	"25",//25
	"VHOST_LOG_ALL",//26
	"ANY_LAYOUT",//27
	"RING_INDIRECT_DESC",//28
	"29",//29
	"VHOST_USER_PROTOCOL_FEATURES",//30
	"31",//31
	"VIRTIO_1.0",//VERSION_1
};

const char* const vhost_features[] = {
	"MQ",//0
	"LOG_SHMFD",//1
	"RARP"
};


#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
static pthread_t vhostuser_thread;
#endif
static char vhost_socket_name_prefix[128];
static char vhost_socket_name_suffix[128];
static bool mk_default_sockets;
struct virtio_vhostuser_conf g_vio_worker_conf;
char *relay_ifname_map[MAX_RELAYS];

static const char
*virtio_vhostuser_id_to_name(unsigned id, char *name, size_t namesz)
{
	snprintf(name, namesz, "%s%u%s", vhost_socket_name_prefix, id,
			vhost_socket_name_suffix);

	return name;
}

/*
 * Expect name to const of vhost_socket_name_prefix followed by unsigned int id
 * followed by vhost_socket_name_suffix.
 * Return id or -1 on mismatch
 */
__attribute__ ((unused))
static int virtio_vhostuser_name_to_id(char *name)
{
	unsigned namelen = strlen(name);
	unsigned prefixlen = strlen(vhost_socket_name_prefix);
	unsigned suffixlen = strlen(vhost_socket_name_suffix);
	int spn;
	unsigned id;

	if (namelen < (prefixlen + suffixlen + 1))
		return -1;
	if (strncmp(name, vhost_socket_name_prefix, prefixlen) != 0)
		return -1;
	if (suffixlen && strcmp(name + namelen - suffixlen,
			vhost_socket_name_suffix) != 0)
		return -1;

	spn = strspn(name + prefixlen, "0123456789");
	if (namelen < prefixlen + spn)
		return -1;
	id = atoi(name + prefixlen);
	if (id < MAX_RELAYS)
		return id;

	return -1;
}

static int get_relay_for_sock(const char *vhost_path)
{
	for (int i=0; i<MAX_RELAYS; ++i) {
		if (relay_ifname_map[i] &&
				strcmp(relay_ifname_map[i], vhost_path) == 0)
			return i;
	}

	return -1;
}

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
static int virtio_vhostuser_new_device_cb(int vid)
{
	int id;
	char ifname[128];
#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	unsigned num_vring_pairs = rte_vhost_get_queue_num(vid);
#else
	unsigned num_vring_pairs = rte_vhost_get_vring_num(vid)/2;
#endif

	rte_vhost_get_ifname(vid, ifname, 128);
	log_info("new virtio device on '%s' (vid=%d)", ifname, vid);
	log_debug("Number of queue pairs=%u\n", num_vring_pairs);

	/* disable notifications. */
	for (unsigned q=0; q<num_vring_pairs; ++q) {
		rte_vhost_enable_guest_notification(vid, q*2, 0); /* RX queue */
		rte_vhost_enable_guest_notification(vid, q*2+1, 0); /* TX queue */
	}

	id = get_relay_for_sock(ifname);
	if (id < 0) {
		log_error("Could not find relay for %s", ifname);
		return -1;
	}

	if (virtio_forwarder_add_virtio(vid, id) == 0)
		return 0;
	else
		log_warning("Could not add virtio worker for '%s'", ifname);

	return -1;
}

static void virtio_vhostuser_destroy_device_cb(int vid)
{
	char str[128];
	int id;

	rte_vhost_get_ifname(vid, str, 128);
	log_info("destroy virtio device on '%s'", str);
	id = get_relay_for_sock(str);
	if (id < 0) {
		log_error("Invalid relay id found for vhostuser fd '%s'", str);
		return;
	}
	virtio_forwarder_remove_virtio(id);
}

static int
virtio_vhostuser_vring_state_change_cb(int vid, uint16_t queue_id, int enable)
{
	char ifname[128];
	int id;

	rte_vhost_get_ifname(vid, ifname, 128);
	id = get_relay_for_sock(ifname);
	if (id < 0) {
		log_error("Invalid relay id found for vhostuser fd '%s'", ifname);
		return -1;
	}
	log_debug("vring state change on queue_id=%hu (enable=%d) on relay %d ('%s')",
		queue_id, enable, id, ifname);
	virtio_forwarder_vring_state_change(id, queue_id, enable);

	return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
static int virtio_vhostuser_features_changed_cb(int vid, uint64_t features)
{
	char ifname[128];
	int id;

	rte_vhost_get_ifname(vid, ifname, 128);
	id = get_relay_for_sock(ifname);
	if (id < 0) {
		log_error("Invalid relay id found for vhostuser fd '%s'", ifname);
		return -1;
	}
	log_debug("Feature change on relay %d ('%s'). New features=%lu", id,
		ifname, features);

	/* From the docs:
	 * >VHOST_F_LOG_ALL will be set/cleared at the start/end of live
	 * >migration, respectively.
	 *
	 * RTE_VHOST_NEED_LOG() checks for VHOST_F_LOG_ALL.
	 * This check ensures that we do not call rte_vhost_avail_entries
	 * (virtio_worker.c, dpdk_rx) too late during the migration procedure. A
	 * segmentation fault results if we do: Note that this may actually be a
	 * DPDK issue, but for now we workaround it using the lm_pending flag.
	 */
	if (RTE_VHOST_NEED_LOG(features)) {
		log_info("VHOST_F_LOG_ALL is set. Live migration is being initiated on virtio %d",
			id);
		virtio_set_lm_pending(id);
	}

	return 0;
}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(17,11,0,0)
static int virtio_vhostuser_new_connection_cb(int vid)
{
	char ifname[128];

	rte_vhost_get_ifname(vid, ifname, 128);
	log_debug("new virtio connection on '%s' (vid=%d)", ifname, vid);

	return 0;
}

static void virtio_vhostuser_destroy_connection_cb(int vid)
{
	char ifname[128];

	rte_vhost_get_ifname(vid, ifname, 128);
	log_debug("destroy virtio connection on '%s'", ifname);

	return;
}
#endif
#else
static int virtio_vhostuser_new_device_cb(struct virtio_net *dev)
{
	int id;

	log_info("new virtio device on '%s': features=0x%08llX,protocol_features=0x%08llX,virt_qp_nb=%u,device_fh=%"PRIu64",q0_len=%u,q1_len=%u",
		dev->ifname, (long long)dev->features,
		(long long)dev->protocol_features, dev->virt_qp_nb,
		dev->device_fh, dev->virtqueue[0]->size, dev->virtqueue[1]->size);

	if (get_log_level() >= LOG_LVL_DEBUG) {
		char buf[512];
		unsigned i;
		unsigned l=0;
		buf[0]=0;
		for (i=0; i<=(sizeof(virtio_features) / sizeof(*virtio_features)); ++i) {
			if (dev->features & (1ULL<<i))
				l+=snprintf(buf+l, 512-l, "%s,", virtio_features[i]);
		}
		if (l) {
			buf[l-1]=0;
			log_debug("VirtIO features: %s", buf);
		}
		buf[0]=0;
		l=0;
		for (i=0; i<=(sizeof(vhost_features) / sizeof(*vhost_features)); ++i) {
			if (dev->protocol_features & (1ULL<<i))
				l+=snprintf(buf+l, 512-l, "%s,", vhost_features[i]);
		}
		if (l) {
			buf[l-1]=0;
			log_debug("Vhost-user features: %s", buf);
		}
		for (i=0; i<=1; ++i)
			log_debug("virtqueue %u: size=%u, last_used_idx=%hu, avail_idx=%hu, used_idx=%hu",
				i, dev->virtqueue[i]->size,
				dev->virtqueue[i]->last_used_idx,
				dev->virtqueue[i]->avail->idx,
				dev->virtqueue[i]->used->idx);
	}
	// disable notifications
	for (unsigned q=0; q<dev->virt_qp_nb; ++q) {
		rte_vhost_enable_guest_notification(dev, q*2, 0); // RX queue
		rte_vhost_enable_guest_notification(dev, q*2+1, 0); // TX queue
	}

	if ((id = virtio_vhostuser_name_to_id(dev->ifname)) >= 0
			&& virtio_forwarder_add_virtio(dev, id) == 0) {
		dev->flags |= VIRTIO_DEV_RUNNING;
		return 0;
	}
	log_warning("Could not add virtio worker for '%s'", dev->ifname);

	return -1;
}

static void virtio_vhostuser_destroy_device_cb(volatile struct virtio_net *dev)
{
	char str[128];
	int i=0;
	while (i<127) {
		str[i]=dev->ifname[i];
		if (str[i]==0)
			break;
		++i;
	}
	str[127]=0;
	int id = virtio_vhostuser_name_to_id(str);
	log_info("destroy virtio device on '%s'", str);
	dev->flags &= ~VIRTIO_DEV_RUNNING;
	if (id<0) {
		log_error("Invalid id found in vhostuser fd name '%s'", dev->ifname);
		return;
	}
	virtio_forwarder_remove_virtio(id);
}

static int
virtio_vhostuser_vring_state_change_cb(struct virtio_net *dev,
			uint16_t queue_id, int enable)
{
	char str[128];
	int i=0;
	while (i<127) {
		str[i]=dev->ifname[i];
		if (str[i]==0)
			break;
		++i;
	}
	str[127]=0;
	int id = virtio_vhostuser_name_to_id(str);
	log_debug("vring state change on queue_id=%hu (enable=%d) on '%s'", queue_id, enable, str);
	if (id<0) {
		log_error("Invalid id found in vhostuser fd name '%s'", str);
		return -1;
	}
	virtio_forwarder_vring_state_change(id, queue_id, enable);
	return 0;
}
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
static const struct vhost_device_ops virtio_vhostuser_ops = {
	.features_changed = virtio_vhostuser_features_changed_cb,
#else
static const struct virtio_net_device_ops virtio_vhostuser_ops = {
#endif
	.new_device =  virtio_vhostuser_new_device_cb,
	.destroy_device = virtio_vhostuser_destroy_device_cb,
	.vring_state_changed = virtio_vhostuser_vring_state_change_cb,
#if RTE_VERSION >= RTE_VERSION_NUM(17,11,0,0)
	.new_connection = virtio_vhostuser_new_connection_cb,
	.destroy_connection = virtio_vhostuser_destroy_connection_cb,
#endif
};

#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
static void* virtio_vhostuser_threadmain(void *arg __attribute__((unused)))
{
	log_debug("Starting vhostuser thread");
	rte_vhost_driver_session_start();
	log_debug("vhostuser thread ending");

	return 0;
}
#endif

static int prep_default_sockets(void)
{
	struct stat fstat;
	bool created_path = false;
	char *s;
	int len;
	uid_t uid, gid;
	const struct virtio_vhostuser_conf *conf = &g_vio_worker_conf;

	/* Check if the provided directory exists and attempt autcreate if not. */
	if (stat(conf->socket_path, &fstat) == 0) {
		if (!S_ISDIR(fstat.st_mode)) {
			log_error("Provided vhost-user path ('%s') is not a valid path",
				conf->socket_path);
			return 1;
		}
	} else {
		// try to autocreate
		log_warning("Could not access provided vhost-user socket path '%s' (%m), trying to create",
			conf->socket_path);
		if (mkdir(conf->socket_path, 0777)!=0) {
			log_error("Error creating provided vhost-user socket path '%s': %m",
			conf->socket_path);
			return 1;
		}
		created_path = true;
	}

	/* Check if the template name is valid. */
	if ((s=strstr(conf->socket_name, "%u")) == 0) {
		log_error("Mandatory single '%%u' not found in provided vhost-user socket name '%s'",
			conf->socket_name);
		return 1;
	} else {
		if (strstr(s+1, "%u") != 0) {
			log_error("More than one occurrence of '%%u' found in provided vhost-user socket name '%s'",
				conf->socket_name);
			return 1;
		}
	}
	if (strchr(conf->socket_name, '/')) {
		log_error("Invalid '/' found in vhost-user socket name '%s'",
			conf->socket_name);
		return 1;
	}

	/* Name seems valid. */
	strlcpy(vhost_socket_name_suffix, conf->socket_name, 128); /* temp copy of const conf string */
	s = strstr(vhost_socket_name_suffix, "%u");
	len = snprintf(vhost_socket_name_prefix, 128, "%s/", conf->socket_path);
	*s=0;
	snprintf(vhost_socket_name_prefix+len, 128-len, "%s", vhost_socket_name_suffix); /* append up to %u */
	if (*(s+2)) {// if something more after %u
		strlcpy(vhost_socket_name_suffix, s+2, 128);
		if (vhost_socket_name_suffix[0] >= '0' &&
				vhost_socket_name_suffix[0] <= '9') {
			log_error("vhost-user socket name '%s' may not contain a number after '%%u'!'",
				conf->socket_name);
			return 1;
		}
	} else
		vhost_socket_name_suffix[0] = 0;

	/* Change ownership of vhost folder if we created it. */
	uid = getuid();
	gid = getgid();
	if (conf->vhost_username[0] && get_uid(conf->vhost_username, &uid) != 0) {
		log_error("Could not get uid for '%s'!", conf->vhost_username);
		return -1;
	}
	if (conf->vhost_groupname[0] && get_gid(conf->vhost_groupname, &gid) != 0) {
		log_error("Could not get gid for '%s'!", conf->vhost_groupname);
		return -1;
	}
	if ((uid != getuid() || gid != getgid()) && created_path) {
		if (chown(conf->socket_path, uid, gid) != 0) {
			log_warning("Could not set ownership of socket path '%s'!",
				conf->socket_path);
		}
	}
	log_debug("Using vhost-user prefix '%s' and suffix '%s', with uid=%u and gid=%u",
		vhost_socket_name_prefix, vhost_socket_name_suffix, uid, gid);

	return 0;
}

static int register_relay_socket(const char *vhost_path, unsigned relay_id)
{
	struct stat statbuf;
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	unsigned flags = 0;
#endif
	uid_t uid, gid;
	const struct virtio_vhostuser_conf *conf = &g_vio_worker_conf;

	log_debug("Got register_relay_socket(%s, %u)", vhost_path, relay_id);

	/* Check for pre-existing sockets if server. */
	if (conf->vhost_client == 0 && stat(vhost_path, &statbuf) == 0) {
		/* virtio-forwarder should not get this far if the
		 * daemon is already running (see pidfile check in
		 * virtio_forwarder_main.c). */
		if (S_ISSOCK(statbuf.st_mode)) {
			if (unlink(vhost_path) != 0) {
				log_warning("Found stale unix socket handle '%s', could not delete!",
					vhost_path);
				return -1;
			}
			log_warning("Deleted stale unix socket handle '%s'",
				vhost_path);
		} else {
			log_error("'%s' unexpectedly already exists!", vhost_path);
			return -1;
		}
	}

	if (relay_ifname_map[relay_id]) {
		log_error("Cannot register %s! Socket %s is already assigned to relay %u",
			vhost_path, relay_ifname_map[relay_id], relay_id);
		return -1;
	}

#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	if (conf->vhost_client)
		flags |= RTE_VHOST_USER_CLIENT;
#endif
#if (RTE_VERSION >= RTE_VERSION_NUM(16,11,0,0)) && (RTE_VERSION < RTE_VERSION_NUM(20,11,0,0))
	if (conf->zerocopy)
		flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;
#endif
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	if (rte_vhost_driver_register(vhost_path, flags) != 0) {
#else
	if (rte_vhost_driver_register(vhost_path) != 0) {
#endif
		log_error("Error starting vhost user instance '%s'!", vhost_path);
		return -1;
	}

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
	/* Set features. */
	if (conf->use_rx_mrgbuf == 0) {
		if (rte_vhost_driver_disable_features(
				vhost_path, 1ULL << VIRTIO_NET_F_MRG_RXBUF))
			log_warning("Failed to disabe mergeable RX buffers on %s",
				vhost_path);
	}
	if (rte_vhost_driver_disable_features(
			vhost_path, 1ULL << VIRTIO_NET_F_CTRL_RX))
		log_warning("Failed to disable RX control commands on %s",
			vhost_path);
	if (conf->enable_tso == 0) {
		int disable_tso = 0;
		disable_tso |= rte_vhost_driver_disable_features(
					vhost_path, 1ULL << VIRTIO_NET_F_HOST_TSO4);
		disable_tso |= rte_vhost_driver_disable_features(
					vhost_path, 1ULL << VIRTIO_NET_F_HOST_TSO6);
		if (disable_tso)
			log_warning("Failed to disable TSO on %s", vhost_path);
	}

	/* Start vhost diver for socket. */
	rte_vhost_driver_callback_register(vhost_path, &virtio_vhostuser_ops);
	if (rte_vhost_driver_start(vhost_path)) {
		log_critical("Error starting vhost driver on socket %s!",
			vhost_path);
		return -1;
	}
#endif
	relay_ifname_map[relay_id] = (char *)malloc(strlen(vhost_path)+1);
	strcpy(relay_ifname_map[relay_id], vhost_path);

	/* Change ownership of the socket files. */
	uid = getuid();
	gid = getgid();
	get_uid(conf->vhost_username, &uid);
	get_gid(conf->vhost_groupname, &gid);
	if (conf->vhost_client == 0 && (uid != getuid() || gid != getgid())) {
		if (chown(vhost_path, uid, gid) != 0) {
			log_error("Could not change ownership of '%s' (%m)!",
				vhost_path);
			return -1;
		}
	}

	return 0;
}

static int deregister_socket(const char *vhost_path)
{
	int err, id;

	log_debug("Got deregister_socket(%s)", vhost_path);

	err = rte_vhost_driver_unregister(vhost_path);
	if (err != 0)
		log_warning("rte_vhost_driver_unregister(%s) failed with error %d",
			vhost_path, err);

	id = get_relay_for_sock(vhost_path);
	if (id < 0) {
		log_error("Found invalid relay id=%d during %s deregistration",
			id, vhost_path);
		return 1;
	}
	if (relay_ifname_map[id]) {
		free(relay_ifname_map[id]);
		relay_ifname_map[id] = NULL;
	} else {
		log_error("Encountered unexpected NULL pointer relay_ifname_map[%d]",
			id);
		return 2;
	}

	return err;
}

int virtio_vhostuser_start(const struct virtio_vhostuser_conf *conf,
			bool create_sockets)
{
	int err;

	mk_default_sockets = create_sockets;
#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	/* TODO: Investigate dynamic socket support for old DPDKs. */
	if (!mk_default_sockets) {
		log_warning("Dynamic sockets not supported for this version of DPDK!");
		mk_default_sockets = true;
	}
#endif

	memset(relay_ifname_map, 0, sizeof(relay_ifname_map));
	g_vio_worker_conf = *conf;

	if (mk_default_sockets) {
		log_debug("Creating default sockets for %d relays", MAX_RELAYS);
		err = prep_default_sockets();
		if (err)
			return err;
	} else {
		log_debug("Using dynamic socket connection API");
	}

	log_debug("Initializing virtio workers");
	if (virtio_forwarders_initialize() != 0) {
		log_error("Error starting worker threads!");
		return -1;
	}

	log_debug("Registering vhost driver");
#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	if (conf->use_rx_mrgbuf == 0)
		/* Enabling RX buffer merging can cause 50% performance
		 * degradation with DPDK <=16.07, and 20% degradation with DPDK 16.11! */
		rte_vhost_feature_disable((1ULL << VIRTIO_NET_F_MRG_RXBUF));

	/* VIRTIO_NET_F_CTRL_RX Requires VIRTIO_NET_F_CTRL_VQ. */
	rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_CTRL_RX);
	if (conf->enable_tso == 0) {
		rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_HOST_TSO4);
		rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_HOST_TSO6);
	}
#endif

	if (mk_default_sockets) {
		for (int id=0; id<MAX_RELAYS; ++id) {
			char buf[128 + 10 + 128];

			virtio_vhostuser_id_to_name(id, buf, 266);
			err = register_relay_socket(buf, id);
			if (err)
				return err;

		}
	}

#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	rte_vhost_driver_callback_register(&virtio_vhostuser_ops);
	log_debug("Creating vhostuser thread");
	pthread_create(&vhostuser_thread, 0, virtio_vhostuser_threadmain, 0);
#endif
	return 0;
}

void virtio_vhostuser_stop(void) {
	virtio_forwarders_remove_all();
	log_debug("Stopping vhostuser thread");
	for (int id=0; id<MAX_RELAYS; ++id) {
		if (relay_ifname_map[id])
			deregister_socket(relay_ifname_map[id]);
	}

#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	struct timespec ts;
	struct timeval tv;
	gettimeofday(&tv, 0);
	tv.tv_sec+=1;
	ts.tv_sec=tv.tv_sec;
	ts.tv_nsec=tv.tv_usec*1000;
	if (pthread_timedjoin_np(vhostuser_thread, 0, &ts) != 0) {
		log_debug("Timeout waiting for vhostuser_thread, cancelling thread...");
		pthread_cancel(vhostuser_thread);
	}
	pthread_join(vhostuser_thread, 0);
#endif
	virtio_forwarders_shutdown();
	log_debug("Stopped vhostuser thread");
}

/* API for dynamic socket operations. */
int virtio_add_sock_dev_pair(const char *vhost_path,
			char slave_dbdfs[MAX_NUM_BOND_SLAVES][RTE_ETH_NAME_MAX_LEN],
			unsigned num_slaves, char *name, uint8_t mode,
			bool conditional)
{
	int err;
	int relay_id;
	char *dev;
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	dpdk_port_t port_id;
#endif

	if (!vhost_path) {
		log_error("vhost-path has to be specified when adding a socket pair");
		return -1;
	}

	dev = num_slaves == 1 ? slave_dbdfs[0] : name;
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
	if (rte_eth_dev_get_port_by_name(dev, &port_id) == 0) {
		log_error("Device %s is already attached to virtio-forwarder",
			dev);
		return -EEXIST;
	}
#endif

	for (int i=0; i<MAX_RELAYS; ++i) {
		if (relay_ifname_map[i]) {
			if (strcmp(relay_ifname_map[i], vhost_path) == 0) {
				log_error("Socket %s is already registered with relay %d",
					vhost_path, i);
				return 1;
			}
		}
	}

	relay_id = virtio_get_free_relay_id(relay_ifname_map);
	if (relay_id < 0) {
		log_error("Could not get idle relay for packet processing!");
		return 2;
	}

	/* Register VF or bond. */
	if (num_slaves == 1) {
		err = virtio_forwarder_add_vf2(dev, relay_id, conditional);
		if (err != 0) {
			log_error("virtio_forwarder_add_vf2(%s, %d, %s) failed with error %d",
				dev, relay_id, conditional ? "true" : "false",
				err);
			return 3;
		}
	} else {
		err = virtio_forwarder_bond_add(slave_dbdfs, num_slaves, name,
						mode, relay_id);
		if (err != 0) {
			log_error("virtio_forwarder_bond_add(<PCI addresses>, %u, %s, %u, %d) failed with error %d",
				num_slaves, name, mode, relay_id, err);
			return 4;
		}
	}

	/* Register socket. */
	err = register_relay_socket(vhost_path, relay_id);
	if (err != 0) {
		log_error("register_relay_socket failed with error %d", err);
		goto exit_deconfigure_pair;
	}

	return err;

exit_deconfigure_pair:
	err = virtio_forwarder_remove_vf2(dev, relay_id, conditional);
	if (err)
		log_warning("During error recovery, virtio_forwarder_remove_vf2 failed with error %d",
			err);

	err = deregister_socket(vhost_path);
	if (err)
		log_warning("During error recovery, deregister_socket failed with error %d",
			err);

	return 5;
}

int virtio_remove_sock_dev_pair(const char *vhost_path, char *dev,
			bool conditional)
{
	int err;
	int relay_id = -1;
	vio_vf_relay_t *relay;
	const char *path;

	if (!vhost_path && strlen(dev) == 0) {
		log_error("Either pci-addr or vhost-path parameter should be passed!");
		return -1;
	}

	/* If no vhost-path is passed, attempt to get the vhost_path
	   and relay_id given the PCI address passed to the function
	*/
	if (vhost_path) {
		relay_id = get_relay_for_sock(vhost_path);
		path = vhost_path;
	} else {
		for (int i=0; i<MAX_RELAYS; ++i) {
			relay = get_relay_from_id(i);
			if (relay && !strcmp(relay->dpdk.pci_dbdf, dev)) {
				path = relay_ifname_map[i];
				relay_id = i;
				break;
			}
		}
	}

	if (relay_id < 0) {
		log_error("Could not get relay id!");
		return 1;
	}

	/* If no PCI address is passed to this function, target the PCI device
	 * associated with this relay. Since there is a one-to-one mapping between
	 * relay, VF and vhost-user socket this is safe to do. If multi-vhost
	 * support is required (virtio-forwarder multiplex) then this
	 * assumption will no longer be valid.
	 */
	if (strlen(dev) == 0) {
		relay = get_relay_from_id(relay_id);
		if (relay == NULL) {
			log_error("Cloud not find relay for relay_id: %d", relay_id);
			return 1;
		}
		strcpy(dev, relay->dpdk.pci_dbdf);
		log_warning("PCI DBDF not passed. Assuming device to be removed is: %s",
			    dev);
	} else {
		/* Check that the device <-> relay pairing is correct. */
		if (!virtio_relay_has_device(relay_id, dev)) {
			log_error("The relay instance for socket %s has no device named %s. Not removing pair",
				   path, dev);
			return 2;
		}
	}

	log_debug("Got virtio_remove_sock_dev_pair(%s, %s, %s)", path,
		dev, conditional ? "true" : "false");

	/* Remove virtio: VM may or may not have shutdown at this point.
	 * Therefore, remove virtio manually to cater for case where the device
	 * destroy callback did not trigger yet. */
	virtio_forwarder_remove_virtio(relay_id);

	/* Deregister socket. */
	err = deregister_socket(path);
	if (err != 0) {
		log_error("deregister_socket(%s) failed with error %d",
			path, err);
		return 3;
	}

	/* Remove VF. */
	err = virtio_forwarder_remove_vf2(dev, relay_id, conditional);
	if (err != 0) {
		log_error("virtio_forwarder_remove_vf2(%s, %d, %s) failed with error %d",
			dev, relay_id, conditional ? "true" : "false",  err);
		return 4;
	}

	return err;
}
