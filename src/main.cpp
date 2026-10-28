// LostRecordsTweaks: a ReShade add-on for Lost Records: Bloom & Rage (Don't Nod, UE ~5.3,
// project UNI, DirectX 12) that raises the game's lighting, shadow and detail settings beyond its
// menu, live, from an in-game overlay. Built from DoubleExposureTweaks (UE 5.2.1), whose console
// variable layout this exe shares (docs/FINDINGS.md).
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, vtable layout read from the game's own call sites). Settings are kept in
// ReShade.ini [LostRecordsTweaks] and applied once the console manager is found. Values are
// set at console priority (the menu can't override them) and copied to the render thread
// (UE 5.2 only does that on the game thread). F8 switches all of it off and on.
//
// The add-on never patches game code: it reads it (to find the console manager) and calls the
// engine's own console variable functions.
//
// Hardware ray tracing test (Advanced tab): r.RayTracing is read once at startup, so the test
// writes it to the user's Engine.ini and checks on the next launch whether the game started with
// it. If that launch never gets far enough to check (the game stops at start when the ray tracing
// shaders weren't shipped, as in Reunion), the launch after it takes the setting out again.
//
// Writes LostRecordsTweaks.log (and -probe.txt, -rttest.ini) next to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Lighting, shadow and detail settings beyond " DT_GAME "'s menu, live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

const uint8_t* GameBase() { return reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)); }

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

