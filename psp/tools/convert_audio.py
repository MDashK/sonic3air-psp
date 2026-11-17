#!/usr/bin/env python3
"""Converts the audio for the PSP: music and jingles to ATRAC3 (decoded by the PSP's Media Engine), sound effects to WAV.

  convert_audio.py --export DIR --out DIR [--original DIR] [--remastered DIR] [--atracdenc PATH] [--ffmpeg PATH]
                   [--sfx-rate 22050] [--sfx-mono]

--export      the desktop build's audio export (psp/build_desktop.sh): <key>.wav files and manifest.json
--out         where the PSP's "data/audio" goes: original/audio_default.json + files, and remastered/ if asked for
--original    the original package (Oxygen/sonic3air/data/audio/original), for its own recorded .ogg files
--remastered  the remastered soundtrack (Oxygen/sonic3air/data/audio/remastered: .ogg files, audio_replacements.json)

The export's WAV files are big (~1.4 GB): delete them after converting, the desktop build makes them again if needed.

The JSON files are the game's own audio definition format, with "File" entries instead of emulation, so the game
loads them as usual (the PSP build plays .at3 and .wav files, see PSPAudioSources.h). Converted files are cached by
the source's content, so running it again only converts what changed.
"""
import argparse
import array
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave


def load_lenient_json(path):
    """The game's JSON files have // comments and trailing commas."""
    with open(path, 'r', encoding='utf-8') as f:
        text = f.read()
    # Drop "//" comments (outside of strings)
    out = []
    in_string = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_string:
            out.append(c)
            if c == '\\' and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
            out.append(c)
        elif text.startswith('//', i):
            while i < len(text) and text[i] != '\n':
                i += 1
            continue
        else:
            out.append(c)
        i += 1
    text = re.sub(r',(\s*[}\]])', r'\1', ''.join(out))
    return json.loads(text)


