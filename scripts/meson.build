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

# Default to python2 version in case something fails here
helper_scripts_ver='python2'

if (rhel_family)
    # For any Fedora release, use python3 helper scripts
    if ((distro.stdout().to_lower().contains('fedora') == true) and
        (distro.stdout().to_lower().contains('centos') == false))
        helper_scripts_ver='python3'
    else
        # CentOS/Redhat
        major_ver = run_command('rpm', '--eval', '%{rhel}')

        if (major_ver.stdout().strip() == '7')
            helper_scripts_ver='python2'
        elif (major_ver.stdout().strip() == '8')
            helper_scripts_ver='python3'
        endif
    endif

elif (debian_family)
    # Only focal and bionic will use python3 helper script versions
    if ((distro.stdout().to_lower().contains('focal') == true) or
       (distro.stdout().to_lower().contains('bionic') == true))
        helper_scripts_ver='python3'
    else
        helper_scripts_ver='python2'
    endif
endif

# When generating source packages both subdirectories (python2 and 3)
# will be present in the source package

scripts = files(join_paths(helper_scripts_ver, 'create_zmq_dir.py'),
        join_paths(helper_scripts_ver, 'bind_uio_driver.py'),
        join_paths(helper_scripts_ver, 'parse_numa.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_config.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_core_pinner.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_core_scheduler.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_monitor_load.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_port_control.py'),
        join_paths(helper_scripts_ver, 'virtioforwarder_stats.py'))

install_data(scripts, install_dir: vf_install_dir)