// ReShade loads the add-on once per D3D12 device, and the game makes several at start, so the
// log is started fresh only by the first load in a process (marked in an environment variable).
void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    wchar_t pid[32] = {}, mine[32];
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"LOSTRECORDSTWEAKS_PID", pid, 32);
    const bool again = !wcscmp(pid, mine);
    SetEnvironmentVariableW(L"LOSTRECORDSTWEAKS_PID", mine);
    g_log = _wfopen((g_exeDir + DT_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded%s", again ? " (again in this process)" : "");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. The Game choice shows the game's current value (the probe writes them all).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

// Options for this game, from the first probe (2026-10-28, 4K, menu all Epic, DLSS on): the
// project has no Lumen (r.DynamicGlobalIlluminationMethod 0, r.ReflectionMethod 2 = SSR; baked
// lighting), so DoubleExposureTweaks' Lumen options are gone. Game values are quoted in the tips.
std::vector<Option> g_options = {
    // ------------------------------------------------ lighting (baked + screen space)
    {"Lighting", "Reflections", "Ssr", "Screen-space reflections",
     "The game reflects with screen-space reflections at quality 3 (Epic), and each scene\n"
     "limits which surfaces get them. Cinematic: quality 4 (more steps, fewer gaps).\n"
     "+ rough surfaces also traces reflections up to roughness 0.8 everywhere (wood,\n"
     "painted walls), overriding the scene's limit.",
     L"r.SSR.Quality",
     {{"Game", 0, {}},
      {"Cinematic", 4, {{L"r.SSR.Quality", L"4"}}},
      {"Cinematic + rough surfaces", 8, {{L"r.SSR.Quality", L"4"}, {L"r.SSR.MaxRoughness", L"0.8"}}}}},
    {"Lighting", "Bounce light (experimental)", "Ssgi", "Screen-space bounce light",
     "The game has no dynamic bounce light (lighting is baked). This turns on UE's\n"
     "screen-space global illumination on top: lamps, the flashlight and bright surfaces bounce\n"
     "colour onto nearby surfaces. Only what's on screen bounces. Experimental: may brighten\n"
     "scenes the artists lit by hand.",
     L"r.DynamicGlobalIlluminationMethod",
     {{"Game", 0, {}},
      {"On", 1, {{L"r.DynamicGlobalIlluminationMethod", L"2"}, {L"r.SSGI.Quality", L"3"}}},
      {"On, high quality", 4, {{L"r.DynamicGlobalIlluminationMethod", L"2"}, {L"r.SSGI.Quality", L"4"}}}}},

    // ------------------------------------------------ shadows
    {"Shadows", "Sun, moon and lamps", "ShadowRes", "Shadow sharpness",
     "The game sharpens shadows itself up to a resolution bias of -1, but backs off (it read\n"
     "-0.58) whenever shadows take over 40% of the frame, with only 3200 shadow pages.\n"
     "Sharp: holds -1 with 8192 pages. Sharpest: holds -1.5 with 16384 pages (more\n"
     "video memory and GPU time). Lower bias = sharper.",
     L"r.Shadow.Virtual.ResolutionLodBiasLocal",
     {{"Game", 0, {}},
      {"Sharp", 1,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-1"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-1"}}},
      {"Sharpest", 2,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"16384"},
        {L"r.Shadow.Virtual.ResolutionLodBiasDirectional", L"-1.5"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocal", L"-1.5"}}}}},
    {"Shadows", "Sun, moon and lamps", "ShadowSoft", "Soft shadow quality",
     "Rays per pixel for the soft edges of shadows. The game uses 8 rays of 4 samples for\n"
     "the sun but only 5 of 3 for lamps and the flashlight. Epic: 8 of 4 for both.\n"
     "Cinematic: 16 of 8, smoother and steadier at a real cost.",
     L"r.Shadow.Virtual.SMRT.RayCountLocal",
     {{"Game", 0, {}},
      {"Epic (8 rays)", 8,
       {{L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"4"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"8"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"4"}}},
      {"Cinematic (16 rays)", 16,
       {{L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"8"}}}}},
    {"Shadows", "Characters", "CapsuleShadows", "Character soft shadows",
     "The soft indirect shadows characters cast on the ground and walls, at full resolution\n"
     "instead of half (the game's).",
     L"r.CapsuleShadowsFullResolution",
     {{"Game", 0, {}}, {"Full resolution", 1, {{L"r.CapsuleShadowsFullResolution", L"1"}}}}},

    // ------------------------------------------------ characters
    {"Characters", "Skin and hair", "Skin", "Skin quality",
     "Subsurface scattering (light through skin): the game lets UE compute it on a\n"
     "checkerboard and fill in the rest (setting 2, automatic); Full computes every pixel:\n"
     "smoother skin, no fine dithering on faces.",
     L"r.SSS.Checkerboard",
     {{"Game", 0, {}}, {"Full", 1, {{L"r.SSS.Checkerboard", L"0"}}}}},
    {"Characters", "Skin and hair", "Hair", "Hair quality",
     "Strand hair samples per pixel and its shadow map resolution. The game uses 4 samples\n"
     "and 2048 px: fine strands break up and shimmer. High: 8 samples, 4096 px.",
     L"r.HairStrands.Visibility.MSAA.SamplePerPixel",
     {{"Game", 0, {}},
      {"High", 1,
       {{L"r.HairStrands.Visibility.MSAA.SamplePerPixel", L"8"},
        {L"r.HairStrands.Visibility.MSAA.MeanSamplePerPixel", L"1"},
        {L"r.HairStrands.DeepShadow.Resolution", L"4096"}}}}},

    // ------------------------------------------------ detail
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The game keeps 2 GB whatever the card, so\n"
     "textures can go blurry and reload. Free on a 16 GB card.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper floors and roads at shallow angles. The game uses 8x. Textures already loaded\n"
     "keep their filtering until they reload (or the game restarts). Free.",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}, {L"r.VT.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Distance", "ViewDistance", "Draw distance",
     "Distance at which objects, foliage and details switch to simpler versions or vanish.\n"
     "The game uses 100%.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}},
      {"Far (150%)", 150,
       {{L"r.ViewDistanceScale", L"1.5"}, {L"foliage.LODDistanceScale", L"1.5"}}}}},
    {"Detail", "Distance", "NaniteDetail", "Geometry detail",
     "Triangle size of the detailed (Nanite) models, in pixels. The game uses 1 px; 0.5\n"
     "keeps more detail on distant and mid-range surfaces, most visible with DLSS.",
     L"r.Nanite.MaxPixelsPerEdge",
     {{"Game", 0, {}}, {"Finer (0.5 px)", 1, {{L"r.Nanite.MaxPixelsPerEdge", L"0.5"}}}}},
    {"Detail", "Fog", "FogCells", "Volumetric fog detail",
     "Size of the fog's light grid in screen pixels. The game uses 8 px with 128 depth\n"
     "slices: blocky light shafts and flicker around lamps and the flashlight beam.\n"
     "Fine: 4 px.",
     L"r.VolumetricFog.GridPixelSize",
     {{"Game", 0, {}},
      {"Fine (4 px)", 4,
       {{L"r.VolumetricFog.GridPixelSize", L"4"}, {L"r.VolumetricFog.GridSizeZ", L"128"}}}}},
    {"Detail", "Image", "Sharpen", "Sharpening",
     "The tonemapper's sharpening (the game: off). Helps the soft look of DLSS 3.7, whose own\n"
     "sharpening no longer works.",
     L"r.Tonemapper.Sharpen",
     {{"Game", 0, {}},
      {"Light (0.5)", 5, {{L"r.Tonemapper.Sharpen", L"0.5"}}},
      {"Medium (1.0)", 10, {{L"r.Tonemapper.Sharpen", L"1"}}}}},

    // ------------------------------------------------ post-processing (taste; in no preset)
    {"Post-processing", "Camera", "FilmGrain", "Film grain", "The game's film grain noise (no menu option).",
     L"r.FilmGrain", {{"Game", 0, {}}, {"Off", 1, {{L"r.FilmGrain", L"0"}, {L"r.Tonemapper.GrainQuantization", L"0"}}}}},
    {"Post-processing", "Camera", "Fringe", "Chromatic aberration",
     "Colour fringes at the screen edges (no menu option).",
     L"r.SceneColorFringeQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.SceneColorFringeQuality", L"0"}}}}},
    {"Post-processing", "Camera", "DepthOfField", "Depth of field",
     "Background blur in close-ups and cutscenes (no menu option). Turning it off also\n"
     "removes deliberate focus effects.",
     L"r.DepthOfFieldQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {"Post-processing", "Camera", "LensFlares", "Lens flares", "Ghost reflections of bright lights across the screen.",
     L"r.LensFlareQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {"Post-processing", "Camera", "MotionBlur", "Motion blur",
     "Blur on fast camera turns and moving objects (the menu's switch sets its strength).",
     L"r.MotionBlurQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.MotionBlurQuality", L"0"}}}}},
};

