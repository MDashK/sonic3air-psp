/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/scriptimpl/ScriptImplementations.h"
#include "sonic3air/Game.h"

#include "oxygen/application/audio/AudioPlayer.h"
#include "oxygen/application/video/VideoOut.h"
#include "oxygen/rendering/parts/RenderParts.h"
#include "oxygen/rendering/parts/SpriteManager.h"
#include "oxygen/rendering/utils/Kosinski.h"
#include "oxygen/simulation/bindings/RendererBindings.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/RuntimeEnvironment.h"

#include <lemon/program/function/FunctionWrapper.h>
#include <lemon/program/Module.h>
#include <lemon/program/ModuleBindingsBuilder.h>


namespace s3air
{
	void setSpindashSoundSlot(uint8 slot)
	{
		AudioPlayer::setSpindashSoundSlot(slot);
	}


	inline EmulatorInterface& getEmulatorInterface()
	{
		return *lemon::Runtime::getActiveEnvironmentSafe<RuntimeEnvironment>().mEmulatorInterface;
	}

	void kosinskiDecompress()
	{
		EmulatorInterface& emulatorInterface = getEmulatorInterface();
		uint32& A0 = emulatorInterface.getRegister(EmulatorInterface::Register::A0);
		uint32& A1 = emulatorInterface.getRegister(EmulatorInterface::Register::A1);

		// TODO: The RAM writes here won't trigger watches, though they should
		uint8* initialPointer = emulatorInterface.getMemoryPointer(A1, false, 1);
		uint8* pointer = initialPointer;
		Kosinski::decompress(emulatorInterface, pointer, A0);

		A1 += (uint32)(pointer - initialPointer);
	}

	void writeScrollOffsetsShared(uint32 value)
	{
		EmulatorInterface& emulatorInterface = getEmulatorInterface();
		const int height = VideoOut::instance().getScreenHeight();
		for (int line = 0; line < height; ++line)
		{
			emulatorInterface.writeMemory32(0xffffe000 + line * 4, value);
		}
	}

	void writeScrollOffsets()
	{
		EmulatorInterface& emulatorInterface = getEmulatorInterface();
		const uint16 foregroundX = -(int16)emulatorInterface.readMemory16(0xffffee80);
		const uint16 backgroundX = -(int16)emulatorInterface.readMemory16(0xffffee8c);
		writeScrollOffsetsShared(((uint32)foregroundX << 16) | backgroundX);
	}

	void writeScrollOffsetsFlipped()
	{
		EmulatorInterface& emulatorInterface = getEmulatorInterface();
		const uint16 foregroundX = -(int16)emulatorInterface.readMemory16(0xffffee80);
		const uint16 backgroundX = -(int16)emulatorInterface.readMemory16(0xffffee8c);
		writeScrollOffsetsShared(((uint32)backgroundX << 16) | foregroundX);
	}

	uint32 putNybbles(uint32 input, uint16 count, uint8 value)
	{
		for (uint16 i = 0; i < count; ++i)
		{
			input = (input << 4) | value;
		}
		return input;
	}

	// The same as the script function "blendColorsPacked" (utility/utility.lemon) with "unpackColor", "blendColors_BGR32" and
	// "packColorExt", in C++ for performance: in white screen flashes it runs for whole palettes in each frame (~3-6 ms per frame on a PSP)
	uint16 blendColorsPackedNative(uint16 colorA, uint16 colorB, uint16 blendFactor)
	{
		auto unpackColor = [](uint16 color) -> uint32
		{
			if (color & 0x8000)
			{
				// Extended 15-bit color format (standalone only)
				const uint32 r = std::min<uint32>(((color)       & 0x1f) * 0x09, 0xff);
				const uint32 g = std::min<uint32>(((color >> 5)  & 0x1f) * 0x09, 0xff);
				const uint32 b = std::min<uint32>(((color >> 10) & 0x1f) * 0x09, 0xff);
				return r + (g << 8) + (b << 16);
			}
			else
			{
				// Original 9-bit color format
				const uint32 r = ((color >> 1) & 0x07) * 0x24;
				const uint32 g = ((color >> 5) & 0x07) * 0x24;
				const uint32 b = ((color >> 9) & 0x07) * 0x24;
				return r + (g << 8) + (b << 16);
			}
		};

		const uint32 colorA32 = unpackColor(colorA);
		const uint32 colorB32 = unpackColor(colorB);
		const uint32 inverseFactor = (uint16)(0x100 - blendFactor);		// (16-bit like in the script)
		const uint32 r = ( (colorA32        & 0xff) * inverseFactor + ( colorB32        & 0xff) * blendFactor + 0x80) >> 8;
		const uint32 g = (((colorA32 >> 8)  & 0xff) * inverseFactor + ((colorB32 >> 8)  & 0xff) * blendFactor + 0x80) >> 8;
		const uint32 b = (((colorA32 >> 16) & 0xff) * inverseFactor + ((colorB32 >> 16) & 0xff) * blendFactor + 0x80) >> 8;
		const uint32 result = r + (g << 8) + (b << 16);

		const uint16 rr = (uint16)((((result)       & 0xff) + 0x04) / 0x09);
		const uint16 gg = (uint16)((((result >> 8)  & 0xff) + 0x04) / 0x09);
		const uint16 bb = (uint16)((((result >> 16) & 0xff) + 0x04) / 0x09);
		return (uint16)(rr + (gg << 5) + (bb << 10) + 0x8000);
	}

