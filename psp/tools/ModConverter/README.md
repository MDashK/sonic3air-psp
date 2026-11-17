# Sonic 3 A.I.R. PSP Mod Converter

Makes Sonic 3 A.I.R. mods (made for the PC version) ready for the PSP port. Runs on Windows 10 and 11, nothing to install.

## Using it

1. Start `S3AIR-PSP-ModConverter.exe`.
2. Choose your mods: the PC game's mods folder, any folder with mods in it (also `.zip` files), or a single mod.
   The list shows each mod's content, how much of the PSP's memory it needs, and anything that won't work on the PSP.
3. Choose where the converted mods go: on the memory stick, `PSP/GAME/Sonic3AIR/savedata/mods`
   (if the memory stick is connected with the game on it, this is filled in already).
4. Check the mods you want and press "Convert checked mods".
5. On the PSP: start the game, open "Mods" in the main menu, and make the mods active there.

The PSP only has 1,536 KB of memory for mods: all the mods that are active at the same time must fit in there together.
The game doesn't let you activate mods beyond that, and mods that weren't converted with this tool show as failed.

## What it does

- **Sprites** (`sprites/*.json` with their `.bmp` / `.png` sheets): cut from their sheets, exactly as the game would do it,
  and packed into `sprites/sprites_psp.bin`. The PSP reads that sprite by sprite, without ever loading a whole sheet.
- **Audio** (`audio/audio_replacements.json` with `.ogg` files): music and jingles to ATRAC3 (decoded by the PSP's own
  audio hardware, streamed), sound effects to 22 kHz WAV (mono where possible), all packed into `audio/audio_bank.bin`.
  Loop points are converted too. Audio that uses the sound chip emulation can't be played on the PSP and is left out.
- **Scripts** are copied as they are: the PSP compiles them when the mod gets loaded.
- Everything else is copied, except files the game never reads (source images like `.psd`, `Thumbs.db`, ...).
- `psp.json` in the mod's folder notes what the mod needs in memory: the game checks it.

Converting again replaces what the tool made before (the folders with a `psp.json`), never anything else.

For scripts and tests: `S3AIR-PSP-ModConverter.exe --cli <mods folder> <output folder, or - to only analyze> <report file> [name filter]`

## Credits

ATRAC3 encoding: [atracdenc](https://github.com/dcherednik/atracdenc) (LGPL 2.1, see `atracdenc-LICENSE.txt`), run as a separate
program. Ogg Vorbis decoding: [NVorbis](https://github.com/NVorbis/NVorbis) (MIT license).
