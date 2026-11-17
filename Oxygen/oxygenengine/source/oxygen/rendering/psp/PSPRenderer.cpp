/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: renders the game screen with the PSP's GPU (the GE).
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#if defined(PLATFORM_PSP)

#include "oxygen/rendering/psp/PSPRenderer.h"
#include "oxygen/rendering/psp/PSPGe.h"
#include <chrono>
#include "oxygen/rendering/Geometry.h"
#include "oxygen/drawing/DrawerTexture.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/rendering/sprite/ComponentSprite.h"
#include "oxygen/rendering/sprite/PaletteSprite.h"
#include "oxygen/resources/SpriteCollection.h"
#include "oxygen/simulation/EmulatorInterface.h"

#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <unordered_map>

// How the GE draws the game screen (see also PSPGe.h for the VRAM layout):
//  - The Mega Drive patterns (2048 of 8x8 pixels, 4 bits per pixel) are a 4-bit texture in VRAM, kept in step with the
//    emulated VRAM (only the patterns that changed are written again). Each palette line is a 16-color part of a CLUT.
//  - A plane is drawn in runs of lines that share the horizontal scroll, the palette (normal / underwater) and the row
//    of patterns: one GE sprite (a rectangle) per pattern, flips by swapped texture coordinates, empty patterns skipped.
//    The rectangles are collected per palette line, so each plane pass needs only a few draw commands.
//  - Plane priority works like the Mega Drive's: high priority patterns write 1 into the stencil (the game screen's
//    alpha channel), and sprites without priority are only drawn where it's still 0.
//  - Sprite masks (e.g. the Sandopolis pyramid door, the Death Egg behind Sky Sanctuary's clouds): the other renderers
//    copy the screen once the render queue reaches 0x8000 (the planes), and a mask puts that copy back in its rectangle,
//    taking away what got drawn in between. Here each mask gets a stencil bit (bits 1..7) set in its rectangle at that
//    point instead, nothing gets drawn where it's set, and the mask clears it when its turn in the render queue comes.
//  - VDP sprites use the same pattern texture. Palette sprites (8-bit) and component sprites (32-bit) are textures read
//    from RAM; bitmaps that don't fit the GE's alignment rules get a padded copy, cached until the sprite changes.


namespace
{
	struct TileVertex
	{
		uint16 u, v;
		int16 x, y, z;
	};

	struct ColorVertex
	{
		uint32 color;
		int16 x, y, z, pad;
	};

	struct FloatVertex
	{
		float u, v;
		float x, y, z;
	};

	enum StencilMode
	{
		STENCIL_NORMAL = 0,			// Stencil stays as it is
		STENCIL_WRITE_PRIORITY,		// Set the stencil to 1 wherever something gets drawn (high priority plane patterns)
		STENCIL_BEHIND_PRIORITY		// Draw only where the stencil is 0 (sprites without priority)
	};

	inline int nextPowerOfTwo(int value)
	{
		int result = 1;
		while (result < value)
			result <<= 1;
		return result;
	}

	inline uint32 toABGR32(const Color& color)
	{
		return Color(saturate(color.r), saturate(color.g), saturate(color.b), saturate(color.a)).getABGR32();
	}
}


struct PSPRenderer::Internal
{
	// Patterns
	uint8 mPatternBackup[0x800 * 32];
	bool mPatternEmpty[0x800];
	bool mPatternsInitialized = false;

	// CLUTs of this frame for the patterns (64 colors each): [0] = standard palette, [1] = underwater palette
	const uint32* mPatternCluts[2] = { nullptr, nullptr };
	int mLoadedPatternClut = -1;
	bool mPatternTextureBound = false;

	// Rectangles of the current pass, per palette (2) and palette line (4)
	std::vector<TileVertex> mBuckets[8];

	// Textures for palette and component sprites
	struct TextureEntry
	{
		std::vector<uint8> mPaddedCopy;		// Empty if the bitmap is used directly
		const void* mTextureData = nullptr;
		int mTextureWidth = 0;		// Power of two
		int mTextureHeight = 0;		// Power of two
		int mBufferWidth = 0;		// In pixels
		int mSourceWidth = 0;
		int mSourceHeight = 0;
		uint32 mChangeCounter = 0;
		uint32 mLastUsedFrame = 0;
		uint32 mLastCopiedFrame = 0;
		bool mSwizzled = false;		// 32-bit sprites: a swizzled copy, made once (they don't change)
	};
	std::unordered_map<const void*, TextureEntry> mTextures;
	size_t mTextureCopiesBytes = 0;
	const void* mBoundTexture = nullptr;
	uint32 mFrameNumber = 0;
	bool mLoggedTooLargeSprite = false;
	bool mLoggedTexturedRect = false;

	// Sprite masks of this frame (render queue 0x8000 and later) and their stencil bits
	struct SpriteMask
	{
		const Geometry* mGeometry = nullptr;
		uint8 mBit = 0;
	};
	std::vector<SpriteMask> mSpriteMasks;
};


PSPRenderer::PSPRenderer(RenderParts& renderParts, DrawerTexture& outputTexture) :
	Renderer(RENDERER_TYPE_ID, renderParts, outputTexture),
	mInternal(*new Internal())
{
}

PSPRenderer::~PSPRenderer()
{
	delete &mInternal;
}

void PSPRenderer::initialize()
{
	pspge::startup();
	mGameResolution = Configuration::instance().mGameScreen;
	mGameResolution.x = std::min(mGameResolution.x, pspge::GAME_MAX_WIDTH);
	mGameResolution.y = std::min(mGameResolution.y, pspge::GAME_MAX_HEIGHT);
	for (int i = 0; i < 8; ++i)
		mInternal.mBuckets[i].reserve(4096);
	RMX_LOG_INFO("PSP: GE renderer ready, game screen " << mGameResolution.x << " x " << mGameResolution.y);
}

void PSPRenderer::reset()
{
}

void PSPRenderer::setGameResolution(const Vec2i& gameResolution)
{
	mGameResolution.x = std::min(gameResolution.x, pspge::GAME_MAX_WIDTH);
	mGameResolution.y = std::min(gameResolution.y, pspge::GAME_MAX_HEIGHT);
}

void PSPRenderer::clearGameScreen()
{
}

