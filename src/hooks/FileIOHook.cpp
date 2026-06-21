#include "PCH.h"
#include "FileIOHook.h"
#include "ProgressTracker.h"

namespace {

// Original function pointers saved by MinHook
HANDLE(WINAPI* orig_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) = nullptr;
BOOL(WINAPI* orig_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) = nullptr;
BOOL(WINAPI* orig_CloseHandle)(HANDLE) = nullptr;

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

BOOL WINAPI Hook_ReadFile(
    HANDLE       hFile,
    LPVOID       lpBuffer,
    DWORD        nNumberOfBytesToRead,
    LPDWORD      lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped)
{
    BOOL result = orig_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    if (result && lpNumberOfBytesRead && *lpNumberOfBytesRead > 0) {
        ProgressTracker::GetSingleton().OnBytesRead(hFile, *lpNumberOfBytesRead);
    }
    return result;
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

    auto createHook = [](LPCWSTR module, LPCSTR funcName, LPVOID hookFn, LPVOID* origFn) -> bool {
        MH_STATUS status = MH_CreateHookApiEx(module, funcName, hookFn, origFn, nullptr);
        if (status != MH_OK) {
            logger::error("FileIOHook: failed to hook {}::{} ({})", "kernel32", funcName, static_cast<int>(status));
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= createHook(L"kernel32", "CreateFileW",  &Hook_CreateFileW,  reinterpret_cast<LPVOID*>(&orig_CreateFileW));
    ok &= createHook(L"kernel32", "ReadFile",      &Hook_ReadFile,     reinterpret_cast<LPVOID*>(&orig_ReadFile));
    ok &= createHook(L"kernel32", "CloseHandle",   &Hook_CloseHandle,  reinterpret_cast<LPVOID*>(&orig_CloseHandle));

    if (!ok) return false;

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        logger::error("FileIOHook: MH_EnableHook failed");
        return false;
    }

    logger::info("FileIOHook: installed (CreateFileW, ReadFile, CloseHandle)");
    return true;
}

void Uninstall() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

} // namespace FileIOHook