	// Nemesis decompression: the same as the script functions "fn00163c" and "fn001604_gatherData" (general/nemesis.lemon), in C++ for performance
	//  -> As script, it took 20-40 ms in some frames on a PSP (e.g. loading new objects' graphics)
	//  -> Same registers and memory as the script version, so the original code's control flow around it stays the same
	namespace nemesis
	{
		struct Registers
		{
			EmulatorInterface& mEmulator;
			uint32& D0; uint32& D1; uint32& D2; uint32& D3; uint32& D4; uint32& D5; uint32& D6; uint32& D7;
			uint32& A0; uint32& A1; uint32& A3; uint32& A4; uint32& A5;

			explicit Registers(EmulatorInterface& emulator) :
				mEmulator(emulator),
				D0(emulator.getRegister(EmulatorInterface::Register::D0)), D1(emulator.getRegister(EmulatorInterface::Register::D1)),
				D2(emulator.getRegister(EmulatorInterface::Register::D2)), D3(emulator.getRegister(EmulatorInterface::Register::D3)),
				D4(emulator.getRegister(EmulatorInterface::Register::D4)), D5(emulator.getRegister(EmulatorInterface::Register::D5)),
				D6(emulator.getRegister(EmulatorInterface::Register::D6)), D7(emulator.getRegister(EmulatorInterface::Register::D7)),
				A0(emulator.getRegister(EmulatorInterface::Register::A0)), A1(emulator.getRegister(EmulatorInterface::Register::A1)),
				A3(emulator.getRegister(EmulatorInterface::Register::A3)), A4(emulator.getRegister(EmulatorInterface::Register::A4)),
				A5(emulator.getRegister(EmulatorInterface::Register::A5))
			{}
		};

		inline void setLow16(uint32& reg, uint32 value)	{ reg = (reg & 0xffff0000) | (value & 0xffff); }
		inline uint16 low16(uint32 reg)					{ return (uint16)reg; }
		inline uint16 shiftRight16(uint16 value, uint32 shift)
		{
			// The shift count is a whole register, of which only the lower 6 bits count (as on the 68000, and in the script runtime's 64-bit shift)
			shift &= 63;
			return (shift >= 16) ? 0 : (uint16)(value >> shift);
		}

		void gatherData(Registers& r)
		{
			setLow16(r.D7, low16(r.D6) - 8);
			setLow16(r.D1, shiftRight16(low16(r.D5), r.D7));
			if ((uint8)r.D1 >= 0xfc)
			{
				setLow16(r.D6, low16(r.D6) - 6);
				if (low16(r.D6) < 9)
				{
					setLow16(r.D6, low16(r.D6) + 8);
					setLow16(r.D5, (uint16)(low16(r.D5) << 8) + r.mEmulator.readMemory8(r.A0));
					++r.A0;
				}

				setLow16(r.D6, low16(r.D6) - 7);
				const uint16 value = shiftRight16(low16(r.D5), r.D6);
				setLow16(r.D0, (value & 0x70) >> 4);
				setLow16(r.D1, (value & 0x0f));

				if (low16(r.D6) < 9)
				{
					setLow16(r.D6, low16(r.D6) + 8);
					setLow16(r.D5, (uint16)(low16(r.D5) << 8) + r.mEmulator.readMemory8(r.A0));
					++r.A0;
				}
			}
			else
			{
				const uint32 offset = (uint32)(uint8)r.D1 * 2;
				setLow16(r.D6, low16(r.D6) - (int8)r.mEmulator.readMemory8(r.A1 + offset));

				if (low16(r.D6) < 9)
				{
					setLow16(r.D6, low16(r.D6) + 8);
					setLow16(r.D5, (uint16)(low16(r.D5) << 8) + r.mEmulator.readMemory8(r.A0));
					++r.A0;
				}

				const uint16 value = r.mEmulator.readMemory8(r.A1 + offset + 1);
				setLow16(r.D0, (value & 0xf0) >> 4);
				setLow16(r.D1, (value & 0x0f));
			}
		}

		// Debugging (desktop): registers on entry and exit of each call, to compare with the script version ("Nemesis.debugLog" called from there)
		void debugLog(uint8 tag)
		{
			static FILE* file = nullptr;
			static bool tried = false;
			if (!tried)
			{
				tried = true;
				const char* path = getenv("S3AIR_NEMESIS_LOG");
				if (nullptr != path)
					file = fopen(path, "w");
			}
			if (nullptr == file)
				return;
			EmulatorInterface& e = getEmulatorInterface();
			fprintf(file, "%s", tag == 0 ? "in " : "out");
			for (int i = 0; i < 16; ++i)
				fprintf(file, " %08x", e.getRegister(i));
			fprintf(file, "\n");
		}

