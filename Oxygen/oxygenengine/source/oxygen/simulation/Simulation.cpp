/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/simulation/Simulation.h"
#include "oxygen/simulation/CodeExec.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/GameRecorder.h"
#include "oxygen/simulation/LogDisplay.h"
#include "oxygen/simulation/SaveStateSerializer.h"
#include "oxygen/simulation/SimulationState.h"
#include "oxygen/simulation/analyse/ROMDataAnalyser.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/audio/AudioOutBase.h"
#include "oxygen/application/audio/AudioPlayer.h"
#include "oxygen/rendering/sprite/ComponentSprite.h"
#include "oxygen/rendering/sprite/PaletteSprite.h"
#include "oxygen/resources/SpriteCollection.h"
#include "oxygen/application/input/InputRecorder.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/engine/EngineMain.h"
#include "oxygen/engine/modding/ModManager.h"
#include "oxygen/helper/Logging.h"
#include "oxygen/network/netplay/NetplayManager.h"
#include "oxygen/platform/PlatformFunctions.h"
#include "oxygen/rendering/parts/RenderParts.h"
#if defined(PLATFORM_PSP)
	#include "oxygen/rendering/psp/PSPGe.h"
	#include "oxygen/application/audio/psp/PSPAudioSources.h"
#endif

#include <lemon/runtime/RuntimeFunction.h>


namespace
{
	void recordKeyFrame(uint32 frameNumber, Simulation& simulation, GameRecorder& gameRecorder, const GameRecorder::InputData& inputData)
	{
		static std::vector<uint8> data;
		data.reserve(0x128000);
		data.clear();

		SaveStateSerializer serializer(simulation, RenderParts::instance());
		serializer.saveState(data);

		gameRecorder.addKeyFrame(frameNumber, inputData, data);
	}
}


Simulation::Simulation() :
	mCodeExec(*new CodeExec()),
	mSimulationState(*new SimulationState()),
	mGameRecorder(*new GameRecorder()),
	mInputRecorder(*new InputRecorder())
{
	if (EngineMain::getDelegate().useDeveloperFeatures())
	{
		mROMDataAnalyser = new ROMDataAnalyser();
	}
}

Simulation::~Simulation()
{
	delete &mCodeExec;
	delete &mSimulationState;
	delete &mGameRecorder;
	delete &mInputRecorder;
	delete mROMDataAnalyser;
}

bool Simulation::startup()
{
	Configuration& config = Configuration::instance();

	RMX_LOG_INFO("Setup of EmulatorInterface");
	mCodeExec.startup();

	// Load scripts
	RMX_LOG_INFO("Loading scripts");
	bool success = mCodeExec.reloadScripts(true, false);	// Note: First parameter could just as well be set to false
	if (success)
	{
		mCodeExec.reinitRuntime(nullptr, CodeExec::CallStackInitPolicy::RESET);
	}

	// Optionally load save state
	mStateLoaded.clear();
	if (success && EngineMain::getDelegate().useDeveloperFeatures() && !config.mLoadSaveState.empty() && config.mStartPhase == 3)
	{
		success = loadState(config.mSaveStatesDirLocal + config.mLoadSaveState + L".state", false);
		if (!success)
			loadState(config.mSaveStatesDir + config.mLoadSaveState + L".state", false);
	}
	RMX_LOG_INFO("Runtime environment ready");

	if (EngineMain::getDelegate().useDeveloperFeatures())
	{
		// Startup input recorder
		mInputRecorder.initFromConfig();
	}

	if (mGameRecorder.isPlaying())
	{
		// Try the long and short name
		if (mGameRecorder.loadRecording(L"gamerecording.bin"))
		{
			RMX_LOG_INFO("Playback of 'gamerecording.bin'");
		}
		else if (mGameRecorder.loadRecording(L"gamerec.bin"))
		{
			RMX_LOG_INFO("Playback of 'gamerec.bin'");
		}

		if (mGameRecorder.hasFrameNumber(mFrameNumber))
		{
			mGameRecorder.setIgnoreKeys(config.mGameRecorder.mPlaybackIgnoreKeys);
			config.setSettingsReadOnly(true);	// Do not overwrite settings
			jumpToFrame(config.mGameRecorder.mPlaybackStartFrame, false);
		}
	}

	return true;
}

void Simulation::shutdown()
{
	RMX_LOG_INFO("Simulation shutdown");
	mInputRecorder.shutdown();

	mIsRunning = false;
}

void Simulation::reloadScriptsAfterModsChange()
{
	// Immediate reload of the scripts (while the loading text box is shown)
	if (mCodeExec.reloadScripts(false, false))
	{
		mCodeExec.reinitRuntime(nullptr, CodeExec::CallStackInitPolicy::RESET);
	}
}

EmulatorInterface& Simulation::getEmulatorInterface()
{
	return mCodeExec.getEmulatorInterface();
}

void Simulation::resetState()
{
	EngineMain::instance().getAudioOut().reset();
	resetIntoGame(nullptr);
}

void Simulation::resetIntoGame(const std::vector<std::pair<std::string, std::string>>* enforcedCallStack)
{
	// Reset randomization
	randomize();

	// Reset simulation
	mSimulationState.reset();

	// Reset video & audio
	VideoOut::instance().reset();
	EngineMain::instance().getAudioOut().resetGame();

	// Reset code execution
	mCodeExec.reset();
	mStateLoaded.clear();

	// Reload and initialize scripts as needed
	if (mCodeExec.reloadScripts(false, false))
	{
		mCodeExec.reinitRuntime(enforcedCallStack, CodeExec::CallStackInitPolicy::RESET);
	}

	// Apply mod settings
	applyModSettingsToGlobals();

	mFrameNumber = 0;
	mCurrentTargetFrame = 0.0;
	mLastCorrectionFrame = 0;
	mStepsLimit = -1;
	mBreakConditions.clearAll();
	mGameRecorder.clear();
}

