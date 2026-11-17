/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/menu/GameApp.h"
#include "sonic3air/menu/GameMenuManager.h"
#include "sonic3air/menu/MenuBackground.h"
#include "sonic3air/menu/PauseMenu.h"
#include "sonic3air/menu/SharedResources.h"
#include "sonic3air/menu/TimeAttackResultsMenu.h"
#include "sonic3air/menu/context/ApplicationContextMenu.h"
#include "sonic3air/menu/overlays/SecretUnlockedWindow.h"
#include "sonic3air/menu/overlays/SkippableCutsceneWindow.h"
#include "sonic3air/audio/AudioOut.h"
#include "sonic3air/data/SharedDatabase.h"
#include "sonic3air/Game.h"

#include "oxygen/application/Application.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/application/audio/AudioExport.h"
#include "oxygen/application/gameview/GameView.h"
#include "oxygen/application/input/ControlsIn.h"
#include "oxygen/application/input/InputManager.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/engine/EngineMain.h"
#include "oxygen/helper/FileHelper.h"
#include "oxygen/menu/imgui/ImGuiManager.h"
#include "oxygen/menu/imgui/implementations/ImGuiFileBrowser.h"
#include "oxygen/platform/PlatformFunctions.h"
#include "oxygen/rendering/utils/RenderUtils.h"

#if defined(PLATFORM_PSP)
	#include <pspctrl.h>
#endif
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/Simulation.h"


namespace
{
	static const constexpr float DISCLAIMER_FADE_IN_TIME  = 0.8f;
	static const constexpr float DISCLAIMER_FADE_OUT_TIME = 0.2f;
}


GameApp::GameApp() :
	mGameMenuManager(new GameMenuManager()),
	mMenuBackground(new MenuBackground()),
	mPauseMenu(new PauseMenu()),
	mTimeAttackResultsMenu(new TimeAttackResultsMenu())
{
	mGameMenuManager->initWithRoot(*mMenuBackground);
}

GameApp::~GameApp()
{
	delete mGameMenuManager;
	delete mMenuBackground;
	delete mPauseMenu;
	delete mTimeAttackResultsMenu;
	delete mSecretUnlockedWindow;
	delete mSkippableCutsceneWindow;
}

void GameApp::initialize()
{
	// Init shared resources
	global::loadSharedResources();

	mGameView = &Application::instance().getGameView();
	Simulation& simulation = Application::instance().getSimulation();
	simulation.setRunning(false);

	// PSP port tool (desktop build): export the emulated audio as WAV files, then quit
#if !defined(PLATFORM_PSP)
	if (nullptr != getenv("S3AIR_EXPORT_AUDIO"))
	{
		AudioExport::exportAll(String(getenv("S3AIR_EXPORT_AUDIO")).toStdWString());
		_Exit(0);
	}
#endif

	if (nullptr == getenv("S3AIR_BENCH_BLUESPHERE") && nullptr == getenv("S3AIR_BENCH_LEVEL"))
		gotoPhase(Configuration::instance().mStartPhase);
	if (nullptr != getenv("S3AIR_BENCH_BLUESPHERE"))
	{
		gotoPhase(3);
		Game::instance().startIntoBlueSphere();
	}
	else if (nullptr != getenv("S3AIR_BENCH_LEVEL"))
	{
		gotoPhase(3);
		Game::instance().startIntoLevel(Game::Mode::UNDEFINED, 0, (uint16)strtol(getenv("S3AIR_BENCH_LEVEL"), nullptr, 16), 1);
	}
	else if (Configuration::instance().mLoadLevel >= 0 && Configuration::instance().mLoadLevel <= 0xffff)
	{
		Game::instance().startIntoLevel(Game::Mode::UNDEFINED, 0, Configuration::instance().mLoadLevel, Configuration::instance().mUseCharacters);
	}

	if (nullptr == mApplicationContextMenu)
	{
		mApplicationContextMenu = &createChild<ApplicationContextMenu>();
	}
}

void GameApp::deinitialize()
{
	// Remove children that get explicitly deleted
	if (nullptr != mMenuBackground)
		mGameView->removeChild(*mMenuBackground);
	if (nullptr != mPauseMenu)
		mGameView->removeChild(*mPauseMenu);
	if (nullptr != mTimeAttackResultsMenu)
		mGameView->removeChild(*mTimeAttackResultsMenu);
	if (nullptr != mSecretUnlockedWindow)
		mGameView->removeChild(*mSecretUnlockedWindow);
	if (nullptr != mSkippableCutsceneWindow)
		mGameView->removeChild(*mSkippableCutsceneWindow);
}

void GameApp::mouse(const rmx::MouseEvent& ev)
{
	GuiBase::mouse(ev);
}

void GameApp::keyboard(const rmx::KeyboardEvent& ev)
{
	GuiBase::keyboard(ev);
}