		void decode()
		{
			debugLog(0);
			struct ExitLog { ~ExitLog() { debugLog(1); } } exitLog;
			Registers r(getEmulatorInterface());

			// Variants: 0 = write to VRAM, 1 = write to VRAM and XOR output with D2, 2 = write to RAM and XOR output with D2
			uint8 variant;
			if (r.A3 == 0x00167c)
				variant = 0;
			else if (r.A3 == 0x001686 || (r.A3 == 0x00169c && r.A4 == 0xc00004))
				variant = 1;
			else if (r.A3 == 0x00169c)
				variant = 2;
			else
			{
				RMX_ASSERT(false, "Invalid variant for Nemesis data loading");
				return;
			}

			if ((int16)low16(r.D0) < 0)
				gatherData(r);

			while (true)
			{
				// (16-bit math like in the script: D0 = 0xffff gives a count of 0, so the next data gets read)
				const uint16 count = std::min<uint16>(low16(r.D3), (uint16)(low16(r.D0) + 1));
				r.D4 = putNybbles(r.D4, count, (uint8)r.D1);

				if (count < low16(r.D3))
				{
					setLow16(r.D3, low16(r.D3) - count);
					setLow16(r.D0, 0xffff);
					gatherData(r);
				}
				else
				{
					// Ready for output
					if (variant == 0)
					{
						RendererBindings::writeVdpData32(r.D4);
					}
					else if (variant == 1)
					{
						r.D2 ^= r.D4;
						RendererBindings::writeVdpData32(r.D2);
					}
					else
					{
						r.D2 ^= r.D4;
						r.mEmulator.writeMemory32(r.A4, r.D2);
						r.A4 += 4;
					}

					r.D4 = 0;
					--r.A5;
					if (low16(r.A5) == 0)
					{
						setLow16(r.D0, low16(r.D0) - (low16(r.D3) - 1));
						setLow16(r.D3, 0);
						return;
					}

					setLow16(r.D0, low16(r.D0) - low16(r.D3));
					r.D3 = 8;
				}
			}
		}
	}


	// TEST!
	void decompressKosinskiData(uint32 sourceAddress, uint16 targetInVRAM)
	{
		// Get the decompressed size
		EmulatorInterface& emulatorInterface = getEmulatorInterface();
		uint16 size = emulatorInterface.readMemory16(sourceAddress);
		if (size == 0xa000)
			size = 0x8000;
		uint32 inputAddress = sourceAddress + 2;

		while (size > 0)
		{
			uint8 buffer[0x1000];
			uint8* pointer = buffer;
			Kosinski::decompress(emulatorInterface, pointer, inputAddress);

			const uint16 bytes = std::min<uint16>(size, 0x1000);
			uint8* dst = emulatorInterface.getVRam() + targetInVRAM;
			const uint8* src = buffer;
			for (uint16 i = 0; i < bytes; i += 2)
			{
				dst[0] = src[1];
				dst[1] = src[0];
				src += 2;
				dst += 2;
			}

			if (size < 0x1000)
				break;

			targetInVRAM += 0x1000;
			size -= bytes;
			inputAddress += 8;	// This is needed, but why...?
		}
	}


	// Object sprite rendering (general/rendering.lemon), sped up for the PSP: with lots of objects on screen (scattered
	// rings, breaking platforms), the script hooks for each object and each of its VDP sprites took most of the frame time
	namespace sprites
	{
		// All object update addresses and mapping offsets that "Standalone.onWriteToSpriteTable" (standalone/rendering/renderhooks.lemon)
		// checks for: each of its cases starts with a test for one of these, so for any other object it returns false anyway
		//  -> Keep in sync with the script (the list was taken from all "objA0.update_address == ..." and "objA0.mapping_offset == ..." in it)
		static const uint32 OBJECT_HOOK_UPDATE_ADDRESSES[] =
		{
			0x00903e, 0x00927a, 0x009488, 0x00bf3e, 0x00d30c, 0x00d42c, 0x010a94, 0x01365c, 0x0160d2, 0x016444, 0x01952a, 0x019602, 0x019732, 0x019922,
			0x01a51a, 0x01a64a, 0x01a662, 0x01a88c, 0x01d7ba, 0x02b962, 0x02dd98, 0x02ea50, 0x02ea5a, 0x02ea64, 0x02ebe8, 0x02ec1e, 0x031754, 0x04dfee,
			0x04dffe, 0x04e000, 0x04e202, 0x04e222, 0x04e23a, 0x04e29c, 0x04e2f0, 0x05c55c, 0x05c588, 0x05c660, 0x05c74a, 0x05c790, 0x05c88c, 0x05c8ac,
			0x05c8c8, 0x05ca5c, 0x05ca9e, 0x05e0d6, 0x05e18a, 0x05e6c0, 0x05ea52, 0x05ebb4, 0x05ed18, 0x05f376, 0x05f39e, 0x05f3da, 0x05f400, 0x05f46a,
			0x05f4e4, 0x061682, 0x061d92, 0x061f98, 0x062124, 0x0622b2, 0x0624f2, 0x062640, 0x0628aa, 0x062b3a, 0x062fa2, 0x062fca, 0x0630fc, 0x063308,
			0x063466, 0x063a6a, 0x063de0, 0x0656da, 0x067472, 0x0677ce, 0x067800, 0x067b96, 0x0820d0, 0x08215c, 0x0837b2, 0x086e5c, 0x087bca, 0x087da4,
			0x087dc0, 0x087e3e, 0x087f5c, 0x088282
		};
		static const uint32 OBJECT_HOOK_MAPPING_OFFSETS[] =
		{
			0x00ce0e, 0x0602d6, 0x146620, 0x146816, 0x148eb8, 0x14a8d6, 0x344bb8, 0x347e30, 0x3a1aee
		};