bool g_enabled = true;  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    return m;
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
int g_sinkFlag = -1;
std::string g_liveError;
ULONGLONG g_readyTick = 0;

std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, int> g_originalPriority;    // and the SetBy priority it had
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)
std::set<std::wstring> g_noMirror;                 // render-thread copy layout not recognised
bool g_dirty = true;                               // options changed: apply at the next present
int g_reapplied = 0;                               // values put back after the game changed them

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

void FindManager() {
    const uint8_t* base = GameBase();
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (RVA 0x%llx, %d of %d matches), manager %p", singleton,
        singleton ? static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(singleton) - base) : 0ull, votes,
        matches, manager);
    if (!manager || !cvars::HasModuleVtable(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, vtables %d, IsVariableBool at +0x%x, values %d / %g",
        aniso, screen, vt, cvars::g_isVariableBool, a, s);
    if (!reads || a < 0 || a > 16 || !(s >= 10 && s <= 400)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    // Where each keeps the render thread's copy (logged to check the layout; nothing written).
    for (auto [name, var] : {std::pair<const char*, void*>{"r.MaxAnisotropy", aniso}, {"r.ScreenPercentage", screen}}) {
        cvars::Type t = cvars::GetType(var);
        void* data = cvars::AsData(var, t);
        Log("  %s: %s, priority 0x%02x, data %s (+0x%llx), ref value offset %d", name, cvars::TypeName(t),
            cvars::Priority(var), data ? "yes" : "no",
            data ? static_cast<unsigned long long>(static_cast<uint8_t*>(data) - static_cast<uint8_t*>(var)) : 0ull,
            cvars::RefValueOffset(var, t));
    }
    g_sinkFlag = cvars::FindSinkFlagOffset(manager);
    Log("console variable sinks flag at +0x%x (now %d)", g_sinkFlag, cvars::ReadSinkFlag(manager, g_sinkFlag));
    g_manager = manager;
    g_readyTick = GetTickCount64();
    g_live = Live::Ready;
}

// Sets at console priority and copies the value to the render thread. priority >= 0 then puts
// the recorded SetBy priority back to it (used when giving a value back to the game, so its menu
// can change it again).
bool SetVar(const std::wstring& key, const std::wstring& value, int priority = -1) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str())) return false;
    cvars::Mirror m = cvars::MirrorToRenderer(GameBase(), v.var, v.type);
    if (m == cvars::Mirror::Unknown && g_noMirror.insert(key).second)
        Log("%ls: render thread copy not recognised (%s); the value may only reach the renderer later", key.c_str(),
            cvars::TypeName(v.type));
    if (priority >= 0 && priority < 0x0a) {
        uint8_t p = static_cast<uint8_t>(priority);
        cvars::SafeCopy(static_cast<uint8_t*>(v.var) + cvars::kFlags + 3, &p, 1);
    }
    std::wstring now;
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second) Log("%ls = %ls not taken (reads %ls)", key.c_str(), value.c_str(), now.c_str());
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's (with the game's priority).
void Apply() {
    std::map<std::wstring, std::wstring> want = Wanted();
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) {
                g_originals[key] = old;
                if (VarInfo v = Lookup(key); v.var) g_originalPriority[key] = cvars::Priority(v.var);
            }
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        auto it = g_originals.find(key);
        auto pr = g_originalPriority.find(key);
        if (it != g_originals.end() && SetVar(key, it->second, pr != g_originalPriority.end() ? pr->second : -1))
            ++restored;
    }
    g_applied = std::move(want);
    if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
}

// Every second: any value that changed anyway (the game's code can set at console priority too)
// becomes the new game value, and ours goes back in.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    int fixed = 0;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++fixed;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (fixed) {
        g_reapplied += fixed;
        if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    }
}

// ---------------------------------------------------------------- probe
//
// Writes the game's value of every console variable under the prefixes below to
// LostRecordsTweaks-probe.txt: automatically 5 s and 60 s after connecting (title screen, then
// after the game has applied its menu), and from the Advanced tab at any time (in a scene). Names
// come from the game's read-only data (UTF-16 strings); nothing is enumerated.

const char* const kProbePrefixes[] = {
    "r.Lumen.", "r.RayTracing", "r.Raytracing", "r.Shadow.", "r.TSR.", "r.TemporalAA", "r.AntiAliasing",
    "r.ScreenPercentage", "r.SecondaryScreenPercentage", "r.DynamicRes.", "r.DynamicGlobalIlluminationMethod",
    "r.ReflectionMethod", "r.Streaming.", "r.MaxAnisotropy", "r.VT.", "r.HDR.", "r.Tonemapper", "r.Nanite.MaxPixelsPerEdge",
    "r.Nanite.ViewMeshLODBias", "r.HairStrands.", "r.SSS.", "r.SubsurfaceScattering", "r.SkinCache.", "r.MegaLights.",
    "r.VolumetricFog", "r.VolumetricCloud", "r.Bloom", "r.LensFlare", "r.DepthOfField", "r.DOF.", "r.MotionBlur",
    "r.SceneColorFringe", "r.FilmGrain", "r.Substrate", "r.PostProcessAAQuality", "r.SSR.", "r.AmbientOcclusion",
    "r.DistanceField", "r.ContactShadows", "r.CapsuleShadows", "r.ViewDistanceScale", "r.StaticMeshLODDistanceScale",
    "r.SkeletalMeshLODBias", "r.LandscapeLODBias", "r.Translucency", "r.SSGI", "r.LightMaxDrawDistanceScale",
    "r.MipMapLODBias", "r.Sharpen", "r.LUT.", "r.Color.", "r.EyeAdaptation", "r.DefaultFeature.", "r.FidelityFX",
    "r.D3D12.DXR", "foliage.", "grass.", "sg.", "r.uni.", "r.DNE", "r.DNM", "Uni.", "r.NGX.", "r.Streamline.",
    "r.NIS.", "r.Reflex", "r.PostProcessing.", "r.Velocity.", "r.BasePassForceOutputsVelocity",
};

