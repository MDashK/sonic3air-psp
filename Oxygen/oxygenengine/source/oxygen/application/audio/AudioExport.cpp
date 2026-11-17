/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: exports the emulated audio (music, jingles, sound effects) as WAV files.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/application/audio/AudioExport.h"
#include "oxygen/application/audio/AudioCollection.h"
#include "oxygen/simulation/sound/SoundDriver.h"
#include "oxygen/simulation/sound/SoundEmulation.h"

#include <unordered_map>


namespace
{
	static const constexpr int SAMPLE_RATE = 44100;
	static const constexpr uint8 SPEED_SHOES_TEMPO = 8;		// What the speed shoes set (see AudioPlayer::applyAudioModifierSingle: relative speed 1.25)

	struct RenderResult
	{
		std::vector<int16> mSamples;	// Stereo, interleaved
		int mLoopStart = -1;			// In samples, -1 if not looping
	};

	bool render(const AudioCollection::SourceRegistration& sourceReg, bool findLoop, uint8 tempoSpeedup, RenderResult& outResult, bool continuous = false, uint8 spindashRev = 0)
	{
		SoundDriver soundDriver;
		SoundEmulation soundEmulation;

		// Same setup as AudioSourceManager::addEmulationAudioSource
		std::vector<uint8> customContent;
		if (!sourceReg.mSourceFile.empty())
		{
			if (!FTX::FileSystem->readFile(sourceReg.mSourceFile, customContent) || customContent.empty())
				return false;
			soundDriver.setFixedContent(&customContent[0], (uint32)customContent.size(), sourceReg.mContentOffset);
		}
		else if (sourceReg.mSourceAddress != 0)
		{
			soundDriver.setSourceAddress(sourceReg.mSourceAddress);
		}

		soundEmulation.init(SAMPLE_RATE, 60.0);
		soundDriver.reset();
		if (spindashRev != 0)
			soundDriver.setSpindashRev(spindashRev);		// (as after playing the spindash sound that many times)
		soundDriver.playSound(sourceReg.mEmulationSfxId);

		// Same loop as EmulationAudioSource::jobFunc, plus the loop detection
		std::unordered_map<uint64, int> stateToSamplePosition;
		const int maxSamples = SAMPLE_RATE * (continuous ? 20 : findLoop ? 600 : 60);
		static int16 soundBuffer[0x10000];
		outResult.mSamples.clear();
		outResult.mLoopStart = -1;

		// (shorter repeats are the driver waiting, not a loop; continuous sound effects have short loops, and get started again all the time)
		const int MIN_LOOP_LENGTH = continuous ? (SAMPLE_RATE / 60) : (SAMPLE_RATE * 3);
		bool started = false;
		std::vector<std::vector<uint8>> tickStates;
		std::vector<int> tickPositions;
		while ((int)outResult.mSamples.size() / 2 < maxSamples)
		{
			if (findLoop && started)
			{
				const uint64 stateHash = soundDriver.getStateHash();
				const int position = (int)outResult.mSamples.size() / 2;
				const auto it = stateToSamplePosition.find(stateHash);
				if (it != stateToSamplePosition.end() && position - it->second >= MIN_LOOP_LENGTH)
				{
					// The driver is in the same state as before: everything from here on repeats
					outResult.mLoopStart = it->second;
					break;
				}
				stateToSamplePosition[stateHash] = position;

				// Kept for the fallback below
				std::vector<uint8>& state = vectorAdd(tickStates);
				soundDriver.getLoopState(state);
				tickPositions.push_back(position);
			}

			// Continuous sound effects: started again in each frame, like the game does while they should go on (the driver then loops them)
			if (continuous && started)
				soundDriver.playSound(sourceReg.mEmulationSfxId);

			const SoundDriver::UpdateResult updateResult = soundDriver.update();
			const std::vector<SoundChipWrite>& writes = soundDriver.getSoundChipWrites();
			const uint32 length = soundEmulation.update(soundBuffer, writes);	// Length in samples

			bool isPlaying = (updateResult == SoundDriver::UpdateResult::CONTINUE);
			if (updateResult == SoundDriver::UpdateResult::FINISHED)
			{
				// Check if the sound chips still produce output
				for (uint32 i = 0; i < length * 2; ++i)
				{
					if (soundBuffer[i] < -2 || soundBuffer[i] > 0)
					{
						isPlaying = true;
						break;
					}
				}
			}
			if (!isPlaying)
				break;

			// The loop detection starts once there's sound (before that, the driver is still setting up the song)
			if (!started)
			{
				for (uint32 i = 0; i < length * 2; ++i)
				{
					if (soundBuffer[i] < -2 || soundBuffer[i] > 0)
					{
						started = true;
						break;
					}
				}

				// The speed shoes' tempo, set like the game does: while the song plays (starting the song resets it)
				if (started && tempoSpeedup != 0)
					soundDriver.setTempoSpeedup(tempoSpeedup);
			}

			outResult.mSamples.insert(outResult.mSamples.end(), soundBuffer, soundBuffer + length * 2);
		}

		// No exact loop: in some songs the tracks have loops of slightly different lengths, so they drift apart a bit
		// each time through (on the Mega Drive too) and the whole state never repeats. Then each track's own loop is
		// looked for, and the longest one is used: the others are only a few updates off at the loop point.
		if (findLoop && outResult.mLoopStart < 0 && (int)outResult.mSamples.size() / 2 >= maxSamples && !tickStates.empty())
		{
			int bestStart = -1;
			int bestEnd = -1;
			for (int track = 0; track < 9; ++track)
			{
				std::unordered_map<uint64, size_t> firstTick;
				for (size_t tick = 0; tick < tickStates.size(); ++tick)
				{
					const uint8* trackState = &tickStates[tick][0x40 + track * 12];
					if ((trackState[0] & 0x80) == 0)
						continue;	// Track not playing
					const uint64 hash = rmx::getMurmur2_64(trackState, 12);
					const auto it = firstTick.find(hash);
					if (it != firstTick.end() && tickPositions[tick] - tickPositions[it->second] >= MIN_LOOP_LENGTH)
					{
						if (tick - it->second > (size_t)(bestEnd - bestStart))
						{
							bestStart = (int)it->second;
							bestEnd = (int)tick;
						}
						break;
					}
					firstTick.emplace(hash, tick);
				}
			}
			if (bestStart >= 0)
			{
				outResult.mLoopStart = tickPositions[bestStart];
				outResult.mSamples.resize((size_t)tickPositions[bestEnd] * 2);
			}
		}
		return true;
	}

