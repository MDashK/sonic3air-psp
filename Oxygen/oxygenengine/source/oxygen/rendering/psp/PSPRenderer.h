/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: renders the game screen with the PSP's GPU (the GE).
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#if defined(PLATFORM_PSP)

#include "oxygen/rendering/Renderer.h"

class PlaneGeometry;
class SpriteGeometry;
class TexturedRectGeometry;
class Bitmap;
class PaletteBase;
namespace renderitems
{
	struct CustomSpriteInfoBase;
}


class PSPRenderer : public Renderer
{
public:
	static constexpr int8 RENDERER_TYPE_ID = 0x30;

public:
	PSPRenderer(RenderParts& renderParts, DrawerTexture& outputTexture);
	~PSPRenderer();

	virtual void initialize() override;
	virtual void reset() override;
	virtual void setGameResolution(const Vec2i& gameResolution) override;
	virtual void clearGameScreen() override;
	virtual void renderGameScreen(const std::vector<Geometry*>& geometries) override;
	virtual void renderDebugDraw(int debugDrawMode, const Recti& rect) override  {}

private:
	struct Internal;

private:
	void updatePatterns();
	void buildCluts();
	void setScissor(const Recti& rect);
	void setStencilMode(int mode);
	void setBlendMode(int blendMode);

	void renderPlane(const PlaneGeometry& geometry);
	void drawPatternBuckets();
	void renderVdpSprite(const SpriteGeometry& geometry);
	void renderCustomSprite(const SpriteGeometry& geometry);
	void renderRect(const Recti& rect, const Color& color);
	void renderTexturedRect(const TexturedRectGeometry& geometry);
	void collectSpriteMasks(const std::vector<Geometry*>& geometries);
	void drawSpriteMaskStencil(const Geometry& maskGeometry, uint8 bit, bool set);
	void setPixelMask(uint32 mask);

	bool bindTextureForBitmap(const void* data, int width, int height, int bytesPerPixel, int psm, uint32 changeCounter);
	const uint32* getPaletteClut(const PaletteBase& palette, int offset, int count, bool opaque = false);	// (opaque: color 0 is drawn as well, else transparent)

private:
	Internal& mInternal;
	Vec2i mGameResolution;
	Recti mCurrentViewport;
	Recti mCurrentScissor;
	int mCurrentStencilMode = -1;
	int mCurrentBlendMode = -1;
	uint32 mCurrentPixelMask = 0xffffffff;
	bool mAnyPrioPlaneDrawn = false;
	int mLastPlaneMode = 0;			// How the last plane got drawn: 0 = pattern by pattern, 1/2 = through a texture (for the statistics)
	uint8 mActiveMaskBits = 0;		// Stencil bits of the sprite masks still to come in the render queue (nothing gets drawn there, see collectSpriteMasks)
};

#endif
