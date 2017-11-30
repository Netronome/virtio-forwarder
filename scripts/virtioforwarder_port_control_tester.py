#!/usr/bin/env python
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

from protobuf.virtioforwarder import virtioforwarder_pb2 as relay


def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('zmq_ep', metavar='ZMQ-EP', help='ZeroMQ endpoint')
    parser.add_argument(
        'op', metavar='OP', choices=('add', 'remove'),
        help='port control operation',
    )
    parser.add_argument('--vf', metavar='VF', help='VF', type=int)
    parser.add_argument(
        '--crash-after-send', action='store_true',
        help='crash after sending request'
    )
    parser.add_argument('--domain', type=int, help='PCI domain')
    parser.add_argument('--bus', type=int, help='PCI bus')
    parser.add_argument('--slot', type=int, help='PCI slot')
    parser.add_argument('--function', type=int, help='PCI function')
    parser.add_argument(
        '--conditional', type=bool,
        help='whether to make a conditional request (default: unspecified)'
    )
    parser.add_argument(
        '--send-garbage', action='store_true',
        help='send garbage instead of a real request',
    )
    return parser


def main():
    args = _syntax().parse_args()

    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.setsockopt(zmq.SNDTIMEO, 0)
    socket.setsockopt(zmq.RCVTIMEO, 2000)
    socket.connect(args.zmq_ep)

    msg = relay.PortControlRequest(
        op=getattr(relay.PortControlRequest, args.op.upper()),
    )
    if args.vf is not None:
        msg.vf = args.vf
    if args.domain is not None:
        msg.pci_addr.domain = args.domain
    if args.bus is not None:
        msg.pci_addr.bus = args.bus
    if args.slot is not None:
        msg.pci_addr.slot = args.slot
    if args.function is not None:
        msg.pci_addr.function = args.function
    if args.conditional is not None:
        msg.conditional = args.conditional

    if not args.send_garbage:
        socket.send(msg.SerializePartialToString())
    else:
        socket.send('garbagé')

    if args.crash_after_send:
        assert False, 'user-initiated crash after send'

    reply = relay.PortControlResponse()
    reply.ParseFromString(socket.recv())
    assert reply.IsInitialized()
    print reply

if __name__ == '__main__':
    main()
