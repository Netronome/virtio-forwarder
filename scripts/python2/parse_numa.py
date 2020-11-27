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

import subprocess

def get_max_node():
    out = subprocess.check_output(
        'cat /proc/cpuinfo | grep "physical id" | sort -u | wc -l',
        shell=True
        )
    try:
        return int(out) - 1
    except ValueError:
        print "Error gathering the number of NUMA nodes!"
        return -1

def get_node_cpus(node):
    out = subprocess.check_output(['lscpu', '--parse=cpu,node'])
    node_cpus = {}
    for line in out.splitlines():
        if line[0] == '#':
            continue

        try:
            cpu, n = line.split(',')
            cpu = int(cpu)
            n = int(n)
        except ValueError:
            continue

        try:
            node_cpus[n].append(cpu)
        except KeyError:
            node_cpus[n] = [cpu]

    return set(node_cpus[node])

if __name__ == '__main__':
    print "NUMA node(s):\t\t%d" % (get_max_node() + 1)
    for node in range(get_max_node() + 1):
        node_cpus = get_node_cpus(node)
        conv = '%d,' * len(node_cpus)
        print "NUMA node%d CPU(s):\t" % node, conv[:-1] % tuple(node_cpus)
