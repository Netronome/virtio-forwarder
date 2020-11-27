#!/usr/bin/python3
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

import argparse
import zmq
import os
import sys

try:
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay
except ImportError:
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay

def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config-ep', help='ZeroMQ configuration endpoint')
    parser.add_argument(
        '--crash-after-send', action='store_true',
        help='crash after sending request'
    )
    parser.add_argument(
        '--send-garbage', action='store_true',
        help='send garbage instead of a real request',
    )
    return parser


def main():
    args = _syntax().parse_args()

    config_ep = args.config_ep if args.config_ep else "ipc:///var/run/virtio-forwarder/config"
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.setsockopt(zmq.SNDTIMEO, 0)
    socket.setsockopt(zmq.RCVTIMEO, 2000)
    socket.connect(config_ep)

    if not args.send_garbage:
        msg = relay.ConfigRequest()
        socket.send(msg.SerializePartialToString())
    else:
        socket.send('@')

    if args.crash_after_send:
        assert False, 'user-initiated crash after send'

    reply = relay.ConfigResponse()
    reply.ParseFromString(socket.recv())
    assert reply.IsInitialized()

    print(reply)

if __name__ == '__main__':
    main()
