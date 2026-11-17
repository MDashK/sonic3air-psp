/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: audio sources for audio prepared on a PC (see AudioExport and psp/tools/convert_audio.py).
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#if defined(PLATFORM_PSP)

#include "oxygen/application/audio/AudioSourceBase.h"


// Audio files packed into one "audio_bank.bin" per folder (made by psp/tools/convert_audio.py)
//  -> Opening a file in a folder with hundreds of files takes 12-15 ms on a memory stick: the bank stays open instead
namespace PSPAudioBank
{
	struct File
	{
		int mHandle = -1;			// The bank's file handle, or a file of its own (then mOwnsHandle is set)
		uint32 mOffset = 0;			// Where the file starts
		uint32 mSize = 0;
		bool mOwnsHandle = false;
		void* mBank = nullptr;		// The bank it's in (its file is open only while some file of it is open), or null
	};

	// Statistics for the benchmark log (time in microseconds)
	inline uint32 gStatFileOpens = 0;
	inline uint32 gStatFileOpenUs = 0;
	inline uint32 gStatFileOpenMaxUs = 0;
	inline uint32 gStatSoundLoads = 0;
	inline uint32 gStatSoundLoadUs = 0;
	inline uint32 gStatSoundLoadMaxUs = 0;
	inline uint64 gStatTotalFileOpenUs = 0;		// (all the time, not reset: for the slow frames)
	inline uint64 gStatTotalSoundLoadUs = 0;

	void startup();				// Loads the ATRAC3 decoder modules early (they take ~50 ms)
	bool open(const std::string& filename, File& outFile);	// From the bank of its folder if there is one, otherwise the file itself
	void close(File& file);
	int read(const File& file, uint32 position, void* destination, uint32 bytes);
}


// A WAV file (sound effects, jingles), loaded completely and kept as long as it's used
class PSPWavAudioSource : public AudioSourceBase
{
public:
	explicit PSPWavAudioSource(int loopStart = -1);		// (loopStart >= 0: continuous sound effect, loops from there; in the file's samples)

	bool load(const std::wstring& filename);

	virtual void onPlaybackStart(AudioReference& audioRef, float time) override;

protected:
	virtual void resetInternal() override;
	virtual State startupInternal() override;
	virtual void progressInternal(float precacheTime) override;

private:
	State loadWav();

private:
	std::wstring mFilename;
	int mLoopStart = -1;
};


// An ATRAC3 file (music), decoded by the PSP's Media Engine while it streams from the memory stick
//  -> Like a dynamic OggAudioSource: the audio buffer only holds what's about to be played, loops are done by seeking
class PSPAtrac3AudioSource : public AudioSourceBase
{
public:
	PSPAtrac3AudioSource(bool isLooping, int loopStart, int loopEnd = 0);
	~PSPAtrac3AudioSource();

	bool load(const std::wstring& filename);

	virtual void onPlaybackStart(AudioReference& audioRef, float time) override;
	virtual float mapAudioRefPositionToTrackPosition(float audioRefPosition) const override;

protected:
	virtual void resetInternal() override;
	virtual State startupInternal() override;
	virtual void progressInternal(float precacheTime) override;

private:
	// Whether it should have a decoder now: played (its read position moved just now) and running out of decoded audio
	bool needsDecoder() { return !(mDecoderEnded && !mIsLooping) && FTX::getTime() - mLastConsumedTime < 0.5f && getDecodedAhead() < 1.0f; }
	float getDecodedAhead() { return mAudioBuffer.getLengthInSec() - mReadTime; }

	bool open();
	void close(const char* reason);
	bool seek(uint32 sample);
	void topUpStream();
	bool decodeUntil(float targetTime);

private:
	std::string mFilename;		// UTF-8, for sceIoOpen
	bool mIsLooping = false;
	int mLoopStart = 0;
	int mLoopEnd = 0;				// Where the loop goes back (0 = at the end of the file): the converter adds a bit of the loop start after it, as the encoder cuts the end
	int mAtracId = -1;
	PSPAudioBank::File mFile;
	bool mStreaming = false;		// Larger than the buffer: topped up from the file while it plays
	bool mStreamRefilling = false;	// Topping up the stream right now (see "topUpStream")
	bool mLoggedPendingLimit = false;
	bool mDecoderEnded = false;
	int mTrackLength = -1;			// In samples, known after the first time through
	int mInitialSeekPos = 0;
	uint32 mNextSample = 0;
	uint8* mBuffer = nullptr;		// Encoded data for the decoder
	bool mEvicted = false;			// Its decoder was taken for another track: opens again where it was if it's still played
	uint32 mResumeSample = 0;
	float mLastReadTime = -1.0f;
	float mLastConsumedTime = -100.0f;	// When the read position moved the last time (a track paused under a jingle doesn't need a decoder)
};

#endif
