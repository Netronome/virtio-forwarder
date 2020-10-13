mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))

# HK: Copr calls this Makefile as follows
# make -f virtio-forwarder/.copr/Makefile srpm outdir="<outdir>"
# The final SRPM should be located in "outdir"
# This is done in a mock chroot of the latest Fedora

.PHONY: srpm
srpm:
	@dnf -y install rpm-build
	@dnf -y install git
	@dnf -y install meson
	# Even though only the source rpm is going to be built by this step meson
	# will check for a valid compilier for the project and bail if it can't
	# find one. Hence gcc is needed here
	@dnf -y install gcc
	# Change directory to source code
	cd $(current_dir)/..
	# This will create a srpm and bail out of the rest of the meson build
	rm -rf build; meson build -Dsrpm=true -Doutdir=$(outdir)
