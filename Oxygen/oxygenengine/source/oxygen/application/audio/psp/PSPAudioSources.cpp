/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: audio sources for audio prepared on a PC (see AudioExport and psp/tools/convert_audio.py).
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#if defined(PLATFORM_PSP)

#include "oxygen/application/audio/psp/PSPAudioSources.h"

#include <pspatrac3.h>
#include <psputility.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <malloc.h>


namespace
{
	static const constexpr int ATRAC3_BUFFER_SIZE = 64 * 1024;	// ~4s at 132 kbps
	static const constexpr int ATRAC3_MAX_READ = 16 * 1024;		// The most read from the file at once
	static const constexpr size_t ATRAC3_MAX_PENDING_BYTES = 768 * 1024;	// The most decoded audio kept (~4.5 seconds), see "decodeUntil"

	bool gAtracModulesTried = false;
	bool gAtracModulesLoaded = false;

	static const constexpr uint32 ATRAC_ERROR_NO_ATRACID = 0x80630003;	// All of the PSP's ATRAC decoders are in use
	std::vector<PSPAtrac3AudioSource*> gOpenAtracSources;				// The ones holding a decoder

	bool loadAtracModules()
	{
		if (!gAtracModulesTried)
		{
			gAtracModulesTried = true;
			const int avcodec = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
			const int atrac = sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);
			// (0x80020139: already loaded)
			gAtracModulesLoaded = (avcodec >= 0 || avcodec == (int)0x80020139) && (atrac >= 0 || atrac == (int)0x80020139);
			RMX_LOG_INFO("PSP: ATRAC3 modules " << rmx::hexString(avcodec, 8) << " " << rmx::hexString(atrac, 8));
		}
		return gAtracModulesLoaded;
	}

	// Adds interleaved stereo or mono samples to an audio buffer, which wants them per channel
	void addInterleaved(AudioBuffer& audioBuffer, const int16* samples, int numSamples, int channels)
	{
		static int16 planar[2][2048];
		int16* pointers[2] = { planar[0], planar[1] };
		while (numSamples > 0)
		{
			const int count = std::min(numSamples, 2048);
			for (int i = 0; i < count; ++i)
			{
				planar[0][i] = samples[i * channels];
				planar[1][i] = samples[i * channels + channels - 1];
			}
			audioBuffer.addData(pointers, count);
			samples += count * channels;
			numSamples -= count;
		}
	}
}


// ----- PSPAudioBank -----

namespace PSPAudioBank
{
	namespace
	{
		// Bank file: "S3AB", version 1, number of files, reserved (all uint32), then per file 64 bytes: name (56 bytes, zero-terminated), offset, size
		struct Entry
		{
			uint32 mOffset = 0;
			uint32 mSize = 0;
		};

		// The memory stick only lets the game have ~10 files open at once (and the mods' sprite packs, logs etc. need some
		// as well): so the music banks' files (the game's own remastered music, the mods' music) are only open while some
		// file in them is open, e.g. the music playing from them, plus the one or two used last (opening takes ~15 ms, a
		// frame drop: the same music often starts again soon, e.g. after a jingle). At most MAX_OPEN_MUSIC_BANKS of them.
		static const constexpr int MAX_OPEN_MUSIC_BANKS = 2;
		struct Bank
		{
			bool mValid = false;	// False if the folder has no bank
			bool mKeepOpen = false;	// The game's sound effects: stays open (loaded all the time, and opening takes ~15 ms each time)
			std::string mPath;
			int mHandle = -1;		// Open while mUsers > 0, and a while after
			int mUsers = 0;
			uint32 mLastUse = 0;
			std::unordered_map<std::string, Entry> mEntries;
		};

		std::unordered_map<std::string, Bank> gBanks;	// By folder (with trailing slash)
		SDL_mutex* gMutex = nullptr;					// Reading means seeking first, so one at a time
		uint32 gBankUses = 0;

