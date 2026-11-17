/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/Game.h"
#include "sonic3air/ConfigurationImpl.h"
#include "sonic3air/audio/AudioOut.h"
#include "sonic3air/data/SharedDatabase.h"
#include "sonic3air/data/TimeAttackData.h"
#include "sonic3air/helper/GameUtils.h"
#include "sonic3air/helper/DiscordIntegration.h"
#include "sonic3air/menu/GameApp.h"
#include "sonic3air/menu/SharedResources.h"
#include "sonic3air/scriptimpl/ScriptImplementations.h"

#include "oxygen/application/Application.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/gameview/GameView.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/input/InputManager.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/drawing/software/Blitter.h"
#include "oxygen/engine/modding/ModManager.h"
#include "oxygen/helper/FileHelper.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/resources/SpriteCollection.h"
#include "oxygen/simulation/CodeExec.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/Simulation.h"

#include <lemon/program/function/FunctionWrapper.h>
#include <lemon/program/Module.h>
#include <lemon/program/ModuleBindingsBuilder.h>


namespace
{
	const constexpr float CUTSCENE_SKIPPING_SPEED = 4.0f;	// Should be more or less "unique", not one of the debug game speeds (3.0f or 5.0f)

	void setDiscordDetails(lemon::StringRef text)
	{
		if (text.isValid())
			DiscordIntegration::setModdedDetails(text.getString());
	}

	void setDiscordState(lemon::StringRef text)
	{
		if (text.isValid())
			DiscordIntegration::setModdedState(text.getString());
	}

	void setDiscordLargeImage(lemon::StringRef imageName)
	{
		if (imageName.isValid())
			DiscordIntegration::setModdedLargeImage(imageName.getString());
	}

	void setDiscordSmallImage(lemon::StringRef imageName)
	{
		if (imageName.isValid())
			DiscordIntegration::setModdedSmallImage(imageName.getString());
	}

	void setUnderwaterAudioEffect(uint8 value)
	{
		AudioOut::instance().enableUnderwaterEffect((float)value / 255.0f);
	}
}


Game::Game()
{
}

void Game::startup(EmulatorInterface& emulatorInterface)
{
	mEmulatorInterface = &emulatorInterface;
	mPlayerRecorder.setEmulatorInterface(emulatorInterface);

	mPlayerProgress.load();
	mBlueSpheresRendering.startup();
	mGameClient.setupClient();
	checkActiveModsUsedFeatures();

	DiscordIntegration::startup();
}

void Game::shutdown()
{
	mPlayerProgress.save();

	DiscordIntegration::shutdown();
}

void Game::update(float timeElapsed)
{
	// Update game client
	mGameClient.updateClient(timeElapsed);

	// Update sprite redirects (like input icons)
	mDynamicSprites.updateSpriteRedirects();

	// Discord rich presence update
	{
		mTimeoutUntilDiscordRefresh -= timeElapsed;
		if (mTimeoutUntilDiscordRefresh <= 0.0f)
		{
			DiscordIntegration::updateInfo(mMode, mSubMode, *mEmulatorInterface);
			mTimeoutUntilDiscordRefresh = 3.0f;
		}
		DiscordIntegration::update();
	}

	if (EngineMain::getDelegate().useDeveloperFeatures() && (FTX::keyState(SDLK_LALT) || FTX::keyState(SDLK_RALT)))
	{
		// Alt + X (and maybe Shift as well) for testing unlock windows
		static bool pressed = false;
		if (FTX::keyState('x') != pressed)
		{
			pressed = !pressed;
			if (pressed)
			{
				if (FTX::keyState(SDLK_LSHIFT))
					GameApp::instance().showUnlockedWindow(SecretUnlockedWindow::EntryType::SECRET, "Secret unlocked!", "Knuckles & Tails character combination can now be selected in Normal Game and Act Select.");
				else
					GameApp::instance().showUnlockedWindow(SecretUnlockedWindow::EntryType::ACHIEVEMENT, "Achievement unlocked!", "Cheated an achievement. Well done!");
			}
		}
	}

	if (mReturnToMenuTriggered)
	{
		Application::instance().getSimulation().setSpeed(0.0f);
		GameApp::instance().returnToMenu();
		mReturnToMenuTriggered = false;
	}
	else if (mRestartTriggered)
	{
		startIntoLevel(mMode, mSubMode, mLastZoneAndAct, mLastCharacters);
		mRestartTriggered = false;
	}
}