void PSPRenderer::renderGameScreen(const std::vector<Geometry*>& geometries)
{
	// The GE may still be drawing the last frame, using the patterns, textures and CLUTs updated below
	pspge::finishPendingFrame();
	const auto renderStart = std::chrono::high_resolution_clock::now();		// (CPU time of the game screen, for statistics and the frame blending)

	++mInternal.mFrameNumber;
	PaletteSprite::trimUpscaledBitmaps(1024 * 1024);	// (the GE finished the last frame: nothing uses them right now)
	updatePatterns();

	pspge::beginFrameList();
	sceGuDrawBufferList(GU_PSM_8888, pspge::getGeAddress(pspge::VRAM_GAME), pspge::GAME_STRIDE);

	// Common state
	sceGuEnable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_DITHER);
	sceGuDisable(GU_BLEND);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuPixelMask(0);
	mCurrentPixelMask = 0;
	sceGuEnable(GU_ALPHA_TEST);
	sceGuAlphaFunc(GU_GREATER, 0, 0xff);
	sceGuEnable(GU_STENCIL_TEST);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	mCurrentStencilMode = -1;
	mCurrentBlendMode = -1;
	setStencilMode(STENCIL_NORMAL);
	setBlendMode((int)BlendMode::OPAQUE);

	// Clear with the backdrop color, stencil 0
	mCurrentViewport.set(0, 0, mGameResolution.x, mGameResolution.y);
	mCurrentScissor.set(-1, -1, 0, 0);
	setScissor(mCurrentViewport);
	sceGuClearColor(mRenderParts.getPaletteManager().getBackdropColor().getABGR32() & 0x00ffffff);
	sceGuClearStencil(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);

	buildCluts();
	mInternal.mPatternTextureBound = false;
	mInternal.mBoundTexture = nullptr;
	mAnyPrioPlaneDrawn = false;
	mActiveMaskBits = 0;
	collectSpriteMasks(geometries);

	// Benchmark: S3AIR_BENCH_SKIP leaves out kinds of drawing, to see what takes the GE's time (1 = planes, 2 = VDP sprites, 4 = palette sprites, 8 = component sprites, 16 = rects)
	static const int skipMask = (nullptr != getenv("S3AIR_BENCH_SKIP")) ? atoi(getenv("S3AIR_BENCH_SKIP")) : 0;

	uint16 lastRenderQueue = 0xffff;
	for (const Geometry* geometry : geometries)
	{
		// Sprite masks: their rectangles get marked in the stencil where the other renderers copy the screen
		const uint16 renderQueue = geometry->mRenderQueue;
		if (!mInternal.mSpriteMasks.empty() && lastRenderQueue < 0x8000 && renderQueue >= 0x8000)
		{
			for (const Internal::SpriteMask& mask : mInternal.mSpriteMasks)
				drawSpriteMaskStencil(*mask.mGeometry, mask.mBit, true);
		}
		lastRenderQueue = renderQueue;
		const int listBytesBefore = pspge::getListBytesUsed();

		switch (geometry->getType())
		{
			case Geometry::Type::PLANE:
				if (skipMask & 1)
					break;
				renderPlane(static_cast<const PlaneGeometry&>(*geometry));
				break;

			case Geometry::Type::SPRITE:
			{
				const SpriteGeometry& spriteGeometry = static_cast<const SpriteGeometry&>(*geometry);
				switch (spriteGeometry.mSpriteInfo.getType())
				{
					case RenderItem::Type::VDP_SPRITE:		 if (!(skipMask & 2)) renderVdpSprite(spriteGeometry);	  break;
					case RenderItem::Type::PALETTE_SPRITE:	 if (!(skipMask & 4)) renderCustomSprite(spriteGeometry); break;
					case RenderItem::Type::COMPONENT_SPRITE: if (!(skipMask & 8)) renderCustomSprite(spriteGeometry); break;

					case RenderItem::Type::SPRITE_MASK:
					{
						// Its turn: what comes after it gets drawn in its rectangle again
						for (const Internal::SpriteMask& mask : mInternal.mSpriteMasks)
						{
							if (mask.mGeometry == geometry && (mActiveMaskBits & mask.mBit))
								drawSpriteMaskStencil(*geometry, mask.mBit, false);
						}
						break;
					}

					default:  break;
				}
				break;
			}

			case Geometry::Type::RECT:
			{
				const RectGeometry& rectGeometry = static_cast<const RectGeometry&>(*geometry);
				if (!(skipMask & 16))
					renderRect(rectGeometry.mRect, rectGeometry.mColor);
				break;
			}

			case Geometry::Type::VIEWPORT:
			{
				const ViewportGeometry& viewportGeometry = static_cast<const ViewportGeometry&>(*geometry);
				mCurrentViewport.set(0, 0, mGameResolution.x, mGameResolution.y);
				mCurrentViewport.intersect(viewportGeometry.mRect);
				setScissor(mCurrentViewport);
				break;
			}

			case Geometry::Type::TEXTURED_RECT:
			{
				renderTexturedRect(static_cast<const TexturedRectGeometry&>(*geometry));
				break;
			}

			default:
				break;	// Blur effect is not supported (yet)
		}

		// Statistics: display list bytes by kind of drawing (see pspge::gStatListBytesByKind)
		{
			int kind = 5;
			if (geometry->getType() == Geometry::Type::PLANE)
				kind = mLastPlaneMode;
			else if (geometry->getType() == Geometry::Type::SPRITE)
			{
				const RenderItem::Type type = static_cast<const SpriteGeometry*>(geometry)->mSpriteInfo.getType();
				kind = (type == RenderItem::Type::VDP_SPRITE) ? 3 : (type == RenderItem::Type::PALETTE_SPRITE || type == RenderItem::Type::COMPONENT_SPRITE) ? 4 : 5;
			}
			pspge::gStatListBytesByKind[kind] += (uint32)std::max(pspge::getListBytesUsed() - listBytesBefore, 0);
			++pspge::gStatDrawsByKind[kind];
		}
	}

	// (no stencil test or pixel mask left over from sprite masks for what comes next)
	mActiveMaskBits = 0;
	mCurrentStencilMode = -1;
	setStencilMode(STENCIL_NORMAL);

	// Put the game screen onto the display buffer, scaled
	pspge::drawGameScreenToDisplay(mGameResolution.x, mGameResolution.y);

	pspge::gLastRenderCpuMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - renderStart).count();
	pspge::gStatRenderCpuMs += pspge::gLastRenderCpuMs;

	// Forget textures not used for a while (only the padded copies take memory)
	if ((mInternal.mFrameNumber & 0x3f) == 0)
	{
		for (auto it = mInternal.mTextures.begin(); it != mInternal.mTextures.end(); )
		{
			if (mInternal.mFrameNumber - it->second.mLastUsedFrame > 60)	// (padded copies are made again each frame anyway)
			{
				mInternal.mTextureCopiesBytes -= it->second.mPaddedCopy.size();
				it = mInternal.mTextures.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

void PSPRenderer::updatePatterns()
{
	// The emulated VRAM's patterns into the 4-bit texture: only those that changed
	//  -> The emulated VRAM holds 16-bit words in little endian, so each pair of bytes is swapped (see RenderUtils::expandPatternDataFromVRAM)
	//  -> The Mega Drive has the left pixel in the high nibble, the GE in the low one
	const uint8* vram = EmulatorInterface::instance().getVRam();
	uint8* texture = pspge::getVRamUncached(pspge::VRAM_PATTERNS);
	const int textureStride = pspge::PATTERNS_TEXTURE_WIDTH / 2;
	const bool updateAll = !mInternal.mPatternsInitialized;

	for (int pattern = 0; pattern < 0x800; ++pattern)
	{
		const uint8* src = &vram[pattern * 32];
		uint8* backup = &mInternal.mPatternBackup[pattern * 32];
		if (!updateAll && memcmp(src, backup, 32) == 0)
			continue;
		memcpy(backup, src, 32);

		uint8* dst = &texture[(pattern >> 6) * 8 * textureStride + (pattern & 63) * 4];
		uint8 any = 0;
		for (int row = 0; row < 8; ++row)
		{
			for (int i = 0; i < 4; ++i)
			{
				const uint8 value = src[row * 4 + (i ^ 1)];
				any |= value;
				dst[row * textureStride + i] = (uint8)((value << 4) | (value >> 4));
			}
		}
		mInternal.mPatternEmpty[pattern] = (any == 0);
	}
	mInternal.mPatternsInitialized = true;
}

void PSPRenderer::buildCluts()
{
	// Pattern CLUTs: the first 64 colors of each main palette, color 0 of each palette line transparent
	const PaletteManager& paletteManager = mRenderParts.getPaletteManager();
	for (int p = 0; p < 2; ++p)
	{
		uint32* clut = (uint32*)pspge::getListMemory(64 * sizeof(uint32), 64);
		const uint32* colors = paletteManager.getMainPalette(p).getRawColors();
		for (int i = 0; i < 64; ++i)
			clut[i] = (i & 0x0f) ? (colors[i] | 0xff000000) : 0;
		mInternal.mPatternCluts[p] = clut;
	}
	mInternal.mLoadedPatternClut = -1;
}

void PSPRenderer::setScissor(const Recti& rect)
{
	if (rect == mCurrentScissor)
		return;
	mCurrentScissor = rect;
	if (rect.width <= 0 || rect.height <= 0)
		sceGuScissor(0, 0, 0, 0);
	else
		sceGuScissor(rect.x, rect.y, rect.width, rect.height);		// (this pspsdk's sceGuScissor takes a size)
}

void PSPRenderer::setStencilMode(int mode)
{
	if (mode == mCurrentStencilMode)
		return;
	mCurrentStencilMode = mode;

	// With sprite masks ahead in the render queue, nothing gets drawn where their bits are set
	//  -> The test is "(ref & mask) == (stencil & mask)", so a ref without the masks' bits means "none of them set"
	const uint8 maskBits = mActiveMaskBits;
	switch (mode)
	{
		case STENCIL_NORMAL:
			if (maskBits != 0)
				sceGuStencilFunc(GU_EQUAL, 0, maskBits);
			else
				sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
			sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
			setPixelMask(0);
			break;

		case STENCIL_WRITE_PRIORITY:
			if (maskBits != 0)
			{
				// Write only bit 0 (the masks' bits stay as they are)
				sceGuStencilFunc(GU_EQUAL, 1, maskBits);
				setPixelMask(0xfe000000);
			}
			else
			{
				sceGuStencilFunc(GU_ALWAYS, 1, 0xff);
				setPixelMask(0);
			}
			sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
			break;

		case STENCIL_BEHIND_PRIORITY:
			sceGuStencilFunc(GU_EQUAL, 0, 1 | maskBits);
			sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
			setPixelMask(0);
			break;
	}
}

void PSPRenderer::setPixelMask(uint32 mask)
{
	// Bits set are not written (in a 32-bit buffer the top 8 bits are the alpha channel, i.e. the stencil)
	if (mask == mCurrentPixelMask)
		return;
	mCurrentPixelMask = mask;
	sceGuPixelMask(mask);
}

void PSPRenderer::collectSpriteMasks(const std::vector<Geometry*>& geometries)
{
	// Sprite masks from render queue 0x8000 on, each with its own stencil bit (1..7; more than 7 share the last one)
	//  -> Earlier ones are left out: the other renderers would use a copy of the screen that wasn't made yet
	mInternal.mSpriteMasks.clear();
	for (const Geometry* geometry : geometries)
	{
		if (geometry->getType() != Geometry::Type::SPRITE || geometry->mRenderQueue < 0x8000)
			continue;
		if (static_cast<const SpriteGeometry*>(geometry)->mSpriteInfo.getType() != RenderItem::Type::SPRITE_MASK)
			continue;
		Internal::SpriteMask& mask = mInternal.mSpriteMasks.emplace_back();
		mask.mGeometry = geometry;
		mask.mBit = (uint8)(2 << std::min((int)mInternal.mSpriteMasks.size() - 1, 6));
	}
}

void PSPRenderer::drawSpriteMaskStencil(const Geometry& maskGeometry, uint8 bit, bool set)
{
	// Sets or clears the mask's stencil bit in its rectangle (only that bit gets written, no color)
	const renderitems::SpriteMaskInfo& mask = static_cast<const renderitems::SpriteMaskInfo&>(static_cast<const SpriteGeometry&>(maskGeometry).mSpriteInfo);
	const Recti rect = Recti::getIntersection(Recti(mask.mInterpolatedPosition, mask.mSize), Recti(0, 0, mGameResolution.x, mGameResolution.y));
	if (set)
		mActiveMaskBits |= bit;
	else
		mActiveMaskBits &= ~bit;
	mCurrentStencilMode = -1;		// (the stencil test depends on the active masks)
	if (rect.width <= 0 || rect.height <= 0)
		return;

	ColorVertex* vertices = (ColorVertex*)pspge::getListMemory(2 * sizeof(ColorVertex));
	if (nullptr == vertices)
		return;

	// (the masks cover the whole screen, regardless of the viewport, as in the software renderer)
	const Recti oldScissor = mCurrentScissor;
	setScissor(Recti(0, 0, mGameResolution.x, mGameResolution.y));
	setPixelMask(~((uint32)bit << 24));
	sceGuStencilFunc(GU_ALWAYS, set ? bit : 0, 0xff);
	sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
	sceGuDisable(GU_TEXTURE_2D);
	vertices[0] = { 0xffffffff, (int16)rect.x, (int16)rect.y, 0, 0 };
	vertices[1] = { 0xffffffff, (int16)(rect.x + rect.width), (int16)(rect.y + rect.height), 0, 0 };
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, vertices);
	sceGuEnable(GU_TEXTURE_2D);
	setScissor(oldScissor);
}

void PSPRenderer::setBlendMode(int blendMode)
{
	if (blendMode == mCurrentBlendMode)
		return;
	mCurrentBlendMode = blendMode;
	switch ((BlendMode)blendMode)
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
			sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);		// (GU_DST_COLOR as source factor: the other color, i.e. the destination)
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


namespace
{
	// A rectangle of one pattern into a bucket: (x, y) on screen, (localX, localY) its first pixel inside the 8x8 pattern,
	// width x height pixels, flipped as the pattern index says
	inline void addPatternRect(std::vector<TileVertex>& bucket, uint16 patternIndex, int x, int y, int localX, int localY, int width, int height)
	{
		const int pattern = patternIndex & 0x07ff;
		const int u = (pattern & 63) * 8;
		const int v = (pattern >> 6) * 8;
		int u0, u1, v0, v1;
		if (patternIndex & 0x0800)
		{
			u0 = u + 8 - localX;
			u1 = u0 - width;
		}
		else
		{
			u0 = u + localX;
			u1 = u0 + width;
		}
		if (patternIndex & 0x1000)
		{
			v0 = v + 8 - localY;
			v1 = v0 - height;
		}
		else
		{
			v0 = v + localY;
			v1 = v0 + height;
		}
		const size_t index = bucket.size();
		bucket.resize(index + 2);
		TileVertex* vertex = &bucket[index];
		vertex[0] = { (uint16)u0, (uint16)v0, (int16)x, (int16)y, 0 };
		vertex[1] = { (uint16)u1, (uint16)v1, (int16)(x + width), (int16)(y + height), 0 };
	}

	// A pattern drawn at (x, y) as a whole, cut to the clip rectangle
	inline void addClippedPattern(std::vector<TileVertex>& bucket, uint16 patternIndex, int x, int y, const Recti& clip)
	{
		const int x0 = std::max(x, clip.x);
		const int y0 = std::max(y, clip.y);
		const int x1 = std::min(x + 8, clip.x + clip.width);
		const int y1 = std::min(y + 8, clip.y + clip.height);
		if (x0 >= x1 || y0 >= y1)
			return;
		addPatternRect(bucket, patternIndex, x0, y0, x0 - x, y0 - y, x1 - x0, y1 - y0);
	}
}

void PSPRenderer::renderPlane(const PlaneGeometry& geometry)
{
	mLastPlaneMode = 0;
	Recti rect(0, 0, mGameResolution.x, mGameResolution.y);
	rect.intersect(geometry.mActiveRect);
	rect.intersect(mCurrentViewport);
	if (rect.width <= 0 || rect.height <= 0)
		return;

	const int minX = rect.x;
	const int maxX = rect.x + rect.width;
	const int minY = rect.y;
	const int maxY = rect.y + rect.height;

	const PlaneManager& planeManager = mRenderParts.getPlaneManager();
	const ScrollOffsetsManager& scrollOffsetsManager = mRenderParts.getScrollOffsetsManager();
	const PaletteManager& paletteManager = mRenderParts.getPaletteManager();

	// Same setup as in the software renderer
	const uint16* planeData = planeManager.getPlaneDataInVRAM(geometry.mPlaneIndex);
	const int numPatternsPerLine = (geometry.mPlaneIndex <= PlaneManager::PLANE_A) ? planeManager.getPlayfieldSizeInPatterns().x : 64;
	const uint16* scrollOffsetsH = nullptr;
	const uint16* scrollOffsetsV = nullptr;
	uint16 scrollMaskH = 0xff;
	uint16 scrollMaskV = 0;
	bool scrollNoRepeat = false;
	uint16 wScrollOffsetX = 0;

	if (geometry.mPlaneIndex == PlaneManager::PLANE_W)
	{
		wScrollOffsetX = (uint16)scrollOffsetsManager.getPlaneWScrollOffset().x;
		scrollOffsetsH = &wScrollOffsetX;
		scrollMaskH = 0;
	}
	else
	{
		scrollOffsetsH = scrollOffsetsManager.getScrollOffsetsH(geometry.mScrollOffsets);
		scrollOffsetsV = scrollOffsetsManager.getScrollOffsetsV(geometry.mScrollOffsets);
		scrollMaskV = scrollOffsetsManager.getVerticalScrolling() ? 0x1f : 0;
		scrollNoRepeat = scrollOffsetsManager.getHorizontalScrollNoRepeat(geometry.mScrollOffsets);
	}
	const int positionMaskH = planeManager.getPlayfieldSizeInPixels().x - 1;
	const int positionMaskV = planeManager.getPlayfieldSizeInPixels().y - 1;
	const int16 verticalScrollOffsetBias = scrollOffsetsManager.getVerticalScrollOffsetBias();
	const int splitY = paletteManager.mSplitPositionY;
	const bool priority = geometry.mPriorityFlag;
	const bool* patternEmpty = mInternal.mPatternEmpty;

	for (int i = 0; i < 8; ++i)
		mInternal.mBuckets[i].clear();

	struct LineSetup
	{
		int vx;			// Plane x of the first pixel (not masked yet)
		int startX;
		int endX;
		int vy;			// Plane y (masked), only without vertical scrolling per column
		int palette;
	};
	auto getLineSetup = [&](int y, LineSetup& out) -> bool
	{
		int vx = minX;
		if (nullptr != scrollOffsetsH)
			vx += (int16)scrollOffsetsH[y & scrollMaskH];
		int startX = minX;
		int endX = maxX;
		if (scrollNoRepeat)
		{
			if (vx > 0x0800)
			{
				startX += 0x1000 - vx;
				vx = 0;
			}
			else if (endX > startX + (positionMaskH - vx))
			{
				endX = startX + (positionMaskH - vx) + 1;
			}
			if (startX >= endX)
				return false;
		}
		out.vx = vx;
		out.startX = startX;
		out.endX = endX;
		out.vy = ((nullptr == scrollOffsetsV) ? y : (y + scrollOffsetsV[0])) & positionMaskV;
		out.palette = (y < splitY) ? 0 : 1;
		return true;
	};

	// Planes drawn through a texture, for scrolling that would otherwise take thousands of small rectangles:
	//  - Lots of lines with their own horizontal scroll (e.g. the heat over the burning forest in AIZ): pattern by pattern
	//    on each line, that's ~11,000 rectangles per plane, and ~10-20 ms of CPU per frame
	//  - Vertical scroll per column of 16 pixels (e.g. the rising fire in AIZ 1)
	//  -> The plane goes into a texture once, without that scroll, and then gets copied in strips with their own scroll
	static const bool planeTextureDisabled = (nullptr != getenv("S3AIR_PSP_NO_PLANE_TEXTURE"));		// (test switch)
	const int planeWidth = positionMaskH + 1;
	const int planeHeight = positionMaskV + 1;
	if (!planeTextureDisabled && planeWidth <= pspge::PLANE_TEXTURE_WIDTH)
	{
		int lineScrollChanges = 0;
		int lastScroll = -0x10000;
		for (int y = minY; y < maxY; ++y)
		{
			const int scroll = (nullptr != scrollOffsetsH) ? (int16)scrollOffsetsH[y & scrollMaskH] : 0;
			if (scroll != lastScroll)
			{
				++lineScrollChanges;
				lastScroll = scroll;
			}
		}

		// Mode 1: lines scrolled horizontally -> texture row t is the plane line of screen line minY + t (so the palette split can be done per screen line)
		// Mode 2: vertical scroll per column -> the whole plane as it is (only without a palette split on screen)
		const int height = maxY - minY;
		const bool singlePalette = (splitY <= minY || splitY >= maxY);
		static const bool forceMode2 = (nullptr != getenv("S3AIR_PSP_FORCE_PLANE_TEXTURE2"));		// (test switch: the same image as without, for any plane)
		int mode = 0;
		if (!forceMode2 && scrollMaskV == 0 && lineScrollChanges > 40 && height <= pspge::PLANE_TEXTURE_HEIGHT - 8)
			mode = 1;
		else if ((scrollMaskV != 0 || forceMode2) && nullptr != scrollOffsetsV && singlePalette && planeHeight <= pspge::PLANE_TEXTURE_HEIGHT)
			mode = 2;
		mLastPlaneMode = mode;

		if (mode != 0)
		{
			// Collect the plane's patterns (of this priority) for the texture
			const int patternRows = planeHeight / 8;
			if (mode == 1)
			{
				const int firstPlaneY = ((nullptr == scrollOffsetsV) ? minY : (minY + scrollOffsetsV[0])) & positionMaskV;
				const int splitRow = clamp(splitY - minY, 0, height);
				const Recti clips[2] = { Recti(0, 0, planeWidth, splitRow), Recti(0, splitRow, planeWidth, height - splitRow) };
				for (int k = 0; k * 8 - (firstPlaneY & 7) < height; ++k)
				{
					const int textureY = k * 8 - (firstPlaneY & 7);
					const uint16* rowData = &planeData[(((firstPlaneY >> 3) + k) % patternRows) * numPatternsPerLine];
					for (int column = 0; column < planeWidth / 8; ++column)
					{
						const uint16 patternIndex = rowData[column];
						if (((patternIndex & 0x8000) != 0) != priority || patternEmpty[patternIndex & 0x07ff])
							continue;
						for (int palette = 0; palette < 2; ++palette)
						{
							if (clips[palette].height > 0)
								addClippedPattern(mInternal.mBuckets[palette * 4 + ((patternIndex >> 13) & 3)], patternIndex, column * 8, textureY, clips[palette]);
						}
					}
				}
			}
			else
			{
				const int palette = (splitY <= minY) ? 1 : 0;
				for (int row = 0; row < patternRows; ++row)
				{
					const uint16* rowData = &planeData[row * numPatternsPerLine];
					for (int column = 0; column < planeWidth / 8; ++column)
					{
						const uint16 patternIndex = rowData[column];
						if (((patternIndex & 0x8000) != 0) != priority || patternEmpty[patternIndex & 0x07ff])
							continue;
						addPatternRect(mInternal.mBuckets[palette * 4 + ((patternIndex >> 13) & 3)], patternIndex, column * 8, row * 8, 0, 0, 8, 8);
					}
				}
			}
			const int textureHeight = (mode == 1) ? height : planeHeight;

			// Draw them into the texture, cleared to transparent first (with a rectangle: sceGuClear only covers the screen's width)
			//  -> In a 32-bit buffer, the GE's alpha channel is the stencil: it only gets written by stencil operations, so
			//     these write alpha 0 for the clear and 0xff for each drawn (opaque) pixel
			const Recti oldScissor = mCurrentScissor;
			sceGuDrawBufferList(GU_PSM_8888, pspge::getGeAddress(pspge::VRAM_PLANE_TEXTURE), pspge::PLANE_TEXTURE_WIDTH);
			setBlendMode((int)BlendMode::OPAQUE);
			mCurrentScissor.set(-1, -1, 0, 0);
			setScissor(Recti(0, 0, planeWidth, textureHeight));
			mCurrentStencilMode = -1;
			setPixelMask(0);	// (all of the alpha channel, whatever sprite masks are active on the game screen)
			sceGuStencilOp(GU_REPLACE, GU_REPLACE, GU_REPLACE);
			{
				ColorVertex* vertices = (ColorVertex*)pspge::getListMemory(2 * sizeof(ColorVertex));
				if (nullptr != vertices)
				{
					sceGuStencilFunc(GU_ALWAYS, 0, 0xff);
					sceGuDisable(GU_TEXTURE_2D);
					sceGuDisable(GU_ALPHA_TEST);
					vertices[0] = { 0, 0, 0, 0, 0 };
					vertices[1] = { 0, (int16)planeWidth, (int16)textureHeight, 0, 0 };
					sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, vertices);
					sceGuEnable(GU_ALPHA_TEST);
					sceGuEnable(GU_TEXTURE_2D);
				}
			}
			sceGuStencilFunc(GU_ALWAYS, 0xff, 0xff);
			drawPatternBuckets();

			// Back to the game screen, and the strips from the texture
			sceGuDrawBufferList(GU_PSM_8888, pspge::getGeAddress(pspge::VRAM_GAME), pspge::GAME_STRIDE);
			mCurrentScissor.set(-1, -1, 0, 0);
			setScissor(oldScissor);
			setStencilMode(priority ? STENCIL_WRITE_PRIORITY : STENCIL_NORMAL);
			setBlendMode((int)BlendMode::OPAQUE);
			if (priority)
				mAnyPrioPlaneDrawn = true;

			std::vector<TileVertex>& strips = mInternal.mBuckets[0];
			strips.clear();
			auto addStrip = [&](int u, int v, int x, int y, int width, int stripHeight)
			{
				strips.push_back({ (uint16)u, (uint16)v, (int16)x, (int16)y, 0 });
				strips.push_back({ (uint16)(u + width), (uint16)(v + stripHeight), (int16)(x + width), (int16)(y + stripHeight), 0 });
			};

			if (mode == 1)
			{
				// One strip per line, from that line's position in the plane (two where it wraps around)
				for (int y = minY; y < maxY; ++y)
				{
					LineSetup line;
					if (!getLineSetup(y, line))
						continue;
					int x = line.startX;
					int remaining = line.endX - line.startX;
					int u = line.vx & positionMaskH;
					while (remaining > 0)
					{
						const int length = std::min(remaining, planeWidth - u);
						addStrip(u, y - minY, x, y, length, 1);
						x += length;
						remaining -= length;
						u = 0;
					}
				}
			}
			else
			{
				// Vertical scroll per 16-pixel column: runs of lines with the same horizontal scroll, and in each of them runs
				// of columns with the same vertical scroll, one strip each (cut where the plane wraps around)
				//  -> E.g. the LRZ 2 boss has both, with the heat haze moving each line by 0 or 1 pixel: a strip per line and
				//     column were ~5,800 strips per plane, ~18 ms of CPU per frame for its four plane passes
				auto addColumnStrips = [&](const LineSetup& line, int y0, int y1)
				{
					int vx = line.vx;
					for (int x = line.startX; x < line.endX; )
					{
						const int u = vx & positionMaskH;
						const int column = (x - verticalScrollOffsetBias) >> 4;
						const uint16 verticalScrollOffset = scrollOffsetsV[column & scrollMaskV];
						int columnsEnd = (column + 1) * 16 + verticalScrollOffsetBias;
						while (columnsEnd < line.endX && scrollOffsetsV[((columnsEnd - verticalScrollOffsetBias) >> 4) & scrollMaskV] == verticalScrollOffset)
							columnsEnd += 16;
						const int width = std::min(std::min(columnsEnd, line.endX) - x, planeWidth - u);
						int y = y0;
						int v = (y + (int16)verticalScrollOffset) & positionMaskV;
						while (y < y1)
						{
							const int stripHeight = std::min(y1 - y, planeHeight - v);
							addStrip(u, v, x, y, width, stripHeight);
							y += stripHeight;
							v = 0;
						}
						x += width;
						vx += width;
					}
				};
				for (int y = minY; y < maxY; )
				{
					LineSetup line;
					if (!getLineSetup(y, line))
					{
						++y;
						continue;
					}
					int runEnd = y + 1;
					LineSetup next;
					while (runEnd < maxY && getLineSetup(runEnd, next) && next.vx == line.vx && next.startX == line.startX && next.endX == line.endX)
						++runEnd;
					addColumnStrips(line, y, runEnd);
					y = runEnd;
				}
			}

			if (!strips.empty())
			{
				TileVertex* vertices = (TileVertex*)pspge::getListMemory((int)(strips.size() * sizeof(TileVertex)));
				if (nullptr != vertices)
				{
					memcpy(vertices, &strips[0], strips.size() * sizeof(TileVertex));
					sceGuTexMode(GU_PSM_8888, 0, 0, 0);
					sceGuTexImage(0, pspge::PLANE_TEXTURE_WIDTH, 256, pspge::PLANE_TEXTURE_WIDTH, (uint8*)sceGeEdramGetAddr() + pspge::VRAM_PLANE_TEXTURE);
					sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
					sceGuTexFlush();
					sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, (int)strips.size(), 0, vertices);
				}
				mInternal.mPatternTextureBound = false;
				mInternal.mBoundTexture = nullptr;
			}
			strips.clear();
			return;
		}
	}

	for (int y = minY; y < maxY; )
	{
		LineSetup line;
		if (!getLineSetup(y, line))
		{
			++y;
			continue;
		}

		if (scrollMaskV != 0)
		{
			// Vertical scrolling per column of 16 pixels: one line at a time, like the software renderer
			int vx = line.vx;
			for (int x = line.startX; x < line.endX; )
			{
				vx &= positionMaskH;
				const int verticalScrollOffset = scrollOffsetsV[((x - verticalScrollOffsetBias) >> 4) & scrollMaskV];
				const int vy = (y + verticalScrollOffset) & positionMaskV;
				const uint16 patternIndex = planeData[(vx / 8) + (vy / 8) * numPatternsPerLine];
				const int vxMod8 = vx & 0x07;
				const int pixels = std::min(8 - vxMod8, line.endX - x);
				if (((patternIndex & 0x8000) != 0) == priority && !patternEmpty[patternIndex & 0x07ff])
				{
					addPatternRect(mInternal.mBuckets[line.palette * 4 + ((patternIndex >> 13) & 3)], patternIndex, x, y, vxMod8, vy & 0x07, pixels, 1);
				}
				x += pixels;
				vx += pixels;
			}
			++y;
			continue;
		}

		// A run of lines with the same setup, inside one row of patterns
		int numLines = 1;
		const int maxLines = std::min(8 - (line.vy & 0x07), maxY - y);
		while (numLines < maxLines)
		{
			LineSetup next;
			if (!getLineSetup(y + numLines, next))
				break;
			if (next.vx != line.vx || next.startX != line.startX || next.endX != line.endX || next.palette != line.palette || next.vy != line.vy + numLines)
				break;
			++numLines;
		}

		const uint16* rowData = &planeData[(line.vy / 8) * numPatternsPerLine];
		const int localY = line.vy & 0x07;
		std::vector<TileVertex>* buckets = &mInternal.mBuckets[line.palette * 4];
		int vx = line.vx & positionMaskH;
		for (int x = line.startX; x < line.endX; )
		{
			const int vxMod8 = vx & 0x07;
			const int pixels = std::min(8 - vxMod8, line.endX - x);
			const uint16 patternIndex = rowData[vx / 8];
			if (((patternIndex & 0x8000) != 0) == priority && !patternEmpty[patternIndex & 0x07ff])
			{
				addPatternRect(buckets[(patternIndex >> 13) & 3], patternIndex, x, y, vxMod8, localY, pixels, numLines);
			}
			x += pixels;
			vx = (vx + pixels) & positionMaskH;
		}
		y += numLines;
	}

	// Draw the collected rectangles
	setStencilMode(priority ? STENCIL_WRITE_PRIORITY : STENCIL_NORMAL);
	setBlendMode((int)BlendMode::OPAQUE);
	if (priority)
		mAnyPrioPlaneDrawn = true;
	drawPatternBuckets();
}

