// SPDX-FileCopyrightText: 2026 Parker Chace
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional permissions under GPL-3.0 section 7 apply; see EXCEPTIONS.md.

#pragma once
#include <atomic>

namespace ScaleformManager {
    extern std::atomic<bool> g_loadMenuCurrentlyOpen;

    void RegisterMenuSink();
    void InstallThreadHook();
    void WaitForHoldRelease();
    void ReleaseLoadingMenuHold();
}
