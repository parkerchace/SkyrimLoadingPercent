#include "PCH.h"
#include "FileIOHook.h"
#include "ProgressTracker.h"
#include <winternl.h>

namespace {

// Original function pointers saved by MinHook
HANDLE(WINAPI* orig_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = nullptr;
BOOL(WINAPI* orig_CloseHandle)(HANDLE) = nullptr;

// NtReadFile is the NT-layer read function that underlies both ReadFile and
// ReadFileEx. Hooking here intercepts overlapped I/O (where ReadFile's
// lpNumberOfBytesRead is NULL) and operates below ENB's kernel32 hooks.
typedef NTSTATUS(NTAPI* NtReadFileFn)(
    HANDLE           FileHandle,
    HANDLE           Event,
    PIO_APC_ROUTINE  ApcRoutine,
    PVOID            ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID            Buffer,
    ULONG            Length,
    PLARGE_INTEGER   ByteOffset,
    PULONG           Key);
static NtReadFileFn orig_NtReadFile = nullptr;

HANDLE WINAPI Hook_CreateFileW(
    LPCWSTR               lpFileName,
    DWORD                 dwDesiredAccess,
    DWORD                 dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecAttr,
    DWORD                 dwCreationDisposition,
    DWORD                 dwFlagsAndAttributes,
    HANDLE                hTemplateFile)
{
    HANDLE hFile = orig_CreateFileW(
        lpFileName, dwDesiredAccess, dwShareMode,
        lpSecAttr, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);

    if (hFile != INVALID_HANDLE_VALUE && lpFileName) {
        LARGE_INTEGER fileSize{};
        if (GetFileSizeEx(hFile, &fileSize)) {
            ProgressTracker::GetSingleton().OnFileOpened(hFile, lpFileName, fileSize.QuadPart);
        }
    }
    return hFile;
}

NTSTATUS NTAPI Hook_NtReadFile(
    HANDLE           FileHandle,
    HANDLE           Event,
    PIO_APC_ROUTINE  ApcRoutine,
    PVOID            ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID            Buffer,
    ULONG            Length,
    PLARGE_INTEGER   ByteOffset,
    PULONG           Key)
{
    // Count bytes REQUESTED before calling the real function so that overlapped and
    // async reads (STATUS_PENDING) are captured. Skyrim uses FILE_FLAG_OVERLAPPED
    // for BSA reads — those return STATUS_PENDING and IoStatusBlock->Information is
    // not set until the IOCP callback fires on another thread, which we never see.
    // Both the current-load numerator and the previous-load denominator are measured
    // the same way, so the progress ratio stays accurate.
    if (Length > 0)
        ProgressTracker::GetSingleton().OnBytesRead(FileHandle, Length);

    return orig_NtReadFile(FileHandle, Event, ApcRoutine, ApcContext,
                           IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

BOOL WINAPI Hook_CloseHandle(HANDLE hObject)
{
    ProgressTracker::GetSingleton().OnFileClosed(hObject);
    return orig_CloseHandle(hObject);
}

} // anonymous namespace

namespace FileIOHook {

bool Install() {
    if (MH_Initialize() != MH_OK) {
        logger::error("FileIOHook: MH_Initialize failed");
        return false;
    }

    auto createHook = [](LPCWSTR module, const char* modName, LPCSTR funcName, LPVOID hookFn, LPVOID* origFn) -> bool {
        MH_STATUS status = MH_CreateHookApiEx(module, funcName, hookFn, origFn, nullptr);
        if (status != MH_OK) {
            logger::error("FileIOHook: failed to hook {}::{} ({})", modName, funcName, static_cast<int>(status));
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= createHook(L"kernel32", "kernel32", "CreateFileW", &Hook_CreateFileW, reinterpret_cast<LPVOID*>(&orig_CreateFileW));
    ok &= createHook(L"ntdll",    "ntdll",    "NtReadFile",  &Hook_NtReadFile,  reinterpret_cast<LPVOID*>(&orig_NtReadFile));
    ok &= createHook(L"kernel32", "kernel32", "CloseHandle", &Hook_CloseHandle, reinterpret_cast<LPVOID*>(&orig_CloseHandle));

    if (!ok) return false;

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        logger::error("FileIOHook: MH_EnableHook failed");
        return false;
    }

    logger::info("FileIOHook: installed (CreateFileW, NtReadFile, CloseHandle)");
    return true;
}

} // namespace FileIOHook