void PSPRenderer::drawPatternBuckets()
{
	// The rectangles collected in the buckets (one per palette and palette line), with the patterns texture
	if (!mInternal.mPatternTextureBound)
	{
		sceGuTexMode(GU_PSM_T4, 0, 0, 0);
		sceGuTexImage(0, pspge::PATTERNS_TEXTURE_WIDTH, pspge::PATTERNS_TEXTURE_HEIGHT, pspge::PATTERNS_TEXTURE_WIDTH, (uint8*)sceGeEdramGetAddr() + pspge::VRAM_PATTERNS);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
		sceGuTexFlush();
		mInternal.mPatternTextureBound = true;
		mInternal.mBoundTexture = nullptr;
	}
	for (int palette = 0; palette < 2; ++palette)
	{
		for (int paletteLine = 0; paletteLine < 4; ++paletteLine)
		{
			std::vector<TileVertex>& bucket = mInternal.mBuckets[palette * 4 + paletteLine];
			if (bucket.empty())
				continue;
			if (mInternal.mLoadedPatternClut != palette)
			{
				sceGuClutLoad(64 / 8, mInternal.mPatternCluts[palette]);
				mInternal.mLoadedPatternClut = palette;
			}
			sceGuClutMode(GU_PSM_8888, 0, 0x0f, paletteLine);
			TileVertex* vertices = (TileVertex*)pspge::getListMemory((int)(bucket.size() * sizeof(TileVertex)));
			if (nullptr == vertices)
				continue;
			memcpy(vertices, &bucket[0], bucket.size() * sizeof(TileVertex));
			sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, (int)bucket.size(), 0, vertices);
		}
	}
}

