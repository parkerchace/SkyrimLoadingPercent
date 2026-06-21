# Skyrim Loading Percent

An SKSE plugin that draws a live loading screen animation and percentage indicator on top of Skyrim SE/AE's loading screens.

## Features

- 20 original hand-coded animations (constellation, standing stone, aurora borealis, alchemy cauldron, word wall, and more)
- Live percentage readout that tracks real load progress
- Fully configurable in-game: press `\` to open the settings menu
- Optional "random animation each load" mode
- Position, color, scale, and opacity controls

## Install

1. Copy `SkyrimLoadingPercent.dll` and `SkyrimLoadingPercent.ini` into:
   `Data/SKSE/Plugins/`
2. Launch the game through SKSE as normal.

## In-game controls

| Key | Action |
|-----|--------|
| `\` | Open / close the settings menu |

All settings are saved automatically when you close the menu.

## Requirements

- Skyrim Special Edition or Anniversary Edition
- SKSE64

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
