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

# Create the dir with the ZMQ endpoints.
@LIBEXECDIR@/create_zmq_dir.py $VIRTIOFWD_ZMQ_CORE_SCHED_EP
@LIBEXECDIR@/create_zmq_dir.py $VIRTIOFWD_ZMQ_STATS_EP
@LIBEXECDIR@/create_zmq_dir.py $VIRTIOFWD_ZMQ_PORT_CONTROL_EP
@LIBEXECDIR@/create_zmq_dir.py $VIRTIOFWD_ZMQ_CONFIG_EP

# Set up the socket dir permissions.
mkdir -vp "$VIRTIOFWD_SOCKET_DIR" || { stop; exit 1; }
test -n "$VIRTIOFWD_SOCKET_OWNER" &&
    chown "$VIRTIOFWD_SOCKET_OWNER" "$VIRTIOFWD_SOCKET_DIR"
test -n "$VIRTIOFWD_SOCKET_GROUP" &&
    chgrp "$VIRTIOFWD_SOCKET_GROUP" "$VIRTIOFWD_SOCKET_DIR"

test -n "$VIRTIOFWD_BIND_VFIO_PCI" &&
   @LIBEXECDIR@//bind_uio_driver.py "$VIRTIOFWD_BIND_VFIO_PCI"

# Clean up any stray PID file that may be lying around.
rm -f "$VIRTIOFWD_PID_DIR"/virtioforwarder.pid
