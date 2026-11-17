#!/bin/bash
# Copies files from the host folder to the memory stick through PSPLink (usbhostfs_pc running, PSP connected).
#   ./psplink_sync.sh <path inside the game folder>...     e.g. ./psplink_sync.sh data/audio data/scripts.bin EBOOT.PBP
#   HOST_DIR, HOST_NAME, MS_DIR: as in psplink_run.sh
HOST_DIR="${HOST_DIR:-/mnt/e/_PSP/ppsspp_win/memstick/PSP/GAME/Sonic3AIR}"
HOST_NAME="${HOST_NAME:-Sonic3AIR}"
MS_DIR="${MS_DIR:-ms0:/PSP/GAME/Sonic3AIR}"
PSPSH="${PSPSH:-$HOME/pspdev/bin/pspsh}"

SCRIPT=$(mktemp)
echo "mkdir $MS_DIR" >> "$SCRIPT"
cd "$HOST_DIR" || exit 1
for path in "$@"; do
	if [ -d "$path" ]; then
		find "$path" -type d | sort | while read -r d; do echo "mkdir $MS_DIR/$d" >> "$SCRIPT"; done
		find "$path" -type f | sort | while read -r f; do echo "cp host0:/$HOST_NAME/$f $MS_DIR/$f" >> "$SCRIPT"; done
	elif [ -f "$path" ]; then
		dir=$(dirname "$path")
		[ "$dir" != "." ] && echo "mkdir $MS_DIR/$dir" >> "$SCRIPT"
		echo "cp host0:/$HOST_NAME/$path $MS_DIR/$path" >> "$SCRIPT"
	fi
done
# DELETE=<text file>: also removes these files (paths inside the game folder, one per line) from the memory stick
if [ -n "$DELETE" ]; then
	while read -r f; do [ -n "$f" ] && echo "rm $MS_DIR/$f" >> "$SCRIPT"; done < "$DELETE"
	echo "$(grep -c '^rm ' "$SCRIPT") files to remove"
fi
echo "$(grep -c '^cp ' "$SCRIPT") files to copy"
# Resume a stopped sync: FROM=<path> skips the copies before that file
if [ -n "$FROM" ]; then
	awk -v from="$FROM" '!/^cp / || found || index($0, "/" from " ") { if (/^cp /) found = 1; print }' "$SCRIPT" > "$SCRIPT.2" && mv "$SCRIPT.2" "$SCRIPT"
	echo "resuming at $FROM: $(grep -c '^cp ' "$SCRIPT") files"
fi

# One command at a time: sending them all at once makes the PSP's shell stall after ~100 copies. Each finished
# command ends with a new prompt, so wait for the prompt count to go up before sending the next one.
LOG=/tmp/psplink_sync.log
rm -f "$LOG"
prompts() { grep -o 'host0:/>' "$LOG" 2>/dev/null | wc -l; }
(
	for i in $(seq 1 30); do [ "$(prompts)" -ge 1 ] && break; sleep 1; done
	n=0
	while read -r line; do
		n=$((n + 1))
		echo "$line"
		for i in $(seq 1 600); do [ "$(prompts)" -gt "$n" ] && break; sleep 0.2; done
		[ $((n % 25)) -eq 0 ] && echo "  $n commands done" >&2
	done < "$SCRIPT"
	sleep 1
) | timeout 7200 "$PSPSH" > "$LOG" 2>&1
echo "done (exit $?), $(grep -ciE 'fail|error' "$LOG") errors"
rm -f "$SCRIPT"
