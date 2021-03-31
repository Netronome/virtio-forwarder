function install_package {
    if [ "$SUB_OS" = "none" ]; then
        sudo apt-get install -q -y python3 python3-pip python3-setuptools python3-wheel \
            protobuf-compiler protobuf-c-compiler libprotobuf-c-dev libnuma-dev libzmq3-dev ninja-build libbsd-dev
    elif [ "$SUB_OS" = "centos:7" ]; then
        yum install -q -y epel-release sudo git gcc
        sudo yum install -q -y python3 python3-pip python3-setuptools python3-wheel \
            protobuf-compiler protobuf-c-compiler protobuf-c-devel numactl-devel czmq-devel ninja-build libbsd-devel
    elif [ "$SUB_OS" = "centos:8" ]; then
        dnf install -q -y epel-release sudo git gcc
        sudo dnf --enablerepo=powertools install -q -y protobuf-compiler protobuf-c-compiler protobuf-c-devel ninja-build
        sudo dnf install -q -y python3 python3-pip python3-setuptools python3-wheel numactl-devel czmq-devel libbsd-devel
    elif [[ "$SUB_OS" = "fedora"* ]]; then
        dnf install -q -y git gcc
        sudo dnf install -q -y python3 python3-pip python3-setuptools python3-wheel \
            protobuf-compiler protobuf-c-compiler protobuf-c-devel ninja-build numactl-devel czmq-devel libbsd-devel
    else
        echo "Not supported OS: $SUB_OS"
        exit 1
    fi
}

function install_meson {
    sudo pip3 install -Iq meson==0.57.1
}

function build_dpdk {
    pushd dpdk
    OPTS="-Dtests=false -Denable_kmods=false -Dmachine=default -Ddefault_library=$DEF_LIB"
    meson build $OPTS
    sudo ninja -C build install
    popd
    sudo rm -rf dpdk
}

function pkg_config {
    export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig
}

function build_xvio {
    meson build
    sudo ninja -C build install
}

install_package &&
install_meson   &&
build_dpdk      &&
pkg_config      &&
build_xvio

