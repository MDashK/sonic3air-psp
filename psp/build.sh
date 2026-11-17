#!/bin/bash
# Builds the PSP version (EBOOT.PBP and sonic3air.prx) with the pspdev toolchain, e.g. in WSL.
#
# The sources are mirrored into a Linux folder first: compiling straight from a Windows drive (/mnt/...) is very slow.
#   S3AIR_PSP_WORK   where the mirror and the build live (default: ~/s3air-psp)
#   PSPDEV           the pspdev toolchain (default: ~/pspdev)
#   ./build.sh [jobs]
set -e

SRC="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${S3AIR_PSP_WORK:-$HOME/s3air-psp}"
JOBS="${1:-4}"
export PSPDEV="${PSPDEV:-$HOME/pspdev}"
export PATH="$PSPDEV/bin:$PATH"

# Mirror only what the build needs (no game data, no other platforms' build folders)
#  -> Compared by content, and copied files get the current time: make then rebuilds exactly what changed
#     (keeping the source's times could make an edited file look older than an object built in the meantime)
mkdir -p "$WORK"
for dir in framework librmx Oxygen/lemonscript Oxygen/oxygenengine/source Oxygen/sonic3air/source Oxygen/sonic3air/build/_psp; do
	mkdir -p "$WORK/$dir"
	rsync -rlpD --checksum --delete --exclude '/build/' --exclude '/out/' "$SRC/$dir/" "$WORK/$dir/"
done

# Configure (again each time: the source lists are collected by CMake, new files have to show up) and build
BUILD="$WORK/Oxygen/sonic3air/build/_psp/build"
mkdir -p "$BUILD"
cd "$BUILD"
cmake -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" -DCMAKE_BUILD_TYPE=Release .. > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
if ! make -j"$JOBS" > build.log 2>&1; then
	grep -E "error|undefined reference" build.log | head -30
	echo "Build failed, see $BUILD/build.log"
	exit 1
fi

# Results
OUT="$SRC/psp/out"
mkdir -p "$OUT"
cp EBOOT.PBP sonic3air.prx "$OUT/"
echo "Built: $OUT/EBOOT.PBP, $OUT/sonic3air.prx (ELF with symbols for psp-addr2line: $BUILD/sonic3air)"