void Game::registerScriptBindings(lemon::Module& module)
{
	lemon::ModuleBindingsBuilder builder(module);

	const BitFlagSet<lemon::Function::Flag> defaultFlags(lemon::Function::Flag::ALLOW_INLINE_EXECUTION);
	const BitFlagSet<lemon::Function::Flag> noInlineExecution;

	// Game
	{
		builder.addNativeFunction("Game.getSetting", lemon::wrap(*this, &Game::useSetting), defaultFlags)
			.setParameters("settingId");

		builder.addNativeFunction("Game.isSecretUnlocked", lemon::wrap(*this, &Game::isSecretUnlocked), defaultFlags)
			.setParameters("secretId");

		builder.addNativeFunction("Game.setSecretUnlocked", lemon::wrap(*this, &Game::setSecretUnlocked), defaultFlags)
			.setParameters("secretId");

		builder.addNativeFunction("Game.triggerRestart", lemon::wrap(*this, &Game::triggerRestart), defaultFlags);

		builder.addNativeFunction("Game.onGamePause", lemon::wrap(*this, &Game::onGamePause), defaultFlags)
			.setParameters("canRestart");

		builder.addNativeFunction("Game.allowRestartInGamePause", lemon::wrap(*this, &Game::allowRestartInGamePause), defaultFlags)
			.setParameters("canRestart");

		builder.addNativeFunction("Game.onLevelStart", lemon::wrap(*this, &Game::onLevelStart), defaultFlags);

		builder.addNativeFunction("Game.onZoneActCompleted", lemon::wrap(*this, &Game::onZoneActCompleted), defaultFlags)
			.setParameters("zoneAndAct");

		builder.addNativeFunction("Game.onTriggerNextZone", lemon::wrap(*this, &Game::onTriggerNextZone), defaultFlags)
			.setParameters("zoneAndAct");

		builder.addNativeFunction("Game.onFadedOutLoadingZone", lemon::wrap(*this, &Game::onFadedOutLoadingZone), defaultFlags)
			.setParameters("zoneAndAct");

		builder.addNativeFunction("Game.onCharacterDied", lemon::wrap(*this, &Game::onCharacterDied), noInlineExecution)		// No inline execution as this function manipulated the call stack
			.setParameters("playerIndex");

		builder.addNativeFunction("Game.returnToMainMenu", lemon::wrap(*this, &Game::returnToMainMenu), defaultFlags);
		builder.addNativeFunction("Game.openOptionsMenu", lemon::wrap(*this, &Game::openOptionsMenu), defaultFlags);

		builder.addNativeFunction("Game.isNormalGame", lemon::wrap(*this, &Game::isNormalGame), defaultFlags);
		builder.addNativeFunction("Game.isTimeAttack", lemon::wrap(*this, &Game::isTimeAttack), defaultFlags);
		builder.addNativeFunction("Game.onTimeAttackFinish", lemon::wrap(*this, &Game::onTimeAttackFinish), defaultFlags);

		builder.addNativeFunction("Game.changePlanePatternRectAtex", lemon::wrap(*this, &Game::changePlanePatternRectAtex), defaultFlags)
			.setParameters("px", "py", "width", "height", "planeIndex", "atex");

		builder.addNativeFunction("Game.setupBlueSpheresGroundSprites", lemon::wrap(*this, &Game::setupBlueSpheresGroundSprites), defaultFlags);

		builder.addNativeFunction("Game.writeBlueSpheresData", lemon::wrap(*this, &Game::writeBlueSpheresData), defaultFlags)
			.setParameters("targetAddress", "sourceAddress", "px", "py", "rotation");

		builder.addNativeFunction("Game.renderBlueSpheresCustomSpheres", lemon::wrap(*this, &Game::renderBlueSpheresCustomSpheres), defaultFlags);

		builder.addNativeFunction("Game.getAchievementValue", lemon::wrap(*this, &Game::getAchievementValue), defaultFlags)
			.setParameters("achievementId");

		builder.addNativeFunction("Game.setAchievementValue", lemon::wrap(*this, &Game::setAchievementValue), defaultFlags)
			.setParameters("achievementId", "value");

		builder.addNativeFunction("Game.isAchievementComplete", lemon::wrap(*this, &Game::isAchievementComplete), defaultFlags)
			.setParameters("achievementId");

		builder.addNativeFunction("Game.setAchievementComplete", lemon::wrap(*this, &Game::setAchievementComplete), defaultFlags)
			.setParameters("achievementId");

		builder.addNativeFunction("Game.startSkippableCutscene", lemon::wrap(*this, &Game::startSkippableCutscene), defaultFlags);
		builder.addNativeFunction("Game.endSkippableCutscene", lemon::wrap(*this, &Game::endSkippableCutscene), defaultFlags);
		builder.addNativeFunction("Game.isInSkippableCutscene", lemon::wrap(*this, &Game::isInSkippableCutscene), defaultFlags);
	}

	// Discord
	{
		builder.addNativeFunction("Game.setDiscordDetails", lemon::wrap(&setDiscordDetails), defaultFlags)
			.setParameters("text");

		builder.addNativeFunction("Game.setDiscordState", lemon::wrap(&setDiscordState), defaultFlags)
			.setParameters("text");

		builder.addNativeFunction("Game.setDiscordLargeImage", lemon::wrap(&setDiscordLargeImage), defaultFlags)
			.setParameters("imageName");

		builder.addNativeFunction("Game.setDiscordSmallImage", lemon::wrap(&setDiscordSmallImage), defaultFlags)
			.setParameters("imageName");
	}

	// Audio
	{
		builder.addNativeFunction("Game.setUnderwaterAudioEffect", lemon::wrap(&setUnderwaterAudioEffect), defaultFlags)
			.setParameters("value");
	}

	ScriptImplementations::registerScriptBindings(module);
}

uint32 Game::getSetting(uint32 settingId, bool ignoreGameMode) const
{
	const SharedDatabase::Setting* setting = SharedDatabase::getSetting(settingId);

	if (!ignoreGameMode && isInTimeAttackMode())
	{
		// In Time Attack, non-visual settings are always using the default value
		if ((settingId & 0x80000000) == 0)
		{
			const bool explicitlyAllowInTimeAttack = (nullptr != setting && setting->mAllowInTimeAttack);
			if (!explicitlyAllowInTimeAttack)
			{
				// Only exception are the "max control" settings
				if (settingId == SharedDatabase::Setting::SETTING_DROPDASH || settingId == SharedDatabase::Setting::SETTING_SUPER_PEELOUT)
				{
					return (mSubMode == 0x11) ? 1 : 0;
				}
				else
				{
					return (settingId & 0xff);
				}
			}
		}
	}

	if (nullptr != setting)
	{
		const uint32 value = ConfigurationImpl::instance().mActiveGameSettings->getValue(settingId);

		// Special handling for Debug Mode setting in dev mode
		if (settingId == SharedDatabase::Setting::SETTING_DEBUG_MODE)
		{
			if (value == 0)
			{
				if (EngineMain::getDelegate().useDeveloperFeatures() && ConfigurationImpl::instance().mDevModeImpl.mEnforceDebugMode)
					return true;
			}
		}
		return value;
	}
	else
	{
		// Use default value
		return (settingId & 0xff);
	}
}

