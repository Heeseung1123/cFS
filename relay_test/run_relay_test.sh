#!/usr/bin/env bash
# Puts a test file in /cf/inbox (standing in for an uplinked file), starts native core-cpu1
# and runs relay_test.py as the second ground station.
# Usage: ./run_relay_test.sh [-b build_dir]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="build-native"
CI_PORT=1234
PROXY_PORT=1236 # CI_LAB accepts only TC frames, so commands go through tc_frame_proxy.py
STATION_PORT=1240
TEST_FILE="/cf/inbox/relay_test.bin"
LOG_FILE="$SCRIPT_DIR/core-cpu1.log"

while getopts "b:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        *) echo "Usage: $0 [-b build_dir]"; exit 2 ;;
    esac
done

EXE_DIR="$CFS_ROOT/$BUILD_DIR/exe/cpu1"
LOCAL_FILE="$EXE_DIR${TEST_FILE}"
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

# 1. Check the build, refuse to disturb a running instance and create the "uplinked" file.
[ -x "$EXE_DIR/core-cpu1" ] || fail "$EXE_DIR/core-cpu1 not found. Build the native target first."
[ -f "$EXE_DIR/cf/relay_app.so" ] || fail "relay_app.so not found in $EXE_DIR/cf"
pgrep -x core-cpu1 >/dev/null && fail "core-cpu1 is already running. Stop it first."
mkdir -p "$(dirname "$LOCAL_FILE")"
head -c 20000 /dev/urandom > "$LOCAL_FILE"

# 2. Start cFS from its own directory because it loads apps from ./cf.
echo "=== Starting core-cpu1 (log: $LOG_FILE) ==="
: > "$LOG_FILE"
(cd "$EXE_DIR" && exec ./core-cpu1 -R PO) > "$LOG_FILE" 2>&1 &
CFE_PID=$!
for _ in $(seq 1 30); do
    grep -q "RELAY App Initialized" "$LOG_FILE" && grep -q "FILE_DL Initialized" "$LOG_FILE" &&
        grep -q "CI Lab Initialized" "$LOG_FILE" && break
    kill -0 "$CFE_PID" 2>/dev/null || fail "core-cpu1 exited during startup"
    sleep 1
done
grep -q "RELAY App Initialized" "$LOG_FILE" || fail "RELAY_APP did not initialize within 30 seconds"

# 3. One proxy for all commands; relay_test.py decides the result.
python3 "$CFS_ROOT/tc_test/tc_frame_proxy.py" --listen-port "$PROXY_PORT" --dest-port "$CI_PORT" --quiet &
PROXY_PID=$!
sleep 0.5
python3 -u "$SCRIPT_DIR/relay_test.py" --cmd-port "$PROXY_PORT" --station-port "$STATION_PORT" \
    --file "$TEST_FILE" --local-file "$LOCAL_FILE" --log "$LOG_FILE"
RESULT=$?

echo
echo "=== RELAY and FILE_DL events in the log ==="
grep -E "RELAY|FILE_DL" "$LOG_FILE"
exit "$RESULT"