std::set<std::wstring> g_probeNames;  // collected once

void CollectProbeNames() {
    std::vector<std::wstring> prefixes;
    for (const char* p : kProbePrefixes) prefixes.emplace_back(p, p + strlen(p));
    for (const cvars::Section& s : cvars::Sections(GameBase(), cvars::SectionKind::ReadOnly)) {
        for (size_t i = 2; i + 8 < s.size; i += 2) {
            if (s.begin[i - 2] || s.begin[i - 1]) continue;  // must start right after a terminator
            const wchar_t* w = reinterpret_cast<const wchar_t*>(s.begin + i);
            wchar_t c0 = w[0];
            if (c0 != L'r' && c0 != L'f' && c0 != L'g' && c0 != L's' && c0 != L'U') continue;
            size_t len = 0;
            while (len < 120 && i + 2 * len + 2 < s.size && w[len] > 32 && w[len] < 127) ++len;
            if (len < 3 || w[len] != 0) continue;
            for (const std::wstring& p : prefixes)
                if (len >= p.size() && !wcsncmp(w, p.c_str(), p.size())) {
                    g_probeNames.emplace(w, len);
                    break;
                }
        }
    }
    Log("probe: %zu candidate names", g_probeNames.size());
}

std::string g_probeResult;

void WriteProbe(const char* label) {
    if (g_probeNames.empty()) CollectProbeNames();
    FILE* f = _wfopen((g_exeDir + DT_PRODUCT_W L"-probe.txt").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== %s (%04u-%02u-%02u %02u:%02u:%02u, %.0f s after start, tweaks %s, holding %zu)\n", label, t.wYear,
            t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount64() - g_loadTick) / 1000.0,
            g_enabled ? "on" : "off", g_applied.size());
    int found = 0, unreadable = 0;
    for (const std::wstring& name : g_probeNames) {
        VarInfo v = Lookup(name);
        if (!v.var) continue;
        std::wstring value;
        if (Read(name, value)) {
            ++found;
            fprintf(f, "%ls (%s, set by 0x%02x) = %ls\n", name.c_str(), cvars::TypeName(v.type), cvars::Priority(v.var),
                    value.c_str());
        } else {
            ++unreadable;
            fprintf(f, "%ls (%s) = ?\n", name.c_str(), cvars::TypeName(v.type));
        }
    }
    fprintf(f, "---- %d values, %d unreadable (strings/commands skipped)\n", found, unreadable);
    fclose(f);
    Log("probe '%s': %d values written", label, found);
    g_probeResult = std::to_string(found) + " values written to " DT_PRODUCT "-probe.txt";
}