void PSPRenderer::renderVdpSprite(const SpriteGeometry& geometry)
{
	const renderitems::VdpSpriteInfo& sprite = static_cast<const renderitems::VdpSpriteInfo&>(geometry.mSpriteInfo);
	const int width = sprite.mSize.x;
	const int height = sprite.mSize.y;
	const Vec2i position = sprite.mInterpolatedPosition;
	const int splitY = mRenderParts.getPaletteManager().mSplitPositionY;
	const bool flipX = (sprite.mFirstPattern & 0x0800) != 0;
	const bool flipY = (sprite.mFirstPattern & 0x1000) != 0;

	for (int i = 0; i < 8; ++i)
		mInternal.mBuckets[i].clear();

	// The parts above and below the palette split use the two palettes
	Recti clips[2];
	clips[0] = Recti::getIntersection(mCurrentViewport, Recti(0, 0, mGameResolution.x, std::max(0, std::min(splitY, mGameResolution.y))));
	clips[1] = Recti::getIntersection(mCurrentViewport, Recti(0, splitY, mGameResolution.x, mGameResolution.y - splitY));

	for (int px = 0; px < width; ++px)
	{
		for (int py = 0; py < height; ++py)
		{
			// Same pattern selection as the software renderer (flip bits and palette taken from the sum)
			const int patternX = flipX ? (width - px - 1) : px;
			const int patternY = flipY ? (height - py - 1) : py;
			const uint16 patternIndex = sprite.mFirstPattern + patternY + patternX * height;
			if (mInternal.mPatternEmpty[patternIndex & 0x07ff])
				continue;

			const int x = position.x + px * 8;
			const int y = position.y + py * 8;
			for (int palette = 0; palette < 2; ++palette)
			{
				if (clips[palette].width > 0 && clips[palette].height > 0)
					addClippedPattern(mInternal.mBuckets[palette * 4 + ((patternIndex >> 13) & 3)], patternIndex, x, y, clips[palette]);
			}
		}
	}

	setStencilMode((sprite.mPriorityFlag || !mAnyPrioPlaneDrawn) ? STENCIL_NORMAL : STENCIL_BEHIND_PRIORITY);
	const bool useTint = (sprite.mTintColor != Color::WHITE);
	setBlendMode(useTint ? (int)BlendMode::ALPHA : (int)BlendMode::OPAQUE);
	if (!mInternal.mPatternTextureBound)
	{
		sceGuTexMode(GU_PSM_T4, 0, 0, 0);
		sceGuTexImage(0, pspge::PATTERNS_TEXTURE_WIDTH, pspge::PATTERNS_TEXTURE_HEIGHT, pspge::PATTERNS_TEXTURE_WIDTH, (uint8*)sceGeEdramGetAddr() + pspge::VRAM_PATTERNS);
		sceGuTexFlush();
		mInternal.mPatternTextureBound = true;
		mInternal.mBoundTexture = nullptr;
	}
	if (useTint)
	{
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		sceGuColor(toABGR32(sprite.mTintColor));
	}
	else
	{
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	}

	for (int palette = 0; palette < 2; ++palette)
	{
		for (int paletteLine = 0; paletteLine < 4; ++paletteLine)
		{
			std::vector<TileVertex>& bucket = mInternal.mBuckets[palette * 4 + paletteLine];
			if (bucket.empty())
				continue;
			if (mInternal.mLoadedPatternClut != palette)
			{
				sceGuClutLoad(64 / 8, mInternal.mPatternCluts[palette]);
				mInternal.mLoadedPatternClut = palette;
			}
			sceGuClutMode(GU_PSM_8888, 0, 0x0f, paletteLine);
			TileVertex* vertices = (TileVertex*)pspge::getListMemory((int)(bucket.size() * sizeof(TileVertex)));
			if (nullptr == vertices)
				continue;
			memcpy(vertices, &bucket[0], bucket.size() * sizeof(TileVertex));
			sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, (int)bucket.size(), 0, vertices);
		}
	}

	if (useTint)
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
}