void Simulation::resetIntoGame(const std::string& entryFunctionName)
{
	const std::vector<std::pair<std::string, std::string>> enforcedCallStack = { { entryFunctionName, "" } };
	resetIntoGame(&enforcedCallStack);
}

void Simulation::reloadLastState()
{
	if (!mStateLoaded.empty())
	{
		loadState(mStateLoaded);
	}
}

bool Simulation::loadState(const std::wstring& filename, bool showError)
{
	VideoOut::instance().reset();
	EngineMain::instance().getAudioOut().reset();

	SaveStateSerializer::StateType stateType;
	SaveStateSerializer serializer(*this, RenderParts::instance());

	const bool success = serializer.loadState(filename, &stateType);
	if (!success)
	{
		if (showError)
			RMX_ERROR("Failed to load save state '" << WString(filename).toStdString() << "'", );
		return false;
	}

	mStateLoaded = filename;
	mCodeExec.reinitRuntime(nullptr, (stateType == SaveStateSerializer::StateType::GENSX) ? CodeExec::CallStackInitPolicy::READ_FROM_ASM : CodeExec::CallStackInitPolicy::USE_EXISTING);

	if (Configuration::instance().mDevMode.mApplyModSettingsAfterLoadState)
	{
		applyModSettingsToGlobals();
	}

	mFrameNumber = 0;
	mCurrentTargetFrame = 0.0;
	mLastCorrectionFrame = 0;
	mStepsLimit = -1;
	mBreakConditions.clearAll();

	mGameRecorder.clear();
	return true;
}

void Simulation::saveState(const std::wstring& filename)
{
	SaveStateSerializer serializer(*this, RenderParts::instance());
	const bool success = serializer.saveState(filename);
	RMX_CHECK(success, "Failed to save save state '" << WString(filename).toStdString() << "'", return);

	// Also save a screenshot
	Bitmap bmp;
	VideoOut::instance().getScreenshot(bmp);
	bmp.save(filename + L".bmp");

	// Set as default for "reloadLastState"
	mStateLoaded = filename;
}

bool Simulation::triggerFullScriptsReload()
{
	if (mCodeExec.reloadScripts(true, true))
	{
		mCodeExec.restoreRuntimeState(!mStateLoaded.empty());
		return true;
	}
	else
	{
		return false;
	}
}

void Simulation::update(float timeElapsed)
{
	if (!isRunning() || !mCodeExec.isCodeExecutionPossible())
		return;

	if (mRewindSteps >= 0)
	{
		setSpeed(0.0f);
		while (mRewindSteps >= 1)
		{
			if (jumpToFrame(mFrameNumber - mRewindSteps))
				mRewindSteps = 0;
			else
				--mRewindSteps;		// Try again with one step less
		}
		mRewindSteps = -1;
	}

	// Limit length of one frame to 100ms
	timeElapsed = clamp(timeElapsed, 0.0f, 0.1f);

	// Do nothing as long as not enough time has passed
	const double oldTargetFrame = mCurrentTargetFrame;
	if (mStepsLimit < 0)
	{
		if (mSimulationSpeed > 0.0f)
		{
			const float step = timeElapsed * mSimulationSpeed;
			mCurrentTargetFrame += (double)step * (double)getSimulationFrequency();
		}
		else
		{
			mCurrentTargetFrame = roundToDouble(mCurrentTargetFrame);
		}
	}
	else if (mStepsLimit > 0)
	{
		mCurrentTargetFrame = roundToDouble(mCurrentTargetFrame + 1.0);
	}

	const bool useFrameInterpolation = (Configuration::instance().mFrameSync == Configuration::FrameSyncType::FRAME_INTERPOLATION);
	const uint32 requiredFrameNumber = useFrameInterpolation ? (uint32)std::ceil(mCurrentTargetFrame) : (uint32)roundToInt(mCurrentTargetFrame);

	if (mFrameNumber < requiredFrameNumber)
	{
		const SDL_TicksType startTime = SDL_GetTicks();
		const SDL_TicksType limitTime = startTime + 200;

	#if defined(PLATFORM_PSP)
		// PSP: at most 2 updates in a row (= one shown frame at 30 fps)
		//  -> When a frame takes longer, the game slows down a bit for that moment, like on the Mega Drive, instead of catching up:
		//     a third update would make the next frame even longer, and the frame rate would spiral down
		int updatesLeft = 2;
	#endif
		while (mFrameNumber < requiredFrameNumber)
		{
		#if defined(PLATFORM_PSP)
			if (updatesLeft-- <= 0)
			{
				mCurrentTargetFrame = (double)mFrameNumber;
				break;
			}
		#endif

			// Update emulation
			const bool result = generateFrame();
			if (!result)
				break;

			// Time limit to prevent non-responding application
			if (SDL_GetTicks() >= limitTime)
			{
				// Reset target frame to an earlier frame, but still make sure we had any progress at all
				mCurrentTargetFrame = std::max((double)mFrameNumber, oldTargetFrame);
				break;
			}
		}

		// Each second, a small correction to the accumulated time gets applied
		if ((int)(mFrameNumber - mLastCorrectionFrame) >= (int)getSimulationFrequency() || mFrameNumber < mLastCorrectionFrame)
		{
			// The idea here is to bring the accumulated time towards the midpoint, where it's most stable against unintentional double frames or frame skips (which might happen otherwise)
			//  -> This is most useful for 60 Hz displays with V-sync on, but should have a similar effect on e.g. 75 Hz, 90 Hz, 120 Hz
			//  -> It should not introduce any noticeable issues or game speed changes in other cases
			const double stableOffset = useFrameInterpolation ? 0.25 : 0.0;
			const double diff = mCurrentTargetFrame - (roundToDouble(mCurrentTargetFrame - stableOffset) + stableOffset);
			const constexpr double maxChange = 0.1;
			mCurrentTargetFrame += clamp(-diff, -maxChange, maxChange);
			mLastCorrectionFrame = mFrameNumber;
		}
	}

	if (mFrameNumber == requiredFrameNumber)
	{
		const float position = saturate((float)(mCurrentTargetFrame - (double)(mFrameNumber - 1)));
		VideoOut::instance().setInterFramePosition(position);
	}
	else
	{
		VideoOut::instance().setInterFramePosition(0.0f);
	}

#if 0
	// Meant for debugging of accumulated time stability
	if ((mFrameNumber % 6) == 0)
		LogDisplay::instance().setModeDisplay(String(0, "diff = %+0.3f", (float)(mCurrentTargetFrame - roundToDouble(mCurrentTargetFrame))));
#endif
}

