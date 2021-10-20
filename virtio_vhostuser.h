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

#ifndef _VIRTIO_VHOSTUSER_THREAD
#define _VIRTIO_VHOSTUSER_THREAD

#include <stdint.h>
#include <stdbool.h>
#include <rte_eal.h>
#include <rte_ethdev.h>

#define MAX_RELAYS RTE_MAX_ETHPORTS
#define MAX_NUM_BOND_SLAVES 8


struct relay_cpus {
   int vf2vio_cpu;
   int vio2vf_cpu;
};

struct static_relay_entry {
   char pci_dbdf[20];
   int virtio_id;
};

struct virtio_vhostuser_conf {
    uint64_t worker_core_bitmap; /** bitmap of logical CPUs that worker threads may run on */
    char socket_path[128]; /** directory path where the vhost-user unix domain sockets will be created */
    char socket_name[32]; /** vhost-user unix domain socket template filename, must contain exactly one %u instance to represent the virtio ID */
    char vhost_username[32]; /** Username which the vhost-user unix domain socket must be assigned to, blank to inherit the process user */
    char vhost_groupname[32]; /** Group name which the vhost-user unix domain socket must be assigned to, blank to inherit the process group */
    struct relay_cpus relay_cpus[MAX_RELAYS];
    struct {
        struct static_relay_entry static_relays[MAX_RELAYS]; /** Relay entries configured on cmdline at startup */
        unsigned num_static_entries;
    } static_relay_conf;
    unsigned use_jumbo:1;
    unsigned use_rx_mrgbuf:1;
    unsigned vhost_client:1;
    unsigned zerocopy:1;
    unsigned enable_tso:1;
};

int virtio_vhostuser_start(const struct virtio_vhostuser_conf *conf,
			bool create_sockets);

void virtio_vhostuser_stop(void);

int virtio_add_sock_dev_pair(const char *vhost_path,
			char slave_dbdfs[MAX_NUM_BOND_SLAVES][RTE_ETH_NAME_MAX_LEN],
			unsigned num_slaves, char *name, uint8_t mode,
			bool conditional);
int virtio_remove_sock_dev_pair(const char *vhost_path, char *dev,
			bool conditional);

#endif // _VIRTIO_VHOSTUSER_THREAD
