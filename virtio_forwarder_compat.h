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

#include <rte_version.h>

#if RTE_VERSION < RTE_VERSION_NUM(19,8,0,0)
#define RTE_ETHER_TYPE_IPV4 ETHER_TYPE_IPv4
#define RTE_IPV4_HDR_IHL_MASK IPV4_HDR_IHL_MASK
#define RTE_ETHER_TYPE_IPV6 ETHER_TYPE_IPv6
#define RTE_IPV4_IHL_MULTIPLIER IPV4_IHL_MULTIPLIER

#define rte_ether_hdr ether_hdr
#define rte_ipv4_hdr ipv4_hdr
#define rte_ipv6_hdr ipv6_hdr
#define rte_udp_hdr udp_hdr
#endif /* RTE_VERSION < RTE_VERSION_NUM(19,8,0,0) */

#if RTE_VERSION < RTE_VERSION_NUM(20,11,0,0)
#define RTE_LCORE_FOREACH_WORKER RTE_LCORE_FOREACH_SLAVE
#define SKIP_MAIN SKIP_MASTER

#define rte_get_main_lcore rte_get_master_lcore
#endif /* RTE_VERSION < RTE_VERSION_NUM(20,11,0,0) */
