#!/bin/bash
# Test run on a real PSP through PSPLink (usbhostfs_pc must be running and the PSP connected).
#   ./psplink_run.sh <seconds>
#
# Copies sonic3air.prx and bench.txt from the host folder to the memory stick, runs the PRX from there (reading
# directories through host0: is too slow), keeps pspsh attached to catch an exception dump, then copies the logs,
# the statistics and the screenshots back to the host folder.
#   HOST_DIR   host folder of the game, seen by the PSP as host0:/$HOST_NAME (usbhostfs_pc serves its parent)
#   MS_DIR     the game folder on the memory stick
HOST_DIR="${HOST_DIR:-/mnt/e/_PSP/ppsspp_win/memstick/PSP/GAME/Sonic3AIR}"
HOST_NAME="${HOST_NAME:-Sonic3AIR}"
MS_DIR="${MS_DIR:-ms0:/PSP/GAME/Sonic3AIR}"
PSPSH="${PSPSH:-$HOME/pspdev/bin/pspsh}"
PRX="${PRX:-$(cd "$(dirname "$0")/.." && pwd)/out/sonic3air.prx}"
SECONDS_TO_RUN="${1:-90}"

cp "$PRX" "$HOST_DIR/sonic3air.prx"
rm -f "$HOST_DIR"/bench.log "$HOST_DIR"/render.log "$HOST_DIR"/allocs.txt "$HOST_DIR"/meminfo.txt "$HOST_DIR"/shot_*.bmp "$HOST_DIR"/logfile_ms0.txt
SHOTS=$(grep S3AIR_BENCH_SHOTS "$HOST_DIR/bench.txt" 2>/dev/null | cut -d= -f2 | tr -d '\r' | tr ',' ' ')

# Prepare the memory stick folder
( echo "cp host0:/$HOST_NAME/sonic3air.prx $MS_DIR/sonic3air.prx"
  echo "cp host0:/$HOST_NAME/bench.txt $MS_DIR/bench.txt"
  for f in bench.log render.log allocs.txt meminfo.txt savedata/logfile.txt; do echo "rm $MS_DIR/$f"; done
  for s in $SHOTS; do printf 'rm %s/shot_%04d.bmp\n' "$MS_DIR" "$s"; done
  sleep 1 ) | timeout 120 "$PSPSH" > /tmp/psplink_prepare.log 2>&1

# Run, with the shell attached
# Ends early when the game ends (PSPLink then resets); at the time limit the game may still run
rm -f /tmp/psplink_run.log
( echo "$MS_DIR/sonic3air.prx"
  for i in $(seq "$SECONDS_TO_RUN"); do grep -qE "Resetting psplink|System shutdown|Exception - |EPC|TERMINATE" /tmp/psplink_run.log 2>/dev/null && break; sleep 1; done
  echo "exprint"; sleep 2 ) | timeout $((SECONDS_TO_RUN + 20)) "$PSPSH" > /tmp/psplink_run.log 2>&1

# Still running: don't copy anything now (that goes through the memory stick and USB while the game runs, it slows the
# game down a lot). The files stay on the memory stick for psplink_fetch.sh, or the next run.
if ! grep -qE "Resetting psplink|System shutdown|Exception - |EPC|TERMINATE" /tmp/psplink_run.log; then
	echo "The game is still running: logs not copied (run psplink_fetch.sh once it ended)"
	exit 0
fi
grep -E "Exception|EPC|BadVAddr|Address |Load/Start|refused" /tmp/psplink_run.log | head -12

# Copy the results back (PSPLink restarts when the program ends: wait for it)
for i in 1 2 3 4 5 6; do "$PSPSH" -e 'ls ms0:/' > /dev/null 2>&1 && break; sleep 3; done
( for f in bench.log render.log allocs.txt meminfo.txt; do echo "cp $MS_DIR/$f host0:/$HOST_NAME/$f"; done
  echo "cp $MS_DIR/savedata/logfile.txt host0:/$HOST_NAME/logfile_ms0.txt"
  for s in $SHOTS; do printf 'cp %s/shot_%04d.bmp host0:/%s/shot_%04d.bmp\n' "$MS_DIR" "$s" "$HOST_NAME" "$s"; done
  sleep 1 ) | timeout 120 "$PSPSH" > /tmp/psplink_back.log 2>&1
ls "$HOST_DIR" | grep -E "\.log|shot_|\.txt"
