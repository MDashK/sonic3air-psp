/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/rendering/sprite/SpriteBase.h"
#include "oxygen/rendering/utils/RenderUtils.h"


class PaletteSprite : public SpriteBase
{
public:
	void clear();
	void create(const Vec2i& size, const Vec2i& offset);
	void createFromBitmap(const PaletteBitmap& bitmap, const Vec2i& offset);
	void createFromBitmap(PaletteBitmap&& bitmap, const Vec2i& offset);
	void createFromBitmap(const PaletteBitmap& bitmap, const Recti& sourceRect, const Vec2i& offset);
	void createFromSpritePatterns(const std::vector<RenderUtils::SinglePattern>& patterns);

	inline PaletteBitmap& accessBitmap()  { return mBitmap; }
	inline const PaletteBitmap& getBitmap() const  { return mBitmap; }
	const PaletteBitmap& getUpscaledBitmap() const;
	inline const PaletteBitmap& getUpscaledBitmapIfPresent() const  { return mUpscaledBitmap; }	// (empty if it wasn't needed yet)

	inline Vec2i getSize() const override  { return mBitmap.getSize(); }

#if defined(PLATFORM_PSP)
	// PSP: the upscaled bitmaps (9x the memory) are a cache with a budget, the least recently used get freed
	//  -> Call at the start of a frame, when nothing drawn before uses them any more
	~PaletteSprite();
	static void trimUpscaledBitmaps(size_t budgetBytes);
#endif

private:
	PaletteBitmap mBitmap;
	mutable PaletteBitmap mUpscaledBitmap;	// Used for smoother rotation
};
