#!/usr/bin/env bash
# Starts native core-cpu1 and packet_view.py, enables TM, then sends one SetRate for the Mission App HK.
# Usage: ./run_packet_view.sh [-b build_dir] [-r hk_seconds] [-- packet_view.py options]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="build-native"
HK_RATE=2
LOG_FILE="$SCRIPT_DIR/core-cpu1.log"

while getopts "b:r:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        r) HK_RATE="$OPTARG" ;;
        *) echo "Usage: $0 [-b build_dir] [-r hk_seconds] [-- packet_view.py options]"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

EXE_DIR="$CFS_ROOT/$BUILD_DIR/exe/cpu1"
CMD_UTIL="$CFS_ROOT/$BUILD_DIR/exe/host/cmdUtil"
CFE_PID=""
CMD_PID=""
RUN_DIR=""

cleanup() {
    [ -n "$CMD_PID" ] && kill "$CMD_PID" 2>/dev/null
    if [ -n "$CFE_PID" ] && kill -0 "$CFE_PID" 2>/dev/null; then
        kill -TERM "$CFE_PID" 2>/dev/null
        for _ in 1 2 3 4 5; do
            kill -0 "$CFE_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$CFE_PID" 2>/dev/null
    fi
    [ -n "$RUN_DIR" ] && rm -rf "$RUN_DIR"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# 1. Check the build and refuse to disturb a running instance.
[ -x "$EXE_DIR/core-cpu1" ] || { echo "FAIL: $EXE_DIR/core-cpu1 not found. Build the native target first."; exit 1; }
[ -x "$CMD_UTIL" ] || { echo "FAIL: $CMD_UTIL not found. Build the native target first."; exit 1; }
if pgrep -x core-cpu1 >/dev/null; then
    echo "FAIL: core-cpu1 is already running. Stop it first."
    exit 1
fi
if ss -Hlun 'sport = :1235 or sport = :1236' | grep -q .; then
    echo "FAIL: UDP 1235 or 1236 is in use. Stop tc_frame_proxy.py or the ground program first."
    exit 1
fi

# 2. Start cFS from a copy, so the files in the build directory do not change.
echo "=== Starting core-cpu1 (log: $LOG_FILE) ==="
RUN_DIR="$(mktemp -d)" || exit 1
cp -r "$EXE_DIR/." "$RUN_DIR" || exit 1
: > "$LOG_FILE"
(cd "$RUN_DIR" && exec ./core-cpu1 -R PO) > "$LOG_FILE" 2>&1 &
CFE_PID=$!
for _ in $(seq 1 30); do
    grep -q "OPERATIONAL state" "$LOG_FILE" && break
    kill -0 "$CFE_PID" 2>/dev/null || { echo "FAIL: core-cpu1 exited during startup"; exit 1; }
    sleep 1
done
grep -q "OPERATIONAL state" "$LOG_FILE" || { echo "FAIL: cFS did not start within 30 seconds"; exit 1; }

# 3. SetRate for the Mission App HK, sent through packet_view.py so that it is dumped too.
(
    sleep 5
    exec "$CMD_UTIL" --host=127.0.0.1 --port=1236 --endian=LE --pktid=0x1886 --cmdcode=0 \
        --uint32=0x18D1 --uint32=0x08D0 --uint32="$HK_RATE" >/dev/null
) &
CMD_PID=$!

# 4. Dump the packets until Ctrl+C (or --count).
python3 -u "$SCRIPT_DIR/packet_view.py" --enable 127.0.0.1 "$@"
