/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/application/audio/AudioSourceBase.h"
#if defined(PLATFORM_PSP)
	#include "oxygen/application/audio/AudioOutBase.h"
	#include "oxygen/application/audio/AudioPlayer.h"
	#include "oxygen/engine/EngineMain.h"
#endif


AudioSourceBase::AudioSourceBase(AudioSourceType type, CachingType cachingType) :
	mAudioSourceType(type),
	mCachingType(cachingType)
{
	mMutex = SDL_CreateMutex();
}

AudioSourceBase::~AudioSourceBase()
{
	RMX_ASSERT(!isJobRegistered(), "Job still registered, even though it should have been removed in 'shutdown'");
	SDL_DestroyMutex(mMutex);

	mAudioBuffer.lock();
	mAudioBuffer.clear();
	mAudioBuffer.unlock();
}

AudioBuffer* AudioSourceBase::startup()
{
	if (mState == State::INACTIVE || isDynamic())
	{
		// Initial startup or reset from scratch
		mState = startupInternal();
		mReadTime = 0.0f;
	}
	return &mAudioBuffer;
}

void AudioSourceBase::shutdown()
{
	if (isJobRegistered())
	{
		FTX::JobManager->removeJob(*this);
	}
}

void AudioSourceBase::progress(float precacheTime)
{
	if (mState == State::STREAMING)
	{
		progressInternal(precacheTime);
	}
}

bool AudioSourceBase::checkForUnload(float timestamp)
{
	bool mayUnload = false;

#if defined(PLATFORM_VITA)
	// PSVITA has limited RAM, so...
	if (((float)EngineMain::instance().getAudioOut().getAudioPlayer().getMemoryUsage() / 1048576.0f) >= 80.0f) // 80 MB
	{
		// Let's make an emergency forced unload since the buffer is getting too big
		mayUnload = (timestamp - mLastUsedTimestamp > 10.0f); // Everything not used in the past 10 seconds
	}
	if (EngineMain::instance().getAudioOut().getAudioPlayer().getNumPlayingSounds() == 0) // No sound playing
	{
		// Since it's silenced, lets take the chance to unload stuff
		mayUnload = (timestamp - mLastUsedTimestamp > 30.0f); // Everything not used in the past 30 seconds
	}
#endif
#if defined(PLATFORM_PSP)
	// PSP: sound effects don't get unloaded here, but by AudioSourceManager::trimPSPSounds, the least recently used first
#endif

	if (isDynamic())
	{
		// Ignore tracks not loaded
		if (mAudioBuffer.getLengthInSec() > 0.2f)
		{
			// Unload after a few seconds already
			mayUnload = (timestamp - mLastUsedTimestamp > 10.0f);
		}
	#if defined(PLATFORM_PSP)
		// PSP: also when nothing is decoded right now (ATRAC3 music keeps a decoder and its 64 KB buffer while it's started)
		else if (mState != State::INACTIVE && timestamp - mLastUsedTimestamp > 10.0f)
		{
			mayUnload = true;
		}
	#endif
	}
	else
	{
		// Ignore short sounds, and tracks not loaded
		if (mAudioBuffer.getLengthInSec() > 5.0f)
		{
			// Unload after 3 minutes
		#if !defined(PLATFORM_VITA)
			mayUnload = (timestamp - mLastUsedTimestamp > 180.0f);
		#else
			// PSVITA has limited RAM, so...
			mayUnload = (timestamp - mLastUsedTimestamp > 60.0f); // 60 seconds and unload
		#endif
		}
	}

	if (mayUnload)
	{
		unload();
		return true;
	}
	return false;
}

void AudioSourceBase::unload()
{
	if (isJobRegistered())
	{
		FTX::JobManager->removeJob(*this);
	}

	SDL_LockMutex(mMutex);
	mAudioBuffer.lock();
	mAudioBuffer.clear();
	mAudioBuffer.unlock();
	mState = State::INACTIVE;
	mReadTime = 0.0f;

	resetInternal();

	SDL_UnlockMutex(mMutex);
}
