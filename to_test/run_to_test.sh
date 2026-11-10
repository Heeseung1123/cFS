#!/usr/bin/env bash
# Starts native core-cpu1, enables TO_LAB output with cmdUtil and checks the TM frames.
# Usage: ./run_to_test.sh [-b build_dir] [-d seconds] [-q]

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="build-native"
DURATION=10
QUIET=""
CI_HOST="127.0.0.1"
CI_PORT=1234
PROXY_PORT=1236 # CI_LAB accepts only TC frames, so commands go through tc_frame_proxy.py
DEST_IP="127.0.0.1"
TLM_PORT=1235
TO_LAB_CMD_MID="0x1880"
TO_OUTPUT_ENABLE_CC=6
LOG_FILE="$SCRIPT_DIR/core-cpu1.log"

while getopts "b:d:qh" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        d) DURATION="$OPTARG" ;;
        q) QUIET="--quiet" ;;
        *) echo "Usage: $0 [-b build_dir] [-d seconds] [-q]"; exit 2 ;;
    esac
done

EXE_DIR="$CFS_ROOT/$BUILD_DIR/exe/cpu1"
CMD_UTIL="$CFS_ROOT/$BUILD_DIR/exe/host/cmdUtil"
CFE_PID=""
PARSER_PID=""

cleanup() {
    if [ -n "$CFE_PID" ] && kill -0 "$CFE_PID" 2>/dev/null; then
        kill -TERM "$CFE_PID" 2>/dev/null
        for _ in 1 2 3 4 5; do
            kill -0 "$CFE_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$CFE_PID" 2>/dev/null
    fi
    if [ -n "$PARSER_PID" ]; then
        kill "$PARSER_PID" 2>/dev/null
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

fail() {
    echo "FAIL: $1"
    if [ -f "$LOG_FILE" ]; then
        echo "--- last lines of $LOG_FILE ---"
        tail -n 20 "$LOG_FILE"
    fi
    exit 1
}

# 1. Check the build and refuse to disturb a running instance.
[ -x "$EXE_DIR/core-cpu1" ] || { echo "FAIL: $EXE_DIR/core-cpu1 not found. Build the native target first."; exit 1; }
[ -x "$CMD_UTIL" ] || { echo "FAIL: $CMD_UTIL not found. Build the native target first."; exit 1; }
if pgrep -x core-cpu1 >/dev/null; then
    echo "FAIL: core-cpu1 is already running. Stop it first."
    exit 1
fi

# 2. Start the parser first so it does not miss the first frame.
echo "=== Starting frame parser on UDP $TLM_PORT ==="
python3 -u "$SCRIPT_DIR/parse_tm_frames.py" --port "$TLM_PORT" --duration "$DURATION" $QUIET &
PARSER_PID=$!
sleep 1
kill -0 "$PARSER_PID" 2>/dev/null || { PARSER_PID=""; echo "FAIL: parser did not start. Is UDP $TLM_PORT in use?"; exit 1; }

# 3. Start cFS from its own directory because it loads apps from ./cf.
echo "=== Starting core-cpu1 (log: $LOG_FILE) ==="
: > "$LOG_FILE"
(cd "$EXE_DIR" && exec ./core-cpu1 -R PO) > "$LOG_FILE" 2>&1 &
CFE_PID=$!

for _ in $(seq 1 30); do
    grep -q "TO Lab Initialized" "$LOG_FILE" && break
    kill -0 "$CFE_PID" 2>/dev/null || fail "core-cpu1 exited during startup"
    sleep 1
done
grep -q "TO Lab Initialized" "$LOG_FILE" || fail "TO_LAB did not initialize within 30 seconds"
echo "TO_LAB initialized"

# 4. Resend the enable command until TO_LAB reports it, as CI_LAB may still be starting.
echo "=== Enabling telemetry output to $DEST_IP ==="
for _ in $(seq 1 10); do
    python3 "$CFS_ROOT/tc_test/tc_frame_proxy.py" --listen-port "$PROXY_PORT" --dest-host "$CI_HOST" \
        --dest-port "$CI_PORT" --count 1 --timeout 5 --quiet >/dev/null &
    PROXY_PID=$!
    sleep 0.5
    "$CMD_UTIL" --host=127.0.0.1 --port="$PROXY_PORT" --pktid="$TO_LAB_CMD_MID" \
        --cmdcode="$TO_OUTPUT_ENABLE_CC" --string="16:$DEST_IP" >/dev/null
    wait "$PROXY_PID"
    sleep 1
    grep -q "telemetry output enabled" "$LOG_FILE" && break
done
grep -q "telemetry output enabled" "$LOG_FILE" || fail "TO_LAB did not accept the enable command"
grep -m1 "telemetry output enabled" "$LOG_FILE"

# 5. The parser decides the result.
wait "$PARSER_PID"
RESULT=$?
PARSER_PID=""

echo
echo "=== TO_LAB error events in the log ==="
grep -E "TO (dropped|sendto|invalid|Can't)" "$LOG_FILE" || echo "none"

exit "$RESULT"
