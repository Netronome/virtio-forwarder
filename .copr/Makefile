mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
current_dir := $(notdir $(patsubst %/,%,$(dir $(mkfile_path))))

.PHONY: srpm
srpm:
	@dnf -y install rpm-build
	@dnf -y install git
	$(MAKE) -f $(current_dir)/../Makefile rpm RPMBUILD_FLAGS=bs RPM_OUTDIR=$(outdir)
