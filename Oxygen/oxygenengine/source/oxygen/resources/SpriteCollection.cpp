/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"
#include "oxygen/resources/SpriteCollection.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/engine/EngineMain.h"
#include "oxygen/engine/modding/ModManager.h"
#include "oxygen/helper/FileHelper.h"
#include "oxygen/helper/JsonHelper.h"
#include "oxygen/rendering/sprite/SpriteDump.h"
#include "oxygen/rendering/utils/Kosinski.h"
#include "oxygen/simulation/EmulatorInterface.h"
#include "oxygen/simulation/LemonScriptRuntime.h"

#if defined(PLATFORM_PSP)
	#include <pspthreadman.h>
#endif

#ifdef DEBUG
	// Notes on sprite dumping:
	//  - Sprite dumping will not replace existing files in "___internal/analysis/spritedump", so make sure to first remove all files / folders that you want to dump again
	//  - In order to match the image sizes of an old sprite dump again, you might want to temporarily remove the rounding to next multiple of 4, inside "PaletteSprite::createFromSpritePatterns"
	//#define CREATE_SPRITEDUMP
#endif


namespace
{

	bool isHexDigit(char ch)
	{
		return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
	}

	uint64 getSpriteKey(std::string_view identifier)
	{
		// Check if it's an hex identifier or a string
		if (identifier.length() >= 3 && identifier[0] == '0' && identifier[1] == 'x')
		{
			bool isHex = true;
			for (int i = 2; i < (int)identifier.length(); ++i)
			{
				if (!isHexDigit(identifier[i]))
				{
					isHex = false;
					break;
				}
			}
			if (isHex)
			{
				return rmx::parseInteger(identifier);
			}
		}

		// Just use the string hash
		return rmx::getMurmur2_64(identifier);
	}

	void decodeROMSpriteData(EmulatorInterface& emulatorInterface, std::vector<RenderUtils::PatternPixelContent>& patternBuffer, uint32 patternsBaseAddress, uint32 patternAddress, SpriteCollection::ROMSpriteEncoding encoding)
	{
		switch (encoding)
		{
			case SpriteCollection::ROMSpriteEncoding::NONE:
			{
				// Uncompressed / unpacked data
				const uint16 numPatterns = patternAddress;
				RenderUtils::expandMultiplePatternDataFromROM(patternBuffer, emulatorInterface.getMemoryPointer(patternsBaseAddress, false, numPatterns * 0x20), numPatterns);
				break;
			}

			case SpriteCollection::ROMSpriteEncoding::CHARACTER:
			{
				// Variant for character sprites
				int numSprites = emulatorInterface.readMemory16(patternAddress);
				uint32 address = patternAddress + 2;

				for (int i = 0; i < numSprites; ++i)
				{
					const uint16 data = emulatorInterface.readMemory16(address);
					address += 2;

					uint32 src = patternsBaseAddress + (data & 0x0fff) * 0x20;
					const uint16 numPatterns = ((data & 0xf000) >> 12) + 1;

					RenderUtils::expandMultiplePatternDataFromROM(patternBuffer, emulatorInterface.getRom() + src, numPatterns);
				}
				break;
			}

			case SpriteCollection::ROMSpriteEncoding::OBJECT:
			{
				// Variant for other object sprites
				int numSprites = emulatorInterface.readMemory16(patternAddress) + 1;
				uint32 address = patternAddress + 2;

				for (int i = 0; i < numSprites; ++i)
				{
					const uint16 data = emulatorInterface.readMemory16(address);
					address += 2;

					uint32 src = patternsBaseAddress + (data & 0x7ff0) * 2;
					const uint16 numPatterns = (data & 0x000f) + 1;

					RenderUtils::expandMultiplePatternDataFromROM(patternBuffer, emulatorInterface.getRom() + src, numPatterns);
				}
				break;
			}

			case SpriteCollection::ROMSpriteEncoding::KOSINSKI:
			{
				// Using Kosinski compressed data
				uint8 buffer[0x1000];

				// Get the decompressed size
				uint16 size = emulatorInterface.readMemory16(patternsBaseAddress);
				if (size == 0xa000)
					size = 0x8000;
				uint32 inputAddress = patternsBaseAddress + 2;

				while (size > 0)
				{
					uint8* pointer = buffer;
					Kosinski::decompress(emulatorInterface, pointer, inputAddress);

					const uint16 bytes = std::min<uint16>(size, 0x1000);
					RMX_ASSERT((bytes & 0x1f) == 0, "Expected decompressed data size to be divisible by 0x20");
					RenderUtils::expandMultiplePatternDataFromROM(patternBuffer, buffer, bytes / 0x20);

					if (size < 0x1000)
						break;

					size -= bytes;
					inputAddress += 8;	// This is needed, but why...?
				}
				break;
			}
		}
	}

