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
	if (suffixlen && strcmp(name + namelen - suffixlen, vhost_socket_name_suffix) != 0)
		return -1;
	spn = strspn(name + prefixlen, "0123456789");
	if (namelen < prefixlen + spn)
		return -1;
	id = atoi(name + prefixlen);
	if (id < MAX_RELAYS)
		return id;

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
	log_info("new virtio device on '%s'", ifname);
	log_debug("Number of queue pairs=%u\n", num_vring_pairs);

	/* disable notifications. */
	for (unsigned q=0; q<num_vring_pairs; ++q) {
		rte_vhost_enable_guest_notification(vid, q*2, 0); /* RX queue */
		rte_vhost_enable_guest_notification(vid, q*2+1, 0); /* TX queue */
	}

	if ((id = virtio_vhostuser_name_to_id(ifname)) >= 0 &&
			virtio_forwarder_add_virtio(vid, id) == 0)
		return 0;
	log_warning("Could not add virtio worker for '%s'", ifname);

	return -1;
}

static void virtio_vhostuser_destroy_device_cb(int vid)
{
	char str[128];
	int id;

	rte_vhost_get_ifname(vid, str, 128);
	id = virtio_vhostuser_name_to_id(str);
	log_info("destroy virtio device on '%s'", str);
	if (id < 0) {
		log_error("Invalid id found in vhostuser fd name '%s'", str);
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
	id = virtio_vhostuser_name_to_id(ifname);
	if (id < 0) {
		log_error("Invalid id found in vhostuser fd name '%s'", ifname);
		return -1;
	}
	log_debug("vring state change on queue_id=%hu (enable=%d) on relay %d ('%s')", queue_id, enable, id, ifname);
	virtio_forwarder_vring_state_change(id, queue_id, enable);

	return 0;
}

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
static int virtio_vhostuser_features_changed_cb(int vid, uint64_t features)
{
	char ifname[128];
	int id;

	rte_vhost_get_ifname(vid, ifname, 128);
	id = virtio_vhostuser_name_to_id(ifname);
	if (id < 0) {
		log_error("Invalid id found in vhostuser fd name '%s'", ifname);
		return -1;
	}
	log_debug("Feature change on relay %d ('%s'). New features=%lu", id, ifname, features);

	return 0;
}
#endif
#else
static int virtio_vhostuser_new_device_cb(struct virtio_net *dev)
{
	int id;

	log_info("new virtio device on '%s': features=0x%08llX,protocol_features=0x%08llX,virt_qp_nb=%u,device_fh=%"PRIu64",q0_len=%u,q1_len=%u", dev->ifname, (long long)dev->features, (long long)dev->protocol_features, dev->virt_qp_nb, dev->device_fh, dev->virtqueue[0]->size, dev->virtqueue[1]->size);

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
			log_debug("virtqueue %u: size=%u, last_used_idx=%hu, avail_idx=%hu, used_idx=%hu", i, dev->virtqueue[i]->size, dev->virtqueue[i]->last_used_idx, dev->virtqueue[i]->avail->idx, dev->virtqueue[i]->used->idx);
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

static void virtio_vhostuser_destroy_device_cb(volatile struct virtio_net *dev) {
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

static int virtio_vhostuser_vring_state_change_cb(struct virtio_net *dev, uint16_t queue_id, int enable) {
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
	.vring_state_changed = virtio_vhostuser_vring_state_change_cb
};

#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
static void* virtio_vhostuser_threadmain(void *arg __attribute__((unused))) {
	log_debug("Starting vhostuser thread");
	rte_vhost_driver_session_start();
	log_debug("vhostuser thread ending");
	return 0;
}
#endif

int virtio_vhostuser_start(const struct virtio_vhostuser_conf *conf) {
	struct stat fstat;
	bool created_path=false;
	if (stat(conf->socket_path, &fstat) == 0) {
		if (!S_ISDIR(fstat.st_mode)) {
			log_error("Provided vhost-user path ('%s') is not a valid path", conf->socket_path);
			return 1;
		}
	} else {
		// try to autocreate
		log_warning("Could not access provided vhost-user socket path '%s' (%m), trying to create", conf->socket_path);
		if (mkdir(conf->socket_path, 0777)!=0) {
			log_error("Error creating provided vhost-user socket path '%s': %m", conf->socket_path);
			return 1;
		}
		created_path = true;
	}
	char *s;
	if ((s=strstr(conf->socket_name, "%u"))==0) {
		log_error("Mandatory single '%%u' not found in provided vhost-user socket name '%s'", conf->socket_name);
		return 1;
	} else {
		if (strstr(s+1, "%u")!=0) {
			log_error("More than one occurrence of '%%u' found in provided vhost-user socket name '%s'", conf->socket_name);
			return 1;
		}
	}
	if (strchr(conf->socket_name, '/')) {
		log_error("Invalid '/' found in vhost-user socket name '%s'", conf->socket_name);
		return 1;
	}
	strncpy(vhost_socket_name_suffix, conf->socket_name, 128); // temp copy of const conf string
	s=strstr(vhost_socket_name_suffix, "%u");
	int len=snprintf(vhost_socket_name_prefix, 128, "%s/", conf->socket_path);
	*s=0;
	snprintf(vhost_socket_name_prefix+len, 128-len, "%s", vhost_socket_name_suffix); // append up to %u
	if (*(s+2)) {// if something more after %u
		strncpy(vhost_socket_name_suffix, s+2, 128);
		if (vhost_socket_name_suffix[0]>='0' && vhost_socket_name_suffix[0]<='9') {
			log_error("vhost-user socket name '%s' may not contain a number after '%%u'!'", conf->socket_name);
			return 1;
		}
	} else
		vhost_socket_name_suffix[0] = 0;
	uid_t uid = getuid();
	gid_t gid = getgid();
	if (conf->vhost_username[0] && get_uid(conf->vhost_username, &uid) != 0) {
		log_error("Could not get uid for '%s'!", conf->vhost_username);
		return -1;
	}
	if (conf->vhost_groupname[0] && get_gid(conf->vhost_groupname, &gid) != 0) {
		log_error("Could not get gid for '%s'!", conf->vhost_groupname);
		return -1;
	}
	if ((uid!=getuid() || gid!=getgid()) && created_path) {
		if (chown(conf->socket_path, uid, gid) != 0) {
			log_warning("Could not set ownership of socket path '%s'!", conf->socket_path);
		}
	}

	log_debug("Using vhost-user prefix '%s' and suffix '%s', with uid=%u and gid=%u", vhost_socket_name_prefix, vhost_socket_name_suffix, uid, gid);
	log_debug("Initializing virtio workers");
	if (virtio_forwarders_initialize(conf) != 0) {
		log_error("Error starting worker threads!");
		return -1;
	}
	log_debug("Registering vhost driver");
	// DPDK 2.2 default features:
	/* #define VHOST_SUPPORTED_FEATURES ((1ULL << VIRTIO_NET_F_MRG_RXBUF) | \
				(1ULL << VIRTIO_NET_F_CTRL_VQ) | \
				(1ULL << VIRTIO_NET_F_CTRL_RX) | \
				(VHOST_SUPPORTS_MQ)			   | \
				(1ULL << VIRTIO_F_VERSION_1)   | \
				(1ULL << VHOST_F_LOG_ALL)	   | \
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES))
	*/
	// DPDK 16.04 default features:
	/* #define VHOST_SUPPORTED_FEATURES ((1ULL << VIRTIO_NET_F_MRG_RXBUF) | \
				(1ULL << VIRTIO_NET_F_CTRL_VQ) | \
				(1ULL << VIRTIO_NET_F_CTRL_RX) | \
				(1ULL << VIRTIO_NET_F_GUEST_ANNOUNCE) | \
				(VHOST_SUPPORTS_MQ)			   | \
				(1ULL << VIRTIO_F_VERSION_1)   | \
				(1ULL << VHOST_F_LOG_ALL)	   | \
				(1ULL << VHOST_USER_F_PROTOCOL_FEATURES) | \
				(1ULL << VIRTIO_NET_F_HOST_TSO4) | \
				(1ULL << VIRTIO_NET_F_HOST_TSO6) | \
				(1ULL << VIRTIO_NET_F_CSUM)    | \
				(1ULL << VIRTIO_NET_F_GUEST_CSUM) | \
				(1ULL << VIRTIO_NET_F_GUEST_TSO4) | \
				(1ULL << VIRTIO_NET_F_GUEST_TSO6))

	Virtio OASIS spec network device feature prerequisites:
		VIRTIO_NET_F_GUEST_TSO4 Requires VIRTIO_NET_F_GUEST_CSUM.
		VIRTIO_NET_F_GUEST_TSO6 Requires VIRTIO_NET_F_GUEST_CSUM.
		VIRTIO_NET_F_GUEST_ECN Requires VIRTIO_NET_F_GUEST_TSO4 or VIRTIO_NET_F_GUEST_TSO6.
		VIRTIO_NET_F_GUEST_UFO Requires VIRTIO_NET_F_GUEST_CSUM.
		VIRTIO_NET_F_HOST_TSO4 Requires VIRTIO_NET_F_CSUM.
		VIRTIO_NET_F_HOST_TSO6 Requires VIRTIO_NET_F_CSUM.
		VIRTIO_NET_F_HOST_ECN Requires VIRTIO_NET_F_HOST_TSO4 or VIRTIO_NET_F_HOST_TSO6.
		VIRTIO_NET_F_HOST_UFO Requires VIRTIO_NET_F_CSUM.
		VIRTIO_NET_F_CTRL_RX Requires VIRTIO_NET_F_CTRL_VQ.
		VIRTIO_NET_F_CTRL_VLAN Requires VIRTIO_NET_F_CTRL_VQ.
		VIRTIO_NET_F_GUEST_ANNOUNCE Requires VIRTIO_NET_F_CTRL_VQ.
		VIRTIO_NET_F_MQ Requires VIRTIO_NET_F_CTRL_VQ.
		VIRTIO_NET_F_CTRL_MAC_ADDR Requires VIRTIO_NET_F_CTRL_VQ.
	*/
	//rte_vhost_feature_disable(0xffffffff); // disable all default vhost features
	/*rte_vhost_feature_enable(
	  1ULL<<VIRTIO_NET_F_CTRL_VQ  // control virt queue
	| 1ULL<<VIRTIO_NET_F_GUEST_ANNOUNCE // Driver may send gratuitous ARP
	| 1ULL<<VIRTIO_F_VERSION_1 // VirtIO 1.0 supported
	| 1ULL<<VHOST_F_LOG_ALL // Log all dirty pages for VM live migration
	| 1ULL<<VHOST_USER_F_PROTOCOL_FEATURES
	);*/
#if RTE_VERSION < RTE_VERSION_NUM(17,5,0,0)
	if (conf->use_rx_mrgbuf == 0)
		rte_vhost_feature_disable((1ULL << VIRTIO_NET_F_MRG_RXBUF)); // enabling RX buffer merging can cause 50% performance degradation with DPDK <=16.07, and 20% degradation with DPDK 16.11!
	//rte_vhost_feature_disable((1ULL << VIRTIO_NET_F_CTRL_VQ));
	/* VIRTIO_NET_F_CTRL_RX Requires VIRTIO_NET_F_CTRL_VQ. */
	rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_CTRL_RX);
	//rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_MQ);
	if (conf->enable_tso == 0) {
		rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_HOST_TSO4);
		rte_vhost_feature_disable(1ULL << VIRTIO_NET_F_HOST_TSO6);
	}
#endif
	for (int id=0; id<MAX_RELAYS; ++id) {
		char buf[128];
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
		unsigned flags=0;
#endif
		virtio_vhostuser_id_to_name(id, buf, 128);
		struct stat statbuf;
		if (conf->vhost_client == 0 && stat(buf, &statbuf)==0) {
			// virtio-forwarder should not get this far if the daemon is already running (see pidfile check in virtio_forwarder_main.c)
			if (S_ISSOCK(statbuf.st_mode)) {
				if (unlink(buf) != 0) {
					log_warning("Found stale unix socket handle '%s', could not delete!", buf);
					return -1;
				}
				log_warning("Deleted stale unix socket handle '%s'", buf);
			} else {
				log_error("'%s' unexpectedly already exists!", buf);
				return -1;
			}
		}
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
		if (conf->vhost_client)
			flags |= RTE_VHOST_USER_CLIENT;
#endif
#if RTE_VERSION >= RTE_VERSION_NUM(16,11,0,0)
		if (conf->zerocopy)
			flags |= RTE_VHOST_USER_DEQUEUE_ZERO_COPY;
#endif
#if RTE_VERSION >= RTE_VERSION_NUM(16,7,0,0)
		if (rte_vhost_driver_register(buf, flags)!=0) {
#else
		if (rte_vhost_driver_register(buf)!=0) {
#endif
			log_error("Error starting vhost user instance '%s'!", buf);
			return -1;
		}

#if RTE_VERSION >= RTE_VERSION_NUM(17,5,0,0)
		/* Set features. */
		if (conf->use_rx_mrgbuf == 0) {
			if (rte_vhost_driver_disable_features(buf, 1ULL << VIRTIO_NET_F_MRG_RXBUF))
				log_warning("Failed to disabe mergeable RX buffers on %s", buf);
		}
		if (rte_vhost_driver_disable_features(buf, 1ULL << VIRTIO_NET_F_CTRL_RX))
			log_warning("Failed to disable RX control commands on %s", buf);
		if (conf->enable_tso == 0) {
			int disable_tso = 0;
			disable_tso |= rte_vhost_driver_disable_features(buf, 1ULL << VIRTIO_NET_F_HOST_TSO4);
			disable_tso |= rte_vhost_driver_disable_features(buf, 1ULL << VIRTIO_NET_F_HOST_TSO6);
			if (disable_tso)
				log_warning("Failed to disable TSO on %s", buf);
		}

		/* Start vhost diver for VF. */
		rte_vhost_driver_callback_register(buf, &virtio_vhostuser_ops);
		if (rte_vhost_driver_start(buf)) {
			log_critical("Error starting vhost driver on socket %s!", buf);
			return -1;
		}
#endif

		/* Change ownership of the socket files. */
		if (conf->vhost_client == 0 && (uid!=getuid() || gid!=getgid())) {
			if (chown(buf, uid, gid) != 0) {
				log_error("Could not change ownership of '%s' (%m)!", buf);
				return -1;
			}
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
		char buf[128];
		rte_vhost_driver_unregister(virtio_vhostuser_id_to_name(id, buf, 128));
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
