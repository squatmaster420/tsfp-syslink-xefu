/* win.h - the handful of Win32 declarations the injector needs, so it builds
 * with plain clang + lld-link and no Windows SDK. */
#ifndef TSFP_WIN_H
#define TSFP_WIN_H
#include "shared.h"

#define WINAPI __stdcall
typedef void *HANDLE;
typedef int BOOL;
typedef u32 DWORD;
typedef u64 SIZE_T;

#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define TH32CS_SNAPPROCESS 0x2
#define TH32CS_SNAPMODULE  0x8
#define TH32CS_SNAPTHREAD  0x4
#define PROCESS_ACCESS     (0x0008 | 0x0010 | 0x0020 | 0x0400 | 0x0800 | 0x00100000) /* VM_OPERATION|VM_READ|VM_WRITE|QUERY_INFORMATION|SUSPEND_RESUME|SYNCHRONIZE */
#define THREAD_ACCESS      (0x0002 | 0x0008)  /* SUSPEND_RESUME | GET_CONTEXT */
#define MEM_COMMIT  0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_READWRITE 0x04
#define STILL_ACTIVE 259

typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID;
    u64   th32DefaultHeapID;
    DWORD th32ModuleID, cntThreads, th32ParentProcessID;
    int   pcPriClassBase;
    DWORD dwFlags;
    char  szExeFile[260];
} PROCESSENTRY32;

typedef struct {
    DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
    u8   *modBaseAddr;
    DWORD modBaseSize;
    HANDLE hModule;
    char  szModule[256];
    char  szExePath[260];
} MODULEENTRY32;

typedef struct {
    DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID;
    int   tpBasePri, tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32;

typedef struct {
    u64 BaseAddress, AllocationBase;
    DWORD AllocationProtect, PartitionId_pad;
    u64 RegionSize;
    DWORD State, Protect, Type, pad2;
} MEMORY_BASIC_INFORMATION;

HANDLE WINAPI GetModuleHandleA(const char *);
void  *WINAPI GetProcAddress(HANDLE, const char *);
HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD);
BOOL   WINAPI Process32First(HANDLE, PROCESSENTRY32 *);
BOOL   WINAPI Process32Next(HANDLE, PROCESSENTRY32 *);
BOOL   WINAPI Module32First(HANDLE, MODULEENTRY32 *);
BOOL   WINAPI Module32Next(HANDLE, MODULEENTRY32 *);
BOOL   WINAPI Thread32First(HANDLE, THREADENTRY32 *);
BOOL   WINAPI Thread32Next(HANDLE, THREADENTRY32 *);
HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD);
HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD);
DWORD  WINAPI SuspendThread(HANDLE);
DWORD  WINAPI ResumeThread(HANDLE);
BOOL   WINAPI GetThreadContext(HANDLE, void *);
BOOL   WINAPI ReadProcessMemory(HANDLE, const void *, void *, SIZE_T, SIZE_T *);
BOOL   WINAPI WriteProcessMemory(HANDLE, void *, const void *, SIZE_T, SIZE_T *);
void  *WINAPI VirtualAllocEx(HANDLE, void *, SIZE_T, DWORD, DWORD);
BOOL   WINAPI VirtualFreeEx(HANDLE, void *, SIZE_T, DWORD);
BOOL   WINAPI VirtualProtectEx(HANDLE, void *, SIZE_T, DWORD, DWORD *);
SIZE_T WINAPI VirtualQueryEx(HANDLE, const void *, MEMORY_BASIC_INFORMATION *, SIZE_T);
void  *WINAPI VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
BOOL   WINAPI VirtualFree(void *, SIZE_T, DWORD);
BOOL   WINAPI FlushInstructionCache(HANDLE, const void *, SIZE_T);
BOOL   WINAPI GetExitCodeProcess(HANDLE, DWORD *);
BOOL   WINAPI CloseHandle(HANDLE);
void   WINAPI Sleep(DWORD);
DWORD  WINAPI GetTickCount(void);
DWORD  WINAPI GetLastError(void);
HANDLE WINAPI GetStdHandle(DWORD);
HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
BOOL   WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DWORD  WINAPI GetModuleFileNameA(HANDLE, char *, DWORD);
DWORD  WINAPI GetPrivateProfileStringA(const char *, const char *, const char *, char *, DWORD, const char *);
BOOL   WINAPI WritePrivateProfileStringA(const char *, const char *, const char *, const char *);
DWORD  WINAPI GetFileAttributesA(const char *);
BOOL   WINAPI CreateProcessA(const char *, char *, void *, void *, BOOL, DWORD, void *, const char *, void *, void *);
BOOL   WINAPI SetConsoleTitleA(const char *);
HANDLE WINAPI GetStdHandle(DWORD);
BOOL   WINAPI ReadConsoleA(HANDLE, void *, DWORD, DWORD *, void *);
void   WINAPI ExitProcess(DWORD);
int __cdecl wsprintfA(char *, const char *, ...);

#endif