		// The same for "Standalone.onDrawVdpSprite" (called for each VDP sprite)
		static const uint32 VDP_HOOK_UPDATE_ADDRESSES[] =
		{
			0x0057a2, 0x005802, 0x00587c, 0x0193ca, 0x0193ec, 0x03b1fc, 0x03b238, 0x0480d4, 0x05068a, 0x057156, 0x067472, 0x079406, 0x09089e
		};
		static const uint32 VDP_HOOK_MAPPING_OFFSETS[] =
		{
			0x083ffc
		};

		template<size_t N>
		inline bool contains(const uint32 (&sortedList)[N], uint32 value)
		{
			return std::binary_search(sortedList, sortedList + N, value);
		}

		// Could "Standalone.onWriteToSpriteTable" do anything for the object at A0? If not, the script can skip calling it
		bool mayHandleObjectSprite(uint32 titleCardRedBarAddress)
		{
			EmulatorInterface& emulatorInterface = getEmulatorInterface();
			const uint32 A0 = emulatorInterface.getRegister(EmulatorInterface::Register::A0);
			const uint32 updateAddress = emulatorInterface.readMemory32(A0);
			const uint32 mappingOffset = emulatorInterface.readMemory32(A0 + 0x0c);
			if (contains(OBJECT_HOOK_UPDATE_ADDRESSES, updateAddress) || contains(OBJECT_HOOK_MAPPING_OFFSETS, mappingOffset))
				return true;
			if ((updateAddress >= 0x01d566 && updateAddress <= 0x01d61e) || (updateAddress >= 0x0396e4 && updateAddress <= 0x0398a6) || updateAddress == titleCardRedBarAddress)
				return true;
			return false;
		}

		// The ring cases of "Standalone.onWriteToSpriteTable" (dynamic rings, spilled rings), with exactly the same result: when Sonic
		// loses his rings, there are up to 32 of them, and the script built a sprite key string for each of them in every frame
		//  -> Returns false for any other object (then the script hook gets called as usual)
		bool drawRingSprite(int16 px, int16 py, uint16 renderQueue)
		{
			EmulatorInterface& emulatorInterface = getEmulatorInterface();
			const uint32 A0 = emulatorInterface.getRegister(EmulatorInterface::Register::A0);
			const uint32 updateAddress = emulatorInterface.readMemory32(A0);
			if (updateAddress != 0x01a51a && updateAddress != 0x01a64a && updateAddress != 0x01a662 && updateAddress != 0x01a88c)
				return false;

			// The cases before the rings in the script hook check these mapping offsets (character art): leave those to the script
			const uint32 mappingOffset = emulatorInterface.readMemory32(A0 + 0x0c);
			if (mappingOffset == 0x344bb8 || mappingOffset == 0x146620 || mappingOffset == 0x146816 || mappingOffset == 0x148eb8 || mappingOffset == 0x14a8d6 || mappingOffset == 0x347e30)
				return false;

			const uint8 baseState = emulatorInterface.readMemory8(A0 + 0x05);
			uint8 animFrame = 0;
			uint8 alpha = 255;
			if (updateAddress == 0x01a51a && baseState == 0x02)
			{
				// Dynamic ring (placed by debug mode, or those in Knuckles' SSZ)
				//  -> (all of this is 8-bit arithmetic in the script, as all values are u8)
				const uint8 staticAnimTimer = emulatorInterface.readMemory8(0xfffffeb2);
				const uint8 staticAnimFrame = emulatorInterface.readMemory8(0xfffffeb3);
				animFrame = (uint8)((uint8)((uint8)(staticAnimFrame * 8 + 7) - staticAnimTimer) / 4) % 8;
			}
			else if ((updateAddress == 0x01a64a || updateAddress == 0x01a662) && baseState == 0x02)
			{
				// Spilled ring
				const uint16 progress = emulatorInterface.readMemory16(0xfffffeb8);
				const uint8 speed = emulatorInterface.readMemory8(0xfffffeb6);
				animFrame = (uint8)((progress >> 8) % 8);
				alpha = (uint8)std::min<uint16>(255, (uint16)(speed * 10));
			}
			else if (updateAddress == 0x01a88c && baseState == 0x00)
			{
				// Dynamic ring
				const uint8 sprite = emulatorInterface.readMemory8(A0 + 0x22);
				const uint8 timer = emulatorInterface.readMemory8(A0 + 0x24);
				animFrame = (uint8)((uint8)((uint8)(sprite * 4 + 3) - timer) / 2) % 8;
			}
			else
			{
				return false;
			}

			// Keys of "ring_00" .. "ring_07" (the script's string hashes)
			static uint64 ringKeys[8] = { 0 };
			if (ringKeys[0] == 0)
			{
				for (int i = 0; i < 8; ++i)
					ringKeys[i] = rmx::getMurmur2_64(String(0, "ring_0%d", i));
			}

			const bool prioFlag = (emulatorInterface.readMemory16(A0 + 0x0a) & 0x8000) != 0;		// sprite_attribute.PRIORITY
			const uint8 flags = prioFlag ? 0x40 : 0;		// SPRITE_FLAG_PRIO
			// (as "Renderer.drawCustomSprite(key, px, py, 0x00, flags, renderQueue, 0, alpha)")
			RenderParts::instance().getSpriteManager().drawCustomSprite(ringKeys[animFrame], Vec2i(px, py), 0x00, flags, renderQueue, Color(1.0f, 1.0f, 1.0f, (float)alpha / 255.0f), 0.0f);
			return true;
		}

