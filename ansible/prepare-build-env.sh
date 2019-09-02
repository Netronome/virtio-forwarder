#!/bin/sh
# Run playbook to set up system as described in:
# https://wiki.ubuntu.com/SimpleSbuild
ansible-playbook playbook.yaml -i hosts.yaml -v $*