		void closeUnusedBanks(int maxOpen)
		{
			// Closes the music banks not in use, used longest ago first, until at most maxOpen music banks are open
			while (true)
			{
				int numOpen = 0;
				Bank* oldest = nullptr;
				for (auto& [folder, bank] : gBanks)
				{
					if (bank.mHandle < 0 || bank.mKeepOpen)
						continue;
					++numOpen;
					if (bank.mUsers == 0 && (nullptr == oldest || bank.mLastUse < oldest->mLastUse))
						oldest = &bank;
				}
				if (numOpen <= maxOpen || nullptr == oldest)
					return;
				sceIoClose(oldest->mHandle);
				oldest->mHandle = -1;
			}
		}

		int timedOpen(const char* path)
		{
			// Opening a file on the memory stick takes long: measured for the benchmark log
			const uint32 startTime = sceKernelGetSystemTimeLow();
			const int handle = sceIoOpen(path, PSP_O_RDONLY, 0);
			const uint32 us = sceKernelGetSystemTimeLow() - startTime;
			++gStatFileOpens;
			gStatFileOpenUs += us;
			gStatTotalFileOpenUs += us;
			gStatFileOpenMaxUs = std::max(gStatFileOpenMaxUs, us);
			return handle;
		}

		uint32 readUint32(const uint8* data)
		{
			uint32 value;
			memcpy(&value, data, 4);
			return value;
		}

		Bank& getBank(const std::string& folder)
		{
			const auto it = gBanks.find(folder);
			if (it != gBanks.end())
				return it->second;

			Bank& bank = gBanks[folder];
			const std::string path = folder + "audio_bank.bin";
			const int handle = timedOpen(path.c_str());
			if (handle < 0)
				return bank;

			uint8 header[16];
			if (sceIoRead(handle, header, 16) == 16 && memcmp(header, "S3AB", 4) == 0 && readUint32(&header[4]) == 1)
			{
				const uint32 count = readUint32(&header[8]);
				std::vector<uint8> index(count * 64);
				if (count > 0 && sceIoRead(handle, &index[0], (SceSize)index.size()) == (int)index.size())
				{
					for (uint32 i = 0; i < count; ++i)
					{
						const uint8* entry = &index[i * 64];
						const std::string name((const char*)entry, strnlen((const char*)entry, 56));
						bank.mEntries[name] = Entry { readUint32(&entry[56]), readUint32(&entry[60]) };
					}
					bank.mValid = true;
					bank.mKeepOpen = (folder == "data/audio/original/");
					bank.mPath = path;
					sceIoClose(handle);		// (opened again when one of its files gets opened)
					RMX_LOG_INFO("PSP: audio bank " << path << " with " << count << " files");
					return bank;
				}
			}
			RMX_LOG_INFO("PSP: invalid audio bank " << path);
			sceIoClose(handle);
			return bank;
		}
	}

	void startup()
	{
		if (nullptr == gMutex)
			gMutex = SDL_CreateMutex();
		loadAtracModules();
	}

	bool open(const std::string& filename, File& outFile)
	{
		close(outFile);
		if (nullptr == gMutex)
			gMutex = SDL_CreateMutex();

		const size_t slash = filename.find_last_of('/');
		const std::string folder = (slash == std::string::npos) ? std::string() : filename.substr(0, slash + 1);
		const std::string name = (slash == std::string::npos) ? filename : filename.substr(slash + 1);

		SDL_LockMutex(gMutex);
		Bank& bank = getBank(folder);
		if (bank.mValid)
		{
			const auto it = bank.mEntries.find(name);
			if (it != bank.mEntries.end())
			{
				bank.mLastUse = ++gBankUses;
				if (bank.mHandle < 0)
				{
					if (!bank.mKeepOpen)
						closeUnusedBanks(MAX_OPEN_MUSIC_BANKS - 1);
					bank.mHandle = timedOpen(bank.mPath.c_str());
					if (bank.mHandle < 0)
					{
						RMX_LOG_INFO("PSP: could not open audio bank " << bank.mPath << ", error " << rmx::hexString(bank.mHandle, 8));
						bank.mHandle = -1;
						SDL_UnlockMutex(gMutex);
						return false;
					}
				}
				++bank.mUsers;
				outFile.mHandle = bank.mHandle;
				outFile.mOffset = it->second.mOffset;
				outFile.mSize = it->second.mSize;
				outFile.mOwnsHandle = false;
				outFile.mBank = &bank;		// (references to unordered_map values stay valid)
				SDL_UnlockMutex(gMutex);
				return true;
			}
		}
		SDL_UnlockMutex(gMutex);

		// Not in a bank: the file itself
		const int handle = timedOpen(filename.c_str());
		if (handle < 0)
			return false;
		outFile.mHandle = handle;
		outFile.mOffset = 0;
		outFile.mSize = (uint32)std::max(sceIoLseek32(handle, 0, PSP_SEEK_END), 0);
		outFile.mOwnsHandle = true;
		return true;
	}