// ---- PSP feasibility benchmark (temporary instrumentation) ----
#include <chrono>
#include <malloc.h>
#if defined(__PSP__)
	#include <psppower.h>
#endif
namespace s3bench
{
	static int enabled = -1;
	static int maxFrames = 0;
	static int inputMode = 0;
	static FILE* logFile = nullptr;
	static std::vector<float> frameMs;
	static std::vector<uint32> frameSteps;
	static uint32 frameCount = 0;
	static std::chrono::high_resolution_clock::time_point wallStart; static bool wallStarted = false;
	static double windowMs = 0; static double windowMax = 0; static uint64 windowSteps = 0; static int windowCount = 0;
	inline bool isEnabled()
	{
		if (enabled < 0)
		{
			const char* e = getenv("S3AIR_BENCH");
			enabled = (e && e[0] == '1') ? 1 : 0;
			if (enabled)
			{
				const char* f = getenv("S3AIR_BENCH_FRAMES"); maxFrames = f ? atoi(f) : 3600;
				const char* m = getenv("S3AIR_BENCH_INPUT"); inputMode = m ? atoi(m) : 1;
				const char* l = getenv("S3AIR_BENCH_LOG"); logFile = fopen(l ? l : "bench.log", "w");
			}
		}
		return enabled == 1;
	}
	inline uint16 getInput(uint32 frame)
	{
		if (inputMode == 0) return 0;
		if (inputMode == 3)
		{
			// A sequence of button presses: "S3AIR_BENCH_INPUT_SEQ=600:80:4,700:02:4" = at frame 600 START (0x80) for 4 frames, at 700 DOWN (0x02) for 4 frames
			//  -> Buttons: up 01, down 02, left 04, right 08, B 10, C 20, A 40, start 80
			struct Press { uint32 mFrame; uint16 mButtons; uint32 mFrames; };
			static std::vector<Press> presses;
			static bool parsed = false;
			if (!parsed)
			{
				parsed = true;
				const char* text = getenv("S3AIR_BENCH_INPUT_SEQ");
				while (nullptr != text && *text)
				{
					Press press;
					char* end = nullptr;
					press.mFrame = (uint32)strtoul(text, &end, 10);
					if (*end != ':') break;
					press.mButtons = (uint16)strtoul(end + 1, &end, 16);
					if (*end != ':') break;
					press.mFrames = (uint32)strtoul(end + 1, &end, 10);
					presses.push_back(press);
					text = (*end == ',') ? end + 1 : end;
				}
			}
			uint16 buttons = 0;
			for (const Press& press : presses)
			{
				if (frame >= press.mFrame && frame < press.mFrame + press.mFrames)
					buttons |= press.mButtons;
			}
			return buttons;
		}
		if (inputMode == 2) { uint16 f = 0; if (frame < 400 && (frame % 120) < 4) f |= 0x0080; if ((frame % 300) >= 150 && (frame % 300) < 158) f |= 0x0008; if ((frame % 200) < 3) f |= 0x0040; return f; }   // blue spheres: START pulses, turn now and then, jump
		uint16 flags = 0x0008;	// RIGHT
		if ((frame % 90) < 20) flags |= 0x0040;	// A (jump)
		return flags;
	}
#if defined(__PSP__)
	inline size_t heapBytes() { struct mallinfo mi = mallinfo(); return mi.uordblks; }
	inline size_t heapArenaBytes() { struct mallinfo mi = mallinfo(); return mi.arena; }
#else
	inline size_t heapBytes() { struct mallinfo2 mi = mallinfo2(); return mi.uordblks + mi.hblkhd; }
	inline size_t heapArenaBytes() { struct mallinfo2 mi = mallinfo2(); return mi.arena; }
#endif
	inline void finish()
	{
		std::vector<float> sorted = frameMs; std::sort(sorted.begin(), sorted.end());
		double sum = 0; for (float f : frameMs) sum += f;
		uint64 steps = 0; uint32 maxSteps = 0; for (uint32 s : frameSteps) { steps += s; maxSteps = std::max(maxSteps, s); }
		const size_t n = frameMs.size();
		const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - wallStart).count();
		fprintf(logFile, "WALL frames=%zu total_ms=%.1f avg_frame_ms=%.3f fps=%.2f\n", n, wallMs, wallMs / n, 1000.0 * n / wallMs);
		fprintf(logFile, "SUMMARY frames=%zu avg_ms=%.4f p50=%.4f p95=%.4f p99=%.4f max=%.4f avg_steps=%.0f max_steps=%u heap_MB=%.2f\n", n, sum / n, sorted[n/2], sorted[n*95/100], sorted[n*99/100], sorted[n-1], (double)steps / n, maxSteps, heapBytes() / 1048576.0);
		{
			// Checksums of the emulated memory: to compare C++ implementations of script code with the original scripts (same input -> same state)
			EmulatorInterface& emulatorInterface = EmulatorInterface::instance();
			fprintf(logFile, "STATE ram=%016llx vram=%016llx\n", (unsigned long long)rmx::getMurmur2_64(emulatorInterface.getRam(), 0x10000), (unsigned long long)rmx::getMurmur2_64(emulatorInterface.getVRam(), 0x10000));
		}
		fclose(logFile);
		_Exit(0);
	}
}
// ---- end benchmark ----

