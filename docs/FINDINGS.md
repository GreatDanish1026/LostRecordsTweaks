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

## First probe (0.1.0, 2026-10-28, menu all Epic, 4K, DLSS on, HDR 1000)

Connected at +7 s; layout check passed (IsVariableBool +0x30, data +0x48, sinks flag +0xe0);
1517 values. ReShade 6.8 loads fine alongside Streamline.

- **No Lumen**: `r.DynamicGlobalIlluminationMethod 0`, `r.ReflectionMethod 2` (SSR), both set by
  project (0x03). Lighting is baked (DNE `VLM`/`LMD` cvars). `r.Lumen.HardwareRayTracing 1` (0x03)
  but unused. Distance fields on (`r.DistanceFieldAO 1`, mesh SDFs exist), so software Lumen might
  work live; untested.
- SSR quality 3, `r.SSR.MaxRoughness -1` (scene's limit). SSGI cvars exist (quality 3) but GI
  method 0, so SSGI is off.
- Shadows (VSM): `ResolutionLodBias*` -0.578 and `MaxPhysicalPages` 3200, all set by code (0x09):
  the game's own system (`r.uni.VirtualShadowsResolutionTargetLodBias -1`,
  `r.uni.ShadowDepthsFrameTimeTargetFraction 0.4`) backs off from -1. SMRT directional 8x4, local
  5x3. `r.Shadow.DistanceScale 2.5` already. Contact shadows on, capsule shadows half res.
- `r.SSS.Checkerboard 2`, hair 4 spp / 2048 deep shadow, pool 2000 MB, aniso 8, view distance 1,
  Nanite 1 px, fog 8 px / 128, `r.Tonemapper.Sharpen 0`.
- Upscaling: `r.NGX.DLSS.Enable 1` (0x08), `r.DynamicRes.OperationMode 2` with min = max 66.7%
  (code: DLSS Quality), `r.ScreenPercentage 90`, `r.AntiAliasingMethod 4`. DLSS preset 0,
  sharpness 0. `nvngx_dlss.dll` and `nvngx_dlssg.dll` are **3.7.10** (CNN model). FSR3 off.
- Post: film grain 1, fringe 1, DOF 4, lens flares 2, motion blur quality 4 but
  `r.MotionBlur.Max 0` (code: menu switch off).

0.2.0 rebuilt the options from this: Lumen options dropped, SSR + experimental SSGI added,
shadow sharpness now -1 / -1.5 (0.1.0's -0.5 was softer than the game), shadow distance dropped,
Epic choices equal to the game dropped, sharpening added.

## Open

- Software Lumen live test (`r.DynamicGlobalIlluminationMethod 1`), SSGI look, presets measured,
  RT test, DLSS DLL swap (310.x).