		// The same as the script function "getTileAtWorldPosition" (maingame/collision.lemon): used by every collision check with the level
		uint32 getTileAtWorldPosition(uint16 x, uint16 y, uint32 chunksBaseAddress)
		{
			EmulatorInterface& emulatorInterface = getEmulatorInterface();
			const uint16 chunkSizeBitmask = emulatorInterface.readMemory16(0xffffeeae);		// level.chunksize.bitmask
			const uint16 offset = (uint16)((y >> 5) & chunkSizeBitmask);
			uint16 ramAddress = emulatorInterface.readMemory16(chunksBaseAddress + offset);
			ramAddress = (uint16)(ramAddress + (x >> 7));

			// (it can be a ROM address, as in the script)
			uint32 address = ((int16)ramAddress >= 0) ? (uint32)ramAddress : (0xffff0000 + (uint32)ramAddress);
			const uint8 chunkType = emulatorInterface.readMemory8(address);

			address = 0xffff0000 + emulatorInterface.readMemory16(0x00f02a + (uint32)chunkType * 2);
			address += y & 0x70;
			address += (x >> 3) & 0x0e;
			return address;
		}

		// The same as the script function "fn00f994" (maingame/collision.lemon) with what it calls: a spilled ring's floor check,
		// done for each falling ring in each frame. Registers and memory end up as with the script.
		namespace ringfloor
		{
			struct Regs
			{
				EmulatorInterface& mEmu;
				uint32& D1; uint32& D2; uint32& D3; uint32& D5; uint32& D6;
				uint32& A1; uint32& A2; uint32& A3; uint32& A4; uint32& A5;
			};

			inline void setLow16(uint32& reg, uint16 value)  { reg = (reg & 0xffff0000) | value; }

			uint16 getTileAndAngle(Regs& r)
			{
				// (as in the script: the setting is queried in any case)
				const bool fixGlitches = (Game::instance().getSetting(0x00000102, false) != 0);		// SETTING_FIX_GLITCHES
				if (fixGlitches && r.mEmu.readMemory16(0xffffee18) != 0xff00)					// level.vertical_wrap
				{
					setLow16(r.D2, (uint16)std::max<int16>((int16)r.D2, 0));
				}
				const uint8 layerBit = (uint8)r.D5;

				// "call A5": fn00eff0 or fn00efb6
				r.A1 = getTileAtWorldPosition((uint16)r.D3, (uint16)r.D2, (r.A5 == 0x00efb6) ? 0xffff800a : 0xffff8008);

				uint16 tile = r.mEmu.readMemory16(r.A1);
				const uint16 tileType = tile & 0x03ff;
				if (tileType != 0 && ((tile >> layerBit) & 1))
				{
					r.A2 = r.mEmu.readMemory32(0xfffff796);
					const uint8 tileForm = r.mEmu.readMemory8(r.A2 + tileType * 2);
					if (tileForm != 0)
					{
						uint8 angle = r.mEmu.readMemory8(0x096000 + tileForm);
						if (tile & 0x0400)
							angle = (uint8)(-angle);
						if (tile & 0x0800)
							angle = (uint8)(0x80 - angle);
						r.mEmu.writeMemory8(r.A4, angle);
						return tile;
					}
				}
				return 0;
			}

