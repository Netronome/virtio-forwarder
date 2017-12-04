# Virtio-forwarder
virtio-forwarder is a userspace networking application that forwards bi-directional traffic between SR-IOV virtual functions and virtio networking devices in virtual machines. virtio-forwarder implements a virtio backend driver using the DPDK’s vhost-user library and services designated VFs by means of the DPDK poll mode driver (PMD) mechanism.

The application supports up to 64 VF <-> virtio forwarding instances. Packets received on the VFs are sent on their corresponding virtio backend and vice versa. The packet relaying principle allows a user to benefit from technologies provided by both NICs and the the virtio network driver. A NIC may offload some or all network functions, while virtio enables VM live migration and is also agnostic to the underlying hardware.

Comprehensive documentation on how to setup host machines for virtualization and configuring virtio-forwarder can be found at http://virtio-forwarder.readthedocs.io/.

### Installation
virtio-forwarder packages are hosted on copr and ppa. To install, add the applicable repository and launch the appropriate package manager:

```
# rpms
yum install yum-plugin-copr
yum copr enable fbotha/virtio-forwarder
yum install virtio-forwarder

# debians
add-apt-repository ppa:fjbotha/virtio-forwarder
apt-get update
apt-get install virtio-forwarder
```
