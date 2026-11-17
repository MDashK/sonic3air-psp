/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/engine/modding/Mod.h"
#include <functional>

class ZipFileProvider;


class ModManager : public SingleInstance<ModManager>
{
public:
	~ModManager();

	inline const std::wstring& getModsBasePath() const  { return mBasePath; }

	inline const std::vector<Mod*>& getAllMods() const	   { return mAllMods; }
	inline const std::vector<Mod*>& getActiveMods() const  { return mActiveMods; }	// Sorted in inverse priority, i.e. highest prio mods are at the end of the list
	inline const std::unordered_map<uint64, Mod*>& getActiveModsByNameHash() const	{ return mActiveModsByNameHash; }
	inline const std::unordered_map<uint64, Mod*>& getModsByIDHash() const			{ return mModsByIDHash; }

	Mod* findModByIDHash(uint64 idHash) const  { return mapFindOrDefault(mModsByIDHash, idHash, nullptr); }

	void startup();
	void clear();
	bool rescanMods();
	void saveActiveMods();

	void setActiveMods(const std::vector<Mod*>& newActiveModsList);

	bool anyActiveModUsesFeature(uint64 featureNameHash) const;

#if defined(PLATFORM_PSP)
	// PSP: what the active mods may use in all, so the game itself never runs out of memory
	//  -> The game uses up to ~37 MB of the ~40 MB heap (special stages), this is a part of the rest
	//  -> Mods need some of it all the time (sprite objects and palettes, sounds, scripts, data: "MemoryBytes" in psp.json),
	//     the rest is the cache for their sprites' pixels, which get loaded when used (see SpriteCollection::loadSpritePack)
	//  -> Mods with sprites need at least PSP_MOD_SPRITE_CACHE_MIN of cache (or less if their sprites are smaller)
	static constexpr uint32 PSP_MOD_MEMORY_BUDGET = 1536 * 1024;
	static constexpr uint32 PSP_MOD_SPRITE_CACHE_MIN = 512 * 1024;
	static uint32 getPspMemoryBytes(const std::vector<Mod*>& mods);
	static bool fitsPspMemoryBudget(const std::vector<Mod*>& mods);
#endif

	void copyModSettingsFromConfig();
	void copyModSettingsToConfig();

	bool addZipFileProvider(const std::wstring& zipLocalPath);
	bool tryRemoveZipFileProvider(const std::wstring& zipLocalPath);

private:
	struct FoundMod
	{
		std::wstring mLocalPath;
		std::wstring mDirectoryName;
		Json::Value mModJson;
	};

private:
	bool scanMods();
	void scanDirectoryRecursive(std::vector<FoundMod>& outFoundMods, const std::wstring& localPath);
	void findZipsRecursively(std::vector<std::wstring>& outZipPaths, const std::wstring& localPath, int maxDepth);
	void onActiveModsChanged(bool duringStartup = false);

private:
	std::wstring mBasePath;
	std::vector<Mod*> mAllMods;
	std::vector<Mod*> mActiveMods;
	std::unordered_map<uint64, Mod*> mActiveModsByNameHash;		// Each mod is registered by both its internal name and display name
	std::unordered_map<uint64, Mod*> mModsByLocalDirectoryHash;
	std::unordered_map<uint64, Mod*> mModsByIDHash;
	std::map<std::wstring, ZipFileProvider*> mZipFileProviders;
};