void Game::setSetting(uint32 settingId, uint32 value)
{
	RMX_CHECK(nullptr != SharedDatabase::getSetting(settingId), "Setting not found", return);
	ConfigurationImpl::instance().mActiveGameSettings->setValue(settingId, value);
}

void Game::checkForUnlockedSecrets(bool saveIfAnyUnlocked)
{
	// Check for unlocked secrets
	uint32 achievementsCompleted = 0;
	for (const SharedDatabase::Achievement& achievement : SharedDatabase::getAchievements())
	{
		if (mPlayerProgress.mAchievements.getAchievementState(achievement.mType) > 0)
		{
			++achievementsCompleted;
		}
	}

	bool anyUnlocked = false;
	for (const SharedDatabase::Secret& secret : SharedDatabase::getSecrets())
	{
		if (secret.mUnlockedByAchievements && !mPlayerProgress.mUnlocks.isSecretUnlocked(secret.mType) && achievementsCompleted >= secret.mRequiredAchievements)
		{
			// Unlock secret now
			mPlayerProgress.mUnlocks.setSecretUnlocked(secret.mType);
			GameApp::instance().showUnlockedWindow(SecretUnlockedWindow::EntryType::SECRET, "Secret unlocked!", secret.mName);
			anyUnlocked = true;
		}
	}

	if (anyUnlocked && saveIfAnyUnlocked)
	{
		mPlayerProgress.save();
	}
}

void Game::unlockSecret(uint32 secretId)
{
	setSecretUnlocked(secretId);
}

void Game::startIntoTitleScreen()
{
	mMode = Mode::TITLE_SCREEN;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetState();

	startIntoGameInternal();
}

void Game::startIntoDataSelect()
{
	mMode = Mode::NORMAL_GAME;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.dataSelect");

	startIntoGameInternal();
}

void Game::startIntoActSelect()
{
	mMode = Mode::ACT_SELECT;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.actSelectMenu");

	startIntoGameInternal();
}

void Game::startIntoLevel(Mode mode, uint32 submode, uint16 zoneAndAct, uint8 characters)
{
	mMode = mode;
	mSubMode = submode;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.actSelect");

	startIntoGameInternal();
	mLastZoneAndAct = zoneAndAct;
	mLastCharacters = characters;

	mEmulatorInterface->writeMemory16(0xfffffe10, zoneAndAct);
	mEmulatorInterface->writeMemory16(0xffffff0a, characters);

	if (mMode == Mode::TIME_ATTACK)
	{
		mPlayerRecorder.setMaxGhosts(getSetting(SharedDatabase::Setting::SETTING_TIME_ATTACK_GHOSTS, true));

		const uint8 category = submode;
		std::wstring recBaseFilename;
		const std::wstring recordingsDir = TimeAttackData::getSavePath(zoneAndAct, category, &recBaseFilename);

		if (!recordingsDir.empty())
		{
			mPlayerRecorder.initForDirectory(recordingsDir, recBaseFilename, zoneAndAct, category);
		}
	}

	GameApp::instance().enableStillImageBlur(false);
}

void Game::restartLevel()
{
	Simulation& simulation = Application::instance().getSimulation();
	simulation.getCodeExec().getLemonScriptRuntime().callFunctionByName("restartLevel");
	simulation.setSpeed(simulation.getDefaultSpeed());		// Because it could be paused
}

void Game::restartAtCheckpoint()
{
	Simulation& simulation = Application::instance().getSimulation();
	simulation.getCodeExec().getLemonScriptRuntime().callFunctionByName("restartAtCheckpoint");
	simulation.setSpeed(simulation.getDefaultSpeed());		// Because it could be paused
}

void Game::restartTimeAttack(bool skipFadeout)
{
	if (skipFadeout)
	{
		startIntoLevel(mMode, mSubMode, mLastZoneAndAct, mLastCharacters);
	}
	else
	{
		if (!mRestartingTimeAttack)
		{
			mRestartingTimeAttack = true;
			Simulation& simulation = Application::instance().getSimulation();
			simulation.getCodeExec().getLemonScriptRuntime().callFunctionByName("restartTimeAttack");
			simulation.setSpeed(simulation.getDefaultSpeed());		// Because it could be paused
		}
	}
}

void Game::startIntoCompetitionMode()
{
	mMode = Mode::COMPETITION;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.competitionMode");

	startIntoGameInternal();
}

void Game::startIntoBlueSphere()
{
	mMode = Mode::BLUE_SPHERE;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.blueSphereGame");

	startIntoGameInternal();
}

void Game::startIntoLevelSelect()
{
	mMode = Mode::ACT_SELECT;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.levelSelect");

	startIntoGameInternal();
}

void Game::startIntoMainMenuBG()
{
	mMode = Mode::MAIN_MENU_BG;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.resetIntoGame("EntryFunctions.mainMenuBG");

	startIntoGameInternal();
}