void ProbeOnPresent() {
    static int done = 0;
    ULONGLONG since = GetTickCount64() - g_readyTick;
    if (done == 0 && since > 5000) WriteProbe("auto: 5 s after connecting"), done = 1;
    else if (done == 1 && since > 60000) WriteProbe("auto: 60 s after connecting"), done = 2;
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset are left alone. The preset shown is derived from the options,
// "Custom" when none matches. Costs aren't measured in this game yet ("Measure preset costs").

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = not measured)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything below at Game.", {}},
    // From the first probe (2026-10-28). Not measured yet; SSGI (experimental) and sharpening
    // (taste) stay out of both.
    {"Ultra+", "",
     "Upgrades beyond the menu that are usually cheap: Cinematic screen-space reflections,\n"
     "sharper shadows held at -1, Epic soft shadows for lamps, full-resolution character\n"
     "shadows, full skin shading, high-quality hair, 8 GB textures, 16x filtering, far draw\n"
     "distance and finer geometry.\n\n"
     "Not measured in this game yet: use Measure preset costs.",
     {{"Ssr", 4}, {"ShadowRes", 1}, {"ShadowSoft", 8}, {"CapsuleShadows", 1}, {"Skin", 1}, {"Hair", 1},
      {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 150}, {"NaniteDetail", 1}}},
    {"Cinematic", "",
     "Ultra+ plus costlier upgrades: reflections on rough surfaces, the sharpest shadows,\n"
     "16-ray soft shadows and the finest fog.\n\n"
     "Not measured in this game yet: use Measure preset costs.",
     {{"Ssr", 8}, {"ShadowRes", 2}, {"ShadowSoft", 16}, {"CapsuleShadows", 1}, {"Skin", 1}, {"Hair", 1},
      {"TexturePool", 8192}, {"Anisotropy", 16}, {"ViewDistance", 150}, {"NaniteDetail", 1}, {"FogCells", 4}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with frame generation the present intervals alternate.
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// A measurement runs stages one after another: each sets the quality options (key -> choice id,
// everything else in a preset at Game), settles, then averages the frame time. "Presets" runs
// Game and each preset; "Each option" runs Game, then every option a preset uses at each choice
// a preset picks, one at a time, then Game again (to show drift).
struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 5000, sampleMs = 8000;
    std::vector<Stage> stages;
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = 0;
    for (auto& [key, id] : st.values)
        if (Option* o = FindOption(key.c_str())) o->value = id;
    g_dirty = true;
}

const char* ChoiceName(const Option& o, int id) {
    for (const Choice& c : o.choices)
        if (c.id == id) return c.label;
    return "?";
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 4000, g_measure.sampleMs = 6000;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const Option& o : g_options) {
            std::set<int> ids;
            for (const QualityPreset& p : kPresets)
                if (int id = PresetValue(p, o.key)) ids.insert(id);
            for (int id : ids) g_measure.stages.push_back({std::string(o.key) + " " + ChoiceName(o, id), {{o.key, id}}});
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, id] : kPresets[i].values) st.values.push_back({key, id});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages)", perOption ? "each option" : "presets", g_measure.stages.size());
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        std::string log = "measure: result";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
            snprintf(line, sizeof(line), ", %s %+.0f%% frame time", g_measure.stages[i].label.c_str(), (r[i] / base - 1) * 100);
            log += line;
        }
        g_measure.summary = sum;
        Log("%s", log.c_str());
        return;
    }
    // Each option, costliest first.
    std::vector<size_t> order;
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in " DT_PRODUCT ".log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- ray tracing activity
//
// Counts acceleration structure builds and DispatchRays calls: the only sure sign that the GPU
// is doing hardware ray tracing (the Lumen cvars can read 1 while r.RayTracing is off). Logged
// every 10 s while the count changes, for the first few minutes.

std::atomic<uint64_t> g_asBuilds{0}, g_rayDispatches{0};
uint64_t g_asPerSec = 0, g_raysPerSec = 0;

bool OnBuildAs(command_list*, acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
               const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
               acceleration_structure_build_mode) {
    ++g_asBuilds;
    return false;
}

bool OnDispatchRays(command_list*, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) {
    ++g_rayDispatches;
    return false;
}

void RayTracingOnPresent() {
    static ULONGLONG last = 0, lastLog = 0;
    static uint64_t prevAs = 0, prevRays = 0, loggedAs = ~0ull;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    uint64_t as = g_asBuilds.load(), rays = g_rayDispatches.load();
    g_asPerSec = as - prevAs, g_raysPerSec = rays - prevRays;
    prevAs = as, prevRays = rays, last = now;
    if (now - lastLog >= 10000 && now - g_loadTick < 600000 && as != loggedAs) {
        lastLog = now, loggedAs = as;
        Log("ray tracing: %llu acceleration structure builds, %llu DispatchRays so far (%llu / %llu per second)", as,
            rays, g_asPerSec, g_raysPerSec);
    }
}

// ---------------------------------------------------------------- ray tracing test
//
// r.RayTracing (and r.SkinCache.CompileShaders, which ray tracing needs for animated meshes) are
// read-only after startup, so the test goes through the user's Engine.ini:
//
//   Stage 1 "armed": the add-on wrote the settings under [/Script/Engine.RendererSettings]; the
//     next launch tests. Written again when the add-on unloads (the game rewrites Engine.ini on
//     exit and would drop them).
//   Stage 2 "running": this launch (Pid) started with them. The engine has read its config before
//     ReShade loads the add-on, so the lines are taken out at once: a launch that stops at start
//     (missing ray tracing shaders) can't take the next launch down with it. 10 s after the
//     add-on connects to the game's settings, r.RayTracing is read: 1 = it works (lines written
//     back, kept until the user turns it off), 0 = the game ignored it.
//   A later launch that finds stage 2 with another Pid: the test launch never got that far, so
//     the test failed (the next launch already started without the settings).
//   Stage 3 "done": Result holds the outcome.
//
// Lost Records saves its config from memory when it hits a fatal error (seen 2026-10-28: the
// test launch stopped at start and wrote r.RayTracing=1 back 2 s after the add-on took it out,
// so every later launch would have crashed). So the test launch also makes Engine.ini read-only
// after taking the lines out (Locked=1 in the state file); the check, or the next launch,
// makes it writable again.
//
// State lives in LostRecordsTweaks-rttest.ini next to the exe (not ReShade.ini: ReShade may
// not save it when the game crashes). SkinCache=1 records that the test added that line too.

constexpr const wchar_t* kRendererSection = L"/Script/Engine.RendererSettings";

std::wstring EngineIniPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return std::wstring(buf) + L"\\Bloom&Rage\\Saved\\Config\\Windows\\Engine.ini";
}

std::wstring RtStatePath() { return g_exeDir + DT_PRODUCT_W L"-rttest.ini"; }

