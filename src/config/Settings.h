#pragma once
#include <unordered_map>

class Settings {
public:
    static Settings& GetSingleton() noexcept {
        static Settings instance;
        return instance;
    }

    void Load();
    void Save();
    void SaveCache();   // persists calibration data without touching user-visible keys

    // Which animation to display (0–19)
    int  animStyle{ 1 };
    bool randomStyle{ false };  // pick a random style on each load

    // Where to draw the widget
    // 0=Bottom-Right  1=Bottom-Left  2=Bottom-Center  3=Top-Right  4=Top-Left
    int position{ 1 };

    // Show numeric percentage text
    bool showPercent{ true };

    // Scale factor for the animation widget
    float scale{ 0.6f };

    // RGB color for the primary highlight (default: aged parchment/stone — vanilla Skyrim UI)
    unsigned int color{ 0xFFD4D0BE };

    // Widget opacity: 1.0 = opaque, lower lets Skyrim's art show through
    float overlayAlpha{ 1.0f };

    float    lastLoadDuration{ 2.0f };  // seconds; fallback when no per-location data
    uint64_t lastStreamCount{ 0 };      // global fallback stream count

    // Per-destination calibration: BGSLocation FormID → stream count for that load.
    // Populated after each load completes.  First visit uses the global fallback.
    std::unordered_map<uint32_t, uint64_t> locationCalib;

    static constexpr auto kIniPath = L"Data/SKSE/Plugins/SkyrimLoadingPercent.ini";

private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
