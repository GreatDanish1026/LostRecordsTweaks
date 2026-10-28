# LostRecordsTweaks 0.3.1

Graphics add-on for **Lost Records: Bloom & Rage** (PC, Steam), for ReShade with add-on support.
Raises the game's shadow, reflection, character and detail settings beyond what its menu
offers, live, from an in-game overlay.

- **Lighting**: higher-quality screen-space reflections, optionally on rough surfaces too.
  The game's lighting is baked: Lumen, screen-space bounce light and hardware ray tracing were
  all tried and don't work in this game (see `docs/FINDINGS.md`).
- **Shadows**: sharper shadows held steady (the game lowers shadow resolution on its own to save
  frame time), smoother soft shadows for lamps and the flashlight, full-resolution character
  soft shadows.
- **Characters**: full-resolution skin shading, higher-quality strand hair.
- **Detail**: texture memory beyond the game's fixed 2 GB, 16x anisotropic filtering, draw
  distance, finer geometry, finer volumetric fog, sharpening.
- **Post-processing**: switches for film grain, chromatic aberration, depth of field, lens flares
  and motion blur (the game's menu has none of the first three).
- **Quality presets**: *Ultra+* (about 9% slower) and *Cinematic* (about 40% slower), measured on
  an RTX 5080 at 4K with DLSS and frame generation. **Measure preset costs** and **Measure each
  option** check them on your PC.
- **F8** switches every tweak off and on, to compare.

DLSS, frame generation and FSR are in the game's own menu, so this add-on leaves upscaling alone.
The game ships DLSS 3.7; replacing `nvngx_dlss.dll` and `nvngx_dlssg.dll` (under
`Engine\Plugins\NvidiaDLSS`) with a 310.x release gives the newer model. Keep the game's
Streamline (`sl.*`) files.

## Requirements

- Lost Records: Bloom & Rage for PC (Steam build 19317136 tested; Unreal Engine 5.3). If the
  game's code doesn't match, the add-on changes nothing and says so in the overlay and in
  `LostRecordsTweaks.log`.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `Bloom&Rage.exe` with DirectX 10/11/12.

## Install

1. Install ReShade with add-on support into `...\Lost Records - Bloom & Rage\UNI\Binaries\Win64`.
2. Copy `LostRecordsTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under LostRecordsTweaks.

Settings are saved in `ReShade.ini` under `[LostRecordsTweaks]`. No game code is patched.
Deleting the `.addon64` file removes the mod.

## Advanced tab

- **Hardware ray tracing test**: kept for other builds of the game; on build 19317136 the game
  stops at start with ray tracing on (no ray tracing shaders shipped). The test takes the
  setting out again and keeps Engine.ini read-only during the test launch, because the game
  saves its config when it crashes.
- **Probe**: writes every lighting, shadow, upscaling and detail setting with the game's value to
  `LostRecordsTweaks-probe.txt`.
- **Console variable**: reads and sets a single setting, for experiments.

## Building

`.\build.ps1` builds `bin\LostRecordsTweaks.addon64` (needs Visual Studio 2022 with the C++
tools). `.\build.ps1 -Install` also copies it into the game folder (close the game first);
`.\build.ps1 -Package` makes the release zip in `bin\release`. See `docs/FINDINGS.md`.

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
