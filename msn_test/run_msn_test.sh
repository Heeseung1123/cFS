#!/usr/bin/env bash
# Starts native core-cpu1 with a clean MSN state, enables TO_LAB output and runs msn_test.py.
# Usage: ./run_msn_test.sh [-b build_dir]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="build-native"
CI_PORT=1234
TLM_PORT=1235
PROXY_PORT=1236 # CI_LAB accepts only TC frames, so commands go through tc_frame_proxy.py
TO_LAB_CMD_MID="0x1880"
TO_OUTPUT_ENABLE_CC=6
LOG_FILE="$SCRIPT_DIR/core-cpu1.log"

while getopts "b:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        *) echo "Usage: $0 [-b build_dir]"; exit 2 ;;
    esac
done

EXE_DIR="$CFS_ROOT/$BUILD_DIR/exe/cpu1"
CMD_UTIL="$CFS_ROOT/$BUILD_DIR/exe/host/cmdUtil"
CFE_PID=""
PROXY_PID=""

cleanup() {
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null
    if [ -n "$CFE_PID" ] && kill -0 "$CFE_PID" 2>/dev/null; then
        kill -TERM "$CFE_PID" 2>/dev/null
        for _ in 1 2 3 4 5; do
            kill -0 "$CFE_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$CFE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

fail() {
    echo "FAIL: $1"
    [ -f "$LOG_FILE" ] && tail -n 20 "$LOG_FILE"
    exit 1
}

# 1. Check the build, refuse to disturb a running instance and start from the default state.
[ -x "$EXE_DIR/core-cpu1" ] || fail "$EXE_DIR/core-cpu1 not found. Build the native target first."
[ -f "$EXE_DIR/cf/msn_app.so" ] || fail "msn_app.so not found in $EXE_DIR/cf"
pgrep -x core-cpu1 >/dev/null && fail "core-cpu1 is already running. Stop it first."
rm -f "$EXE_DIR/cf/msn_state.txt" "$EXE_DIR/cf/msn_log.txt"

# 2. Start cFS from its own directory because it loads apps from ./cf.
echo "=== Starting core-cpu1 (log: $LOG_FILE) ==="
: > "$LOG_FILE"
(cd "$EXE_DIR" && exec ./core-cpu1 -R PO) > "$LOG_FILE" 2>&1 &
CFE_PID=$!
for _ in $(seq 1 30); do
    grep -q "MSN App Initialized" "$LOG_FILE" && grep -q "TO Lab Initialized" "$LOG_FILE" && break
    kill -0 "$CFE_PID" 2>/dev/null || fail "core-cpu1 exited during startup"
    sleep 1
done
grep -q "MSN App Initialized" "$LOG_FILE" || fail "MSN_APP did not initialize within 30 seconds"

# 3. Keep one proxy running for all commands, then enable telemetry output.
python3 "$CFS_ROOT/tc_test/tc_frame_proxy.py" --listen-port "$PROXY_PORT" --dest-port "$CI_PORT" --quiet &
PROXY_PID=$!
sleep 0.5
for _ in $(seq 1 10); do
    "$CMD_UTIL" --host=127.0.0.1 --port="$PROXY_PORT" --pktid="$TO_LAB_CMD_MID" \
        --cmdcode="$TO_OUTPUT_ENABLE_CC" --string="16:127.0.0.1" >/dev/null
    sleep 1
    grep -q "telemetry output enabled" "$LOG_FILE" && break
done
grep -q "telemetry output enabled" "$LOG_FILE" || fail "TO_LAB did not accept the enable command"

# 4. msn_test.py decides the result.
python3 -u "$SCRIPT_DIR/msn_test.py" --cmd-port "$PROXY_PORT" --tlm-port "$TLM_PORT" \
    --sensor-file "$EXE_DIR/cf/msn_sensor.csv" --state-file "$EXE_DIR/cf/msn_state.txt"
RESULT=$?

echo
echo "=== MSN events in the log ==="
grep "MSN" "$LOG_FILE"
exit "$RESULT"
