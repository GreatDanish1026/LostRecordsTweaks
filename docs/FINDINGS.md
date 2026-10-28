# Lost Records: Bloom & Rage: findings

Game: Lost Records: Bloom & Rage (Don't Nod), Steam app 1902960, build 19317136 (`UNI/ProjectBuild.version`:
`132116;r.bedard;Ep2_TU4.0`, i.e. Tape 2, title update 4). Unreal Engine **~5.3** (project `UNI`;
no version string in the exe, see below). DirectX 12 (Agility SDK `D3D12Core.dll` shipped, SM6
pipeline cache `UNI_PCD3D_SM6.upipelinecache`).

Paths:
- Game: `E:\SteamLibrary\steamapps\common\Lost Records - Bloom & Rage`
- Executable: `UNI\Binaries\Win64\Bloom&Rage.exe` (153 MB; the manifest calls it `UNI-Win64-Shipping.exe`)
- User config: `%LOCALAPPDATA%\Bloom&Rage\Saved\Config\Windows\` (`Engine.ini`, `GameUserSettings.ini`).
  No per-Steam-user folder. Shipping build: no engine log in `Saved\Logs` (only `NGX\`).
- Menu settings 2026-10-28: 3840x2160 fullscreen, every scalability group 3 (Epic), resolution
  quality 90, HDR on at 1000 nits, VSync off.

## Executable

Sections `.text .rodata .rdata .data .pdata .msvcjmc _RDATA .rsrc .reloc .bind`. `.bind` is the
SteamStub DRM wrapper, but `.text` isn't encrypted on disk (entropy ~6.4, normal code), so the
file can be read offline. Not checked whether patching code would trip anything; the add-on
doesn't patch.

Engine version, from which cvars exist (UTF-16 names in `.rdata`):
- has Substrate (`r.Substrate`), `r.Lumen.HardwareRayTracing.Inline`,
  `r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing`, `r.Lumen.TraceMeshSDFs`,
  `r.TSR.History.R11G11B10`
- lacks `r.Nanite.Tessellation` / `r.Nanite.AllowTessellation` (5.4), `r.MegaLights.Allow` (5.5),
  `r.TSR.ShadingRejection.ExposureOffset`, `r.TSR.Visualize`
- so 5.3 most likely.

## Upscalers and plugins

Ships NVIDIA's UE plugin (`Engine\Plugins\NvidiaDLSS`: `nvngx_dlss.dll`, Streamline with
`sl.dlss_g`, `sl.reflex`, `sl.pcl`, `sl.deepdvc`), AMD FSR3 (`amd_fidelityfx_dx12.dll`,
`Engine/Plugins/AMDFSR/FSR3`) and NIS (`r.NIS.*`). So DLSS and frame generation are native: no
separate DLSS add-on needed (unlike Double Exposure / Reunion).

## Console variable layout

Static scan of `Bloom&Rage.exe` (scratch script, 2026-10-28):
- `IConsoleManager::Singleton` at RVA `0x89e77d0` (8 of 9 `r.ScreenPercentage` lookups).
- Manager slot `+0xb8` FindConsoleVariable: 1699 calls.
- After Find: `+0x58` AsVariableInt (48), `+0x90` GetInt (16), `+0x98` GetFloat (4), `+0x88` GetBool (2).
- SetBy passed before a Set call: 0x09 (SetByCode) and 0x0A (SetByConsole), nothing higher.

Same as UE 5.2.1 (Double Exposure), so the add-on's cvars.hpp is unchanged apart from comments.
All 48 cvars the options use exist in this exe.

## Game-specific cvars

- `r.uni.VirtualShadowsResolutionTargetLodBias`, `r.uni.ShadowDepthsFrameTimeTargetFraction`: a
  frame-time-driven virtual shadow resolution. May rewrite `r.Shadow.Virtual.ResolutionLodBias*`;
  the add-on's Hold() would log "put back" if so.
- `r.uni.VirtualTextureKeepAliveEnable`, `r.uni.VirtualTextureKeepAliveMipLevel`
- `Uni.Rendering.*`: dynamic resolution / VT mip bias on camera cuts
  (`AllowLowerMaxDynamicResScaleOnCameraCut`, `AllowMipBiasOnCameraCut`, `MaxDynamicResScaleOnRequest`, ...).
- `r.DNELMDViewDistanceScale`, `r.DNMOverrideGlobalDFClipmapDistance`, `r.DNEReflectionCaptureLevelForDSG`,
  `r.DNERequireNanite`, `r.MotionBlur.DNEScaleWithTimeDilation`, `r.PostProcessing.DNEClusteredColorStrokes`.
- Halation post effect (`DNMHalation` plugin, `UNI.HalationOutput` pass name).

No menu option for film grain, depth of field or chromatic aberration (players edit Engine.ini);
the Post-processing tab covers them.

## Hardware ray tracing

`r.RayTracing`, `r.Lumen.HardwareRayTracing`, `r.RayTracing.Nanite.Mode` exist. Project defaults
are in the compressed pak, so unknown; whether RT shaders were cooked is untested. The Advanced
tab's self-reverting test (from DoubleExposureTweaks) checks it.

## Open

- First probe in game (values per menu tier), presets measured, RT test.
