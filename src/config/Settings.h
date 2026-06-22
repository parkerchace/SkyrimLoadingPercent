#pragma once

class Settings {
public:
    static Settings& GetSingleton() noexcept {
        static Settings instance;
        return instance;
    }

    void Load();
    void Save();
    void SaveCache();   // persists byte-calibration data without touching user-visible keys

    // Which animation to display (0-19)
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

    // Bytes read during the last completed load — used as denominator for GetProgress().
    // Persisted so the first load of each session has a real estimate.
    uint64_t lastStreamCount{ 0 };

    // Virtual key code for the menu toggle key. Default 220 = VK_OEM_5 = backslash on US layouts.
    int menuKey{ 220 };

    static constexpr auto kIniPath = L"Data/SKSE/Plugins/SkyrimLoadingPercent.ini";

private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
