#!/bin/bash


DPDK_URL=http://git.dpdk.org/dpdk-stable/snapshot/dpdk-stable-19.11.8.tar.gz

# Change to the repo directory

MESON_TARGET="__MESON_TARGET__"  # either 'deb' or 'rpm'

# Compile DPDK using meson
cd /tmp
wget $DPDK_URL
DPDK_DIR=$(tar -tf $(basename $DPDK_URL) | head -n1)
tar -xf $(basename $DPDK_URL)
cd $DPDK_DIR

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
meson -Denable_kmods=false \
      -Dmax_ethports=64 \
      -Ddisable_drivers="${disable_list%,}" \
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