	bool writeWav(const std::wstring& filename, const std::vector<int16>& samples)
	{
		const uint32 dataBytes = (uint32)(samples.size() * sizeof(int16));
		std::vector<uint8> content(44 + dataBytes);
		uint8* header = &content[0];
		memcpy(&header[0], "RIFF", 4);
		*(uint32_ua*)&header[4]  = 36 + dataBytes;
		memcpy(&header[8], "WAVEfmt ", 8);
		*(uint32_ua*)&header[16] = 16;
		*(uint16_ua*)&header[20] = 1;		// PCM
		*(uint16_ua*)&header[22] = 2;		// Stereo
		*(uint32_ua*)&header[24] = SAMPLE_RATE;
		*(uint32_ua*)&header[28] = SAMPLE_RATE * 4;
		*(uint16_ua*)&header[32] = 4;
		*(uint16_ua*)&header[34] = 16;
		memcpy(&header[36], "data", 4);
		*(uint32_ua*)&header[40] = dataBytes;
		if (dataBytes > 0)
			memcpy(&content[44], &samples[0], dataBytes);
		return FTX::FileSystem->saveFile(filename, content);
	}

	const char* getTypeName(AudioCollection::AudioDefinition::Type type)
	{
		switch (type)
		{
			case AudioCollection::AudioDefinition::Type::MUSIC:	  return "Music";
			case AudioCollection::AudioDefinition::Type::JINGLE:  return "Jingle";
			default:											  return "Sound";
		}
	}
}


