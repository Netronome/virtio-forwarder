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
    meson_version: '>= 0.45.1'
)

# Attempt to detect the Distro we are building on
vf_install_dir = ''
rhel_family = false
debian_family = false
distro = run_command('cat', '/etc/os-release')

if (distro.stdout().to_lower().contains('fedora') or
    distro.stdout().to_lower().contains('centos') or
    distro.stdout().to_lower().contains('rhel'))
    message('Fedora/CentOS/Rhel detected')
    vf_install_dir = join_paths('lib64', 'virtio-forwarder')
    rhel_family = true
elif (distro.stdout().to_lower().contains('debian') or
    distro.stdout().to_lower().contains('ubuntu'))
    message('Debian/Ubuntu detected')
    vf_install_dir = join_paths('lib', 'virtio-forwarder')
    debian_family = true
else
    error('Unknown distro detected!')
endif

# If the project version string is empty it is likley that the git command used
# to grab the version failed. In this situasion meson was probably called from a
# source tree with the git info stripped out (source rpm and debs).
if meson.project_version() == ''
    message('Not running in a git tree')
else
    # Populate vrelay_version.h with the project versions
    vrelay_version = configuration_data()
    vrelay_version.set('MAJOR', '@0@'.format(meson.project_version().split('.').get(0)))
    vrelay_version.set('MINOR', '@0@'.format(meson.project_version().split('.').get(1)))
    vrelay_version.set('PATCH', '@0@'.format(meson.project_version().split('.').get(2).split('-').get(0)))
    vrelay_version.set('BUILD', '@0@'.format(meson.project_version().split('-').get(-2)))
    vrelay_version.set('SHASH', '@0@'.format(meson.project_version().split('-').get(-1)))
    vrelay = configure_file(input: 'vrelay_version.h.in',
        output: 'vrelay_version.h',
        configuration: vrelay_version)
endif

vrelay_file_root = join_paths(meson.source_root(), 'vrelay_version.h')
vrelay_file_build = join_paths(meson.build_root(), 'vrelay_version.h')

if (run_command('[', '-e', vrelay_file_build, ']').returncode() == 0)
    vrelay = vrelay_file_build
elif (run_command('[', '-e', vrelay_file_root, ']').returncode() == 0)
    vrelay = vrelay_file_root
else
    # This shouldn't happen ordinarily. Either vrelay_version.h was generated
    # from the git tree or it was included in the root source directory by the
    # packaging logic
    error('vrelay_version.h not found! Exiting...')
endif

# Hook to call the packaging meson scripts from the virtio-forwarder-dist repo
if get_option('rpm') or get_option('srpm') or get_option('deb') or get_option('dsc')
    # clone packaging repo and include as subdir
    dist_repo = 'https://github.com/Netronome/virtio-forwarder-dist'
    git_clone = run_command('git', 'clone', dist_repo, 'packaging')
    subdir('packaging')
    # The copr build system will typically call this meson file with the option
    # "srpm". This is done in a mock chroot, usually of the latest Fedora. In
    # order to avoid any unnecessary installs in the chroot meson will stop here
    #
    # For PPA uploads a signed .dsc file (among other things) needs to be generated
    # The scripts in the virtio-forwarder-dist repo will do this for us.
    if get_option('srpm') or get_option('dsc')
        subdir_done()
    endif
endif

# Upstream dpdk-devel (RHEL/CentOS) does not yet ship a libdpdk.pc
# file or static versions of the DPDK libraries. Use this script
# to attempt to detect what libraries are installed and generate a
# custom libdpdk.pc.
if rhel_family
    libdpdk_pc = join_paths('/usr/local/lib64/pkgconfig/' , 'libdpdk.pc')
    if (run_command('[', '-e', libdpdk_pc, ']').returncode() != 0)
        message('Creating libdpdk.pc file')
        pkgconfig_builder = find_program('pkg_config_builder.py')
        r = run_command(pkgconfig_builder)
        if r.returncode() != 0
            error(r.stderr().strip())
        else
            message(r.stdout().strip())
        endif
    else
        # Use the libdpdk.pc found on the system
        message('Using libdpdk.pc located at: ' + libdpdk_pc)
    endif
endif

protoc_c = find_program('protoc-c')
protoc = find_program('protoc')
touch = find_program('touch')

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
if rhel_family
    meson.add_install_script('sh', '-c',
      'install -m0644 $MESON_BUILD_ROOT/__init__.py $MESON_INSTALL_DESTDIR_PREFIX/lib64/virtio-forwarder/protobuf/__init__.py')
elif debian_family
    meson.add_install_script('sh', '-c',
      'install -m0644 $MESON_BUILD_ROOT/__init__.py $MESON_INSTALL_DESTDIR_PREFIX/lib/virtio-forwarder/protobuf/__init__.py')
endif

subdir('scripts')

pb_c = generator(protoc_c,
    output: ['@BASENAME@.pb-c.h', '@BASENAME@.pb-c.c'],
    arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--c_out=@BUILD_DIR@', '@INPUT@'])
generated_c = pb_c.process('virtioforwarder.proto')

if get_option('static')
  dpdk = dependency('libdpdk', static : true)
else
  dpdk = dependency('libdpdk')
endif

cflags = ['-Wno-error=cpp']
cflags += '-std=gnu11'
cflags += '-D_GNU_SOURCE'
cflags += '-mavx'

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

if not get_option('static')
  if dpdk.version().version_compare('>=20.11')
    dpdk = declare_dependency(dependencies : dpdk, link_args : '-lrte_net_bond')
  endif
endif

deps = [
    dpdk,
    dependency('libprotobuf-c'),
    dependency('libzmq'),
    dependency('threads')]

executable('virtio-forwarder',
    [vrelay, generated_c, sources],
    c_args: cflags,
    dependencies: deps,
    install: true)


subdir('startup')
subdir('doc')
