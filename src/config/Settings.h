#pragma once
#include <mutex>

class Settings {
public:
    static Settings& GetSingleton() noexcept {
        static Settings instance;
        return instance;
    }

    void Load();
    void Save();

    // Which animation to display (0-19)
    int  animStyle{ 0 };
    bool randomStyle{ false };  // pick a random style on each load
    // Waveform and Helix Spiral aren't lore-friendly; excluded from the random
    // pool unless this is on. Has no effect on a manually-picked animStyle.
    bool includeNonLoreAnims{ false };

    // Where to draw the widget
    // 0=Bottom-Right  1=Bottom-Left  2=Bottom-Center  3=Top-Right  4=Top-Left
    int position{ 1 };

    // Show the animation widget at all (false = percentage tracking still works,
    // but nothing is drawn — only the "Press any key" prompt shows if holdScreen is on)
    bool showAnimation{ true };

    // Show numeric percentage text
    bool showPercent{ true };

    // Size of the animation widget (1.0 ≈ designed for 1080p)
    float scale{ 0.6f };

    // Size multiplier for the percentage and prompt text (independent of scale)
    float textScale{ 0.45f };

    // Fine position nudge applied on top of the position preset, in 1280×720 stage units
    int offsetX{ 0 };
    int offsetY{ 0 };

    // RGB color for the primary highlight (default: aged parchment/stone — vanilla Skyrim UI)
    unsigned int color{ 0xFFD4D0BE };

    // Widget opacity: 1.0 = opaque, lower lets Skyrim's art show through
    float overlayAlpha{ 1.0f };

    // Hold the overlay on screen after 100% until the player presses a key
    bool holdScreen{ false };

    // Extra seconds to stay at 100% before the overlay exits (0 = off)
    int lingerSeconds{ 0 };

    // Where to draw "Press any key to continue" (0-4 same as position; 5 = center)
    int promptPosition{ 5 };

    // Write each load's duration to SkyrimLoadingPercent_times.log
    bool logLoadTimes{ false };

    static constexpr auto kIniPath    = L"Data/SKSE/Plugins/SkyrimLoadingPercent.ini";
    // MCMHelper writes user overrides here; we read it after kIniPath so MCM takes priority.
    static constexpr auto kMcmIniPath = L"Data/MCM/Settings/SkyrimLoadingPercent.ini";

    std::mutex iniMutex;  // serialises Save() across threads

private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
