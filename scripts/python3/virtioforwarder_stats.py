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
import logging
import numbers
import os
import sys
import zmq

try:
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay
except ImportError:
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay

logger = logging.getLogger(os.path.split(sys.argv[0])[-1])


def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stats-ep', help='ZeroMQ statistics endpoint')
    parser.add_argument(
        '--include-inactive', action='store_true',
        help='include inactive relays in response',
    )
    parser.add_argument(
        '--delay', type=int, default=200,
        help='Delay in ms to use for calculating rate statistics.',
    )
    parser.add_argument(
        '--output-format', choices=('flat', 'protobuf'), default='flat',
        help='output format'
    )
    parser.add_argument(
        '-z', '--suppress-zero', action='store_true',
        help=(
            "don't show statistics that are zero (only applies to flat output "
            "format)"
        ),
    )
    return parser


def _output_flat(reply, suppress_zero):
    if reply.status != reply.OK:
        logger.error(
            'StatsResponse: %s', relay.StatsResponse.Status.Name(reply.status)
        )
        return 1

    for r in reply.relay:
        relay_str = 'relay_{}'.format(r.id)
        middle = []
        fields = {}

        def fieldset(o):
            return frozenset(x.name for x, y in o.ListFields())

        def out(o, k):
            if o is None:
                return

            f = getattr(fields, repr(o), None)
            if f is None:
                f = fields[repr(o)] = fieldset(o)

            if k in f:
                v = getattr(o, k)
                if not (
                    suppress_zero
                    and isinstance(v, numbers.Integral)
                    and not isinstance(v, bool)
                    and v == 0
                ):
                    print('.'.join(
                        [relay_str] + middle + ['{}={}'.format(k, v)])
                    )

        out(r, 'active')
        middle = ['cpu']
        out(r.cpu, 'vf_to_vm')
        out(r.cpu, 'vm_to_vf')
        middle = ['vf']
        out(r.vf, 'pci_addr_str')

        middle = ['vhost']
        out(r.vhost, 'vhost_socket_name')

        cpu_fieldset = fieldset(r.cpu)

        middle = ['vf_to_vm']
        v = r.vf_to_vm
        out(v, 'active')
        out(v, 'internal_state')

        if middle[0] in cpu_fieldset:
            middle.append('cpu_{}'.format(r.cpu.vf_to_vm))
        out(v, 'pkts_rx_from_vf')
        out(v, 'bytes_rx_from_vf')
        out(v, 'pkts_tx_to_vm')
        out(v, 'bytes_tx_to_vm')
        out(v, 'pkt_rate_rx_from_vf')
        out(v, 'byte_rate_rx_from_vf')
        out(v, 'pkt_rate_tx_to_vm')
        out(v, 'byte_rate_tx_to_vm')
        out(v, 'pkts_dropped_vm_queue_full')
        out(v, 'bytes_dropped_vm_queue_full')
        out(v, 'pkts_dropped_vm_not_connected')
        out(v, 'bytes_dropped_vm_not_connected')

        middle = ['vm_to_vf']
        v = r.vm_to_vf
        out(v, 'active')
        out(v, 'internal_state')

        if middle[0] in cpu_fieldset:
            middle.append('cpu_{}'.format(r.cpu.vm_to_vf))
        out(v, 'pkts_rx_from_vm')
        out(v, 'bytes_rx_from_vm')
        out(v, 'pkts_tx_to_vf')
        out(v, 'bytes_tx_to_vf')
        out(v, 'pkt_rate_rx_from_vm')
        out(v, 'byte_rate_rx_from_vm')
        out(v, 'pkt_rate_tx_to_vf')
        out(v, 'byte_rate_tx_to_vf')
        out(v, 'pkts_dropped_vf_queue_full')
        out(v, 'bytes_dropped_vf_queue_full')
        out(v, 'pkts_dropped_vf_not_connected')
        out(v, 'bytes_dropped_vf_not_connected')


def _output_protobuf(reply):
    print(reply)


def main():
    logging.basicConfig(level=logging.DEBUG)

    args = _syntax().parse_args()

    stats_ep = args.stats_ep if args.stats_ep else "ipc:///var/run/virtio-forwarder/stats"
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.setsockopt(zmq.SNDTIMEO, 0)
    socket.setsockopt(zmq.RCVTIMEO, 2000+args.delay)
    socket.connect(stats_ep)

    msg = relay.StatsRequest(relay=None)
    msg.include_inactive = args.include_inactive
    msg.delay = args.delay
    socket.send(msg.SerializeToString())

    reply = relay.StatsResponse()
    reply.ParseFromString(socket.recv())
    assert reply.IsInitialized()

    output_formats = {
        'flat': lambda reply: _output_flat(reply, args.suppress_zero),
        'protobuf': _output_protobuf,
    }
    return output_formats[args.output_format](reply)

if __name__ == '__main__':
    sys.exit(main())
