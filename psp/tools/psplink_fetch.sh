#!/bin/bash
# Copies files from the game folder on the memory stick to the host folder (PSPLink running, no other pspsh attached).
#   ./psplink_fetch.sh <file>...      e.g. ./psplink_fetch.sh allocs.txt bench.log render.log savedata/logfile.txt
HOST_NAME="${HOST_NAME:-Sonic3AIR}"
MS_DIR="${MS_DIR:-ms0:/PSP/GAME/Sonic3AIR}"
PSPSH="${PSPSH:-$HOME/pspdev/bin/pspsh}"
( sleep 1; for f in "$@"; do echo "cp $MS_DIR/$f host0:/$HOST_NAME/$(basename "$f")"; sleep 3; done ) | timeout $((10 + 3 * $#)) "$PSPSH" 2>&1 | grep -E "^cp|rror|fail"
