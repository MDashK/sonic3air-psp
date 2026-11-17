#!/bin/bash
# Checks that C++ implementations of script code give the same result as the original scripts: runs the desktop build's
# benchmark with this source's scripts, then with one script file from the original source, and compares the emulated
# RAM and VRAM at the end.
#   ./compare_scripts.sh <script file, relative to Oxygen/sonic3air/scripts> [frames] [level]
#   ORIGINAL: the original source's Oxygen/sonic3air/scripts (default: /mnt/e/_PSP/sonic3air-main/Oxygen/sonic3air/scripts)
FILE="$1"
FRAMES="${2:-1500}"
LEVEL="${3:-0000}"
ORIGINAL="${ORIGINAL:-/mnt/e/_PSP/sonic3air-main/Oxygen/sonic3air/scripts}"
GAME="$HOME/s3air-desktop/Oxygen/sonic3air"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"

run() {
	rm -f /tmp/compare_bench.log "$GAME/saves/scripts.bin"	# (or it uses the scripts compiled before)
	( cd "$GAME" && SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy S3AIR_BENCH=1 S3AIR_BENCH_FRAMES=$FRAMES S3AIR_BENCH_INPUT=1 S3AIR_BENCH_LEVEL=$LEVEL S3AIR_BENCH_LOG=/tmp/compare_bench.log timeout 300 ./sonic3air_linux > /dev/null 2>&1 )
	grep -E "STATE" /tmp/compare_bench.log
}

cp "$SRC/Oxygen/sonic3air/scripts/$FILE" "$GAME/scripts/$FILE"
echo "with this source: $(run)"
cp "$ORIGINAL/$FILE" "$GAME/scripts/$FILE"
echo "original script:  $(run)"
cp "$SRC/Oxygen/sonic3air/scripts/$FILE" "$GAME/scripts/$FILE"
