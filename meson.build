# BSD 3-Clause License
#
# Copyright 2018 Luca Boccassi <bluca@debian.org>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# * Neither the name of the copyright holder nor the names of its
#   contributors may be used to endorse or promote products derived from
#   this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

project('virtio-forwarder', 'C',
	version: run_command(['git', 'describe', '--tags', '--long']).stdout().strip(),
	license: 'BSD',
	default_options: ['buildtype=release'],
	meson_version: '>= 0.47'
)

dpdk = dependency('libdpdk')
deps = [
	dpdk,
	dependency('libprotobuf-c'),
	dependency('libzmq'),
	dependency('threads'),
]
protoc_c = find_program('protoc-c')
protoc = find_program('protoc')
touch = find_program('touch')

vf_install_dir = join_paths('lib', 'virtio-forwarder')

custom_target('proto-py',
	input: 'virtioforwarder.proto',
	output: 'virtioforwarder_pb2.py',
	command: [protoc, '--proto_path=@CURRENT_SOURCE_DIR@', '--python_out=@OUTDIR@', '@INPUT@'],
	build_by_default: true,
	install: true,
	install_dir: join_paths(vf_install_dir, 'protobuf', 'virtioforwarder'))

custom_target('init',
	output: '__init__.py',
	command: [touch, '@OUTPUT@'],
	build_by_default: true,
	install: true,
	install_dir: join_paths(vf_install_dir, 'protobuf', 'virtioforwarder'))
# non-portable hack - cannot currently install same file in multiple directories
meson.add_install_script('sh', '-c',
	'install -m0644 $MESON_BUILD_ROOT/__init__.py $MESON_INSTALL_DESTDIR_PREFIX/lib/virtio-forwarder/protobuf/__init__.py')

subdir('scripts')

pb_c = generator(protoc_c,
	output: ['@BASENAME@.pb-c.h', '@BASENAME@.pb-c.c'],
	arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--c_out=@BUILD_DIR@', '@INPUT@'])
generated_c = pb_c.process('virtioforwarder.proto')

vrelay_version = configuration_data()
vrelay_version.set('MAJOR', '@0@'.format(meson.project_version().split('.').get(0)))
vrelay_version.set('MINOR', '@0@'.format(meson.project_version().split('.').get(1)))
vrelay_version.set('PATCH', '@0@'.format(meson.project_version().split('.').get(2).split('-').get(0)))
vrelay_version.set('BUILD', '@0@'.format(meson.project_version().split('-').get(-2)))
vrelay_version.set('SHASH', '@0@'.format(meson.project_version().split('-').get(-1)))
vrelay = configure_file(input: 'vrelay_version.h.in',
	output: 'vrelay_version.h',
	configuration: vrelay_version,
	install: false)

cflags = ['-Wno-error=cpp']
cflags += '-std=gnu11'
cflags += '-D_GNU_SOURCE'

# older versions of the dpdk pc file did not include the baseline architecture
# set it to DPDK's old default (it has since been moved to corei7 in 17.08)
# this can be dropped once support for DPDK < 18.11 is no longer necessary
dpdk_cflags = dpdk.get_pkgconfig_variable('cflags')
if host_machine.cpu_family().startswith('x86') and not dpdk_cflags.contains('-march')
	cflags += '-march=core2'
endif

sources = files('argv.c',
	'cmdline.c',
	'cpuinfo.c',
	'dpdk_eal.c',
	'file_mon.c',
	'log.c',
	'ovsdb_mon.c',
	'sriov.c',
	'stats_dump.c',
	'ugid.c',
	'virtio_forwarder_main.c',
	'virtio_vhostuser.c',
	'virtio_worker.c',
	'zmq_config.c',
	'zmq_port_control.c',
	'zmq_server.c',
	'zmq_service.c',
	'zmq_stats.c',
	'zmq_core_sched.c',)

executable('virtio-forwarder',
	[vrelay, generated_c, sources],
	c_args: cflags,
	dependencies: deps,
	install: true)

subdir('startup')
subdir('doc')
