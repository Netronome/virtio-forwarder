#!/usr/bin/python3

import os
import sys
import re
from jinja2 import Template
import subprocess

if os.path.exists("/usr/lib64"):
    # Rhel/CentOS/Fedora
    library_path = "/usr/lib64"
    pkg_config_path = os.path.join(library_path, "pkgconfig")
    dpdk_version = subprocess.check_output(
        ['rpm', '-q', '--qf', '%{VERSION}', 'dpdk']).decode('UTF-8')
else:
    # Ubuntu/Debian
    library_path = "/usr/lib"
    pkg_config_path = os.path.join(library_path,
        "x86_64-linux-gnu/pkgconfig/pkgconfig")
    dpkg_query_out = subprocess.check_output(
        ['dpkg-query', '-f', '${Version}', '-W', 'dpdk']).decode('UTF-8')
    dpdk_version = dpkg_query_outplit('-')[0]

if (dpdk_version == ''):
    print("Could not find DPDK package", file=sys.stderr)
    exit(1)

dpdk_libraries = []

# Order is important here
dpdk_libraries.append('-Wl,--whole-archive -L${libdir}')

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_common.*\.so$', lib)]

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_bus.*\.so$', lib)]

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_mempool.*\.so$', lib)]

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_raw.*\.so$', lib)]

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_pmd.*\.so$', lib)]

dpdk_libraries.append('-Wl,--no-whole-archive')

dpdk_libraries += [os.path.splitext(lib)[0] for lib in os.listdir(library_path)
        if re.search(r'librte_(?!common|bus|mempool|raw|pmd).*\.so$', lib)]

dpdk_libraries = [x.replace('librte', '-lrte') for x in dpdk_libraries]

template = """prefix={{ prefix }}
libdir=${prefix}/{{ os_lib_path }}
includedir=${prefix}/include/dpdk

Name: {{ name }}
Description: {{ description }}
Version: {{ version }}
Libs: {{ libraries }}
Cflags: -I${includedir} -include rte_config.h -march=corei7"""

config_data = {
        "prefix": "/usr",
        "os_lib_path": os.path.split(library_path)[1],
        "name": "DPDK",
        "description" : "Manually built libdpdk.pc file",
        "version" : dpdk_version,
        "libraries": " ".join(dpdk_libraries)
        }

j2_template = Template(template)

if not os.path.exists(os.path.join(pkg_config_path, "libdpdk.pc")):
    with open(os.path.join(pkg_config_path, "libdpdk.pc"), "w") as f:
        f.write(j2_template.render(config_data))
        f.flush()
        os.fsync(f)

print("Wrote {}".format(os.path.join(pkg_config_path, "libdpdk.pc")))
exit(0)