bool Simulation::generateFrame()
{
	ControlsIn& controlsIn = ControlsIn::instance();

	const bool beginningNewFrame = mCodeExec.willBeginNewFrame();
	const float tickLength = 1.0f / getSimulationFrequency();

	bool completedCurrentFrame = false;

	// Steps to do when beginning a new frame
	if (beginningNewFrame)
	{
		// Check if we can even begin a new frame
		if (!NetplayManager::instance().canBeginNextFrame(mFrameNumber))
			return false;

		// Tell game instance
		EngineMain::getDelegate().onPreFrameUpdate();

		// Tell video that we begin a new frame
		VideoOut::instance().preFrameUpdate();

		// Game recorder: Save initial frame
		if (mGameRecorder.isRecording() && mGameRecorder.getRangeEnd() == 0)
		{
			recordKeyFrame(0, *this, mGameRecorder, GameRecorder::InputData());
		}

		controlsIn.beginInputUpdate();

		// Update netplay
		NetplayManager::instance().onFrameUpdate(controlsIn, mFrameNumber);

		// If game recorder has input data for the frame transition, then use that
		//  -> This is particularly relevant for rewinds, namely for the small fast forwards from the previous keyframe
		GameRecorder::PlaybackResult result;
		if (mGameRecorder.getFrameData(mFrameNumber + 1, result))
		{
			if (mGameRecorder.isPlaying())
				LogDisplay::instance().setModeDisplay("Game recorder playback at frame: " + std::to_string(mFrameNumber + 1));

			if (nullptr != result.mData && !Configuration::instance().mGameRecorder.mPlaybackIgnoreKeys)
			{
				// Load save state
				SaveStateSerializer::StateType stateType;
				SaveStateSerializer serializer(*this, RenderParts::instance());

				const bool success = serializer.loadState(*result.mData, &stateType);
				if (success)
				{
					mCodeExec.reinitRuntime(nullptr, (stateType == SaveStateSerializer::StateType::GENSX) ? CodeExec::CallStackInitPolicy::READ_FROM_ASM : CodeExec::CallStackInitPolicy::USE_EXISTING);
					completedCurrentFrame = true;
				}
				else
				{
					RMX_ERROR("Failed to load save state", );
				}
			}

			controlsIn.injectInputs(result.mInput->mInputs);
		}

		// Input recorder playback
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			if (mInputRecorder.isPlaying())
			{
				const InputRecorder::InputState& inputState = mInputRecorder.updatePlayback(mFrameNumber);
				controlsIn.injectInputs(inputState.mInputFlags);
			}
		}

		if (s3bench::isEnabled() && s3bench::inputMode >= 0)		// (-1: the player's own input)
		{
			uint16 benchInputs[InputManager::NUM_PLAYERS] = { 0 };
			benchInputs[0] = s3bench::getInput(mFrameNumber);
			controlsIn.injectInputs(benchInputs);
		}

		// Update input state
		{
			controlsIn.endInputUpdate();

			EngineMain::getDelegate().onControlsUpdate();

			// Input state can be queried by scripts via "Input.getController" and "Input.getControllerPrevious"
		}
	}

	if (!completedCurrentFrame)		// Can be true already if game recorder playback just loaded a state
	{
		// Perform game simulation
		const auto benchT0 = std::chrono::high_resolution_clock::now();
		const size_t benchStepsBefore = mCodeExec.mBenchSteps;
		completedCurrentFrame = mCodeExec.performFrameUpdate();
		if (s3bench::isEnabled())
		{
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - benchT0).count();
			mBenchFrameMs += ms;
			mBenchFrameSteps += (mCodeExec.mBenchSteps - benchStepsBefore);
		}
	}

	// Steps to do when a frame got completed
	if (completedCurrentFrame)
	{
		// Tell game instance
		EngineMain::getDelegate().onPostFrameUpdate();

		// Tell video that we begin a new frame
		VideoOut::instance().postFrameUpdate();

		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			// Update input recording
			if (mInputRecorder.isRecording())
			{
				InputRecorder::InputState inputState;
				controlsIn.writeCurrentState(inputState.mInputFlags);

				mInputRecorder.updateRecording(inputState);
			}
		}

		// Update game recording
		if (mGameRecorder.isRecording())
		{
			if (!mGameRecorder.hasFrameNumber(mFrameNumber + 1))
			{
				GameRecorder::InputData inputData;
				controlsIn.writeCurrentState(inputData.mInputs);

				// Keyframe every 3 seconds - except when dev mode is active, because rewinding requires more frequent keyframes
				const int keyframeFrequency = EngineMain::getDelegate().useDeveloperFeatures() ? 10 : 180;
				const int framesToKeep = EngineMain::getDelegate().useDeveloperFeatures() ? 3600 : 1800;
				if (((mFrameNumber + 1) % keyframeFrequency) == 0)
				{
					recordKeyFrame(mFrameNumber + 1, *this, mGameRecorder, inputData);
					mGameRecorder.discardOldFrames(framesToKeep);
				}
				else
				{
					mGameRecorder.addFrame(mFrameNumber + 1, inputData);
				}
			}
		}
		else if (mGameRecorder.isPlaying() && EngineMain::getDelegate().useDeveloperFeatures())
		{
			// Generate a keyframe every 10 frames, to allow for quick rewinds during game recording playback as well
			const int keyframeFrequency = 10;
			if (((mFrameNumber + 1) % keyframeFrequency) == 0 && !mGameRecorder.isKeyframe(mFrameNumber + 1) && mGameRecorder.canAddFrame(mFrameNumber + 1))
			{
				GameRecorder::InputData inputData;
				controlsIn.writeCurrentState(inputData.mInputs);

				recordKeyFrame(mFrameNumber + 1, *this, mGameRecorder, inputData);
			}
		}

		if (s3bench::isEnabled())
		{
			if (!s3bench::wallStarted) { s3bench::wallStart = std::chrono::high_resolution_clock::now(); s3bench::wallStarted = true; }
			#if defined(PLATFORM_PSP)
				static const bool nativeStats = (nullptr != getenv("S3AIR_BENCH_NATIVES"));		// Slows down native calls a bit
				lemon::NativeFunction::mStatEnabled = nativeStats;
			#endif
			// (the per-frame times only for the first frames: long sessions would otherwise keep growing these lists, and memory is tight)
			++s3bench::frameCount;
			if (s3bench::frameMs.size() < 18000)
			{
				if (s3bench::frameMs.empty())
				{
					s3bench::frameMs.reserve(18000);
					s3bench::frameSteps.reserve(18000);
				}
				s3bench::frameMs.push_back((float)mBenchFrameMs);
				s3bench::frameSteps.push_back((uint32)mBenchFrameSteps);
			}
			s3bench::windowMs += mBenchFrameMs; s3bench::windowMax = std::max(s3bench::windowMax, mBenchFrameMs); s3bench::windowSteps += mBenchFrameSteps; ++s3bench::windowCount;
			{
				// Slow frames: how much of it was building script functions on their first call
				static uint32 lastBuildCount = 0;
				static double lastBuildMs = 0.0;
				const uint32 builds = lemon::RuntimeFunction::mStatBuildCount - lastBuildCount;
				const double buildMs = lemon::RuntimeFunction::mStatBuildMilliseconds - lastBuildMs;
				lastBuildCount = lemon::RuntimeFunction::mStatBuildCount;
				lastBuildMs = lemon::RuntimeFunction::mStatBuildMilliseconds;
			#if defined(PLATFORM_PSP)
				// File access on the memory stick in this frame: opening audio files, loading sound effects, reading mod sprites
				static uint64 lastOpenUs = 0, lastSoundUs = 0;
				static uint32 lastSpriteMs = 0;
				const double openMs = (PSPAudioBank::gStatTotalFileOpenUs - lastOpenUs) / 1000.0;
				const double soundMs = (PSPAudioBank::gStatTotalSoundLoadUs - lastSoundUs) / 1000.0;
				const uint32 spriteMs = SpriteCollection::instance().getSpriteCacheIoMilliseconds() - lastSpriteMs;
				lastOpenUs = PSPAudioBank::gStatTotalFileOpenUs;
				lastSoundUs = PSPAudioBank::gStatTotalSoundLoadUs;
				lastSpriteMs = SpriteCollection::instance().getSpriteCacheIoMilliseconds();
			#endif
				if (mBenchFrameMs >= 20.0)
				{
				#if defined(PLATFORM_PSP)
					fprintf(s3bench::logFile, "slow frame %6u script_ms=%.2f steps=%llu function_builds=%u build_ms=%.2f io_audio_open_ms=%.1f io_sound_load_ms=%.1f io_sprite_ms=%u\n", mFrameNumber, mBenchFrameMs, (unsigned long long)mBenchFrameSteps, builds, buildMs, openMs, soundMs, spriteMs);
				#else
					fprintf(s3bench::logFile, "slow frame %6u script_ms=%.2f steps=%llu function_builds=%u build_ms=%.2f\n", mFrameNumber, mBenchFrameMs, (unsigned long long)mBenchFrameSteps, builds, buildMs);
				#endif

					// With the profiler: the functions that took most time in this frame
					std::vector<std::pair<size_t, std::pair<const lemon::RuntimeFunction*, const lemon::RuntimeFunction*>>> sorted;
					for (const auto& [key, microseconds] : mCodeExec.mBenchFrameProfile)
						sorted.emplace_back(microseconds, key);
					std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
					auto getName = [](const lemon::RuntimeFunction* function) { return (nullptr != function && nullptr != function->mFunction) ? function->mFunction->getName().getString() : std::string_view("-"); };
					for (size_t i = 0; i < std::min<size_t>(sorted.size(), 5); ++i)
					{
						const std::string_view name = getName(sorted[i].second.first);
						const std::string_view caller = getName(sorted[i].second.second);
						fprintf(s3bench::logFile, "    %6.2f ms  %.*s  (from %.*s)\n", sorted[i].first / 1000.0, (int)name.size(), name.data(), (int)caller.size(), caller.data());
					}
				}
				mCodeExec.mBenchFrameProfile.clear();
				static const bool profile = (nullptr != getenv("S3AIR_BENCH_PROFILE"));
				mCodeExec.mBenchProfileEnabled = profile;
				if (profile && (mFrameNumber + 1) % 300 == 0)
				{
					// Profiler: the script functions that took most time in the last 300 frames (time of the native functions they call included)
					std::vector<std::pair<size_t, std::pair<const lemon::RuntimeFunction*, const lemon::RuntimeFunction*>>> sorted;
					size_t total = 0;
					for (const auto& [key, microseconds] : mCodeExec.mBenchProfile)
					{
						sorted.emplace_back(microseconds, key);
						total += microseconds;
					}
					std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
					auto getName = [](const lemon::RuntimeFunction* function) { return (nullptr != function && nullptr != function->mFunction) ? function->mFunction->getName().getString() : std::string_view("-"); };
					printf("[prof] frame %u: %.2f ms per frame in scripts\n", mFrameNumber, total / 300000.0);
					fprintf(s3bench::logFile, "  profile: %.2f ms per frame in scripts\n", total / 300000.0);
					for (size_t i = 0; i < std::min<size_t>(sorted.size(), 12); ++i)
					{
						const std::string_view name = getName(sorted[i].second.first);
						const std::string_view caller = getName(sorted[i].second.second);
						printf("[prof]   %6.3f ms  %.*s  (from %.*s)\n", sorted[i].first / 300000.0, (int)name.size(), name.data(), (int)caller.size(), caller.data());
						fprintf(s3bench::logFile, "    %6.3f ms  %.*s  (from %.*s)\n", sorted[i].first / 300000.0, (int)name.size(), name.data(), (int)caller.size(), caller.data());
					}
					mCodeExec.mBenchProfile.clear();
				}
			}
			if (s3bench::windowCount == 60)
			{
				// (every 5th window, i.e. 300 frames: the frame number isn't aligned with the windows)
				static uint32 windowNumber = 0;
				++windowNumber;
				// Audio: mixing on the audio thread as a share of the playback time, and the main thread's audio update per frame
				const double mixedSeconds = (double)rmx::AudioManager::mStatMixSamples / 44100.0;
				const double mixPercent = (mixedSeconds > 0.0) ? (rmx::AudioManager::mStatMixMs / (mixedSeconds * 1000.0) * 100.0) : 0.0;
				fprintf(s3bench::logFile, "frame %6u avg_ms=%.4f max_ms=%.4f avg_steps=%llu heap_MB=%.2f audio_mix=%.1f%% audio_update_ms=%.3f cpu_mhz=%d bus_mhz=%d opcodes_nat/opt/def=%u/%u/%u\n", mFrameNumber, s3bench::windowMs / 60, s3bench::windowMax, (unsigned long long)(s3bench::windowSteps / 60), s3bench::heapBytes() / 1048576.0,
						mixPercent, AudioPlayer::mStatUpdateMs / 60,
					#if defined(__PSP__)
						scePowerGetCpuClockFrequencyInt(), scePowerGetBusClockFrequencyInt(),
					#else
						0, 0,
					#endif
						lemon::RuntimeFunction::mStatOpcodesNativized, lemon::RuntimeFunction::mStatOpcodesOptimized, lemon::RuntimeFunction::mStatOpcodesDefault);
			#if defined(PLATFORM_PSP)
				{
					// File access on the memory stick in this window: audio file opens, sound effect loads, sprite pack reads
					static uint32 lastSpriteReads = 0;
					static uint32 lastSpriteMs = 0;
					const SpriteCollection& spriteCollection = SpriteCollection::instance();
					const uint32 spriteReads = spriteCollection.getSpriteCacheReads() - lastSpriteReads;
					const uint32 spriteMs = spriteCollection.getSpriteCacheIoMilliseconds() - lastSpriteMs;
					lastSpriteReads = spriteCollection.getSpriteCacheReads();
					lastSpriteMs = spriteCollection.getSpriteCacheIoMilliseconds();
					if (PSPAudioBank::gStatFileOpens + PSPAudioBank::gStatSoundLoads + spriteReads > 0)
					{
						fprintf(s3bench::logFile, "  io: audio file opens %u (%.1f ms, max %.1f), sound loads %u (%.1f ms, max %.1f), sprite reads %u (%u ms)\n",
								PSPAudioBank::gStatFileOpens, PSPAudioBank::gStatFileOpenUs / 1000.0, PSPAudioBank::gStatFileOpenMaxUs / 1000.0,
								PSPAudioBank::gStatSoundLoads, PSPAudioBank::gStatSoundLoadUs / 1000.0, PSPAudioBank::gStatSoundLoadMaxUs / 1000.0, spriteReads, spriteMs);
					}
					PSPAudioBank::gStatFileOpens = PSPAudioBank::gStatFileOpenUs = PSPAudioBank::gStatFileOpenMaxUs = 0;
					PSPAudioBank::gStatSoundLoads = PSPAudioBank::gStatSoundLoadUs = PSPAudioBank::gStatSoundLoadMaxUs = 0;
				}
			#endif
				rmx::AudioManager::mStatMixMs = 0.0;
				rmx::AudioManager::mStatMixSamples = 0;
				AudioPlayer::mStatUpdateMs = 0.0;
				if (windowNumber % 5 == 0)
				{
					// Memory breakdown: the sprite cache by source, audio
					size_t bytes[3] = { 0, 0, 0 };		// Unknown, sprite files, ROM data
					size_t counts[3] = { 0, 0, 0 };
					size_t upscaledBytes = 0;
					for (const auto& [key, item] : SpriteCollection::instance().getAllSprites())
					{
						if (nullptr == item.mSprite)
							continue;
						const int type = std::min((int)item.mSourceInfo.mType, 2);
						if (item.mUsesComponentSprite)
						{
							bytes[type] += (size_t)static_cast<ComponentSprite*>(item.mSprite)->getBitmap().getPixelCount() * 4;
						}
						else
						{
							const PaletteSprite& sprite = *static_cast<PaletteSprite*>(item.mSprite);
							bytes[type] += (size_t)sprite.getBitmap().getPixelCount();
							upscaledBytes += (size_t)sprite.getUpscaledBitmapIfPresent().getPixelCount();
						}
						++counts[type];
					}
					fprintf(s3bench::logFile, "  memory: sprites from files %u (%.2f MB), from ROM %u (%.2f MB), other %u (%.2f MB), upscaled %.2f MB, audio %.2f MB\n",
							(uint32)counts[1], bytes[1] / 1048576.0, (uint32)counts[2], bytes[2] / 1048576.0, (uint32)counts[0], bytes[0] / 1048576.0, upscaledBytes / 1048576.0,
							EngineMain::instance().getAudioOut().getAudioPlayer().getMemoryUsage() / 1048576.0);
				#if defined(PLATFORM_PSP)
					{
						const SpriteCollection& spriteCollection = SpriteCollection::instance();
						if (spriteCollection.getSpriteCacheLoads() > 0)
							fprintf(s3bench::logFile, "  mod sprite cache: %u KB of %u KB, %u loads (%u read ahead), %u freed, %u reads, %u pack opens, %u ms reading\n", (uint32)(spriteCollection.getSpriteCacheBytes() / 1024),
									(uint32)(spriteCollection.getSpriteCacheBudget() / 1024), spriteCollection.getSpriteCacheLoads(), spriteCollection.getSpriteCachePrefetched(), spriteCollection.getSpriteCacheEvictions(),
									spriteCollection.getSpriteCacheReads(), spriteCollection.getSpriteCacheOpens(), spriteCollection.getSpriteCacheIoMilliseconds());
						fprintf(s3bench::logFile, "  script functions: %u KB built, %u builds, %u freed\n", (uint32)(lemon::RuntimeFunction::mStatBuiltBytes / 1024),
								lemon::RuntimeFunction::mStatBuildCount, lemon::RuntimeFunction::mStatFreedCount);
					}
				#endif
					// (also on the console, to follow it live through PSPLink)
					printf("[mem] frame %u: heap %.2f MB (arena %.2f MB), ROM sprites %.2f MB, upscaled %.2f MB, audio %.2f MB:%s\n", mFrameNumber, s3bench::heapBytes() / 1048576.0, s3bench::heapArenaBytes() / 1048576.0,
						   bytes[2] / 1048576.0, upscaledBytes / 1048576.0, EngineMain::instance().getAudioOut().getAudioPlayer().getMemoryUsage() / 1048576.0,
						   EngineMain::instance().getAudioOut().getAudioPlayer().getMemoryReport(3).c_str());
				}
			#if defined(PLATFORM_PSP)
				{
					// Native functions called by the scripts: total time per frame, and every 300 frames the most expensive ones
					auto& functions = lemon::NativeFunction::mStatCalledFunctions;
					uint32 totalMicroseconds = 0;
					uint32 totalCalls = 0;
					for (const lemon::NativeFunction* f : functions)
					{
						totalMicroseconds += f->mStatMicroseconds;
						totalCalls += f->mStatCalls;
					}
					if (!functions.empty())
						fprintf(s3bench::logFile, "  natives_ms=%.3f calls=%u per frame\n", totalMicroseconds / 60000.0, totalCalls / 60);
					if (windowNumber % 5 == 0)
					{
						std::sort(functions.begin(), functions.end(), [](const lemon::NativeFunction* a, const lemon::NativeFunction* b) { return a->mStatMicroseconds > b->mStatMicroseconds; });
						for (size_t i = 0; i < std::min<size_t>(functions.size(), 12); ++i)
							fprintf(s3bench::logFile, "    %-40s %8.3f ms/frame %7.1f calls/frame\n", functions[i]->getName().getString().data(), functions[i]->mStatMicroseconds / 60000.0, functions[i]->mStatCalls / 60.0);
					}
					for (const lemon::NativeFunction* f : functions)
					{
						f->mStatMicroseconds = 0;
						f->mStatCalls = 0;
					}
				}
			#endif
				fflush(s3bench::logFile);
				s3bench::windowMs = 0; s3bench::windowMax = 0; s3bench::windowSteps = 0; s3bench::windowCount = 0;
			}
			mBenchFrameMs = 0; mBenchFrameSteps = 0;
			if ((int)s3bench::frameCount >= s3bench::maxFrames)
				s3bench::finish();
		}

		++mFrameNumber;
	#if defined(PLATFORM_PSP)
		pspge::gSimulationFrame = mFrameNumber;
		// The script functions built (each when first called) would otherwise keep growing, by several MB in a long session
		//  -> S3AIR_SCRIPT_BUDGET_KB (debugging, e.g. in bench.txt): another budget, a small one to test freeing and rebuilding
		static const size_t scriptFunctionsBudget = []() { const char* e = getenv("S3AIR_SCRIPT_BUDGET_KB"); return (e && atoi(e) > 0) ? (size_t)atoi(e) * 1024 : (size_t)1536 * 1024; }();
		mCodeExec.getLemonScriptRuntime().getInternalLemonRuntime().trimRuntimeFunctions(scriptFunctionsBudget);
	#endif

		if (mStepsLimit > 0)
			--mStepsLimit;
	}

	// Return false if frame got interrupted
	//  -> In this case, the outer loop should break
	//  -> Same if code execution is not possible any more
	return (completedCurrentFrame && mCodeExec.isCodeExecutionPossible());
}

