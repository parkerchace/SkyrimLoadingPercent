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

    animStyle        = std::clamp(animStyle,    0, 19);
    position         = std::clamp(position,    0, 4);
    scale            = std::clamp(scale,        0.2f, 3.0f);
    overlayAlpha     = std::clamp(overlayAlpha, 0.1f, 1.0f);
    lastLoadDuration = std::clamp(
        static_cast<float>(ini.GetDoubleValue("Cache", "fLastLoadDuration", lastLoadDuration)),
        0.5f, 120.0f);

    if (const char* s = ini.GetValue("Cache", "uLastStreamCount", nullptr)) {
        try { lastStreamCount = std::stoull(s); } catch (...) {}
    }

    // Load per-location calibration map
    CSimpleIniA::TNamesDepend keys;
    if (ini.GetAllKeys("LocationCalibration", keys)) {
        for (auto& key : keys) {
            std::string_view sv{ key.pItem };
            if (sv.size() > 6 && sv.substr(0, 6) == "loc_0x") {
                try {
                    uint32_t formID = static_cast<uint32_t>(
                        std::stoul(std::string(sv.substr(6)), nullptr, 16));
                    if (const char* v = ini.GetValue("LocationCalibration", key.pItem))
                        locationCalib[formID] = std::stoull(v);
                } catch (...) {}
            }
        }
    }

    logger::info("Settings loaded: style={} pos={} showPct={} scale={:.2f} dur={:.2f}s "
                 "streams={} locations={}",
        animStyle, position, showPercent, scale, lastLoadDuration,
        lastStreamCount, locationCalib.size());
}

void Settings::Save() {
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    ini.SetLongValue  ("General", "iAnimationStyle",  animStyle);
    ini.SetBoolValue  ("General", "bRandomStyle",     randomStyle);
    ini.SetLongValue  ("General", "iPosition",        position);
    ini.SetBoolValue  ("Display", "bShowPercentage",  showPercent);
    ini.SetDoubleValue("Display", "fScale",           static_cast<double>(scale));
    ini.SetDoubleValue("Display", "fOverlayAlpha",    static_cast<double>(overlayAlpha));

    char colorBuf[16];
    snprintf(colorBuf, sizeof(colorBuf), "%08X", color);
    ini.SetValue("Display", "sColor", colorBuf);

    if (ini.SaveFile(kIniPath) < SI_OK) {
        logger::error("Settings::Save — failed to write INI");
    } else {
        logger::info("Settings saved");
    }
}

void Settings::SaveCache() {
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    ini.SetDoubleValue("Cache", "fLastLoadDuration", static_cast<double>(lastLoadDuration));
    char tickBuf[32];
    snprintf(tickBuf, sizeof(tickBuf), "%llu", static_cast<unsigned long long>(lastStreamCount));
    ini.SetValue("Cache", "uLastStreamCount", tickBuf);

    // Write per-location calibration
    ini.Delete("LocationCalibration", nullptr);  // clear stale keys before rewriting
    for (auto& [formID, count] : locationCalib) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "loc_0x%08X", formID);
        snprintf(val, sizeof(val), "%llu", static_cast<unsigned long long>(count));
        ini.SetValue("LocationCalibration", key, val);
    }

    ini.SaveFile(kIniPath);
}
