# Skyrim Loading Percent

An SKSE plugin that draws a live animation and a real-time load percentage on Skyrim Anniversary Edition and Skyrim VR loading screens. Progress climbs from the current load's real data — save-file bytes parsed against the save's exact file size, and cell references attached against the real reference count of the cells being loaded — never a fixed timer or a fake bar. The last stretch of a load (asset/navmesh initialization, after streaming has finished and there's no further data left to measure) is paced by a short, self-calibrating estimate of how long that stretch typically takes *on your own PC*, learned from your recent load times, so the bar keeps moving smoothly to 100 instead of stalling. It renders directly into the loading-screen Scaleform movie — so it does **not** hook Direct3D and stays out of the way of ENB, upscalers, and other overlays.

## Features

- 20 original hand-coded animations (Nordic Runes, Constellation, Standing Stone, Word Wall, Dwemer Cogs, Daedric Portal, and more)
- Live percentage readout driven by real measured progress (save-file parse + cell-reference attach, paced through the signal-less tail by a self-calibrating per-PC estimate), rendered in Skyrim's own UI font
- Fully configurable in-game via an **MCM** (SkyUI Mod Configuration Menu)
- Random-animation-each-load mode
- 6 position presets (four corners + top/bottom center) plus fine X/Y offset, independent animation and text size, opacity, and color
- Optional "hold at 100% until a key is pressed" with a pulsing "Press any key" prompt
- Configurable linger timer — stay at 100% for N seconds before the screen closes
- Flagged as a Light Master (ESL) — doesn't consume a full plugin slot in your load order

## Requirements

- **Skyrim Anniversary Edition 1.6.x – 1.7.104**, or **Skyrim VR**
- [SKSE64](https://skse.silverlock.org/) — 2.3.1 for the current 1.7.104 Steam build, or SKSEVR
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444), or [VR Address Library](https://www.nexusmods.com/skyrimspecialedition/mods/58101) for VR
- [SkyUI](https://www.nexusmods.com/skyrimspecialedition/mods/12604)
- [MCM Helper](https://www.nexusmods.com/skyrimspecialedition/mods/53000)

> **⚠️ Skyrim SE 1.5.97 is no longer supported.** From version 4.0.0 this plugin is built for current
> Skyrim only. On a 1.5.97 game the DLL refuses to install itself and writes a line saying so to its
> log, rather than loading with wrong memory offsets. **Version 3.1.1 is the last release that
> supports pre-Anniversary Edition**, and it stays available for that purpose.

> **⚠️ Match SKSE and MCM Helper to your game version.**
> SKSE and **MCM Helper ship separate downloads for each runtime** (AE vs VR, and different AE
> builds across game patches). On each mod's Files page, install the build that matches *your*
> Skyrim version. A mismatched MCM Helper makes the MCM fail to register — the **Loading Percent
> menu won't appear and your settings won't apply**, even though the DLL itself loads fine. If the
> menu is missing, that mismatch is the first thing to check. Your exact game version is shown in
> the lower-left of the main menu, and this plugin logs the runtime it detected to
> `Documents/My Games/Skyrim Special Edition/SKSE/SkyrimLoadingPercent.log`.

## Install

Install with a mod manager (Vortex / MO2), or copy the contents of the archive into your `Data` folder:

- `SkyrimLoadingPercent.esp` — registers the MCM (a start-game-enabled quest)
- `SKSE/Plugins/SkyrimLoadingPercent.dll`
- `SKSE/Plugins/SkyrimLoadingPercent.ini` — built-in default settings (the MCM overrides it at runtime)
- `Scripts/SkyrimLoadingPercentMCM.pex`
- `MCM/Config/SkyrimLoadingPercent/config.json` + `settings.ini`

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
| Other upscaling / frame-generation mods | Should be compatible, but please comment! |
| Loading-screen replacers (`LoadingMenu.swf`) | Compatible — this mod ships no SWF of its own; the overlay draws into whatever loading movie is active |
| No Grass In Objects (grass cache generation) | Compatible — the plugin detects an in-progress `PrecacheGrass` run and stays fully inactive (no hooks, no overlay) so it can't interfere |

## Building

Requires CMake 3.21+, a `VCPKG_ROOT` environment variable, and Visual Studio 2022 (Desktop C++). CommonLibSSE-NG is fetched automatically.

```
cmake --preset release
cmake --build --preset release
```

Output: `build/release/Release/SkyrimLoadingPercent.dll`.

The first configure is slow: it compiles CommonLibSSE-NG from source (~16 minutes) because the
project targets AE + VR only, and upstream publishes a prebuilt binary solely for the
all-three-runtimes combination. Later builds reuse it. Two options control this:

| Option | Default | Effect |
|--------|---------|--------|
| `SLP_SUPPORT_SKYRIM_VR` | `ON` | Target Skyrim VR in addition to AE |
| `SLP_SUPPORT_SKYRIM_SE` | `OFF` | Also target pre-Anniversary SE 1.5.97. Turning this **on** alongside VR makes the build an SE+AE+VR one, which picks up upstream's prebuilt binary and configures in seconds | The MCM Papyrus script is compiled separately with [Caprica](https://github.com/Orvid/Caprica) against the MCM-Helper SDK scripts.

## Libraries used

| Library | Author | License |
|---------|--------|---------|
| [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG) | alandtse, CharmedBaryon, Ryan-rsm-McKenzie and contributors | GPL-3.0-or-later WITH Modding Exception |
| [MCM Helper](https://github.com/Exit-9B/MCM-Helper) | Exit-9B | Apache-2.0 |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD 2-Clause |
| [spdlog](https://github.com/gabime/spdlog) | Gabi Melman | MIT |
| [CSimpleIni](https://github.com/brofield/simpleini) | Brodie Thiesfield | MIT |

## License

**GPL-3.0-or-later**, WITH a Modding Exception AND a GPL-3.0 Linking Exception — the same additional
permissions carried by CommonLibSSE-NG, which together allow this plugin to be linked and
distributed together with Skyrim and the SKSE and Windows platform libraries. See
[COPYING.txt](COPYING.txt) for the license and [EXCEPTIONS.md](EXCEPTIONS.md) for the exact terms of
the exceptions.

Releases up to and including 3.1.1 were MIT-licensed, and that grant still stands for those
versions. 4.0.0 changed the license because [CommonLibSSE-NG relicensed from MIT to
GPL-3.0-or-later on 2026-08-20](https://github.com/alandtse/CommonLibSSE-NG/blob/ng/EXCEPTIONS.md);
its Modding Exception covers linking against Skyrim, but deliberately does not cover plugin code, so
a plugin that statically links it forms a combined work and has to be GPL-compatible. There is no
MIT-licensed fork that supports current Skyrim — 1.7.99 and 1.7.104 both shipped after the
relicense — so staying on MIT would have meant staying on Skyrim 1.6.x forever.

## Credits

Created by Parker Chace.

Community contributions: Kreorporus (hold screen / linger timer suggestion), AleksandrShepard (animation feedback), legionnaire79 (top-center position suggestion), merowen (compatibility testing).

Built with assistance from [Claude Code](https://claude.ai/code) (Anthropic) — the animations, progress tracking, Scaleform rendering, and MCM integration were designed and written collaboratively in this project.
