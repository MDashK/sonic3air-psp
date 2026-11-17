#!/bin/bash
# Builds the desktop (Linux) version, needed as a tool for the PSP port: it compiles the scripts (scripts.bin) and
# exports the emulated audio. Then runs it once for both:
#   ./build_desktop.sh [jobs]  ->  psp/out/scripts.bin, psp/out/audio_export/*.wav + manifest.json
#
#   S3AIR_DESKTOP_WORK   where the mirror and the build live (default: ~/s3air-desktop)
#   ROM                  the Sonic 3 & Knuckles ROM (default: psp/rom/Sonic_Knuckles_wSonic3.bin)
# Needs the packages from Oxygen/sonic3air/build/_cmake/how-to-build.txt. Without libcurl-dev or GL/glu.h (which
# aren't needed here), the build in the mirror goes on without them.
set -e

SRC="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${S3AIR_DESKTOP_WORK:-$HOME/s3air-desktop}"
JOBS="${1:-4}"
ROM="${ROM:-$SRC/psp/rom/Sonic_Knuckles_wSonic3.bin}"
OUT="$SRC/psp/out"

# Mirror (sources, scripts and the data the game loads on startup)
mkdir -p "$WORK"
for dir in framework librmx Oxygen/lemonscript Oxygen/oxygenengine/source Oxygen/oxygenengine/data Oxygen/sonic3air/source Oxygen/sonic3air/scripts Oxygen/sonic3air/build/_cmake; do
	mkdir -p "$WORK/$dir"
	rsync -rlpD --checksum --delete --exclude '/build/' "$SRC/$dir/" "$WORK/$dir/"
done
mkdir -p "$WORK/Oxygen/sonic3air/data"
rsync -rlpD --checksum --exclude 'audio/remastered/' "$SRC/Oxygen/sonic3air/data/" "$WORK/Oxygen/sonic3air/data/"
cp "$SRC/Oxygen/sonic3air/oxygenproject.json" "$WORK/Oxygen/sonic3air/"
# (runs without a window: software renderer)
cat > "$WORK/Oxygen/sonic3air/config.json" <<'CONFIG'
{
	"ScriptsDir": "scripts",
	"MainScriptName": "main.lemon",
	"SaveStatesDir": "saves/states",
	"StartPhase": "2",
	"RenderMethod": "software",
	"CompileScripts": "0"
}
CONFIG

# Optional dependencies that aren't there
CMAKE_EXTRA=""
if ! pkg-config --exists libcurl 2>/dev/null; then
	sed -i 's/^if (UNIX)\r\?$/if (FALSE)	# (no libcurl-dev here)/' "$WORK/Oxygen/sonic3air/build/_cmake/CMakeLists.txt"
	sed -i 's/#elif defined(PLATFORM_LINUX) || defined(PLATFORM_MAC)/#elif defined(PLATFORM_MAC)/' "$WORK/Oxygen/oxygenengine/source/oxygen/download/Downloader.cpp"
fi
if [ ! -f /usr/include/GL/glu.h ]; then
	mkdir -p "$WORK/stubinclude/GL"
	echo "#pragma once" > "$WORK/stubinclude/GL/glu.h"
	CMAKE_EXTRA="-DCMAKE_C_FLAGS=-I$WORK/stubinclude -DCMAKE_CXX_FLAGS=-I$WORK/stubinclude"
fi

BUILD="$WORK/Oxygen/sonic3air/build/_cmake/build"
mkdir -p "$BUILD"
cd "$BUILD"
cmake -DCMAKE_BUILD_TYPE=Release -DUSE_DISCORD=OFF -DUSE_IMGUI=OFF $CMAKE_EXTRA .. > cmake.log 2>&1 || { tail -20 cmake.log; exit 1; }
if ! make -j"$JOBS" > build.log 2>&1; then
	grep -E "error|undefined reference" build.log | head -30
	echo "Build failed, see $BUILD/build.log"
	exit 1
fi

# Run once without a window: compiles the scripts (saved as saves/scripts.bin) and exports the audio
GAME="$WORK/Oxygen/sonic3air"
if [ ! -f "$ROM" ]; then
	echo "Built, but no ROM at $ROM: put it there (or set ROM=...) to get scripts.bin and the audio"
	exit 0
fi
cp "$ROM" "$GAME/Sonic_Knuckles_wSonic3.bin"
rm -f "$GAME/saves/scripts.bin"
# SCRIPTS_ONLY=1: only the scripts, no audio export (that takes a while, and ~1.4 GB)
EXPORT_DIR="$OUT/audio_export"
EXPORT_FILTER="${EXPORT_ONLY:-}"		# EXPORT_ONLY=BC,BD,...: only these audio keys (the converter keeps the others as converted before)
if [ "$SCRIPTS_ONLY" = "1" ]; then
	EXPORT_DIR="$WORK/no_audio_export"
	EXPORT_FILTER="none"
fi
mkdir -p "$EXPORT_DIR"
cd "$GAME"
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy S3AIR_EXPORT_AUDIO="$EXPORT_DIR" S3AIR_EXPORT_AUDIO_ONLY="$EXPORT_FILTER" ./sonic3air_linux > "$BUILD/export.log" 2>&1 || true
if [ -f "$GAME/saves/scripts.bin" ]; then
	cp "$GAME/saves/scripts.bin" "$OUT/scripts.bin"
	echo "Scripts: $OUT/scripts.bin"
else
	echo "No scripts.bin, see $BUILD/export.log"
fi
[ "$SCRIPTS_ONLY" = "1" ] && rm -rf "$EXPORT_DIR" || echo "Audio: $(ls "$OUT/audio_export" | wc -l) files in $OUT/audio_export"
