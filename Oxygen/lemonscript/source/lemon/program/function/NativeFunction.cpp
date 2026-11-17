/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "lemon/pch.h"
#include "lemon/program/function/NativeFunction.h"
#include "lemon/runtime/Runtime.h"

#if defined(PLATFORM_PSP)
	#include <pspthreadman.h>
#endif


namespace lemon
{

	void NativeFunction::setFunction(const FunctionWrapper& functionWrapper)
	{
		mFunctionWrapper = &functionWrapper;
		mReturnType = functionWrapper.getReturnType();
		setParametersByTypes(functionWrapper.getParameterTypes());
	}

	NativeFunction& NativeFunction::setParameterInfo(size_t index, const std::string& identifier)
	{
		RMX_ASSERT(index < mParameters.size(), "Invalid parameter index " << index);
		RMX_ASSERT(!mParameters[index].mName.isValid(), "Parameter identifier is already set for index " << index);
		mParameters[index].mName.set(identifier);
		return *this;
	}

	void NativeFunction::execute(const Context context) const
	{
		RuntimeDetailHandler* runtimeDetailHandler = context.mControlFlow.getRuntime().getRuntimeDetailHandler();
		if (nullptr != runtimeDetailHandler)
		{
			runtimeDetailHandler->preExecuteExternalFunction(*this, context.mControlFlow);
			mFunctionWrapper->execute(context);
			runtimeDetailHandler->postExecuteExternalFunction(*this, context.mControlFlow);
		}
		else
		{
		#if defined(PLATFORM_PSP)
			// PSP benchmarks only: time spent in each native function, and log the ones that stall the game
			//  -> Never in normal play: reading the clock is a system call, and doing it twice per native call made the scripts much slower
			if (mStatEnabled)
			{
				const uint32 start = sceKernelGetSystemTimeLow();
				mFunctionWrapper->execute(context);
				const uint32 microseconds = sceKernelGetSystemTimeLow() - start;
				if (!mStatListed)
				{
					mStatCalledFunctions.push_back(this);
					mStatListed = true;
				}
				++mStatCalls;
				mStatMicroseconds += microseconds;
				if (microseconds >= 20000)
				{
					RMX_LOG_INFO("Slow native function: " << getName().getString() << " took " << (microseconds / 1000) << " ms");
				}
				return;
			}
		#endif
			mFunctionWrapper->execute(context);
		}
	}

}