struct RtTest {
    int stage = 0;
    DWORD pid = 0;
    bool skinCache = false;  // the test added r.SkinCache.CompileShaders
    bool works = false;
    bool locked = false;  // the add-on made Engine.ini read-only for the test launch
    std::string result;
    bool startupChecked = false;  // this process ran the load-time step
} g_rt;

void SaveRtState() {
    const std::wstring p = RtStatePath();
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", g_rt.stage);
    WritePrivateProfileStringW(L"Test", L"Stage", buf, p.c_str());
    swprintf(buf, 32, L"%lu", g_rt.pid);
    WritePrivateProfileStringW(L"Test", L"Pid", buf, p.c_str());
    WritePrivateProfileStringW(L"Test", L"SkinCache", g_rt.skinCache ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"Test", L"Works", g_rt.works ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"Test", L"Locked", g_rt.locked ? L"1" : L"0", p.c_str());
    std::wstring r(g_rt.result.begin(), g_rt.result.end());
    WritePrivateProfileStringW(L"Test", L"Result", r.c_str(), p.c_str());
}

void LoadRtState() {
    const std::wstring p = RtStatePath();
    g_rt.stage = GetPrivateProfileIntW(L"Test", L"Stage", 0, p.c_str());
    g_rt.pid = static_cast<DWORD>(GetPrivateProfileIntW(L"Test", L"Pid", 0, p.c_str()));
    g_rt.skinCache = GetPrivateProfileIntW(L"Test", L"SkinCache", 0, p.c_str()) != 0;
    g_rt.works = GetPrivateProfileIntW(L"Test", L"Works", 0, p.c_str()) != 0;
    g_rt.locked = GetPrivateProfileIntW(L"Test", L"Locked", 0, p.c_str()) != 0;
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"Test", L"Result", L"", buf, 512, p.c_str());
    g_rt.result = Narrow(buf);
}

// The value of <key> in the renderer section of Engine.ini, or empty.
std::wstring EngineIniValue(const wchar_t* key) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(kRendererSection, key, L"", buf, 64, EngineIniPath().c_str());
    return buf;
}

// Engine.ini can be read-only (some mods make it so); writing then fails.
bool EngineIniWritable(std::string* why) {
    const std::wstring path = EngineIniPath();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        if (why) *why = "Engine.ini is read-only";
        return false;
    }
    return true;
}

bool WriteRtLines() {
    const std::wstring path = EngineIniPath();
    if (path.empty() || !EngineIniWritable(nullptr)) return false;
    bool ok = WritePrivateProfileStringW(kRendererSection, L"r.RayTracing", L"1", path.c_str()) != 0;
    if (g_rt.skinCache) ok &= WritePrivateProfileStringW(kRendererSection, L"r.SkinCache.CompileShaders", L"1", path.c_str()) != 0;
    return ok;
}

void RemoveRtLines() {
    const std::wstring path = EngineIniPath();
    if (path.empty()) return;
    WritePrivateProfileStringW(kRendererSection, L"r.RayTracing", nullptr, path.c_str());
    if (g_rt.skinCache) WritePrivateProfileStringW(kRendererSection, L"r.SkinCache.CompileShaders", nullptr, path.c_str());
}

// Sets or clears Engine.ini's read-only flag (only ever the flag this add-on set).
void LockEngineIni(bool lock) {
    const std::wstring path = EngineIniPath();
    DWORD attr = GetFileAttributesW(path.c_str());
    if (path.empty() || attr == INVALID_FILE_ATTRIBUTES) return;
    DWORD want = lock ? (attr | FILE_ATTRIBUTE_READONLY) : (attr & ~FILE_ATTRIBUTE_READONLY);
    if (want != attr && !SetFileAttributesW(path.c_str(), want)) {
        Log("ray tracing test: couldn't make Engine.ini %s (error %lu)", lock ? "read-only" : "writable", GetLastError());
        return;
    }
    g_rt.locked = lock;
    Log("ray tracing test: Engine.ini %s", lock ? "made read-only for this launch" : "writable again");
}

// At load: moves an armed test to running, or finishes a test whose launch never reported back.
void RtTestOnLoad() {
    LoadRtState();
    const DWORD me = GetCurrentProcessId();
    if (g_rt.locked) {  // a test launch ended without unlocking (it stopped at start)
        LockEngineIni(false);
        SaveRtState();
    }
    if (g_rt.stage == 1) {
        if (EngineIniValue(L"r.RayTracing") != L"1") {
            // Too late for this launch (the engine read its config before ReShade loaded).
            WriteRtLines();
            Log("ray tracing test: Engine.ini had lost the setting; written again, test on the next launch");
            return;
        }
        // The engine has read its config by now, so the lines come out at once: if this launch
        // stops at start, the next one starts clean. They go back in only if the test passes.
        RemoveRtLines();
        g_rt.stage = 2;
        g_rt.pid = me;
        LockEngineIni(true);  // the game rewrites Engine.ini from memory if it stops at start
        SaveRtState();
        Log("ray tracing test: this launch started with r.RayTracing=1%s in Engine.ini (taken out again until "
            "the check passes)",
            g_rt.skinCache ? " and r.SkinCache.CompileShaders=1" : "");
    } else if (g_rt.stage == 2 && g_rt.pid != me) {
        RemoveRtLines();  // already out since that launch started; just in case
        g_rt.stage = 3;
        g_rt.works = false;
        g_rt.result =
            "The test launch closed before the check (most likely it stopped at start: no ray tracing "
            "shaders shipped). The setting is out of Engine.ini.";
        SaveRtState();
        Log("ray tracing test: the previous launch (pid %lu) never reached the check; setting removed", g_rt.pid);
    }
}

