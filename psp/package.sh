#!/bin/bash
# Puts together the game folder for the memory stick: psp/out/PSP/GAME/Sonic3AIR
#   ./package.sh <scripts.bin> [rom]
#
# - scripts.bin: the compiled scripts of this exact version (the PSP can't compile them itself). A desktop build of this
#   source writes it to "saves/scripts.bin" when it compiles the scripts on startup.
# - rom: Sonic_Knuckles_wSonic3.bin (optional here, it can also be copied to the folder by hand)
set -e

SRC="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPTS_BIN="$1"
ROM="$2"
OUT="$SRC/psp/out/PSP/GAME/Sonic3AIR"

if [ ! -f "$SRC/psp/out/EBOOT.PBP" ]; then
	echo "No EBOOT.PBP yet, run build.sh first"
	exit 1
fi
if [ -z "$SCRIPTS_BIN" ] || [ ! -f "$SCRIPTS_BIN" ]; then
	echo "Usage: package.sh <scripts.bin> [rom]"
	exit 1
fi

mkdir -p "$OUT/data"
cp "$SRC/psp/out/EBOOT.PBP" "$OUT/"
cp "$SRC/Oxygen/sonic3air/oxygenproject.json" "$OUT/"

# Engine data, then the game's data on top (the remastered music is converted separately)
#  -> Not the touch screen overlay: its sprites take ~2.5 MB of memory (two 770x540 sheets at load time), for nothing on a PSP
rsync -a --exclude 'sprites/input/touch_overlay*' "$SRC/Oxygen/oxygenengine/data/" "$OUT/data/"
rm -f "$OUT/data/sprites/input/touch_overlay"*	# (left from older packages)
#  -> Not the original package's .ogg files either: the converted audio replaces them
rsync -a --exclude 'audio/remastered/' --exclude 'audio/original/*.ogg' "$SRC/Oxygen/sonic3air/data/" "$OUT/data/"
cp "$SCRIPTS_BIN" "$OUT/data/scripts.bin"

# The game's own sprites packed for loading when used (see SpriteCollection::loadSpritePack): ~4 MB of them, most only for
# the menus or single places, would otherwise be in memory all the time. Made by the PSP mod converter (a Windows
# program, run through WSL's interop); without it the game loads the sprite files as on other platforms.
CONVERTER="${MOD_CONVERTER:-$SRC/psp/tools/ModConverter/bin/Release/net48/S3AIR-PSP-ModConverter.exe}"
rm -f "$OUT/data/sprites_psp.bin"
if [ -f "$CONVERTER" ] && command -v wslpath > /dev/null; then
	"$CONVERTER" --pack-sprites "$(wslpath -w "$OUT/data")" "$(wslpath -w "$OUT/data/sprites_psp.bin")" | tr -d '\r'
else
	echo "Note: no mod converter (psp/tools/ModConverter: dotnet build -c Release), the game's sprites stay in memory all the time"
fi

# The disclaimer image is 1920x1075 (8 MB in memory, more than the PSP has left when the menus start): 480 wide is
# enough for the PSP's screen
if command -v ffmpeg > /dev/null; then
	ffmpeg -v error -y -i "$SRC/Oxygen/sonic3air/data/images/menu/disclaimer.png" -vf scale=480:-2 "$OUT/data/images/menu/disclaimer.png"
	# The menu backgrounds at half size: a quarter of the memory (they are drawn stretched, see MenuBackground's "drawQuad"),
	# opening the options in the game has little memory left
	for f in dataselect_bg dataselect_dark_bg levelselect_bg; do
		ffmpeg -v error -y -i "$SRC/Oxygen/sonic3air/data/images/menu/$f.png" -vf "scale=iw/2:ih/2:flags=lanczos" "$OUT/data/images/menu/$f.png"
	done
else
	echo "Note: no ffmpeg, the disclaimer image stays 1920x1075 (the menus will run out of memory on the PSP)"
fi

# The converted audio (psp/tools/convert_audio.py) replaces the emulated one: its JSON files point to .at3 and .wav files
if [ -d "$SRC/psp/out/audio" ]; then
	# Only the audio banks (audio_bank.bin, one file per folder with all .wav and .at3 files in it) and the JSON files
	rsync -a --exclude '*.wav' --exclude '*.at3' "$SRC/psp/out/audio/" "$OUT/data/audio/"
	find "$OUT/data/audio" \( -name '*.wav' -o -name '*.at3' -o -name '*.ogg' \) -delete	# (left from older packages)
else
	echo "Note: no converted audio in psp/out/audio, the game will be silent (see psp/README.md)"
fi

if [ -n "$ROM" ]; then
	cp "$ROM" "$OUT/Sonic_Knuckles_wSonic3.bin"
fi

echo "Game folder ready: $OUT"
du -sh "$OUT"
