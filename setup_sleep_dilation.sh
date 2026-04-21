#!/usr/bin/env bash
# Build and insmod the sleep_dilation kernel module (or --clean to rmmod).
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [--clean] [-h|--help]

Build the sleep_dilation kernel module and insmod it.

Options:
  --clean       rmmod the module (if loaded) and clean build artifacts.
  -h, --help    Show this help message.

Requires kernel headers for the running kernel (kernel-devel on Fedora/RHEL,
linux-headers-\$(uname -r) on Debian/Ubuntu). insmod/rmmod are invoked via sudo.

On success, the module exposes two sysfs knobs:
  /sys/kernel/sleep_dilation/dilation_factor   parts per thousand; 1000 = 1.0x
  /sys/kernel/sleep_dilation/target_pid        0 = affect all processes
EOF
}

CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)   CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *)         echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MOD_DIR="${SCRIPT_DIR}/src/kernel_dilation"
MOD_NAME="sleep_dilation"

is_loaded() {
    lsmod | awk '{print $1}' | grep -qx "${MOD_NAME}"
}

if [[ $CLEAN -eq 1 ]]; then
    if is_loaded; then
        echo "==> rmmod ${MOD_NAME}"
        sudo rmmod "${MOD_NAME}"
    else
        echo "==> ${MOD_NAME} not currently loaded"
    fi
    echo "==> make clean"
    make -C "${MOD_DIR}" clean
    exit 0
fi

echo "==> Building ${MOD_NAME} module"
make -C "${MOD_DIR}"

if is_loaded; then
    echo "==> ${MOD_NAME} already loaded; reloading"
    sudo rmmod "${MOD_NAME}"
fi

echo "==> insmod ${MOD_NAME}.ko"
sudo insmod "${MOD_DIR}/${MOD_NAME}.ko"

echo "==> Loaded. sysfs interface:"
echo "    /sys/kernel/sleep_dilation/dilation_factor"
echo "    /sys/kernel/sleep_dilation/target_pid"