// On present, once connected: the check for a running test.
void RtTestOnPresent() {
    if (g_rt.stage != 2 || g_rt.pid != GetCurrentProcessId()) return;
    if (GetTickCount64() - g_readyTick < 10000) return;
    LockEngineIni(false);
    std::wstring rt, skin;
    Read(L"r.RayTracing", rt);
    Read(L"r.SkinCache.CompileShaders", skin);
    char buf[400];
    if (rt == L"1") {
        g_rt.works = true;
        WriteRtLines();  // keep it from now on
        snprintf(buf, sizeof(buf),
                 "The game started with hardware ray tracing on (r.RayTracing = 1, skin cache %s; %llu acceleration "
                 "structure builds so far). It stays on until you turn it off here.",
                 Narrow(skin).c_str(), static_cast<unsigned long long>(g_asBuilds.load()));
    } else {
        g_rt.works = false;
        RemoveRtLines();
        snprintf(buf, sizeof(buf),
                 "The game started but kept ray tracing off (r.RayTracing reads %s). The setting was taken out of "
                 "Engine.ini.",
                 rt.empty() ? "nothing" : Narrow(rt).c_str());
    }
    g_rt.result = buf;
    g_rt.stage = 3;
    SaveRtState();
    Log("ray tracing test: %s", buf);
}

std::string g_rtError;

void ArmRtTest() {
    std::string why;
    if (!EngineIniWritable(&why)) {
        g_rtError = why + "; can't write the test setting.";
        return;
    }
    std::wstring skin;
    g_rt.skinCache = Read(L"r.SkinCache.CompileShaders", skin) && skin == L"0" && EngineIniValue(L"r.SkinCache.CompileShaders").empty();
    if (!WriteRtLines()) {
        g_rtError = "Couldn't write Engine.ini.";
        return;
    }
    g_rt.stage = 1;
    g_rt.pid = 0;
    g_rt.works = false;
    g_rt.result.clear();
    g_rtError.clear();
    SaveRtState();
    Log("ray tracing test: armed (r.RayTracing=1%s written to Engine.ini)",
        g_rt.skinCache ? ", r.SkinCache.CompileShaders=1" : "");
}

void DisarmRtTest(const char* why) {
    RemoveRtLines();
    g_rt.stage = 0;
    g_rt.works = false;
    g_rt.result.clear();
    SaveRtState();
    Log("ray tracing test: %s; setting removed from Engine.ini", why);
}

// ---------------------------------------------------------------- present

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void LiveOnPresent() {
    Live state = g_live.load();
    if (state == Live::Idle) {
        g_live = Live::Searching;
        std::thread(FindManager).detach();
        return;
    }
    if (state != Live::Ready) return;
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys;
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        keys.insert(L"r.RayTracing");
        keys.insert(L"r.SkinCache.CompileShaders");
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s, set by 0x%02x) = %ls", key.c_str(), cvars::TypeName(info.type),
                info.var ? cvars::Priority(info.var) : -1, Read(key, v) ? v.c_str() : L"(not found)");
        }
    }
    if (g_dirty) {
        g_dirty = false;
        Apply();
    }
    Hold();
    ProbeOnPresent();
    RtTestOnPresent();
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                          : g_colorSpace == color_space::extended_srgb_linear ? "scRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(pid == GetCurrentProcessId());
    RayTracingOnPresent();
    LiveOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

