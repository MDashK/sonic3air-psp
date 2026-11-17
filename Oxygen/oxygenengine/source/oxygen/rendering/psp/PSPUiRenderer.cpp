/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: the UI (menus, texts, fades) drawn by the GE, instead of the software drawer.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#if defined(PLATFORM_PSP)

#include "oxygen/rendering/psp/PSPUiRenderer.h"
#include "oxygen/rendering/psp/PSPGe.h"
#include "oxygen/drawing/DrawCommand.h"
#include "oxygen/drawing/DrawerTexture.h"
#include "oxygen/rendering/RenderingDefinitions.h"
#include "oxygen/rendering/sprite/ComponentSprite.h"
#include "oxygen/rendering/sprite/PaletteSprite.h"
#include "oxygen/resources/PaletteCollection.h"
#include "oxygen/resources/SpriteCollection.h"

#include <pspgu.h>
#include <pspkernel.h>
#include <unordered_map>

// How the UI gets drawn:
//  - The game view draws its UI into a texture the size of the game screen, which used to be filled by the software drawer
//    and then put over the game screen. Instead, its drawing commands come here and the GE draws them right onto the
//    display buffer, over the game screen, in the same place and scale (400x224 -> 480x269).
//  - Textures (images, sprites, printed texts) get a swizzled copy in RAM, made again only when they change. Texts get
//    printed into a bitmap once per text / font / size, as long as they're used.
//  - Coordinates stay those of the UI texture, they only get scaled on the way to the screen.


namespace pspui
{
	namespace
	{
		struct UiVertex
		{
			float u, v;
			uint32 color;
			float x, y, z;
		};
		static const constexpr int VERTEX_FORMAT_TEXTURED = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D;

		struct TextureCopy
		{
			std::vector<uint8> mMemory;
			const void* mData = nullptr;	// 16-byte aligned, inside "mMemory"
			int mPsm = GU_PSM_8888;
			int mWidth = 0;					// Size of the texture content (after a reduction for sizes above 512)
			int mHeight = 0;
			int mBufferWidth = 0;
			int mTextureWidth = 0;			// Powers of two
			int mTextureHeight = 0;
		};

		struct TextureEntry
		{
			TextureCopy mCopy;
			uint32 mVersion = 0;			// Unique ID and change counter of a drawer texture
			int mSourceWidth = 0;
			int mSourceHeight = 0;
			uint32 mLastUsedFrame = 0;
		};

		struct TextEntry
		{
			TextureCopy mCopy;
			Vec2i mDrawOffset;				// Where the bitmap goes, relative to the text's rect
			uint32 mLastUsedFrame = 0;
		};

		DrawerTexture* gUiTexture = nullptr;
		bool gEnabled = true;

		float gScale = 1.2f;
		float gOffsetY = 1.6f;
		Vec2i gTargetSize(400, 224);

		BlendMode gBlendMode = BlendMode::OPAQUE;
		SamplingMode gSamplingMode = SamplingMode::POINT;
		TextureWrapMode gWrapMode = TextureWrapMode::CLAMP;
		int gAppliedBlendMode = -1;
		const void* gBoundTexture = nullptr;
		int gAppliedFilter = -1;
		int gAppliedWrap = -1;
		bool gTextureEnabled = false;

		Recti gScissor;
		std::vector<Recti> gScissorStack;

		uint32 gFrame = 1;
		size_t gCopiesBytes = 0;
		std::unordered_map<const void*, TextureEntry> gTextures;
		std::unordered_map<uint64, TextEntry> gTexts;
		std::vector<std::vector<uint8>> gDeferredFree;		// Copies replaced while the open display list may still use them
		Bitmap gTextBitmap;
		int gTextBitmapReserved = 0;

		// Statistics (S3AIR_DEBUG_PRESENT)
		uint32 gStatDraws = 0;
		uint32 gStatTextPrints = 0;
		uint32 gStatTextureCopies = 0;

		inline uint32 toABGR32(const Color& color)
		{
			return Color(saturate(color.r), saturate(color.g), saturate(color.b), saturate(color.a)).getABGR32();
		}

		inline int nextPowerOfTwo(int value)
		{
			int result = 8;
			while (result < value)
				result <<= 1;
			return result;
		}

