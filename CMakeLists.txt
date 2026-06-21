cmake_minimum_required(VERSION 3.21)

project(
    SkyrimLoadingPercent
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed.")
endif()

# ── Fetch CommonLibSSE-NG ─────────────────────────────────────────────────────
include(FetchContent)

set(BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        b93280e832f263dbef44e44cbe2936622a02f91a
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(CommonLibSSE)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")

# ── vcpkg-provided dependencies ───────────────────────────────────────────────
find_package(minhook CONFIG REQUIRED)
find_package(spdlog  CONFIG REQUIRED)
find_package(imgui   CONFIG REQUIRED)

# ── Plugin target ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/ProgressTracker.cpp
    src/hooks/FileIOHook.cpp
    src/ui/ScaleformManager.cpp
    src/ui/D3DOverlay.cpp
    src/config/Settings.cpp
)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES ${SOURCES}
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
        imgui::imgui
        d3d11
        dxgi
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
set(SKYRIM_PATH "" CACHE PATH "Path to Skyrim SE/AE Data folder (optional)")

if(SKYRIM_PATH)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
            "${SKYRIM_PATH}/SKSE/Plugins/${PROJECT_NAME}.dll"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/SKSE/Plugins/SkyrimLoadingPercent.ini"
            "${SKYRIM_PATH}/SKSE/Plugins/SkyrimLoadingPercent.ini"
        VERBATIM
    )
endif()
