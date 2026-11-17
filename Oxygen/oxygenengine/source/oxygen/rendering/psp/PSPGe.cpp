/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: the PSP's GPU (the GE) - VRAM layout, display lists, putting frames on screen.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#if defined(PLATFORM_PSP)

#include "oxygen/rendering/psp/PSPGe.h"

#include <pspdisplay.h>
#include <pspthreadman.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <chrono>
#include <malloc.h>


namespace pspge
{
	double gStatRenderCpuMs = 0.0;
	int gFrameBlendingPercent = 0;
	double gLastRenderCpuMs = 0.0;

	namespace
	{
		static const constexpr int VBLANKS_PER_FRAME = 2;		// Fixed 30 fps (the simulation still runs at 60 updates per second)
		static const constexpr int LIST_BYTES = 768 * 1024;		// (planes with a different scroll on each line take ~230 KB each, e.g. the burning forest in AIZ)

		static unsigned int __attribute__((aligned(64))) gList[LIST_BYTES / 4];
		bool gRunning = false;
		bool gListOpen = false;
		int gGameScreensThisFrame = 0;		// Game screens drawn to the display buffer since the last frame was sent (see "drawGameScreenToDisplay")
		bool gFrameBlendingActive = false;
		int gFrameBlendingCounter = 0;
		int gBlendedFrames = 0;				// (statistics)

		// Display buffers, as in the S1F/S2A port: one on screen, maybe one waiting for the vblank, one drawn into
		const uint32 gDisplayBufferOffsets[3] = { VRAM_FB0, VRAM_FB1, VRAM_FB2 };
		int gDisplayShown = 1;
		int gDisplayDraw = 0;
		uint32 gShownVcount = 0;

		// The last frame's list, sent to the GE but maybe not drawn yet: the CPU goes on with the next updates meanwhile
		bool gFramePending = false;
		int gDisplayPending = -1;
		int gPendingListBytes = 0;

		// The game screen's last size and where it goes on screen, and the UI over it
		int gGameWidth = 400;
		int gGameHeight = 224;
		const Bitmap* gUiOverlay = nullptr;
		const Bitmap* gLastUiOverlay = nullptr;		// (for screenshots)
		uint32 gUiScreenshotRequest = 0;
		uint32* gUiOverlayCopy = nullptr;		// For a texture that isn't 16-byte aligned
		size_t gUiOverlayCopySize = 0;

		// Statistics ("render.log", only in benchmark runs)
		typedef std::chrono::high_resolution_clock Clock;
		FILE* gStatsFile = nullptr;
		bool gStatsEnabled = false;
		Clock::time_point gLastPresentTime;
		bool gHasLastPresentTime = false;
		double gSumGeWait = 0.0;
		double gSumVblankWait = 0.0;
		double gSumFrame = 0.0;
		double gMaxFrame = 0.0;
		int gSumListBytes = 0;
		int gMissedVblanks = 0;
		int gCount = 0;
		uint32 gShownFrames = 0;

		// Screenshots ("S3AIR_BENCH_SHOTS=120,300": shown frame numbers)
		std::vector<uint32> gShotFrames;

		struct BlitVertex
		{
			float u, v;
			float x, y, z;
		};

		double msSince(Clock::time_point start)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		}