void Game::onPreUpdateFrame()
{
#if defined(PLATFORM_PSP)
	// PSP: free the special stage's ground sprites (~4.4 MB) once back in a level (game mode 0x0c), they get built again for the next one
	if (mBlueSpheresRendering.hasSprites() && nullptr != mEmulatorInterface && (mEmulatorInterface->readMemory8(0xfffff600) & 0x7f) == 0x0c)
	{
		mBlueSpheresRendering.releaseSprites();
		RMX_LOG_INFO("PSP: special stage ground sprites released");
	}
#endif
}

void Game::onPostUpdateFrame()
{
#if defined(PLATFORM_PSP)
	// PSP: sound effects and the mods' sprites may use more memory in a level (heap use stays ~10 MB below the heap's size
	// there), not elsewhere: the special stages need that memory, and their ground sprites get built when they're drawn first,
	// just after this (the sprite cache gets trimmed at the start of drawing, see GameView)
	//  -> Sound effects played again and again in a level (spin dash, peel-out...) and the characters' sprites from mods
	//     then don't get loaded again from the memory stick each time, a frame drop each
	if (nullptr != mEmulatorInterface)
	{
		const bool inLevel = ((mEmulatorInterface->readMemory8(0xfffff600) & 0x7f) == 0x0c && !mBlueSpheresRendering.hasSprites());
		AudioOut::instance().getAudioPlayer().setPSPSoundBudget((inLevel ? 2560 : 1536) * 1024);
		SpriteCollection::instance().setSpriteCacheExtra(inLevel ? 1024 * 1024 : 0);
	}
#endif

	EmulatorInterface& emulatorInterface = *mEmulatorInterface;

	// If code execution stopped, return to the main menu
	if (!Application::instance().getSimulation().getCodeExec().isCodeExecutionPossible())
	{
		if (mMode != Mode::UNDEFINED && mMode != Mode::MAIN_MENU_BG)
		{
			GameApp::instance().returnToMenu();
		}
	}

	// Check for invalid game mode inside simulation
	{
		const uint8 gameMode = emulatorInterface.readMemory8(0xfffff600) & 0x7f;
		bool isValidGameMode = true;
		if (isInTimeAttackMode())
		{
			// Only allowed is the main game
			isValidGameMode = (gameMode <= 0x0c);
		}

		if (!isValidGameMode)
		{
			resetCurrentMode();
			Application::instance().getSimulation().setSpeed(0.0f);
			GameApp::instance().returnToMenu();
			return;
		}
	}

	// Update skippable cutscene
	if (mSkippableCutsceneFrames > 0)
	{
		--mSkippableCutsceneFrames;
		if (mSkippableCutsceneFrames == 0)
		{
			endSkippableCutscene();
		}
	#if !defined(PLATFORM_PSP)	// PSP: no fast forwarding of cutscenes, the PSP can't run the game any faster (see onUpdateControls)
		else
		{
			Simulation& simulation = Application::instance().getSimulation();
			if (mButtonYPressedDuringSkippableCutscene)
			{
				if (simulation.getSpeed() == 1.0f)
				{
					simulation.setSpeed(CUTSCENE_SKIPPING_SPEED);
					GameApp::instance().showSkippableCutsceneWindow(true);
				}
			}
			else
			{
				if (simulation.getSpeed() == CUTSCENE_SKIPPING_SPEED)
				{
					simulation.setSpeed(simulation.getDefaultSpeed());
				}
				GameApp::instance().showSkippableCutsceneWindow(false);
			}
		}
	#endif
	}

	// Update player recorder
	mPlayerRecorder.onPostUpdateFrame();

	// Update ghost sync
	GameClient::instance().getGhostSync().onPostUpdateFrame();

	if (mReceivedTimeAttackFinished)
	{
		int hundreds = 0;
		std::vector<int> otherTimes;
		if (getPlayerRecorder().onTimeAttackFinish(hundreds, otherTimes))
		{
			GameApp::instance().showTimeAttackResults(hundreds, otherTimes);
		}
	}
}

void Game::onUpdateControls()
{
#if defined(PLATFORM_PSP)
	// PSP: no fast forwarding of cutscenes, so holding the button doesn't block the game's input either
	return;
#endif
	if (mSkippableCutsceneFrames > 0 || mButtonYPressedDuringSkippableCutscene)	// Last check makes sure we'll ignore the press until it gets released
	{
		// Block input to the game
		mButtonYPressedDuringSkippableCutscene = ControlsIn::instance().getGamepad(0).isPressed(ControlsIn::Button::Y);
		if (mButtonYPressedDuringSkippableCutscene)
		{
			ControlsIn::instance().injectEmptyInputs();
		}
	}
}

void Game::updateSpecialInput(float timeElapsed)
{
	// In time attack mode: Restart if holding Y button for a while
	bool isCharging = false;
	if (isInTimeAttackMode())
	{
		if (Application::instance().getSimulation().getSpeed() > 0.0f)	// Not in pause
		{
			const InputManager::Control& buttonY = InputManager::instance().getController(0).Y;
			if (buttonY.isPressed())
			{
				if (buttonY.justPressed())
				{
					// Start charging
					isCharging = true;
					mTimeAttackRestartCharge = 0.0f;
				}
				else
				{
					// Keep charging if already start, otherwise ignore the button
					isCharging = mTimeAttackRestartCharging;
				}
			}
		}
	}

	if (isCharging)
	{
		if (ConfigurationImpl::instance().mInstantTimeAttackRestart)
		{
			// Restart now
			mTimeAttackRestartCharge = 0.0f;
			restartTimeAttack(false);
			isCharging = false;
		}
		else
		{
			mTimeAttackRestartCharge += timeElapsed / 0.5f;
			if (mTimeAttackRestartCharge >= 1.0f)
			{
				// Restart now
				mTimeAttackRestartCharge = 1.0f;
				restartTimeAttack(true);
				isCharging = false;
			}
		}
	}
	else if (mTimeAttackRestartCharge > 0.0f)
	{
		mTimeAttackRestartCharge = std::max(0.0f, mTimeAttackRestartCharge - timeElapsed / 0.2f);
	}
	mTimeAttackRestartCharging = isCharging;

	// Update white overlay
	Application::instance().getGameView().setWhiteOverlayAlpha(mTimeAttackRestartCharge);
}

