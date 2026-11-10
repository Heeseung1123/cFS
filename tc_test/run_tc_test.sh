#!/usr/bin/env bash
# Starts native core-cpu1, sends good and broken TC frames to CI_LAB and checks the results.
# Usage: ./run_tc_test.sh [-b build_dir]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="build-native"
PROXY_PORT=1236
CI_LAB_CMD_MID="0x1884"
LOG_FILE="$SCRIPT_DIR/core-cpu1.log"

while getopts "b:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        *) echo "Usage: $0 [-b build_dir]"; exit 2 ;;
    esac
done

EXE_DIR="$CFS_ROOT/$BUILD_DIR/exe/cpu1"
CMD_UTIL="$CFS_ROOT/$BUILD_DIR/exe/host/cmdUtil"
PROXY="$SCRIPT_DIR/tc_frame_proxy.py"
CFE_PID=""
PASSED=0
FAILED=0

cleanup() {
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

count() { grep -c -- "$1" "$LOG_FILE"; }

# Sends one command through the proxy in the given mode.
send_cmd() {
    local mode="$1" cc="$2"
    python3 -u "$PROXY" --listen-port "$PROXY_PORT" --mode "$mode" --count 1 --timeout 5 &
    local proxy_pid=$!
    sleep 0.5
    "$CMD_UTIL" --host=127.0.0.1 --port="$PROXY_PORT" --pktid="$CI_LAB_CMD_MID" --cmdcode="$cc" >/dev/null
    wait "$proxy_pid"
}

# Waits until the log has more lines matching the pattern than before.
wait_for() {
    local pattern="$1" before="$2"
    for _ in $(seq 1 10); do
        [ "$(count "$pattern")" -gt "$before" ] && return 0
        sleep 0.5
    done
    return 1
}

report() {
    if [ "$1" -eq 0 ]; then
        echo "  PASS: $2"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $2"
        FAILED=$((FAILED + 1))
    fi
}

# A good frame must run the command.
test_good() {
    local name="$1" cc="$2" pattern="$3"
    echo "=== $name ==="
    local before
    before=$(count "$pattern")
    send_cmd good "$cc"
    wait_for "$pattern" "$before"
    report $? "'$pattern' logged"
}

# A broken frame must be dropped with an event, and the command must not run.
test_bad() {
    local mode="$1" pattern="$2"
    echo "=== broken frame: $mode ==="
    local before_err before_noop
    before_err=$(count "$pattern")
    before_noop=$(count "CI: NOOP command")
    send_cmd "$mode" 0
    wait_for "$pattern" "$before_err"
    report $? "'$pattern' logged"
    sleep 1
    [ "$(count "CI: NOOP command")" -eq "$before_noop" ]
    report $? "NOOP was not executed"
}

# 1. Check the build and refuse to disturb a running instance.
[ -x "$EXE_DIR/core-cpu1" ] || { echo "FAIL: $EXE_DIR/core-cpu1 not found. Build the native target first."; exit 1; }
[ -x "$CMD_UTIL" ] || { echo "FAIL: $CMD_UTIL not found. Build the native target first."; exit 1; }
if pgrep -x core-cpu1 >/dev/null; then
    echo "FAIL: core-cpu1 is already running. Stop it first."
    exit 1
fi

# 2. Start cFS from its own directory because it loads apps from ./cf.
echo "=== Starting core-cpu1 (log: $LOG_FILE) ==="
: > "$LOG_FILE"
(cd "$EXE_DIR" && exec ./core-cpu1 -R PO) > "$LOG_FILE" 2>&1 &
CFE_PID=$!
for _ in $(seq 1 30); do
    grep -q "CI Lab Initialized" "$LOG_FILE" && break
    kill -0 "$CFE_PID" 2>/dev/null || { echo "FAIL: core-cpu1 exited during startup"; exit 1; }
    sleep 1
done
grep -q "CI Lab Initialized" "$LOG_FILE" || { echo "FAIL: CI_LAB did not initialize within 30 seconds"; exit 1; }
sleep 2

# 3. Test cases.
test_good "good frame: NOOP" 0 "CI: NOOP command"
test_good "good frame: RESET_COUNTERS" 1 "CI: RESET command"
test_bad length "TC frame dropped, length field"
test_bad packet "TC frame dropped, packet size"
test_bad short "TC frame dropped, too short"
test_bad raw "TC frame dropped"

echo
echo "=== CI_LAB events in the log ==="
grep -E "CI: " "$LOG_FILE"
echo
echo "passed: $PASSED, failed: $FAILED"
if [ "$FAILED" -eq 0 ]; then
    echo "RESULT: PASS"
    exit 0
fi
echo "RESULT: FAIL"
exit 1
