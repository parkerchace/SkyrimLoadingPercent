# Skyrim Loading Percent

An SKSE plugin that draws a live loading screen animation and percentage indicator on top of Skyrim SE/AE's loading screens.

## Features

- 20 original hand-coded animations (constellation, standing stone, aurora borealis, alchemy cauldron, word wall, and more)
- Live percentage readout that tracks real load progress via file I/O monitoring
- Fully configurable in-game: press `\` to open the settings menu — settings save automatically on close
- Optional "random animation each load" mode
- Position, colour, scale, and opacity controls
- Drag any corner of the settings menu to scale the entire UI — all text, controls, and the animation preview scale together
- Hold the loading screen until a key is pressed (shows a pulsing prompt)
- Configurable linger timer — stay at 100% for N seconds before the screen exits

## Install

1. Copy `SkyrimLoadingPercent.dll` and `SkyrimLoadingPercent.ini` into:
   `Data/SKSE/Plugins/`
2. Launch the game through SKSE as normal.

## Uninstall

Remove `SkyrimLoadingPercent.dll` and `SkyrimLoadingPercent.ini` from `Data/SKSE/Plugins/`. No other files are created.

## In-game controls

| Key | Action |
|-----|--------|
| `\` (default) | Open / close the settings menu |

The toggle key defaults to `\` (backslash). If that key is awkward on your keyboard layout, rebind it inside the menu or set `iMenuKey` in the INI to any of these numbers before launching:

| Key | Number | Key | Number |
|-----|--------|-----|--------|
| `\` (default) | 220 | F1  | 112 |
| `` ` `` / `~` | 192 | F2  | 113 |
| `-`            | 189 | F3  | 114 |
| `=`            | 187 | F4  | 115 |
| Tab            | 9   | F5  | 116 |
| Caps Lock      | 20  | F6  | 117 |
| Insert         | 45  | F7  | 118 |
| Delete         | 46  | F8  | 119 |
| Home           | 36  | F9  | 120 |
| End            | 35  | F10 | 121 |
| Page Up        | 33  | F11 | 122 |
| Page Down      | 34  | F12 | 123 |
| Numpad 0–9     | 96–105 | | |

For any key not listed: https://www.rbase.com/support/rsyntax/virtual_keycodes.html

Settings take effect immediately and are saved when you close the menu.

## Requirements

- Skyrim Special Edition or Anniversary Edition
- SKSE64

## Compatibility

| Mod | Status |
|-----|--------|
| ENB (without Frame Generation) | Compatible |
| ENB Frame Generation | Not compatible — ENB Frame Generation proxies D3D12 rather than D3D11, which this plugin hooks. The overlay will not appear. |
| Community Shaders | Compatible |
| Community Shaders Upscaling | Compatible |
| PureDark Upscaler | Compatible |
| Other SKSE overlays | Generally compatible; conflicts are possible if another plugin hooks `IDXGISwapChain::Present` at the same slot |

## Libraries used

| Library | Author | License |
|---------|--------|---------|
| [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) | CharmedBaryon and contributors | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD 2-Clause |
| [CSimpleIni](https://github.com/brofield/simpleini) | Brodie Thiesfield | MIT |

## Credits

Created by Parker Chace

Community contributions: Kreorporus (hold screen / linger timer suggestion), AleksandrShepard (animation feedback), legionnaire79 (top-center position suggestion), merowen (compatibility testing)

Built with assistance from [Claude Code](https://claude.ai/code) (Anthropic) — all 20 animations, the progress tracking system, and the configuration UI were designed and written collaboratively in this project.
