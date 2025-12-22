#!/bin/bash
# DPDK Quick Setup for Pop!_OS

echo "=== Installing DPDK ==="
sudo apt update
sudo apt install -y dpdk dpdk-dev libdpdk-dev dpdk-doc

echo ""
echo "=== Allocating Huge Pages ==="
sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'

# Make permanent
if ! grep -q "vm.nr_hugepages" /etc/sysctl.conf; then
    echo 'vm.nr_hugepages = 1024' | sudo tee -a /etc/sysctl.conf
fi

echo ""
echo "=== Loading Kernel Modules ==="
sudo modprobe uio_pci_generic

# Make permanent
if [ ! -f /etc/modules-load.d/dpdk.conf ]; then
    echo 'uio_pci_generic' | sudo tee /etc/modules-load.d/dpdk.conf
fi

echo ""
echo "=== Verifying Installation ==="
echo "DPDK version: $(pkg-config --modversion libdpdk 2>/dev/null || echo 'NOT FOUND')"
echo "Huge pages: $(cat /proc/meminfo | grep HugePages_Total)"
echo "UIO module: $(lsmod | grep uio || echo 'NOT LOADED')"

echo ""
echo "=== Done! ==="
echo "Run ./check_dpdk_prereq.sh again to verify everything is ready."#!/bin/bash
# DPDK Quick Setup for Pop!_OS

echo "=== Installing DPDK ==="
sudo apt update
sudo apt install -y dpdk dpdk-dev libdpdk-dev dpdk-doc

echo ""
echo "=== Allocating Huge Pages ==="
sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'

# Make permanent
if ! grep -q "vm.nr_hugepages" /etc/sysctl.conf; then
    echo 'vm.nr_hugepages = 1024' | sudo tee -a /etc/sysctl.conf
fi

echo ""
echo "=== Loading Kernel Modules ==="
sudo modprobe uio_pci_generic

# Make permanent
if [ ! -f /etc/modules-load.d/dpdk.conf ]; then
    echo 'uio_pci_generic' | sudo tee /etc/modules-load.d/dpdk.conf
fi

echo ""
echo "=== Verifying Installation ==="
echo "DPDK version: $(pkg-config --modversion libdpdk 2>/dev/null || echo 'NOT FOUND')"
echo "Huge pages: $(cat /proc/meminfo | grep HugePages_Total)"
echo "UIO module: $(lsmod | grep uio || echo 'NOT LOADED')"

echo ""
echo "=== Done! ==="
echo "Run ./check_dpdk_prereq.sh again to verify everything is ready."