	void close(File& file)
	{
		if (file.mOwnsHandle && file.mHandle >= 0)
			sceIoClose(file.mHandle);
		if (nullptr != file.mBank)
		{
			SDL_LockMutex(gMutex);
			Bank& bank = *static_cast<Bank*>(file.mBank);
			if (bank.mUsers > 0 && --bank.mUsers == 0)
			{
				// Stays open for now (see MAX_OPEN_MUSIC_BANKS)
				bank.mLastUse = ++gBankUses;
				closeUnusedBanks(MAX_OPEN_MUSIC_BANKS);
			}
			SDL_UnlockMutex(gMutex);
		}
		file = File();
	}

	int read(const File& file, uint32 position, void* destination, uint32 bytes)
	{
		if (file.mHandle < 0 || position >= file.mSize)
			return 0;
		bytes = std::min(bytes, file.mSize - position);
		SDL_LockMutex(gMutex);
		int result = sceIoLseek32(file.mHandle, (int)(file.mOffset + position), PSP_SEEK_SET);
		if (result >= 0)
			result = sceIoRead(file.mHandle, destination, bytes);
		SDL_UnlockMutex(gMutex);
		return result;
	}
}


// ----- PSPWavAudioSource -----

PSPWavAudioSource::PSPWavAudioSource(int loopStart) :
	AudioSourceBase(AudioSourceType::PSP_WAV, CachingType::STREAMING_STATIC),
	mLoopStart(loopStart)
{
	mAudioBuffer.setPersistent(true);
}

void PSPWavAudioSource::onPlaybackStart(AudioReference& audioRef, float time)
{
	// Continuous sound effect: loops while it gets started again (see AudioPlayer::playAudioInternal)
	if (mLoopStart >= 0)
	{
		audioRef.setLoopStartInSamples(mLoopStart);
		audioRef.setLoop(true);
	}
}

bool PSPWavAudioSource::load(const std::wstring& filename)
{
	mFilename = filename;
	mAudioBuffer.setName(WString(filename).toStdString());
	return true;
}

void PSPWavAudioSource::resetInternal()
{
}

AudioSourceBase::State PSPWavAudioSource::startupInternal()
{
	// Out of memory (the special stages leave only a few MB, and that gets fragmented): rather go without this sound than
	// let the exception stop the game's scripts in every frame from now on
	try
	{
		const uint32 startTime = sceKernelGetSystemTimeLow();
		const State state = loadWav();
		const uint32 us = sceKernelGetSystemTimeLow() - startTime;
		++PSPAudioBank::gStatSoundLoads;
		PSPAudioBank::gStatSoundLoadUs += us;
		PSPAudioBank::gStatTotalSoundLoadUs += us;
		PSPAudioBank::gStatSoundLoadMaxUs = std::max(PSPAudioBank::gStatSoundLoadMaxUs, us);
		return state;
	}
	catch (const std::bad_alloc&)
	{
		static int logged = 0;
		if (logged < 5)
		{
			++logged;
			RMX_LOG_INFO("PSP: not enough memory for sound " << *WString(mFilename).toString() << ", not played");
		}
		mAudioBuffer.lock();
		mAudioBuffer.clear(44100, 2);
		mAudioBuffer.setCompleted();
		mAudioBuffer.unlock();
		return State::COMPLETED;
	}
}