	void createPaletteSpriteFromROM(EmulatorInterface& emulatorInterface, PaletteSprite& paletteSprite, uint32 patternsBaseAddress, uint32 patternAddress, uint32 mappingAddress, SpriteCollection::ROMSpriteEncoding encoding, int16 indexOffset)
	{
		// Fill sprite pattern buffer
		static std::vector<RenderUtils::PatternPixelContent> patternBuffer;
		patternBuffer.clear();
		decodeROMSpriteData(emulatorInterface, patternBuffer, patternsBaseAddress, patternAddress, encoding);

		// Fill sprite patterns
		static std::vector<RenderUtils::SinglePattern> patterns;
		patterns.clear();
		if (!patternBuffer.empty())
		{
			EmulatorInterface& emulatorInterface = EmulatorInterface::instance();
			const int count = emulatorInterface.readMemory16(mappingAddress);
			uint32 address = mappingAddress + 2;

			for (int i = 0; i < count; ++i)
			{
				RenderUtils::fillPatternsFromSpriteData(patterns, emulatorInterface.getRom() + address, patternBuffer, indexOffset);
				address += 6;
			}
		}

		// Create the palette sprite
		paletteSprite.createFromSpritePatterns(patterns);
	}

	void createPaletteSpriteFromROM(EmulatorInterface& emulatorInterface, PaletteSprite& paletteSprite, uint32 patternsBaseAddress, uint32 tableAddress, uint32 mappingOffset, uint8 animationSprite, SpriteCollection::ROMSpriteEncoding encoding, int16 indexOffset)
	{
		const uint32 patternAddress = (encoding == SpriteCollection::ROMSpriteEncoding::NONE || encoding == SpriteCollection::ROMSpriteEncoding::KOSINSKI) ? tableAddress : (tableAddress + emulatorInterface.readMemory16(tableAddress + animationSprite * 2));
		const uint32 mappingAddress = mappingOffset + emulatorInterface.readMemory16(mappingOffset + animationSprite * 2);

		createPaletteSpriteFromROM(emulatorInterface, paletteSprite, patternsBaseAddress, patternAddress, mappingAddress, encoding, indexOffset);
	}

}



void SpriteCollection::ROMSpriteData::serialize(VectorBinarySerializer& serializer)
{
	serializer.serialize(mPatternsBaseAddress);
	serializer.serialize(mTableAddress);
	serializer.serialize(mMappingOffset);
	serializer.serialize(mAnimationSprite);
	serializer.serializeAs<uint8>(mEncoding);
	serializer.serialize(mIndexOffset);
}

uint64 SpriteCollection::ROMSpriteData::getKey() const
{
	return (((uint64)mPatternsBaseAddress) << 42) ^ (((uint64)mTableAddress) << 25) ^ (((uint64)mMappingOffset) << 8) ^ (uint64)mAnimationSprite;
}


SpriteCollection::SpriteCollection()
{
}

SpriteCollection::~SpriteCollection()
{
	clear();

	if (nullptr != mSpriteDump)
	{
		mSpriteDump->save();
		SAFE_DELETE(mSpriteDump);
	}
}

void SpriteCollection::clear()
{
	// Delete the sprite instances
	for (auto& pair : mSpriteItems)
	{
		delete pair.second.mSprite;
	}
	mSpriteItems.clear();
	mSpritePalettes.clear();
	mPaletteRedirections.clear();
	++mGlobalChangeCounter;
#if defined(PLATFORM_PSP)
	closeSpritePacks();
	mPackLoadedBytes = 0;
#endif
}

void SpriteCollection::loadAllSpriteDefinitions()
{
#if defined(PLATFORM_PSP)
	// The sprite cache: 1 MB for the game's own sprites (4 MB in all, mostly for menus and single places), plus what the
	// mods' sprites may use: the mod memory budget minus what the active mods need otherwise
	{
		const std::vector<Mod*>& activeMods = ModManager::instance().getActiveMods();
		size_t modCache = 0;
		bool anyModSprites = false;
		for (const Mod* mod : activeMods)
			anyModSprites = anyModSprites || (mod->mPspSpritePixelBytes > 0);
		if (anyModSprites)
		{
			const uint32 otherBytes = ModManager::getPspMemoryBytes(activeMods);
			modCache = (otherBytes < ModManager::PSP_MOD_MEMORY_BUDGET) ? (ModManager::PSP_MOD_MEMORY_BUDGET - otherBytes) : 0;
			modCache = std::max<size_t>(modCache, ModManager::PSP_MOD_SPRITE_CACHE_MIN);
		}
		mPackCacheBudget = 1024 * 1024 + modCache;
		mPackHasModSprites = anyModSprites;
	}
#endif

	// Load or reload from all mods
	loadSpriteDefinitions(L"data/sprites", nullptr);
	for (const Mod* mod : ModManager::instance().getActiveMods())
	{
		loadSpriteDefinitions(mod->mFullPath + L"sprites", mod);
	}
}

bool SpriteCollection::hasSprite(uint64 key) const
{
	return (mSpriteItems.count(key) != 0);
}