			int16 getCollisionIndentY(Regs& r)
			{
				uint16 tile = getTileAndAngle(r);
				if (tile != 0)
				{
					const uint16 inputX = (uint16)r.D3;
					const uint8 tileForm = r.mEmu.readMemory8(r.A2 + (tile & 0x3ff) * 2);
					uint16 ix = inputX & 0x0f;
					if (tile & 0x0400)
						ix = 15 - ix;
					int16 indent = (int8)r.mEmu.readMemory8(0x096100 + (uint32)tileForm * 0x10 + ix);
					if (indent != 0)
					{
						tile ^= (uint16)r.D6;
						if (tile & 0x0800)
							indent = -indent;
						return indent;
					}
				}
				return 0;
			}

			void getDistanceY1(Regs& r)
			{
				const int16 fraction = (uint16)r.D2 & 0x0f;
				const int16 indent = getCollisionIndentY(r);
				if (indent != 0)
				{
					if (indent > 0)
					{
						setLow16(r.D1, (uint16)(15 - fraction - indent));
						return;
					}
					if (indent + fraction < 0)
					{
						setLow16(r.D1, (uint16)(-1 - fraction));
						return;
					}
				}
				setLow16(r.D1, (uint16)(15 - fraction));
			}

			void getDistanceY2a(Regs& r)
			{
				const int16 fraction = (uint16)r.D2 & 0x0f;
				const int16 indent = getCollisionIndentY(r);
				if (indent > 0 && indent < 16)
				{
					setLow16(r.D1, (uint16)(15 - fraction - indent));
				}
				else if (indent + fraction < 0 || indent == 16)
				{
					setLow16(r.D2, (uint16)((uint16)r.D2 - (uint16)r.A3));
					getDistanceY1(r);
					setLow16(r.D2, (uint16)((uint16)r.D2 + (uint16)r.A3));
					setLow16(r.D1, (uint16)((int16)r.D1 - 16));
				}
				else
				{
					setLow16(r.D1, 16);
				}
			}
		}

		void spilledRingFloorCheck()
		{
			EmulatorInterface& emu = getEmulatorInterface();
			using R = EmulatorInterface::Register;
			ringfloor::Regs r { emu, emu.getRegister(R::D1), emu.getRegister(R::D2), emu.getRegister(R::D3), emu.getRegister(R::D5), emu.getRegister(R::D6),
								emu.getRegister(R::A1), emu.getRegister(R::A2), emu.getRegister(R::A3), emu.getRegister(R::A4), emu.getRegister(R::A5) };
			uint32& D0 = emu.getRegister(R::D0);
			const uint32 A0 = emu.getRegister(R::A0);

			// fn00f994
			ringfloor::setLow16(r.D3, emu.readMemory16(A0 + 0x10));
			ringfloor::setLow16(r.D2, (uint16)(emu.readMemory16(A0 + 0x14) + emu.readMemory8(A0 + 0x1e)));
			emu.writeMemory8(0xfffff768, 0);
			r.D5 = 0x0c;

			// checkStaticCollisionVerticalShort(D3.u16, D2.u16, 0xfffff768, 0x10, 0)
			r.D3 = (uint16)r.D3;
			r.D2 = (uint16)r.D2;
			r.A4 = 0xfffff768;
			r.A3 = 0x10;
			r.D6 = 0;

			// CheckStaticCollisionVerticalShort
			r.A5 = 0x00eff0;
			if (emu.readMemory8(0xfffff664) == 0)		// level.dualplanecollision
			{
				ringfloor::getDistanceY2a(r);
			}
			else
			{
				ringfloor::getDistanceY2a(r);
				ringfloor::setLow16(r.D3, (uint16)((uint16)r.D3 - emu.readMemory16(0xffffee3e)));
				ringfloor::setLow16(r.D2, (uint16)((uint16)r.D2 - emu.readMemory16(0xffffee40)));
				const int16 result1 = (int16)r.D1;
				emu.writeMemory8(r.A4 + 1, emu.readMemory8(r.A4));

				r.A5 = 0x00efb6;
				ringfloor::getDistanceY2a(r);
				ringfloor::setLow16(r.D3, (uint16)((uint16)r.D3 + emu.readMemory16(0xffffee3e)));
				ringfloor::setLow16(r.D2, (uint16)((uint16)r.D2 + emu.readMemory16(0xffffee40)));

				ringfloor::setLow16(D0, (uint16)result1);
				if ((int16)r.D1 > (int16)D0)
				{
					emu.writeMemory8(r.A4, emu.readMemory8(r.A4 + 1));
					ringfloor::setLow16(r.D1, (uint16)D0);
				}
			}
		}

