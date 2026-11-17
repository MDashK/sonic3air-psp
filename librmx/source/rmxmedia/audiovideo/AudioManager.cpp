/*
*	rmx Library
*	Copyright (C) 2008-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "rmxmedia.h"

#include <chrono>


namespace rmx
{

	AudioManager::AudioManager() :
		mRootMixer(*new AudioMixer("Root", 0))	// Root mixer always uses ID 0
	{
		mAudioMixers[0] = &mRootMixer;
	}

	AudioManager::~AudioManager()
	{
	#ifndef RMX_USE_SDL3
		RMX_ASSERT(mAudioLocks == 0, "Pending audio locks");
	#endif

		// Delete all audio mixers (including the root mixer)
		for (const auto& [key, audioMixer] : mAudioMixers)
		{
			delete audioMixer;
		}
	}

	void AudioManager::initialize(int sampleFrequency, int numChannels, int audioBufferSamples)
	{
		if (!FTX::System->initialize())
			return;

		// Initialize SDL2 audio subsystem
		SDL_InitSubSystem(SDL_INIT_AUDIO);

		// Reset instances
		mInstances.clear();
		mRootMixer.clearAudioInstances();

	#ifdef RMX_USE_SDL3
		// Destroy old audio stream, if there was one already
		if (nullptr != mAudioStream)
		{
			SDL_DestroyAudioStream(mAudioStream);
			mAudioStream = nullptr;
		}
	#endif

		// Check input, we don't support everything
		if ((sampleFrequency % 11025) != 0)
		{
			if (sampleFrequency != 48000)
			{
				// It might be supported anyway, you'd have to try
				RMX_ASSERT(false, "Unsupported sample frequency: " << sampleFrequency << " Hz");
				int sf = sampleFrequency / 11025;
				sf = (sf == 3) ? 2 : clamp(sf, 1, 4);	// Result is 1, 2 or 4
				sampleFrequency = sf * 11025;
			}
		}
		numChannels = clamp(numChannels, 1, 2);

		// Define format
		mFormat.channels = numChannels;
		mFormat.freq = sampleFrequency;

	#ifdef RMX_USE_SDL3
		mFormat.format = SDL_AUDIO_S16;
		mAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &mFormat, AudioManager::mixAudioStatic, nullptr);
		if (nullptr == mAudioStream)
		{
			RMX_ERROR("SDL_OpenAudioDeviceStream failed with error: '" << SDL_GetError() << "'", );
			return;
		}

		const SDL_AudioDeviceID audioDeviceID = SDL_GetAudioStreamDevice(mAudioStream);
		if (audioDeviceID == 0)
		{
			RMX_ERROR("SDL_GetAudioStreamDevice failed with error: '" << SDL_GetError() << "'", );
			return;
		}
		SDL_GetAudioDeviceFormat(audioDeviceID, &mFormat, &mOutputBufferSize);

	#else
		mFormat.format = AUDIO_S16LSB;
		mFormat.samples = audioBufferSamples;
		mFormat.callback = AudioManager::mixAudioStatic;
		mFormat.userdata = 0;
		mOutputBufferSize = mFormat.samples;

		// Open audio device
		SDL_AudioSpec requested = mFormat;
		mAudioDeviceID = SDL_OpenAudioDevice(nullptr, 0, &requested, &mFormat, 0);
		if (mAudioDeviceID == 0)
		{
			const int numAudioDevices = SDL_GetNumAudioDevices(0);
			RMX_CHECK(numAudioDevices >= 0, "SDL_GetNumAudioDevices failed to determine audio devices", );
			RMX_CHECK(numAudioDevices != 0, "SDL_GetNumAudioDevices could not find any audio devices", );

			std::string text;
			for (int i = 0; i < numAudioDevices; ++i)
			{
				if (!text.empty())
					text += ", ";
				text += SDL_GetAudioDeviceName(i, 0);
			}
			RMX_ERROR("SDL_OpenAudioDevice failed with error: '" << SDL_GetError() << "' (found " << numAudioDevices << " audio devices: " << text << ")", );
			return;
		}
	#endif

#if defined(PLATFORM_PSP)
		audiocapture::isEnabled();	// Set up here, before the audio thread uses it
#endif
		// Everything alright so far
		mPlayedSamples = 0;
		playAudio(true);
	}

	void AudioManager::exit()
	{
	#ifdef RMX_USE_SDL3
		// Destroy audio stream
		if (nullptr != mAudioStream)
		{
			SDL_DestroyAudioStream(mAudioStream);
			mAudioStream = nullptr;
		}
	#else
		SDL_CloseAudioDevice(mAudioDeviceID);
	#endif
	}

	void AudioManager::clear()
	{
		if (!mInstances.empty())
		{
			// The audio thread goes through "mInstances" after each mix (see "mixAudio"): so it may only change while the audio is locked,
			// else an instance can get freed while a mixer still has it, or the map's tree is read while it changes
			lockAudio();
			for (const auto& [key, audioMixer] : mAudioMixers)
			{
				audioMixer->clearAudioInstances();
			}
			mInstances.clear();
			mRemoveIDs.clear();
			unlockAudio();
			++mChangeCounter;
		}
	}

	void AudioManager::playAudio(bool onoff)
	{
	#ifdef RMX_USE_SDL3
		if (nullptr != mAudioStream)
		{
			const SDL_AudioDeviceID audioDeviceId = SDL_GetAudioStreamDevice(mAudioStream);
			if (audioDeviceId != 0)
			{
				if (onoff)
					SDL_ResumeAudioDevice(audioDeviceId);
				else
					SDL_PauseAudioDevice(audioDeviceId);
			}
		}
	#else
		SDL_PauseAudioDevice(mAudioDeviceID, onoff ? 0 : 1);
	#endif
	}

	bool AudioManager::getAudioState()
	{
	#ifdef RMX_USE_SDL3
		if (nullptr != mAudioStream)
		{
			const SDL_AudioDeviceID audioDeviceId = SDL_GetAudioStreamDevice(mAudioStream);
			if (audioDeviceId != 0)
			{
				return SDL_AudioDevicePaused(audioDeviceId);
			}
		}
		return false;
	#else
		return (SDL_GetAudioStatus() == SDL_AUDIO_PLAYING);
	#endif
	}

	void AudioManager::lockAudio()
	{
		// Not required any more in SDL3, function can be removed
	#ifndef RMX_USE_SDL3
		if (mAudioLocks == 0)
		{
			SDL_LockAudioDevice(mAudioDeviceID);
		}
		++mAudioLocks;
	#endif
	}

	void AudioManager::unlockAudio()
	{
		// Not required any more in SDL3, function can be removed
	#ifndef RMX_USE_SDL3
		RMX_ASSERT(mAudioLocks > 0, "Called 'AudioManager::unlockAudio' without locking");
		--mAudioLocks;
		if (mAudioLocks == 0)
		{
			SDL_UnlockAudioDevice(mAudioDeviceID);
		}
	#endif
	}

	void AudioManager::regularUpdate(float deltaSeconds)
	{
		// Do the following cleanup only every 0.5 seconds
		mTimeSinceLastUpdate += deltaSeconds;
		if (mTimeSinceLastUpdate >= 0.5f)
		{
			mTimeSinceLastUpdate = 0.0f;
			lockAudio();

			// Collect non-persistent audio buffers currently played back, and the earliest playback positions
			static std::vector<std::pair<AudioBuffer*, int>> audioBufferPurgePositions;
			audioBufferPurgePositions.clear();
			for (const auto& instancePair : mInstances)
			{
				const AudioInstance& instance = instancePair.second;
				AudioBuffer* audioBuffer = instance.mAudioBuffer;
			#if defined(PLATFORM_PSP)
				// PSP: not for paused instances. Music paused while another playback of the same track goes on (e.g. paused
				// for a jingle, then started again) would otherwise keep everything decoded since then, growing until the memory
				// runs out. A paused instance that resumes after its samples got purged just ends (see AudioMixer).
				if (instance.mPaused)
					continue;
			#endif
				if (nullptr != audioBuffer && !audioBuffer->isPersistent())
				{
					// Already had this one before?
					bool found = false;
					for (auto& bufferPair : audioBufferPurgePositions)
					{
						if (bufferPair.first == audioBuffer)
						{
							// Update the playback position
							bufferPair.second = std::min(bufferPair.second, instance.mPosition);
							found = true;
							break;
						}
					}

					if (!found)
					{
						audioBufferPurgePositions.emplace_back(audioBuffer, instance.mPosition);
					}
				}
			}

			// Now update the audio buffers
			for (auto& bufferPair : audioBufferPurgePositions)
			{
				AudioBuffer* audioBuffer = bufferPair.first;
				audioBuffer->lock();
				audioBuffer->markPurgeableSamples(bufferPair.second);
				audioBuffer->unlock();
			}

			unlockAudio();
		}
	}

	void AudioManager::setGlobalVolume(float volume)
	{
		mRootMixer.setVolume(volume);
	}

	AudioMixer* AudioManager::getAudioMixerByID(int mixerId) const
	{
		const auto it = mAudioMixers.find(mixerId);
		return (it == mAudioMixers.end()) ? nullptr : it->second;
	}

	void AudioManager::deleteAudioMixerByID(int mixerId)
	{
		RMX_ASSERT(mixerId != 0, "Can't delete root audio mixer (with ID 0)");
		mAudioMixers.erase(mixerId);
	}

	float AudioManager::getAudioMixerVolumeByID(int mixerId) const
	{
		const AudioMixer* audioMixer = getAudioMixerByID(mixerId);
		return (nullptr != audioMixer) ? audioMixer->getVolume() : 0.0f;
	}

	void AudioManager::setAudioMixerVolumeByID(int mixerId, float relativeVolume)
	{
		AudioMixer* audioMixer = getAudioMixerByID(mixerId);
		if (nullptr != audioMixer)
		{
			audioMixer->setVolume(relativeVolume);
		}
	}

	bool AudioManager::addSound(const PlaybackOptions& playbackOptions, AudioReference& ref)
	{
		if (nullptr == playbackOptions.mAudioBuffer)
			return false;

		AudioMixer* audioMixer = getAudioMixerByID(playbackOptions.mAudioMixerId);
		if (nullptr == audioMixer)
			return false;

		// The audio thread goes through "mInstances" after each mix (see "mixAudio"): so it may only change while the audio is locked,
		// else an instance can get freed while a mixer still has it, or the map's tree is read while it changes
		lockAudio();
		AudioInstance& instance = mInstances[mNextFreeID];
		instance.mID = mNextFreeID;
		instance.mAudioBuffer = playbackOptions.mAudioBuffer;
		instance.mAudioMixer = audioMixer;
		instance.mVolume = playbackOptions.mVolume;
		instance.mVolumeChange = playbackOptions.mVolumeChange;
		instance.mSpeed = playbackOptions.mSpeed;
		instance.mPosition = roundToInt(playbackOptions.mPosition * (float)playbackOptions.mAudioBuffer->getFrequency());
		instance.mLoop = playbackOptions.mLoop;
		instance.mStreaming = playbackOptions.mStreaming;
		instance.mPaused = playbackOptions.mStartPaused;

		audioMixer->addAudioInstance(instance);
		unlockAudio();

		++mChangeCounter;
		++mNextFreeID;

		ref.setInstanceID(instance.mID);
		return (instance.mID != 0);
	}

	int AudioManager::addSound(AudioBuffer* audiobuffer, bool streaming)
	{
		PlaybackOptions playbackOptions;
		playbackOptions.mAudioBuffer = audiobuffer;
		playbackOptions.mStreaming = streaming;

		AudioReference ref;
		addSound(playbackOptions, ref);
		return ref.getInstanceID();
	}

	bool AudioManager::addSound(AudioBuffer* audiobuffer, AudioReference& ref, bool streaming)
	{
		PlaybackOptions playbackOptions;
		playbackOptions.mAudioBuffer = audiobuffer;
		playbackOptions.mStreaming = streaming;
		return addSound(playbackOptions, ref);
	}

	void AudioManager::removeSound(AudioReference& ref)
	{
		if (ref.isValid())
		{
			removeInstance(ref.getInstanceID());
		}
	}

	void AudioManager::removeAllSounds()
	{
		// This function is just a synonym for clear
		clear();
	}

	AudioManager::AudioInstance* AudioManager::findInstance(int ID)
	{
		if (ID <= 0 || ID >= mNextFreeID)
			return nullptr;

		// This seems like a good place to do some cleanup if needed
		processRemoveIDs();

		const auto it = mInstances.find(ID);
		if (it == mInstances.end())
			return nullptr;

		return &it->second;
	}

	void AudioManager::removeInstance(int ID)
	{
		assert(ID > 0 && ID < mNextFreeID);
		const auto it = mInstances.find(ID);
		if (it != mInstances.end())
		{
			// The audio thread goes through "mInstances" after each mix (see "mixAudio"): so it may only change while the audio is locked,
			// else an instance can get freed while a mixer still has it, or the map's tree is read while it changes
			lockAudio();
			AudioInstance& audioInstance = it->second;
			if (nullptr != audioInstance.mAudioMixer)
			{
				audioInstance.mAudioMixer->removeAudioInstance(audioInstance);
			}
			mInstances.erase(it);
			unlockAudio();
			++mChangeCounter;
		}
	}

	void AudioManager::processRemoveIDs()
	{
		if (!mRemoveIDs.empty())
		{
			lockAudio();
			for (int ID : mRemoveIDs)
			{
				const auto it = mInstances.find(ID);
				if (it != mInstances.end())
				{
					AudioInstance& audioInstance = it->second;
					if (nullptr != audioInstance.mAudioMixer)
					{
						audioInstance.mAudioMixer->removeAudioInstance(audioInstance);
					}
				}
			}
			// (in the same lock: see "addSound" - and the audio thread adds to "mRemoveIDs" while mixing)
			for (int ID : mRemoveIDs)
			{
				mInstances.erase(ID);
			}
			mRemoveIDs.clear();
			unlockAudio();
			++mChangeCounter;
		}
	}

	void AudioManager::registerAudioMixer(AudioMixer& audioMixer, int parentMixerId)
	{
		// Is there another audio mixer with the same ID already?
		const auto it = mAudioMixers.find(audioMixer.mMixerId);
		if (it != mAudioMixers.end() && it->second != &audioMixer)
		{
			// Replace the old mixer in the hierarchy
			AudioMixer* oldMixer = it->second;

			std::swap(audioMixer.mParent, oldMixer->mParent);
			for (AudioMixer*& child : audioMixer.mParent->mChildren)
			{
				if (child == oldMixer)
					child = &audioMixer;
			}

			std::swap(audioMixer.mChildren, oldMixer->mChildren);
			for (AudioMixer* child : audioMixer.mChildren)
			{
				child->mParent = &audioMixer;
			}

			delete oldMixer;
		}

		// Register under its mixer ID
		mAudioMixers[audioMixer.mMixerId] = &audioMixer;

		// Register at (new) parent
		AudioMixer* parent = getAudioMixerByID(parentMixerId);
		if (nullptr == parent)
			parent = &mRootMixer;
		parent->addChild(audioMixer);
	}

#ifdef RMX_USE_SDL3
	void AudioManager::mixAudioStatic(void* userdata, SDL_AudioStream* audioStream, int additionalAmount, int totalAmount)
	{
		// TODO: This is just simple migration from SDL2 to SDL3
		//  -> Better avoid the stack allow and directly write into the stream
		if (additionalAmount > 0)
		{
			uint8* data = SDL_stack_alloc(uint8, additionalAmount);
			if (nullptr != data)
			{
				FTX::Audio->mixAudio(data, additionalAmount);
				SDL_PutAudioStreamData(audioStream, data, additionalAmount);
				SDL_stack_free(data);
			}
		}
	}
#else
	void AudioManager::mixAudioStatic(void* _userdata, uint8* outputStream, int outputBytes)
	{
		FTX::Audio->mixAudio(outputStream, outputBytes);
	#if defined(PLATFORM_PSP)
		if (audiocapture::isEnabled() && FTX::Audio->getOutputChannels() == 2)
			audiocapture::feed((const int16*)outputStream, outputBytes / 4);
	#endif
	}
#endif

#if defined(PLATFORM_PSP)
	namespace audiocapture
	{
		static const constexpr int CAPTURE_RATE = 22050;
		static const constexpr int CAPTURE_SAMPLES = CAPTURE_RATE * 24;		// The last 24 seconds (~1 MB)
		static int16* gCapture = nullptr;
		static volatile uint32 gCaptureWritten = 0;		// Samples written in total (the position in the ring buffer is this modulo its size)
		static int gEnabled = -1;

		bool isEnabled()
		{
			if (gEnabled < 0)
			{
				const char* value = getenv("S3AIR_AUDIO_CAPTURE");
				gEnabled = (nullptr != value && value[0] == '1') ? 1 : 0;
				if (gEnabled)
				{
					gCapture = (int16*)calloc(CAPTURE_SAMPLES, sizeof(int16));
					if (nullptr == gCapture)
						gEnabled = 0;
				}
			}
			return gEnabled == 1;
		}

		void feed(const int16* stereoSamples, int numFrames)
		{
			// Two stereo frames at 44100 Hz become one mono sample at 22050 Hz
			uint32 written = gCaptureWritten;
			for (int i = 0; i + 1 < numFrames; i += 2)
			{
				const int sum = stereoSamples[i * 2] + stereoSamples[i * 2 + 1] + stereoSamples[i * 2 + 2] + stereoSamples[i * 2 + 3];
				gCapture[written % CAPTURE_SAMPLES] = (int16)(sum / 4);
				++written;
			}
			gCaptureWritten = written;
		}

		bool save(const char* filename)
		{
			if (!isEnabled())
				return false;
			FILE* file = fopen(filename, "wb");
			if (nullptr == file)
				return false;

			const uint32 written = gCaptureWritten;
			const uint32 count = std::min<uint32>(written, CAPTURE_SAMPLES);
			const uint32 dataBytes = count * 2;
			struct
			{
				char mRiff[4] = { 'R', 'I', 'F', 'F' };  uint32 mRiffSize;  char mWave[4] = { 'W', 'A', 'V', 'E' };
				char mFmt[4] = { 'f', 'm', 't', ' ' };   uint32 mFmtSize = 16;  uint16 mFormat = 1;  uint16 mChannels = 1;
				uint32 mRate = CAPTURE_RATE;  uint32 mByteRate = CAPTURE_RATE * 2;  uint16 mBlockAlign = 2;  uint16 mBits = 16;
				char mData[4] = { 'd', 'a', 't', 'a' };  uint32 mDataSize;
			} header;
			header.mRiffSize = 36 + dataBytes;
			header.mDataSize = dataBytes;
			fwrite(&header, sizeof(header), 1, file);

			// Oldest first: from the current ring buffer position to its end, then from its start
			const uint32 start = (written - count) % CAPTURE_SAMPLES;
			const uint32 firstPart = std::min<uint32>(count, CAPTURE_SAMPLES - start);
			fwrite(gCapture + start, 2, firstPart, file);
			if (count > firstPart)
				fwrite(gCapture, 2, count - firstPart, file);
			fclose(file);
			return true;
		}
	}
#endif

	void AudioManager::mixAudio(uint8* outputStream, int outputBytes)
	{
		const auto statStart = std::chrono::high_resolution_clock::now();
		struct StatScope
		{
			std::chrono::high_resolution_clock::time_point mStart;
			~StatScope() { mStatMixMs += std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - mStart).count(); }
		} statScope { statStart };

		const size_t outputSamples = outputBytes / (getOutputChannels() * sizeof(short));
		const constexpr size_t MAX_SAMPLES = 2048;

		RMX_ASSERT(outputSamples <= MAX_SAMPLES, "Mixing more than " << MAX_SAMPLES << " samples at once is not supported");
		RMX_ASSERT(getOutputChannels() <= 2, "More than 2 channels is not supported");

		// Setup intermediate buffer
		static int32 fullOutputBuffer[MAX_SAMPLES * 2];
		memset(fullOutputBuffer, 0, sizeof(fullOutputBuffer));

		AudioMixer::MixerParameters parameters;
		parameters.mOutputBuffers[0] = &fullOutputBuffer[0];
		parameters.mOutputBuffers[1] = &fullOutputBuffer[outputSamples];
		parameters.mOutputSamples = outputSamples;
		parameters.mOutputFormat = &mFormat;
		parameters.mAccumulatedVolume = 1.0f;
		mRootMixer.performAudioMix(parameters);

		// Copy results into the output stream
		for (int channelIndex = 0; channelIndex < mFormat.channels; ++channelIndex)
		{
			const int32* RESTRICT src = parameters.mOutputBuffers[channelIndex];
			short* dst = ((short*)outputStream) + channelIndex;

			for (size_t i = 0; i < outputSamples; ++i)
			{
				if (*src >= 0x800000)
				{
					*dst = 0x7fff;
				}
				else if (*src < -0x800000)
				{
					*dst = -0x8000;
				}
				else
				{
					*dst = (*src >> 8);
				}
				++src;
				dst += 2;
			}
		}

		mPlayedSamples += (uint32)outputSamples;
		mStatMixSamples += (uint32)outputSamples;

		// Remove instance that are done playing
		for (auto& [key, audioInstance] : mInstances)
		{
			if (audioInstance.mPlaybackDone)
			{
				// Add to remove IDs list, but only once please
				if (!containsElement(mRemoveIDs, key))
				{
					mRemoveIDs.push_back(key);
				}
				++mChangeCounter;
			}
		}
	}



	bool WavLoader::load(AudioBuffer& outBuffer, const String& source, const String& params)
	{
		// Load WAV file
		if (source.empty())
			return false;
		if (!source.endsWith(".wav"))
			return false;

		// Just use SDL for this
		SDL_AudioSpec waveFormat;
		unsigned char* waveData = nullptr;
		Uint32 waveLength = 0;
		if (!SDL_LoadWAV(*source, &waveFormat, &waveData, &waveLength))
			return false;

		// Rebuild audio buffer
		SDL_AudioSpec targetFormat;
		targetFormat.freq = 44100;
		targetFormat.channels = 2;
		outBuffer.clear(targetFormat.freq, targetFormat.channels);

		// Convert format
	#ifdef RMX_USE_SDL3
		targetFormat.format = SDL_AUDIO_S16LE;
		uint8* outputBuffer = nullptr;
		int outputLength = 0;
		if (!SDL_ConvertAudioSamples(&waveFormat, waveData, waveLength, &targetFormat, &outputBuffer, &outputLength))
			return false;

		// Write data
		const short* data = (short*)outputBuffer;
		const int length = outputLength / (targetFormat.channels * sizeof(short));
	#else
		SDL_AudioCVT cvt;
		SDL_BuildAudioCVT(&cvt, waveFormat.format, waveFormat.channels, waveFormat.freq, AUDIO_S16LSB, targetFormat.channels, targetFormat.freq);
		cvt.buf = new unsigned char[waveLength * cvt.len_mult];
		memcpy(cvt.buf, waveData, waveLength);
		cvt.len = waveLength;
		SDL_ConvertAudio(&cvt);
		SDL_FreeWAV(waveData);

		// Write data
		const short* data = (short*)cvt.buf;
		const int length = cvt.len_cvt / (targetFormat.channels * sizeof(short));
	#endif

		short buf0[2048];
		short buf1[2048];
		short* buf[2] = { buf0, buf1 };

		const int pages = length/2048+1;
		for (int page = 0; page < pages; ++page)
		{
			int pagesize = (page < pages-1) ? 2048 : (length % 2048);
			for (int i = 0; i < pagesize; ++i)
			{
				for (int j = 0; j < 2; ++j)
				{
					buf[j][i] = data[(page*2048+i)*2+j];
				}
			}

			outBuffer.addData(buf, pagesize);
		}
		return true;
	}

}