void Game::onActiveModsChanged()
{
	checkActiveModsUsedFeatures();
	mDynamicSprites.updateSpriteRedirects();
}

bool Game::shouldPauseOnFocusLoss() const
{
	return (GameApp::hasInstance() && Application::instance().getSimulation().isRunning() && Application::instance().getSimulation().getSpeed() > 0.0f && !isInMainMenuMode());
}

void Game::fillDebugVisualization(Bitmap& bitmap, int& mode)
{
	mode = mode % 2;
	static Bitmap textBitmap;

	const Vec2i cameraPosition = VideoOut::instance().getInterpolatedWorldSpaceOffset();
	const int32 cameraX = cameraPosition.x;
	const int32 cameraY = cameraPosition.y;
	const int32 cameraTileX = cameraX / 16;
	const int32 cameraTileY = cameraY / 16;

	const int32 minX = cameraTileX;
	const int32 maxX = cameraTileX + bitmap.getWidth() / 16;
	const int32 minY = cameraTileY;
	const int32 maxY = cameraTileY + bitmap.getHeight() / 16;

	const uint16 filterByCharacterPath = (3 << mEmulatorInterface->readMemory8(0xffffb046));

	for (int32 y = minY; y <= maxY; ++y)
	{
		for (int32 x = minX; x <= maxX; ++x)
		{
			const int32 screenX = x*16 - cameraX;
			const int32 screenY = y*16 - cameraY;

			// Access the right chunk
			const uint16 chunkX = (x / 8);
			const uint16 chunkY = (y / 8) & 0x1f;
			uint32 address = mEmulatorInterface->readMemory16(0xffff8008 + chunkY * 4) + chunkX;
			const uint16 chunkType = mEmulatorInterface->readMemory8(0xffff0000 + address);

			// Access tile info
			address = mEmulatorInterface->readMemory16(0x00f02a + chunkType * 2);
			address += ((x % 8) + (y % 8) * 8) * 2;
			const uint16 tile = mEmulatorInterface->readMemory16(0xffff0000 + address);
			const uint8 tileForm = mEmulatorInterface->readMemory8(mEmulatorInterface->readMemory32(0xfffff796) + (tile & 0x03ff) * 2);

			if ((tile & 0xf000) != 0 && tileForm != 0)
			{
				Color baseColor;
				uint8 angle;
				if (mode == 1)	// Angle is only relevant in mode 1
				{
					angle = mEmulatorInterface->readMemory8(0x096000 + tileForm);
					if (tile & 0x0400)	// Flip horizontally
						angle = 0x100 - angle;
					if (tile & 0x0800)	// Flip vertically
						angle = 0x80 - angle;

					baseColor.setFromHSL(Vec3f((float)angle / 256.0f * 360.0f, 1.0f, 0.5f));
					baseColor.a = 0.7f;
				}

				for (int32 iy = 0; iy < 16; ++iy)
				{
					const int32 py = (screenY + iy);
					if (py < 0 || py >= bitmap.getHeight())
						continue;

					for (int32 ix = 0; ix < 16; ++ix)
					{
						const int32 px = (screenX + ix);
						if (px < 0 || px >= bitmap.getWidth())
							continue;

						bool pixelVisible = false;

					#if 1
						// Vertical
						const uint16 offset = (tile & 0x0400) ? (15 - ix) : ix;
						int8 indent = (int8)mEmulatorInterface->readMemory8(0x096100 + (tileForm * 0x10) + offset);
						if (indent != 0)
						{
							if (tile & 0x0800)
								indent = -indent;

							if (indent > 0)
							{
								pixelVisible = ((signed)(15 - iy) < indent);
							}
							else
							{
								pixelVisible = ((signed)iy < -indent);
							}
						}
					#else
						// Horizontal
						const uint16 offset = (tile & 0x0800) ? (15 - iy) : iy;
						int8 indent = (int8)mEmulatorInterface->readMemory8(0x097100 + (tileForm * 0x10) + offset);
						if (indent != 0)
						{
							if (tile & 0x0400)
								indent = -indent;

							if (indent > 0)
							{
								pixelVisible = ((signed)(15 - ix) < indent);
							}
							else
							{
								pixelVisible = ((signed)ix < -indent);
							}
						}
					#endif

						uint32& dst = *bitmap.getPixelPointer(px, py);
						switch (mode)
						{
							case 0:
							{
								if (pixelVisible)
								{
									const bool isSolidAbove = (tile & 0x5000) != 0;		// Solid from above
									const bool isSolidOther = (tile & 0xa000) != 0;		// Solid from left/right/below
									const bool isPath1 = (tile & 0x3000) != 0;
									const bool isPath2 = (tile & 0xc000) != 0;

									dst = ((tile & filterByCharacterPath) != 0) ? 0xc0000000 : 0x50000000;

									if (!isPath1 && (ix + iy) & 0x04)	dst = 0x40000000;
									if (!isPath2 && (ix - iy) & 0x04)	dst = 0x40000000;
									if (isSolidAbove)	dst += 0xff00ff;
									if (isSolidOther)	dst += 0x00ff00;
								}
								break;
							}

							case 1:
							{
								dst = baseColor.getABGR32();
								if (pixelVisible)
								{
									if (ix == 0x0f || iy == 0x0f)
										dst -= (dst & 0xf8f8f8) >> 3;
								}
								else
								{
									dst = ((dst & 0xfc000000) >> 2) | (dst & 0xffffff);
								}
								break;
							}
						}
					}
				}

				if (mode == 1)
				{
					Vec2i dummy;
					global::mSmallfont.printBitmap(textBitmap, dummy, Recti(0, 0, 16, 16), rmx::hexString(angle, 2, ""));

					Blitter::Options blitterOptions;
					blitterOptions.mBlendMode = BlendMode::ALPHA;

					static Blitter blitter;
					blitter.blitSprite(Blitter::OutputWrapper(bitmap), Blitter::SpriteWrapper(textBitmap, Vec2i()), Vec2i(screenX+4, screenY+5), blitterOptions);
				}
			}
		}
	}
}

