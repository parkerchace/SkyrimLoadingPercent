# Skyrim Loading Percent

An SKSE plugin that draws a live loading screen animation and percentage indicator on top of Skyrim SE/AE's loading screens.

## Features

- 20 original hand-coded animations (constellation, standing stone, aurora borealis, alchemy cauldron, word wall, and more)
- Live percentage readout that tracks real load progress via file I/O monitoring
- Fully configurable in-game: press `\` to open the settings menu — settings save automatically on close
- Optional "random animation each load" mode
- Position, color, scale, and opacity controls

## Install

1. Copy `SkyrimLoadingPercent.dll` and `SkyrimLoadingPercent.ini` into:
   `Data/SKSE/Plugins/`
2. Launch the game through SKSE as normal.

## Uninstall

Remove `SkyrimLoadingPercent.dll` and `SkyrimLoadingPercent.ini` from `Data/SKSE/Plugins/`. No other files are created.

## In-game controls

| Key | Action |
|-----|--------|
| `\` | Open / close the settings menu |

Settings take effect immediately and are saved automatically when you close the menu.

## Requirements

- Skyrim Special Edition or Anniversary Edition
- SKSE64

## Compatibility

Largely untested. This plugin hooks `IDXGISwapChain::Present` to draw the overlay and `ReadFile`/`CreateFileW` to track load progress — the same techniques used by many SKSE overlays and ENBs. Conflicts are possible with other plugins that hook the same entry points.

Your antivirus may flag this DLL as suspicious. This is a false positive — SKSE plugins routinely hook Windows and DirectX APIs as a core part of how they work. The source code is available on [GitHub](https://github.com/parkerchace/SkyrimLoadingPercent).

## Libraries used

| Library | Author | License |
|---------|--------|---------|
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | CharmedBaryon and contributors | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD 2-Clause |
| [CSimpleIni](https://github.com/brofield/simpleini) | Brodie Thiesfield | MIT |

## Credits

Created by Parker Chace

Built with assistance from [Claude Code](https://claude.ai/code) (Anthropic) — all 20 animations, the progress tracking system, and the configuration UI were designed and written collaboratively in this project.
