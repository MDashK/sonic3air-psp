/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/application/audio/AudioCollection.h"
#include "oxygen/application/audio/AudioSourceBase.h"


class AudioSourceManager
{
public:
	using SourceRegistration = AudioCollection::SourceRegistration;

public:
	void clear();

	AudioSourceBase* getAudioSourceForPlayback(SourceRegistration& sourceRegistration);

	void updateStreaming(float currentTime);

	size_t getMemoryUsage() const;
#if defined(PLATFORM_PSP)
	void setPSPSoundBudget(size_t bytes, float currentTime);	// (unloads sounds right away if it's lower than before)
#endif
	std::string getMemoryReport(size_t count) const;	// The audio sources using most memory (for benchmarks)

private:
	AudioSourceBase* addChipWritesAudioSource(std::wstring_view filename, bool useCaching = true);
	AudioSourceBase* addEmulationAudioSource(uint8 soundId, AudioSourceBase::CachingType cachingType, const std::wstring& filename = L"", uint32 sourceAddress = 0, uint32 contentOffset = 0);
	AudioSourceBase* addOggAudioSource(const std::wstring& filename, bool useCaching = true, bool isLooping = false, int loopStart = -1);
#if defined(PLATFORM_PSP)
	void trimPSPSounds(float currentTime);
#endif

private:
	std::vector<AudioSourceBase*> mAudioSources;
	std::map<uint64, AudioSourceBase*> mMappedAudioSourcesByHash;
#if defined(PLATFORM_PSP)
	size_t mPSPSoundBudget = 1536 * 1024;
	bool mPSPBudgetLowered = false;						// The budget went down (for a special stage), not up again yet
	std::vector<AudioSourceBase*> mPSPFreedSinceLower;	// Sounds unloaded since the budget went down (in the order unloaded)
	std::vector<AudioSourceBase*> mPSPToRestore;		// Those to load again now that it's up again, one per frame (the last first)
#endif
};
