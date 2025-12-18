#!/bin/bash
#
# DPDK Prerequisites Diagnostic Script
# Run this and share the output to check your system readiness
#

echo "=============================================="
echo "   DPDK Prerequisites Diagnostic Report"
echo "=============================================="
echo ""
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo ""

# System info
echo "=== SYSTEM INFO ==="
echo "Kernel: $(uname -r)"
echo "Arch: $(uname -m)"
if [ -f /etc/os-release ]; then
    source /etc/os-release
    echo "OS: $PRETTY_NAME"
fi
echo ""

# Check if running in WSL
echo "=== ENVIRONMENT CHECK ==="
if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "⚠️  WARNING: Running in WSL - DPDK will NOT work here!"
    echo "   DPDK requires native Linux or a VM with PCI passthrough."
elif [ -f /sys/hypervisor/type ]; then
    echo "Running in VM: $(cat /sys/hypervisor/type)"
    echo "Note: DPDK works in VMs but with reduced performance"
else
    echo "Environment: Bare metal or unknown hypervisor"
fi
echo ""

# CPU features
echo "=== CPU FEATURES ==="
echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)"
echo "Cores: $(nproc)"
if grep -q sse4_2 /proc/cpuinfo; then
    echo "✅ SSE4.2: Supported"
else
    echo "❌ SSE4.2: NOT FOUND (required for DPDK)"
fi
if grep -q avx /proc/cpuinfo; then
    echo "✅ AVX: Supported"
else
    echo "⚠️  AVX: Not found (optional)"
fi
if grep -q avx2 /proc/cpuinfo; then
    echo "✅ AVX2: Supported"
else
    echo "⚠️  AVX2: Not found (optional)"
fi
echo ""

# DPDK installation
echo "=== DPDK INSTALLATION ==="
if command -v dpdk-devbind.py &> /dev/null; then
    echo "✅ dpdk-devbind.py: Found at $(which dpdk-devbind.py)"
else
    echo "❌ dpdk-devbind.py: NOT FOUND"
fi

if pkg-config --exists libdpdk 2>/dev/null; then
    DPDK_VERSION=$(pkg-config --modversion libdpdk 2>/dev/null)
    echo "✅ libdpdk: Version $DPDK_VERSION"
else
    echo "❌ libdpdk: NOT FOUND (pkg-config)"
fi

# Check for DPDK libraries
if ldconfig -p 2>/dev/null | grep -q librte; then
    echo "✅ DPDK libraries found in ldconfig"
else
    echo "❌ DPDK libraries NOT in ldconfig"
fi
echo ""

# Huge pages
echo "=== HUGE PAGES ==="
HUGE_TOTAL=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
HUGE_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
HUGE_SIZE=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')

if [ "$HUGE_TOTAL" -gt 0 ]; then
    echo "✅ Huge pages allocated: $HUGE_TOTAL (${HUGE_SIZE}KB each)"
    echo "   Free: $HUGE_FREE"
    echo "   Total memory: $(( HUGE_TOTAL * HUGE_SIZE / 1024 )) MB"
else
    echo "❌ No huge pages allocated (HugePages_Total: 0)"
    echo "   Run: sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'"
fi

# Check hugetlbfs mount
if mount | grep -q hugetlbfs; then
    echo "✅ hugetlbfs mounted at: $(mount | grep hugetlbfs | awk '{print $3}')"
else
    echo "❌ hugetlbfs NOT mounted"
    echo "   Run: sudo mkdir -p /mnt/huge && sudo mount -t hugetlbfs nodev /mnt/huge"
fi
echo ""

# Network interfaces
echo "=== NETWORK INTERFACES ==="
echo "Interfaces:"
ip -br link show 2>/dev/null || ifconfig -a 2>/dev/null | grep -E "^[a-z]"
echo ""

# PCI Network devices
echo "=== PCI NETWORK DEVICES ==="
if command -v lspci &> /dev/null; then
    lspci | grep -i ethernet
    echo ""
    echo "Detailed info (first Ethernet device):"
    FIRST_ETH=$(lspci | grep -i ethernet | head -1 | awk '{print $1}')
    if [ -n "$FIRST_ETH" ]; then
        lspci -v -s "$FIRST_ETH" 2>/dev/null | head -20
    fi
else
    echo "lspci not found. Install pciutils:"
    echo "  sudo apt install pciutils"
fi
echo ""

# ethtool info
echo "=== NIC DRIVER INFO ==="
for iface in $(ip -br link show | awk '{print $1}' | grep -v lo | head -3); do
    if command -v ethtool &> /dev/null; then
        echo "--- $iface ---"
        ethtool -i "$iface" 2>/dev/null | grep -E "driver|version|bus-info" || echo "  (no driver info)"
    else
        echo "ethtool not found. Install:"
        echo "  sudo apt install ethtool"
        break
    fi
done
echo ""

# Kernel modules
echo "=== KERNEL MODULES ==="
echo "UIO modules:"
lsmod | grep -E "^uio" || echo "  (none loaded)"
echo ""
echo "VFIO modules:"
lsmod | grep -E "^vfio" || echo "  (none loaded)"
echo ""

# IOMMU
echo "=== IOMMU STATUS ==="
if dmesg 2>/dev/null | grep -qi "iommu"; then
    echo "IOMMU messages found in dmesg:"
    dmesg 2>/dev/null | grep -i iommu | head -5
else
    echo "No IOMMU messages in dmesg (may need to enable in BIOS/kernel)"
fi
echo ""

# DPDK device binding status
echo "=== DPDK BINDING STATUS ==="
if command -v dpdk-devbind.py &> /dev/null; then
    dpdk-devbind.py --status 2>/dev/null || echo "  (unable to run dpdk-devbind.py)"
else
    echo "dpdk-devbind.py not found"
fi
echo ""

# Summary
echo "=============================================="
echo "                  SUMMARY"
echo "=============================================="

READY=true
WARNINGS=""

# Check WSL
if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "❌ BLOCKER: WSL2 detected - DPDK cannot work"
    READY=false
fi

# Check DPDK
if ! pkg-config --exists libdpdk 2>/dev/null; then
    echo "❌ DPDK not installed"
    echo "   Install: sudo apt install dpdk dpdk-dev libdpdk-dev"
    READY=false
fi

# Check huge pages
if [ "$HUGE_TOTAL" -eq 0 ]; then
    echo "❌ Huge pages not allocated"
    echo "   Run: sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'"
    READY=false
fi

# Check hugetlbfs
if ! mount | grep -q hugetlbfs; then
    echo "❌ hugetlbfs not mounted"
    echo "   Run: sudo mkdir -p /mnt/huge && sudo mount -t hugetlbfs nodev /mnt/huge"
    READY=false
fi

# Check NIC
if ! lspci 2>/dev/null | grep -qi ethernet; then
    echo "⚠️  No PCI Ethernet devices detected"
    WARNINGS="$WARNINGS NIC"
fi

echo ""
if [ "$READY" = true ]; then
    echo "✅ System appears ready for DPDK!"
    echo "   Next step: Bind a NIC to DPDK driver"
else
    echo "⚠️  System NOT ready for DPDK - see issues above"
fi
echo ""
echo "=============================================="