AudioSourceBase::State PSPWavAudioSource::loadWav()
{
	// From the audio bank if possible, as opening a file of its own takes long
	//  -> Only the header gets read first: the samples go right into their aligned buffer (no second copy of the whole file)
	PSPAudioBank::File file;
	std::vector<uint8> content;		// (only for the fallback, a file of its own)
	const bool fromBank = PSPAudioBank::open(*WString(mFilename).toUTF8(), file);
	struct FileGuard
	{
		PSPAudioBank::File& mFile;
		~FileGuard() { PSPAudioBank::close(mFile); }	// (also when the samples don't fit in memory: the bank's file gets closed again)
	} fileGuard { file };
	uint8 header[512];
	uint32 headerSize = 0;
	if (fromBank)
	{
		const int bytes = PSPAudioBank::read(file, 0, header, std::min<uint32>(sizeof(header), file.mSize));
		headerSize = (bytes > 0) ? (uint32)bytes : 0;
	}
	else
	{
		FTX::FileSystem->readFile(mFilename, content);
		headerSize = (uint32)std::min<size_t>(sizeof(header), content.size());
		if (headerSize > 0)
			memcpy(header, &content[0], headerSize);
	}
	if (headerSize < 44 || memcmp(header, "RIFF", 4) != 0)
	{
		if (fromBank)
			PSPAudioBank::close(file);
		RMX_LOG_INFO("PSP: could not load audio file " << *WString(mFilename).toString());
		return State::COMPLETED;
	}

	// The chunks of a simple WAV file: "fmt " and "data"
	int frequency = 44100;
	int channels = 2;
	uint32 dataOffset = 0;
	uint32 dataBytes = 0;
	uint32 position = 12;
	while (position + 8 <= headerSize)
	{
		const uint32 chunkSize = *(const uint32_ua*)&header[position + 4];
		if (memcmp(&header[position], "fmt ", 4) == 0 && chunkSize >= 16 && position + 8 + 16 <= headerSize)
		{
			channels = *(const uint16_ua*)&header[position + 10];
			frequency = *(const uint32_ua*)&header[position + 12];
		}
		else if (memcmp(&header[position], "data", 4) == 0)
		{
			dataOffset = position + 8;
			dataBytes = chunkSize;
			break;
		}
		position += 8 + chunkSize + (chunkSize & 1);
	}
	const uint32 fileSize = fromBank ? file.mSize : (uint32)content.size();
	if (dataOffset > 0)
		dataBytes = std::min(dataBytes, fileSize - std::min(dataOffset, fileSize));
	const int numSamples = (channels == 1 || channels == 2) ? (int)(dataBytes / (2 * channels)) : 0;

	std::vector<int16> samples;
	if (numSamples > 0)
	{
		samples.resize((size_t)numSamples * channels);
		if (fromBank)
		{
			if (PSPAudioBank::read(file, dataOffset, &samples[0], (uint32)(samples.size() * sizeof(int16))) != (int)(samples.size() * sizeof(int16)))
				samples.clear();
		}
		else
		{
			memcpy(&samples[0], &content[dataOffset], samples.size() * sizeof(int16));
			std::vector<uint8>().swap(content);
		}
	}
	if (fromBank)
		PSPAudioBank::close(file);

	struct BufferLock
	{
		AudioBuffer& mBuffer;
		explicit BufferLock(AudioBuffer& buffer) : mBuffer(buffer) { mBuffer.lock(); }
		~BufferLock() { mBuffer.unlock(); }		// (also when adding the samples runs out of memory)
	};
	BufferLock bufferLock(mAudioBuffer);
	mAudioBuffer.clear(frequency, (channels == 1) ? 1 : 2);
	if (!samples.empty())
		addInterleaved(mAudioBuffer, &samples[0], numSamples, channels);
	mAudioBuffer.setCompleted();
	return State::COMPLETED;
}

void PSPWavAudioSource::progressInternal(float precacheTime)
{
	// Everything is loaded at startup
}


// ----- PSPAtrac3AudioSource -----

PSPAtrac3AudioSource::PSPAtrac3AudioSource(bool isLooping, int loopStart, int loopEnd) :
	AudioSourceBase(AudioSourceType::PSP_ATRAC3, CachingType::STREAMING_DYNAMIC),
	mIsLooping(isLooping),
	mLoopStart(loopStart),
	mLoopEnd(loopEnd)
{
	if (mIsLooping && mLoopEnd > mLoopStart)
		mTrackLength = mLoopEnd;
	// Only what's about to be played stays in the audio buffer
	mAudioBuffer.setPersistent(false);
}