namespace AudioExport
{
	void exportAll(const std::wstring& outputDirectory)
	{
		std::wstring directory = outputDirectory;
		if (!directory.empty() && directory.back() != L'/' && directory.back() != L'\\')
			directory += L'/';
		FTX::FileSystem->createDirectory(directory);

		// Sorted by key, so the manifest is the same from run to run
		std::vector<const AudioCollection::AudioDefinition*> definitions;
		for (const auto& pair : AudioCollection::instance().getAudioDefinitions())
			definitions.push_back(&pair.second);
		std::sort(definitions.begin(), definitions.end(), [](const AudioCollection::AudioDefinition* a, const AudioCollection::AudioDefinition* b) { return a->mKeyString < b->mKeyString; });

		std::string manifest = "{\n";
		bool firstEntry = true;
		int numExported = 0;

		for (const AudioCollection::AudioDefinition* definition : definitions)
		{
			// The original soundtrack's emulated source
			const AudioCollection::SourceRegistration* sourceReg = nullptr;
			for (const AudioCollection::SourceRegistration& source : definition->mSources)
			{
				if (source.mPackage == AudioCollection::Package::ORIGINAL && source.mType != AudioCollection::SourceRegistration::Type::FILE)
				{
					sourceReg = &source;
					break;
				}
			}
			if (nullptr == sourceReg)
				continue;

			// Optional filter for testing: "S3AIR_EXPORT_AUDIO_ONLY=01,02,2C"
			const char* onlyKeys = getenv("S3AIR_EXPORT_AUDIO_ONLY");
			if (nullptr != onlyKeys && onlyKeys[0] != 0 && ("," + std::string(onlyKeys) + ",").find("," + definition->mKeyString + ",") == std::string::npos)
				continue;

			const bool isMusic = (definition->mType == AudioCollection::AudioDefinition::Type::MUSIC);
			const bool isContinuous = (sourceReg->mType == AudioCollection::SourceRegistration::Type::EMULATION_CONTINUOUS) && sourceReg->mEmulationSfxId >= 0xbc;	// (not the spindash sound, which uses the same kind of source)
			// Variants: the normal one, music also with the speed shoes' tempo ("_fast"), and the Special Stage music with each
			// of its speed-up steps ("_fastNN", NN = the tempo value in hex, see AudioPlayer::applyAudioModifierSingle)
			//  -> And the spindash sound with each pitch step ("_revNN"): it rises each time it's played again (see AudioPlayer::playAudioInternal)
			struct Variant { std::string mPostfix; uint8 mTempo = 0; uint8 mSpindashRev = 0; };
			std::vector<Variant> variants = { { "", 0, 0 } };
			if (isMusic)
				variants.push_back({ "_fast", SPEED_SHOES_TEMPO, 0 });
			if (definition->mKeyString == "1C_speedup" || definition->mKeyString == "1C_speedup45")
			{
				for (uint8 tempo : { 0x20, 0x18, 0x10 })
					variants.push_back({ String(0, "_fast%02x", tempo).toStdString(), tempo, 0 });
			}
			const bool isSpindash = (sourceReg->mType == AudioCollection::SourceRegistration::Type::EMULATION_CONTINUOUS && sourceReg->mEmulationSfxId == 0xab);
			if (isSpindash)
			{
				// Where the pitch stops rising: play it again and again, like when charging the spindash, until the driver's counter stays
				uint8 revLimit = 0;
				{
					SoundDriver soundDriver;
					if (sourceReg->mSourceAddress != 0)
						soundDriver.setSourceAddress(sourceReg->mSourceAddress);
					soundDriver.reset();
					for (int k = 0; k < 0x40; ++k)
					{
						const uint8 revBefore = soundDriver.getSpindashRev();
						soundDriver.playSound(sourceReg->mEmulationSfxId);
						for (int frame = 0; frame < 8; ++frame)
							soundDriver.update();
						if (soundDriver.getSpindashRev() == revBefore)
						{
							revLimit = revBefore;
							break;
						}
					}
				}
				RMX_LOG_INFO("Audio export: spindash pitch steps up to " << (int)revLimit);
				for (uint8 rev = 1; rev <= revLimit; ++rev)
					variants.push_back({ String(0, "_rev%02x", rev).toStdString(), 0, rev });
			}
			std::vector<int16> lastSpindashSamples;
			for (size_t variant = 0; variant < variants.size(); ++variant)
			{
				const std::string key = definition->mKeyString + variants[variant].mPostfix;
				RenderResult result;
				// Loop detection only for music and continuous sound effects: jingles and other sounds play until they're over
				//  -> (it found "loops" in jingles before, e.g. the Level Results jingle got cut off before its end)
				if (!render(*sourceReg, isMusic || isContinuous, variants[variant].mTempo, result, isContinuous, variants[variant].mSpindashRev))
				{
					RMX_LOG_INFO("Audio export: could not render " << key);
					continue;
				}
				if (isSpindash)
				{
					// The pitch stops rising at some point: no more variants from there on
					if (result.mSamples == lastSpindashSamples)
						break;
					lastSpindashSamples = result.mSamples;
				}
				if (!isMusic && !isContinuous)
					result.mLoopStart = -1;
				writeWav(directory + String(key).toStdWString() + L".wav", result.mSamples);

				const int numSamples = (int)result.mSamples.size() / 2;
				RMX_LOG_INFO("Audio export: " << key << " (" << getTypeName(definition->mType) << "), " << numSamples << " samples" << ((result.mLoopStart >= 0) ? (", loop from " + std::to_string(result.mLoopStart)) : std::string()));

				const char* sourceName = (sourceReg->mType == AudioCollection::SourceRegistration::Type::EMULATION_CONTINUOUS) ? "EmulationContinuous" :
										 (sourceReg->mType == AudioCollection::SourceRegistration::Type::EMULATION_DIRECT) ? "EmulationDirect" : "Emulation";
				manifest += String(0, "%s\t\"%s\": { \"Name\": \"%s%s\", \"Type\": \"%s\", \"Source\": \"%s\", \"Samples\": \"%d\", \"LoopStart\": \"%d\", \"Channel\": \"%02x\", \"Volume\": \"%.3f\" }",
								   firstEntry ? "" : ",\n", key.c_str(), definition->mDisplayName.c_str(), (variant == 0) ? "" : (" (" + variants[variant].mPostfix.substr(1) + ")").c_str(), getTypeName(definition->mType), sourceName,
								   numSamples, result.mLoopStart, (int)definition->mChannel, sourceReg->mVolume).toStdString();
				firstEntry = false;
				++numExported;
			}
		}

		manifest += "\n}\n";
		FTX::FileSystem->saveFile(directory + L"manifest.json", manifest.c_str(), manifest.length());
		RMX_LOG_INFO("Audio export: " << numExported << " files written to " << *WString(directory).toString());
	}
}
