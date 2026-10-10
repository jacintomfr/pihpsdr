/* Windows (MinGW64) shim for WDSP's <wdsp.h>.
 * WHY: wdsp.h is written for the Linux/macOS build and #defines Win32 type
 * names itself (DWORD, LPCRITICAL_SECTION, __stdcall). If it is parsed
 * before <windows.h>, the later Windows headers break ("expected identifier
 * before ';'" in minwinbase.h); if after, every later Windows header would
 * see the macros. So: Windows headers first, then wdsp.h with its macros
 * confined to the WDSP prototypes (same types on Win64: DWORD is unsigned
 * long, the pointer arguments are void * at the ABI level). */
#pragma once
#include "win_compat.h"
#pragma push_macro("DWORD")
#pragma push_macro("LPCRITICAL_SECTION")
#pragma push_macro("__stdcall")
#undef DWORD
#undef LPCRITICAL_SECTION
#undef __stdcall
#include_next <wdsp.h>
#undef DWORD
#undef LPCRITICAL_SECTION
#undef __stdcall
#pragma pop_macro("DWORD")
#pragma pop_macro("LPCRITICAL_SECTION")
#pragma pop_macro("__stdcall")
