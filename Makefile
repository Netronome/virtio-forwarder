#   BSD LICENSE
#
#   Copyright(c) 2016-2017 Netronome.
#   All rights reserved.
#
#   Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions
#   are met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#     * Neither the name of Netronome nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
#   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

ARCH ?= x86_64
RTE_SDK ?= /usr/share/dpdk
RTE_TARGET ?= $(ARCH)-default-linuxapp-gcc

ifeq ($(wildcard $(RTE_SDK)),)
    $(warning RTE_SDK=$(RTE_SDK) does not exist!)
else
    include $(RTE_SDK)/mk/rte.vars.mk
endif

# Install utility
INSTALL = install
INSTALL_PROGRAM = $(INSTALL) -m 755
INSTALL_DATA = $(INSTALL) -m 644

# Default directories
prefix ?= /usr/local
bindir = $(prefix)/bin
libexecdir ?= $(prefix)/lib/virtio-forwarder
mandir = $(prefix)/share/man/man8
unitdir ?= /usr/lib/systemd/system
VIO4WD_SHIP_UPSTART ?= n

# binary name
APP = virtio-forwarder

# Default Ubuntu distro
DEBIAN_DISTRO ?= unstable
PKG_RELEASE = 1

# Path and branch of distribution repo. Note, the distro tag should dictate the
# branch specified here.
DIST_REPO_URL ?= https://github.com/Netronome/virtio-forwarder-dist
DIST_REPO_BRANCH ?= master

# all source are stored in SRCS-y
SRCS-y := \
    argv.c \
    cmdline.c \
    cpuinfo.c \
    dpdk_eal.c \
    file_mon.c \
    log.c \
    ovsdb_mon.c \
    sriov.c \
    ugid.c \
    virtio_forwarder_main.c \
    virtio_vhostuser.c \
    virtio_worker.c \
    zmq_config.c \
    zmq_port_control.c \
    zmq_server.c \
    zmq_service.c \
    zmq_stats.c \
    zmq_core_sched.c \
    pb2/virtioforwarder.pb-c.c

CFLAGS += -O2 -g -Wno-error=cpp
CFLAGS += -std=gnu11
CFLAGS += -D_GNU_SOURCE
#CFLAGS += -DVIRTIO_ECHO -Wno-error=unused-but-set-variable -Wno-error=unused-parameter -Wno-error=unused-function
CFLAGS += $(WERROR_FLAGS)
CFLAGS += -Ipb2

LDLIBS += -lprotobuf-c -lzmq

ifeq ($(CONFIG_RTE_BUILD_SHARED_LIB),y)
ifneq ($(CONFIG_RTE_BUILD_COMBINE_LIBS),y)
ifeq ($(CONFIG_RTE_LIBRTE_PMD_BOND),y)
LDLIBS += -lrte_pmd_bond
endif
endif
endif

.PHONY: all version deb rpm vio_install vio_uninstall vio_installdirs doc prepare_docs manual

#
# Add requiremnts to targets that may be predefined in DPDK makefiles:
#

# Default goal
build: version
.DEFAULT_GOAL := build

.PHONY: install
install: target-appinstall vio_install

# The man page should be created and distributed whenever the binary is,
# therefore add it to the internal _postbuild target as a prerequisite.
_postbuild: manual

.PHONY: uninstall
uninstall: vio_uninstall

all: version

.PHONY: clean, vio_clean
clean: vio_clean

POSTBUILD := protobuf/virtioforwarder/virtioforwarder_pb2.py

ifneq ($(wildcard $(RTE_SDK)),)
    include $(RTE_SDK)/mk/rte.extapp.mk
endif

define mkdir
    test -d '$(@D)' || mkdir -p '$(@D)'
endef

define touch
    test -e '$1' || touch '$1'
endef

# Need explicit dependencies from .o files consuming Protocol Buffers .h files
# to the .h files themselves. Otherwise the correctness of the build depends on
# the order of SRCS-y, and parallel builds may randomly fail.
zmq_config.o zmq_port_control.o zmq_stats.o zmq_core_sched.o: pb2/virtioforwarder.pb-c.h

# The .h is generated before the .c so let's make the .c depend on the .h.
pb2/virtioforwarder.pb-c.c: pb2/virtioforwarder.pb-c.h