namespace
{
	// The GE reads textures much faster in its "swizzled" layout: blocks of 16 bytes x 8 rows, left to right, then down
	//  -> Copies "height" rows of "widthBytes" each, padded with zeroes to "bufferWidthBytes" (multiple of 16) and "paddedHeight" (multiple of 8)
	void copySwizzled(uint8* dst, const uint8* src, int widthBytes, int height, int bufferWidthBytes, int paddedHeight)
	{
		uint32* out = (uint32*)dst;
		for (int blockY = 0; blockY < paddedHeight; blockY += 8)
		{
			for (int blockX = 0; blockX < bufferWidthBytes; blockX += 16)
			{
				for (int row = 0; row < 8; ++row)
				{
					uint32 block[4] = { 0, 0, 0, 0 };
					const int y = blockY + row;
					if (y < height && blockX < widthBytes)
						memcpy(block, src + y * widthBytes + blockX, std::min(16, widthBytes - blockX));
					out[0] = block[0];
					out[1] = block[1];
					out[2] = block[2];
					out[3] = block[3];
					out += 4;
				}
			}
		}
	}
}

bool PSPRenderer::bindTextureForBitmap(const void* data, int width, int height, int bytesPerPixel, int psm, uint32 changeCounter)
{
	if (nullptr == data || width <= 0 || height <= 0)
		return false;
	if (width > 512 || height > 512)
	{
		if (!mInternal.mLoggedTooLargeSprite)
		{
			RMX_LOG_INFO("PSP: sprite too large for the GE (" << width << " x " << height << "), not drawn");
			mInternal.mLoggedTooLargeSprite = true;
		}
		return false;
	}

	Internal::TextureEntry& entry = mInternal.mTextures[data];
	entry.mLastUsedFrame = mInternal.mFrameNumber;
	// (a padded copy is made again once per frame it's used in: sprites can change their pixels without a new change counter)
	const bool copyOutdated = !entry.mPaddedCopy.empty() && !entry.mSwizzled && entry.mLastCopiedFrame != mInternal.mFrameNumber;
	if (nullptr == entry.mTextureData || entry.mChangeCounter != changeCounter || entry.mSourceWidth != width || entry.mSourceHeight != height || copyOutdated)
	{
		// The GE wants the buffer width in multiples of 16 bytes and the data 16-byte aligned
		const int alignmentPixels = 16 / bytesPerPixel;
		const bool usable = ((uintptr_t)data & 15) == 0 && (width % alignmentPixels) == 0;
		mInternal.mTextureCopiesBytes -= entry.mPaddedCopy.size();
		entry.mSwizzled = false;
		if (bytesPerPixel == 4)
		{
			// 32-bit sprites (from image files, they don't change): a swizzled copy, the linear layout from main memory is very slow for the GE
			const int bufferWidth = (width + alignmentPixels - 1) / alignmentPixels * alignmentPixels;
			const int paddedHeight = (height + 7) & ~7;
			entry.mPaddedCopy.resize((size_t)bufferWidth * paddedHeight * bytesPerPixel + 16);
			uint8* dst = (uint8*)(((uintptr_t)&entry.mPaddedCopy[0] + 15) & ~(uintptr_t)15);
			copySwizzled(dst, (const uint8*)data, width * bytesPerPixel, height, bufferWidth * bytesPerPixel, paddedHeight);
			sceKernelDcacheWritebackRange(dst, bufferWidth * paddedHeight * bytesPerPixel);
			entry.mTextureData = dst;
			entry.mBufferWidth = bufferWidth;
			entry.mLastCopiedFrame = mInternal.mFrameNumber;
			entry.mSwizzled = true;
			mInternal.mTextureCopiesBytes += entry.mPaddedCopy.size();
		}
		else if (usable)
		{
			std::vector<uint8>().swap(entry.mPaddedCopy);
			entry.mTextureData = data;
			entry.mBufferWidth = width;
		}
		else
		{
			const int bufferWidth = (width + alignmentPixels - 1) / alignmentPixels * alignmentPixels;
			entry.mPaddedCopy.resize((size_t)bufferWidth * height * bytesPerPixel + 16);
			uint8* dst = (uint8*)(((uintptr_t)&entry.mPaddedCopy[0] + 15) & ~(uintptr_t)15);
			for (int y = 0; y < height; ++y)
			{
				memcpy(&dst[y * bufferWidth * bytesPerPixel], (const uint8*)data + y * width * bytesPerPixel, width * bytesPerPixel);
				memset(&dst[(y * bufferWidth + width) * bytesPerPixel], 0, (bufferWidth - width) * bytesPerPixel);
			}
			sceKernelDcacheWritebackRange(dst, bufferWidth * height * bytesPerPixel);
			entry.mTextureData = dst;
			entry.mBufferWidth = bufferWidth;
			entry.mLastCopiedFrame = mInternal.mFrameNumber;
			mInternal.mTextureCopiesBytes += entry.mPaddedCopy.size();
		}
		entry.mSourceWidth = width;
		entry.mSourceHeight = height;
		entry.mTextureWidth = nextPowerOfTwo(width);
		entry.mTextureHeight = nextPowerOfTwo(height);
		entry.mChangeCounter = changeCounter;
		if (entry.mTextureData == mInternal.mBoundTexture)
			mInternal.mBoundTexture = nullptr;
	}

	// Used directly, without a copy: the CPU wrote these pixels through its data cache, the GE reads them from memory
	//  -> Without writing the cache back, the GE can draw what was there before for a frame (e.g. a character sprite that was
	//     just rotated, "upscaled" for that); once per frame it's used in, the GE's texture cache gets flushed as well
	if (entry.mPaddedCopy.empty() && entry.mTextureData == data && entry.mLastCopiedFrame != mInternal.mFrameNumber)
	{
		sceKernelDcacheWritebackRange(data, width * height * bytesPerPixel);
		entry.mLastCopiedFrame = mInternal.mFrameNumber;
		if (entry.mTextureData == mInternal.mBoundTexture)
			mInternal.mBoundTexture = nullptr;
	}

	if (entry.mTextureData != mInternal.mBoundTexture)
	{
		sceGuTexMode(psm, 0, 0, entry.mSwizzled ? 1 : 0);
		sceGuTexImage(0, entry.mTextureWidth, entry.mTextureHeight, entry.mBufferWidth, entry.mTextureData);
		sceGuTexFlush();
		mInternal.mBoundTexture = entry.mTextureData;
		mInternal.mPatternTextureBound = false;
	}
	return true;
}

