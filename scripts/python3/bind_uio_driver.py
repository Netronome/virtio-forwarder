#!/usr/bin/python3
# -*- coding: utf-8 -*- #
#
# Copyright 2019, Netronome Systems, Inc.
# SPDX-License-Identifier: BSD-3-Clause
"""Trivial utility to bind a set of virtual functions to a UIO driver."""

import argparse
import os
import re
import textwrap


RE_PCI_ADDR = re.compile(
    r'^([a-f0-9]{4}:|)([a-f0-9]{2}:[a-f0-9]{2}\.[a-f0-9])$',
    flags=re.IGNORECASE)


def _get_sysfs_entry(pci_addr, entry):
    sysfspath = '/sys/bus/pci/devices/%s/%s' % (pci_addr, entry)
    with open(sysfspath, 'r') as sysfsfile:
        return sysfsfile.read()


def _get_driver(pci_addr):
    sysfspath = '/sys/bus/pci/devices/%s/driver' % pci_addr
    try:
        driver = os.path.basename(os.readlink(sysfspath))
    except OSError:
        driver = ''
    return driver


def _unbind_from_driver(pci_addr, driver):
    if _get_driver(pci_addr) == '':
        return

    unbind_path = '/sys/bus/pci/drivers/%s/unbind' % driver
    with open(unbind_path, 'a') as unbind:
        unbind.write(pci_addr)


def bind_driver(pci_addr, driver):
    """Bind one pci device to a kernel driver."""
    current_driver = _get_driver(pci_addr)
    if current_driver == driver:
        return

    # NOTE(jangutter): driver_override is a new race-free way to bind a
    # driver to a specific device, for more info see:
    # https://github.com/torvalds/linux/commit/782a985d7af26db39e86070d28f987c
    override_path = '/sys/bus/pci/devices/%s/driver_override' % pci_addr

    if os.path.exists(override_path):
        with open(override_path, 'w') as driver_override:
            driver_override.write('%s' % driver)
    else:
        # Fall back to classic method: the drawback is that other devices
        # might spontaneously rebind. It's generally not a big issue but
        # sometimes unexpected binding can occur.
        vendor = int(_get_sysfs_entry(pci_addr, 'vendor'), 0)
        device = int(_get_sysfs_entry(pci_addr, 'device'), 0)
        new_id_path = '/sys/bus/pci/drivers/%s/new_id' % driver
        with open(new_id_path, 'w') as new_id:
            new_id.write('%04x %04x' % (vendor, device))

    _unbind_from_driver(pci_addr, current_driver)

    # Instead of drivers_probe, just poke the specific driver with the device
    bind_path = '/sys/bus/pci/drivers/%s/bind' % driver
    try:
        with open(bind_path, 'a') as bind:
            bind.write(pci_addr)
    except IOError:
        if _get_driver(pci_addr) == driver:
            pass
        else:
            raise

    if os.path.exists(override_path):
        with open(override_path, 'w') as driver_override:
            driver_override.write('\00')


def _get_vfs(netdev_arg):
    netdev, sep, count = netdev_arg.partition(':')
    if sep:
        count = int(count)
    else:
        count = None
    pfdir = '/sys/class/net/%s/device' % netdev
    vfns = [fname for fname in os.listdir(pfdir) if fname.startswith('virtfn')]
    vfs = [os.path.basename(os.readlink(os.path.join(pfdir, vfn))).lower()
           for vfn in vfns]
    return sorted(vfs)[:count]


def gen_pci_addr_list(configstr):
    """Parse a config string and return a list of PCI addresses."""
    elements = configstr.split(',')
    pci_addrs = []
    for element in elements:
        entry = element.strip()
        match = RE_PCI_ADDR.match(entry)
        if match:
            if match.groups()[0] == '':
                pci_addr = '0000:%s' % match.groups()[1]
            else:
                pci_addr = ''.join(match.groups())
            pci_addrs.append(pci_addr.lower())
        else:
            pci_addrs.extend(_get_vfs(entry))
    return sorted(list(set(pci_addrs)))


def main():
    """Main function."""
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument('--driver', default='vfio-pci', help=(
        'Driver to bind devices to (default: vfio-pci)'))
    parser.add_argument('devices', help=textwrap.dedent('''
        Comma separated list of devices to bind to the UIO driver.
        The following device notation is supported:
         * 0000:01:02.3 - bind a specific PCI device
         * 01:02.3 - If no prefix is given, 0000: is assumed
         * netdevname - bind all the VFs associated with the netdev
         * netdevname:count - only bind the first \'count\' VF\'s
        Example: ens3,0000:01:02.3,ens4:16'''))
    args = parser.parse_args()
    for pci_addr in gen_pci_addr_list(args.devices):
        bind_driver(pci_addr, args.driver)


if __name__ == "__main__":
    main()