pb2/virtioforwarder.pb-c.h: virtioforwarder.proto
	@echo "  PROTOC-C $(<F)"
	$(Q)$(mkdir)
	$(Q)protoc-c -I'$(SRCDIR)' --c_out='$(@D)' '$<'

protobuf/virtioforwarder/virtioforwarder_pb2.py: virtioforwarder.proto
	@echo "  PROTOC-PYTHON $(<F)"
	$(Q)$(mkdir)
	$(Q)$(call touch,protobuf/__init__.py)
	$(Q)$(call touch,protobuf/virtioforwarder/__init__.py)
	$(Q)protoc -I'$(SRCDIR)' --python_out='$(@D)' '$<'

version:
	@(git log>/dev/null 2>&1 && \
	cp $${RTE_SRCDIR:+$$RTE_SRCDIR/}vrelay_version.h.in vrelay_version.h 2>/dev/null && \
	cp $${RTE_SRCDIR:+$$RTE_SRCDIR/}meson.build meson.build.out 2>/dev/null && \
	sed -ri "s/^(\s*version: )run_command.*$$/\1\'$(shell git describe --tags --long)\',/" meson.build.out && \
	sed -ri "s/@MAJOR@/$(shell git describe --tags --long | sed -r 's/([0-9]+)\.([0-9]+)\.([0-9]+)\-.*/\1/')/" vrelay_version.h && \
	sed -ri "s/@MINOR@/$(shell git describe --tags --long | sed -r 's/([0-9]+)\.([0-9]+)\.([0-9]+)\-.*/\2/')/" vrelay_version.h && \
	sed -ri "s/@PATCH@/$(shell git describe --tags --long | sed -r 's/([0-9]+)\.([0-9]+)\.([0-9]+)\-.*/\3/')/" vrelay_version.h && \
	sed -ri "s/@BUILD@/$(shell git describe --tags --long | sed -r 's/(.*)?\-([0-9]+)\-.*/\2/')/" vrelay_version.h && \
	sed -ri "s/@SHASH@/$(shell git describe --tags --long | sed -r 's/(.*)?\-([0-9]+)\-(.*)/\3/')/" vrelay_version.h) || \
	cp $${RTE_SRCDIR:+$$RTE_SRCDIR/}vrelay_version.h . || :

vio_installdirs:
	mkdir -p $(DESTDIR)$(bindir)
	mkdir -p $(DESTDIR)$(libexecdir)
	mkdir -p $(DESTDIR)/etc/default
	mkdir -p $(DESTDIR)$(unitdir)
	@if [ "$(VIO4WD_SHIP_UPSTART)" = "y" ]; then \
		mkdir -p $(DESTDIR)/etc/init; \
	fi
	mkdir -p $(DESTDIR)$(mandir)

vio_install: vio_installdirs
	$(INSTALL_PROGRAM) $(RTE_OUTPUT)/$(APP) $(DESTDIR)$(bindir)
	$(INSTALL_DATA) $(RTE_OUTPUT)/_build/$(APP).8 $(DESTDIR)$(mandir)
	find $(RTE_SRCDIR)/scripts/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.py' -exec $(INSTALL_PROGRAM) {} $(DESTDIR)$(libexecdir) \;
	find $(RTE_SRCDIR)/startup/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.sh' -exec $(INSTALL_PROGRAM) {} $(DESTDIR)$(libexecdir) \;
	find $(RTE_SRCDIR)/startup/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.sh' -exec sh -c 'sed -ri "s#@LIBEXECDIR@#$(libexecdir)#" $(DESTDIR)$(libexecdir)/$$(basename {})' \;
	$(INSTALL_DATA) $(RTE_SRCDIR)/startup/virtioforwarder $(DESTDIR)/etc/default
	sed -ri "s#@BINDIR@#$(bindir)#" $(DESTDIR)/etc/default/virtioforwarder
	find $(RTE_SRCDIR)/startup/systemd -maxdepth 1 -type f -regextype posix-extended -regex '.*\.service' -exec $(INSTALL_DATA) {} $(DESTDIR)$(unitdir) \;
	find $(RTE_SRCDIR)/startup/systemd -maxdepth 1 -type f -regextype posix-extended -regex '.*\.service' -exec sh -c 'sed -ri "s#@LIBEXECDIR@#$(libexecdir)#" $(DESTDIR)$(unitdir)/$$(basename {})' \;
	@if [ "$(VIO4WD_SHIP_UPSTART)" = "y" ]; then \
		find $(RTE_SRCDIR)/startup/upstart -maxdepth 1 -type f -regextype posix-extended -regex '.*\.conf' -exec $(INSTALL_DATA) {} $(DESTDIR)/etc/init \;; \
		find $(RTE_SRCDIR)/startup/upstart -maxdepth 1 -type f -regextype posix-extended -regex '.*\.conf' -exec sh -c 'sed -ri "s#@LIBEXECDIR@#$(libexecdir)#" $(DESTDIR)/etc/init/$$(basename {})' \;; \
	fi
	cd $(RTE_OUTPUT); \
	find ./protobuf/ -type f -regextype posix-extended -regex '.*\.py' -exec $(INSTALL_DATA) -D {} $(DESTDIR)$(libexecdir)/{} \;