const SpriteCollection::Item* SpriteCollection::getSprite(uint64 key)
{
	Item* item = mapFind(mSpriteItems, key);
	if (nullptr != item)
	{
		// Resolve redirect
		while (nullptr != item->mRedirect)
		{
			item = item->mRedirect;
		}
	#if defined(PLATFORM_PSP)
		touchSprite(*item);
	#endif
		return item;
	}
	else
	{
		// Output an error
		if (EngineMain::getDelegate().useDeveloperFeatures())
		{
			const std::string_view* str = LemonScriptRuntime::tryResolveStringHash(key);
			if (nullptr != str)
			{
				RMX_ERROR("Invalid sprite cache key with string '" << *str << "'", );
			}
			else
			{
				RMX_ERROR("Invalid sprite cache key with unknown hash " << rmx::hexString(key), );
			}
		}
		return nullptr;
	}
}

SpriteCollection::Item& SpriteCollection::getOrCreatePaletteSprite(uint64 key)
{
	Item* item = mapFind(mSpriteItems, key);
	if (nullptr != item)
	{
		RMX_CHECK(!item->mUsesComponentSprite, "Sprite is not a palette sprite", );
	}
	else
	{
		item = &createItem(key);
		item->mSprite = new PaletteSprite();
		item->mUsesComponentSprite = false;
	}
	return *item;
}

SpriteCollection::Item& SpriteCollection::getOrCreateComponentSprite(uint64 key)
{
	Item* item = mapFind(mSpriteItems, key);
	if (nullptr != item)
	{
		RMX_CHECK(item->mUsesComponentSprite, "Sprite is not a component sprite", );
	}
	else
	{
		item = &createItem(key);
		item->mSprite = new ComponentSprite();
		item->mUsesComponentSprite = true;
	}
	return *item;
}

SpriteCollection::Item& SpriteCollection::setupSpriteFromROM(EmulatorInterface& emulatorInterface, const ROMSpriteData& romSpriteData, uint8 atex)
{
	const uint64 key = romSpriteData.getKey();
	Item* item = mapFind(mSpriteItems, key);
	if (nullptr == item)
	{
		item = &getOrCreatePaletteSprite(key);
		item->mSourceInfo.mType = SourceInfo::Type::ROM_DATA;
		item->mSourceInfo.mROMSpriteData = romSpriteData;

		PaletteSprite& paletteSprite = *static_cast<PaletteSprite*>(item->mSprite);
		createPaletteSpriteFromROM(emulatorInterface, paletteSprite, romSpriteData.mPatternsBaseAddress, romSpriteData.mTableAddress, romSpriteData.mMappingOffset, romSpriteData.mAnimationSprite, romSpriteData.mEncoding, romSpriteData.mIndexOffset);

	#ifdef CREATE_SPRITEDUMP
		if (romSpriteData.mAnimationSprite != 0)	// TODO: Do this only for characters
		{
			String categoryKey(0, "%06x_%06x_%06x", romSpriteData.mPatternsBaseAddress, romSpriteData.mTableAddress, romSpriteData.mMappingOffset);
			getSpriteDump().addSpriteWithTranslation(paletteSprite, *categoryKey, romSpriteData.mAnimationSprite, atex);
			item->mGotDumped = true;
		}
	#endif
	}
	return *item;
}

void SpriteCollection::clearRedirect(uint64 sourceKey)
{
	Item* source = mapFind(mSpriteItems, sourceKey);
	if (nullptr != source)
	{
		source->mRedirect = nullptr;
	}
}

void SpriteCollection::setupRedirect(uint64 sourceKey, uint64 targetKey)
{
	Item* source = mapFind(mSpriteItems, sourceKey);
	if (nullptr == source)
	{
		source = &createItem(sourceKey);
	}

	Item* target = mapFind(mSpriteItems, targetKey);
	source->mRedirect = target;
}

SpriteDump& SpriteCollection::getSpriteDump()
{
	if (nullptr == mSpriteDump)
	{
		mSpriteDump = new SpriteDump();
		mSpriteDump->load();
	}
	return *mSpriteDump;
}

void SpriteCollection::dumpSprite(uint64 key, std::string_view categoryKey, uint8 spriteNumber, uint8 atex)
{
	Item* item = mapFind(mSpriteItems, key);
	if (nullptr != item && !item->mGotDumped)
	{
		if (!item->mUsesComponentSprite)
		{
			const PaletteSprite& paletteSprite = *static_cast<const PaletteSprite*>(item->mSprite);
			getSpriteDump().addSprite(paletteSprite, categoryKey, spriteNumber, atex);
		}
		else
		{
			RMX_ERROR("Can't dump component sprites (attempted to dump '" << categoryKey << "' sprite " << rmx::hexString(spriteNumber, 2) << ")", );
		}
		item->mGotDumped = true;
	}
}

SpriteCollection::Item& SpriteCollection::createItem(uint64 key)
{
	Item& item = mSpriteItems[key];
	item.mKey = key;
	item.mSprite = nullptr;
	item.mUsesComponentSprite = false;
	item.mChangeCounter = mGlobalChangeCounter;
#if defined(PLATFORM_PSP)
	// (an item replaced by another definition: the pixels it had loaded are gone with its sprite)
	mPackLoadedBytes -= std::min<size_t>(item.mPackBytes, mPackLoadedBytes);
	item.mPackIndex = -1;
	item.mPackBytes = 0;
#endif
	return item;
}

