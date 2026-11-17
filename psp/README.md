# Sonic 3 A.I.R. - PSP port

A port of Sonic 3 A.I.R. to the PSP, for the models with 64 MB of RAM (PSP-2000, PSP-3000, PSP Go, PSP Street) with
custom firmware. The PSP-1000 (32 MB) doesn't have enough memory.

## Installing (players)

1. Copy the `PSP` folder of the release onto the memory stick (into its root, next to the existing `PSP` folder).
2. Put your own Sonic 3 & Knuckles ROM into `PSP/GAME/Sonic3AIR/`, named `Sonic_Knuckles_wSonic3.bin`. It is not
   included: you need a legally obtained copy (e.g. from the Steam version of "Sonic 3 & Knuckles").
3. Start "Sonic 3 A.I.R." from the XMB (Game > Memory Stick).

Both soundtracks are included, the remastered one (the default, as on other platforms) and the emulated original one,
switched in Options > Audio > Soundtrack Type.

## Mods

Mods made for the PC version need converting first, with the Sonic 3 A.I.R. PSP Mod Converter
(`psp/tools/ModConverter`, a Windows program, see its README): it puts the converted mods into
`PSP/GAME/Sonic3AIR/savedata/mods` on the memory stick, where the game's "Mods" menu finds them.

- Sprites get cut from their sheets and packed (`sprites/sprites_psp.bin`); the game loads a sprite's pixels when it's
  used and frees the least recently used ones when the sprite cache is full, so a mod with sprites for all characters
  only takes the memory of what's in use (`SpriteCollection::loadSpritePack`).
- Music and jingles get converted to ATRAC3, sound effects to WAV, as for the game's own soundtracks. Mod audio still
  in Ogg Vorbis is skipped on the PSP.
- Scripts get compiled by the PSP when the mod is loaded (tens of milliseconds for a typical mod).
- Memory: the active mods together get 1,536 KB (`ModManager::PSP_MOD_MEMORY_BUDGET`). The converter writes what a mod
  needs into `psp.json`; the game doesn't activate mods beyond the budget, and shows unconverted mods as failed.

## How it runs

- The game logic (the scripts) runs at the original 60 updates per second; the screen shows a fixed 30 fps (every
  other update is drawn).
- The game screen (400x224, scaled to 480x269) is drawn by the PSP's GPU (the GE): see
  `Oxygen/oxygenengine/source/oxygen/rendering/psp/`. Planes, sprites and priorities are done like on the Mega Drive,
  the CPU only builds the GE's command list. The menus and the rest of the UI are drawn by the GE too
  (`PSPUiRenderer.cpp`), straight onto the screen.
- Script code that runs very often is also in C++ (`Oxygen/sonic3air/source/sonic3air/scriptimpl/ScriptImplementations.cpp`,
  e.g. sprite output, rings, the tile lookup for collisions), giving exactly the same results as the scripts.
- Memory: ~31-36 MB of the ~40 MB heap in game. Images only some menus use are loaded while those menus are shown.
- Audio: emulating the Mega Drive's sound chips (as the game does on other platforms) takes too much CPU here, so the
  audio is rendered on a PC beforehand, with the game's own sound emulation (see below). Music and jingles are ATRAC3,
  decoded by the PSP's Media Engine (only two decoders are available: tracks take turns, see `PSPAudioSources.cpp`);
  sound effects are 22 kHz WAV, mono where possible. Each soundtrack is in one file, `data/audio/*/audio_bank.bin`:
  opening a file in a folder with hundreds of files takes 12-15 ms on a memory stick, and the game would stall each
  time a sound plays for the first time.
- The memory stick lets the game have only ~10 files open at once (PPSSPP allows many more): sprite packs and audio
  banks stay open only while they're used (at most two sprite packs, and the files of the last two music tracks).
- Sound effects and the mods' sprites get more memory in levels (2.5 MB of audio, 1 MB more sprite cache) than elsewhere
  (1.5 MB, no extra): the special stages need that memory, and it's freed in the frame they start
  (`Game::onPostUpdateFrame`). The least recently used ones go first; sound effects freed for a special stage get loaded
  again while the level fades back in.
- The ROM is in memory only once (no unmodified copy, that's 4 MB): what mods' ROM injections and script writes
  overwrite is kept and put back on each reset, before the active mods' injections (`RuntimeMemory::clear`).
- The scripts' shared memory is 256 KB instead of 1 MB (the game's scripts use 0x800000 .. 0x818080); what's beyond
  reads as zero.
- Not available on the PSP: the renderer, screen filter, window and controller options (fixed buttons, single player),
  rumble, the "System" options tab, the background blur.

## Requirements (building)