void GameApp::update(float timeElapsed)
{
	GuiBase::update(timeElapsed);
	global::releaseUnusedMenuImages();	// (PSP: frees the images of menus not shown)

#if defined(PLATFORM_PSP)
	// Diagnostics: L + R + SELECT saves the last seconds of audio output as "audio_NN.wav" (with "S3AIR_AUDIO_CAPTURE=1")
	if (rmx::audiocapture::isEnabled())
	{
		static bool comboHeld = false;
		static int captureNumber = 0;
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(&pad, 1);
		const unsigned int combo = PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_SELECT;
		const bool held = ((pad.Buttons & combo) == combo);
		if (held && !comboHeld)
		{
			char filename[32];
			snprintf(filename, sizeof(filename), "audio_%02d.wav", ++captureNumber);
			const bool saved = rmx::audiocapture::save(filename);
			RMX_LOG_INFO("PSP: audio capture " << filename << (saved ? " saved" : " failed"));
		}
		comboHeld = held;
	}
#endif

	// Update disclaimer fade-in / fade-out
	{
		const float dt = std::min(timeElapsed, 0.05f);
		if (mCurrentState == State::DISCLAIMER)
		{
			mDisclaimerVisibility = std::min(mDisclaimerVisibility + dt / DISCLAIMER_FADE_IN_TIME, 1.0f);

			mStateTimeout -= dt;
			if (mStateTimeout <= 0.0f || InputManager::instance().anythingPressed())
			{
				gotoPhase(1);
			}
		}
		else
		{
			if (mDisclaimerVisibility > 0.0f)
			{
				mDisclaimerVisibility -= dt / DISCLAIMER_FADE_OUT_TIME;
				if (mDisclaimerVisibility <= 0.0f)
				{
					mDisclaimerVisibility = 0.0f;
					mDisclaimerTexture.clearBitmap();	// Unload to save on RAM
				}
			}
		}
	}

	if (mCurrentState == State::INGAME)
	{
		// Input
		Game::instance().updateSpecialInput(timeElapsed);
	}

	// GUI
	mGameMenuManager->updateMenus();

	if (nullptr != mPauseMenu->getParent() && mPauseMenu->canBeRemoved())
	{
		mGameView->removeChild(*mPauseMenu);
	}

	if (nullptr != mRemoveChild && mRemoveChild->getParent() == mGameView)
	{
		mGameView->removeChild(*mRemoveChild);
		mRemoveChild = nullptr;
	}

	// Make sure the overlay windows are always on top
	if (nullptr != mSecretUnlockedWindow && nullptr != mSecretUnlockedWindow->getParent())
	{
		mSecretUnlockedWindow->getParent()->moveToFront(*mSecretUnlockedWindow);
	}
	if (nullptr != mSkippableCutsceneWindow && nullptr != mSkippableCutsceneWindow->getParent())
	{
		if (mSkippableCutsceneWindow->canBeRemoved())
		{
			mGameView->removeChild(*mSkippableCutsceneWindow);
		}
		else
		{
			mSkippableCutsceneWindow->getParent()->moveToFront(*mSkippableCutsceneWindow);
		}
	}
}

void GameApp::render()
{
	Drawer& drawer = EngineMain::instance().getDrawer();

	if (mDisclaimerVisibility > 0.0f)
	{
		const Rectf rect = RenderUtils::getLetterBoxRect(FTX::screenRect(), (float)mDisclaimerTexture.getWidth() / (float)mDisclaimerTexture.getHeight());
		drawer.setBlendMode(BlendMode::OPAQUE);
		drawer.setSamplingMode(SamplingMode::BILINEAR);
		drawer.drawRect(rect, mDisclaimerTexture, Color(mDisclaimerVisibility, mDisclaimerVisibility, mDisclaimerVisibility));
		drawer.setSamplingMode(SamplingMode::POINT);
		drawer.performRendering();
	}

	GuiBase::render();
}

void GameApp::onStartGame()
{
	mCurrentState = State::INGAME;
	mRemoveChild = mMenuBackground;
	mGameView->setFadedIn();
}

void GameApp::openTitleScreen()
{
	mRemoveChild = mMenuBackground;
	mGameView->setFadedIn();
	gotoPhase(1);
}

void GameApp::openMainMenu()
{
	Application::instance().getSimulation().setRunning(false);
	AudioOut::instance().stopSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_MUSIC);
	AudioOut::instance().stopSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_SOUND);

	if (mPauseMenu->getParent() == mGameView)
		mGameView->removeChild(*mPauseMenu);
	if (mTimeAttackResultsMenu->getParent() == mGameView)
		mGameView->removeChild(*mTimeAttackResultsMenu);

	mCurrentState = State::MAIN_MENU;
	mGameView->addChild(*mMenuBackground);
	mGameView->startFadingIn();

	mGameMenuManager->forceRemoveAll();
	mMenuBackground->openGameStartedMenu();

	Game::instance().resetCurrentMode();
}

void GameApp::openOptionsMenuInGame()
{
	mRestoreGameResolution = VideoOut::instance().getScreenRect().getSize();
	Application::instance().getSimulation().setSpeed(0.0f);

	mCurrentState = State::INGAME_OPTIONS;

	mPauseMenu->setEnabled(false);
	mGameView->addChild(*mMenuBackground);
	mGameView->startFadingIn();
	mMenuBackground->openOptions(true);
}