const uint32* PSPRenderer::getPaletteClut(const PaletteBase& palette, int offset, int count, bool opaque)
{
	// A copy in the display list: the palette can still change during the frame
	uint32* clut = (uint32*)pspge::getListMemory(256 * sizeof(uint32), 64);
	if (nullptr == clut)
	{
		// The display list is full (the sprite won't get drawn anyway)
		static uint32 fallback[256] __attribute__((aligned(64)));
		clut = fallback;
	}
	const uint32* colors = palette.getRawColors();
	const int available = std::max(0, std::min(count, (int)palette.getSize() - offset));
	clut[0] = (opaque && available > 0) ? colors[offset] : 0;
	for (int i = 1; i < available; ++i)
		clut[i] = colors[offset + i];
	for (int i = std::max(available, 1); i < 256; ++i)
		clut[i] = 0;
	return clut;
}

void PSPRenderer::renderCustomSprite(const SpriteGeometry& geometry)
{
	const renderitems::CustomSpriteInfoBase& spriteBase = static_cast<const renderitems::CustomSpriteInfoBase&>(geometry.mSpriteInfo);
	if (nullptr == spriteBase.mCacheItem || nullptr == spriteBase.mCacheItem->mSprite)
		return;
	SpriteCollection::instance().touchSprite(*spriteBase.mCacheItem);	// (a mod sprite's pixels may need loading again)

	const PaletteManager& paletteManager = mRenderParts.getPaletteManager();
	const bool isPaletteSprite = (geometry.mSpriteInfo.getType() == RenderItem::Type::PALETTE_SPRITE);
	const uint32 changeCounter = spriteBase.mCacheItem->mChangeCounter;

	Vec2i size;
	Vec2i offset;
	if (isPaletteSprite)
	{
		// When rotating, the sprite manager asks for the 3x upscaled sprite (and scales the transformation down to match), like the software renderer
		const PaletteSprite& paletteSprite = *static_cast<PaletteSprite*>(spriteBase.mCacheItem->mSprite);
		const bool useUpscaled = static_cast<const renderitems::PaletteSpriteInfo&>(spriteBase).mUseUpscaledSprite;
		const PaletteBitmap& bitmap = useUpscaled ? paletteSprite.getUpscaledBitmap() : paletteSprite.getBitmap();
		if (!bindTextureForBitmap(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 1, GU_PSM_T8, changeCounter))
			return;
		size = bitmap.getSize();
		offset = paletteSprite.mOffset;
	}
	else
	{
		const ComponentSprite& componentSprite = *static_cast<ComponentSprite*>(spriteBase.mCacheItem->mSprite);
		const Bitmap& bitmap = componentSprite.getBitmap();
		if (!bindTextureForBitmap(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, GU_PSM_8888, changeCounter))
			return;
		size = bitmap.getSize();
		offset = componentSprite.mOffset;
	}

	// Tint, and the added color (a second pass, see "drawWithAddedColor")
	Vec4f tintColor = spriteBase.mTintColor;
	Vec4f addedColor = spriteBase.mAddedColor;
	if (spriteBase.mUseGlobalComponentTint)
		paletteManager.applyGlobalComponentTint(tintColor, addedColor);
	const bool useTint = (tintColor != Vec4f(1.0f, 1.0f, 1.0f, 1.0f));
	if (useTint)
	{
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		sceGuColor(toABGR32(Color(tintColor.x, tintColor.y, tintColor.z, tintColor.w)));
	}
	else
	{
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	}

	setStencilMode((spriteBase.mPriorityFlag || !mAnyPrioPlaneDrawn) ? STENCIL_NORMAL : STENCIL_BEHIND_PRIORITY);
	setBlendMode((int)spriteBase.mBlendMode);

	// Opaque sprites are drawn as a whole, transparent pixels and palette color 0 included (as in the OpenGL renderer, e.g. the act select's background)
	const bool opaque = (spriteBase.mBlendMode == BlendMode::OPAQUE);
	if (opaque)
		sceGuDisable(GU_ALPHA_TEST);

	// Geometry: a rectangle without transformation, else two triangles
	const Vec2i position = spriteBase.mInterpolatedPosition;
	const bool hasTransform = !spriteBase.mTransformation.isIdentity();
	auto drawSpriteGeometry = [&]()
	{
		if (!hasTransform)
		{
			const Recti& clip = mCurrentScissor;
			const int x = position.x + offset.x;
			const int y = position.y + offset.y;
			const int x0 = std::max(x, clip.x);
			const int y0 = std::max(y, clip.y);
			const int x1 = std::min(x + size.x, clip.x + clip.width);
			const int y1 = std::min(y + size.y, clip.y + clip.height);
			if (x0 >= x1 || y0 >= y1)
				return;
			TileVertex* vertices = (TileVertex*)pspge::getListMemory(2 * sizeof(TileVertex));
			if (nullptr == vertices)
				return;
			vertices[0] = { (uint16)(x0 - x), (uint16)(y0 - y), (int16)x0, (int16)y0, 0 };
			vertices[1] = { (uint16)(x1 - x), (uint16)(y1 - y), (int16)x1, (int16)y1, 0 };
			sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, vertices);
		}
		else
		{
			const Vec4f& m = spriteBase.mTransformation.mMatrix;
			FloatVertex* vertices = (FloatVertex*)pspge::getListMemory(4 * sizeof(FloatVertex));
			if (nullptr == vertices)
				return;
			const float corners[4][2] = { { 0.0f, 0.0f }, { (float)size.x, 0.0f }, { 0.0f, (float)size.y }, { (float)size.x, (float)size.y } };
			for (int i = 0; i < 4; ++i)
			{
				const float lx = corners[i][0] + (float)offset.x;
				const float ly = corners[i][1] + (float)offset.y;
				vertices[i].u = corners[i][0];
				vertices[i].v = corners[i][1];
				vertices[i].x = (float)position.x + lx * m.x + ly * m.y;
				vertices[i].y = (float)position.y + lx * m.z + ly * m.w;
				vertices[i].z = 0.0f;
			}
			sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, vertices);
		}
	};

	// The added color (e.g. the title screen's white flash: tint towards black plus added white) as a second pass on top
	//  -> Texture function "blend" with the added color as both fragment and environment color gives exactly that color, with
	//     the sprite's alpha; added onto what the first pass drew
	const bool useAddedColor = (addedColor.x > 0.002f || addedColor.y > 0.002f || addedColor.z > 0.002f);
	auto drawWithAddedColor = [&]()
	{
		drawSpriteGeometry();
		if (!useAddedColor)
			return;

		const uint32 added = toABGR32(Color(addedColor.x, addedColor.y, addedColor.z, tintColor.w));
		sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
		sceGuColor(added);
		sceGuTexEnvColor(added & 0x00ffffff);
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffff);
		drawSpriteGeometry();

		mCurrentBlendMode = -1;
		setBlendMode((int)spriteBase.mBlendMode);
		if (useTint)
		{
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuColor(toABGR32(Color(tintColor.x, tintColor.y, tintColor.z, tintColor.w)));
		}
		else
		{
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
		}
	};

	if (isPaletteSprite)
	{
		const renderitems::PaletteSpriteInfo& spriteInfo = static_cast<const renderitems::PaletteSpriteInfo&>(spriteBase);
		const PaletteBase& primaryPalette = (nullptr == spriteInfo.mPrimaryPalette) ? paletteManager.getMainPalette(0) : *spriteInfo.mPrimaryPalette;
		const int splitY = paletteManager.mSplitPositionY;
		const Recti viewport = mCurrentViewport;

		sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
		sceGuClutLoad(256 / 8, getPaletteClut(primaryPalette, spriteInfo.mAtex, 256, opaque));
		mInternal.mLoadedPatternClut = -1;
		if (splitY < mGameResolution.y)
		{
			// Underwater part with the secondary palette
			setScissor(Recti::getIntersection(viewport, Recti(0, 0, mGameResolution.x, std::max(0, splitY))));
			drawWithAddedColor();

			const PaletteBase& secondaryPalette = (nullptr == spriteInfo.mSecondaryPalette) ? paletteManager.getMainPalette(1) : *spriteInfo.mSecondaryPalette;
			sceGuClutLoad(256 / 8, getPaletteClut(secondaryPalette, spriteInfo.mAtex, 256, opaque));
			setScissor(Recti::getIntersection(viewport, Recti(0, splitY, mGameResolution.x, mGameResolution.y - splitY)));
			drawWithAddedColor();
			setScissor(viewport);
		}
		else
		{
			drawWithAddedColor();
		}
	}
	else
	{
		drawWithAddedColor();
	}

	if (useTint)
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	if (opaque)
		sceGuEnable(GU_ALPHA_TEST);
}