		uint64 mix64(uint64 hash, uint64 value)
		{
			hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
			return hash;
		}

		// Swizzled copy (blocks of 16 bytes x 8 rows): the GE reads that much faster than the linear layout
		//  -> The pixels around the content are transparent (at least one column and row), so the linear filter fades the
		//     edges like the UI texture did when it got scaled as a whole
		//  -> "step" > 1 takes every step-th pixel only (for images larger than the GE's maximum of 512)
		void makeCopy(TextureCopy& copy, const uint8* src, int srcWidth, int srcHeight, int srcStrideBytes, int bytesPerPixel, int step)
		{
			const int width = srcWidth / step;
			const int height = srcHeight / step;
			const int alignmentPixels = 16 / bytesPerPixel;
			// (no transparent border for power-of-two sizes: those can repeat, e.g. the main menu's separator)
			const bool padX = !((width & (width - 1)) == 0 && width >= alignmentPixels);
			const bool padY = !((height & (height - 1)) == 0 && height >= 8);
			const int bufferWidth = std::min((width + (padX ? 1 : 0) + alignmentPixels - 1) / alignmentPixels * alignmentPixels, 512);
			const int paddedHeight = std::min((height + (padY ? 1 : 0) + 7) & ~7, 512);
			const int bufferWidthBytes = bufferWidth * bytesPerPixel;
			const size_t bytes = (size_t)bufferWidthBytes * paddedHeight;

			gCopiesBytes -= copy.mMemory.size();
			copy.mMemory.resize(bytes + 16);
			gCopiesBytes += copy.mMemory.size();
			uint8* dst = (uint8*)(((uintptr_t)&copy.mMemory[0] + 15) & ~(uintptr_t)15);

			uint8 line[16];
			uint8* out = dst;
			for (int blockY = 0; blockY < paddedHeight; blockY += 8)
			{
				for (int blockX = 0; blockX < bufferWidthBytes; blockX += 16)
				{
					for (int row = 0; row < 8; ++row)
					{
						const int y = blockY + row;
						const int firstPixel = blockX / bytesPerPixel;
						if (y < height && firstPixel < width)
						{
							const uint8* srcRow = src + (size_t)(y * step) * srcStrideBytes;
							const int pixels = std::min(alignmentPixels, width - firstPixel);
							if (step == 1)
							{
								memcpy(line, srcRow + firstPixel * bytesPerPixel, pixels * bytesPerPixel);
							}
							else
							{
								for (int k = 0; k < pixels; ++k)
									memcpy(&line[k * bytesPerPixel], srcRow + (size_t)(firstPixel + k) * step * bytesPerPixel, bytesPerPixel);
							}
							if (pixels < alignmentPixels)
								memset(&line[pixels * bytesPerPixel], 0, (alignmentPixels - pixels) * bytesPerPixel);
							memcpy(out, line, 16);
						}
						else
						{
							memset(out, 0, 16);
						}
						out += 16;
					}
				}
			}
			sceKernelDcacheWritebackRange(dst, bytes);

			copy.mData = dst;
			copy.mPsm = (bytesPerPixel == 4) ? GU_PSM_8888 : GU_PSM_T8;
			copy.mWidth = width;
			copy.mHeight = height;
			copy.mBufferWidth = bufferWidth;
			copy.mTextureWidth = nextPowerOfTwo(bufferWidth);
			copy.mTextureHeight = nextPowerOfTwo(paddedHeight);
			++gStatTextureCopies;
		}

		void releaseCopyLater(TextureCopy& copy)
		{
			gCopiesBytes -= copy.mMemory.size();
			if (!copy.mMemory.empty())
				gDeferredFree.emplace_back(std::move(copy.mMemory));
			copy.mMemory = std::vector<uint8>();
			copy.mData = nullptr;
		}

