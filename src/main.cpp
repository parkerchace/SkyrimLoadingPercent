#include "PCH.h"
#include "config/Settings.h"
#include "hooks/FileIOHook.h"
#include "ui/ScaleformManager.h"
#include "ui/D3DOverlay.h"
#include "ProgressTracker.h"

namespace {

void OnDataLoaded() {
    ScaleformManager::RegisterMenuSink();
    // D3DOverlay is installed from SKSEPluginLoad (before D3D exists).
    // Nothing D3D-related to do here.
}

void OnPostLoadGame(bool success) {
    if (success) {
        ProgressTracker::GetSingleton().OnLoadComplete();
    }
}

void MessageListener(SKSE::MessagingInterface::Message* a_msg) {
    switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            OnDataLoaded();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            OnPostLoadGame(static_cast<bool>(
                reinterpret_cast<std::uintptr_t>(a_msg->data)));
            break;
        case SKSE::MessagingInterface::kNewGame:
            ProgressTracker::GetSingleton().OnLoadComplete();
            break;
        default:
            break;
    }
}

void SetupLog() {
    auto logsPath = SKSE::log::log_directory();
    if (!logsPath) {
        SKSE::stl::report_and_fail("Could not find SKSE log directory");
    }
    auto logPath  = *logsPath / "SkyrimLoadingPercent.log";
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        logPath.string(), true);
    auto log = std::make_shared<spdlog::logger>("global", fileSink);
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(log);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

} // anonymous namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    logger::info("SkyrimLoadingPercent v1.0.0 loading");

    SKSE::Init(a_skse);
    Settings::GetSingleton().Load();

    // Prime the byte-denominator from the last saved load so the first load
    // of this session uses a real estimate instead of the 20 MB fallback.
    if (auto saved = Settings::GetSingleton().lastStreamCount; saved > 0)
        ProgressTracker::GetSingleton().SeedEstimate(static_cast<LONGLONG>(saved));

    // FileIOHook calls MH_Initialize and creates the file hooks.
    if (!FileIOHook::Install()) {
        logger::error("Failed to install FileIO hooks — aborting");
        return false;
    }

    // D3DOverlay hooks D3D11CreateDeviceAndSwapChain so it can intercept the
    // game's own device+swapchain creation (which happens a few seconds later,
    // before kDataLoaded).  Must be installed HERE — by kDataLoaded it's too late.
    D3DOverlay::Install();  // non-fatal; game still runs without overlay

    auto* msg = SKSE::GetMessagingInterface();
    if (!msg) {
        logger::error("Could not get MessagingInterface");
        return false;
    }
    msg->RegisterListener(MessageListener);

    logger::info("SkyrimLoadingPercent loaded successfully");
    return true;
}