PSPAtrac3AudioSource::~PSPAtrac3AudioSource()
{
	close("destroyed");
	if (nullptr != mBuffer)
		free(mBuffer);
}

bool PSPAtrac3AudioSource::load(const std::wstring& filename)
{
	mFilename = *WString(filename).toUTF8();
	mAudioBuffer.setName(mFilename);
	return true;
}

bool PSPAtrac3AudioSource::open()
{
	close("opened again");
	if (!loadAtracModules())
		return false;
	if (nullptr == mBuffer)
		mBuffer = (uint8*)memalign(64, ATRAC3_BUFFER_SIZE);

	if (!PSPAudioBank::open(mFilename, mFile))
	{
		RMX_LOG_INFO("PSP: could not open music file " << mFilename);
		return false;
	}
	const int fileSize = (int)mFile.mSize;
	const int bytes = PSPAudioBank::read(mFile, 0, mBuffer, std::min(fileSize, ATRAC3_BUFFER_SIZE));
	if (bytes < 12 || memcmp(mBuffer, "RIFF", 4) != 0)
	{
		RMX_LOG_INFO("PSP: music file " << mFilename << " is not a RIFF file");
		close("not a RIFF file");
		return false;
	}

	mAtracId = sceAtracSetDataAndGetID(mBuffer, std::min(fileSize, ATRAC3_BUFFER_SIZE));
	while (mAtracId == (int)ATRAC_ERROR_NO_ATRACID)
	{
		// The PSP has only two ATRAC decoders for the game: take one from a track that doesn't need it right now, the one
		// used longest ago (it opens again when it gets played); if all of them do, from the one with the most decoded ahead
		PSPAtrac3AudioSource* oldest = nullptr;
		bool oldestNeedsDecoder = true;
		for (PSPAtrac3AudioSource* source : gOpenAtracSources)
		{
			const bool needsDecoder = source->needsDecoder();
			const bool better = (nullptr == oldest) ||
								(oldestNeedsDecoder && !needsDecoder) ||
								(oldestNeedsDecoder == needsDecoder && (needsDecoder ? source->getDecodedAhead() > oldest->getDecodedAhead() : source->mLastUsedTimestamp < oldest->mLastUsedTimestamp));
			if (better)
			{
				oldest = source;
				oldestNeedsDecoder = needsDecoder;
			}
		}
		if (nullptr == oldest)
			break;
		// (a track whose decoder already ended, e.g. a jingle, has everything decoded: it doesn't need to open again)
		oldest->mEvicted = !(oldest->mDecoderEnded && !oldest->mIsLooping);
		oldest->mResumeSample = oldest->mNextSample;
		oldest->close(("decoder taken by " + mFilename).c_str());
		mAtracId = sceAtracSetDataAndGetID(mBuffer, std::min(fileSize, ATRAC3_BUFFER_SIZE));
	}
	if (mAtracId >= 0)
	{
		gOpenAtracSources.push_back(this);
		RMX_LOG_INFO("PSP: ATRAC3 opened " << mFilename << " (decoder " << mAtracId << ", " << gOpenAtracSources.size() << " open)");
	}
	if (mAtracId < 0)
	{
		RMX_LOG_INFO("PSP: music file " << mFilename << " rejected by the ATRAC3 decoder (" << rmx::hexString(mAtracId, 8) << ")");
		mAtracId = -1;
		close("rejected");
		return false;
	}
	mStreaming = (fileSize > ATRAC3_BUFFER_SIZE);
	mStreamRefilling = false;
	if (!mStreaming)
		PSPAudioBank::close(mFile);
	sceAtracSetLoopNum(mAtracId, 0);	// The loop is done here, by seeking
	mDecoderEnded = false;
	mNextSample = 0;
	return true;
}

void PSPAtrac3AudioSource::close(const char* reason)
{
	if (mAtracId >= 0)
	{
		RMX_LOG_INFO("PSP: ATRAC3 closed " << mFilename << " (" << reason << ")");
		sceAtracReleaseAtracID(mAtracId);
		gOpenAtracSources.erase(std::remove(gOpenAtracSources.begin(), gOpenAtracSources.end(), this), gOpenAtracSources.end());
	}
	PSPAudioBank::close(mFile);
	mAtracId = -1;

	// The encoded data buffer too: every music track played so far would keep its 64 KB otherwise (after the decoder is released)
	if (nullptr != mBuffer)
	{
		free(mBuffer);
		mBuffer = nullptr;
	}
}

