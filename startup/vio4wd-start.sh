#!/bin/bash
#   BSD LICENSE
#
#   Copyright(c) 2016-2017 Netronome.
#   All rights reserved.
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#     * Neither the name of Netronome nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

. /etc/default/virtioforwarder || { stop; exit 1; }

CORE_SCHED_CMD_LINE=${VIRTIOFWD_ZMQ_CORE_SCHED_EP:+--zmq-core-sched-ep="$VIRTIOFWD_ZMQ_CORE_SCHED_EP"}
if ! test "$VIO4WD_CORE_SCHED_ENABLE" = true; then
    CORE_SCHED_CMD_LINE=""
fi

STATIC_VFS_CMD_LINE=
if [ -n "$VIRTIOFWD_STATIC_VFS" ]; then
    if [[ "$(declare -p VIRTIOFWD_STATIC_VFS)" =~ "declare -a" ]]; then
        # Array format
        for var in "${VIRTIOFWD_STATIC_VFS[@]}"
        do
            STATIC_VFS_CMD_LINE="$STATIC_VFS_CMD_LINE -P$var"
        done
    else
        # Single VF
        STATIC_VFS_CMD_LINE="-P$VIRTIOFWD_STATIC_VFS"
    fi
fi

CPU_PINS_CMD_LINE=
if [ -n "$VIRTIOFWD_CPU_PINS" ]; then
    if [[ "$(declare -p VIRTIOFWD_CPU_PINS)" =~ "declare -a" ]]; then
        # Array format
        for var in "${VIRTIOFWD_CPU_PINS[@]}"
        do
            CPU_PINS_CMD_LINE="$CPU_PINS_CMD_LINE -c$var"
        done
    else
        # Single pin map
        CPU_PINS_CMD_LINE="-c$VIRTIOFWD_CPU_PINS"
    fi
fi

# Start virtio-forwarder.
"${VIRTIOFWD_BINARY:?not set}" \
    -C"${VIRTIOFWD_CPU_MASK:?not set}" \
    --nodaemon=log_syslog \
    ${VIRTIOFWD_LOG_LEVEL:+-l"$VIRTIOFWD_LOG_LEVEL"} \
    ${VIRTIOFWD_MASTER_LCORE:+-M"$VIRTIOFWD_MASTER_LCORE"} \
    ${VIRTIOFWD_PID_DIR:+-p"$VIRTIOFWD_PID_DIR"} \
    ${VIRTIOFWD_HUGETLBFS_MOUNT_POINT:+-H"$VIRTIOFWD_HUGETLBFS_MOUNT_POINT"} \
    ${VIRTIOFWD_OVSDB_SOCK_PATH:+-O"$VIRTIOFWD_OVSDB_SOCK_PATH"} \
    ${VIRTIOFWD_ZMQ_CONFIG_EP:+--zmq-config-ep="$VIRTIOFWD_ZMQ_CONFIG_EP"} \
    ${VIRTIOFWD_ZMQ_PORT_CONTROL_EP:+--zmq-port-control-ep="$VIRTIOFWD_ZMQ_PORT_CONTROL_EP"} \
    ${VIRTIOFWD_ZMQ_STATS_EP:+--zmq-stats-ep="$VIRTIOFWD_ZMQ_STATS_EP"} \
    ${CORE_SCHED_CMD_LINE} \
    ${VIRTIOFWD_IPC_PORT_CONTROL:+--ipc} \
    ${VIRTIOFWD_SOCKET_OWNER:+-u"$VIRTIOFWD_SOCKET_OWNER"} \
    ${VIRTIOFWD_SOCKET_GROUP:+-g"$VIRTIOFWD_SOCKET_GROUP"} \
    ${VIRTIOFWD_SOCKET_DIR:+--vhost-path="$VIRTIOFWD_SOCKET_DIR"} \
    ${VIRTIOFWD_SOCKET_FNAME_PATTERN:+--vhost-socket="$VIRTIOFWD_SOCKET_FNAME_PATTERN"} \
    ${VIRTIOFWD_JUMBO:+-J} \
    ${VIRTIOFWD_MRGBUF:+--enable-mrgbuf} \
    ${VIRTIOFWD_VHOST_CLIENT:+--vhostuser-client} \
    ${VIRTIOFWD_ZERO_COPY:+--zero-copy} \
    ${VIRTIOFWD_TSO:+--enable-tso} \
    ${CPU_PINS_CMD_LINE} \
    ${STATIC_VFS_CMD_LINE}
