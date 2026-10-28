# LostRecordsTweaks 0.1.0

Graphics add-on for **Lost Records: Bloom & Rage** (PC, Steam), for ReShade with add-on support.
Raises the game's lighting, shadow, character and detail settings beyond what its menu offers,
live, from an in-game overlay. Work in progress: options and presets come from DoubleExposureTweaks
and aren't measured in this game yet.

- **Lighting** (Lumen): detailed bounce light tracing, more lighting probes, reflections on
  rougher surfaces and on glass and water.
- **Shadows**: sharper shadows, smoother soft shadows, longer shadow distance, full-resolution
  character soft shadows.
- **Characters**: full-resolution skin shading, higher-quality strand hair.
- **Detail**: texture memory, 16x anisotropic filtering, draw distance, finer geometry, finer
  volumetric fog, higher-precision TSR history.
- **Post-processing**: switches for film grain, lens flares, depth of field, motion blur and
  chromatic aberration (the game's menu has none of these).
- **Quality presets**: *Ultra+* and *Cinematic*, with **Measure preset costs** / **Measure each
  option** to see what they cost on your PC.
- **F8** switches every tweak off and on, to compare.

DLSS, frame generation and FSR are in the game's own menu, so this add-on leaves upscaling alone.

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

Settings are saved in `ReShade.ini` under `[LostRecordsTweaks]`. No game code is patched. The ray
tracing test writes `r.RayTracing=1` to your `Engine.ini` for one launch when you start it and
takes it out again (even if that launch fails). Deleting the `.addon64` file removes the mod.

## Advanced tab

- **Hardware ray tracing test**: writes the setting for the next launch, then reports whether
  the game started with ray tracing on.
- **Probe**: writes every lighting, shadow, upscaling and detail setting with the game's value to
  `LostRecordsTweaks-probe.txt`.
- **Console variable**: reads and sets a single setting, for experiments.

## Building

`.\build.ps1` builds `bin\LostRecordsTweaks.addon64` (needs Visual Studio 2022 with the C++
tools). `.\build.ps1 -Install` also copies it into the game folder (close the game first);
`.\build.ps1 -Package` makes the release zip in `bin\release`. See `docs/FINDINGS.md`.

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