std::string GameValue(const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = g_originals.find(name);
    if (it != g_originals.end() && g_applied.count(name)) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

std::string ChoiceLabel(const Option& o, const Choice& c) {
    if (c.id != 0) return c.label;
    std::string v = GameValue(o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

bool DrawOption(Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

bool DrawTab(const char* tab) {
    bool changed = false;
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(o);
    }
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

void DrawRayTracingTest() {
    ImGui::SeparatorText("Hardware ray tracing test");
    std::wstring rt;
    const bool rtOn = Read(L"r.RayTracing", rt) && rt == L"1";
    ImGui::TextDisabled("Ray tracing in this launch: %s", rt.empty() ? "?" : rtOn ? "on (r.RayTracing = 1)" : "off (r.RayTracing = 0)");
    switch (g_rt.stage) {
        case 1:
            ImGui::TextColored(kAccent, "Test armed: close and restart the game to run it.");
            if (ImGui::Button("Cancel the test")) DisarmRtTest("cancelled");
            break;
        case 2:
            ImGui::TextColored(kAccent, "Testing in this launch... (result 10 s after connecting)");
            break;
        default:
            if (g_rt.stage == 3 && !g_rt.result.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
                ImGui::TextColored(g_rt.works ? kGood : kWarn, "Last test: %s", g_rt.result.c_str());
                ImGui::PopTextWrapPos();
            }
            if (EngineIniValue(L"r.RayTracing") == L"1") {
                if (ImGui::Button("Turn ray tracing off again")) DisarmRtTest("turned off");
                Help("Takes r.RayTracing out of Engine.ini (and the skin cache line, if the test\n"
                     "added it). Applies from the next launch.");
            } else {
                if (ImGui::Button("Test on next launch")) ArmRtTest();
                Help("Hardware ray tracing can only be switched on at startup. This writes\n"
                     "r.RayTracing=1 (plus r.SkinCache.CompileShaders=1 if it's off) to your\n"
                     "Engine.ini; restart the game to test. The test launch takes the setting out\n"
                     "again as soon as it starts and puts it back only if ray tracing came on, so if\n"
                     "the game stops at start (missing ray tracing shaders), the next launch is\n"
                     "normal and shows the result here.\n\n"
                     "On its own this turns nothing visible on: it shows whether the game can run\n"
                     "ray tracing at all, which ray-traced options would need.");
            }
            break;
    }
    if (!g_rtError.empty()) ImGui::TextColored(kBad, "%s", g_rtError.c_str());
}

char g_cvarName[128] = "r.DynamicGlobalIlluminationMethod";
char g_cvarValue[64] = "";
std::string g_cvarResult;

void DrawAdvanced() {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Holding %zu settings; put back %d times after they changed", g_applied.size(), g_reapplied);
    if (!g_rejected.empty()) {
        std::string list;
        for (const std::wstring& k : g_rejected) list += (list.empty() ? "" : ", ") + Narrow(k);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
    }
    ImGui::TextDisabled("Hardware ray tracing: %s (%llu structure builds, %llu ray dispatches per second)",
                        g_asPerSec || g_raysPerSec ? "active" : "not in use", g_asPerSec, g_raysPerSec);
    ImGui::TextDisabled("Log: " DT_PRODUCT ".log next to the game's exe");
    if (live != Live::Ready) return;

    DrawRayTracingTest();

    ImGui::SeparatorText("Probe");
    if (ImGui::Button("Write all settings to file")) WriteProbe("manual");
    Help("Writes the game's value of every lighting, shadow, upscaling, detail and\n"
         "post-processing setting to " DT_PRODUCT "-probe.txt next to the game's exe.\n"
         "Do it in a scene (not a menu) for the values the game really plays with.");
    if (!g_probeResult.empty()) ImGui::TextDisabled("%s", g_probeResult.c_str());

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s (set by 0x%02x)%s", cvars::TypeName(v.type),
                            Read(name, value) ? Narrow(value).c_str() : "?", cvars::Priority(v.var),
                            g_applied.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        bool ok = SetVar(name, val);
        Log("console: %ls = %ls %s", name.c_str(), val.c_str(), ok ? "set" : "NOT taken");
        g_cvarResult = ok ? "Set (until an option changes it or the game restarts)" : "Not taken";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value now, at console priority. It isn't saved.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    ImGui::Spacing();
    if (kPresetCount > 1) {
        ImGui::BeginDisabled(measuring || live != Live::Ready);
        const int current = CurrentPreset();
        ChangedMark(current != 0);
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
            for (int i = 0; i < kPresetCount; ++i) {
                std::string item = kPresets[i].label;
                if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
                if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                    ApplyPreset(i);
                    changed = true;
                    Log("preset: %s", kPresets[i].label);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        Help("Sets the quality options together. Measure preset costs checks them on your PC.\n"
             "Changing an option afterwards shows Custom.");
        ImGui::SameLine();
    }
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (kPresetCount > 1) {
        if (measuring) {
            ImGui::TextColored(kAccent, "Measuring %s (%zu of %zu)... keep the camera still.",
                               g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1,
                               g_measure.stages.size());
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
        } else {
            ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
            if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
            ImGui::EndDisabled();
            Help("Runs each preset for 13 s (5 s to settle, 8 s measured) and shows how much longer\n"
                 "each frame takes than with Game. Stand still in a typical scene and leave the mouse\n"
                 "alone. Your options come back afterwards.");
            ImGui::SameLine();
            ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
            if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
            ImGui::EndDisabled();
            Help("Runs every option the presets use on its own (10 s each, about 5 minutes in all)\n"
                 "and writes how much each one costs to " DT_PRODUCT ".log, costliest first.");
            if (g_measure.done) {
                ImGui::TextDisabled("%s", g_measure.summary.c_str());
            } else if (!g_measure.note.empty()) {
                ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
            }
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        std::vector<const char*> tabs;  // in the order the options list them
        for (const Option& o : g_options)
            if (std::none_of(tabs.begin(), tabs.end(), [&](const char* t) { return !strcmp(t, o.tab); }))
                tabs.push_back(o.tab);
        for (const char* tab : tabs)
            if (ImGui::BeginTabItem(tab)) {
                changed |= DrawTab(tab);
                ImGui::EndTabItem();
            }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            RtTestOnLoad();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::build_acceleration_structure>(&OnBuildAs);
            reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
            reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            // The game rewrites Engine.ini when it exits; put an armed test or a passed one back.
            // Never during a running test (stage 2): its lines are out on purpose.
            if (g_rt.stage == 1 || (g_rt.stage == 3 && g_rt.works)) WriteRtLines();
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