void Game::onGameRecordingHeaderLoaded(const std::string& buildString, const std::vector<uint8>& buffer)
{
	// Switch to using the alternative set of settings
	ConfigurationImpl::instance().mActiveGameSettings = &ConfigurationImpl::instance().mAlternativeGameSettings;

	const std::unordered_map<uint32, SharedDatabase::Setting>& settings = SharedDatabase::getSettings();
	VectorBinarySerializer serializer(true, buffer);
	const size_t numSettings = serializer.read<uint32>();
	for (size_t i = 0; i < numSettings; ++i)
	{
		const uint32 settingId = serializer.read<uint32>();
		const uint32 value = serializer.read<uint32>();
		ConfigurationImpl::instance().mActiveGameSettings->setValue(settingId, value);
	}
}

void Game::onGameRecordingHeaderSave(std::vector<uint8>& buffer)
{
	std::vector<const SharedDatabase::Setting*> relevantSettings;
	{
		relevantSettings.reserve(32);
		const std::unordered_map<uint32, SharedDatabase::Setting>& settings = SharedDatabase::getSettings();
		for (const auto& pair : settings)
		{
			const SharedDatabase::Setting& setting = pair.second;
			if (!setting.mPurelyVisual)
				relevantSettings.push_back(&setting);
		}
	}

	VectorBinarySerializer serializer(false, buffer);
	serializer.writeAs<uint32>(relevantSettings.size());
	for (const SharedDatabase::Setting* setting : relevantSettings)
	{
		const uint32 value = ConfigurationImpl::instance().mLocalGameSettings.getValue(setting->mSettingId);
		serializer.writeAs<uint32>(setting->mSettingId);
		serializer.write(value);
	}
}

void Game::checkActiveModsUsedFeatures()
{
	// Update number of players depending on the three / four player mod feature
	const bool usesThreePlayers = ModManager::instance().anyActiveModUsesFeature(rmx::constMurmur2_64("ThreePlayers"));
	const bool usesFourPlayers = ModManager::instance().anyActiveModUsesFeature(rmx::constMurmur2_64("FourPlayers"));
	Configuration::instance().mNumPlayers = usesFourPlayers ? 4 : usesThreePlayers ? 3 : 2;
}

void Game::startIntoGameInternal()
{
	// Setup defaults -- these should not get used outside of Time Attack anyway
	mLastZoneAndAct = 0;
	mLastCharacters = 0;
	mReceivedTimeAttackFinished = false;
	mSkippableCutsceneFrames = 0;

	Simulation& simulation = Application::instance().getSimulation();
	simulation.setRunning(true);
	simulation.setSpeed(simulation.getDefaultSpeed());

	// Enforce fixed simulation frequency in Time Attack and for the Main Menu background
	if (isInTimeAttackMode() || isInMainMenuMode())
		simulation.setSimulationFrequencyOverride(60.0f);
	else
		simulation.disableSimulationFrequencyOverride();

	setSetting(SharedDatabase::Setting::SETTING_KNUCKLES_AND_TAILS, false);		// Not queried by scripts at all, but it can't hurt to set it to false nevertheless

	SharedDatabase::resetAchievementValues();

	if (!isInMainMenuMode())
	{
		AudioOut::instance().moveMenuMusicToIngame();	// Needed only for the data select music to continue from main menu to data select
		AudioOut::instance().resumeSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_MUSIC);
		AudioOut::instance().resumeSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_SOUND);
	}

	mPlayerRecorder.reset();
	GameApp::instance().enableStillImageBlur(false);

	mTimeoutUntilDiscordRefresh = 0.0f;
}

uint32 Game::useSetting(uint32 settingId)
{
	return getSetting(settingId, false);
}

int32 Game::getAchievementValue(uint32 achievementId)
{
	SharedDatabase::Achievement* achievement = SharedDatabase::getAchievement(achievementId);
	return (nullptr == achievement) ? 0 : achievement->mValue;
}

void Game::setAchievementValue(uint32 achievementId, int32 value)
{
	SharedDatabase::Achievement* achievement = SharedDatabase::getAchievement(achievementId);
	if (nullptr != achievement)
	{
		achievement->mValue = value;
	}
}

bool Game::isAchievementComplete(uint32 achievementId)
{
	return (mPlayerProgress.mAchievements.getAchievementState(achievementId) != 0);
}