bool Simulation::jumpToFrame(uint32 frameNumber, bool clearRecordingAfterwards)
{
	if (mGameRecorder.isRecording() || mGameRecorder.isPlaying())
	{
		if (mGameRecorder.isPlaying())
			clearRecordingAfterwards = false;

		// Go back until the most recent keyframe, in case the selected frame is not a keyframe itself
		GameRecorder::PlaybackResult result;
		uint32 keyframeNumber = frameNumber;
		if (!mGameRecorder.getFrameData(keyframeNumber, result))
			return false;

		while (nullptr == result.mData)
		{
			if (keyframeNumber == 0)
				return false;
			--keyframeNumber;
			if (!mGameRecorder.getFrameData(keyframeNumber, result))
				return false;
		}

		SaveStateSerializer::StateType stateType;
		SaveStateSerializer serializer(*this, RenderParts::instance());

		const bool success = serializer.loadState(*result.mData, &stateType);
		if (success)
		{
			// Inject inputs for this frame, so that previous input will be set correctly in the next frame
			ControlsIn::instance().injectInputs(result.mInput->mInputs);

			mCodeExec.reinitRuntime(nullptr, (stateType == SaveStateSerializer::StateType::GENSX) ? CodeExec::CallStackInitPolicy::READ_FROM_ASM : CodeExec::CallStackInitPolicy::USE_EXISTING);
			mFrameNumber = keyframeNumber;
			mCurrentTargetFrame = (float)frameNumber;

			if (clearRecordingAfterwards)
			{
				// Discard later frames to disable the logic that uses their recorded inputs instead of player input
				mGameRecorder.discardFramesAfter(frameNumber);
			}
			return true;
		}
	}

	return false;
}

