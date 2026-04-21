#!/usr/bin/env bash
# Download Intel Pin 3.30-98830 (if needed) and build the BulletTime pintool.
#
# Pin resolution order:
#   1. PIN_ROOT environment variable, if set and points to a valid Pin kit.
#   2. ./pin/ alongside this script (from a previous run of this script).
#   3. Download Pin 3.30-98830 into ./pin/.
#
# Note: Pin 3.30 was built against Ubuntu 22.04 glibc. Very new kernels
# (e.g. Fedora 42 / 6.16) may refuse to run it at execution time, but the
# build itself only needs a working g++ and 32-bit libstdc++/glibc headers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_PIN_DIR="${SCRIPT_DIR}/pin"
PIN_VERSION="3.30-98830-g1d7b601b3"
PIN_TARBALL="pin-${PIN_VERSION}-gcc-linux.tar.gz"
PIN_URL="https://software.intel.com/sites/landingpage/pintool/downloads/${PIN_TARBALL}"

if [[ -n "${PIN_ROOT:-}" && -x "${PIN_ROOT}/pin" ]]; then
    PIN_DIR="${PIN_ROOT}"
    echo "==> Using existing Pin from PIN_ROOT=${PIN_DIR}"
elif [[ -x "${DEFAULT_PIN_DIR}/pin" ]]; then
    PIN_DIR="${DEFAULT_PIN_DIR}"
    echo "==> Using existing Pin at ${PIN_DIR}"
else
    PIN_DIR="${DEFAULT_PIN_DIR}"
    echo "==> Downloading Pin ${PIN_VERSION} (~33 MB)"
    mkdir -p "${PIN_DIR}"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    curl -fL --progress-bar -o "${tmp}/${PIN_TARBALL}" "${PIN_URL}"
    echo "==> Extracting to ${PIN_DIR}"
    tar -xzf "${tmp}/${PIN_TARBALL}" -C "${PIN_DIR}" --strip-components=1
fi

echo "==> Pin version:"
"${PIN_DIR}/pin" -version 2>&1 | head -2 | sed 's/^/    /'
if ! "${PIN_DIR}/pin" -version 2>&1 | grep -q "pin-3.30-98830"; then
    echo "    WARNING: expected Pin 3.30-98830; build may fail if ABI differs." >&2
fi

echo "==> Building pintool (PIN_ROOT=${PIN_DIR})"
make -C "${SCRIPT_DIR}/src/pintool" PIN_ROOT="${PIN_DIR}" clean >/dev/null
make -C "${SCRIPT_DIR}/src/pintool" PIN_ROOT="${PIN_DIR}" -j"$(nproc)"

PINTOOL_OUT="${SCRIPT_DIR}/src/pintool/obj-intel64/bullettime.so"
if [[ ! -f "${PINTOOL_OUT}" ]]; then
    echo "==> FAIL: ${PINTOOL_OUT} was not produced" >&2
    exit 1
fi

echo "==> Building consumer"
make -C "${SCRIPT_DIR}/src/consumer" clean >/dev/null
make -C "${SCRIPT_DIR}/src/consumer" -j"$(nproc)"

CONSUMER_OUT="${SCRIPT_DIR}/src/consumer/consumer"
if [[ ! -x "${CONSUMER_OUT}" ]]; then
    echo "==> FAIL: ${CONSUMER_OUT} was not produced" >&2
    exit 1
fi

echo "==> SUCCESS:"
echo "    pintool:  ${PINTOOL_OUT}"
echo "    consumer: ${CONSUMER_OUT}"
