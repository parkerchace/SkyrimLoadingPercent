# Skyrim Loading Percent

An SKSE plugin that draws a live animation and a real-time load percentage on Skyrim SE/AE loading screens. Progress is measured from the current load only — save-file bytes parsed against the save's exact file size, and cell references attached against the real reference count of the cells being loaded. No history, no learned estimates, no time-based guessing. It renders directly into the loading-screen Scaleform movie — so it does **not** hook Direct3D and stays out of the way of ENB, upscalers, and other overlays.

## Features

- 20 original hand-coded animations (Nordic Runes, Constellation, Standing Stone, Word Wall, Dwemer Cogs, Daedric Portal, and more)
- Live percentage readout driven by real measured progress (save-file parse + cell-reference attach), rendered in Skyrim's own UI font
- Fully configurable in-game via an **MCM** (SkyUI Mod Configuration Menu)
- Random-animation-each-load mode
- Position presets plus fine X/Y offset, independent animation and text size, opacity, and color
- Optional "hold at 100% until a key is pressed" with a pulsing "Press any key" prompt
- Configurable linger timer — stay at 100% for N seconds before the screen closes

## Requirements

- Skyrim Special Edition or Anniversary Edition
- [SKSE64](https://skse.silverlock.org/)
- [SkyUI](https://www.nexusmods.com/skyrimspecialedition/mods/12604)
- [MCM Helper](https://www.nexusmods.com/skyrimspecialedition/mods/53000)

## Install

Install with a mod manager (Vortex / MO2), or copy the contents of the archive into your `Data` folder:

- `SkyrimLoadingPercent.esp` — registers the MCM (a start-game-enabled quest)
- `SKSE/Plugins/SkyrimLoadingPercent.dll`
- `SKSE/Plugins/SkyrimLoadingPercent.ini` — built-in default settings (the MCM overrides it at runtime)
- `Scripts/SkyrimLoadingPercentMCM.pex`
- `MCM/Config/SkyrimLoadingPercent/config.json` + `settings.ini`
- `interface/LoadingMenu.swf` — renders the widget inside the loading movie. Optional: if it's omitted, or another loading-screen mod overrides it, the plugin falls back to drawing the overlay directly.

Enable the ESP in your load order (after `SkyUI_SE.esp` and `MCMHelper.esp`). Launch through SKSE.

## Configuration

Open the pause menu → **Mod Configuration** → **Loading Percent**. Changes are picked up on the next loading screen.

| Page | Settings |
|------|----------|
| Animation | Animation Style, Random Style, Include Non-Lore-Friendly Animations, Show Animation, Show Percentage |
| Appearance | Position, Offset X/Y, Animation Size, Text Size, Opacity, Color |
| Behavior | Hold at 100%, Linger Seconds, Prompt Position |

Advanced users can edit defaults in `Data/SKSE/Plugins/SkyrimLoadingPercent.ini` (the MCM overrides it at runtime).

## Compatibility

The overlay is drawn through Scaleform (GFx) into the loading-screen movie — there is **no Direct3D present hook** — so the D3D-related conflicts that affect many overlay mods do not apply here.

| Mod | Status |
|-----|--------|
| ENB (incl. ENB Frame Generation) | Compatible |
| Community Shaders (incl. its Upscaler) | Compatible |
| PureDark Upscaler (AIO & older per-game builds) | Compatible |
| Other upscaling / frame-generation mods | Compatible |
| Custom loading-screen SWFs | Supported — a SWF exposing `_root.progress_canvas` receives progress/config via ActionScript variables |

## Building

Requires CMake, a `VCPKG_ROOT` environment variable, and Visual Studio 2022 (Desktop C++). CommonLibSSE-NG is fetched automatically.

```
cmake --preset release
cmake --build --preset release
```

Output: `build/release/Release/SkyrimLoadingPercent.dll`. The MCM Papyrus script is compiled separately with [Caprica](https://github.com/Orvid/Caprica) against the MCM-Helper SDK scripts.

## Libraries used

| Library | Author | License |
|---------|--------|---------|
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | CharmedBaryon and contributors | MIT |
| [MCM Helper](https://github.com/Exit-9B/MCM-Helper) | Exit-9B | Apache-2.0 |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD 2-Clause |
| [CSimpleIni](https://github.com/brofield/simpleini) | Brodie Thiesfield | MIT |

## Credits

Created by Parker Chace.

Community contributions: Kreorporus (hold screen / linger timer suggestion), AleksandrShepard (animation feedback), legionnaire79 (top-center position suggestion), merowen (compatibility testing).

Built with assistance from [Claude Code](https://claude.ai/code) (Anthropic) — the animations, progress tracking, Scaleform rendering, and MCM integration were designed and written collaboratively in this project.