void SpriteCollection::loadSpriteDefinitions(const std::wstring& path, const Mod* mod)
{
	struct PaletteSpriteSheet
	{
		PaletteBitmap mBitmap;
		uint64 mFirstSpritePaletteKey = 0;
	};
	struct SheetCache
	{
		std::map<uint64, PaletteSpriteSheet> mPaletteSpriteSheets;
		std::map<uint64, Bitmap> mComponentSpriteSheets;
	};
	SheetCache sheetCache;

#if defined(PLATFORM_PSP)
	// PSP: a mod's sprites come from "sprites/sprites_psp.bin" made by the PSP mod converter (see loadSpritePack),
	// never from the sprite sheets themselves; the game's own sprites from "data/sprites_psp.bin" (made by psp/package.sh
	// with the same converter) if it's there
	if (nullptr != mod)
	{
		loadSpritePack(mod->mFullPath + L"sprites/sprites_psp.bin", mod);
		return;
	}
	if (FTX::FileSystem->exists(L"data/sprites_psp.bin"))
	{
		loadSpritePack(L"data/sprites_psp.bin", nullptr);
		return;
	}
#endif

	std::vector<rmx::FileIO::FileEntry> fileEntries;
	fileEntries.reserve(8);
	FTX::FileSystem->listFilesByMask(path + L"/*.json", true, fileEntries);
	if (fileEntries.empty())
		return;

	std::vector<uint32> palette;

	++mGlobalChangeCounter;
	for (const rmx::FileIO::FileEntry& fileEntry : fileEntries)
	{
		const Json::Value spritesJson = JsonHelper::loadFile(fileEntry.mPath + fileEntry.mFilename);
		for (auto iterator = spritesJson.begin(); iterator != spritesJson.end(); ++iterator)
		{
			const String identifier(iterator.key().asString());
			const uint64 spriteKey = getSpriteKey(identifier);

			std::wstring filename;
			Vec2i center;
			Recti rect;

			for (auto it = iterator->begin(); it != iterator->end(); ++it)
			{
				Json::String keyString = it.key().asString();
				if (keyString == "File")
				{
					JsonHelper::parseWString(filename, it);
				}
				else if (keyString == "Center")
				{
					JsonHelper::parseVec2i(center, it);
				}
				else if (keyString == "Rect")
				{
					JsonHelper::parseRecti(rect, it);
				}
			}

			if (!filename.empty())
			{
				// Check for overloading
				{
					Item* existingItem = mapFind(mSpriteItems, spriteKey);
					if (nullptr != existingItem)
					{
						// This sprite got overloaded e.g. by a mod -- remove the old version
						SAFE_DELETE(existingItem->mSprite);
					}
				}

				Item& item = createItem(spriteKey);
				item.mSourceInfo.mType = SourceInfo::Type::SPRITE_FILE;
				item.mSourceInfo.mSourceIdentifier = *identifier;
				item.mSourceInfo.mMod = mod;

				const std::wstring fullpath = fileEntry.mPath + filename;

				// Palette or RGBA?
				item.mUsesComponentSprite = WString(filename).endsWith(L".png");

				// If this is part of a sprite sheet, we set the sheet key
				uint64 sheetKey = 0;
				if (rect.width != 0)
					sheetKey = rmx::getMurmur2_64(fullpath);

				bool success = false;
				if (!item.mUsesComponentSprite)
				{
					// Load palette sprite (= 8-bit palette sprite)
					PaletteSprite* sprite = new PaletteSprite();
					item.mSprite = sprite;

					const uint64 paletteKey = spriteKey;

					if (sheetKey != 0)
					{
						// Part of a sprite sheet
						PaletteSpriteSheet& sheet = sheetCache.mPaletteSpriteSheets[sheetKey];
						if (sheet.mBitmap.empty())
						{
							success = FileHelper::loadPaletteBitmap(sheet.mBitmap, fullpath, &palette);
							if (success)
							{
								sheet.mFirstSpritePaletteKey = paletteKey;
								addSpritePalette(paletteKey, item, palette);
							}
						}
						else
						{
							success = true;
							mPaletteRedirections[paletteKey] = sheet.mFirstSpritePaletteKey;
						}

						if (success)
						{
							sprite->createFromBitmap(sheet.mBitmap, rect, -center);
						}
					}
					else
					{
						// The sprite is the whole bitmap
						PaletteBitmap bitmap;
						success = FileHelper::loadPaletteBitmap(bitmap, fullpath, &palette);
						if (success)
						{
							sprite->createFromBitmap(std::move(bitmap), -center);
							addSpritePalette(paletteKey, item, palette);
						}
					}
				}
				else
				{
					// Load component sprite (= 32-bit RGBA sprite)
					ComponentSprite* sprite = new ComponentSprite();
					item.mSprite = sprite;

					if (sheetKey != 0)
					{
						// Part of a sprite sheet
						Bitmap& bitmap = sheetCache.mComponentSpriteSheets[sheetKey];
						if (bitmap.empty())
						{
							success = FileHelper::loadBitmap(bitmap, fullpath);
						}
						else
						{
							success = true;
						}

						if (success)
						{
							sprite->accessBitmap().copy(bitmap, rect);
						}
					}
					else
					{
						// The sprite is the whole bitmap
						success = FileHelper::loadBitmap(static_cast<ComponentSprite*>(item.mSprite)->accessBitmap(), fullpath);
					}
					item.mSprite->mOffset = -center;
				}
			}
		}
	}
}

