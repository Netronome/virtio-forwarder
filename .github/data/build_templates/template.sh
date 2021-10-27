#!/bin/bash

DPDK_STABLE="https://dpdk.org/git/dpdk-stable"
STABLE_BRANCH="19.11"
MESON_TARGET="__MESON_TARGET__"  # either 'deb' or 'rpm'

# Clone and checkout the correct dpdk-stable branch
cd /usr/local/src
git clone --depth=1 --single-branch --branch=$STABLE_BRANCH $DPDK_STABLE
cd dpdk-stable

# Attempt to apply any stable patches present
PATCHES_DIR=$GITHUB_WORKSPACE/.github/data/dpdk_patches

for patch in $PATCHES_DIR/*.patch
do
    # Handle the case where the *.patch glob won't expand,
    # for example when no patches are present. Check if
    # the file exists first
    [ -e $patch ] || continue

    # EMAIL environment variable here is not important
    # git am just needs someone as the committer
    EMAIL=root@localhost git am $patch > /dev/null 2>&1
    if [ $? -eq 0 ]
    then
        echo "Successfully applied $patch"
    else
        # Consider this a hard error and exit here.
        # Patches being applied here should not be
        # considered optional
        git am --abort
        echo "WARNING: Could not apply $patch"
        exit 1
    fi
done

# Switch to using the "enable_driver" option once it becomes available in
# the stable version of DPDK to avoid the long driver exclude list
disable_drv_list=("net/af_packet"
                  "net/ark"
                  "net/atlantic"
                  "net/avp"
                  "net/axgbe"
                  "net/bnxt"
                  "net/bnx2x"
                  "net/cxgbe"
                  "net/dpaa"
                  "net/dpaa2"
                  "net/e1000"
                  "net/ena"
                  "net/enetc"
                  "net/enic"
                  "net/failsafe"
                  "net/fm10k"
                  "net/i40e"
                  "net/hinic"
                  "net/hns3"
                  "net/iavf"
                  "net/ice"
                  "net/ifc"
                  "net/ixgbe"
                  "net/kni"
                  "net/liquidio"
                  "net/memif"
                  "net/netvsc"
                  "net/octeontx"
                  "net/octeontx2"
                  "net/pfe"
                  "net/qede"
                  "net/ring"
                  "net/sfc"
                  "net/softnic"
                  "net/tap"
                  "net/thunderx"
                  "net/vdev_netvsc"
                  "net/vmxnet3")

printf -v disable_list '%s,' "${disable_drv_list[@]}"
echo "Disabling the following drivers: ${disable_list%,}"

# Build DPDK
CFLAGS="-mavx -msse -msse4.2" meson -Denable_kmods=false \
      -Dmax_ethports=64 \
      -Ddisable_drivers="${disable_list%,}" \
      -Dmachine=corei7 \
      build

ninja install -C build && ldconfig

# Export the custom libdpdk.pc location to pkg-config
if [ $MESON_TARGET == "deb" ]
then
    export PKG_CONFIG_PATH="/usr/local/lib/x86_64-linux-gnu/pkgconfig/"
    libdpdk_libs="/usr/local/lib/x86_64-linux-gnu/pkgconfig/libdpdk.pc"
    CODENAME=$(lsb_release -c | awk '{print $2}')
    MESON_EXTRA_ARGS="-Ddeb=true -Ddebian_dist=$CODENAME"

# Setup debuild to ignore missing build dependencies
    echo 'DEBUILD_DPKG_BUILDPACKAGE_OPTS="-d"' >> /etc/devscripts.conf

else
    export PKG_CONFIG_PATH="/usr/local/lib64/pkgconfig/"
    libdpdk_libs="/usr/local/lib64/pkgconfig/libdpdk.pc"
    MESON_EXTRA_ARGS="-Drpm=true"
fi

# Fixup the dpdk.pc file. Mainly we need to add --whole-archive
# --no-whole-archive around the static objects created by the DPDK compilation
# otherwise unused symbols are stripped from them, causing the virtio-forwarder
# "whitelist" workaround to fail
sed -i 's/Libs:/Libs: -Wl,--whole-archive/' $libdpdk_libs
sed -i '/Libs:/s/$/ -lrte_bus_pci -lrte_mempool_ring -lrte_mempool_stack -lrte_pmd_vhost -lrte_pmd_nfp -Wl,--no-whole-archive/' $libdpdk_libs

# Compile...
cd $GITHUB_WORKSPACE
meson -Dstatic=true -Doutdir=$GITHUB_WORKSPACE ${MESON_EXTRA_ARGS} build
ninja -v build_$MESON_TARGET -C build

