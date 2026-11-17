/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: exports the emulated audio (music, jingles, sound effects) as WAV files.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once


// The PSP can't emulate the Mega Drive's sound chips in time, so its audio is prepared on a PC: this renders every
// emulated audio definition of the original soundtrack with the same sound driver and chip emulation as the game,
// into "<key>.wav" files plus a "manifest.json" describing them (type, length, loop start, channel, volume).
//  - Music loops are found by the sound driver's state: when it repeats exactly, the song loops from there.
//  - Music also gets a "<key>_fast.wav" version with the speed shoes' tempo.
//  - Run with the environment variable S3AIR_EXPORT_AUDIO set to the output directory (desktop build).
namespace AudioExport
{
	void exportAll(const std::wstring& outputDirectory);
}