#if defined(PLATFORM_PSP)
void SpriteCollection::loadSpritePack(const std::wstring& path, const Mod* mod)
{
	// A mod's sprites, already cut from their sheets by the PSP mod converter (psp/tools/ModConverter), in the order the
	// sprite JSON files define them. Only the sprites' descriptions get read here: their pixels get loaded when the sprite
	// is used (touchSprite), and freed again when the sprite cache is full (trimSpriteCache). So a mod with sprites for all
	// characters only takes the memory of what's in use, and no sprite sheet is ever in memory.
	//  - Header: "S3SP", version (1), number of palettes, number of sprites (uint32 each)
	//  - Palettes: number of colors (uint32), then the colors (uint32 each, as PaletteBitmap::loadBMP gives them)
	//  - Sprites: identifier length (uint16), identifier (UTF-8), type (uint8: 0 = palette sprite, 1 = component sprite),
	//    0 (uint8), width, height, offset x, offset y (int16 each), palette index (int32, -1 for none), then the pixels:
	//    one byte each for palette sprites, four (RGBA) for component sprites, exactly as the engine would create them
	const std::string packName = (nullptr != mod) ? ("mod '" + mod->mDisplayName + "'") : std::string("the game");
	int packIndex = -1;
	for (size_t i = 0; i < mSpritePacks.size(); ++i)
	{
		if (mSpritePacks[i].mPath == path)
			packIndex = (int)i;
	}
	if (packIndex < 0)
	{
		if (!FTX::FileSystem->exists(path))
			return;
		packIndex = (int)mSpritePacks.size();
		SpritePack& pack = vectorAdd(mSpritePacks);
		pack.mPath = path;
		pack.mModName = packName;
	}
	InputStream* stream = openSpritePack(mSpritePacks[packIndex]);
	if (nullptr == stream)
		return;
	stream->setPosition(0);
	std::vector<SpritePack::IndexEntry>& packSpriteIndex = mSpritePacks[packIndex].mIndex;
	packSpriteIndex.clear();
	auto getCharacterGroup = [](const std::string& identifier) -> uint32
	{
		// "character_sonic_0x1a" -> a hash of "character_sonic" (also "character_tails_tails_0x05" -> "character_tails_tails")
		if (identifier.compare(0, 10, "character_") != 0)
			return 0;
		size_t length = identifier.find_last_of('_');
		if (length == std::string::npos || length <= 10)
			return 0;
		const uint32 group = (uint32)rmx::getMurmur2_64((const uint8*)identifier.data(), length);
		return (group != 0) ? group : 1;
	};

	auto readUint32 = [&](uint32& value) { return stream->read(&value, 4) == 4; };
	auto readInt16 = [&](int16& value) { return stream->read(&value, 2) == 2; };

	bool valid = false;
	uint32 numSprites = 0;
	std::vector<std::vector<uint32>> palettes;
	{
		char signature[4];
		uint32 version = 0;
		uint32 numPalettes = 0;
		if (stream->read(signature, 4) == 4 && memcmp(signature, "S3SP", 4) == 0 && readUint32(version) && version == 1 && readUint32(numPalettes) && readUint32(numSprites) && numPalettes <= 0x10000)
		{
			valid = true;
			palettes.resize(numPalettes);
			for (std::vector<uint32>& palette : palettes)
			{
				uint32 numColors = 0;
				if (!readUint32(numColors) || numColors == 0 || numColors > 256)
				{
					valid = false;
					break;
				}
				palette.resize(numColors);
				if (stream->read(&palette[0], numColors * 4) != numColors * 4)
				{
					valid = false;
					break;
				}
			}
		}
	}

	std::vector<uint64> firstSpriteByPalette(palettes.size(), 0);
	std::vector<bool> paletteRegistered(palettes.size(), false);
	uint32 numIndexed = 0;
	size_t pixelBytes = 0;
	if (valid)
	{
		++mGlobalChangeCounter;
		std::string identifier;
		const size_t streamSize = stream->getSize();
		for (uint32 index = 0; index < numSprites; ++index)
		{
			uint16 identifierLength = 0;
			uint8 typeAndReserved[2];
			int16 width, height, offsetX, offsetY;
			uint32 paletteIndexRaw = 0;
			if (stream->read(&identifierLength, 2) != 2)
				break;
			identifier.resize(identifierLength);
			if (identifierLength > 0 && stream->read(&identifier[0], identifierLength) != identifierLength)
				break;
			if (stream->read(typeAndReserved, 2) != 2 || !readInt16(width) || !readInt16(height) || !readInt16(offsetX) || !readInt16(offsetY) || !readUint32(paletteIndexRaw))
				break;
			if (width < 0 || height < 0 || width > 1024 || height > 1024)
				break;
			const bool isComponentSprite = (typeAndReserved[0] != 0);
			const int paletteIndex = (int)paletteIndexRaw;
			const uint64 spriteKey = getSpriteKey(identifier);
			const size_t bytes = (size_t)width * (size_t)height * (isComponentSprite ? 4 : 1);
			const size_t pixelsOffset = stream->getPosition();
			if (pixelsOffset + bytes > streamSize)
				break;
			stream->setPosition(pixelsOffset + bytes);

			// Overloading another definition (e.g. the base game's): remove the old version, as with the sprite JSON files
			{
				Item* existingItem = mapFind(mSpriteItems, spriteKey);
				if (nullptr != existingItem)
				{
					SAFE_DELETE(existingItem->mSprite);
				}
			}

			Item& item = createItem(spriteKey);
			item.mSourceInfo.mType = SourceInfo::Type::SPRITE_FILE;
			item.mSourceInfo.mSourceIdentifier = identifier;
			item.mSourceInfo.mMod = mod;
			item.mUsesComponentSprite = isComponentSprite;
			if (isComponentSprite)
			{
				item.mSprite = new ComponentSprite();
			}
			else
			{
				item.mSprite = new PaletteSprite();

				// The palette: registered for the first sprite using it, the others redirect there (as for sprite sheets)
				if (paletteIndex >= 0 && paletteIndex < (int)palettes.size())
				{
					if (!paletteRegistered[paletteIndex])
					{
						std::vector<uint32> palette = palettes[paletteIndex];
						addSpritePalette(spriteKey, item, palette);
						firstSpriteByPalette[paletteIndex] = spriteKey;
						paletteRegistered[paletteIndex] = true;
					}
					else
					{
						mPaletteRedirections[spriteKey] = firstSpriteByPalette[paletteIndex];
					}
				}
			}
			item.mSprite->mOffset.set(offsetX, offsetY);
			if (bytes > 0)
			{
				item.mPackIndex = (int16)packIndex;
				item.mPackWidth = (uint16)width;
				item.mPackHeight = (uint16)height;
				item.mPackOffset = (uint32)pixelsOffset;
				packSpriteIndex.push_back(SpritePack::IndexEntry { (uint32)pixelsOffset, getCharacterGroup(identifier), spriteKey });
			}
			++numIndexed;
			pixelBytes += bytes;
		}
	}

	if (numIndexed == numSprites && valid)
	{
		RMX_LOG_INFO("PSP: " << numIndexed << " sprites of " << packName << ", " << (pixelBytes / 1024) << " KB of pixels, loaded when used (sprite cache " << (mPackCacheBudget / 1024) << " KB)");
	}
	else
	{
		RMX_LOG_INFO("PSP: invalid sprite pack of " << packName << ", got " << numIndexed << " of " << numSprites << " sprites");
	}
}