bool PSPAtrac3AudioSource::seek(uint32 sample)
{
	mDecoderEnded = false;
	mNextSample = sample;
	if (!mStreaming)
		return sceAtracResetPlayPosition(mAtracId, sample, 0, 0) >= 0;

	// When streaming, the decoder says which parts of the file it needs from there
	PspBufferInfo info;
	if (sceAtracGetBufferInfoForReseting(mAtracId, sample, &info) < 0)
		return false;
	int first = 0;
	int second = 0;
	if (info.uiWritableByteFirstBuf)
		first = PSPAudioBank::read(mFile, info.uiReadPositionFirstBuf, info.pucWritePositionFirstBuf, info.uiWritableByteFirstBuf);
	if (info.uiWritableByteSecondBuf)
		second = PSPAudioBank::read(mFile, info.uiReadPositionSecondBuf, info.pucWritePositionSecondBuf, info.uiWritableByteSecondBuf);
	return (first >= 0 && second >= 0 && sceAtracResetPlayPosition(mAtracId, sample, first, second) >= 0);
}

void PSPAtrac3AudioSource::topUpStream()
{
	if (!mStreaming || mFile.mHandle < 0)
		return;

	// Only when the decoder is running low, then in a few reads in a row: reading a bit every second keeps the memory
	// stick busy all the time (its LED never stops blinking)
	if (!mStreamRefilling)
	{
		int remainingFrames = 0;
		if (sceAtracGetRemainFrame(mAtracId, &remainingFrames) >= 0 && remainingFrames > 64)	// (1024 samples each, so about 1.5 seconds)
			return;
		mStreamRefilling = true;
	}
	u8* writePosition = nullptr;
	u32 available = 0;
	u32 readOffset = 0;
	if (sceAtracGetStreamDataInfo(mAtracId, &writePosition, &available, &readOffset) >= 0 && available > 0)
	{
		const int bytesRead = PSPAudioBank::read(mFile, readOffset, writePosition, std::min<u32>(available, ATRAC3_MAX_READ));
		if (bytesRead > 0)
			sceAtracAddStreamData(mAtracId, bytesRead);
		else
			mStreamRefilling = false;	// (end of the file)
	}
	else
	{
		mStreamRefilling = false;		// The buffer is full again
	}
}

bool PSPAtrac3AudioSource::decodeUntil(float targetTime)
{
	// Returns false if the track ended (without looping)
	//  -> The audio buffer is only locked while adding samples: the audio thread mixes from it in the meantime
	static int16 pcm[2048] __attribute__((aligned(64)));		// One decoded frame: 1024 stereo samples
	int failures = 0;
	while (mAudioBuffer.getLengthInSec() < targetTime)
	{
		// Never more than this decoded and not played yet (normally it's about a second): if the played part doesn't get
		// purged (by the audio manager, behind the playback position), this would otherwise grow until memory runs out
		{
			mAudioBuffer.lock();
			const size_t pendingBytes = mAudioBuffer.getMemoryUsage();
			mAudioBuffer.unlock();
			if (pendingBytes > ATRAC3_MAX_PENDING_BYTES)
			{
				if (!mLoggedPendingLimit)
				{
					RMX_LOG_INFO("PSP: ATRAC3 " << mFilename << " has " << (pendingBytes / 1024) << " KB decoded that isn't played back, decoding stops");
					mLoggedPendingLimit = true;
				}
				break;
			}
		}

		if (mDecoderEnded)
		{
			if (mTrackLength < 0)
				mTrackLength = (int)mNextSample;
			if (mIsLooping && seek((uint32)std::max(mLoopStart, 0)))
				continue;
			return false;
		}

		topUpStream();
		int numSamples = 0;
		int end = 0;
		int remainingFrames = 0;
		const int result = sceAtracDecodeData(mAtracId, (u16*)pcm, &numSamples, &end, &remainingFrames);
		if (result < 0)
		{
			if (result == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED)
			{
				mDecoderEnded = true;
				continue;
			}
			// Starved (the memory stick fell behind): try again next time
			if (++failures > 2)
				break;
			continue;
		}
		if (mIsLooping && mLoopEnd > mLoopStart && (int)mNextSample + numSamples >= mLoopEnd)
		{
			// Loop end reached: what comes after it is only there for the encoder
			numSamples = std::max(mLoopEnd - (int)mNextSample, 0);
			end = 1;
		}
		if (numSamples > 0)
		{
			mAudioBuffer.lock();
			addInterleaved(mAudioBuffer, pcm, numSamples, 2);
			mAudioBuffer.unlock();
			mNextSample += numSamples;
		}
		if (end)
			mDecoderEnded = true;
		if (numSamples == 0 && !mDecoderEnded)
			break;
	}
	return true;
}

