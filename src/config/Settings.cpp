#include "PCH.h"
#include "Settings.h"

void Settings::Load() {
    CSimpleIniA ini;
    ini.SetUnicode();
    if (ini.LoadFile(kIniPath) < SI_OK) {
        logger::warn("Could not load INI; using defaults.");
        return;
    }

    animStyle    = static_cast<int>(ini.GetLongValue("General", "iAnimationStyle", animStyle));
    randomStyle  = ini.GetBoolValue("General", "bRandomStyle", randomStyle);
    position     = static_cast<int>(ini.GetLongValue("General", "iPosition",       position));
    showPercent  = ini.GetBoolValue("Display", "bShowPercentage", showPercent);
    scale        = static_cast<float>(ini.GetDoubleValue("Display", "fScale",        scale));
    overlayAlpha = static_cast<float>(ini.GetDoubleValue("Display", "fOverlayAlpha", overlayAlpha));

    const char* colorStr = ini.GetValue("Display", "sColor", nullptr);
    if (colorStr) {
        try {
            color = static_cast<unsigned int>(std::stoul(colorStr, nullptr, 16));
        } catch (...) {
            logger::warn("Invalid sColor '{}'; using default.", colorStr);
        }
    }

    animStyle    = std::clamp(animStyle,    0, 19);
    position     = std::clamp(position,     0, 4);
    scale        = std::clamp(scale,        0.2f, 3.0f);
    overlayAlpha = std::clamp(overlayAlpha, 0.1f, 1.0f);

    if (const char* s = ini.GetValue("Cache", "uLastStreamCount", nullptr)) {
        try { lastStreamCount = std::stoull(s); } catch (...) {}
    }

    menuKey = std::clamp(
        static_cast<int>(ini.GetLongValue("General", "iMenuKey", menuKey)),
        1, 254);

    holdScreen     = ini.GetBoolValue ("General", "bHoldScreen",     holdScreen);
    lingerSeconds  = std::clamp(
        static_cast<int>(ini.GetLongValue("General", "iLingerSeconds", lingerSeconds)),
        0, 120);
    promptPosition = std::clamp(
        static_cast<int>(ini.GetLongValue("General", "iPromptPosition", promptPosition)),
        0, 5);

    logger::info("Settings loaded: style={} pos={} showPct={} scale={:.2f} streams={}",
        animStyle, position, showPercent, scale, lastStreamCount);
}

void Settings::Save() {
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    ini.SetLongValue  ("General", "iAnimationStyle",  animStyle);
    ini.SetBoolValue  ("General", "bRandomStyle",     randomStyle);
    ini.SetLongValue  ("General", "iPosition",        position);
    ini.SetLongValue  ("General", "iMenuKey",         menuKey);
    ini.SetBoolValue  ("General", "bHoldScreen",      holdScreen);
    ini.SetLongValue  ("General", "iLingerSeconds",   lingerSeconds);
    ini.SetLongValue  ("General", "iPromptPosition",  promptPosition);
    ini.SetBoolValue  ("Display", "bShowPercentage",  showPercent);
    ini.SetDoubleValue("Display", "fScale",           static_cast<double>(scale));
    ini.SetDoubleValue("Display", "fOverlayAlpha",    static_cast<double>(overlayAlpha));

    char colorBuf[16];
    snprintf(colorBuf, sizeof(colorBuf), "%08X", color);
    ini.SetValue("Display", "sColor", colorBuf);

    if (ini.SaveFile(kIniPath) < SI_OK) {
        logger::error("Settings::Save - failed to write INI");
    } else {
        logger::info("Settings saved");
    }
}

void Settings::SaveCache() {
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    char tickBuf[32];
    snprintf(tickBuf, sizeof(tickBuf), "%llu", static_cast<unsigned long long>(lastStreamCount));
    ini.SetValue("Cache", "uLastStreamCount", tickBuf);

    ini.SaveFile(kIniPath);
}