class Converter:
    def __init__(self, atracdenc, ffmpeg, cache_dir):
        self.atracdenc = atracdenc
        self.ffmpeg = ffmpeg
        self.cache_dir = cache_dir
        os.makedirs(cache_dir, exist_ok=True)

    def _cached(self, source, kind, make, output):
        with open(source, 'rb') as f:
            digest = hashlib.sha1(f.read() + kind.encode()).hexdigest()
        cached = os.path.join(self.cache_dir, digest + '.' + kind.split(':')[0])
        if not os.path.exists(cached):
            make(cached + '.tmp')
            os.replace(cached + '.tmp', cached)
        shutil.copyfile(cached, output)
        return cached

    def to_atrac3(self, source, output):
        """Any audio file as ATRAC3, 44.1 kHz stereo (what the PSP's decoder plays)."""
        def make(target):
            with tempfile.TemporaryDirectory() as tmp:
                wav = os.path.join(tmp, 'in.wav')
                at3 = os.path.join(tmp, 'out.at3')    # (atracdenc picks the container by the extension: .at3 is RIFF, else OMA)
                subprocess.run([self.ffmpeg, '-v', 'error', '-y', '-i', source, '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', wav], check=True)
                subprocess.run([self.atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                with open(at3, 'rb') as f:
                    if f.read(4) != b'RIFF':
                        raise RuntimeError('atracdenc did not write a RIFF file for ' + source)
                shutil.move(at3, target)
        self._cached(source, 'at3', make, output)

    def to_atrac3_padded(self, source, output, loop_start):
        """Like to_atrac3, but resampled with soxr and with ATRAC3_END_PADDING samples added after the end: what follows
        "loop_start" for music, silence for anything else (loop_start < 0). Returns the samples (44.1 kHz) before the padding."""
        def make(target):
            with tempfile.TemporaryDirectory() as tmp:
                wav = os.path.join(tmp, 'in.wav')
                at3 = os.path.join(tmp, 'out.at3')
                subprocess.run([self.ffmpeg, '-v', 'error', '-y', '-i', source, '-af', 'aresample=resampler=soxr:precision=28',
                                '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', wav], check=True)
                with wave.open(wav, 'rb') as w:
                    frames = w.getnframes()
                    data = w.readframes(frames)
                padding = data[loop_start * 4:(loop_start + ATRAC3_END_PADDING) * 4] if loop_start >= 0 else b''
                padding += b'\0' * (ATRAC3_END_PADDING * 4 - len(padding))
                with wave.open(wav, 'wb') as w:
                    w.setnchannels(2)
                    w.setsampwidth(2)
                    w.setframerate(44100)
                    w.writeframes(data + padding)
                subprocess.run([self.atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                with open(at3, 'rb') as f:
                    if f.read(4) != b'RIFF':
                        raise RuntimeError('atracdenc did not write a RIFF file for ' + source)
                shutil.move(at3, target)
                with open(target[:-len('.tmp')] + '.frames', 'w') as f:
                    f.write(str(frames))
        cached = self._cached(source, 'at3:padded:%d:%d' % (loop_start, ATRAC3_END_PADDING), make, output)
        with open(cached + '.frames') as f:
            return int(f.read())

    def to_wav(self, source, output, rate, mono):
        def make(target):
            subprocess.run([self.ffmpeg, '-v', 'error', '-y', '-i', source, '-ar', str(rate), '-ac', '1' if mono else '2',
                            '-c:a', 'pcm_s16le', '-f', 'wav', '-bitexact', target], check=True)
        self._cached(source, 'wav:%d:%d' % (rate, mono), make, output)


def make_mono_if_same(path, tolerance=8):
    """Most sound effects have the same left and right channel: as mono they read twice as fast from the memory stick."""
    with wave.open(path, 'rb') as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2:
            return False
        rate = w.getframerate()
        samples = array.array('h')
        samples.frombytes(w.readframes(w.getnframes()))
    left = samples[0::2]
    right = samples[1::2]
    if any(abs(l - r) > tolerance for l, r in zip(left, right)):
        return False
    mono = array.array('h', ((l + r) >> 1 for l, r in zip(left, right)))
    with wave.open(path + '.tmp', 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(mono.tobytes())
    os.replace(path + '.tmp', path)
    return True


# The ATRAC3 audio as the PSP decodes it is behind the source (atracdenc's encoder plus the decoder: a click comes out
# 1171 samples later with ffmpeg's decoder, the PSP's decoder may skip 69 of them). Loop points use this a bit larger value:
# going back from the loop end then happens inside the padding (a copy of what follows the loop start), which is seamless
# for any actual delay up to this
ATRAC3_DELAY = 1280
# Added after the end before encoding: the encoder cuts the last partial frame, and the decoded audio is ATRAC3_DELAY
# samples behind, so the actual end would be lost otherwise
ATRAC3_END_PADDING = 4096

BANK_ALIGNMENT = 2048

def write_bank(out_dir, entries):
    """Packs the .wav and .at3 files into "audio_bank.bin": the PSP opens that once, instead of every file
    (opening a file in a folder with hundreds of files takes 12-15 ms on a memory stick, and the game stalls meanwhile).
    Format: "S3AB", version 1, number of files, 0 (uint32 each), then per file: name (56 bytes, zero-padded), offset, size."""
    align = lambda value: (value + BANK_ALIGNMENT - 1) // BANK_ALIGNMENT * BANK_ALIGNMENT
    names = sorted(set(entry['File'] for _, entry in entries if entry.get('File', '').endswith(('.wav', '.at3'))))
    index = bytearray()
    offset = align(16 + 64 * len(names))
    layout = []
    for name in names:
        encoded = name.encode('utf-8')
        if len(encoded) >= 56:
            raise RuntimeError('File name too long for the audio bank: ' + name)
        size = os.path.getsize(os.path.join(out_dir, name))
        index += encoded.ljust(56, b'\0') + struct.pack('<II', offset, size)
        layout.append((name, offset))
        offset = align(offset + size)
    path = os.path.join(out_dir, 'audio_bank.bin')
    with open(path + '.tmp', 'wb') as f:
        f.write(b'S3AB' + struct.pack('<III', 1, len(names), 0) + index)
        for name, position in layout:
            f.write(b'\0' * (position - f.tell()))
            with open(os.path.join(out_dir, name), 'rb') as source:
                f.write(source.read())
    os.replace(path + '.tmp', path)
    print('Audio bank: %d files, %.1f MB' % (len(names), os.path.getsize(path) / 1048576))


def write_json(path, entries):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('{\n')
        lines = []
        for key, entry in entries:
            fields = ', '.join('"%s": "%s"' % (name, str(value).replace('"', '\\"')) for name, value in entry.items())
            lines.append('\t"%s": { %s }' % (key, fields))
        f.write(',\n'.join(lines))
        f.write('\n}\n')


def convert_original(conv, export_dir, original_dir, out_dir, sfx_rate, sfx_mono):
    os.makedirs(out_dir, exist_ok=True)
    if export_dir:
        # (an export of only some entries, see S3AIR_EXPORT_AUDIO_ONLY: the others stay as converted before)
        manifest = load_lenient_json(os.path.join(export_dir, 'manifest.json'))
        previous_path = os.path.join(out_dir, 'audio_default.json')
        previous = load_lenient_json(previous_path) if os.path.exists(previous_path) else {}
    else:
        # No export: keep what was converted before (the export's WAV files are big and get deleted), redo the rest
        manifest = {}
        previous = load_lenient_json(os.path.join(out_dir, 'audio_default.json'))
    entries = []
    sizes = {'Music': 0, 'Jingle': 0, 'Sound': 0}

    # The original package's own audio files (recorded sounds and a few songs, not emulated)
    if original_dir:
        for key, info in load_lenient_json(os.path.join(original_dir, 'audio_default.json')).items():
            file = info.get('File', '')
            if not file.endswith('.ogg') or 'Source' in info:
                continue
            kind = info.get('Type', 'Sound')
            entry = {k: v for k, v in info.items() if k != 'File'}
            source = os.path.join(original_dir, file)
            if kind == 'Jingle':
                filename = os.path.splitext(file)[0] + '.at3'
                conv.to_atrac3_padded(source, os.path.join(out_dir, filename), -1)    # (silence after it, see to_atrac3_padded)
            elif kind == 'Music':
                filename = os.path.splitext(file)[0] + '.at3'
                conv.to_atrac3(source, os.path.join(out_dir, filename))
            else:
                filename = os.path.splitext(file)[0] + '.wav'
                conv.to_wav(source, os.path.join(out_dir, filename), sfx_rate, sfx_mono)
            entry['File'] = filename
            entries.append((key, entry))
            sizes[kind] += os.path.getsize(os.path.join(out_dir, filename))
            print('  %-24s %s' % (key, filename), flush=True)
    for key, info in manifest.items():
        source = os.path.join(export_dir, key + '.wav')
        kind = info['Type']
        entry = {'Name': info['Name']}
        if kind in ('Music', 'Jingle'):
            # With padding after the end (see to_atrac3_padded): jingles end in silence instead of the encoder's cut-off
            # last frame, music loops seamlessly at "LoopEnd"
            filename = key + '.at3'
            loop_start = max(int(info['LoopStart']), 0) if kind == 'Music' else -1
            frames = conv.to_atrac3_padded(source, os.path.join(out_dir, filename), loop_start)
            if kind == 'Music':
                entry['LoopStart'] = str(loop_start + ATRAC3_DELAY)
                entry['LoopEnd'] = str(frames + ATRAC3_DELAY)
        else:
            filename = key + '.wav'
            conv.to_wav(source, os.path.join(out_dir, filename), sfx_rate, sfx_mono)
            entry['Channel'] = 'multiple' if info['Channel'] == 'ff' else info['Channel']
            if info.get('Source') == 'EmulationContinuous' and int(info.get('LoopStart', -1)) >= 0:
                # Continuous sound effect: loops from there while the game keeps starting it (in the WAV's own samples)
                entry['LoopStart'] = str(int(info['LoopStart']) * sfx_rate // 44100)
                entry['Continuous'] = '1'
        entry['File'] = filename
        entry['Type'] = kind
        if abs(float(info['Volume']) - 1.0) > 0.001:
            entry['Volume'] = info['Volume']
        entries.append((key, entry))
        sizes[kind] += os.path.getsize(os.path.join(out_dir, filename))
        print('  %-24s %s' % (key, filename), flush=True)
    # Entries kept from before (no export given), unless converted again just now
    done = set(key for key, _ in entries)
    entries += [(key, entry) for key, entry in previous.items() if key not in done]

    # Sound effects as mono where both channels are the same (also the ones kept from before)
    mono = 0
    sounds = sorted(set(entry['File'] for _, entry in entries if entry.get('File', '').endswith('.wav')))
    for filename in sounds:
        if make_mono_if_same(os.path.join(out_dir, filename)):
            mono += 1
    sizes['Sound'] = sum(os.path.getsize(os.path.join(out_dir, filename)) for filename in sounds)
    print('Sounds: %d of %d are mono' % (mono, len(sounds)))

    write_json(os.path.join(out_dir, 'audio_default.json'), entries)
    write_bank(out_dir, entries)
    print('Original soundtrack: %d entries, music %.1f MB, jingles %.1f MB, sounds %.1f MB' %
          (len(entries), sizes['Music'] / 1048576, sizes['Jingle'] / 1048576, sizes['Sound'] / 1048576))


def probe_sample_rate(ffmpeg, source):
    ffprobe = os.path.join(os.path.dirname(ffmpeg), 'ffprobe') if os.path.dirname(ffmpeg) else 'ffprobe'
    result = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'a:0', '-show_entries', 'stream=sample_rate',
                             '-of', 'csv=p=0', source], check=True, capture_output=True, text=True)
    return int(result.stdout.strip())


def convert_remastered(conv, remastered_dir, out_dir, sfx_rate, sfx_mono):
    """The remastered soundtrack (48 kHz .ogg files) as ATRAC3 at 44.1 kHz, packed into the folder's audio bank.
    Music gets "LoopStart" and "LoopEnd" in the samples the PSP decodes: the end is followed by what comes after the loop
    start (see to_atrac3_padded), and the game goes back at "LoopEnd", so the loop is seamless.
    The Special Stage's "1C_speedup" has its speed-ups in the recording itself, as on the desktop."""
    definitions = load_lenient_json(os.path.join(remastered_dir, 'audio_replacements.json'))
    os.makedirs(out_dir, exist_ok=True)
    entries = []
    total = 0
    for key, info in definitions.items():
        if 'File' not in info:
            continue
        source = os.path.join(remastered_dir, info['File'])
        if not os.path.exists(source):
            continue
        entry = {k: v for k, v in info.items() if k not in ('File', 'LoopStart')}
        if info.get('Type', '') == 'Sound':
            # Sound effects as WAV, like the original soundtrack's
            filename = os.path.splitext(info['File'])[0] + '.wav'
            conv.to_wav(source, os.path.join(out_dir, filename), sfx_rate, sfx_mono)
            make_mono_if_same(os.path.join(out_dir, filename))
            entry['File'] = filename
            entries.append((key, entry))
            total += os.path.getsize(os.path.join(out_dir, filename))
            print('  %-24s %s' % (key, filename), flush=True)
            continue
        is_music = (info.get('Type', '') == 'Music')
        rate = probe_sample_rate(conv.ffmpeg, source)
        loop_start = (int(info.get('LoopStart', 0)) * 44100 + rate // 2) // rate if is_music else -1
        filename = os.path.splitext(info['File'])[0] + '.at3'
        frames = conv.to_atrac3_padded(source, os.path.join(out_dir, filename), loop_start)
        if is_music:
            entry['LoopStart'] = str(loop_start + ATRAC3_DELAY)
            entry['LoopEnd'] = str(frames + ATRAC3_DELAY)
        entry['File'] = filename
        entries.append((key, entry))
        total += os.path.getsize(os.path.join(out_dir, filename))
        print('  %-24s %s' % (key, filename), flush=True)
    write_json(os.path.join(out_dir, 'audio_replacements.json'), entries)
    write_bank(out_dir, entries)
    print('Remastered soundtrack: %d entries, %.1f MB' % (len(entries), total / 1048576))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--export', help='without it, the entries converted before are kept')
    ap.add_argument('--out', required=True)
    ap.add_argument('--original', help="the original package's folder (Oxygen/sonic3air/data/audio/original), for its own audio files")
    ap.add_argument('--remastered')
    ap.add_argument('--skip-original', action='store_true', help='only convert the remastered soundtrack')
    ap.add_argument('--atracdenc', default=os.path.expanduser('~/src/atracdenc/build/atracdenc'))
    ap.add_argument('--ffmpeg', default='ffmpeg')
    ap.add_argument('--cache', help='converted files by content (default: .audio_cache next to --out)')
    ap.add_argument('--sfx-rate', type=int, default=44100)
    ap.add_argument('--sfx-mono', action='store_true')
    args = ap.parse_args()

    cache = args.cache or os.path.join(os.path.dirname(os.path.abspath(args.out)), '.audio_cache')
    conv = Converter(args.atracdenc, args.ffmpeg, cache)
    if not args.skip_original:
        convert_original(conv, args.export, args.original, os.path.join(args.out, 'original'), args.sfx_rate, args.sfx_mono)
    if args.remastered:
        convert_remastered(conv, args.remastered, os.path.join(args.out, "remastered"), args.sfx_rate, args.sfx_mono)
    return 0


if __name__ == '__main__':
    sys.exit(main())
