/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	PSP port: the UI (menus, texts, fades) drawn by the GE, instead of the software drawer.
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#if defined(PLATFORM_PSP)

class DrawCommand;
class DrawerTexture;

namespace pspui
{
	// The texture the game view draws its UI into: drawing commands for it go to the GE (straight onto the screen, over the game
	// screen and scaled like it), commands for other textures stay with the software drawer
	void setUiTexture(DrawerTexture* texture);
	bool isUiTexture(const DrawerTexture* texture);
	bool isEnabled();		// (S3AIR_PSP_SOFTWARE_UI=1 for the software drawer instead)

	// Drawing commands for the UI texture, in order; state commands (blend mode, scissor) included
	void beginWindowCommands(int blendMode, int samplingMode, int wrapMode);	// (drawing into the window, e.g. the disclaimer: screen coordinates)
	bool isWindowActive();
	void beginCommands(int blendMode, int samplingMode, int wrapMode);	// (when the UI texture becomes the render target; the drawer's current state)
	void executeCommand(DrawCommand& drawCommand);

	// Once per frame: frees texture copies and texts not used for a while
	void endFrame();
}

#endif
