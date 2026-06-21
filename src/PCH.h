#pragma once

// CommonLibSSE-NG must come first — it owns the Windows.h include and sets
// up WIN32 defines, COM, DirectX, and the REX/W32 wrappers correctly.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// STL
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// MinHook — Windows.h is already in scope from RE/Skyrim.h above
#include <MinHook.h>

// spdlog
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

// SimpleIni (header-only, installed by vcpkg into the include path)
#include <SimpleIni.h>

// Required for the "..."sv literals used by the generated SKSEPluginInfo file
using namespace std::string_view_literals;

namespace logger = SKSE::log;
