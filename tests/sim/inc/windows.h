// Just enough of windows.h for the simulator build of the script (Linux).
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int BOOL;
typedef uint64_t UINT64;
typedef UINT64 *PUINT64;
typedef void *HMODULE;
typedef void *LPVOID;
typedef unsigned int UINT;
typedef void *HKL;
typedef int64_t LONGLONG;
typedef union
{
	struct
	{
		uint32_t LowPart;
		int32_t HighPart;
	};
	LONGLONG QuadPart;
} LARGE_INTEGER;

#define TRUE 1
#define FALSE 0
#define APIENTRY
#define WINAPI
#define MAXDWORD 0xffffffffu
// (as the real windows.h: these two are macros for nothing, so a variable named so breaks the real build)
#define near
#define far
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F24 0x87
#define MAPVK_VK_TO_CHAR 2

BOOL QueryPerformanceCounter(LARGE_INTEGER *c); // the simulator's clock
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
	f->QuadPart = 1000000000LL;
	return TRUE;
}
inline short GetAsyncKeyState(int) { return 0; }
inline UINT MapVirtualKeyW(UINT, UINT) { return 0; }
inline BOOL GetKeyboardState(BYTE *) { return FALSE; }
inline int ToUnicodeEx(UINT, UINT, const BYTE *, wchar_t *, int, UINT, HKL) { return 0; }
inline HKL GetKeyboardLayout(DWORD) { return nullptr; }
#define MAX_PATH 260
// MCPassthrough.ini: none in the simulator (the defaults apply)
inline DWORD GetModuleFileNameA(HMODULE, char *path, DWORD size)
{
	snprintf(path, size, "MCPassthrough.asi");
	return DWORD(strlen(path));
}
inline DWORD GetPrivateProfileStringA(const char *, const char *, const char *def, char *out, DWORD size, const char *)
{
	snprintf(out, size, "%s", def);
	return DWORD(strlen(out));
}
inline UINT GetPrivateProfileIntA(const char *, const char *, int def, const char *) { return UINT(def); }
int sim_sscanf_s(const char *str, const char *fmt, ...);
#define sscanf_s sim_sscanf_s