void SpriteCollection::loadPackedPixels(Item& item)
{
	if (item.mPackIndex < 0 || item.mPackIndex >= (int)mSpritePacks.size() || nullptr == item.mSprite)
		return;

	// Reading from the memory stick takes long for each read (10..25 ms, and ~15 ms more if the pack has to be opened
	// again, see openSpritePack), much less for each byte. So more than this one sprite gets read at once:
	//  - For a character's sprite ("character_sonic_0x1a" etc.): all sprites of that character in this pack, in reads of
	//    up to PACK_READ_CHUNK bytes. That's a few hundred KB once (when the character shows up first), and then its
	//    animations never have to wait for the memory stick again.
	//  - Else the sprites right after this one in the pack, up to PACK_READ_AHEAD bytes: usually the next frames of the
	//    same animation, needed soon anyway.
	// (The others get freed again like all sprites when the sprite cache is full, see trimSpriteCache.)
	static constexpr size_t PACK_READ_CHUNK = 48 * 1024;
	static constexpr size_t PACK_READ_AHEAD = 16 * 1024;
	const int16 packIndex = item.mPackIndex;
	SpritePack& pack = mSpritePacks[packIndex];
	const uint32 startTime = sceKernelGetSystemTimeLow();
	const bool wasOpen = (nullptr != pack.mStream);
	InputStream* stream = openSpritePack(pack);
	if (nullptr == stream)
		return;		// (tried again the next time the sprite gets used)
	if (!wasOpen)
		++mPackOpens;

	auto getPackedBytes = [](const Item& other) { return (size_t)other.mPackWidth * other.mPackHeight * (other.mUsesComponentSprite ? 4 : 1); };
	auto getPackItem = [&](size_t index) -> Item*
	{
		// The sprite at this place in the pack, if it's still the one used (a later mod may have overloaded it)
		Item* other = mapFind(mSpriteItems, pack.mIndex[index].mKey);
		return (nullptr != other && other->mPackIndex == packIndex && other->mPackOffset == pack.mIndex[index].mOffset && nullptr != other->mSprite) ? other : nullptr;
	};

	// Which sprites to read: index range [firstIndex, lastIndex] in the pack
	const auto found = std::lower_bound(pack.mIndex.begin(), pack.mIndex.end(), item.mPackOffset, [](const SpritePack::IndexEntry& entry, uint32 offset) { return entry.mOffset < offset; });
	const size_t itemIndex = (size_t)(found - pack.mIndex.begin());
	size_t firstIndex = itemIndex;
	size_t lastIndex = itemIndex;
	uint32 itemGroup = 0;
	if (itemIndex < pack.mIndex.size() && pack.mIndex[itemIndex].mOffset == item.mPackOffset)
	{
		const uint32 group = pack.mIndex[itemIndex].mGroup;
		itemGroup = group;
		if (group != 0)
		{
			while (firstIndex > 0 && pack.mIndex[firstIndex - 1].mGroup == group)
				--firstIndex;
			while (lastIndex + 1 < pack.mIndex.size() && pack.mIndex[lastIndex + 1].mGroup == group)
				++lastIndex;
		}
		else
		{
			for (size_t index = itemIndex + 1; index < pack.mIndex.size(); ++index)
			{
				const Item* other = getPackItem(index);
				if (nullptr != other && pack.mIndex[index].mOffset + getPackedBytes(*other) - item.mPackOffset > PACK_READ_AHEAD)
					break;
				lastIndex = index;
			}
		}
	}
	else
	{
		lastIndex = firstIndex = pack.mIndex.size();	// (not in the index: only this sprite, read on its own below)
	}

	// Read in chunks of consecutive sprites, each up to PACK_READ_CHUNK bytes (or one bigger sprite on its own)
	bool success = true;
	size_t index = firstIndex;
	while (success && index <= lastIndex && index < pack.mIndex.size())
	{
		// Skip the ones loaded already, or not used any more
		Item* firstItem = getPackItem(index);
		if (nullptr == firstItem || firstItem->mPackBytes > 0)
		{
			++index;
			continue;
		}
		const uint32 chunkStart = pack.mIndex[index].mOffset;
		size_t chunkEnd = chunkStart + getPackedBytes(*firstItem);
		size_t chunkLast = index;
		for (size_t next = index + 1; next <= lastIndex; ++next)
		{
			const Item* other = getPackItem(next);
			if (nullptr == other)
				continue;
			const size_t otherEnd = pack.mIndex[next].mOffset + getPackedBytes(*other);
			if (otherEnd - chunkStart > PACK_READ_CHUNK)
				break;
			chunkEnd = otherEnd;
			chunkLast = next;
		}

		mPackReadBuffer.resize(chunkEnd - chunkStart);
		stream->setPosition(chunkStart);
		success = (stream->read(&mPackReadBuffer[0], mPackReadBuffer.size()) == mPackReadBuffer.size());
		++mPackReads;
		for (size_t loaded = index; success && loaded <= chunkLast; ++loaded)
		{
			Item* other = getPackItem(loaded);
			if (nullptr == other || other->mPackBytes > 0)
				continue;
			size_t otherBytes = 0;
			void* destination = createPackedPixels(*other, otherBytes);
			if (nullptr == destination || otherBytes == 0)
				continue;
			memcpy(destination, &mPackReadBuffer[pack.mIndex[loaded].mOffset - chunkStart], otherBytes);
			other->mPackBytes = (uint32)otherBytes;
			// A new change counter: the renderer caches textures by the pixels' address, which may be that of pixels freed before
			other->mChangeCounter = ++mGlobalChangeCounter;
			mPackLoadedBytes += otherBytes;
			++mPackLoads;
			if (other != &item)
			{
				other->mLastUsedFrame = (itemGroup != 0) ? (mPackFrame - 1) : 0;		// (not used yet: the character's other sprites stay a while, others go first when the cache is full)
				++mPackPrefetched;
			}
		}
		index = chunkLast + 1;
	}
	if (mPackReadBuffer.capacity() > PACK_READ_CHUNK * 2)
		std::vector<uint8>().swap(mPackReadBuffer);		// (a single big sprite: don't keep its buffer)

	// Not in the index (shouldn't happen): only this sprite
	if (success && item.mPackBytes == 0)
	{
		size_t bytes = 0;
		void* destination = createPackedPixels(item, bytes);
		stream->setPosition(item.mPackOffset);
		success = (nullptr != destination && stream->read(destination, bytes) == bytes);
		++mPackReads;
		if (success)
		{
			item.mPackBytes = (uint32)bytes;
			item.mChangeCounter = ++mGlobalChangeCounter;
			mPackLoadedBytes += bytes;
			++mPackLoads;
		}
	}
	mPackIoMicroseconds += (uint32)(sceKernelGetSystemTimeLow() - startTime);

	if (!success)
	{
		// Can't read it: leave it empty (not drawn), and don't try again
		if (!pack.mLoggedError)
		{
			RMX_LOG_INFO("PSP: could not read a sprite from the sprite pack of " << pack.mModName);
			pack.mLoggedError = true;
		}
		size_t bytes = 0;
		createPackedPixels(item, bytes);
		item.mPackBytes = (uint32)bytes;	// (so freePackedPixels below gets the accounting right)
		mPackLoadedBytes += bytes;
		freePackedPixels(item);
		item.mPackIndex = -1;
	}
}

