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

systemd_pc = dependency('systemd', required: false)

if systemd_pc.found()
	service_dir = systemd_pc.get_pkgconfig_variable('systemdsystemunitdir')
endif

if not systemd_pc.found() or service_dir == ''
	service_dir = join_paths('/', 'usr', 'lib', 'systemd', 'system')
endif

service_data = configuration_data()
service_data.set('LIBEXECDIR', join_paths(get_option('prefix'), vf_install_dir))

configure_file(input: 'virtio-forwarder.service',
	output: 'virtio-forwarder.service',
	configuration: service_data,
	install: true,
	install_dir: service_dir)

configure_file(input: 'vio4wd_core_scheduler.service',
	output: 'vio4wd_core_scheduler.service',
	configuration: service_data,
	install: true,
	install_dir: service_dir)
