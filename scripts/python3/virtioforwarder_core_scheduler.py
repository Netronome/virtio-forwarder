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
import syslog
from math import sqrt
import signal
import os
import sys
from parse_numa import get_max_node, get_node_cpus
try:
    from protobuf.virtioforwarder import virtioforwarder_pb2 as relay_pb2
except ImportError:
    PWD = os.path.dirname(os.path.abspath(__file__))
    sys.path.append(PWD + '/../build/protobuf/virtioforwarder')
    import virtioforwarder_pb2 as relay_pb2

must_run = True

# [ Argument parser
def _syntax():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stats-ep', help='ZeroMQ statistics endpoint')
    parser.add_argument('--sched-ep', help='ZeroMQ core scheduler endpoint')
    parser.add_argument(
        '--poll-interval', type=float,
        help='set the polling/control interval in seconds'
    )
    parser.add_argument(
        '--loglevel', type=int,
        help='set the logging verbosity'
    )
    parser.add_argument(
        '--sensitivity', type=float, default=0.25,
        help=("set the load balancing sensitivity (>=0)"
            " Smaller is more sensitive")
    )
    parser.add_argument(
        '--global-numa-opt', action='store_true',
        help='Do not condider NUMA affinities when optimizing'
    )

    return parser
# ]

# [ Core scheduler helpers
def print_core_mapping(relays_stats, workers, lvl=syslog.LOG_DEBUG):
    affinities = {n: {'VM2VF': [], 'VF2VM': []} for n in workers}
    for relay in relays_stats.relay:
        affinities[relay.cpu.vm_to_vf]['VM2VF'].append(relay.id)
        affinities[relay.cpu.vf_to_vm]['VF2VM'].append(relay.id)

    n_workers = len(workers)
    syslog.syslog(lvl, '=' * (8*n_workers+3*(n_workers-1)))
    max_col = -1
    msg = ""
    for worker, mapping in affinities.items():
        msg += '== %02d ==   ' % worker
        if len(mapping['VM2VF']) > max_col:
            max_col = len(mapping['VM2VF'])
        if len(mapping['VF2VM']) > max_col:
            max_col = len(mapping['VF2VM'])
    syslog.syslog(lvl, msg.rstrip())

    for i in range(max_col):
        msg = ""
        for worker, mapping in affinities.items():
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
        syslog.syslog(lvl, msg.rstrip())
    syslog.syslog(lvl, '=' * (8*n_workers+3*(n_workers-1)))

class RelayRate:
    """ Class to store and calculate relay rate statistics.
    """
    def __init__(self, relay_state):
        self._id = relay_state.id
        # VM2VF
        self._virtio_rx_pps = relay_state.vm_to_vf.pkt_rate_rx_from_vm
        self._virtio_rx_Bps = relay_state.vm_to_vf.byte_rate_rx_from_vm
        self._dpdk_tx_pps = relay_state.vm_to_vf.pkt_rate_tx_to_vf
        self._dpdk_tx_Bps = relay_state.vm_to_vf.byte_rate_tx_to_vf
        # VF2VM
        self._dpdk_rx_pps = relay_state.vf_to_vm.pkt_rate_rx_from_vf
        self._dpdk_rx_Bps = relay_state.vf_to_vm.byte_rate_rx_from_vf
        self._virtio_tx_pps = relay_state.vf_to_vm.pkt_rate_tx_to_vm
        self._virtio_tx_Bps = relay_state.vf_to_vm.byte_rate_tx_to_vm
        # Loads
        self._vm2vf_load = 0
        self._vf2vm_load = 0

    def estimate_vm2vf_load(self):
        """return load = a*Mpps + b*MBps"""
        self._vm2vf_load =  8.2e-02*(self._virtio_rx_pps+self._dpdk_tx_pps)/2.*1e-6 + \
                            9.3e-05*(self._virtio_rx_Bps+self._dpdk_tx_Bps)/2.*1e-6
        return self._vm2vf_load

    def estimate_vf2vm_load(self):
        """return load = a*Mpps + b*MBps"""
        self._vf2vm_load =  7.95e-02*(self._dpdk_rx_pps+self._virtio_tx_pps)/2.*1e-6 + \
                            8.2e-05*(self._dpdk_rx_Bps+self._virtio_tx_Bps)/2.*1e-6
        return self._vf2vm_load

    def get_vm2vf_load(self):
        return self._vm2vf_load

    def get_vf2vm_load(self):
        return self._vf2vm_load