void GameApp::onFadedOutOptions()
{
	if (mCurrentState == State::INGAME_OPTIONS)
	{
		// Coming from in-game options, then go back into the game
		if (mMenuBackground->getParent() == mGameView)
			mGameView->removeChild(*mMenuBackground);

		mPauseMenu->setEnabled(true);
		mPauseMenu->onReturnFromOptions();

		ControlsIn::instance().setAllIgnores();

		VideoOut::instance().setScreenSize(mRestoreGameResolution);

		GameApp::instance().getGameView().startFadingIn(0.1f);

		// TODO: Fade out the context instead
		AudioOut::instance().stopSoundContext(AudioOut::CONTEXT_MENU + AudioOut::CONTEXT_MUSIC);

		Application::instance().getSimulation().setSpeed(1.0f);

		mCurrentState = State::INGAME;
	}
}

void GameApp::onGamePaused(bool canRestart)
{
	showSkippableCutsceneWindow(false);

	Application::instance().getSimulation().setSpeed(0.0f);
	AudioOut::instance().pauseSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_MUSIC);
	AudioOut::instance().pauseSoundContext(AudioOut::CONTEXT_INGAME + AudioOut::CONTEXT_SOUND);

	mPauseMenu->enableRestart(canRestart);
	mPauseMenu->onFadeIn();
	if (nullptr == mPauseMenu->getParent())
	{
		mGameView->addChild(*mPauseMenu);
	}
}

void GameApp::restartTimeAttack()
{
	mCurrentState = State::INGAME;
	Game::instance().restartTimeAttack(true);
	mGameView->removeChild(*mTimeAttackResultsMenu);
}

void GameApp::returnToMenu()
{
	openMainMenu();
}

void GameApp::showTimeAttackResults(int hundreds, const std::vector<int>& otherTimes)
{
	mCurrentState = State::TIME_ATTACK_RESULTS;
	if (mTimeAttackResultsMenu->getParent() != mGameView)
	{
		mTimeAttackResultsMenu->setYourTime(hundreds);
		for (int time : otherTimes)
		{
			mTimeAttackResultsMenu->addOtherTime(time);
		}
		mTimeAttackResultsMenu->onFadeIn();

		mGameView->addChild(*mTimeAttackResultsMenu);
	}
}

void GameApp::enableStillImageBlur(bool enable, float timeout)
{
	mGameView->setStillImageMode(enable ? GameView::StillImageMode::BLURRING : GameView::StillImageMode::NONE, timeout);
}

void GameApp::showUnlockedWindow(SecretUnlockedWindow::EntryType entryType, const std::string& title, const std::string& content)
{
	if (nullptr == mSecretUnlockedWindow)
	{
		mSecretUnlockedWindow = new SecretUnlockedWindow();
	}
	mGameView->addChild(*mSecretUnlockedWindow);
	mSecretUnlockedWindow->show(entryType, title, content, (entryType == SecretUnlockedWindow::EntryType::SECRET) ? 0x68 : 0x63);
}

void GameApp::showSkippableCutsceneWindow(bool show)
{
	if (nullptr == mSkippableCutsceneWindow)
	{
		if (!show)
			return;
		mSkippableCutsceneWindow = &mGameView->createChild<SkippableCutsceneWindow>();
	}
	else
	{
		if (nullptr == mSkippableCutsceneWindow->getParent())
			mGameView->addChild(*mSkippableCutsceneWindow);
	}
	mSkippableCutsceneWindow->show(show);
}

bool GameApp::supportsFileBrowser()
{
#if defined(SUPPORT_IMGUI)
	return true;
#else
	return false;
#endif
}

bool GameApp::openFileBrowser()
{
#if defined(SUPPORT_IMGUI)
	ImGuiManager::instance().getOrAddImGuiContentProvider<ImGuiFileBrowser>(100);
	return true;
#else
	return false;
#endif
}

void GameApp::gotoPhase(int phaseNumber)
{
	switch (phaseNumber)
	{
		case 0:
		{
			// Start with the disclaimer
			mCurrentState = State::DISCLAIMER;
			mStateTimeout = 8.0f;
			InputManager::instance().setTouchInputMode(InputManager::TouchInputMode::FULLSCREEN_START);

			// Load disclaimer texture if not done already
			if (!mDisclaimerTexture.isValid())
			{
				FileHelper::loadTexture(mDisclaimerTexture, L"data/images/menu/disclaimer.png");
			}
			break;
		}

		case 1:
		{
			// Start with the intro & title screen
			mCurrentState = State::TITLE_SCREEN;
			Game::instance().startIntoTitleScreen();
			break;
		}

		case 2:
		{
			// Start with the main menu
			openMainMenu();
			break;
		}

		case 3:
		{
			// Start in-game
			mCurrentState = State::INGAME;
			Game::instance().resetCurrentMode();
			Simulation& simulation = Application::instance().getSimulation();
			simulation.setRunning(true);
			break;
		}
	}
}