		// A textured rectangle, in vertical strips of about 32 texels (the GE's texture cache works far better that way)
		void blitRect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
		{
			int strips = (int)((u1 - u0) / 32.0f + 0.999f);
			strips = std::max(strips, 1);
			BlitVertex* v = (BlitVertex*)sceGuGetMemory(strips * 2 * sizeof(BlitVertex));
			for (int s = 0; s < strips; ++s)
			{
				const float a = (float)s / strips;
				const float b = (float)(s + 1) / strips;
				v[s * 2]	 = { u0 + (u1 - u0) * a, v0, x0 + (x1 - x0) * a, y0, 0.0f };
				v[s * 2 + 1] = { u0 + (u1 - u0) * b, v1, x0 + (x1 - x0) * b, y1, 0.0f };
			}
			sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, strips * 2, 0, v);
		}

		void writeStats(double geWaitMs, double vblankWaitMs, int listBytes)
		{
			const Clock::time_point now = Clock::now();
			if (gHasLastPresentTime)
			{
				const double frameMs = std::chrono::duration<double, std::milli>(now - gLastPresentTime).count();
				gSumFrame += frameMs;
				gMaxFrame = std::max(gMaxFrame, frameMs);
				gSumGeWait += geWaitMs;
				gSumVblankWait += vblankWaitMs;
				gSumListBytes += listBytes;
				++gCount;
			}
			gLastPresentTime = now;
			gHasLastPresentTime = true;

			if (gCount == 60 && nullptr != gStatsFile)
			{
				fprintf(gStatsFile, "shown %6u fps=%.2f frame_ms=%.2f max_frame_ms=%.2f render_cpu_ms=%.2f ge_wait_ms=%.2f vblank_wait_ms=%.2f list_kb=%d missed_vblanks=%d blended=%d\n",
						gShownFrames, 1000.0 * gCount / gSumFrame, gSumFrame / gCount, gMaxFrame, gStatRenderCpuMs / gCount, gSumGeWait / gCount, gSumVblankWait / gCount, gSumListBytes / gCount / 1024, gMissedVblanks, gBlendedFrames);
				if (gSumListBytes / gCount > 150 * 1024)
				{
					// What makes the display list this large (per frame: KB and number of draws)
					static const char* KIND_NAMES[6] = { "planes", "planes_tex1", "planes_tex2", "vdp_sprites", "custom_sprites", "other" };
					fprintf(gStatsFile, "  list by kind:");
					for (int k = 0; k < 6; ++k)
						fprintf(gStatsFile, " %s=%uKB/%u", KIND_NAMES[k], gStatListBytesByKind[k] / gCount / 1024, gStatDrawsByKind[k] / gCount);
					fprintf(gStatsFile, "\n");
				}
				for (int k = 0; k < 6; ++k)
					gStatListBytesByKind[k] = gStatDrawsByKind[k] = 0;
				fflush(gStatsFile);
				gSumGeWait = gSumVblankWait = gSumFrame = gMaxFrame = 0.0;
				gStatRenderCpuMs = 0.0;
				gSumListBytes = 0;
				gBlendedFrames = 0;
				gMissedVblanks = 0;
				gCount = 0;
			}
		}

		void readShotFrames()
		{
			const char* shots = getenv("S3AIR_BENCH_SHOTS");
			if (nullptr == shots)
				return;
			while (*shots)
			{
				gShotFrames.push_back((uint32)strtoul(shots, (char**)&shots, 10));
				while (*shots == ',' || *shots == ' ')
					++shots;
			}
		}
	}


	void startup()
	{
		if (gRunning)
			return;
		{
			// Frame blending: "S3AIR_PSP_FRAME_BLENDING=35" = the earlier update at 35%, the later one at 65% ("1" means 50%)
			const char* blending = getenv("S3AIR_PSP_FRAME_BLENDING");
			const int percent = (nullptr == blending) ? 0 : atoi(blending);
			gFrameBlendingPercent = (percent <= 0) ? 0 : (percent == 1) ? 50 : clamp(percent, 10, 50);
		}

		sceGuInit();
		sceGuStart(GU_DIRECT, (void*)((uintptr_t)gList | 0x40000000));
		sceGuDrawBuffer(GU_PSM_5650, getGeAddress(VRAM_FB0), FB_STRIDE);
		sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, getGeAddress(VRAM_FB1), FB_STRIDE);
		sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
		sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
		sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		sceGuEnable(GU_SCISSOR_TEST);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDisable(GU_BLEND);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_DITHER);
		sceGuClearColor(0xff000000);
		sceGuClear(GU_COLOR_BUFFER_BIT);
		sceGuFinish();
		sceGuSync(0, 0);
		sceDisplayWaitVblankStart();
		sceGuDisplay(GU_TRUE);

		// Clear all three display buffers and the patterns texture
		for (int i = 0; i < 3; ++i)
			memset(getVRamUncached(gDisplayBufferOffsets[i]), 0, FB_SIZE);
		memset(getVRamUncached(VRAM_PATTERNS), 0, PATTERNS_SIZE);
		memset(getVRamUncached(VRAM_GAME), 0, GAME_SIZE);

		gDisplayShown = 1;
		gDisplayDraw = 0;
		gShownVcount = sceDisplayGetVcount();

		gStatsEnabled = (nullptr != getenv("S3AIR_BENCH"));
		if (gStatsEnabled)
			gStatsFile = fopen("render.log", "w");
		readShotFrames();

		gRunning = true;
		RMX_LOG_INFO("PSP: GE started, display list " << (LIST_BYTES / 1024) << " KB");
	}

	bool isRunning()
	{
		return gRunning;
	}

	uint8* getVRamUncached(uint32 offset)
	{
		return (uint8*)((uintptr_t)sceGeEdramGetAddr() | 0x40000000) + offset;
	}

	void beginFrameList()
	{
		if (gListOpen)
			return;

		// The list memory gets used again: the GE must be done with the last frame
		finishPendingFrame();

		// The GE reads textures from RAM: what the CPU wrote has to be out of its data cache
		sceKernelDcacheWritebackInvalidateAll();
		// The list (commands, vertices, CLUTs) is written through the uncached address: sceGuGetMemory lets the GE run ahead,
		// so it could otherwise read vertices still sitting in the CPU's data cache
		sceGuStart(GU_DIRECT, (void*)((uintptr_t)gList | 0x40000000));
		gListOpen = true;
	}

	bool isFrameListOpen()
	{
		return gListOpen;
	}

	void* getListMemory(int bytes, int alignment)
	{
		// Overflowing the list would overwrite other memory: rather drop what doesn't fit (nullptr), and keep room for the
		// commands that follow (the callers skip that drawing)
		static const constexpr int RESERVE_BYTES = 32 * 1024;
		static uint32 loggedFrames = 0;
		if (sceGuCheckList() + bytes + alignment + RESERVE_BYTES > LIST_BYTES)
		{
			if (loggedFrames < 5)
			{
				++loggedFrames;
				RMX_LOG_INFO("PSP: GE display list full (" << (sceGuCheckList() / 1024) << " KB used, " << bytes << " bytes more asked for), drawing skipped");
			}
			return nullptr;
		}
		uint8* memory = (uint8*)sceGuGetMemory(bytes + alignment);
		return (void*)(((uintptr_t)memory + alignment - 1) & ~(uintptr_t)(alignment - 1));
	}

	int getListBytesUsed()
	{
		return sceGuCheckList();
	}

	bool isFrameBlendingWanted()
	{
		// Only while there's time to spare for the second game screen (see "updateFrameBlending")
		//  -> And with two game screens per frame, the display list gets twice as long: not when the last frame was already heavy
		return (gFrameBlendingPercent > 0) && gFrameBlendingActive && gPendingListBytes < 300 * 1024;
	}

	namespace
	{
		void updateFrameBlending(double spareMs)
		{
			// Frame blending costs a second game screen per frame: on only while frames have time to spare for it (= time the
			// CPU waited for the vblank), off as soon as time gets short, so it never slows the game down
			if (gFrameBlendingPercent <= 0)
				return;
			if (gFrameBlendingActive)
			{
				gFrameBlendingCounter = (spareMs < 1.5) ? (gFrameBlendingCounter + 1) : 0;
				if (gFrameBlendingCounter >= 3)
				{
					gFrameBlendingActive = false;
					gFrameBlendingCounter = 0;
				}
			}
			else
			{
				gFrameBlendingCounter = (spareMs > gLastRenderCpuMs + 3.0) ? (gFrameBlendingCounter + 1) : 0;
				if (gFrameBlendingCounter >= 20)
				{
					gFrameBlendingActive = true;
					gFrameBlendingCounter = 0;
				}
			}
		}
	}

	void drawGameScreenToDisplay(int gameWidth, int gameHeight)
	{
		// Frame blending: the second game screen before the next frame gets shown goes over the first at 50%
		//  -> At 30 fps, effects that alternate in every update (60 times a second) would otherwise always show the same half:
		//     e.g. the rolling animation alternates with a plain ball, bosses flash white in every other update
		const bool blendOver = (gFrameBlendingPercent > 0) && gGameScreensThisFrame > 0;
		++gGameScreensThisFrame;

		gGameWidth = gameWidth;
		gGameHeight = gameHeight;
		sceGuDrawBufferList(GU_PSM_5650, getGeAddress(gDisplayBufferOffsets[gDisplayDraw]), FB_STRIDE);
		sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
		sceGuDisable(GU_STENCIL_TEST);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_BLEND);
		sceGuPixelMask(0);
		if (blendOver)
		{
			sceGuEnable(GU_BLEND);
			const uint32 earlier = (uint32)(255 * gFrameBlendingPercent / 100);
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (255 - earlier) * 0x010101, earlier * 0x010101);
		}
		else
		{
			sceGuClearColor(0xff000000);
			sceGuClear(GU_COLOR_BUFFER_BIT);
		}

		// The game screen keeps its aspect ratio: 400x224 becomes 480x269, centered vertically
		const float width = (float)SCREEN_WIDTH;
		const float height = (float)gameHeight * width / (float)gameWidth;
		const float y0 = ((float)SCREEN_HEIGHT - height) * 0.5f;

		sceGuEnable(GU_TEXTURE_2D);
		sceGuTexMode(GU_PSM_8888, 0, 0, 0);
		sceGuTexImage(0, GAME_MAX_WIDTH, GAME_MAX_HEIGHT, GAME_STRIDE, (uint8*)sceGeEdramGetAddr() + VRAM_GAME);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
		sceGuTexFilter(GU_LINEAR, GU_LINEAR);
		sceGuTexWrap(GU_CLAMP, GU_CLAMP);
		sceGuTexFlush();
		// (half a texel in at the edges, so the linear filter never reads past the game screen)
		blitRect(0.0f, y0, width, y0 + height, 0.5f, 0.5f, (float)gameWidth - 0.5f, (float)gameHeight - 0.5f);
		if (blendOver)
			sceGuDisable(GU_BLEND);
	}

	void prepareUiDrawing(float& outScale, float& outOffsetY)
	{
		// The UI goes straight onto the display buffer, over the game screen: make sure that's there (the last one, if
		// there's no new one, e.g. behind menus)
		if (!gListOpen)
			beginFrameList();
		if (gGameScreensThisFrame == 0)
			drawGameScreenToDisplay(gGameWidth, gGameHeight);
		else
			sceGuDrawBufferList(GU_PSM_5650, getGeAddress(gDisplayBufferOffsets[gDisplayDraw]), FB_STRIDE);

		// Same place and scale as the game screen (see "drawGameScreenToDisplay")
		outScale = (float)SCREEN_WIDTH / (float)gGameWidth;
		outOffsetY = ((float)SCREEN_HEIGHT - (float)gGameHeight * outScale) * 0.5f;

		sceGuDisable(GU_STENCIL_TEST);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuDisable(GU_DEPTH_TEST);
		sceGuPixelMask(0);
		sceGuEnable(GU_SCISSOR_TEST);
	}

	void setUiOverlay(const Bitmap* uiTexture)
	{
		gUiOverlay = (nullptr != uiTexture && !uiTexture->empty()) ? uiTexture : nullptr;
	}

	namespace
	{
		void drawUiOverlay()
		{
			const Bitmap& bitmap = *gUiOverlay;
			const int width = std::min(bitmap.getWidth(), 512);
			const int height = std::min(bitmap.getHeight(), 512);

			// The GE wants the texture 16-byte aligned, with a width that's a multiple of 4 pixels
			const uint32* pixels = bitmap.getData();
			int bufferWidth = bitmap.getWidth();
			if (((uintptr_t)pixels & 15) != 0 || (bufferWidth & 3) != 0)
			{
				bufferWidth = (width + 3) & ~3;
				const size_t size = (size_t)bufferWidth * height;
				if (gUiOverlayCopySize < size)
				{
					free(gUiOverlayCopy);
					gUiOverlayCopy = (uint32*)memalign(64, size * 4);
					gUiOverlayCopySize = size;
				}
				for (int y = 0; y < height; ++y)
					memcpy(&gUiOverlayCopy[y * bufferWidth], &pixels[y * bitmap.getWidth()], width * 4);
				pixels = gUiOverlayCopy;
			}
			sceKernelDcacheWritebackRange(pixels, bufferWidth * height * 4);

			int textureWidth = 1;
			while (textureWidth < width)
				textureWidth <<= 1;
			int textureHeight = 1;
			while (textureHeight < height)
				textureHeight <<= 1;

			// Over the game screen, same place and scale; the colors are premultiplied by their alpha (see BlitterHelper::blendPixelAlpha)
			const float screenWidth = (float)SCREEN_WIDTH;
			const float screenHeight = (float)gGameHeight * screenWidth / (float)gGameWidth;
			const float y0 = ((float)SCREEN_HEIGHT - screenHeight) * 0.5f;
			sceGuEnable(GU_BLEND);
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xffffff, 0);
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexMode(GU_PSM_8888, 0, 0, 0);
			sceGuTexImage(0, textureWidth, textureHeight, bufferWidth, pixels);
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
			sceGuTexFilter(GU_LINEAR, GU_LINEAR);
			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			sceGuTexFlush();
			blitRect(0.0f, y0, screenWidth, y0 + screenHeight, 0.5f, 0.5f, (float)width - 0.5f, (float)height - 0.5f);
			sceGuDisable(GU_BLEND);
		}
	}

	void presentFrame()
	{
		// (debugging: once a second, how many calls had a new game screen and how many UI)
		static const bool debugPresent = (nullptr != getenv("S3AIR_DEBUG_PRESENT"));
		if (debugPresent)
		{
			static uint32 calls = 0, withList = 0, withUI = 0;
			static uint32 lastTime = 0;
			++calls;
			if (gListOpen) ++withList;
			if (nullptr != gUiOverlay) ++withUI;
			const uint32 now = sceKernelGetSystemTimeLow();
			if (now - lastTime > 1000000)
			{
				RMX_LOG_INFO("PSP: present calls " << calls << ", with game screen " << withList << ", with UI " << withUI);
				calls = withList = withUI = 0;
				lastTime = now;
			}
		}

		if (!gListOpen)
		{
			finishPendingFrame();
			if (nullptr == gUiOverlay)
			{
				// Nothing new to show (and no UI changing): just wait for the next vblank, so the main loop doesn't spin
				sceDisplayWaitVblankStart();
				return;
			}
			// No new game screen, but UI to show: over the last game screen, which is still in VRAM
			beginFrameList();
			drawGameScreenToDisplay(gGameWidth, gGameHeight);
		}
		gLastUiOverlay = gUiOverlay;
		if (gUiScreenshotRequest != 0 && nullptr != gUiOverlay)
		{
			char filename[64];
			snprintf(filename, sizeof(filename), "ui_%04u.bmp", gUiScreenshotRequest);
			dumpUiOverlay(filename, *gUiOverlay);
			gUiScreenshotRequest = 0;
		}
		if (nullptr != gUiOverlay)
		{
			drawUiOverlay();
			gUiOverlay = nullptr;
		}

		// Send the list to the GE, and don't wait for it: it draws while the CPU goes on with the next updates (see "finishPendingFrame")
		gPendingListBytes = sceGuCheckList();
		sceKernelDcacheWritebackAll();
		sceGuFinish();
		gListOpen = false;
		if (gGameScreensThisFrame >= 2)
			++gBlendedFrames;
		gGameScreensThisFrame = 0;
		gFramePending = true;
		gDisplayPending = gDisplayDraw;
		gDisplayDraw = 3 - gDisplayPending - gDisplayShown;		// (the third buffer)

		// Test switch: wait for the GE right away, like before
		static const bool noOverlap = (nullptr != getenv("S3AIR_PSP_NO_OVERLAP"));
		if (noOverlap)
			finishPendingFrame();
	}

	namespace
	{
		void dumpDisplayBuffer(const char* filename, uint32 vramOffset)
		{
			// A display buffer (5650) as a 24-bit BMP
			FILE* file = fopen(filename, "wb");
			if (nullptr == file)
				return;
			const int width = SCREEN_WIDTH;
			const int height = SCREEN_HEIGHT;
			const int paddedRowBytes = (width * 3 + 3) & ~3;
			const uint32 imageBytes = paddedRowBytes * height;
			uint8 header[54] = { 'B', 'M' };
			*(uint32_ua*)&header[2]  = 54 + imageBytes;
			*(uint32_ua*)&header[10] = 54;
			*(uint32_ua*)&header[14] = 40;
			*(int32_ua*)&header[18]  = width;
			*(int32_ua*)&header[22]  = height;
			*(uint16_ua*)&header[26] = 1;
			*(uint16_ua*)&header[28] = 24;
			*(uint32_ua*)&header[34] = imageBytes;
			fwrite(header, 1, 54, file);

			// Copied to RAM by the GE: emulators like PPSSPP only then have the frame in their emulated VRAM / RAM
			//  -> (the display list memory is free: the GE is done with the last frame, the next one isn't started yet)
			uint16* pixels = (uint16*)memalign(64, FB_STRIDE * SCREEN_HEIGHT * 2);
			if (nullptr == pixels)
			{
				fclose(file);
				return;
			}
			sceKernelDcacheWritebackInvalidateRange(pixels, FB_STRIDE * SCREEN_HEIGHT * 2);
			sceGuStart(GU_DIRECT, (void*)((uintptr_t)gList | 0x40000000));
			sceGuCopyImage(GU_PSM_5650, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, FB_STRIDE, (uint8*)sceGeEdramGetAddr() + vramOffset, 0, 0, FB_STRIDE, pixels);
			sceGuTexSync();
			sceGuFinish();
			sceGuSync(0, 0);
			sceKernelDcacheInvalidateRange(pixels, FB_STRIDE * SCREEN_HEIGHT * 2);

			std::vector<uint8> row(paddedRowBytes, 0);
			for (int y = height - 1; y >= 0; --y)
			{
				for (int x = 0; x < width; ++x)
				{
					const uint16 color = pixels[y * FB_STRIDE + x];
					const int r = (color & 0x1f) << 3;
					const int g = ((color >> 5) & 0x3f) << 2;
					const int b = ((color >> 11) & 0x1f) << 3;
					row[x * 3 + 0] = (uint8)b;
					row[x * 3 + 1] = (uint8)g;
					row[x * 3 + 2] = (uint8)r;
				}
				fwrite(&row[0], 1, paddedRowBytes, file);
			}
			fclose(file);
			free(pixels);
		}
	}

	void finishPendingFrame()
	{
		if (!gFramePending)
			return;

		// Wait until the GE drew the last frame (by now it usually has)
		const Clock::time_point geWaitStart = Clock::now();
		sceGuSync(0, 0);
		gFramePending = false;
		const double geWaitMs = msSince(geWaitStart);

		++gShownFrames;
		if (gUiScreenshotRequest != 0)
		{
			// UI drawn by the GE (see PSPUiRenderer.h): the whole display buffer, as it goes on screen
			char filename[64];
			snprintf(filename, sizeof(filename), "ui_%04u.bmp", gUiScreenshotRequest);
			dumpDisplayBuffer(filename, gDisplayBufferOffsets[gDisplayPending]);
			gUiScreenshotRequest = 0;
		}
		for (uint32 shotFrame : gShotFrames)
		{
			if (shotFrame == gShownFrames)
			{
				char filename[64];
				snprintf(filename, sizeof(filename), "shot_%04u.bmp", gShownFrames);
				dumpGameScreen(filename, 400, 224);
				RMX_LOG_INFO("PSP: screenshot " << filename << " at simulation frame " << gSimulationFrame);
				if (nullptr != gLastUiOverlay)
				{
					snprintf(filename, sizeof(filename), "shot_%04u_ui.bmp", gShownFrames);
					dumpUiOverlay(filename, *gLastUiOverlay);
				}
			}
		}

		// Each frame stays on screen for two refreshes: wait for the vblank before the one that puts it up
		const Clock::time_point vblankWaitStart = Clock::now();
		const uint32 due = gShownVcount + VBLANKS_PER_FRAME;
		if ((int32)(sceDisplayGetVcount() - due) > 0)
			gMissedVblanks += (int)(sceDisplayGetVcount() - due);
		while ((int32)(sceDisplayGetVcount() - due) < 0)
			sceDisplayWaitVblankStart();
		// (the one before has to be up, or the buffer it replaced could still be on screen)
		if (sceDisplayGetVcount() == gShownVcount)
			sceDisplayWaitVblankStart();
		sceDisplaySetFrameBuf((uint8*)sceGeEdramGetAddr() + gDisplayBufferOffsets[gDisplayPending], FB_STRIDE, PSP_DISPLAY_PIXEL_FORMAT_565, PSP_DISPLAY_SETBUF_NEXTFRAME);
		gShownVcount = sceDisplayGetVcount();
		gDisplayShown = gDisplayPending;
		gDisplayPending = -1;
		const double vblankWaitMs = msSince(vblankWaitStart);
		updateFrameBlending(vblankWaitMs);

		if (gStatsEnabled)
			writeStats(geWaitMs, vblankWaitMs, gPendingListBytes);
	}

	void requestUiScreenshot(uint32 number)
	{
		gUiScreenshotRequest = number;
	}

	void dumpUiOverlay(const char* filename, const Bitmap& bitmap)
	{
		// The UI texture as a BMP, over gray (its colors are premultiplied by alpha, see BlitterHelper)
		FILE* file = fopen(filename, "wb");
		if (nullptr == file)
			return;
		const int width = bitmap.getWidth();
		const int height = bitmap.getHeight();
		const int paddedRowBytes = (width * 3 + 3) & ~3;
		const uint32 imageBytes = paddedRowBytes * height;
		uint8 header[54] = { 'B', 'M' };
		*(uint32_ua*)&header[2]  = 54 + imageBytes;
		*(uint32_ua*)&header[10] = 54;
		*(uint32_ua*)&header[14] = 40;
		*(int32_ua*)&header[18]  = width;
		*(int32_ua*)&header[22]  = height;
		*(uint16_ua*)&header[26] = 1;
		*(uint16_ua*)&header[28] = 24;
		*(uint32_ua*)&header[34] = imageBytes;
		fwrite(header, 1, 54, file);
		std::vector<uint8> row(paddedRowBytes, 0);
		for (int y = height - 1; y >= 0; --y)
		{
			for (int x = 0; x < width; ++x)
			{
				const uint32 color = bitmap.getData()[x + y * width];		// ABGR
				const uint32 inverseAlpha = 255 - (color >> 24);
				row[x * 3 + 0] = (uint8)std::min<uint32>(((color >> 16) & 0xff) + 0x80 * inverseAlpha / 255, 255);
				row[x * 3 + 1] = (uint8)std::min<uint32>(((color >> 8) & 0xff) + 0x80 * inverseAlpha / 255, 255);
				row[x * 3 + 2] = (uint8)std::min<uint32>((color & 0xff) + 0x80 * inverseAlpha / 255, 255);
			}
			fwrite(&row[0], 1, paddedRowBytes, file);
		}
		fclose(file);
	}

	void dumpGameScreen(const char* filename, int gameWidth, int gameHeight)
	{
		FILE* file = fopen(filename, "wb");
		if (nullptr == file)
			return;

		const int rowBytes = gameWidth * 3;
		const int paddedRowBytes = (rowBytes + 3) & ~3;
		const uint32 imageBytes = paddedRowBytes * gameHeight;
		uint8 header[54] = { 'B', 'M' };
		*(uint32_ua*)&header[2]  = 54 + imageBytes;
		*(uint32_ua*)&header[10] = 54;
		*(uint32_ua*)&header[14] = 40;
		*(int32_ua*)&header[18]  = gameWidth;
		*(int32_ua*)&header[22]  = gameHeight;
		*(uint16_ua*)&header[26] = 1;
		*(uint16_ua*)&header[28] = 24;
		*(uint32_ua*)&header[34] = imageBytes;
		fwrite(header, 1, 54, file);

		// The GE copies the game screen into RAM (emulators like PPSSPP only see GE-drawn VRAM that way)
		uint32* pixels = (uint32*)memalign(64, GAME_STRIDE * gameHeight * 4);
		if (nullptr == pixels)
		{
			fclose(file);
			return;
		}
		sceKernelDcacheWritebackInvalidateAll();
		sceGuStart(GU_DIRECT, (void*)((uintptr_t)gList | 0x40000000));
		sceGuCopyImage(GU_PSM_8888, 0, 0, gameWidth, gameHeight, GAME_STRIDE, (uint8*)sceGeEdramGetAddr() + VRAM_GAME, 0, 0, GAME_STRIDE, pixels);
		sceGuTexSync();
		sceGuFinish();
		sceGuSync(0, 0);
		sceKernelDcacheInvalidateRange(pixels, GAME_STRIDE * gameHeight * 4);

		std::vector<uint8> row(paddedRowBytes, 0);
		for (int y = gameHeight - 1; y >= 0; --y)		// (BMP rows go bottom-up)
		{
			for (int x = 0; x < gameWidth; ++x)
			{
				const uint32 color = pixels[x + y * GAME_STRIDE];		// ABGR
				row[x * 3 + 0] = (color >> 16) & 0xff;
				row[x * 3 + 1] = (color >> 8) & 0xff;
				row[x * 3 + 2] = color & 0xff;
			}
			fwrite(&row[0], 1, paddedRowBytes, file);
		}
		fclose(file);
		free(pixels);
	}
}

#endif