		// The same as the script function "fn01a75c" (objects/basics/ring.lemon): a spilled ring's update in each frame
		//  -> Returns true if the ring has to be unloaded now (the script does that)
		bool spilledRingUpdate()
		{
			EmulatorInterface& emu = getEmulatorInterface();
			using R = EmulatorInterface::Register;
			const uint32 A0 = emu.getRegister(R::A0);

			// objA0.animation.sprite = spilled_rings.animframe
			emu.writeMemory8(A0 + 0x22, emu.readMemory8(0xfffffeb7));

			// UpdateMovementStraightSimple()
			emu.writeMemory32(A0 + 0x10, emu.readMemory32(A0 + 0x10) + ((uint32)(int32)(int16)emu.readMemory16(A0 + 0x18) << 8));
			uint32& D0 = emu.getRegister(R::D0);
			D0 = (uint32)(int32)(int16)emu.readMemory16(A0 + 0x1a) << 8;
			emu.writeMemory32(A0 + 0x14, emu.readMemory32(A0 + 0x14) + D0);

			int16 velocityY = (int16)(emu.readMemory16(A0 + 0x1a) + 0x18);
			emu.writeMemory16(A0 + 0x1a, (uint16)velocityY);
			if (velocityY >= 0)
			{
				if (emu.readMemory8(A0 + 0x04) & 0x80)		// render_flag.VISIBLE
				{
					spilledRingFloorCheck();
					const int16 D1 = (int16)emu.getRegister(R::D1);
					if (D1 < 0)
					{
						emu.writeMemory16(A0 + 0x14, (uint16)(emu.readMemory16(A0 + 0x14) + (uint16)D1));
						velocityY = (int16)emu.readMemory16(A0 + 0x1a);
						emu.writeMemory16(A0 + 0x1a, (uint16)(int16)((velocityY >> 2) - velocityY));
					}
				}

				const uint16 bottom = (uint16)(emu.readMemory16(0xffffee1a) + (uint16)VideoOut::instance().getScreenHeight());	// move_area.bottom.current + getScreenHeight()
				if (emu.readMemory8(0xfffffeb6) == 0 || emu.readMemory16(A0 + 0x14) > bottom)
				{
					// Time over for this ring, or it fell out of the lower level bounds
					return true;
				}
			}

			// AddAsDynamicObject()
			const uint16 dynamicObjects = emu.readMemory16(0xffffe380);
			if (dynamicObjects < 0x7e)
			{
				emu.writeMemory16(0xffffe380, dynamicObjects + 2);
				emu.writeMemory16(0xffffe380 + dynamicObjects + 2, (uint16)A0);
			}

			emu.writeMemory16(A0 + 0x10, (uint16)(emu.readMemory16(A0 + 0x10) - emu.readMemory16(0xffffeebc)));

			// DrawObject()
			for (uint32 address = 0xffffac00 + emu.readMemory16(A0 + 0x08); address <= 0xffffaf80; address += 0x80)
			{
				const uint16 offset = emu.readMemory16(address);
				if (offset < 0x7e)
				{
					emu.writeMemory16(address, offset + 2);
					emu.writeMemory16(address + offset + 2, (uint16)A0);
					break;
				}
			}
			return false;
		}

		// The same for "Standalone.onDrawVdpSprite"
		bool mayHandleVdpSprite()
		{
			EmulatorInterface& emulatorInterface = getEmulatorInterface();
			const uint32 A0 = emulatorInterface.getRegister(EmulatorInterface::Register::A0);
			return contains(VDP_HOOK_UPDATE_ADDRESSES, emulatorInterface.readMemory32(A0)) || contains(VDP_HOOK_MAPPING_OFFSETS, emulatorInterface.readMemory32(A0 + 0x0c));
		}