void* SpriteCollection::createPackedPixels(Item& item, size_t& outBytes)
{
	// The sprite's bitmap in its size, to read its pixels into
	const int width = item.mPackWidth;
	const int height = item.mPackHeight;
	if (item.mUsesComponentSprite)
	{
		Bitmap& bitmap = static_cast<ComponentSprite*>(item.mSprite)->accessBitmap();
		bitmap.create(width, height);
		outBytes = (size_t)width * height * 4;
		return bitmap.getData();
	}
	else
	{
		PaletteBitmap& bitmap = static_cast<PaletteSprite*>(item.mSprite)->accessBitmap();
		bitmap.create(width, height);
		outBytes = (size_t)width * height;
		return bitmap.getData();
	}
}

void SpriteCollection::freePackedPixels(Item& item)
{
	if (nullptr != item.mSprite)
	{
		if (item.mUsesComponentSprite)
			static_cast<ComponentSprite*>(item.mSprite)->accessBitmap().clear();
		else
			PaletteBitmap().swap(static_cast<PaletteSprite*>(item.mSprite)->accessBitmap());
	}
	mPackLoadedBytes -= std::min<size_t>(item.mPackBytes, mPackLoadedBytes);
	item.mPackBytes = 0;
	item.mChangeCounter = ++mGlobalChangeCounter;
}