void PSPRenderer::renderTexturedRect(const TexturedRectGeometry& geometry)
{
	// A texture (e.g. a menu's image) into the game screen, unscaled and alpha blended, like in the software renderer
	Bitmap& bitmap = geometry.mDrawerTexture.accessBitmap();
	if (bitmap.empty())
		return;
	// (its pixels can change any time, e.g. level previews: it gets copied again each frame)
	if (!bindTextureForBitmap(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, GU_PSM_8888, mInternal.mFrameNumber))
		return;

	setStencilMode(STENCIL_NORMAL);
	setBlendMode((int)BlendMode::ALPHA);
	const bool useTint = (geometry.mTintColor != Color::WHITE);
	if (useTint)
	{
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		sceGuColor(toABGR32(geometry.mTintColor));
	}

	TileVertex* vertices = (TileVertex*)pspge::getListMemory(2 * sizeof(TileVertex));
	if (nullptr != vertices)
	{
		const int x = geometry.mRect.x;
		const int y = geometry.mRect.y;
		vertices[0] = { 0, 0, (int16)x, (int16)y, 0 };
		vertices[1] = { (uint16)bitmap.getWidth(), (uint16)bitmap.getHeight(), (int16)(x + bitmap.getWidth()), (int16)(y + bitmap.getHeight()), 0 };
		sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, vertices);
	}

	if (useTint)
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
}

void PSPRenderer::renderRect(const Recti& rect, const Color& color)
{
	const Recti clipped = Recti::getIntersection(rect, mCurrentViewport);
	if (clipped.width <= 0 || clipped.height <= 0)
		return;

	setStencilMode(STENCIL_NORMAL);
	setBlendMode((int)BlendMode::ALPHA);
	sceGuDisable(GU_TEXTURE_2D);
	ColorVertex* vertices = (ColorVertex*)pspge::getListMemory(2 * sizeof(ColorVertex));
	if (nullptr == vertices)
	{
		sceGuEnable(GU_TEXTURE_2D);
		return;
	}
	const uint32 abgr = toABGR32(color);
	vertices[0] = { abgr, (int16)clipped.x, (int16)clipped.y, 0, 0 };
	vertices[1] = { abgr, (int16)(clipped.x + clipped.width), (int16)(clipped.y + clipped.height), 0, 0 };
	sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, vertices);
	sceGuEnable(GU_TEXTURE_2D);
}

#endif
