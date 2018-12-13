#!/usr/bin/python2

# NOTE: This is a hack which duplicates the functionality of the prepare_docs
# make target.
# This file does not follow the goals and principles of your typical setup.py.
# It is used here for a single purpose, and that is to populate the version
# fields in doc/conf.py when readthedocs' builder builds the documentation. This
# file would not exist had rtd implemented a make target hook in its code import
# infrastructure. The only agile hook they do include at this stage is the
# invocation of setup.py; hence this file.

import subprocess
import re

cmd = ['git', 'describe', '--tags', '--long']
output = subprocess.Popen( cmd, stdout=subprocess.PIPE ).communicate()[0]

v_maj = output.split('.')[0]
v_min = output.split('.')[1]
v_patch = output.split('.')[2].split('-')[0]
v_build = output.split('-')[1]
ver_string = "%s.%s.%s.%s" % (v_maj, v_min, v_patch, v_build)

with open ('./doc/conf.py', 'r') as f:
    lines = f.readlines()

lines_new = []
for line in lines:
    tmp = line
    tmp = re.sub('@VRELAY_VERSION@', ver_string, tmp)
    tmp = re.sub('@APP__NAME@', 'virtio-forwarder', tmp)
    lines_new.append(tmp)

with open ('./doc/conf.py', 'w') as f:
    for line in lines_new:
        f.write(line)
