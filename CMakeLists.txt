# SPDX-FileCopyrightText: 2026 Parker Chace
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional permissions under GPL-3.0 section 7 apply; see EXCEPTIONS.md.

cmake_minimum_required(VERSION 3.21)

project(
    SkyrimLoadingPercent
    VERSION 4.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed.")
endif()

# ── Runtime targeting ─────────────────────────────────────────────────────────
# One DLL covers every runtime enabled here; CommonLibSSE-NG resolves the struct
# layouts at load time and addresses come from Address Library, so no separate
# per-runtime build is needed.
#
# Pre-AE Skyrim SE (1.5.97) is off: this plugin targets current Skyrim. Note that
# turning it back on is not purely additive — CommonLibSSE-NG only publishes a
# prebuilt binary for the all-three-runtimes combination, so an SE+AE+VR build
# downloads that (seconds) while any other combination compiles the library from
# source (~16 minutes, once, then cached in the build tree).
option(SLP_SUPPORT_SKYRIM_SE "Also target pre-Anniversary Skyrim SE 1.5.97." OFF)
option(SLP_SUPPORT_SKYRIM_VR "Also target Skyrim VR." ON)

# ── Fetch CommonLibSSE-NG ─────────────────────────────────────────────────────
# alandtse/CommonLibSSE-NG is the maintained upstream. CharmedBaryon's tree (the
# previous dependency) has not been touched since September 2024: it predates
# Skyrim 1.7.x, misclassifies AE 1.7.x runtimes as SE because the minor-version
# check is an equality test, and cannot read Address Library format 5. There is
# no MIT-licensed fork with 1.7.x support — CommonLibSSE-NG relicensed to
# GPL-3.0-or-later on 2026-08-20 and both 1.7.99 and 1.7.104 shipped after that
# date. That relicense is why this plugin is GPL-3.0-or-later; see COPYING.txt.
include(FetchContent)

set(BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK   OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE     ${SLP_SUPPORT_SKYRIM_SE} CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE     ON  CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR     ${SLP_SUPPORT_SKYRIM_VR} CACHE BOOL "" FORCE)

FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/alandtse/CommonLibSSE-NG.git
    GIT_TAG        v8.4.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

# ── vcpkg-provided dependencies ───────────────────────────────────────────────
find_package(minhook CONFIG REQUIRED)
find_package(spdlog  CONFIG REQUIRED)

# ── Plugin target ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/ProgressTracker.cpp
    src/hooks/FileIOHook.cpp
    src/ui/ScaleformManager.cpp
    src/config/Settings.cpp
)

# USE_ADDRESS_LIBRARY (the default) declares version independence to SKSE, so the
# plugin loads on any runtime in the set above rather than a pinned version list.
# StructCompatibility::Independent additionally sets the Address Library v5 flag
# that AE 1.7.99+ requires.
add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES ${SOURCES}
    AUTHOR  "Parker Chace"
    USE_ADDRESS_LIBRARY
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        minhook::minhook
        spdlog::spdlog
        xinput.lib
)

target_compile_options(${PROJECT_NAME}
    PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /external:anglebrackets
        /external:W0
        /wd4200
        /wd4505  # unreferenced local function (animation helpers)
)

# ── Optional: auto-deploy to Skyrim Data folder ───────────────────────────────
# Set -DSKYRIM_PATH=..., or export SKYRIM_DATA_PATH once and every build tree
# deploys automatically. The env var keeps machine-specific paths out of the
# repo, so a fresh build directory still deploys without re-passing the flag.
set(SKYRIM_PATH "" CACHE PATH "Path to Skyrim AE/VR Data folder (optional)")

if(NOT SKYRIM_PATH AND DEFINED ENV{SKYRIM_DATA_PATH})
    set(SKYRIM_PATH "$ENV{SKYRIM_DATA_PATH}" CACHE PATH "" FORCE)
    message(STATUS "Auto-deploy enabled from SKYRIM_DATA_PATH: ${SKYRIM_PATH}")
endif()

# Fail at configure time rather than mid-build. This matters when deploying to a
# Vortex staging folder: its name embeds the mod version and a Vortex hash, so
# reinstalling the mod renames it and the stale path would otherwise surface as
# a confusing "cmake -E copy" failure in the POST_BUILD step.
if(SKYRIM_PATH AND NOT IS_DIRECTORY "${SKYRIM_PATH}")
    message(FATAL_ERROR
        "SKYRIM_PATH does not exist:
  ${SKYRIM_PATH}
"
        "If this is a Vortex staging folder, reinstalling the mod renames it. "
        "Update the SKYRIM_DATA_PATH environment variable (or pass -DSKYRIM_PATH=...) "
        "to the current folder name.")
endif()

if(SKYRIM_PATH)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        # DLL — always overwrite (it's the build output)
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
            "${SKYRIM_PATH}/SKSE/Plugins/${PROJECT_NAME}.dll"
        # INI — only deploy on first install; never overwrite user changes
        # The quotes wrap each whole -D argument. Putting them around only the
        # value (-DSRC="${...}") makes VERBATIM escape them into the value
        # itself, so the path arrives with literal quote characters, never
        # matches EXISTS, and COPY_FILE then fails on the quoted name.
        COMMAND ${CMAKE_COMMAND}
            "-DSRC=${CMAKE_SOURCE_DIR}/SKSE/Plugins/SkyrimLoadingPercent.ini"
            "-DDST=${SKYRIM_PATH}/SKSE/Plugins/SkyrimLoadingPercent.ini"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake"
        # ESP — always overwrite (structure never changes)
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/SkyrimLoadingPercent.esp"
            "${SKYRIM_PATH}/SkyrimLoadingPercent.esp"
        # MCMHelper config — always overwrite (structure/content may improve across builds)
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${SKYRIM_PATH}/MCM/Config/SkyrimLoadingPercent"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/MCM/Config/SkyrimLoadingPercent/config.json"
            "${SKYRIM_PATH}/MCM/Config/SkyrimLoadingPercent/config.json"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/MCM/Config/SkyrimLoadingPercent/settings.ini"
            "${SKYRIM_PATH}/MCM/Config/SkyrimLoadingPercent/settings.ini"
        VERBATIM
    )
endif()
