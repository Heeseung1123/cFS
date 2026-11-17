#!/usr/bin/env bash
# Builds the host harness from the real tm_sdlp_my.c and tm_sdlp_of.c and runs the segmentation checks.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFS_ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${1:-build-native}"
FLAGS="$(find "$CFS_ROOT/$BUILD" -path '*to_lab.dir*' -name flags.make | head -1)"
[ -n "$FLAGS" ] || { echo "FAIL: build $BUILD first"; exit 1; }
INC="$(sed -n 's/^C_INCLUDES = //p' "$FLAGS")"
DEF="$(sed -n 's/^C_DEFINES = //p' "$FLAGS")"
SRC="$CFS_ROOT/apps/to_lab/fsw/src"
cc -g -Wall -Werror $DEF $INC -o "$HERE/seg_harness" "$HERE/seg_harness.c" "$SRC/tm_sdlp_my.c" "$SRC/tm_sdlp_of.c"
python3 "$HERE/seg_unit_test.py" "$HERE/seg_harness"