		const TextureCopy* getTextureCopy(const void* key, const uint8* data, int width, int height, int bytesPerPixel, uint32 version)
		{
			if (nullptr == data || width <= 0 || height <= 0)
				return nullptr;

			TextureEntry& entry = gTextures[key];
			if (nullptr == entry.mCopy.mData || entry.mVersion != version || entry.mSourceWidth != width || entry.mSourceHeight != height)
			{
				// The display list that's open may already use the old copy: free it only once that's done
				if (nullptr != entry.mCopy.mData)
					releaseCopyLater(entry.mCopy);

				int step = 1;
				while (width / step > 511 || height / step > 511)
					++step;
				makeCopy(entry.mCopy, data, width, height, width * bytesPerPixel, bytesPerPixel, step);
				entry.mVersion = version;
				entry.mSourceWidth = width;
				entry.mSourceHeight = height;
			}
			entry.mLastUsedFrame = gFrame;
			return &entry.mCopy;
		}

		const TextureCopy* getDrawerTextureCopy(DrawerTexture& texture)
		{
			const Bitmap& bitmap = texture.getBitmap();
			const uint32 version = texture.getUniqueID() * 0x10001u + texture.getChangeCounter();
			return getTextureCopy(&texture, (const uint8*)bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, version);
		}

		void applyBlendMode(BlendMode blendMode)
		{
			if ((int)blendMode == gAppliedBlendMode)
				return;
			gAppliedBlendMode = (int)blendMode;

			if (blendMode == BlendMode::ONE_BIT)
			{
				sceGuEnable(GU_ALPHA_TEST);
				sceGuAlphaFunc(GU_GREATER, 0x7f, 0xff);
			}
			else
			{
				sceGuDisable(GU_ALPHA_TEST);
			}

			switch (blendMode)
			{
				case BlendMode::OPAQUE:
				case BlendMode::ONE_BIT:
					sceGuDisable(GU_BLEND);
					break;

				case BlendMode::ALPHA:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
					break;

				case BlendMode::ADDITIVE:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffff);
					break;

				case BlendMode::SUBTRACTIVE:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_SRC_ALPHA, GU_FIX, 0, 0xffffff);
					break;

				case BlendMode::MULTIPLICATIVE:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
					break;

				case BlendMode::MINIMUM:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_MIN, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
					break;

				case BlendMode::MAXIMUM:
					sceGuEnable(GU_BLEND);
					sceGuBlendFunc(GU_MAX, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
					break;
			}
		}

		void applyScissor()
		{
			// UI texture coordinates -> screen pixels (rounded outwards, a partly covered pixel is drawn into)
			int x0 = (int)floorf((float)gScissor.x * gScale);
			int y0 = (int)floorf((float)gScissor.y * gScale + gOffsetY);
			int x1 = (int)ceilf((float)(gScissor.x + gScissor.width) * gScale);
			int y1 = (int)ceilf((float)(gScissor.y + gScissor.height) * gScale + gOffsetY);
			x0 = clamp(x0, 0, pspge::SCREEN_WIDTH);
			y0 = clamp(y0, 0, pspge::SCREEN_HEIGHT);
			x1 = clamp(x1, 0, pspge::SCREEN_WIDTH);
			y1 = clamp(y1, 0, pspge::SCREEN_HEIGHT);
			if (x1 <= x0 || y1 <= y0)
				sceGuScissor(0, 0, 0, 0);
			else
				sceGuScissor(x0, y0, x1 - x0, y1 - y0);		// (this pspsdk's sceGuScissor takes a size)
		}

		void enableTexturing(bool enable)
		{
			if (enable == gTextureEnabled)
				return;
			gTextureEnabled = enable;
			if (enable)
				sceGuEnable(GU_TEXTURE_2D);
			else
				sceGuDisable(GU_TEXTURE_2D);
		}

		void bindTexture(const TextureCopy& copy, bool linear, bool repeat)
		{
			enableTexturing(true);
			if (copy.mData != gBoundTexture)
			{
				sceGuTexMode(copy.mPsm, 0, 0, 1);
				sceGuTexImage(0, copy.mTextureWidth, copy.mTextureHeight, copy.mBufferWidth, copy.mData);
				sceGuTexFlush();
				gBoundTexture = copy.mData;
			}
			const int filter = linear ? GU_LINEAR : GU_NEAREST;
			if (filter != gAppliedFilter)
			{
				sceGuTexFilter(filter, filter);
				gAppliedFilter = filter;
			}
			const int wrap = repeat ? GU_REPEAT : GU_CLAMP;
			if (wrap != gAppliedWrap)
			{
				sceGuTexWrap(wrap, wrap);
				gAppliedWrap = wrap;
			}
		}