vio_uninstall:
	@rm -f $(DESTDIR)$(bindir)/$(APP)
	@find $(RTE_SRCDIR)/scripts/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.py' -exec sh -c 'rm -f $(DESTDIR)$(libexecdir)/$$(basename {})*' \;
	@find $(RTE_SRCDIR)/startup/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.sh' -exec sh -c 'rm -f $(DESTDIR)$(libexecdir)/$$(basename {})' \;
	@rm -f $(DESTDIR)/etc/default/virtioforwarder
	@find $(RTE_SRCDIR)/startup/systemd -maxdepth 1 -type f -regextype posix-extended -regex '.*\.service' -exec sh -c 'rm -f $(DESTDIR)$(unitdir)/$$(basename {})' \;
	@if [ "$(VIO4WD_SHIP_UPSTART)" = "y" ]; then \
		find $(RTE_SRCDIR)/startup/upstart -maxdepth 1 -type f -regextype posix-extended -regex '.*\.conf' -exec sh -c 'rm -f $(DESTDIR)/etc/init/$$(basename {})' \;; \
	fi
	@rm -rf $(DESTDIR)$(libexecdir)/protobuf/

vio_clean:
	@rm -rf $(RTE_OUTPUT)/_build

deb: version
	@rm -fr _build; mkdir -p _build/virtio-forwarder
	@find . -maxdepth 1 -type f -regextype posix-extended -regex '.*(README.rst|README.md|Makefile|\.(c|h|proto))' -exec cp {} _build/virtio-forwarder/ \;
	@find scripts/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.py' -exec cp --parents {} _build/virtio-forwarder/ \;
	@cp --parents startup/virtioforwarder _build/virtio-forwarder/
	@cp --parents startup/systemd/*.service _build/virtio-forwarder/
	@if [ "$(VIO4WD_SHIP_UPSTART)" = "y" ]; then \
		cp --parents startup/upstart/*.conf _build/virtio-forwarder/; \
	fi
	@cp -r doc/ _build/virtio-forwarder/
	@cp vrelay_version.h.in _build/virtio-forwarder/
	@find . -not -path "./_build/*" -type f -name "meson.build" -exec cp --parents {} _build/virtio-forwarder/ \;
	@cp meson.build.out _build/virtio-forwarder/meson.build
	@find startup/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.sh' -exec cp --parents {} _build/virtio-forwarder/ \;
	cd _build; \
	VERSION_VER_STRING="$(shell awk '/VIRTIO_FWD_VERSION/&&/define/&&!/SHASH/{count++; if (count<4) printf "%s.", $$3; else print $$3}' vrelay_version.h)"; \
	mv virtio-forwarder/ virtio-forwarder-$$VERSION_VER_STRING; \
	tar cfjp virtio-forwarder_$${VERSION_VER_STRING}.orig.tar.bz2 virtio-forwarder-$$VERSION_VER_STRING/; \
	git clone -b $(DIST_REPO_BRANCH) $(DIST_REPO_URL) packaging; \
	cp -r ./packaging/debian virtio-forwarder-$$VERSION_VER_STRING/; \
	sed -ri "s/__VRELAY_VERSION__/$$VERSION_VER_STRING/" virtio-forwarder-$$VERSION_VER_STRING/debian/changelog; \
	sed -ri "s/__PKG_RELEASE__/$(PKG_RELEASE)/" virtio-forwarder-$$VERSION_VER_STRING/debian/changelog; \
	cd virtio-forwarder-$$VERSION_VER_STRING/; \
	DATE_STR="$(shell date --rfc-2822)"; \
	sed -ri "s/__DEBIAN_DIST__/$(DEBIAN_DISTRO)/g" ./debian/changelog; \
	sed -ri "s/__DATE__/$$DATE_STR/g" ./debian/changelog; \
	debuild --rootcmd=fakeroot -e RTE_SDK -e RTE_TARGET -e PATH -e CFLAGS \
	-e V -e VIO4WD_SHIP_UPSTART -us -uc

rpm: version
	mkdir -p rpmbuild/BUILD
	mkdir -p rpmbuild/RPMS
	mkdir -p rpmbuild/SOURCES
	mkdir -p rpmbuild/SPECS
	mkdir -p rpmbuild/SRPMS
	@rm -fr _build; mkdir -p _build/virtio-forwarder
	@find . -maxdepth 1 -type f -regextype posix-extended -regex '.*(LICENSE|README.rst|README.md|Makefile|\.(c|h|proto))' -exec cp {} _build/virtio-forwarder/ \;
	@find scripts/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.py' -exec cp --parents {} _build/virtio-forwarder/ \;
	@cp --parents startup/virtioforwarder _build/virtio-forwarder/
	@cp --parents startup/systemd/*.service _build/virtio-forwarder/
	@cp -r doc/ _build/virtio-forwarder/
	@find startup/ -maxdepth 1 -type f -regextype posix-extended -regex '.*\.sh' -exec cp --parents {} _build/virtio-forwarder/ \;
	cd _build; \
	VERSION_VER_STRING="$(shell awk '/VIRTIO_FWD_VERSION/&&/define/&&!/SHASH/{count++; if (count<4) printf "%s.", $$3; else print $$3}' vrelay_version.h)"; \
	git clone -b $(DIST_REPO_BRANCH) $(DIST_REPO_URL) packaging; \
	cp ./packaging/virtio-forwarder.spec.in ../rpmbuild/SPECS/virtio-forwarder.spec; \
	sed -ri "s/__VRELAY_VERSION__/$$VERSION_VER_STRING/" ../rpmbuild/SPECS/virtio-forwarder.spec; \
	sed -ri "s/__PKG_RELEASE__/$(PKG_RELEASE)/" ../rpmbuild/SPECS/virtio-forwarder.spec; \
	DATE_STR="$(shell date +'%a %b %d %Y')"; \
	sed -ri "s/__DATE__/$$DATE_STR/g" ../rpmbuild/SPECS/virtio-forwarder.spec; \
	mv virtio-forwarder/ virtio-forwarder-$$VERSION_VER_STRING; \
	tar cfjp ../rpmbuild/SOURCES/virtio-forwarder-$$VERSION_VER_STRING-$(PKG_RELEASE).tar.bz2 virtio-forwarder-$$VERSION_VER_STRING/; \
	rpmbuild -$${RPMBUILD_FLAGS:-ba} -D "_topdir $(shell pwd)/rpmbuild/" ../rpmbuild/SPECS/virtio-forwarder.spec; \
	find ../rpmbuild/ -name "*.rpm" -exec cp {} $${RPM_OUTDIR:-.} \;

prepare_docs: version
	@rm -fr _build; mkdir -p _build/
	cp $${RTE_SRCDIR:+$$RTE_SRCDIR/}doc/* _build/
	cd _build; \
	VERSION_VER_STRING="$(shell awk '/VIRTIO_FWD_VERSION/&&/define/&&!/SHASH/{count++; if (count<4) printf "%s.", $$3; else print $$3}' vrelay_version.h)"; \
	sed -ri "s/@VRELAY_VERSION@/$$VERSION_VER_STRING/" conf.py; \
	sed -ri "s/@APP__NAME@/$(APP)/" conf.py

doc: prepare_docs
	cd _build; \
	$(MAKE) latexpdf; \
	$(MAKE) html; \
	cp _build/latex/Virtio_forwarder.pdf .; \
	cp -r _build/html .

manual: prepare_docs
	cd _build; \
	env -i PATH=$$PATH $(MAKE) man; \
	cp _build/man/$(APP).8 .