		// The same as the script function "writeToSpriteTable" (general/rendering.lemon), for objects "Standalone.onDrawVdpSprite"
		// does nothing for (see "mayHandleVdpSprite"): every VDP sprite gets drawn as it is, and written into the sprite table
		void writeToSpriteTableNative(uint16 renderQueue)
		{
			EmulatorInterface& emulatorInterface = getEmulatorInterface();
			uint32& D4 = emulatorInterface.getRegister(EmulatorInterface::Register::D4);
			uint32& D7 = emulatorInterface.getRegister(EmulatorInterface::Register::D7);
			uint32& A1 = emulatorInterface.getRegister(EmulatorInterface::Register::A1);
			uint32& A6 = emulatorInterface.getRegister(EmulatorInterface::Register::A6);
			const uint16 baseX = (uint16)emulatorInterface.getRegister(EmulatorInterface::Register::D0);
			const uint16 baseY = (uint16)emulatorInterface.getRegister(EmulatorInterface::Register::D1);
			const uint16 attributes = (uint16)emulatorInterface.getRegister(EmulatorInterface::Register::D5);
			const uint8 renderFlags = (uint8)emulatorInterface.getRegister(EmulatorInterface::Register::D6);
			const bool flipX = (renderFlags & 0x01) != 0;		// render_flag.FLIP_X
			const bool flipY = (renderFlags & 0x02) != 0;		// render_flag.FLIP_Y
			SpriteManager& spriteManager = RenderParts::instance().getSpriteManager();

			auto setLow16 = [](uint32& reg, uint16 value) { reg = (reg & 0xffff0000) | value; };
			if ((int16)D4 >= 0x100)
				return;		// (the script asserts here)

			while ((int16)D4 >= 0)
			{
				const uint8 size = emulatorInterface.readMemory8(A1 + 1);
				const uint8 width  = (uint8)((size / 4 + 1) * 8);
				const uint8 height = (uint8)((size % 4 + 1) * 8);

				int16 px = (int16)baseX;
				if (flipX)
					px = (int16)(px - ((int16)emulatorInterface.readMemory16(A1 + 4) + width));
				else
					px = (int16)(px + emulatorInterface.readMemory16(A1 + 4));

				uint16 py = baseY;
				if (flipY)
					py = (uint16)(py - (int8)((int8)emulatorInterface.readMemory8(A1) + height));		// (the script adds s8 and u8 as 8-bit values)
				else
					py = (uint16)(py + (int8)emulatorInterface.readMemory8(A1));

				uint16 index = (uint16)(emulatorInterface.readMemory16(A1 + 2) + attributes);
				if (flipX)
					index ^= 0x0800;		// sprite_attribute.FLIP_X
				if (flipY)
					index ^= 0x1000;		// sprite_attribute.FLIP_Y

				spriteManager.drawVdpSprite(Vec2i((int16)(px - 0x80), (int16)(uint16)(py - 0x80)), size, index, renderQueue);

				if ((int16)D7 >= 0)
				{
					emulatorInterface.writeMemory16(A6 + 0, py);
					emulatorInterface.writeMemory8(A6 + 2, size);
					emulatorInterface.writeMemory16(A6 + 4, index);
					emulatorInterface.writeMemory16(A6 + 6, (uint16)std::max(1, (int)px & 0x03ff));		// (px_bitmask is 0x03ff in standalone)
					A6 += 8;
					setLow16(D7, (uint16)(D7 - 1));
				}

				A1 += 6;
				setLow16(D4, (uint16)(D4 - 1));
			}
		}
	}

}



void ScriptImplementations::registerScriptBindings(lemon::Module& module)
{
	lemon::ModuleBindingsBuilder builder(module);

	const BitFlagSet<lemon::Function::Flag> defaultFlags(lemon::Function::Flag::ALLOW_INLINE_EXECUTION);

	builder.addNativeFunction("Kosinski.Decompress", lemon::wrap(&s3air::kosinskiDecompress), defaultFlags);
	builder.addNativeFunction("WriteScrollOffsets", lemon::wrap(&s3air::writeScrollOffsets), defaultFlags);
	builder.addNativeFunction("WriteScrollOffsetsFlipped", lemon::wrap(&s3air::writeScrollOffsetsFlipped), defaultFlags);

	builder.addNativeFunction("putNybbles", lemon::wrap(&s3air::putNybbles), defaultFlags)
		.setParameters("input", "count", "value");

	builder.addNativeFunction("Nemesis.decodeInternal", lemon::wrap(&s3air::nemesis::decode), defaultFlags);
	builder.addNativeFunction("blendColorsPackedNative", lemon::wrap(&s3air::blendColorsPackedNative), defaultFlags)
		.setParameters("colorA", "colorB", "blendFactor");
	builder.addNativeFunction("Nemesis.debugLog", lemon::wrap(&s3air::nemesis::debugLog), defaultFlags);

	builder.addNativeFunction("uncompressKosinskiData", lemon::wrap(&s3air::decompressKosinskiData), defaultFlags)
		.setParameters("sourceAddress", "targetInVRAM");

	builder.addNativeFunction("Standalone.mayHandleObjectSprite", lemon::wrap(&s3air::sprites::mayHandleObjectSprite), defaultFlags)
		.setParameters("titleCardRedBarAddress");
	builder.addNativeFunction("Standalone.drawRingSprite", lemon::wrap(&s3air::sprites::drawRingSprite), defaultFlags)
		.setParameters("px", "py", "renderQueue");
	builder.addNativeFunction("getTileAtWorldPositionNative", lemon::wrap(&s3air::sprites::getTileAtWorldPosition), defaultFlags)
		.setParameters("x", "y", "chunksBaseAddress");
	builder.addNativeFunction("Standalone.mayHandleVdpSprite", lemon::wrap(&s3air::sprites::mayHandleVdpSprite), defaultFlags);
	builder.addNativeFunction("Standalone.setSpindashSoundSlot", lemon::wrap(&s3air::setSpindashSoundSlot), defaultFlags);
	builder.addNativeFunction("spilledRingFloorCheckNative", lemon::wrap(&s3air::sprites::spilledRingFloorCheck), defaultFlags);
	builder.addNativeFunction("spilledRingUpdateNative", lemon::wrap(&s3air::sprites::spilledRingUpdate), defaultFlags);
	builder.addNativeFunction("writeToSpriteTableNative", lemon::wrap(&s3air::sprites::writeToSpriteTableNative), defaultFlags)
		.setParameters("renderQueue");
}