# ]

# [
def round_robin(relay_rates, worker_cores):
    """Round robin assignment of relay instance to worker cores.
    """
    # Grab loads
    relay_throughput = []
    for key, obj in relay_rates.items():
        n_relay = key
        load = obj.estimate_vm2vf_load()
        relay_throughput.append([n_relay, load, 'VM2VF'])
        load = obj.estimate_vf2vm_load()
        relay_throughput.append([n_relay, load, 'VF2VM'])

    # Round robin assignment to least busy worker
    # Sort by load
    relay_throughput = sorted(relay_throughput, key=lambda tup: tup[1], reverse=True)
    worker_loads = {n: 0 for n in worker_cores}
    for i in range(len(relay_throughput)):
        load_map = relay_throughput[i]
        least_loaded = min(worker_loads, key=worker_loads.get)
        worker_loads[least_loaded] += load_map[1]
        relay_throughput[i].append(least_loaded)

    # Build relay_mappings structure
    # Sort by relay id
    relay_throughput = sorted(relay_throughput, key=lambda tup: tup[0], reverse=False)
    relay_mappings = []
    i = 0
    while i <len(relay_throughput):
        vm2vf = relay_throughput[i] if 'VM2VF' in relay_throughput[i] else relay_throughput[i+1]
        vf2vm = relay_throughput[i+1] if 'VF2VM' in relay_throughput[i+1] else relay_throughput[i]
        assert(vm2vf[0] == vf2vm[0]) # must have same ids
        req = relay_pb2.CoreSchedRequest.RelayCPU()
        req.relay_number = vm2vf[0]
        req.virtio2vf_cpu = vm2vf[-1]
        req.vf2virtio_cpu = vf2vm[-1]
        relay_mappings.append(req)
        i += 2

    # Calculate goodness measure as the coefficient of variation of worker loads.
    mu = float(sum(worker_loads.values()))/len(worker_loads)
    if mu == 0:
        return (relay_mappings, -1.0)

    var = sum([(load-mu)**2 for load in worker_loads.values()])
    stddev = sqrt(var/len(worker_loads))
    cv = stddev/mu
    return (relay_mappings, cv)

def get_variation_coefficient(prev_mappings, relay_rates, worker_cores):
    """Calculate goodness of fit according to the given mapping.

    Parameters
    ----------
    prev_mappings : list
        list of CoreSchedRequest objects.
    relay_rates : dict
        dict containing RelayRate class instances.
    worker_cores : list
        VIO4WD worker cores.

    Returns
    -------
    Coefficient of variation of the worker loads according to the given mapping and rates.
    """
    # Return extreme deviation if there is not yet a previous result
    if prev_mappings == None:
        return 1e10

    worker_loads = {n: 0 for n in worker_cores}
    for req in prev_mappings:
        try:
            worker_loads[req.virtio2vf_cpu] += relay_rates[req.relay_number].get_vm2vf_load()
            worker_loads[req.vf2virtio_cpu] += relay_rates[req.relay_number].get_vf2vm_load()
        # An inter numa event will result in a KeyError.
        except KeyError:
            return 1e10

    mu = float(sum(worker_loads.values()))/len(worker_loads)
    if mu == 0:
        return -1.0

    var = sum([(load-mu)**2 for load in worker_loads.values()])
    stddev = sqrt(var/len(worker_loads))
    return stddev/mu
# ]

# [ socket connection functions
def stop_socket(sock):
    sock.setsockopt(zmq.LINGER, 0)
    sock.close()

def open_socket(ep):
    sock_ctx = zmq.Context()
    sock = sock_ctx.socket(zmq.REQ)
    sock.setsockopt(zmq.LINGER, 0)
    sock.setsockopt(zmq.SNDTIMEO, 0)
    sock.setsockopt(zmq.RCVTIMEO, 2000)
    sock.connect(ep)

    return sock

