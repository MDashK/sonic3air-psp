/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: the PSP's GPU (the GE) - VRAM layout, display lists, putting frames on screen.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#if defined(PLATFORM_PSP)

namespace pspge
{
	// The simulation's frame number (set by the simulation), for the benchmark screenshots' log lines
	inline uint32 gSimulationFrame = 0;

	// Display list bytes and draws by kind, over a statistics window of "render.log" (written there when the list is large)
	//  -> [0..2] planes drawn in mode 0/1/2 (see PSPRenderer::renderPlane), [3] VDP sprites, [4] palette and component sprites, [5] the rest
	inline uint32 gStatListBytesByKind[6] = {};
	inline uint32 gStatDrawsByKind[6] = {};

	// Screen
	static const constexpr int SCREEN_WIDTH  = 480;
	static const constexpr int SCREEN_HEIGHT = 272;
	static const constexpr int FB_STRIDE	 = 512;
	static const constexpr uint32 FB_SIZE	 = FB_STRIDE * SCREEN_HEIGHT * 2;	// 16-bit (5650)

	// VRAM layout (offsets from the start of VRAM, 2 MB in total)
	//  - three display buffers (a frame that comes in late goes up at the very next vblank, as in the S1F/S2A port)
	//  - the game screen: 32-bit, the alpha channel doubles as the stencil (plane priority)
	//  - the Mega Drive patterns as a 4-bit texture: 64 x 32 patterns of 8x8 pixels
	static const constexpr uint32 VRAM_FB0		 = 0;
	static const constexpr uint32 VRAM_FB1		 = VRAM_FB0 + FB_SIZE;
	static const constexpr uint32 VRAM_FB2		 = VRAM_FB1 + FB_SIZE;
	static const constexpr uint32 VRAM_GAME		 = VRAM_FB2 + FB_SIZE;		// 0x0cc000
	static const constexpr int    GAME_STRIDE	 = 512;
	static const constexpr int    GAME_MAX_WIDTH = 512;
	static const constexpr int    GAME_MAX_HEIGHT = 256;
	static const constexpr uint32 GAME_SIZE		 = GAME_STRIDE * GAME_MAX_HEIGHT * 4;
	static const constexpr uint32 VRAM_PATTERNS	 = VRAM_GAME + GAME_SIZE;	// 0x14c000
	static const constexpr int    PATTERNS_TEXTURE_WIDTH  = 512;
	static const constexpr int    PATTERNS_TEXTURE_HEIGHT = 256;
	static const constexpr uint32 PATTERNS_SIZE	 = PATTERNS_TEXTURE_WIDTH * PATTERNS_TEXTURE_HEIGHT / 2;
	static const constexpr uint32 VRAM_FREE		 = VRAM_PATTERNS + PATTERNS_SIZE;	// 0x15c000
	// A plane drawn without its line scroll, for planes with lots of lines scrolled differently (see PSPRenderer::renderPlane)
	static const constexpr uint32 VRAM_PLANE_TEXTURE   = VRAM_FREE;
	static const constexpr int    PLANE_TEXTURE_WIDTH  = 512;
	static const constexpr int    PLANE_TEXTURE_HEIGHT = 256;		// (up to 0x1dc000, 0x200000 is the end of VRAM)

	void startup();
	bool isRunning();

	// Address the CPU writes to without going through its data cache (nothing to flush for the GE)
	uint8* getVRamUncached(uint32 offset);
	// Address as the GE sees it (an offset into VRAM)
	inline void* getGeAddress(uint32 offset)  { return (void*)(uintptr_t)offset; }

	// One display list per shown frame: opened by the game screen rendering, closed before putting it on screen
	void beginFrameList();
	bool isFrameListOpen();
	void* getListMemory(int bytes, int alignment = 16);		// Memory inside the display list, e.g. for vertices or CLUTs; nullptr if the list is full
	int getListBytesUsed();

	// Draws the game screen (of the given size) onto the display buffer that's drawn into now, scaled to the screen
	void drawGameScreenToDisplay(int gameWidth, int gameHeight);

	// The UI (menus, fades, text) drawn by the software drawer into a texture the size of the game screen, with premultiplied
	// alpha over transparency: put over the game screen when the frame goes on screen. nullptr: no UI this frame.
	void setUiOverlay(const Bitmap* uiTexture);
	void prepareUiDrawing(float& outScale, float& outOffsetY);	// Before the UI gets drawn onto the screen by the GE (see PSPUiRenderer.h)

	// Puts the frame on screen, paced to a fixed 30 fps: closes the display list and waits for the GE. Without a new game
	// screen, the last one (still in VRAM) is used, if there's UI to show over it.
	void presentFrame();		// Sends the frame to the GE, without waiting for it to be drawn
	void finishPendingFrame();	// Waits until the GE drew the last frame and puts it on screen (before changing anything the GE reads)

	// CPU time of the game screen rendering, accumulated for the statistics in "render.log"
	extern double gStatRenderCpuMs;
	extern double gLastRenderCpuMs;	// (CPU time of the last game screen)
	bool isFrameBlendingWanted();		// Frame blending (S3AIR_PSP_FRAME_BLENDING): mix the two updates of a frame, if the last frame wasn't too heavy for it

	// Debug: writes the game screen (as it is in VRAM) to a BMP file
	void dumpGameScreen(const char* filename, int gameWidth, int gameHeight);
	void dumpUiOverlay(const char* filename, const Bitmap& bitmap);	// (the same for the UI texture over the game screen)
	void requestUiScreenshot(uint32 number);		// Saves the UI texture as "ui_NNNN.bmp" with the next frame (debugging)
}

#endif