- [pspdev](https://github.com/pspdev/pspdev) (toolchain and pspsdk), e.g. in WSL on Windows
- CMake and rsync
- For the scripts and audio: a desktop (Linux) build of this source, see `Oxygen/sonic3air/build/_cmake/how-to-build.txt`
- For the audio: [atracdenc](https://github.com/dcherednik/atracdenc), ffmpeg (with soxr) and Python 3
- The Sonic 3 & Knuckles ROM (`Sonic_Knuckles_wSonic3.bin`) in `psp/rom/`, legally obtained (not part of this repository)

## Building

```
psp/build.sh                          # -> psp/out/EBOOT.PBP, psp/out/sonic3air.prx
psp/build_desktop.sh                  # -> psp/out/scripts.bin, and the emulated audio: psp/out/audio_export
python3 psp/tools/convert_audio.py --export psp/out/audio_export --original Oxygen/sonic3air/data/audio/original \
    --remastered Oxygen/sonic3air/data/audio/remastered --out psp/out/audio --sfx-rate 22050
rm -r psp/out/audio_export            # (WAV, ~1.4 GB) not needed any more, see below
psp/package.sh psp/out/scripts.bin    # -> psp/out/PSP/GAME/Sonic3AIR, the game folder for the memory stick
```

- `build.sh` mirrors the sources into `~/s3air-psp` (compiling from a Windows drive is very slow) and builds there.
- `scripts.bin` are the compiled scripts of this exact version: the PSP can't compile them itself. The desktop build
  compiles them on startup; `SCRIPTS_ONLY=1 psp/build_desktop.sh` skips the audio export.
- The audio export plays every sound with the game's sound emulation and records it (also the special stage's tempo
  steps, the spindash pitch steps and the loops of continuous sounds). `EXPORT_ONLY=29,2B psp/build_desktop.sh`
  exports only those keys. The converter caches its results by content (`psp/out/.audio_cache`), and without
  `--export` it keeps the entries it converted before.
- Music gets "LoopStart" and "LoopEnd" in the decoded audio's samples, and a bit of padding after the end (ATRAC3
  cuts the last partial frame, and the decoded audio is delayed): see `to_atrac3_padded` in `convert_audio.py`.

## Testing

- `psp/tools/psplink_run.sh <seconds>`: runs on a real PSP through PSPLink, keeps the shell attached (for crash
  dumps), and copies the logs back once the game ended. The PRX is run from the memory stick: reading directories
  through `host0:` takes seconds per directory there.
- `psp/tools/psplink_sync.sh <paths>`: copies files from the host game folder to the memory stick through PSPLink.
- `psp/tools/psplink_fetch.sh <files>`: copies files from the memory stick back.
- `psp/tools/compare_scripts.sh <script file> [frames] [level]`: checks that C++ versions of script code give the same
  emulated RAM and VRAM as the original scripts (desktop build).
- `psp/tools/ppsspp_run.ps1`: runs the game folder in PPSSPP (Windows).
- `psp/tools/ModConverter`: the mod converter (C#, .NET Framework 4.8): `dotnet build -c Release` there. Its command line
  mode (`--cli`, see its README) converts or only analyzes mods without the window.

The tools' default paths (`HOST_DIR`, `ORIGINAL`, ...) are those of the machine this was made on: set them for yours.

A `bench.txt` next to the EBOOT sets environment variables (`KEY=VALUE` per line), for benchmark and test runs. Don't
put one into a release: the logs get written to the memory stick all the time.

| Variable | |
|---|---|
| `S3AIR_BENCH=1` | Benchmark mode: statistics in `bench.log` (scripts) and `render.log` (drawing), exits after the frames below |
| `S3AIR_BENCH_FRAMES=1800` | Number of simulation frames |
| `S3AIR_BENCH_LEVEL=0000` | Start directly in this zone and act (hex, e.g. `0100` = Hydrocity act 1) |
| `S3AIR_BENCH_BLUESPHERE=1` | Start the Blue Sphere game instead |
| `S3AIR_BENCH_INPUT=1` | Input: `1` = run right and jump now and then, `2` = for Blue Spheres, `0` = none, `-1` = the player's own |
| `S3AIR_BENCH_PROFILE=1` | The script functions taking the most time, every 300 frames |
| `S3AIR_BENCH_LOG=bench.log` | Where the statistics go |
| `S3AIR_BENCH_MUTE=1` | No sounds at all |
| `S3AIR_BENCH_NATIVES=1` | Time spent in the native functions the scripts call, in `bench.log`; logs the ones over 20 ms (slows down the scripts a bit) |
| `S3AIR_BENCH_SHOTS=200,400` | Save the game screen as `shot_NNNN.bmp` at these shown frames |
| `S3AIR_DEBUG_PRESENT=1` | Frames shown and UI statistics in the log file, once a second |
| `S3AIR_AUDIO_CAPTURE=1` | Keeps the last 24 seconds of audio output: L + R + SELECT saves them as `audio_NN.wav` |
| `S3AIR_PSP_SOFTWARE_UI=1` | Draw the UI with the software drawer (as before the GE UI) |
