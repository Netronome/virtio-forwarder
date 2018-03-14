#!/usr/bin/env python
# -*- coding: utf-8 -*-
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
import re
try:
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay_
except ImportError:
    import os
    import sys
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay

def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zmq_ep', help='ZeroMQ port control endpoint')
    parser.add_argument(
        'op', metavar='OP', choices=('add', 'remove'),
        help='port control operation',
    )
    parser.add_argument(
        'virtio_id', metavar='virtio-id', type=int,
        help='virtio instance to connect to'
    )
    parser.add_argument(
        '--crash-after-send', action='store_true',
        help='crash after sending request'
    )
    parser.add_argument(
        '--pci-addr', default=[], action='append',
        help='PCI address, e.g. 0000:11:22.3')
    parser.add_argument(
        '--conditional', type=bool,
        help='whether to make a conditional request (default: unspecified)'
    )
    parser.add_argument(
        '--send-garbage', action='store_true',
        help='send garbage instead of a real request',
    )
    return parser

def parse_pci_addr(addr):
    """Parse PCI address into protobuf.

       Parameters
       ----------
       addr : string
           PCI address. Either xxxx:xx:xx.x or xx:xx.x

       Returns
       -------
       PortControlRequest.PciAddress()
    """
    assert(type(addr) == str)
    if re.search('[^\.:0-9a-fA-F]', addr):
        return (None, 1) # Badly formatted address

    try:
        dom, b, dev, f = [int(x, 16) for x in re.split('[:\.]', addr)]
    except ValueError:
        try:
            dom = 0
            b, dev, f = [int(x, 16) for x in re.split('[:\.]', addr)]
        except ValueError:
            return (None, 1) # Badly formatted address
        
    return (relay.PortControlRequest.PciAddress(domain=dom, bus=b, slot=dev,
                                                function=f), 0)

def main():
    args = _syntax().parse_args()

    port_control_ep = args.zmq_ep if args.zmq_ep else "ipc:///var/run/virtio-forwarder/port_control"
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.setsockopt(zmq.SNDTIMEO, 0)
    socket.setsockopt(zmq.RCVTIMEO, 2000)
    socket.connect(port_control_ep)

    msg = relay.PortControlRequest(
        op=getattr(relay.PortControlRequest, args.op.upper()),
    )
    if args.virtio_id is not None:
        msg.virtio_id = int(args.virtio_id)
    for addr in args.pci_addr:
        parsed_addr, ret = parse_pci_addr(addr)
        if ret == 0:
            # Address format is good
            pb_pci_addr = relay.PortControlRequest.PciAddress()
            msg.pci_addrs.extend([parsed_addr])
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