void Game::setAchievementComplete(uint32 achievementId)
{
	// Can't affect achievements in debug mode (except if dev mode is active)
	const bool hasDebugModeActive = getSetting(SharedDatabase::Setting::SETTING_DEBUG_MODE, true) != 0;
	if (hasDebugModeActive && !EngineMain::getDelegate().useDeveloperFeatures())
		return;

	if (mPlayerProgress.mAchievements.getAchievementState(achievementId) == 0)
	{
		mPlayerProgress.mAchievements.mAchievementStates[achievementId] = 1;
		SharedDatabase::Achievement* achievement = SharedDatabase::getAchievement(achievementId);
		if (nullptr != achievement)
		{
			GameApp::instance().showUnlockedWindow(SecretUnlockedWindow::EntryType::ACHIEVEMENT, "Achievement complete", achievement->mName);
		}
		else
		{
			RMX_ERROR("Achievement not found", );
		}

		checkForUnlockedSecrets(false);
		mPlayerProgress.save();
	}
}

bool Game::isSecretUnlocked(uint32 secretId)
{
	return mPlayerProgress.mUnlocks.isSecretUnlocked(secretId);
}

void Game::setSecretUnlocked(uint32 secretId)
{
	SharedDatabase::Secret* secret = SharedDatabase::getSecret(secretId);
	RMX_CHECK(nullptr != secret, "Secret with ID " << secretId << " not found", return);

	if (!mPlayerProgress.mUnlocks.isSecretUnlocked(secretId))
	{
		mPlayerProgress.mUnlocks.setSecretUnlocked(secretId);
		const char* text = (secret->mType == SharedDatabase::Secret::SECRET_DOOMSDAY_ZONE) ? "Unlocked in Act Select" : secret->mHiddenUntilUnlocked ? "Found hidden secret!" : "Secret unlocked!";
		GameApp::instance().showUnlockedWindow(SecretUnlockedWindow::EntryType::SECRET, text, secret->mName);
		mPlayerProgress.save();
	}
}

void Game::triggerRestart()
{
	mRestartTriggered = true;
}

void Game::onGamePause(uint8 canRestart)
{
	GameApp::instance().onGamePaused(canRestart != 0);
}

void Game::allowRestartInGamePause(uint8 canRestart)
{
	mAllowRestartInGamePause = (canRestart != 0);
}

void Game::onLevelStart()
{
	mRestartingTimeAttack = false;
}

void Game::onZoneActCompleted(uint16 zoneAndAct)
{
	const uint8 bitNumber = (zoneAndAct >> 7) + (zoneAndAct & 1);
	const uint32 bitValue = (1 << bitNumber);
	const uint8 character = clamp(mLastCharacters, 1, 3) - 1;

	mPlayerProgress.mUnlocks.mFinishedZoneAct |= bitValue;
	mPlayerProgress.mUnlocks.mFinishedZoneActByCharacter[character] |= bitValue;
	mPlayerProgress.save();
}

uint16 Game::onTriggerNextZone(uint16 zoneAndAct)
{
	return zoneAndAct;
}

uint16 Game::onFadedOutLoadingZone(uint16 zoneAndAct)
{
	return zoneAndAct;
}

bool Game::onCharacterDied(uint8 playerIndex)
{
	// Death in time attack means level restart
	if (isInTimeAttackMode())
	{
		if (mReceivedTimeAttackFinished)
		{
			// No death allowed!
			return false;
		}

		Application::instance().getSimulation().getCodeExec().getLemonScriptRuntime().callFunctionByName("restartTimeAttack");
	}
	return true;
}

void Game::returnToMainMenu()
{
	mReturnToMenuTriggered = true;
#if defined(PLATFORM_PSP)
	RMX_LOG_INFO("PSP: return to main menu, data select music playing: " << (AudioOut::instance().isPlayingAudioKey(0x2f) ? "yes" : "no"));
#endif

	// Do not restart data select music
	if (AudioOut::instance().isPlayingAudioKey(0x2f))
	{
		AudioOut::instance().moveIngameMusicToMenu();
	}
	else
	{
		// Do a quick fade-out of the music (especially for transition from Blue Sphere mode and Competition Mode)
		AudioOut::instance().fadeOutChannel(0, 0.15f);
	}

	mTimeoutUntilDiscordRefresh = 0.0f;
}

void Game::openOptionsMenu()
{
	GameApp::instance().openOptionsMenuInGame();
}

bool Game::onTimeAttackFinish()
{
	if (!isInTimeAttackMode() || mReceivedTimeAttackFinished)
		return false;

	mReceivedTimeAttackFinished = true;
	return true;
}

void Game::changePlanePatternRectAtex(uint16 px, uint16 py, uint16 width, uint16 height, uint8 planeIndex, uint8 atex)
{
	s3air::changePlanePatternRectAtex(*mEmulatorInterface, px, py, width, height, planeIndex, atex);
}

void Game::setupBlueSpheresGroundSprites()
{
	mBlueSpheresRendering.createSprites(VideoOut::instance().getScreenSize());
}