def reconnect_send_recv(sock, name, request, response, ep, poll_interval):
    # Request-reply in teardown-initialize loop.
    global must_run
    err = 0
    while True:
        try:
            sock.send(request.SerializePartialToString())
            response.ParseFromString(sock.recv())
            assert response.IsInitialized()
            if err > 0:
                syslog.syslog(syslog.LOG_ERR, "Reconnected to %s server" % name)
            return (err, sock)
        except zmq.Again:
            if not must_run:
                syslog.syslog(syslog.LOG_WARNING, "ZMQ server unreachable on exit. Forcing exit")
                sys.exit(0)
            err = 1
            syslog.syslog(syslog.LOG_ERR, "No response from %s server. Reconnecting..." % name)
            stop_socket(sock)
            sock = open_socket(ep)
            time.sleep(poll_interval)
# ]

def sig_handler(signum, frame):
    global must_run
    if signum == signal.SIGHUP:
        return
    syslog.syslog(syslog.LOG_DEBUG, "Got signal %d" % signum)
    must_run = False

# [ main
def main():
    global must_run
    args = _syntax().parse_args()

    # Define signal handler
    signal.signal(signal.SIGTERM, sig_handler)
    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGHUP, sig_handler)

    # Initiate logging
    syslog.openlog(
        ident='vio4wd_core_scheduler',
        logoption=syslog.LOG_PID,
        facility=syslog.LOG_USER
    )
    loglevel = args.loglevel if args.loglevel else 7
    if loglevel > 7 or loglevel < 0:
        loglevel = 7
        syslog.syslog(syslog.LOG_WARNING, "Specified invalid loglevel. Defaulting to %d" % loglevel)
    syslog.setlogmask(syslog.LOG_UPTO(loglevel))
    syslog.syslog(syslog.LOG_NOTICE, "Dynamic load balancing initiated...")

    if args.sensitivity < 0:
        args.sensitivity = 0.25
        syslog.syslog(syslog.LOG_WARNING, "Specified invalid sensitivity. Defaulting to %f" % args.sensitivity)

    poll_interval = 5.
    if args.poll_interval:
        poll_interval = args.poll_interval
    syslog.syslog(syslog.LOG_INFO, "Polling every %.2f seconds." % poll_interval)

    # Initialize stats client
    stats_ep = args.stats_ep if args.stats_ep else "ipc:///var/run/virtio-forwarder/stats"
    syslog.syslog(syslog.LOG_INFO, "Connecting to stats server on %s..." % stats_ep)
    stats_sock = open_socket(stats_ep)

    # Get stats for initial worker core mapping
    stats_request = relay_pb2.StatsRequest()
    stats_request.include_inactive = False
    stats_request.delay = 200
    stats_response = relay_pb2.StatsResponse()
    stats_sock = reconnect_send_recv(stats_sock, 'stats', stats_request, stats_response, stats_ep, poll_interval)[1]

    # Initialize core scheduler client
    sched_ep = args.sched_ep if args.sched_ep else "ipc:///var/run/virtio-forwarder/core_sched"
    syslog.syslog(syslog.LOG_INFO, "Connecting to core_scheduler server on %s..." % sched_ep)
    sched_sock = open_socket(sched_ep)

    # Get worker core mapping
    sched_request = relay_pb2.CoreSchedRequest(op=relay_pb2.CoreSchedRequest.GET_EAL_CORES)
    sched_response = relay_pb2.CoreSchedResponse()
    sched_sock = reconnect_send_recv(sched_sock, 'core scheduler', sched_request, sched_response, sched_ep, poll_interval)[1]
    worker_cores = sched_response.eal_cores
    syslog.syslog(syslog.LOG_DEBUG, "Worker cores at startup:")
    print_core_mapping(stats_response, worker_cores, syslog.LOG_DEBUG)

    # Gather NUMA information
    node_cpus = {}
    for i in range(get_max_node()+1):
        cpus = get_node_cpus(i)
        workers_on_numa = cpus & set(worker_cores)
        if len(workers_on_numa) > 0:
            node_cpus[i] = cpus.copy()

    # [ main processing loop
    while must_run:
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
            syslog.syslog(syslog.LOG_DEBUG, "Worker cores upon reconnection:")
            print_core_mapping(stats_response, worker_cores, syslog.LOG_DEBUG)
        if len(relays) == 0:
            time.sleep(poll_interval)
            continue

        # There are running relays
        # Initialize rate and current mapping structures per NUMA node
        mapping_changed = False
        numa_info = {}
        for node, cpus in node_cpus.items():
            numa_info[node] = (list(cpus&set(worker_cores)), {}, [])
        for relay in relays:
            # Rates
            rate = RelayRate(relay)
            rate.estimate_vm2vf_load()
            rate.estimate_vf2vm_load()
            try:
                numa_info[relay.socket_id][1][relay.id] = rate
            except KeyError:
                syslog.syslog(syslog.LOG_WARNING, "%d is not a valid socket id! Relay %d will not form part of the optimization." % (relay.socket_id, relay.id))
                continue
            # Current mapping
            req = relay_pb2.CoreSchedRequest.RelayCPU()
            req.relay_number = relay.id
            req.virtio2vf_cpu = relay.cpu.vm_to_vf
            req.vf2virtio_cpu = relay.cpu.vf_to_vm
            numa_info[relay.socket_id][2].append(req)

        # Merge NUMA infos if global optimization was requested
        if args.global_numa_opt:
            e0 = list(set(worker_cores))
            e1 = {}
            e2 = []
            for numa_node, info in numa_info.items():
                e1.update(info[1])
                e2 = e2 + info[2]
            numa_info = {-1: (e0, e1, e2)}

        # [ [NUMA-local] optimization
        for numa_node, info in numa_info.items():
            workers = info[0]           # set of worker cores
            relay_rates = info[1]       # dict of relay rate objects
            current_mappings = info[2]  # list of core mappings (CoreSchedRequest objects)
            if len(relay_rates) == 0:
                continue

            # Distribute loads
            # Use simple round robin algorithm: Optimal optimization would entail
            # integer programming which may be intractable when there are many
            # workers and/or relays.
            new_mappings, fval = round_robin(relay_rates, workers)

            # Check if a reshuffle is warranted:
            # We use coefficient of variation since it is unitless.
            # If the new mapping results in loads 'tighter' by some margin,
            # apply the new mapping, else do nothing. Parameter might require tuning.
            cv = fval # coefficient of variation
            prev_cv = get_variation_coefficient(current_mappings, relay_rates, workers)
            if cv == -1:
                # No loads are running. Do nothing.
                pass
            elif (cv+args.sensitivity) < prev_cv:
                syslog.syslog(syslog.LOG_INFO, "Migrating workers on NUMA %d..." % numa_node)
                # Trigger cpu migration
                sched_req = relay_pb2.CoreSchedRequest(op=relay_pb2.CoreSchedRequest.UPDATE,
                                                       relay_cpu_map=new_mappings)
                sched_sock.send(sched_req.SerializePartialToString())
                # Gather response
                # Do not attempt infinite reconnect here.
                sched_response = relay_pb2.CoreSchedResponse()
                try:
                    sched_response.ParseFromString(sched_sock.recv())
                    if sched_response.status == relay_pb2.CoreSchedResponse.OK:
                        syslog.syslog(syslog.LOG_INFO, "Scheduler response: OK")
                        mapping_changed = True
                    else:
                        syslog.syslog(syslog.LOG_ERR, "Scheduler response: ERROR")
                except zmq.Again:
                    syslog.syslog(syslog.LOG_ERROR, "Connection to server lost. "
                        "Could not migrate workers on NUMA %d." % numa_node)
            else:
                syslog.syslog(syslog.LOG_INFO, "Worker cores still sufficiently balanced.")
        # ] end [NUMA-local] optimization

        if mapping_changed:
            # Print new mapping
            stats_sock.send(stats_request.SerializePartialToString())
            stats_response = relay_pb2.StatsResponse()
            stats_response.ParseFromString(stats_sock.recv())
            syslog.syslog(syslog.LOG_DEBUG, "New worker core mapping:")
            print_core_mapping(stats_response, worker_cores, syslog.LOG_DEBUG)

        time.sleep(poll_interval)
    # ] End while true

    syslog.syslog(syslog.LOG_NOTICE, "Stopping vio4wd_core_scheduler")
# ] End main().

if __name__ == '__main__':
    main()
