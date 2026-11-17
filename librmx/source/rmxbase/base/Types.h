/*
*	rmx Library
*	Copyright (C) 2008-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once


#ifdef int8
	#undef int8
#endif
#ifdef uint8
	#undef uint8
#endif
#ifdef int16
	#undef int16
#endif
#ifdef uint16
	#undef uint16
#endif
#ifdef int32
	#undef int32
#endif
#ifdef uint32
	#undef uint32
#endif
#ifdef int64
	#undef int64
#endif
#ifdef uint64
	#undef uint64
#endif


// Data types
typedef signed char		int8;
typedef unsigned char	uint8;
typedef signed short	int16;
typedef unsigned short	uint16;
typedef signed int		int32;
typedef unsigned int	uint32;
typedef int64_t			int64;
typedef uint64_t		uint64;


// Types for memory that may not be naturally aligned (e.g. emulated 68k memory, packed data)
//  -> Platforms with strict alignment (like the PSP) get safe accesses through pointers to these types
#if defined(__GNUC__) || defined(__clang__)
	typedef int16  __attribute__((aligned(1), may_alias)) int16_ua;
	typedef uint16 __attribute__((aligned(1), may_alias)) uint16_ua;
	typedef int32  __attribute__((aligned(1), may_alias)) int32_ua;
	typedef uint32 __attribute__((aligned(1), may_alias)) uint32_ua;
	typedef int64  __attribute__((aligned(1), may_alias)) int64_ua;
	typedef uint64 __attribute__((aligned(1), may_alias)) uint64_ua;
	typedef float  __attribute__((aligned(1), may_alias)) float_ua;
#else
	typedef int16  int16_ua;
	typedef uint16 uint16_ua;
	typedef int32  int32_ua;
	typedef uint32 uint32_ua;
	typedef int64  int64_ua;
	typedef uint64 uint64_ua;
	typedef float  float_ua;
#endif