void SpriteCollection::trimSpriteCache()
{
	// The sprites used since the last call are the ones drawn now: those stay. The others can go, oldest use first:
	// the GE finished drawing everything before.
	const size_t budget = getSpriteCacheBudget();
	if (mPackLoadedBytes > budget)
	{
		std::vector<Item*> candidates;
		for (auto& pair : mSpriteItems)
		{
			Item& item = pair.second;
			if (item.mPackBytes > 0 && item.mLastUsedFrame != mPackFrame)
				candidates.push_back(&item);
		}
		std::sort(candidates.begin(), candidates.end(), [](const Item* a, const Item* b) { return a->mLastUsedFrame < b->mLastUsedFrame; });
		for (Item* item : candidates)
		{
			if (mPackLoadedBytes <= budget * 7 / 8)		// (a bit below the budget, so this doesn't happen every frame)
				break;
			freePackedPixels(*item);
			++mPackEvictions;
		}
	}
	++mPackFrame;
}

InputStream* SpriteCollection::openSpritePack(SpritePack& pack)
{
	// The memory stick only lets the game have a few files open at once (and the audio needs some of them while playing):
	// so only the sprite packs used last stay open, the others get opened again when one of their sprites gets loaded
	static constexpr int MAX_OPEN_SPRITE_PACKS = 2;
	pack.mLastOpenUse = ++mPackOpenUses;
	if (nullptr != pack.mStream)
		return pack.mStream;

	while (true)
	{
		int numOpen = 0;
		SpritePack* oldest = nullptr;
		for (SpritePack& other : mSpritePacks)
		{
			if (nullptr == other.mStream)
				continue;
			++numOpen;
			if (nullptr == oldest || other.mLastOpenUse < oldest->mLastOpenUse)
				oldest = &other;
		}
		if (numOpen < MAX_OPEN_SPRITE_PACKS || nullptr == oldest)
			break;
		oldest->mStream->close();
		delete oldest->mStream;
		oldest->mStream = nullptr;
	}

	pack.mStream = FTX::FileSystem->createInputStream(pack.mPath);
	if (nullptr == pack.mStream && !pack.mLoggedError)
	{
		RMX_LOG_INFO("PSP: could not open the sprite pack of " << pack.mModName);
		pack.mLoggedError = true;
	}
	return pack.mStream;
}

void SpriteCollection::closeSpritePacks()
{
	for (SpritePack& pack : mSpritePacks)
	{
		if (nullptr != pack.mStream)
		{
			pack.mStream->close();
			delete pack.mStream;
		}
	}
	mSpritePacks.clear();
}
#endif

void SpriteCollection::addSpritePalette(uint64 paletteKey, const Item& item, std::vector<uint32>& palette)
{
	// After loading from a BMP, the palette is set to all opaque colors, but we usually need index 0 to be transparent
	palette[0] &= 0x00ffffff;
	SpritePalette& spritePalette = mSpritePalettes[paletteKey];
	spritePalette.mItem = &item;
	spritePalette.mColors = palette;
}
