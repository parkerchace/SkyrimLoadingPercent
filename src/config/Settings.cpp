#include "PCH.h"
#include "Settings.h"

// Reads user-facing settings from an already-loaded INI, keeping current values as
// defaults so a layer only overrides the keys it actually defines. Enum indices are
// stored 0-based (MCM enum controls store the selected index directly).
static void ReadFrom(Settings& s, CSimpleIniA& ini) {
    s.animStyle      = std::clamp(static_cast<int>(ini.GetLongValue("General", "iAnimationStyle", s.animStyle)), 0, 19);
    s.randomStyle    = ini.GetBoolValue ("General", "bRandomStyle",    s.randomStyle);
    s.includeNonLoreAnims = ini.GetBoolValue ("General", "bIncludeNonLoreAnims", s.includeNonLoreAnims);
    s.position       = std::clamp(static_cast<int>(ini.GetLongValue("General", "iPosition",       s.position)),  0, 5);
    s.holdScreen     = ini.GetBoolValue ("General", "bHoldScreen",     s.holdScreen);
    s.lingerSeconds  = std::clamp(static_cast<int>(ini.GetLongValue("General", "iLingerSeconds",  s.lingerSeconds)), 0, 120);
    s.promptPosition = std::clamp(static_cast<int>(ini.GetLongValue("General", "iPromptPosition", s.promptPosition)), 0, 5);
    s.logLoadTimes   = ini.GetBoolValue ("General", "bLogLoadTimes",   s.logLoadTimes);

    s.showAnimation  = ini.GetBoolValue ("Display", "bShowAnimation",  s.showAnimation);
    s.showPercent    = ini.GetBoolValue ("Display", "bShowPercentage", s.showPercent);
    s.scale          = std::clamp(static_cast<float>(ini.GetDoubleValue("Display", "fScale",        s.scale)),     0.2f, 3.0f);
    s.textScale      = std::clamp(static_cast<float>(ini.GetDoubleValue("Display", "fTextScale",    s.textScale)), 0.3f, 3.0f);
    s.overlayAlpha   = std::clamp(static_cast<float>(ini.GetDoubleValue("Display", "fOverlayAlpha", s.overlayAlpha)), 0.1f, 1.0f);
    s.offsetX        = std::clamp(static_cast<int>(ini.GetLongValue("Display", "iOffsetX", s.offsetX)), -640, 640);
    s.offsetY        = std::clamp(static_cast<int>(ini.GetLongValue("Display", "iOffsetY", s.offsetY)), -360, 360);
    s.color          = static_cast<unsigned int>(ini.GetLongValue("Display", "iColor", static_cast<long>(s.color)));
}

void Settings::Load() {
    CSimpleIniA ini;
    ini.SetUnicode();
    if (ini.LoadFile(kIniPath) < SI_OK) {
        logger::warn("Could not load INI; using defaults.");
        return;
    }

    ReadFrom(*this, ini);

    // MCMHelper writes user overrides here; load second so the MCM wins.
    CSimpleIniA mcm;
    mcm.SetUnicode();
    if (mcm.LoadFile(kMcmIniPath) >= SI_OK) {
        ReadFrom(*this, mcm);
        logger::info("Settings: MCM overrides applied");
    }

    logger::info("Settings loaded: style={} pos={} showPct={} scale={:.2f}",
        animStyle, position, showPercent, scale);
}

void Settings::Save() {
    std::lock_guard lock(iniMutex);
    CSimpleIniA ini;
    ini.SetUnicode();
    ini.LoadFile(kIniPath);

    ini.SetLongValue  ("General", "iAnimationStyle",  animStyle);
    ini.SetBoolValue  ("General", "bRandomStyle",     randomStyle);
    ini.SetBoolValue  ("General", "bIncludeNonLoreAnims", includeNonLoreAnims);
    ini.SetLongValue  ("General", "iPosition",        position);
    ini.SetBoolValue  ("General", "bHoldScreen",      holdScreen);
    ini.SetLongValue  ("General", "iLingerSeconds",   lingerSeconds);
    ini.SetLongValue  ("General", "iPromptPosition",  promptPosition);
    ini.SetBoolValue  ("General", "bLogLoadTimes",    logLoadTimes);
    ini.SetBoolValue  ("Display", "bShowAnimation",   showAnimation);
    ini.SetBoolValue  ("Display", "bShowPercentage",  showPercent);
    ini.SetDoubleValue("Display", "fScale",           static_cast<double>(scale));
    ini.SetDoubleValue("Display", "fTextScale",       static_cast<double>(textScale));
    ini.SetDoubleValue("Display", "fOverlayAlpha",    static_cast<double>(overlayAlpha));
    ini.SetLongValue  ("Display", "iOffsetX",         offsetX);
    ini.SetLongValue  ("Display", "iOffsetY",         offsetY);
    ini.SetLongValue  ("Display", "iColor",           static_cast<long>(color));

    if (ini.SaveFile(kIniPath) < SI_OK) {
        logger::error("Settings::Save - failed to write INI");
    } else {
        logger::info("Settings saved");
    }
}
