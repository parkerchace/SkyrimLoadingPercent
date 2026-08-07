#include "PCH.h"
#include "config/Settings.h"
#include "hooks/FileIOHook.h"
#include "ui/ScaleformManager.h"
#include "ProgressTracker.h"

#include <filesystem>

namespace {

// No Grass In Objects (and its NG fork) generate the grass cache by launching the
// game with a PrecacheGrass.txt marker in the root folder and keeping ONE loading
// screen up for the entire multi-minute process. Every active thing this mod does
// would sabotage that: we hold the loading menu open, draw into it, and post our own
// kHide to close it once we think the load finished — which would tear down NGIO's
// persistent screen mid-generation — and our global NtReadFile hook adds overhead to
// the millions of reads generation performs. Detect the marker exactly the way NGIO
// does and install nothing, so grass caching runs untouched.
bool IsGrassGenerationMode() {
    std::error_code ec;
    if (std::filesystem::exists(L"PrecacheGrass.txt", ec)) return true;
    // Fall back to the executable's directory in case the working directory differs.
    wchar_t exePath[MAX_PATH];
    if (DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH); n > 0 && n < MAX_PATH) {
        std::filesystem::path p(exePath);
        p.replace_filename(L"PrecacheGrass.txt");
        if (std::filesystem::exists(p, ec)) return true;
    }
    return false;
}

void OnDataLoaded() {
    ScaleformManager::RegisterMenuSink();
    ScaleformManager::InstallThreadHook();
    ProgressTracker::GetSingleton().RegisterCellAttachSink();
}

void OnPostLoadGame(bool success) {
    if (success) {
        ProgressTracker::GetSingleton().OnLoadComplete();
        ScaleformManager::WaitForHoldRelease();
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
    logger::info("SkyrimLoadingPercent v3.1.0 loading");

    SKSE::Init(a_skse);

    if (IsGrassGenerationMode()) {
        logger::info("PrecacheGrass.txt detected — grass cache generation in progress; "
                     "staying inactive so No Grass In Objects can generate untouched.");
        return true;  // load cleanly but install no hooks or listeners
    }

    Settings::GetSingleton().Load();

    // FileIOHook calls MH_Initialize and creates the file hooks.
    if (!FileIOHook::Install()) {
        logger::error("Failed to install FileIO hooks — aborting");
        return false;
    }

    auto* msg = SKSE::GetMessagingInterface();
    if (!msg) {
        logger::error("Could not get MessagingInterface");
        return false;
    }
    msg->RegisterListener(MessageListener);

    logger::info("SkyrimLoadingPercent loaded successfully");
    return true;
}