void Game::renderBlueSpheresCustomSpheres()
{
	// The same as the script function "BlueSpheres.renderCustomSpheres", in C++ for performance (it runs for every sphere in each frame, ~3 ms per frame on a PSP as script)
	EmulatorInterface& emulatorInterface = *mEmulatorInterface;
	const uint32 dataAddress = 0x800000;
	writeBlueSpheresData(dataAddress, 0xfffff100, emulatorInterface.readMemory16(0xffffe422), emulatorInterface.readMemory16(0xffffe424), emulatorInterface.readMemory8(0xffffe426));

	SpriteManager& spriteManager = RenderParts::instance().getSpriteManager();
	const uint16 screenExtend = (uint16)(VideoOut::instance().getScreenWidth() - 320) / 2;
	const uint16 liftedHeight = emulatorInterface.readMemory16(0xffffe44a);			// bluespheres.lifted_height
	const bool useGlow = (useSetting(0x80009103) & 0x02) != 0;						// SETTING_BS_VISUAL_STYLE
	auto convertColors32 = [](uint32 color) { return ((color & 0x000000ff) << 24) + ((color & 0x0000ff00) << 8) + ((color & 0x00ff0000) >> 8) + ((color & 0xff000000) >> 24); };

	// Size thresholds for the sphere forms 0x0f (smallest) down to 0x01, above the last one it's 0x00 (largest)
	static const uint16 FORM_THRESHOLDS[15] = { 0x1740, 0x18a0, 0x1a10, 0x1b90, 0x1d30, 0x1f00, 0x2100, 0x2330, 0x25a0, 0x2960, 0x2c80, 0x3180, 0x35c0, 0x3c80, 0x4500 };

	uint32& D0 = emulatorInterface.getRegister(EmulatorInterface::Register::D0);
	uint32& D1 = emulatorInterface.getRegister(EmulatorInterface::Register::D1);
	uint32& D6 = emulatorInterface.getRegister(EmulatorInterface::Register::D6);
	uint32& A3 = emulatorInterface.getRegister(EmulatorInterface::Register::A3);
	auto setLow16 = [](uint32& reg, uint16 value) { reg = (reg & 0xffff0000) | value; };

	uint32 ptr = dataAddress;
	const uint16 count = emulatorInterface.readMemory16(ptr);
	ptr += 2;
	for (uint16 i = 0; i < count; ++i)
	{
		// Read sphere properties
		const uint16 px = emulatorInterface.readMemory16(ptr) + screenExtend - 40;
		uint16 py = emulatorInterface.readMemory16(ptr + 2);
		const uint16 size = emulatorInterface.readMemory16(ptr + 4);
		const uint8 sphereType = emulatorInterface.readMemory8(ptr + 6);
		ptr += 7;

		uint8 form = 0x00;
		for (int k = 0; k < 15; ++k)
		{
			if (size < FORM_THRESHOLDS[k])
			{
				form = (uint8)(0x0f - k);
				break;
			}
		}

		py -= (uint16)((16 - form) * 2 / 3 + 1);
		py -= liftedHeight;		// Lift spheres when stage was won

		const bool behindHorizon = (size < 0x1b00);
		const uint16 renderQueue = (behindHorizon ? 0x2000 : 0x9000) + size / 0x100;
		const uint32 alpha = (sphereType == 0x0a) ? 0xa0 : 0xff;
		const uint32 fog = (uint32)clamp((int32)0x100000 / (int32)size - 0x68, 0, 0x80) * 0x010101;

		setLow16(D0, (uint16)(sphereType * 8));
		const uint32 tableEntry = 0xffffe480 + (D0 & 0xffff);
		A3 = emulatorInterface.readMemory32(tableEntry);
		setLow16(D6, emulatorInterface.readMemory16(tableEntry + 4));
		setLow16(D0, emulatorInterface.readMemory16(tableEntry + 6));
		D1 = (D0 & 0x8000) ? 0 : (uint32)(form * 2);
		setLow16(D1, (uint16)((D1 & 0xffff) + (D0 & 0xffff) * 2));
		A3 = A3 + (int32)(int16)emulatorInterface.readMemory16(A3 + (int32)(int16)(D1 & 0xffff));		// tableLookupAddress

		// A3 = Pointer to sprite data
		setLow16(D1, (uint16)(emulatorInterface.readMemory16(A3) - 1));		// This is always 0, except for Super Emeralds
		A3 += 2;
		while ((int16)(D1 & 0xffff) >= 0)
		{
			const Vec2i position((int16)(uint16)(px + emulatorInterface.readMemory16(A3 + 4)), (int16)(uint16)(py + (int8)emulatorInterface.readMemory8(A3)));
			const uint8 encodedSize = emulatorInterface.readMemory8(A3 + 1);
			const uint16 patternIndex = emulatorInterface.readMemory16(A3 + 2) + (uint16)(D6 & 0xffff);
			if (useGlow)
			{
				const uint32 tintColor = (0xffffff - fog) + (alpha << 24);
				spriteManager.drawVdpSprite(position, encodedSize, patternIndex, renderQueue, Color::fromRGBA32(convertColors32(tintColor)), Color::fromRGBA32(convertColors32(fog)));
			}
			else
			{
				spriteManager.drawVdpSprite(position, encodedSize, patternIndex, renderQueue);
			}
			A3 += 6;
			setLow16(D1, (uint16)((D1 & 0xffff) - 1));
		}
	}
}

void Game::writeBlueSpheresData(uint32 targetAddress, uint32 sourceAddress, uint16 px, uint16 py, uint8 rotation)
{
	mBlueSpheresRendering.writeVisibleSpheresData(targetAddress, sourceAddress, px, py, rotation, *mEmulatorInterface);
}

void Game::startSkippableCutscene()
{
	mSkippableCutsceneFrames = 5 * 60 * 60;		// Limit cutscene length to five minutes
}

void Game::endSkippableCutscene()
{
	mSkippableCutsceneFrames = 0;
	mButtonYPressedDuringSkippableCutscene = false;

	// Back to normal
	Simulation& simulation = Application::instance().getSimulation();
	if (simulation.getSpeed() == CUTSCENE_SKIPPING_SPEED)
		simulation.setSpeed(simulation.getDefaultSpeed());

	GameApp::instance().showSkippableCutsceneWindow(false);
}

bool Game::isInSkippableCutscene()
{
	return mSkippableCutsceneFrames > 0;
}
