/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include <rmxmedia.h>
#include "oxygen/rendering/sprite/ComponentSprite.h"
#include "oxygen/rendering/sprite/PaletteSprite.h"

class EmulatorInterface;
class Mod;
class SpriteDump;


class SpriteCollection : public SingleInstance<SpriteCollection>
{
friend class PaletteCollection;	// For direct access to mSpritePalettes

public:
	enum class ROMSpriteEncoding : uint8
	{
		NONE		= 0,
		CHARACTER	= 1,
		OBJECT		= 2,
		KOSINSKI	= 3
	};

	struct ROMSpriteData
	{
		uint32 mPatternsBaseAddress = 0;
		uint32 mTableAddress = 0;
		uint32 mMappingOffset = 0;
		uint8  mAnimationSprite = 0;
		ROMSpriteEncoding mEncoding = ROMSpriteEncoding::NONE;
		int16  mIndexOffset = 0;

		void serialize(VectorBinarySerializer& serializer);
		uint64 getKey() const;
	};

	struct SourceInfo
	{
		enum class Type : uint8
		{
			UNKNOWN,
			SPRITE_FILE,
			ROM_DATA
		};

		Type mType = Type::UNKNOWN;
		ROMSpriteData mROMSpriteData;	// Only for type ROM_DATA
		std::string mSourceIdentifier;	// Only for type SPRITE_FILE
		const Mod* mMod = nullptr;		// Only for type SPRITE_FILE
	};

	struct Item
	{
		uint64 mKey = 0;
		bool mUsesComponentSprite = false;
		SpriteBase* mSprite = nullptr;
		uint32 mChangeCounter = 0;
		Item* mRedirect = nullptr;
		SourceInfo mSourceInfo;
		bool mGotDumped = false;
	#if defined(PLATFORM_PSP)
		// PSP: a mod's sprite from its sprite pack, whose pixels get loaded when needed (see loadSpritePack)
		int16 mPackIndex = -1;				// In mSpritePacks, -1 if not from a pack
		uint16 mPackWidth = 0;
		uint16 mPackHeight = 0;
		uint32 mPackOffset = 0;				// Where the pixels are in the pack file
		mutable uint32 mPackBytes = 0;		// Pixel bytes loaded right now, 0 if not loaded
		mutable uint32 mLastUsedFrame = 0;
	#endif
	};

public:
	SpriteCollection();
	~SpriteCollection();

	void clear();
	void loadAllSpriteDefinitions();

	bool hasSprite(uint64 key) const;
	const Item* getSprite(uint64 key);
	Item& getOrCreatePaletteSprite(uint64 key);
	Item& getOrCreateComponentSprite(uint64 key);

	inline const std::unordered_map<uint64, Item>& getAllSprites() const  { return mSpriteItems; }

	Item& setupSpriteFromROM(EmulatorInterface& emulatorInterface, const ROMSpriteData& romSpriteData, uint8 atex);

	void clearRedirect(uint64 sourceKey);
	void setupRedirect(uint64 sourceKey, uint64 targetKey);

	inline uint32 getGlobalChangeCounter() const  { return mGlobalChangeCounter; }

	SpriteDump& getSpriteDump();
	void dumpSprite(uint64 key, std::string_view categoryKey, uint8 spriteNumber, uint8 atex);

#if defined(PLATFORM_PSP)
	// PSP: mod sprites from sprite packs get their pixels loaded when used, and the least recently used ones freed again
	// when they take more than the sprite cache budget
	//  - "touchSprite": marks the sprite as used now and loads its pixels if needed; getSprite does it, and so does the
	//    renderer for every sprite it draws (render items can be drawn again without a new getSprite, e.g. while paused)
	//  - "trimSpriteCache": call at the start of a frame, when the GE finished the last frame
	inline void touchSprite(const Item& item)  { if (item.mPackIndex >= 0) { item.mLastUsedFrame = mPackFrame; if (item.mPackBytes == 0) loadPackedPixels(const_cast<Item&>(item)); } }
	void trimSpriteCache();
	inline size_t getSpriteCacheBytes() const  { return mPackLoadedBytes; }
	inline size_t getSpriteCacheBudget() const  { return mPackCacheBudget + (mPackHasModSprites ? mPackCacheExtra : 0); }
	inline void setSpriteCacheExtra(size_t bytes)  { mPackCacheExtra = bytes; }	// More cache for the mods' sprites where there's memory left (in levels), see Game::onPostUpdateFrame
	inline uint32 getSpriteCacheLoads() const  { return mPackLoads; }
	inline uint32 getSpriteCacheEvictions() const  { return mPackEvictions; }
	inline uint32 getSpriteCacheOpens() const  { return mPackOpens; }
	inline uint32 getSpriteCacheReads() const  { return mPackReads; }
	inline uint32 getSpriteCachePrefetched() const  { return mPackPrefetched; }
	inline uint32 getSpriteCacheIoMilliseconds() const  { return (uint32)(mPackIoMicroseconds / 1000); }
#endif

private:
	struct SpritePalette
	{
		const Item* mItem = nullptr;
		std::vector<uint32> mColors;
	};

private:
	Item& createItem(uint64 key);
	void loadSpriteDefinitions(const std::wstring& path, const Mod* mod);
#if defined(PLATFORM_PSP)
	void loadSpritePack(const std::wstring& path, const Mod* mod);	// Mod is null for the game's own sprites
	void loadPackedPixels(Item& item);
	void* createPackedPixels(Item& item, size_t& outBytes);
	void freePackedPixels(Item& item);
	void closeSpritePacks();
#endif
	void addSpritePalette(uint64 paletteKey, const Item& item, std::vector<uint32>& palette);

private:
	std::unordered_map<uint64, Item> mSpriteItems;

	std::unordered_map<uint64, SpritePalette> mSpritePalettes;
	std::unordered_map<uint64, uint64> mPaletteRedirections;

	SpriteDump* mSpriteDump = nullptr;
	uint32 mGlobalChangeCounter = 0;

#if defined(PLATFORM_PSP)
	struct SpritePack
	{
		std::wstring mPath;
		InputStream* mStream = nullptr;		// Only open while in use, see openSpritePack
		uint32 mLastOpenUse = 0;
		std::string mModName;
		bool mLoggedError = false;
		struct IndexEntry
		{
			uint32 mOffset = 0;		// Where the pixels are in the pack file
			uint32 mGroup = 0;		// For a character's sprites: the same for all of that character (e.g. "character_sonic_..."), else 0
			uint64 mKey = 0;
		};
		std::vector<IndexEntry> mIndex;		// The pack's sprites in file order (for reading ahead)
	};
	InputStream* openSpritePack(SpritePack& pack);
	std::vector<SpritePack> mSpritePacks;
	std::vector<uint8> mPackReadBuffer;
	uint32 mPackOpenUses = 0;
	uint32 mPackOpens = 0;
	uint32 mPackReads = 0;
	uint32 mPackPrefetched = 0;
	uint64 mPackIoMicroseconds = 0;
	size_t mPackLoadedBytes = 0;
	size_t mPackCacheBudget = 0;
	size_t mPackCacheExtra = 0;
	bool mPackHasModSprites = false;
	uint32 mPackFrame = 1;
	uint32 mPackLoads = 0;
	uint32 mPackEvictions = 0;
#endif
};
