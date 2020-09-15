mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))

# HK: Copr calls this Makefile as follows
# make -f virtio-forwarder/.copr/Makefile srpm outdir="<outdir>"
# The final RPM should be located in "outdir"

.PHONY: srpm
srpm:
	@dnf -y install rpm-build
	@dnf -y install git
	@dnf -y install meson
	# Change directory to source code
	cd $(current_dir)/..
	rm -rf build; meson build -Drpm=true -Doutdir=$(outdir)
	# Finally call ninja to build the rpm
	ninja -C build build_rpm
