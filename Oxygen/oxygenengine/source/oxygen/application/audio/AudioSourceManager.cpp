/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/application/audio/AudioSourceManager.h"
#include "oxygen/application/audio/ChipWritesAudioSource.h"
#include "oxygen/application/audio/EmulationAudioSource.h"
#include "oxygen/application/audio/OggAudioSource.h"
#include "oxygen/application/audio/psp/PSPAudioSources.h"


void AudioSourceManager::clear()
{
	// Before destroying the audio sources (incl. audio buffers), make sure no sound is playing any more
	FTX::Audio->removeAllSounds();

	// Now it's safe to destroy all the audio sources
	for (AudioSourceBase* audioSource : mAudioSources)
	{
		audioSource->shutdown();
		delete audioSource;
	}
	mAudioSources.clear();
	mMappedAudioSourcesByHash.clear();
#if defined(PLATFORM_PSP)
	mPSPFreedSinceLower.clear();
	mPSPToRestore.clear();
#endif
}

AudioSourceBase* AudioSourceManager::getAudioSourceForPlayback(SourceRegistration& sourceRegistration)
{
	// Is the audio source pointer already cached, then just use it
	if (nullptr != sourceRegistration.mAudioSource)
	{
		return sourceRegistration.mAudioSource;
	}

	// Determine a hash
	uint64 hash = 0;
	if (sourceRegistration.mType == SourceRegistration::Type::FILE)
	{
		hash = rmx::getMurmur2_64("OggFile:" + rmx::convertToUTF8(sourceRegistration.mSourceFile));
	}
	else if (!sourceRegistration.mSourceFile.empty())
	{
		hash = rmx::getMurmur2_64("SMPSFile:" + rmx::convertToUTF8(sourceRegistration.mSourceFile));
	}
	else if (sourceRegistration.mSourceAddress != 0)
	{
		hash = rmx::getMurmur2_64(String(0, "EmulatedSource:%08x", sourceRegistration.mSourceAddress));
	}
	else
	{
		hash = rmx::getMurmur2_64(String(0, "EmulatedKey:%016llx", sourceRegistration.mAudioDefinition->mAllKeyIds.back()));	// Usually the numeric key, at least if there is one
	}

	AudioSourceBase* audioSource = nullptr;

	// Is there an entry already with that hash?
	//  -> By checking this, we can easily re-use already loaded audio sources after changes to the audio collection, e.g. after a full mods reload
	const auto it = mMappedAudioSourcesByHash.find(hash);
	if (it != mMappedAudioSourcesByHash.end())
	{
		// Found it
		audioSource = it->second;
	}
	else
	{
		// Create a new audio source
		if (sourceRegistration.mType == SourceRegistration::Type::FILE)
		{
			if (rmx::endsWith(sourceRegistration.mSourceFile, L".chipw"))
			{
				audioSource = addChipWritesAudioSource(sourceRegistration.mSourceFile, false);
			}
		#if defined(PLATFORM_PSP)
			else if (rmx::endsWith(sourceRegistration.mSourceFile, L".at3"))
			{
				PSPAtrac3AudioSource* pspSource = new PSPAtrac3AudioSource(sourceRegistration.mIsLooping, sourceRegistration.mLoopStart, (int)sourceRegistration.mLoopEnd);
				mAudioSources.push_back(pspSource);
				pspSource->load(sourceRegistration.mSourceFile);
				audioSource = pspSource;
			}
			else if (rmx::endsWith(sourceRegistration.mSourceFile, L".wav"))
			{
				PSPWavAudioSource* pspSource = new PSPWavAudioSource(sourceRegistration.mContinuousLoop ? (int)sourceRegistration.mLoopStart : -1);
				mAudioSources.push_back(pspSource);
				pspSource->load(sourceRegistration.mSourceFile);
				audioSource = pspSource;
			}
		#endif
			else
			{
				const bool useCaching = !String(sourceRegistration.mAudioDefinition->mKeyString).endsWith("_fast");
				audioSource = addOggAudioSource(sourceRegistration.mSourceFile, useCaching, sourceRegistration.mIsLooping, sourceRegistration.mLoopStart);
			}
		}
		else
		{
			const AudioSourceBase::CachingType cachingType = (sourceRegistration.mType == SourceRegistration::Type::EMULATION_BUFFERED) ? AudioSourceBase::CachingType::STREAMING_STATIC :
															 (sourceRegistration.mType == SourceRegistration::Type::EMULATION_DIRECT) ? AudioSourceBase::CachingType::STREAMING_DYNAMIC : AudioSourceBase::CachingType::FULL_DYNAMIC;
			audioSource = addEmulationAudioSource(sourceRegistration.mEmulationSfxId, cachingType, sourceRegistration.mSourceFile, sourceRegistration.mSourceAddress, sourceRegistration.mContentOffset);
		}

		mMappedAudioSourcesByHash[hash] = audioSource;
	}

	sourceRegistration.mAudioSource = audioSource;
	return audioSource;
}

void AudioSourceManager::updateStreaming(float currentTime)
{
#if defined(PLATFORM_PSP)
	trimPSPSounds(currentTime);

	// Sounds unloaded for a special stage, needed again back in the level: loaded again one per frame (each takes 3..20 ms)
	// while the level fades in, instead of each when it gets played first, during the game
	while (!mPSPToRestore.empty())
	{
		AudioSourceBase* audioSource = mPSPToRestore.back();
		mPSPToRestore.pop_back();
		if (audioSource->getMemoryUsage() > 0 || audioSource->isCompletelyLoaded())
			continue;	// (loaded again already)
		if (getMemoryUsage() + 192 * 1024 > mPSPSoundBudget)
		{
			mPSPToRestore.clear();
			break;
		}
		audioSource->startup();
		audioSource->setLastUsedTimestamp(currentTime);
		break;
	}
#endif

	for (AudioSourceBase* audioSource : mAudioSources)
	{
		// Check if this audio source should get unloaded now
		if (!audioSource->checkForUnload(currentTime))
		{
			// Otherwise update streaming
			if (audioSource->isStreaming())
			{
				const float precacheTime = audioSource->needsMinimalLag() ? 0.1f : 0.25f;
				audioSource->progress(audioSource->getReadTime() + precacheTime);
			}
		}
	}
}

