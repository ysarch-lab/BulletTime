#!/usr/bin/env bash
# Launch a BulletTime trace: starts the consumer, runs the target program
# under Pin + bullettime.so, and waits for both to finish cleanly.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] -- <command> [args...]

Options:
  -o, --output DIR          Output directory for traces (required)
  -f, --fcalls NAMES        Comma-separated function names identifying key threads
      --exact-fcalls        Require exact match on -f names (default: substring)
      --no-compression      Disable zstd trace compression (default: enabled)
      --no-app-dilation     Disable application-thread time dilation (default: enabled)
      --no-kernel-dilation  Disable kernel-thread time dilation (default: enabled)
      --pin-root PATH       Intel Pin kit location (default: ./pin alongside script)
  -h, --help                Show this help

Prerequisites:
  - ./setup_pintool.sh has been run (produces pin/, bullettime.so, consumer).
  - Hugepages are reserved (e.g. echo 1024 | sudo tee /proc/sys/vm/nr_hugepages).
  - If --kernel-dilation is enabled (default), setup_sleep_dilation.sh has insmod'd the module.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIN_ROOT="${SCRIPT_DIR}/pin"
OUTPUT_DIR=""
FCALLS=""
FCALLS_EXACT=0
USE_COMPRESSION=1
USE_APP_DIL=1
USE_KERNEL_DIL=1

CMD=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)             OUTPUT_DIR="$2"; shift 2 ;;
        -f|--fcalls)             FCALLS="$2"; shift 2 ;;
        --exact-fcalls)          FCALLS_EXACT=1; shift ;;
        --no-compression)        USE_COMPRESSION=0; shift ;;
        --no-app-dilation)       USE_APP_DIL=0; shift ;;
        --no-kernel-dilation)    USE_KERNEL_DIL=0; shift ;;
        --pin-root)              PIN_ROOT="$2"; shift 2 ;;
        -h|--help)               usage; exit 0 ;;
        --)                      shift; CMD=("$@"); break ;;
        *)                       echo "Unknown option: $1" >&2; echo >&2; usage >&2; exit 1 ;;
    esac
done

if [[ -z "$OUTPUT_DIR" ]]; then
    echo "ERROR: -o/--output DIR is required" >&2
    exit 1
fi
if [[ ${#CMD[@]} -eq 0 ]]; then
    echo "ERROR: no command given (pass it after --)" >&2
    exit 1
fi

PINTOOL_SO="${SCRIPT_DIR}/src/pintool/obj-intel64/bullettime.so"
CONSUMER_BIN="${SCRIPT_DIR}/src/consumer/consumer"

[[ -x "${PIN_ROOT}/pin" ]] || {
    echo "ERROR: Pin not found at ${PIN_ROOT}/pin — run setup_pintool.sh" >&2
    exit 1
}
[[ -f "${PINTOOL_SO}" ]] || {
    echo "ERROR: ${PINTOOL_SO} not built — run setup_pintool.sh" >&2
    exit 1
}
[[ -x "${CONSUMER_BIN}" ]] || {
    echo "ERROR: ${CONSUMER_BIN} not built — run setup_pintool.sh" >&2
    exit 1
}

FREE_HP=$(awk '/^HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
if (( FREE_HP < 4 )); then
    echo "WARNING: only ${FREE_HP} hugepages free; BulletTime needs one 2MB hugepage per traced thread." >&2
    echo "         Reserve some with:  echo 1024 | sudo tee /proc/sys/vm/nr_hugepages" >&2
fi

mkdir -p "$OUTPUT_DIR"
if [[ -n "$(ls -A "$OUTPUT_DIR" 2>/dev/null)" ]]; then
    echo "WARNING: ${OUTPUT_DIR} is not empty; trace files will be intermixed." >&2
fi

CONSUMER_FLAGS=(
    --direct-io      1
    --hugepages      1
    --compression    "$USE_COMPRESSION"
    --app-dilation   "$USE_APP_DIL"
    --kernel-dilation "$USE_KERNEL_DIL"
)

echo "==> Launching consumer"
"$CONSUMER_BIN" "$OUTPUT_DIR" "${CONSUMER_FLAGS[@]}" &
CONSUMER_PID=$!
trap 'kill -TERM "$CONSUMER_PID" 2>/dev/null || true' EXIT

PIN_ARGS=(-t "$PINTOOL_SO" -outprefix "$OUTPUT_DIR/trace")
if [[ -n "$FCALLS" ]]; then
    PIN_ARGS+=(-fcalls "$FCALLS")
    (( FCALLS_EXACT == 1 )) && PIN_ARGS+=(-fcalls_exact 1)
fi

echo "==> Launching pin"
"$PIN_ROOT/pin" "${PIN_ARGS[@]}" -- "${CMD[@]}"
PIN_RC=$?
echo "==> Pin exited with code $PIN_RC; waiting for consumer to drain"

wait "$CONSUMER_PID"
CONSUMER_RC=$?
trap - EXIT

if (( CONSUMER_RC != 0 )); then
    echo "ERROR: consumer exited with code ${CONSUMER_RC}" >&2
    exit "$CONSUMER_RC"
fi
echo "==> Done. Outputs in ${OUTPUT_DIR}"
exit "$PIN_RC"