void PSPAtrac3AudioSource::onPlaybackStart(AudioReference& audioRef, float time)
{
	SDL_LockMutex(mMutex);
	audioRef.setLoop(false);	// Looping is done by seeking in the stream

	// Always start with an empty audio buffer, it will get filled now
	mAudioBuffer.lock();
	mAudioBuffer.clear(44100, 2);
	mAudioBuffer.unlock();
	mReadTime = 0.0f;
	mLoggedPendingLimit = false;

	mInitialSeekPos = 0;
	if (mAtracId >= 0 && time > 0.0f)
	{
		uint32 sample = (uint32)roundToInt(time * 44100.0f);
		if (mTrackLength > 0 && (int)sample >= mTrackLength && mTrackLength > mLoopStart)
			sample = (uint32)(mLoopStart + ((int)sample - mLoopStart) % (mTrackLength - mLoopStart));
		if (seek(sample))
			mInitialSeekPos = (int)sample;
	}
	else if (mAtracId >= 0 && mNextSample != 0)
	{
		seek(0);
	}
	SDL_UnlockMutex(mMutex);
}

float PSPAtrac3AudioSource::mapAudioRefPositionToTrackPosition(float audioRefPosition) const
{
	// Same as in OggAudioSource: the position inside the track, looping included
	int trackPosition = mInitialSeekPos + roundToInt(audioRefPosition * 44100.0f);
	if (mTrackLength > 0 && trackPosition >= mTrackLength)
	{
		const int loopingPartLength = mTrackLength - mLoopStart;
		if (loopingPartLength > 0)
			trackPosition = mLoopStart + (trackPosition - mLoopStart) % loopingPartLength;
	}
	return (float)trackPosition / 44100.0f;
}

void PSPAtrac3AudioSource::resetInternal()
{
	close("unloaded");
}

AudioSourceBase::State PSPAtrac3AudioSource::startupInternal()
{
	SDL_LockMutex(mMutex);
	mAudioBuffer.lock();
	mAudioBuffer.clear(44100, 2);
	mAudioBuffer.unlock();
	const bool success = open();
	SDL_UnlockMutex(mMutex);
	return success ? State::STREAMING : State::COMPLETED;
}

void PSPAtrac3AudioSource::progressInternal(float precacheTime)
{
	// Decoding on the calling thread: the Media Engine does the actual work
	SDL_LockMutex(mMutex);
	if (mReadTime != mLastReadTime)
	{
		mLastReadTime = mReadTime;
		mLastConsumedTime = FTX::getTime();
	}
	if (mAtracId < 0 && mEvicted && needsDecoder())
	{
		// Its decoder got taken for another track while this one still plays: continue where it was
		mEvicted = false;
		const uint32 resumeSample = mResumeSample;
		if (open() && seek(resumeSample))
			RMX_LOG_INFO("PSP: ATRAC3 resumed " << mFilename << " at sample " << resumeSample);
	}
	if (mAtracId >= 0)
	{
		const bool playing = decodeUntil(precacheTime);
		if (!playing)
		{
			RMX_LOG_INFO("PSP: ATRAC3 ended " << mFilename << (mIsLooping ? " (loop seek failed)" : ""));
			mAudioBuffer.lock();
			mAudioBuffer.setCompleted();
			mAudioBuffer.unlock();
			mState = State::COMPLETED;
		}
	}
	SDL_UnlockMutex(mMutex);
}

#endif