#if defined(PLATFORM_PSP)
void AudioSourceManager::trimPSPSounds(float currentTime)
{
	// PSP: the sound effects loaded add up (the special stages then lack memory), so they're kept within a budget. When it's
	// exceeded, the ones used longest ago get unloaded, only as many as needed: loading a sound again from the memory stick
	// takes up to ~70 ms, a frame drop each time, so the ones played again and again (and the big ones are the slowest to
	// load) should stay. Music (ATRAC3, dynamic) counts too, but it gets unloaded on its own (see checkForUnload).
	//  -> The budget is bigger in levels, where there's memory left (see Game::updatePSPSoundBudget)
	const size_t BUDGET = mPSPSoundBudget;
	size_t memoryUsage = getMemoryUsage();
	if (memoryUsage <= BUDGET)
		return;

	static std::vector<AudioSourceBase*> candidates;
	candidates.clear();
	for (AudioSourceBase* audioSource : mAudioSources)
	{
		// Not the ones playing right now (their timestamp gets updated in each frame) or just played
		if (!audioSource->isDynamic() && audioSource->getMemoryUsage() > 0 && currentTime - audioSource->getLastUsedTimestamp() > 1.0f)
			candidates.push_back(audioSource);
	}
	std::sort(candidates.begin(), candidates.end(), [](const AudioSourceBase* a, const AudioSourceBase* b) { return a->getLastUsedTimestamp() < b->getLastUsedTimestamp(); });

	for (AudioSourceBase* audioSource : candidates)
	{
		if (memoryUsage <= BUDGET)
			break;
		const size_t bytes = audioSource->getMemoryUsage();
		if (mPSPBudgetLowered && currentTime - audioSource->getLastUsedTimestamp() < 120.0f)
			mPSPFreedSinceLower.push_back(audioSource);		// (to load it again when the budget goes up again, if it was used not long ago)
		audioSource->unload();
		memoryUsage -= std::min(memoryUsage, bytes);
	}
}
#endif

#if defined(PLATFORM_PSP)
void AudioSourceManager::setPSPSoundBudget(size_t bytes, float currentTime)
{
	if (bytes == mPSPSoundBudget)
		return;
	const bool lower = (bytes < mPSPSoundBudget);
	mPSPSoundBudget = bytes;
	mPSPBudgetLowered = lower;
	mPSPToRestore.clear();
	if (lower)
	{
		mPSPFreedSinceLower.clear();
		trimPSPSounds(currentTime);
	}
	else
	{
		// Load again what got unloaded since then, the most recently used first (the last ones unloaded)
		mPSPToRestore.swap(mPSPFreedSinceLower);
		mPSPFreedSinceLower.clear();
	}
}
#endif

size_t AudioSourceManager::getMemoryUsage() const
{
	size_t memoryUsage = 0;
	for (const AudioSourceBase* audioSource : mAudioSources)
	{
		memoryUsage += audioSource->getMemoryUsage();
	}
	return memoryUsage;
}

std::string AudioSourceManager::getMemoryReport(size_t count) const
{
	std::vector<std::pair<size_t, const AudioSourceBase*>> sources;
	for (const AudioSourceBase* audioSource : mAudioSources)
	{
		if (audioSource->getMemoryUsage() > 0)
			sources.emplace_back(audioSource->getMemoryUsage(), audioSource);
	}
	std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::string result;
	for (size_t i = 0; i < std::min(count, sources.size()); ++i)
	{
		result += " " + sources[i].second->getAudioBufferName() + "=" + std::to_string(sources[i].first / 1024) + "KB";
	}
	return result;
}

AudioSourceBase* AudioSourceManager::addChipWritesAudioSource(std::wstring_view filename, bool useCaching)
{
	// Register audio source
	ChipWritesAudioSource* audioSource = new ChipWritesAudioSource(useCaching);
	mAudioSources.push_back(audioSource);

	// Load content
	audioSource->load(filename);
	return audioSource;
}

AudioSourceBase* AudioSourceManager::addEmulationAudioSource(uint8 soundId, AudioSourceBase::CachingType cachingType, const std::wstring& filename, uint32 sourceAddress, uint32 contentOffset)
{
	// Register audio source
	EmulationAudioSource* audioSource = new EmulationAudioSource(cachingType);
	mAudioSources.push_back(audioSource);

	// Initialize and load content from file if needed
	if (!filename.empty())
	{
		audioSource->initWithCustomContent(soundId, filename, contentOffset);
	}
	else if (sourceAddress != 0)
	{
		audioSource->initWithCustomAddress(soundId, sourceAddress);
	}
	else
	{
		audioSource->initWithSfxId(soundId);
	}
	return audioSource;
}

AudioSourceBase* AudioSourceManager::addOggAudioSource(const std::wstring& filename, bool useCaching, bool isLooping, int loopStart)
{
	// Register audio source
	OggAudioSource* audioSource = new OggAudioSource(useCaching, isLooping, loopStart);
	mAudioSources.push_back(audioSource);

	// Load content
	audioSource->load(filename);
	return audioSource;
}
