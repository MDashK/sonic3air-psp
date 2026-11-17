/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "sonic3air/pch.h"
#include "sonic3air/EngineDelegate.h"
#include "sonic3air/GameArgumentsReader.h"
#include "sonic3air/helper/PackageBuilder.h"
#include "sonic3air/platform/PlatformSpecifics.h"

#include "oxygen/platform/CommandForwarder.h"
#include "oxygen/platform/PlatformFunctions.h"

#ifdef RMX_USE_SDL3
	#include <SDL3/SDL_main.h>
#endif


// [Added for Switch platform] HJW: I know it's sloppy to put this here... it'll get moved afterwards
// Building with my env (msys2,gcc) requires this stub for some reason
#ifndef pathconf
long pathconf(const char* path, int name)
{
	errno = ENOSYS;
	return -1;
}
#endif

#if defined(PLATFORM_WINDOWS) & !defined(__GNUC__)
extern "C"
{
	// Tell graphics drivers to prefer the dedicated GPU, if there's integrated graphics as well
	_declspec(dllexport) uint32 NvOptimusEnablement = 1;
	_declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#if defined(PLATFORM_VITA)
extern "C"
{
	// Any value higher than 324 MB will make the game either boot without sound or just crash the PSVITA due to lack of physical RAM
	int _newlib_heap_size_user = 324 * 1024 * 1024;
	unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}
#endif

#if defined(PLATFORM_PSP)
	#include <pspkernel.h>
	#include <psppower.h>
	#include <pspsdk.h>
	PSP_MODULE_INFO("S3AIR", PSP_MODULE_USER, 1, 0);
	PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
	PSP_MAIN_THREAD_STACK_SIZE_KB(1024);

	// The HOME menu's "exit game"
	static int pspExitCallback(int arg1, int arg2, void* common)
	{
		sceKernelExitGame();
		return 0;
	}
	static int pspCallbackThread(SceSize args, void* argp)
	{
		const int cbid = sceKernelCreateCallback("ExitCallback", pspExitCallback, nullptr);
		sceKernelRegisterExitCallback(cbid);
		sceKernelSleepThreadCB();
		return 0;
	}
	static void pspSetupCallbacks()
	{
		const int thid = sceKernelCreateThread("CallbackThread", pspCallbackThread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, 0);
		if (thid >= 0)
			sceKernelStartThread(thid, 0, 0);
	}
	// malloc gets all memory except this much, left for the other threads' stacks (SDL audio etc.)
	PSP_HEAP_THRESHOLD_SIZE_KB(1536);
#endif


#if defined(PLATFORM_PSP)
	// Memory diagnostics: the allocation that fails (and with S3AIR_BENCH, the big ones, into "allocs.txt")
	#include <malloc.h>
	#include <new>
	#include <exception>
	namespace pspmem
	{
		// On the PSPLink console as well (psplink_run.sh keeps pspsh attached): an allocation that fails, and the heap then
		static void printHeap(const char* what)
		{
			struct mallinfo mi = mallinfo();
			printf("[mem] %s: heap used %.2f MB, arena %.2f MB, free in arena %.2f MB, outside the heap: max block %u KB\n", what,
				   mi.uordblks / 1048576.0, mi.arena / 1048576.0, mi.fordblks / 1048576.0, (unsigned)(sceKernelMaxFreeMemSize() / 1024));
		}

		// An exception nothing catches: say so on the console; with S3AIR_BENCH stop right here, so PSPLink shows it (and keeps the state)
		static void onTerminate()
		{
			printHeap("TERMINATE, uncaught exception");
			fflush(stdout);
			if (nullptr != getenv("S3AIR_BENCH"))
				*(volatile int*)nullptr = 0;
			abort();
		}

		static bool gAllocLogStarted = false;
		static void logAlloc(const char* tag, size_t size, void* caller)
		{
			static const bool enabled = (nullptr != getenv("S3AIR_BENCH"));
			if (!enabled)
				return;
			// Opened for each line: the memory stick lets the game have only ~10 files open at once
			FILE* allocLog = fopen("allocs.txt", gAllocLogStarted ? "a" : "w");
			if (nullptr == allocLog)
				return;
			gAllocLogStarted = true;
			struct mallinfo mi = mallinfo();
			fprintf(allocLog, "%s size=%u caller=%p heap_used=%u heap_arena=%u\n", tag, (unsigned)size, caller, (unsigned)mi.uordblks, (unsigned)mi.arena);
			fclose(allocLog);
		}
	}
	void* operator new(size_t size)
	{
		void* ptr = malloc(size ? size : 1);
		if (nullptr == ptr)
		{
			pspmem::logAlloc("FAILED", size, __builtin_return_address(0));
			char what[96];
			snprintf(what, sizeof(what), "OUT OF MEMORY, %u bytes (caller %p)", (unsigned)size, __builtin_return_address(0));
			pspmem::printHeap(what);
			throw std::bad_alloc();
		}
		if (size >= 256 * 1024)
			pspmem::logAlloc("big", size, __builtin_return_address(0));
		return ptr;
	}
	void* operator new[](size_t size)  { return operator new(size); }
	void operator delete(void* ptr) noexcept  { free(ptr); }
	void operator delete[](void* ptr) noexcept  { free(ptr); }
	void operator delete(void* ptr, size_t) noexcept  { free(ptr); }
	void operator delete[](void* ptr, size_t) noexcept  { free(ptr); }
#endif

int main(int argc, char** argv)
{
#if defined(PLATFORM_PSP)
	// Like on PC: invalid float operations give NaN / infinity instead of an exception
	pspSdkDisableFPUExceptions();
	std::set_terminate(&pspmem::onTerminate);
	SDL_SetMainReady();
	pspSetupCallbacks();
	{
		FILE* f = fopen("bench.txt", "r");
		if (nullptr != f)
		{
			char line[256];
			while (fgets(line, sizeof(line), f))
			{
				char* eq = strchr(line, '=');
				if (nullptr == eq)
					continue;
				*eq = 0;
				char* value = eq + 1;
				for (char* c = value; *c; ++c)
					if (*c == 13 || *c == 10) { *c = 0; break; }
				setenv(line, value, 1);
			}
			fclose(f);
		}
	}
#endif
	EngineMain::earlySetup();
	PlatformSpecifics::platformStartup();
	CommandForwarder::setApplicationName("S3AIR");

	GameArgumentsReader arguments;

#if defined(PLATFORM_VITA)
	argc = 0;
#else
	// Read command line arguments
	arguments.read(argc, argv);

	// For certain arguments, just try to forward them to an already running instance of S3AIR
	if (!arguments.mForwardedCommand.empty())
	{
		if (CommandForwarder::trySendCommand("ForwardedCommand:" + arguments.mForwardedCommand))
			return 0;
	}
	if (!arguments.mUrl.empty())
	{
		if (CommandForwarder::trySendCommand("ForwardedCommand:Url:" + arguments.mUrl))
			return 0;
	}
	if (arguments.mStop)
		return 0;

	// Make sure we're in the correct working directory
	PlatformFunctions::changeWorkingDirectory(arguments.mExecutableCallPath);
#endif

#if defined(PLATFORM_WINDOWS)
	// Check if the user has an old version of "audioremaster.bin", and remove it if that's the case
	//  -> As the newer installations don't include that file, it is most likely an out-dated one, and could cause problems (at least an assert) down the line
	if (FTX::FileSystem->exists(L"data/audioremaster.bin"))
		FTX::FileSystem->removeFile(L"data/audioremaster.bin");
#endif

#if !defined(PLATFORM_ANDROID) && !defined(PLATFORM_VITA)
	if (arguments.mPack)
	{
		PackageBuilder::performPacking();
		if (!arguments.mNativize && !arguments.mDumpCppDefinitions)		// In case multiple arguments got combined, the others would get ignored without this check
			return 0;
	}
#endif

	// Randomization is quite important for server communication
	randomize();

	try
	{
		// Create engine delegate and engine main instance
		EngineDelegate myDelegate;
		EngineMain myMain(myDelegate, arguments);

		// Evaluate some more arguments
		Configuration& config = Configuration::instance();
		if (arguments.mNativize)
		{
			config.mRunScriptNativization = 1;
			config.mScriptNativizationOutput = L"source/sonic3air/_nativized/NativizedCode.inc";
			config.mExitAfterScriptLoading = true;
		}
		if (arguments.mDumpCppDefinitions)
		{
			config.mDumpCppDefinitionsOutput = L"scripts/_reference/cpp_core_functions.lemon";
			config.mExitAfterScriptLoading = true;
		}

		// Now run the game
		myMain.execute();
	}
	catch (const std::exception& e)
	{
		RMX_ERROR("Caught unhandled exception in main loop: " << e.what(), );
	}

	return 0;
}