int Simulation::setRewind(int rewindSteps)
{
	mRewindSteps = rewindSteps;
	if (mGameRecorder.isRecording())
		mRewindSteps = std::min<int>(mFrameNumber - mGameRecorder.getRangeStart(), mRewindSteps);
	return mRewindSteps;
}

float Simulation::getSimulationFrequency() const
{
	return (mSimulationFrequencyOverride > 0.0f) ? mSimulationFrequencyOverride : (float)Configuration::instance().mSimulationFrequency;
}

void Simulation::setSpeed(float emulatorSpeed)
{
	mSimulationSpeed = emulatorSpeed;
	mStepsLimit = -1;
	mBreakConditions.clearAll();
}

void Simulation::setNextSingleStep()
{
	mStepsLimit = 1;
}

void Simulation::removeStepsLimit()
{
	mStepsLimit = -1;
}

void Simulation::setBreakCondition(BreakCondition breakCondition, bool enable)
{
	mBreakConditions.set(breakCondition, enable);
}

void Simulation::sendBreakSignal(BreakCondition breakCondition)
{
	if (mBreakConditions.isSet(breakCondition))
	{
		mStepsLimit = 0;
	}
}

void Simulation::refreshDebugging()
{
	VideoOut::instance().preRefreshDebugging();
	mCodeExec.executeScriptFunction("OxygenCallback.setupCustomSidePanelEntries", false);
	VideoOut::instance().postRefreshDebugging();
}

uint32 Simulation::saveGameRecording(WString* outFilename)
{
	std::wstring filename = L"gamerecording.bin";
	const std::string timeString = PlatformFunctions::getCompactSystemTimeString();
	if (!timeString.empty())
	{
		filename = L"gamerecording_" + String(timeString).toStdWString() + L".bin";
	}
	filename = Configuration::instance().mGameAppDataPath + L"gamerecordings/" + filename;

	if (!mGameRecorder.saveRecording(filename, 180))
		return 0;

	if (nullptr != outFilename)
		*outFilename = filename;

	return mGameRecorder.getCurrentNumberOfFrames();
}

void Simulation::applyModSettingsToGlobals()
{
	// Apply mod settings
	for (Mod* mod : ModManager::instance().getActiveMods())
	{
		for (Mod::SettingCategory& modSettingCategory : mod->mSettingCategories)
		{
			for (Mod::Setting& modSetting : modSettingCategory.mSettings)
			{
				mCodeExec.getLemonScriptRuntime().setGlobalVariableValue<int64>(modSetting.mBinding, modSetting.mCurrentValue);
			}
		}
	}
}
