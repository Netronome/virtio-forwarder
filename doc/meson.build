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

sphinx = find_program('sphinx-build', required: false)

if sphinx.found()
	# This is a "dummy" configuration_data() object. It's used as a
	# workaround to copy the README.rst and index.rst to the build directory
	# in meson v0.45.1
	dummy_conf_data = configuration_data()

	conf_py_data = configuration_data()
	conf_py_data.set('VRELAY_VERSION', meson.project_version())
	conf_py_data.set('APP__NAME', meson.project_name())
	conf_py = configure_file(input: 'conf.py',
		output: 'conf.py',
		configuration: conf_py_data,
		install: false)
	# need to copy index.rst and README.rst in the build directory
	configure_file(input: 'index.rst',
		output: 'index.rst',
		configuration: dummy_conf_data,
		install: false)
	configure_file(input: 'README.rst',
		output: 'README.rst',
		configuration: dummy_conf_data,
		install: false)

	# always build the manpages, as the makefiles do
	custom_target('man_doc',
		input: meson.current_source_dir(),
		output: 'man8',
		command: [sphinx, '-b', 'man',
			'-d', join_paths(meson.current_build_dir(), '.doctrees'),
			meson.current_build_dir(),
			join_paths(meson.current_build_dir(), 'man8')],
		build_by_default: true,
		install: true,
		install_dir: join_paths(get_option('datadir'), 'man'))

	html_build = custom_target('html_build',
		input: meson.current_source_dir(),
		output: 'html',
		command: [sphinx, '-b', 'html',
			'-d', join_paths(meson.current_build_dir(), '.doctrees'),
			meson.current_build_dir(),
			join_paths(meson.current_build_dir(), 'html')],
		build_by_default: false,
		install: false)

	latex_build = custom_target('latex_build',
		input: meson.current_source_dir(),
		output: 'latex',
		command: [sphinx, '-b', 'latex',
			'-d', join_paths(meson.current_build_dir(), '.doctrees'),
			meson.current_build_dir(),
			join_paths(meson.current_build_dir(), 'latex')],
		build_by_default: false,
		install: false)

	run_target('doc', command: ['echo', 'Building docs:', 'HTML and LateX'],
		depends: [html_build, latex_build])
endif
