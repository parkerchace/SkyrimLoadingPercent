#pragma once
#include <atomic>

namespace ScaleformManager {
    extern std::atomic<bool> g_loadMenuCurrentlyOpen;

    void RegisterMenuSink();
    void InstallThreadHook();
    void ReleaseLoadingMenuHold();
}