		bool useLinearFilter(float scaleX, float scaleY)
		{
			// The UI texture used to get scaled up linearly as a whole: the same here. Only things drawn larger anyway
			// (twice the size or more, with point sampling) stay sharp.
			if (gSamplingMode == SamplingMode::BILINEAR)
				return true;
			return (fabsf(scaleX) < 1.9f || fabsf(scaleY) < 1.9f);
		}

		// A rectangle (UI texture coordinates), textured from "u0/v0" to "u1/v1" (texels) or only colored (copy == nullptr)
		//  -> "x1 < x0" mirrors horizontally
		void drawQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const TextureCopy* copy, uint32 color, bool repeat = false)
		{
			if (x1 < x0)
			{
				std::swap(x0, x1);
				std::swap(u0, u1);
			}
			if (y1 < y0)
			{
				std::swap(y0, y1);
				std::swap(v0, v1);
			}
			if (x1 <= x0 || y1 <= y0)
				return;

			// Clip against the scissor rect here already (the GE would, but it's cheaper and keeps coordinates in range)
			const float sx0 = (float)gScissor.x;
			const float sy0 = (float)gScissor.y;
			const float sx1 = (float)(gScissor.x + gScissor.width);
			const float sy1 = (float)(gScissor.y + gScissor.height);
			if (x1 <= sx0 || x0 >= sx1 || y1 <= sy0 || y0 >= sy1)
				return;
			if (x0 < sx0)  { u0 += (u1 - u0) * (sx0 - x0) / (x1 - x0);  x0 = sx0; }
			if (x1 > sx1)  { u1 -= (u1 - u0) * (x1 - sx1) / (x1 - x0);  x1 = sx1; }
			if (y0 < sy0)  { v0 += (v1 - v0) * (sy0 - y0) / (y1 - y0);  y0 = sy0; }
			if (y1 > sy1)  { v1 -= (v1 - v0) * (y1 - sy1) / (y1 - y0);  y1 = sy1; }

			const float px0 = x0 * gScale;
			const float px1 = x1 * gScale;
			const float py0 = y0 * gScale + gOffsetY;
			const float py1 = y1 * gScale + gOffsetY;

			if (nullptr != copy)
			{
				const float scaleX = (px1 - px0) / std::max(fabsf(u1 - u0), 0.01f);
				const float scaleY = (py1 - py0) / std::max(fabsf(v1 - v0), 0.01f);
				bindTexture(*copy, useLinearFilter(scaleX, scaleY), repeat);

				// In strips of 32 texels: the GE's texture cache handles those much better than one wide rectangle
				int strips = (int)(fabsf(u1 - u0) / 32.0f + 0.999f);
				strips = clamp(strips, 1, 32);
				UiVertex* vertices = (UiVertex*)pspge::getListMemory(strips * 2 * sizeof(UiVertex));
				if (nullptr == vertices)
					return;
				for (int s = 0; s < strips; ++s)
				{
					const float a = (float)s / strips;
					const float b = (float)(s + 1) / strips;
					vertices[s * 2]		= { u0 + (u1 - u0) * a, v0, color, px0 + (px1 - px0) * a, py0, 0.0f };
					vertices[s * 2 + 1] = { u0 + (u1 - u0) * b, v1, color, px0 + (px1 - px0) * b, py1, 0.0f };
				}
				sceGuDrawArray(GU_SPRITES, VERTEX_FORMAT_TEXTURED, strips * 2, 0, vertices);
			}
			else
			{
				enableTexturing(false);
				UiVertex* vertices = (UiVertex*)pspge::getListMemory(2 * sizeof(UiVertex));
				if (nullptr == vertices)
					return;
				vertices[0] = { 0.0f, 0.0f, color, px0, py0, 0.0f };
				vertices[1] = { 0.0f, 0.0f, color, px1, py1, 0.0f };
				sceGuDrawArray(GU_SPRITES, VERTEX_FORMAT_TEXTURED, 2, 0, vertices);
			}
			++gStatDraws;
		}

		void drawTextureRect(const Recti& rect, const TextureCopy& copy, const Vec2f& uv0, const Vec2f& uv1, const Color& tint)
		{
			const bool repeat = (gWrapMode == TextureWrapMode::REPEAT) && copy.mWidth == copy.mTextureWidth && copy.mHeight == copy.mTextureHeight;
			drawQuad((float)rect.x, (float)rect.y, (float)(rect.x + rect.width), (float)(rect.y + rect.height),
					 uv0.x * copy.mWidth, uv0.y * copy.mHeight, uv1.x * copy.mWidth, uv1.y * copy.mHeight, &copy, toABGR32(tint), repeat);
		}

		void loadClut(const PaletteBase& palette)
		{
			// A copy in the display list: the palette can still change during the frame
			uint32* clut = (uint32*)pspge::getListMemory(256 * sizeof(uint32), 64);
			if (nullptr == clut)
				return;
			const uint32* colors = palette.getRawColors();
			const int available = std::min(256, (int)palette.getSize());
			clut[0] = 0;
			for (int i = 1; i < available; ++i)
				clut[i] = colors[i];
			for (int i = std::max(available, 1); i < 256; ++i)
				clut[i] = 0;
			sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
			sceGuClutLoad(256 / 8, clut);
		}

		template<typename STRING>
		void printText(Font& font, const STRING& text, uint64 textHash, const Recti& rect, const DrawerPrintOptions& printOptions)
		{
			if (text.empty())
				return;

			uint64 key = mix64(textHash, (uint64)(uintptr_t)&font);
			key = mix64(key, font.getChangeCounter());
			key = mix64(key, ((uint64)(uint32)rect.width << 32) | (uint32)rect.height);
			key = mix64(key, ((uint64)(uint32)printOptions.mAlignment << 32) | (uint32)printOptions.mSpacing);

			TextEntry& entry = gTexts[key];
			if (nullptr == entry.mCopy.mData)
			{
				Vec2i drawPosition;
				font.printBitmap(gTextBitmap, drawPosition, Recti(0, 0, rect.width, rect.height), text, printOptions.mAlignment, printOptions.mSpacing, &gTextBitmapReserved);
				if (gTextBitmap.getWidth() <= 0 || gTextBitmap.getHeight() <= 0)
				{
					gTexts.erase(key);
					return;
				}
				int step = 1;
				while (gTextBitmap.getWidth() / step > 511 || gTextBitmap.getHeight() / step > 511)
					++step;
				makeCopy(entry.mCopy, (const uint8*)gTextBitmap.getData(), gTextBitmap.getWidth(), gTextBitmap.getHeight(), gTextBitmap.getWidth() * 4, 4, step);
				entry.mDrawOffset = drawPosition;
				++gStatTextPrints;
			}
			entry.mLastUsedFrame = gFrame;

			// Texts always get alpha blended (as in the software drawer)
			applyBlendMode(BlendMode::ALPHA);
			const TextureCopy& copy = entry.mCopy;
			const float x = (float)(rect.x + entry.mDrawOffset.x);
			const float y = (float)(rect.y + entry.mDrawOffset.y);
			drawQuad(x, y, x + (float)copy.mWidth,y + (float)copy.mHeight, 0.0f, 0.0f, (float)copy.mWidth, (float)copy.mHeight, &copy, toABGR32(printOptions.mTintColor));
			applyBlendMode(gBlendMode);
		}

		void evictUnused(uint32 maxAge)
		{
			for (auto it = gTextures.begin(); it != gTextures.end(); )
			{
				if (gFrame - it->second.mLastUsedFrame > maxAge)
				{
					gCopiesBytes -= it->second.mCopy.mMemory.size();
					it = gTextures.erase(it);
				}
				else
				{
					++it;
				}
			}
			for (auto it = gTexts.begin(); it != gTexts.end(); )
			{
				if (gFrame - it->second.mLastUsedFrame > maxAge)
				{
					gCopiesBytes -= it->second.mCopy.mMemory.size();
					it = gTexts.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}


	void setUiTexture(DrawerTexture* texture)
	{
		gUiTexture = texture;
	}

	bool isUiTexture(const DrawerTexture* texture)
	{
		return isEnabled() && nullptr != texture && texture == gUiTexture;
	}

	bool isEnabled()
	{
		static const bool softwareUI = (nullptr != getenv("S3AIR_PSP_SOFTWARE_UI") && atoi(getenv("S3AIR_PSP_SOFTWARE_UI")) != 0);
		return gEnabled && !softwareUI;
	}

	namespace
	{
		bool gPrepared = false;
		bool gWindowMode = false;		// Drawing into the window (screen coordinates) instead of the UI texture (game screen coordinates)

		void ensurePrepared()
		{
			// Only once there's something to draw: without UI, frames without a new game screen need no display list at all
			if (gPrepared && pspge::isFrameListOpen())
				return;
			gPrepared = true;
			pspge::prepareUiDrawing(gScale, gOffsetY);
			if (gWindowMode)
			{
				gScale = (float)pspge::SCREEN_WIDTH / (float)std::max(gTargetSize.x, 1);
				gOffsetY = ((float)pspge::SCREEN_HEIGHT - (float)gTargetSize.y * gScale) * 0.5f;
			}

			// The game screen part left the GE in its own state: set everything needed here
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuShadeModel(GU_FLAT);
			sceGuDisable(GU_CULL_FACE);
			sceGuDisable(GU_DITHER);
			gAppliedBlendMode = -1;
			gBoundTexture = nullptr;
			gAppliedFilter = -1;
			gAppliedWrap = -1;
			gTextureEnabled = false;
			sceGuDisable(GU_TEXTURE_2D);
			applyBlendMode(gBlendMode);
			applyScissor();
		}
	}

	void beginWindowCommands(int blendMode, int samplingMode, int wrapMode)
	{
		beginCommands(blendMode, samplingMode, wrapMode);
		gWindowMode = true;
		gTargetSize = FTX::screenSize();
		gScissor.set(0, 0, gTargetSize.x, gTargetSize.y);
	}

	bool isWindowActive()
	{
		return gWindowMode;
	}

	void beginCommands(int blendMode, int samplingMode, int wrapMode)
	{
		gPrepared = false;
		gWindowMode = false;
		gTargetSize = (nullptr != gUiTexture) ? gUiTexture->getSize() : Vec2i(400, 224);
		gBlendMode = (BlendMode)blendMode;
		gSamplingMode = (SamplingMode)samplingMode;
		gWrapMode = (TextureWrapMode)wrapMode;
		gScissorStack.clear();
		gScissor.set(0, 0, gTargetSize.x, gTargetSize.y);
	}

	void executeCommand(DrawCommand& drawCommand)
	{
		switch (drawCommand.getType())
		{
			case DrawCommand::Type::SET_BLEND_MODE:
			case DrawCommand::Type::SET_SAMPLING_MODE:
			case DrawCommand::Type::SET_WRAP_MODE:
			case DrawCommand::Type::PUSH_SCISSOR:
			case DrawCommand::Type::POP_SCISSOR:
				break;
			default:
				ensurePrepared();
				break;
		}

		switch (drawCommand.getType())
		{
			case DrawCommand::Type::RECT:
			{
				RectDrawCommand& dc = drawCommand.as<RectDrawCommand>();
				if (nullptr == dc.mTexture)
				{
					drawQuad((float)dc.mRect.x, (float)dc.mRect.y, (float)(dc.mRect.x + dc.mRect.width), (float)(dc.mRect.y + dc.mRect.height), 0.0f, 0.0f, 0.0f, 0.0f, nullptr, toABGR32(dc.mColor));
				}
				else
				{
					const TextureCopy* copy = getDrawerTextureCopy(*dc.mTexture);
					if (nullptr != copy)
						drawTextureRect(dc.mRect, *copy, dc.mUV0, dc.mUV1, dc.mColor);
				}
				break;
			}

			case DrawCommand::Type::UPSCALED_RECT:
			{
				UpscaledRectDrawCommand& dc = drawCommand.as<UpscaledRectDrawCommand>();
				if (nullptr != dc.mTexture)
				{
					const TextureCopy* copy = getDrawerTextureCopy(*dc.mTexture);
					if (nullptr != copy)
					{
						applyBlendMode(BlendMode::OPAQUE);
						drawTextureRect(dc.mRect, *copy, Vec2f(0.0f, 0.0f), Vec2f(1.0f, 1.0f), Color::WHITE);
						applyBlendMode(gBlendMode);
					}
				}
				break;
			}

			case DrawCommand::Type::SPRITE:
			{
				SpriteDrawCommand& sc = drawCommand.as<SpriteDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item)
					break;

				SpriteBase& sprite = *item->mSprite;
				const Vec2i size = sprite.getSize();
				const float x0 = (float)sc.mPosition.x + (float)sprite.mOffset.x * sc.mScale.x;
				const float y0 = (float)sc.mPosition.y + (float)sprite.mOffset.y * sc.mScale.y;
				const float x1 = x0 + (float)size.x * sc.mScale.x;
				const float y1 = y0 + (float)size.y * sc.mScale.y;

				if (item->mUsesComponentSprite)
				{
					ComponentSprite& componentSprite = static_cast<ComponentSprite&>(sprite);
					const Bitmap& bitmap = componentSprite.accessBitmap();
					const TextureCopy* copy = getTextureCopy(&componentSprite, (const uint8*)bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, 0);
					if (nullptr != copy)
						drawQuad(x0, y0, x1, y1, 0.0f, 0.0f, (float)copy->mWidth, (float)copy->mHeight, copy, toABGR32(sc.mTintColor));
				}
				else
				{
					const PaletteBase* palette = PaletteCollection::instance().getPalette(sc.mPaletteKey, 0);
					if (nullptr == palette)
						break;
					PaletteSprite& paletteSprite = static_cast<PaletteSprite&>(sprite);
					const PaletteBitmap& bitmap = paletteSprite.accessBitmap();
					const TextureCopy* copy = getTextureCopy(&paletteSprite, (const uint8*)bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 1, 0);
					if (nullptr != copy)
					{
						loadClut(*palette);
						drawQuad(x0, y0, x1, y1, 0.0f, 0.0f, (float)copy->mWidth, (float)copy->mHeight, copy, toABGR32(sc.mTintColor));
					}
				}
				break;
			}

			case DrawCommand::Type::SPRITE_RECT:
			{
				SpriteRectDrawCommand& sc = drawCommand.as<SpriteRectDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item || !item->mUsesComponentSprite)
					break;

				ComponentSprite& sprite = *static_cast<ComponentSprite*>(item->mSprite);
				const Bitmap& bitmap = sprite.accessBitmap();
				const TextureCopy* copy = getTextureCopy(&sprite, (const uint8*)bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, 0);
				if (nullptr != copy)
					drawTextureRect(sc.mRect, *copy, Vec2f(0.0f, 0.0f), Vec2f(1.0f, 1.0f), sc.mTintColor);
				break;
			}

			case DrawCommand::Type::MESH:
			{
				MeshDrawCommand& dc = drawCommand.as<MeshDrawCommand>();
				if (nullptr == dc.mTexture || dc.mTriangles.size() < 3)
					break;
				const TextureCopy* copy = getDrawerTextureCopy(*dc.mTexture);
				if (nullptr == copy)
					break;

				const int count = (int)dc.mTriangles.size() / 3 * 3;
				UiVertex* vertices = (UiVertex*)pspge::getListMemory(count * sizeof(UiVertex));
				if (nullptr == vertices)
					break;
				bindTexture(*copy, true, (gWrapMode == TextureWrapMode::REPEAT) && copy->mWidth == copy->mTextureWidth && copy->mHeight == copy->mTextureHeight);
				for (int i = 0; i < count; ++i)
				{
					const DrawerMeshVertex& input = dc.mTriangles[i];
					vertices[i] = { input.mTexcoords.x * copy->mWidth, input.mTexcoords.y * copy->mHeight, 0xffffffff, input.mPosition.x * gScale, input.mPosition.y * gScale + gOffsetY, 0.0f };
				}
				sceGuDrawArray(GU_TRIANGLES, VERTEX_FORMAT_TEXTURED, count, 0, vertices);
				++gStatDraws;
				break;
			}

			case DrawCommand::Type::MESH_VERTEX_COLOR:
			{
				MeshVertexColorDrawCommand& dc = drawCommand.as<MeshVertexColorDrawCommand>();
				if (dc.mTriangles.size() < 3)
					break;

				const int count = (int)dc.mTriangles.size() / 3 * 3;
				UiVertex* vertices = (UiVertex*)pspge::getListMemory(count * sizeof(UiVertex));
				if (nullptr == vertices)
					break;
				enableTexturing(false);
				sceGuShadeModel(GU_SMOOTH);
				for (int i = 0; i < count; ++i)
				{
					const DrawerMeshVertex_P2_C4& input = dc.mTriangles[i];
					vertices[i] = { 0.0f, 0.0f, toABGR32(input.mColor), input.mPosition.x * gScale, input.mPosition.y * gScale + gOffsetY, 0.0f };
				}
				sceGuDrawArray(GU_TRIANGLES, VERTEX_FORMAT_TEXTURED, count, 0, vertices);
				sceGuShadeModel(GU_FLAT);
				++gStatDraws;
				break;
			}

			case DrawCommand::Type::SET_BLEND_MODE:
			{
				gBlendMode = drawCommand.as<SetBlendModeDrawCommand>().mBlendMode;
				if (gPrepared && pspge::isFrameListOpen()) applyBlendMode(gBlendMode);
				break;
			}

			case DrawCommand::Type::SET_SAMPLING_MODE:
			{
				gSamplingMode = drawCommand.as<SetSamplingModeDrawCommand>().mSamplingMode;
				break;
			}

			case DrawCommand::Type::SET_WRAP_MODE:
			{
				gWrapMode = drawCommand.as<SetWrapModeDrawCommand>().mWrapMode;
				break;
			}

			case DrawCommand::Type::PRINT_TEXT:
			{
				PrintTextDrawCommand& dc = drawCommand.as<PrintTextDrawCommand>();
				printText(*dc.mFont, dc.mText, rmx::getMurmur2_64(dc.mText), dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PRINT_TEXT_W:
			{
				PrintTextWDrawCommand& dc = drawCommand.as<PrintTextWDrawCommand>();
				printText(*dc.mFont, dc.mText, mix64(rmx::getMurmur2_64(dc.mText), 1), dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PUSH_SCISSOR:
			{
				PushScissorDrawCommand& dc = drawCommand.as<PushScissorDrawCommand>();
				gScissor.intersect(dc.mRect);
				gScissorStack.emplace_back(gScissor);
				if (gPrepared && pspge::isFrameListOpen()) applyScissor();
				break;
			}

			case DrawCommand::Type::POP_SCISSOR:
			{
				if (!gScissorStack.empty())
					gScissorStack.pop_back();
				if (gScissorStack.empty())
					gScissor.set(0, 0, gTargetSize.x, gTargetSize.y);
				else
					gScissor = gScissorStack.back();
				if (gPrepared && pspge::isFrameListOpen()) applyScissor();
				break;
			}

			default:
				break;
		}
	}

	void endFrame()
	{
		// Only while the GE isn't using anything (after "pspge::finishPendingFrame", before the next display list)
		gWindowMode = false;		// (window drawing starts again with "beginWindowCommands")
		gPrepared = false;
		if (pspge::isFrameListOpen())
			return;

		gDeferredFree.clear();
		++gFrame;

		static const constexpr size_t MAX_COPIES_BYTES = 3 * 1024 * 1024;
		if ((gFrame & 0x1f) == 0)
			evictUnused(120);
		if (gCopiesBytes > MAX_COPIES_BYTES)
			evictUnused(2);

		static const bool debugPresent = (nullptr != getenv("S3AIR_DEBUG_PRESENT"));
		if (debugPresent && (gFrame % 60) == 0)
		{
			RMX_LOG_INFO("PSP UI: draws " << gStatDraws << ", texts printed " << gStatTextPrints << ", texture copies " << gStatTextureCopies
						 << " (last 60 frames), cached " << gTextures.size() << " textures + " << gTexts.size() << " texts, " << (gCopiesBytes / 1024) << " KB");
			gStatDraws = 0;
			gStatTextPrints = 0;
			gStatTextureCopies = 0;
		}
	}
}

#endif
