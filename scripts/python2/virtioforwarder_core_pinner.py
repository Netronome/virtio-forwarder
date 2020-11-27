#!/usr/bin/python2
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
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay_pb2
except ImportError:
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay_pb2

# [ Argument parser
def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stats-ep', help='ZeroMQ statistics endpoint')
    parser.add_argument('--sched-ep', help='ZeroMQ core scheduler endpoint')
    parser.add_argument(
        '--virtio-cpu', default=[],  action='append',
        help='set a relay\'s worker cpus; e.g. --virtio-cpu=42:1,2'
    )
    parser.add_argument(
        '--print-core-map', action='store_true',
        help='print the current core map and exit'
    )
    return parser
# ]

def print_core_mapping(relays_stats, workers):
    affinities = {n: {'VM2VF': [], 'VF2VM': []} for n in workers}
    for relay in relays_stats.relay:
        affinities[relay.cpu.vm_to_vf]['VM2VF'].append(relay.id)
        affinities[relay.cpu.vf_to_vm]['VF2VM'].append(relay.id)

    n_workers = len(workers)
    print('=' * (8*n_workers+3*(n_workers-1)))
    max_col = -1
    msg = ""
    for worker, mapping in affinities.iteritems():
        msg += '== %02d ==   ' % worker
        if len(mapping['VM2VF']) > max_col:
            max_col = len(mapping['VM2VF'])
        if len(mapping['VF2VM']) > max_col:
            max_col = len(mapping['VF2VM'])
    print(msg.rstrip())

    for i in range(max_col):
        msg = ""
        for worker, mapping in affinities.iteritems():
            try:
                msg += "%02d" % mapping['VM2VF'][i]
            except IndexError:
                msg += '  '
                pass
            msg += '    '
            try:
                msg += "%02d" % mapping['VF2VM'][i]
            except IndexError:
                msg += '  '
                pass
            msg += '   '
        print(msg.rstrip())
    print('=' * (8*n_workers+3*(n_workers-1)))

# [ main
def main():
    args = _syntax().parse_args()

    # Initialize stats client
    stats_ep = args.stats_ep if args.stats_ep else "ipc:///var/run/virtio-forwarder/stats"
    stats_ctx = zmq.Context()
    stats_sock = stats_ctx.socket(zmq.REQ)
    stats_sock.setsockopt(zmq.LINGER, 0)
    stats_sock.setsockopt(zmq.SNDTIMEO, 0)
    stats_sock.setsockopt(zmq.RCVTIMEO, 2000)
    stats_sock.connect(stats_ep)

    # Initialize core pinning client
    sched_ep = args.sched_ep if args.sched_ep else "ipc:///var/run/virtio-forwarder/core_sched"
    sched_ctx = zmq.Context()
    sched_sock = sched_ctx.socket(zmq.REQ)
    sched_sock.setsockopt(zmq.LINGER, 0)
    sched_sock.setsockopt(zmq.SNDTIMEO, 0)
    sched_sock.setsockopt(zmq.RCVTIMEO, 2000)
    sched_sock.connect(sched_ep)

    # Get relay stats
    stats_request = relay_pb2.StatsRequest()
    stats_request.include_inactive = False
    stats_sock.send(stats_request.SerializePartialToString())
    stats_response = relay_pb2.StatsResponse()
    stats_response.ParseFromString(stats_sock.recv())
    assert stats_response.IsInitialized()

    # Get worker core mapping
    msg = relay_pb2.CoreSchedRequest(op=relay_pb2.CoreSchedRequest.GET_EAL_CORES)
    sched_sock.send(msg.SerializePartialToString())
    worker_cores = relay_pb2.CoreSchedResponse()
    worker_cores.ParseFromString(sched_sock.recv())
    worker_cores = worker_cores.eal_cores

    if args.print_core_map:
        print "Worker cores affinities:"
        print_core_mapping(stats_response, worker_cores)
        sys.exit(0)

    # Initialize mapping structure
    relay_mappings = []
    for mapping in args.virtio_cpu:
        try:
            n_relay = int(mapping.split(':')[0])
            virtio2vf = int(mapping.split(':')[1].split(',')[0])
            vf2virtio = int(mapping.split(':')[1].split(',')[1])
        except ValueError:
            print "Invalid virtio-cpu command line format. Usage: virtio-cpu=n:c1,c2"
            sys.exit(0)
        print "Setting relay %d=%d,%d" % (n_relay, virtio2vf, vf2virtio)
        req = relay_pb2.CoreSchedRequest.RelayCPU()
        req.relay_number = n_relay
        req.virtio2vf_cpu = virtio2vf
        req.vf2virtio_cpu = vf2virtio
        relay_mappings.append(req)

    if relay_mappings != []:
        # Trigger cpu migration
        sched_req = relay_pb2.CoreSchedRequest(op=relay_pb2.CoreSchedRequest.UPDATE,
                                               relay_cpu_map=relay_mappings)
        sched_sock.send(sched_req.SerializePartialToString())
        # Gather response
        sched_response = relay_pb2.CoreSchedResponse()
        sched_response.ParseFromString(sched_sock.recv())
        if sched_response.status == relay_pb2.CoreSchedResponse.OK:
            print "Scheduler response: OK"
        else:
            print "Scheduler response: ERROR"
        # Print new mapping
        stats_sock.send(stats_request.SerializePartialToString())
        stats_response = relay_pb2.StatsResponse()
        stats_response.ParseFromString(stats_sock.recv())
        print "New worker core mapping:"
        print_core_mapping(stats_response, worker_cores)
    else:
        print "Worker cores affinities:"
        print_core_mapping(stats_response, worker_cores)
# ] End main().

if __name__ == '__main__':
    main()
