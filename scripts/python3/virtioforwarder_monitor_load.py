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
import time
from virtioforwarder_core_scheduler import RelayRate
from virtioforwarder_core_scheduler import open_socket
from virtioforwarder_core_scheduler import reconnect_send_recv

import os
import sys
try:
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay_pb2
except ImportError:
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay_pb2

MAX_PPS = 15e6 # pps
MAX_MBPS = 50e3 # Mbps

# [ Argument parser
def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stats-ep', help='ZeroMQ statistics endpoint')
    parser.add_argument('--sched-ep', help='ZeroMQ core scheduler endpoint')
    parser.add_argument(
        '--poll-interval', type=float,
        help='set the polling/control interval in seconds'
    )
    return parser
# ]

class RelayRateExt(RelayRate):
    def __init__(self, pps, Bps):
        # VM2VF
        self._virtio_rx_pps = pps
        self._virtio_rx_Bps = Bps
        self._dpdk_tx_pps = pps
        self._dpdk_tx_Bps = Bps
        # VF2VM
        self._dpdk_rx_pps = pps
        self._dpdk_rx_Bps = Bps
        self._virtio_tx_pps = pps
        self._virtio_tx_Bps = Bps
        # Loads
        self._vm2vf_load = 0
        self._vf2vm_load = 0

def main():
    args = _syntax().parse_args()

    poll_interval = 2.
    if args.poll_interval:
        poll_interval = args.poll_interval

    # Initialize stats client
    stats_ep = args.stats_ep if args.stats_ep else "ipc:///var/run/virtio-forwarder/stats"
    stats_sock = open_socket(stats_ep)

    # Send message
    # First message is strictly not necessary, but it is done
    # here in order to get a reference time for VIO4WD rate stats.
    stats_request = relay_pb2.StatsRequest()
    stats_request.include_inactive = False
    stats_request.delay = 200
    stats_response = relay_pb2.StatsResponse()
    stats_sock = reconnect_send_recv(stats_sock, 'stats', stats_request, stats_response, stats_ep, poll_interval)[1]

    # Initialize core scheduler client
    sched_ep = args.sched_ep if args.sched_ep else "ipc:///var/run/virtio-forwarder/core_sched"
    sched_sock = open_socket(sched_ep)

    # Get worker core mapping
    sched_request = relay_pb2.CoreSchedRequest(op=relay_pb2.CoreSchedRequest.GET_EAL_CORES)
    sched_response = relay_pb2.CoreSchedResponse()
    sched_sock = reconnect_send_recv(sched_sock, 'core scheduler', sched_request, sched_response, sched_ep, poll_interval)[1]
    worker_cores = sched_response.eal_cores

    max_relay = RelayRateExt(MAX_PPS, MAX_MBPS/8*1e6)
    max_load = 1 * (max_relay.estimate_vm2vf_load() + max_relay.estimate_vf2vm_load())
    max_chars = 60
    try:
        # [ Main processing loop
        while True:
            # Get stats
            relays = []
            stats_response = relay_pb2.StatsResponse()
            err, stats_sock = reconnect_send_recv(stats_sock, 'stats', stats_request, stats_response, stats_ep, poll_interval)
            relays = stats_response.relay
            # Gather worker cores again if the server went down.
            if err:
                sched_response = relay_pb2.CoreSchedResponse()
                sched_sock = reconnect_send_recv(sched_sock, 'core scheduler', sched_request, sched_response, sched_ep, poll_interval)[1]
                worker_cores = sched_response.eal_cores
            if len(relays) == 0:
                time.sleep(poll_interval)
                continue

            # There are running relays
            # Calculate worker loads
            worker_loads = {i: 0. for i in worker_cores}
            for relay in relays:
                rate = RelayRate(relay)
                worker_loads[relay.cpu.vm_to_vf] += rate.estimate_vm2vf_load()
                worker_loads[relay.cpu.vf_to_vm] += rate.estimate_vf2vm_load()

            os.system('clear')
            print("CPU loads")
            for cpu,load in worker_loads.items():
                try:
                    bars = int(round(load/max_load*max_chars))
                except ZeroDivisionError:
                    bars = 0
                print("CPU{}:".format(cpu), "-"*bars)

            time.sleep(poll_interval)
        # ] End while true
    except KeyboardInterrupt:
        pass

if __name__ == '__main__':
    main()
