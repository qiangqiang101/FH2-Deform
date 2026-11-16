// FH2 Deform: cars crumple more in Forza Horizon 2.
//
// How FH2 deforms a car (from its car vertex shaders, e.g. carpaint_standardCarLightScenarioMorphing.pc.vso):
//   - a "damage cage", a 3D texture around the car's bounding box (DamageCageTexture), holds how damaged each spot is;
//   - each vertex has an authored "fully crushed" offset (g_MorphDataBuffer);
//   - the vertex moves by that offset times the cage's damage where it sits, clamped to 1 (g_HoodMorphDamageScalar).
// How much damage a hit writes into the cage depends on the surface hit: CarDamageScalar in
// game:\media\Physics\SurfaceTypes.xml (1.0 for most surfaces, 1.65 for some, the car's own materials included).
//
// Step 1: the Impact Multiplier. The game's own SurfaceTypes.xml (and NatalSurfaceTypes.xml, the Kinect variant) is
// read at startup, every CarDamageScalar is multiplied, and the result is handed to the game through the platform
// (set_file): the game's files aren't changed. So a hit dents as if it were that much harder; how deep a spot can go
// is still the car's authored crushed shape. (In game, even 10x made no visible difference.)
//
// Step 2: the shaders. The game reads _library\Shaders.zip's .durango.vso (DXBC) shaders. Every car
// ...LightScenarioMorphing vertex shader is patched (see dxbc_patch.h): the authored dent x Dent Depth, and the body
// pushed in where the cage says it was hit (Dents Where Hit), with ripples (Crumple) and where it was scraped (Scrape
// Dents). Every part of the car (paint, glass, trim, lights) and its shadow gets the same, so they stay together.

#include "fh2mod.h"
#include "fh2mod_l10n.h"
#include "fh2deform_l10n.inc"

#include "dxbc_patch.h"
#include "zip_read.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>

#include <string>
#include <vector>

static const FH2ModApi* g_api = nullptr;
static FH2Mod* g_mod = nullptr;
static wchar_t g_ini[MAX_PATH] = L"";
static std::string g_hide_list;   // [Test] HideList=a,b: families whose name has one of these are collapsed always (finding what draws what)
static int g_glass_extra = 0;         // [Test] GlassExtra: extra family names to treat as glass (bisecting which one draws the defroster)
static int g_test_all_variants = 0;   // 1 plain + Morphing + Skinning, 2 plain + Morphing, 3 Morphing + Skinning
static bool g_test_join_push = false;   // [Test] in the ini, for finding the black paint

static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_parts_fall_off{true};   // the player's car drops parts as the AI cars do
static std::atomic<float> g_impact_mult{2.0f};   // CarDamageScalar x this
static std::atomic<float> g_dent_depth{2.0f};     // the shaders' damage morph x this
static std::atomic<float> g_dents_hit{1.0f};      // x kDentM pushed in where hit
static std::atomic<float> g_crumple{1.0f};        // x kCrumpleM of ripples where hit
static std::atomic<float> g_scrape{1.0f};         // x kScrapeM pushed in where scraped
static bool g_glass_pixel_always = false;   // [Test] GlassPixel=2: discard all glass pixels (checks the override is used)
static std::atomic<bool> g_glass_pixel{true};   // [Test] GlassPixel=0: leave the glass pixel shaders alone
static std::atomic<float> g_glass_breaks{0.3f};   // windows pop where the impact damage there is over this (1 = never)
static const float kMinGlass = 0.05f, kMaxGlass = 1.0f;
static std::atomic<float> g_push_speed{8.0f};   // PushSpeed in the ini: the push keys' change of velocity (m/s)
static int g_forward_axis = 2, g_forward_sign = 1;   // which row of the car's matrix is "forward"
// The Numpad hit (8/2/4/6) and push (7/1/9/3) keys are switched off in the release: set to 1 to build them in again
// (the `hit`, `push`, `pose` and `hitraw` console commands stay).
#define FH2DEFORM_TEST_KEYS 0
// Parts Fall Off (the player's car dropping bumpers like the AI cars) doesn't work yet: the game never sends the drop event for
// the player's car, so the switch and its code patch are off. Set to 1 to build them in again.
#define FH2DEFORM_PARTS_FALL_OFF 0
static std::atomic<bool> g_hit_keys{true};          // Numpad 8/2/4/6 hit the player's car (testing)
static std::atomic<float> g_hit_strength{8.0f};     // ApplyDamage's b for those hits (a real crash: 14.4, then less)

static const float kMinMult = 1.0f, kMaxMult = 10.0f;
static const float kMinDepth = 1.0f, kMaxDepth = 5.0f;
static const float kMinExtra = 0.0f, kMaxExtra = 2.0f;
// At 1.0, where fully damaged: 5 cm pushed in toward the car's middle and 1.2 cm ripples (both x Dent Depth),
// and 1 cm pushed in where fully scraped.
static const float kDentM = 0.05f, kCrumpleM = 0.012f, kScrapeM = 0.01f;

static float clampf(float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); }

static void logf(const char* fmt, ...) {
    if (!g_api || !g_mod) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_api->log(g_mod, buf);
}

// ---- settings: fh2-mods\FH2Deform.ini ----
static float read_float(const wchar_t* key, float def, float lo, float hi) {
    wchar_t dflt[32], buf[32];
    swprintf(dflt, 32, L"%.2f", def);
    GetPrivateProfileStringW(L"Settings", key, dflt, buf, 32, g_ini);
    return clampf(static_cast<float>(_wtof(buf)), lo, hi);
}

static void write_float(const wchar_t* key, float v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", v);
    WritePrivateProfileStringW(L"Settings", key, buf, g_ini);
}

static void load_config() {
    if (!g_ini[0]) return;
    g_enabled.store(GetPrivateProfileIntW(L"Settings", L"Enabled", 1, g_ini) != 0);
    g_parts_fall_off.store(GetPrivateProfileIntW(L"Settings", L"PartsFallOff", 1, g_ini) != 0);
    g_hit_keys.store(GetPrivateProfileIntW(L"Settings", L"HitKeys", 1, g_ini) != 0);
    g_test_all_variants = static_cast<int>(GetPrivateProfileIntW(L"Test", L"TestAllVariants", 0, g_ini));
    { wchar_t w[256] = L""; GetPrivateProfileStringW(L"Test", L"HideList", L"", w, 256, g_ini);
      g_hide_list.clear(); for (wchar_t* c = w; *c; ++c) g_hide_list += static_cast<char>(tolower(*c)); }
    g_glass_pixel.store(GetPrivateProfileIntW(L"Test", L"GlassPixel", 1, g_ini) != 0);
    g_glass_pixel_always = GetPrivateProfileIntW(L"Test", L"GlassPixel", 1, g_ini) == 2;
    g_glass_extra = static_cast<int>(GetPrivateProfileIntW(L"Test", L"GlassExtra", 0, g_ini));
    g_test_join_push    = GetPrivateProfileIntW(L"Test", L"TestJoinPush", 0, g_ini) != 0;
    g_push_speed.store(read_float(L"PushSpeed", 8.0f, 0.5f, 60.0f));
    g_glass_breaks.store(read_float(L"GlassBreaksAt", 0.3f, kMinGlass, kMaxGlass));
    g_forward_axis = static_cast<int>(GetPrivateProfileIntW(L"Test", L"ForwardAxis", 2, g_ini));
    g_forward_sign = GetPrivateProfileIntW(L"Test", L"ForwardSign", 1, g_ini) < 0 ? -1 : 1;
    g_hit_strength.store(read_float(L"HitStrength", 8.0f, 0.5f, 30.0f));
    g_impact_mult.store(read_float(L"ImpactMultiplier", 2.0f, kMinMult, kMaxMult));
    g_dent_depth.store(read_float(L"DentDepth", 2.0f, kMinDepth, kMaxDepth));
    g_dents_hit.store(read_float(L"DentsWhereHit", 1.0f, kMinExtra, kMaxExtra));
    g_crumple.store(read_float(L"Crumple", 1.0f, kMinExtra, kMaxExtra));
    g_scrape.store(read_float(L"ScrapeDents", 1.0f, kMinExtra, kMaxExtra));
}

static void save_config() {
    if (!g_ini[0]) return;
    WritePrivateProfileStringW(L"Settings", L"Enabled", g_enabled.load() ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"Settings", L"PartsFallOff", g_parts_fall_off.load() ? L"1" : L"0", g_ini);
    WritePrivateProfileStringW(L"Settings", L"HitKeys", g_hit_keys.load() ? L"1" : L"0", g_ini);
    write_float(L"HitStrength", g_hit_strength.load());
    write_float(L"PushSpeed", g_push_speed.load());
    write_float(L"GlassBreaksAt", g_glass_breaks.load());
    write_float(L"ImpactMultiplier", g_impact_mult.load());
    write_float(L"DentDepth", g_dent_depth.load());
    write_float(L"DentsWhereHit", g_dents_hit.load());
    write_float(L"Crumple", g_crumple.load());
    write_float(L"ScrapeDents", g_scrape.load());
}

// ---- the surface types ----
static bool read_file(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Every <CarDamageScalar value="x"/> times `mult`; the rest of the file as it was. Returns how many were changed.
static int scale_car_damage(std::string& xml, float mult) {
    static const char kOpen[] = "<CarDamageScalar value=\"";
    std::string out;
    out.reserve(xml.size() + 4096);
    int changed = 0;
    size_t last = 0;
    for (size_t at = xml.find(kOpen); at != std::string::npos; at = xml.find(kOpen, at + 1)) {
        const size_t v = at + sizeof(kOpen) - 1;
        const size_t q = xml.find('"', v);
        if (q == std::string::npos || q - v > 32) continue;
        char* end = nullptr;
        const double old = strtod(xml.c_str() + v, &end);
        if (end != xml.c_str() + q) continue;   // not a plain number: leave it
        char num[64];
        snprintf(num, sizeof(num), "%.6f", old * mult);
        out.append(xml, last, v - last);
        out += num;
        last = q;
        ++changed;
    }
    out.append(xml, last, std::string::npos);
    xml.swap(out);
    return changed;
}

// The game's own file (game:\media\<media_path>), scaled, handed over as media\<media_path>; or the game's own again.
static void apply_surface_file(const char* media_path, const wchar_t* file_in_game) {
    if (!g_api || !g_mod || g_api->api_version < 2 || !g_api->set_file) return;
    if (!g_enabled.load() || g_impact_mult.load() == 1.0f) {
        g_api->clear_file(g_mod, media_path);
        logf("%s: the game's own (%s).", media_path, g_enabled.load() ? "Impact Multiplier 1.0" : "FH2 Deform off");
        return;
    }
    if (!g_api->game_dir || !g_api->game_dir[0]) {
        logf("%s: the game's folder isn't known; nothing changed.", media_path);
        return;
    }
    std::string xml;
    const std::wstring path = std::wstring(g_api->game_dir) + L"\\media\\" + file_in_game;
    if (!read_file(path, xml)) {
        logf("%s: couldn't read the game's copy; nothing changed.", media_path);
        return;
    }
    const int n = scale_car_damage(xml, g_impact_mult.load());
    if (n == 0) {
        logf("%s: no CarDamageScalar in it; nothing changed.", media_path);
        return;
    }
    if (g_api->set_file(g_mod, media_path, xml.data(), xml.size()))
        logf("%s: CarDamageScalar x%.2f on %d surfaces.", media_path, g_impact_mult.load(), n);
    else
        logf("%s: couldn't be replaced (another mod replaces it? see above).", media_path);
}

// ---- the car shaders ----
// Families that draw a window: glass, windscreens, car_window* (rear screens with the defroster lines, sunroofs),
// label_transparentwhenbroken (the cracked-glass layer) and the gls_ glass materials.
static bool in_hide_list(const std::string& low) {
    size_t i = 0;
    while (i < g_hide_list.size()) {
        size_t j = g_hide_list.find(',', i);
        if (j == std::string::npos) j = g_hide_list.size();
        if (j > i && low.find(g_hide_list.substr(i, j - i)) != std::string::npos) return true;
        i = j + 1;
    }
    return false;
}
static bool is_glass_family(const std::string& low) {
    if (in_hide_list(low)) return true;
    for (const char* k : {"glass", "windsheild", "windshield", "window", "transparentwhenbroken", "gls_", "smashable"})
        if (low.find(k) != std::string::npos) return true;
    static const char* const kExtra[] = {nullptr, "emissive", "diff1", "uniform", "car_cf", "badge", "simplebase", "clc_"};
    for (int i = 1; i <= g_glass_extra && i < 8; ++i)
        if (low.find(kExtra[i]) != std::string::npos) return true;
    return false;
}

// Every ...Morphing .durango.vso in media\_library\Shaders.zip, patched (or the game's own again), handed over as
// _library/Shaders/<family>/<name>.
static void apply_shaders() {
    if (!g_api || !g_mod || g_api->api_version < 2 || !g_api->set_file) return;
    if (!g_api->game_dir || !g_api->game_dir[0]) {
        logf("shaders: the game's folder isn't known; nothing changed.");
        return;
    }
    const DWORD t0 = GetTickCount();
    ZipReader zip;
    if (!zip.open(std::wstring(g_api->game_dir) + L"\\media\\_library\\Shaders.zip")) {
        logf("shaders: couldn't read _library\\Shaders.zip; nothing changed.");
        return;
    }
    MorphParams mp;
    mp.depth   = g_dent_depth.load();
    mp.dent    = kDentM * g_dents_hit.load();
    mp.crumple = kCrumpleM * g_crumple.load();
    mp.scrape  = kScrapeM * g_scrape.load();
    mp.join_push = g_test_join_push;
    const bool on = g_enabled.load() && (mp.depth != 1.0f || mp.extras());
    int patched = 0, skipped = 0, failed = 0, without_extras = 0;
    const char* why = nullptr;
    std::vector<unsigned char> d;
    // The car vertex shaders with the damage morph, as in 0.3.0 (tested good): every ...LightScenarioMorphing, near,
    // far (CarLOD15) and shadows. 0.4.2-0.6.1 also did the plain and ...Skinning variants (3690 files, then the 1500
    // near ones), and a crashed car's paint went black: back to these until that's understood.
    std::vector<const ZipEntry*> todo;
    for (const ZipEntry& e : zip.entries()) {
        static const char kExt[] = ".durango.vso";
        const size_t n = sizeof(kExt) - 1;
        if (e.name.size() <= n || e.name.compare(e.name.size() - n, n, kExt) != 0) continue;
        // TestAllVariants=1 in the ini (testing): the near ones in all three forms instead, as 0.4.3-0.6.1 did
        std::string low = e.name;
        for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const bool glass = is_glass_family(low);
        if (glass) {
            // windows: every variant (they take the plain or the Skinning one, not Morphing), so they dent with the body
            // and can pop
            if (e.name.find("LightScenario") == std::string::npos || e.name.find("ShadowDepth") != std::string::npos ||
                e.name.find("DebugLighting") != std::string::npos || e.name.find("Cubemap") != std::string::npos ||
                e.name.find("Track") != std::string::npos || e.name.find("WheelBlur") != std::string::npos)
                continue;
        } else if (!g_test_all_variants) {
            if (e.name.find("LightScenarioMorphing") == std::string::npos) continue;
        } else {
            if (e.name.find("CarLightScenario") == std::string::npos) continue;
            const bool skin = e.name.find("Skinning") != std::string::npos;
            const bool morph = e.name.find("Morphing") != std::string::npos;
            if (g_test_all_variants == 2 && skin) continue;
            // 4: plain and Skinning variants too, except the carpaint families' (which turn black when patched)
            if (g_test_all_variants == 4 && !morph && low.rfind("carpaint", 0) == 0) continue;
            if (g_test_all_variants == 4 && !morph && e.name.find("carpaint") != std::string::npos) continue;
            if (g_test_all_variants == 3 && !skin && !morph) continue;
        }
        if (e.name.find("DebugLighting") != std::string::npos) continue;   // a debug view, never drawn in play
        todo.push_back(&e);
    }
    for (const ZipEntry* ep : todo) {
        const ZipEntry& e = *ep;
        const std::string media_path = "_library/Shaders/" + e.name;
        if (!on) {
            g_api->clear_file(g_mod, media_path.c_str());
            continue;
        }
        if (!zip.read(e, d)) {
            ++failed;
            continue;
        }
        MorphPatchInfo info;
        MorphParams use = mp;
        // The plain and ...Skinning shaders move a part as a whole by one authored offset (a bumper, a wing, a window): the
        // game's own amount there (x1), or the whole part slides inside the car and renders black. Only ...Morphing
        // (per-vertex crush) gets Dent Depth; all of them get the push.
        if (e.name.find("Morphing") == std::string::npos) use.depth = 1.0f;
        {
            std::string low = e.name;
            for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            const bool glass = is_glass_family(low);
            use.glass_hide = glass && g_glass_breaks.load() < kMaxGlass ? g_glass_breaks.load() * use.depth : 0.0f;
            if (in_hide_list(low)) use.glass_hide = -1.0f;
        }
        if (patch_morph_shader(d, use, &info) != MorphPatch::Patched) {
            ++skipped;   // vegetation, skidmarks and such: no damage morph
            continue;
        }
        if (info.no_extras) {
            ++without_extras;   // only deeper there; the log says why
            why = info.no_extras;
        }
        if (g_api->set_file(g_mod, media_path.c_str(), d.data(), d.size())) ++patched;
        else ++failed;
    }
    // Test (HideList): every vertex shader of a matching name, whatever its kind (the Track variants too), collapsed.
    int hidden = 0;
    if (!g_hide_list.empty() && on) {
        for (const ZipEntry& e : zip.entries()) {
            static const char kVso[] = ".durango.vso";
            if (e.name.size() <= sizeof(kVso) - 1 || e.name.compare(e.name.size() - (sizeof(kVso) - 1), sizeof(kVso) - 1, kVso) != 0) continue;
            std::string low = e.name;
            for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (low.find("shadowdepth") != std::string::npos || low.find("debuglighting") != std::string::npos || !in_hide_list(low)) continue;
            if (low.find("lightscenario") == std::string::npos) continue;
            if (!zip.read(e, d) || !patch_hide_vertex_shader(d)) continue;
            if (g_api->set_file(g_mod, ("_library/Shaders/" + e.name).c_str(), d.data(), d.size())) ++hidden;
        }
        logf("shaders: HideList hid %d vertex shaders.", hidden);
    }
    // The glass pixel shaders: where the game would draw the cracks, nothing (the window has popped).
    int ps_done = 0;
    for (const ZipEntry& e : zip.entries()) {
        static const char kPso[] = ".durango.pso";
        if (e.name.size() <= sizeof(kPso) - 1 || e.name.compare(e.name.size() - (sizeof(kPso) - 1), sizeof(kPso) - 1, kPso) != 0) continue;
        if (e.name.find("LightScenario") == std::string::npos || e.name.find("Track") != std::string::npos ||
            e.name.find("ShadowDepth") != std::string::npos || e.name.find("DebugLighting") != std::string::npos ||
            e.name.find("Cubemap") != std::string::npos || e.name.find("WheelBlur") != std::string::npos)
            continue;
        if (e.name.compare(0, 14, "car_windsheild") != 0 && e.name.compare(0, 19, "car_window_interior") != 0) continue;
        const std::string media_path = "_library/Shaders/" + e.name;
        if (!g_glass_pixel.load() || !on || g_glass_breaks.load() >= kMaxGlass) {
            g_api->clear_file(g_mod, media_path.c_str());
            continue;
        }
        if (!zip.read(e, d) || !patch_glass_pixel_shader(d, g_glass_pixel_always)) continue;
        if (g_api->set_file(g_mod, media_path.c_str(), d.data(), d.size())) ++ps_done;
    }
    if (ps_done) logf("shaders: %d glass pixel shaders discard the cracks.", ps_done);
    if (on) {
        if (g_test_all_variants || g_test_join_push)
            logf("shaders: TEST: all variants %d, join push %d.", g_test_all_variants, g_test_join_push ? 1 : 0);
        logf("shaders: Dent Depth x%.2f, Dents Where Hit %.2f, Crumple %.2f, Scrape Dents %.2f on %d car shaders "
             "(%d without a damage morph, %d failed), %lu ms.",
             mp.depth, g_dents_hit.load(), g_crumple.load(), g_scrape.load(), patched, skipped, failed,
             static_cast<unsigned long>(GetTickCount() - t0));
        if (without_extras) logf("shaders: %d only deeper (%s).", without_extras, why);
    } else {
        logf("shaders: the game's own (%s).", g_enabled.load() ? "nothing to change" : "FH2 Deform off");
    }

}

static void apply_surfaces() {
    apply_surface_file("Physics/SurfaceTypes.xml", L"Physics\\SurfaceTypes.xml");
    apply_surface_file("Physics/NatalSurfaceTypes.xml", L"Physics\\NatalSurfaceTypes.xml");
}

// The shaders on a thread of their own, once more if the setting moved again meanwhile. Each set_file rewrites the
// platform's file list, so 1200 of them take 10 s or more: done in fh2mod_init they held every mod loading after this
// one back, and Underglow then found Platform.NET's hook in ForzaCar::Update and stayed off. On a thread is safe:
// the files written last time are still there for the game (set_file leaves an identical file alone), and the
// platform's leftover cleanup (15 s after the mods are loaded) only takes what nobody has claimed yet, which
// set_file writes again when the thread gets to it.
static std::mutex g_shader_lock;
static bool g_shader_busy = false, g_shader_again = false;

static void request_shaders() {
    std::lock_guard<std::mutex> lk(g_shader_lock);
    if (g_shader_busy) {
        g_shader_again = true;
        return;
    }
    g_shader_busy = true;
    std::thread([] {
        std::unique_lock<std::mutex> lk(g_shader_lock);
        do {
            g_shader_again = false;
            lk.unlock();
            apply_shaders();
            lk.lock();
        } while (g_shader_again);
        g_shader_busy = false;
    }).detach();
}

// ---- parts falling off the player's car ----
// A car part comes off through ForzaCar's drop routine (game+0x2978BE..): it builds the part's 0xA4-byte description
// and calls game+0x290C20(car, part, mode), which tells the car's presentation (car+0x4D38, slot 0xB0, the part's
// type) to show it gone and makes the falling piece (a CCarPartCollObj). Just before that call it asks the car
// (vtable +0xC8) whether to skip it, and skips it for a car driven by a human:
//     game+0x297D02  ff 90 c8 00 00 00   call [rax+0xC8]
//     game+0x297D08  84 c0               test al, al
//     game+0x297D0A  75 4f               jne  <past the drop>
// (+0xC8 is game+0x295A90: the car's controller says it's a human, or a global flag). That's why the AI cars lose
// their bumpers and the player's car doesn't. Two NOPs in place of that jne, and the player's car drops them too.
// (ForzaCar's +0xC8 is left alone: the game asks it for other things.)
static const uintptr_t kDropSkipJump = 0x297D0A;
static FH2Hook* g_drop_patch = nullptr;

// Which cars get a part-drop event (event 10 -> game+0x298AF0 -> the drop routine game+0x2978B0(car, part, mode)):
// the first 40 calls, with the car's kind (0 = the player's) and the part's type (part+0xA0), for finding out
// whether the player's car ever gets one.
using DropRoutineFn = void (*)(void* car, void* part, uint8_t mode);
static DropRoutineFn g_drop_routine_orig = nullptr;
static std::atomic<int> g_drop_logged{0};
static void drop_routine_detour(void* car, void* part, uint8_t mode) {
    if (g_drop_logged.fetch_add(1) < 40)
        logf("part drop event: car %p kind %d, mode %u, part type %d", car,
             *reinterpret_cast<int32_t*>(static_cast<char*>(car) + 0x4D08), mode,
             *reinterpret_cast<int32_t*>(static_cast<char*>(part) + 0xA0));
    g_drop_routine_orig(car, part, mode);
}

static void set_parts_hook() {
    if (!g_drop_patch) return;
    const bool on = g_enabled.load() && g_parts_fall_off.load();
    if (on) g_api->enable_hooks(g_mod, g_drop_patch);
    else g_api->disable_hooks(g_mod, g_drop_patch);
    logf("parts falling off: %s.", on ? "your car too" : "only the AI cars' (the game's own)");
}

static void install_parts_hook() {
    if (!g_api || g_api->api_version < 7 || !g_api->patch_bytes || !g_api->wait_for_code) return;
    std::thread([] {
        auto* base = static_cast<unsigned char*>(g_api->wait_for_code(120000));
        if (!base) {
            logf("parts falling off: the game's code wasn't ready; not changed.");
            return;
        }
        static const unsigned char kExpect[2] = {0x75, 0x4F}, kNops[2] = {0x90, 0x90};
        g_drop_patch = g_api->patch_bytes(g_mod, "ForzaCar part drop: not only for AI cars", base + kDropSkipJump,
                                          kExpect, kNops, 2);
        static const unsigned char kDropStart[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
        if (std::memcmp(base + 0x2978B0, kDropStart, sizeof kDropStart) == 0) {
            if (FH2Hook* h = g_api->hook_function(g_mod, "ForzaCar part drop (log)", base + 0x2978B0,
                                                  reinterpret_cast<void*>(&drop_routine_detour),
                                                  reinterpret_cast<void**>(&g_drop_routine_orig)))
                g_api->enable_hooks(g_mod, h);
        }
        if (!g_drop_patch) {
            logf("parts falling off: couldn't change the game's code (see above).");
            return;
        }
        set_parts_hook();
    }).detach();
}

// ---- hit keys: dent the player's car at the front, back or sides, no wall needed ----
// A car's visible damage comes from its presentation's ApplyDamage (ICarPresentation slot 0x190):
//     ApplyDamage(self, uint64 q, const vec4* where (world), float a, float b, float c, int flags)
// The car's handler for an impact event (game+0x275D20, event 0x18) makes exactly that call on the car's
// presentation (car+0x4D38, a CCarPresentationDeferred that queues it for the render thread; its slot 0x190 is
// game+0x132AF50), with q = the 8 bytes at car+0x344, the event's x, y, z, a = its +0x14, b = +0x0C, c = +0x10,
// flags 2 (8 for another flag). CarPresentation's own ApplyDamage (game+0x137D0C0): nothing happens if a is under
// DamageMinImpact and b under the scrape minimum; a at or over it counts in full; c sets how much (it goes through
// the damage settings' curves), and `where` picks the spot (each part's model is skipped when it's too far away).
// The player's car is the ForzaCar of kind 0 (car+0x4D08), seen in ForzaCar::Update (game+0x2A9400).
static const uintptr_t kForzaCarUpdate = 0x2A9400, kDeferredApplyDamage = 0x132AF50;
static const uintptr_t kDeferredPresVtables[] = {0x2EBEF68, 0x2EBE7E0};
using ForzaCarUpdateFn = void (*)(void* car, float dt, float b, uint32_t flag);
using ApplyDamageFn = void (*)(void* self, uint64_t q, const float* where, float a, float b, float c, int flags);
static ForzaCarUpdateFn g_car_update_orig = nullptr;
static ApplyDamageFn g_apply_damage_orig = nullptr;
static std::atomic<void*> g_player_car{nullptr};
static std::atomic<int> g_pending_hit{-1};      // 0 front, 1 back, 2 left, 3 right, 4 at g_hit_at
static std::atomic<int> g_hits_done{0};         // how many the game thread has made
static float g_hit_at[3] = {};                  // for 4 (set before g_pending_hit)
static std::atomic<float> g_hit_once{-1.0f};    // a strength for the next hit only (the command's)
static std::atomic<int> g_real_hits_logged{0};
static thread_local bool t_our_hit = false;   // ApplyDamage called by do_hit, not by the game

static uintptr_t game_base_addr() {
    return reinterpret_cast<uintptr_t>(g_api && g_api->game_base ? g_api->game_base() : nullptr);
}

// The car's deferred presentation, if it's what we know (its vtable checked)
static void* car_presentation(void* car) {
    void* pres = *reinterpret_cast<void**>(static_cast<char*>(car) + 0x4D38);
    if (!pres) return nullptr;
    const uintptr_t vt = *static_cast<uintptr_t*>(pres) - game_base_addr();
    for (uintptr_t v : kDeferredPresVtables)
        if (vt == v) return pres;
    return nullptr;
}

// The game's own hits (seen in the log, a crash into a post): `where` is in the car's own space (x across, y up,
// z along: (-0.45, 0.46, 2.30) at a front corner, (0.81, 0.51, -2.09) at the back, x = +-0.97 at the sides),
// a = 0 (0.01 for light touches), b = how hard (14.4 for the first contact of that crash, then halving), c = about
// b / 2, flags 13 for the first contact and 8 after. The hit keys do the same: b = Hit Strength, c = b / 2, flags 8.
static void do_hit(void* car, int side) {
    void* pres = car_presentation(car);
    if (!pres) return;
    static const char* kNames[] = {"front", "back", "left (x-)", "right (x+)", "a point"};
    static const float kWhere[4][3] = {{0.0f, 0.45f, 2.0f}, {0.0f, 0.45f, -2.0f}, {-0.85f, 0.4f, 0.0f}, {0.85f, 0.4f, 0.0f}};
    const float* w = side < 4 ? kWhere[side] : g_hit_at;
    alignas(16) float where[4] = {w[0], w[1], w[2], 1.0f};
    const uint64_t q = *reinterpret_cast<uint64_t*>(static_cast<char*>(car) + 0x344);
    const float once = g_hit_once.exchange(-1.0f);
    const float b = once > 0.0f ? once : g_hit_strength.load();
    void** vt = *static_cast<void***>(pres);
    t_our_hit = true;
    reinterpret_cast<ApplyDamageFn>(vt[0x190 / 8])(pres, q, where, 0.0f, b, b * 0.5f, 8);
    t_our_hit = false;
    logf("hit: %s of your car, strength %.1f, at (%.2f, %.2f, %.2f) in the car's space.", kNames[side], b, where[0],
         where[1], where[2]);
    g_hits_done.fetch_add(1);
}

// ---- pushing the player's car (test keys): through its dynamics, CCarDynamics (car+0x4C08):
// slot 0x278 the car's matrix (out float[16]), 0x288 velocity (out vec4), 0x290 SetVelocity (game+0x12E3CF0)
static const uintptr_t kDynSetVel = 0x12E3CF0, kDynGetVel = 0x12CDCF0, kDynMatrix = 0x12CD160;
static std::atomic<int> g_pending_push{-1};   // 0 forward, 1 back, 2 right, 3 left
static std::atomic<bool> g_pose_pending{false};
static bool car_dyn(void* car, void*& dyn, void**& vt) {
    dyn = *reinterpret_cast<void**>(static_cast<char*>(car) + 0x4C08);
    if (!dyn) return false;
    vt = *static_cast<void***>(dyn);
    const uintptr_t base = game_base_addr();
    return reinterpret_cast<uintptr_t>(vt[0x290 / 8]) == base + kDynSetVel &&
           reinterpret_cast<uintptr_t>(vt[0x288 / 8]) == base + kDynGetVel &&
           reinterpret_cast<uintptr_t>(vt[0x278 / 8]) == base + kDynMatrix;
}
// The car's rotation matrix: a copy in its dynamics object, found by scanning (rows: left, up, forward, all unit length;
// the forward row matches the direction of travel in game). Checked every time, nothing is done if it doesn't look right.
static bool car_axes(void* dyn, float left[3], float fwd[3]) {
    const float* m = reinterpret_cast<const float*>(static_cast<char*>(dyn) - 0x3A0 + 0x1AF0);
    __try {
        auto len2 = [](const float* r) { return r[0] * r[0] + r[1] * r[1] + r[2] * r[2]; };
        if (std::fabs(len2(m) - 1) > 0.02f || std::fabs(len2(m + 4) - 1) > 0.02f || std::fabs(len2(m + 8) - 1) > 0.02f) return false;
        for (int i = 0; i < 3; ++i) { left[i] = m[i]; fwd[i] = m[8 + i]; }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void do_push(void* car, int dir) {
    void* dyn; void** vt;
    if (!car_dyn(car, dyn, vt)) return;
    float left[3], fwd[3];
    if (!car_axes(dyn, left, fwd)) { logf("push: the car's axes weren't where expected"); return; }
    alignas(16) float v[4];
    reinterpret_cast<float* (*)(void*, float*)>(vt[0x288 / 8])(dyn, v);
    float d[3] = {};
    const float sp = g_push_speed.load();
    for (int i = 0; i < 3; ++i) d[i] = dir == 0 ? fwd[i] : dir == 1 ? -fwd[i] : dir == 2 ? -left[i] : left[i];
    alignas(16) float nv[4] = {v[0] + d[0] * sp, v[1] + d[1] * sp, v[2] + d[2] * sp, v[3]};
    reinterpret_cast<void (*)(void*, const float*)>(vt[0x290 / 8])(dyn, nv);
    static const char* kN[] = {"forward", "back", "right", "left"};
    logf("push: %s by %.1f m/s, along (%.2f %.2f %.2f)", kN[dir], sp, d[0], d[1], d[2]);
}
static void do_pose(void* car) {
    void* dyn; void** vt;
    if (!car_dyn(car, dyn, vt)) { logf("pose: not the dynamics I know"); return; }
    alignas(16) float v[4];
    reinterpret_cast<float* (*)(void*, float*)>(vt[0x288 / 8])(dyn, v);
    logf("pose: velocity %.2f %.2f %.2f; looking for a rotation matrix in the car and its dynamics", v[0], v[1], v[2]);
    // rows of 16 bytes: three that are unit length (xyz) and at right angles to each other
    const struct { const char* name; const char* at; } areas[] = {{"dyn-0x3A0", static_cast<char*>(dyn) - 0x3A0}, {"car", static_cast<char*>(car)}};
    for (const auto& ar : areas) {
        __try {
            for (int off = 0; off < 0x3000; off += 16) {
                const float* r0 = reinterpret_cast<const float*>(ar.at + off);
                const float* r1 = r0 + 4;
                const float* r2 = r0 + 8;
                auto len2 = [](const float* r) { return r[0] * r[0] + r[1] * r[1] + r[2] * r[2]; };
                auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
                if (!(std::fabs(len2(r0) - 1) < 0.01f && std::fabs(len2(r1) - 1) < 0.01f && std::fabs(len2(r2) - 1) < 0.01f)) continue;
                if (std::fabs(dot(r0, r1)) > 0.02f || std::fabs(dot(r1, r2)) > 0.02f || std::fabs(dot(r0, r2)) > 0.02f) continue;
                logf("pose: %s+0x%X rows (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)", ar.name, off, r0[0], r0[1], r0[2],
                     r1[0], r1[1], r1[2], r2[0], r2[1], r2[2]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static void push_command(void*, const char*, const char* args, char* reply, size_t n) {
    static const char* kD[] = {"forward", "back", "right", "left"};
    char w[16] = {};
    float sp = -1;
    sscanf_s(args ? args : "", "%15s %f", w, static_cast<unsigned>(sizeof w), &sp);
    for (int k = 0; k < 4; ++k)
        if (_stricmp(w, kD[k]) == 0) {
            if (sp > 0) g_push_speed.store(sp);
            g_pending_push.store(k);
            snprintf(reply, n, "pushing %s", kD[k]);
            return;
        }
    snprintf(reply, n, "error: push forward|back|right|left [m/s]");
}
static void pose_command(void*, const char*, const char*, char* reply, size_t n) {
    g_pose_pending.store(true);
    snprintf(reply, n, "asked (see platform.log)");
}

// fh2mod FH2Deform hitraw A B C FLAGS X Y Z: ApplyDamage with these exact values (testing)
static std::atomic<bool> g_raw_pending{false};
static float g_raw[7] = {};
static int g_raw_flags = 0;
static void do_raw_hit(void* car) {
    void* pres = car_presentation(car);
    if (!pres) return;
    alignas(16) float where[4] = {g_raw[3], g_raw[4], g_raw[5], 1.0f};
    const uint64_t q = *reinterpret_cast<uint64_t*>(static_cast<char*>(car) + 0x344);
    void** vt = *static_cast<void***>(pres);
    t_our_hit = true;
    reinterpret_cast<ApplyDamageFn>(vt[0x190 / 8])(pres, q, where, g_raw[0], g_raw[1], g_raw[2], g_raw_flags);
    t_our_hit = false;
    logf("hitraw: a %.3f b %.3f c %.3f flags %d at (%.2f, %.2f, %.2f)", g_raw[0], g_raw[1], g_raw[2], g_raw_flags, where[0],
         where[1], where[2]);
    g_hits_done.fetch_add(1);
}
static void hitraw_command(void*, const char*, const char* args, char* reply, size_t reply_size) {
    float v[7] = {};
    int fl = 0;
    if (sscanf_s(args ? args : "", "%f %f %f %d %f %f %f", &v[0], &v[1], &v[2], &fl, &v[3], &v[4], &v[5]) != 7) {
        snprintf(reply, reply_size, "error: hitraw A B C FLAGS X Y Z");
        return;
    }
    for (int i = 0; i < 6; ++i) g_raw[i] = v[i];
    g_raw_flags = fl;
    const int before = g_hits_done.load();
    g_raw_pending.store(true);
    for (int i = 0; i < 100 && g_hits_done.load() == before; ++i) Sleep(10);
    snprintf(reply, reply_size, g_hits_done.load() != before ? "made" : "error: not made within 1 s");
}

// fh2mod FH2Deform hit front|back|left|right [strength]   or   hit at X Y Z [strength]   (the car's own space)
static void hit_command(void*, const char*, const char* args, char* reply, size_t reply_size) {
    char word[16] = {};
    float a1 = -1, a2 = 0, a3 = 0, a4 = -1;
    const int n = sscanf_s(args ? args : "", "%15s %f %f %f %f", word, static_cast<unsigned>(sizeof word), &a1, &a2, &a3, &a4);
    static const char* kSides[] = {"front", "back", "left", "right"};
    int side = -1;
    for (int k = 0; k < 4; ++k)
        if (n >= 1 && _stricmp(word, kSides[k]) == 0) side = k;
    float strength = -1.0f;
    if (side >= 0) {
        strength = n >= 2 ? a1 : -1.0f;
    } else if (n >= 4 && _stricmp(word, "at") == 0) {
        g_hit_at[0] = a1; g_hit_at[1] = a2; g_hit_at[2] = a3;
        side = 4;
        strength = n >= 5 ? a4 : -1.0f;
    } else {
        snprintf(reply, reply_size, "error: hit front|back|left|right [strength], or hit at X Y Z [strength]");
        return;
    }
    if (!g_player_car.load()) {
        snprintf(reply, reply_size, "error: no player's car yet (drive in the world first)");
        return;
    }
    g_hit_once.store(strength);
    const int before = g_hits_done.load();
    g_pending_hit.store(side);
    for (int i = 0; i < 100 && g_hits_done.load() == before; ++i) Sleep(10);
    snprintf(reply, reply_size, g_hits_done.load() != before ? "hit made (see platform.log)" : "error: the game didn't make it within 1 s");
}

static void car_update_detour(void* car, float dt, float b, uint32_t flag) {
    g_car_update_orig(car, dt, b, flag);
    if (*reinterpret_cast<int32_t*>(static_cast<char*>(car) + 0x4D08) != 0) return;   // not the player's
    g_player_car.store(car);
    const int side = g_pending_hit.exchange(-1);
    if (side >= 0) do_hit(car, side);
    if (g_raw_pending.exchange(false)) do_raw_hit(car);
    if (g_pose_pending.exchange(false)) do_pose(car);
    const int push = g_pending_push.exchange(-1);
    if (push >= 0) do_push(car, push);
}

// The game's own hits on the player's car, to see what a real crash passes (the first 30)
static void apply_damage_detour(void* self, uint64_t q, const float* where, float a, float b, float c, int flags) {
    void* car = g_player_car.load();
    if (!t_our_hit && car && self == *reinterpret_cast<void**>(static_cast<char*>(car) + 0x4D38) &&
        g_real_hits_logged.fetch_add(1) < 30) {
        logf("real hit on your car: a %.3f b %.3f c %.3f flags %d at (%.2f, %.2f, %.2f) in the car's space", a, b, c,
             flags, where[0], where[1], where[2]);
    }
    g_apply_damage_orig(self, q, where, a, b, c, flags);
}

// Numpad 8 / 2 / 4 / 6: front / back / left / right, while the game's window is in front
static void hit_keys_thread() {
    static const int kKeys[4] = {VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6};
    static const int kPushKeys[4] = {VK_NUMPAD7, VK_NUMPAD1, VK_NUMPAD9, VK_NUMPAD3};   // forward, back, right, left
    bool down[4] = {}, pdown[4] = {};
    for (;;) {
        Sleep(30);
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool ours = pid == GetCurrentProcessId();
        for (int k = 0; k < 4; ++k) {
            const bool now = ours && (GetAsyncKeyState(kKeys[k]) & 0x8000) != 0;
            if (now && !down[k] && g_hit_keys.load() && g_enabled.load()) g_pending_hit.store(k);
            down[k] = now;
            const bool pn = ours && (GetAsyncKeyState(kPushKeys[k]) & 0x8000) != 0;
            if (pn && !pdown[k] && g_hit_keys.load() && g_enabled.load()) g_pending_push.store(k);
            pdown[k] = pn;
        }
    }
}

static void install_hit_keys() {
    if (!g_api || g_api->api_version < 3 || !g_api->hook_function || !g_api->wait_for_code) return;
    std::thread([] {
        auto* base = static_cast<unsigned char*>(g_api->wait_for_code(120000));
        if (!base) return;
        static const unsigned char kUpdateStart[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x57};
        static const unsigned char kApplyStart[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18};
        if (std::memcmp(base + kForzaCarUpdate, kUpdateStart, sizeof kUpdateStart) != 0 ||
            std::memcmp(base + kDeferredApplyDamage, kApplyStart, sizeof kApplyStart) != 0) {
            logf("hit keys: this isn't the FH2 build FH2 Deform knows; off.");
            return;
        }
        FH2Hook* h1 = g_api->hook_function(g_mod, "ForzaCar::Update (hit keys)", base + kForzaCarUpdate,
                                           reinterpret_cast<void*>(&car_update_detour),
                                           reinterpret_cast<void**>(&g_car_update_orig));
        FH2Hook* h2 = g_api->hook_function(g_mod, "CCarPresentationDeferred::ApplyDamage (hit keys)",
                                           base + kDeferredApplyDamage, reinterpret_cast<void*>(&apply_damage_detour),
                                           reinterpret_cast<void**>(&g_apply_damage_orig));
        if (!h1 || !h2) {
            logf("hit keys: couldn't hook (see above).");
            return;
        }
        g_api->enable_hooks(g_mod, h1);
        g_api->enable_hooks(g_mod, h2);
        if (g_api->api_version >= 9 && g_api->add_command)
            g_api->add_command(g_mod, "hit", "hit front|back|left|right [strength] | hit at X Y Z [strength]: dent your car there",
                               hit_command, nullptr);
        if (g_api->api_version >= 9 && g_api->add_command)
            g_api->add_command(g_mod, "push", "push forward|back|right|left [m/s]: shove your car (testing)", push_command, nullptr);
        if (g_api->api_version >= 9 && g_api->add_command)
            g_api->add_command(g_mod, "pose", "pose: logs the car's matrix and velocity (testing)", pose_command, nullptr);
        if (g_api->api_version >= 9 && g_api->add_command)
            g_api->add_command(g_mod, "hitraw", "hitraw A B C FLAGS X Y Z: ApplyDamage with exactly these (testing)",
                               hitraw_command, nullptr);
#if FH2DEFORM_TEST_KEYS
        logf("hit keys: Numpad 8 front, 2 back, 4 left, 6 right (strength %.1f; Num Lock on).", g_hit_strength.load());
        hit_keys_thread();
#endif
    }).detach();
}

// ---- the mod's page ----
static void on_setting_changed(void*, const char* key, int value) {
    if (strcmp(key, "Enabled") == 0) {
        g_enabled.store(value != 0);
        save_config();
        apply_surfaces();   // written now, read by the game when it next starts
        request_shaders();
        set_parts_hook();   // this one right away
    } else if (strcmp(key, "PartsFallOff") == 0) {
        g_parts_fall_off.store(value != 0);
        save_config();
        set_parts_hook();
    } else if (strcmp(key, "HitKeys") == 0) {
        g_hit_keys.store(value != 0);
        save_config();
    }
}

static void on_slider_changed(void*, const char* key, float value) {
    if (strcmp(key, "ImpactMultiplier") == 0) {
        g_impact_mult.store(clampf(value, kMinMult, kMaxMult));
        save_config();
        apply_surfaces();
        return;
    }
    if (strcmp(key, "DentDepth") == 0) g_dent_depth.store(clampf(value, kMinDepth, kMaxDepth));
    else if (strcmp(key, "DentsWhereHit") == 0) g_dents_hit.store(clampf(value, kMinExtra, kMaxExtra));
    else if (strcmp(key, "Crumple") == 0) g_crumple.store(clampf(value, kMinExtra, kMaxExtra));
    else if (strcmp(key, "ScrapeDents") == 0) g_scrape.store(clampf(value, kMinExtra, kMaxExtra));
    else if (strcmp(key, "GlassBreaksAt") == 0) g_glass_breaks.store(clampf(value, kMinGlass, kMaxGlass));
    else if (strcmp(key, "HitStrength") == 0) {   // used right away, no shaders to write
        g_hit_strength.store(clampf(value, 0.5f, 30.0f));
        save_config();
        return;
    }
    else return;
    save_config();
    request_shaders();
}

static void add_slider(const FH2ModApi* api, const char* key, const char* label, const char* description, float lo,
                       float hi, float value) {
    FH2Setting st{};
    st.size          = sizeof(st);
    st.key           = key;
    st.label         = label;
    st.description   = description;
    st.kind          = FH2_SETTING_SLIDER;
    st.min           = lo;
    st.max           = hi;
    st.step          = 0.25f;
    st.slider_value  = value;
    st.display_scale = 1.0f;
    st.decimals      = 2;
    st.needs_restart = 1;
    st.on_slide      = on_slider_changed;
    api->add_setting(g_mod, &st);
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;

    FH2ModInfo info{};
    info.size             = sizeof(info);
    info.name             = "Deform";
    info.version          = FH2DEFORM_VERSION;
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.description      = FH2_TR_DESCRIPTION("Cars crumple more when they crash: deeper dents, dents where the car was hit, crumpled panels, scrapes that flatten the body, and windows that pop out.");
    info.github_repo      = "qiangqiang101/FH2-Deform";
    info.check_updates    = 1;
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    FH2_ADD_TRANSLATIONS(api, g_mod, fh2deform_translations);

    if (api->azimuth_dir) swprintf(g_ini, MAX_PATH, L"%s\\fh2-mods\\FH2Deform.ini", api->azimuth_dir);
    load_config();
    save_config();   // the defaults go into the ini, so they can be edited
    logf("FH2 Deform %s: %s, Impact Multiplier %.2f, Dent Depth %.2f, Dents Where Hit %.2f, Crumple %.2f, "
         "Scrape Dents %.2f.", FH2DEFORM_VERSION, g_enabled.load() ? "on" : "off", g_impact_mult.load(),
         g_dent_depth.load(), g_dents_hit.load(), g_crumple.load(), g_scrape.load());
    apply_surfaces();   // before the game reads its surface types
    request_shaders();  // and its shaders, on a thread (see request_shaders)
#if FH2DEFORM_PARTS_FALL_OFF
    install_parts_hook();
#endif
    install_hit_keys();

    if (api->api_version >= 4) {
        FH2Setting on{};
        on.size          = sizeof(on);
        on.key           = "Enabled";
        // L10N(row-label): the main switch on the mod's page
        on.label         = FH2_TR(api, g_mod, "row.Enabled.label", "Stronger Deformation");
        // L10N(description): what the main switch does
        on.description   = FH2_TR(api, g_mod, "row.Enabled.description", "Cars crumple more when they crash. Off: the game's own damage.");
        on.value         = g_enabled.load() ? 1 : 0;
        on.needs_restart = 1;
        on.on_change     = on_setting_changed;
        api->add_setting(g_mod, &on);

#if FH2DEFORM_PARTS_FALL_OFF
        FH2Setting fall{};
        fall.size          = sizeof(fall);
        fall.key           = "PartsFallOff";
        // L10N(row-label): switch for parts coming off the player's car
        fall.label         = FH2_TR(api, g_mod, "row.PartsFallOff.label", "Parts Fall Off");
        // L10N(description): what the switch does
        fall.description   = FH2_TR(api, g_mod, "row.PartsFallOff.description",
                                    "Your car's parts can come off in heavy crashes, as the AI cars' do. "
                                    "Off: they stay on however crushed (the game's own).");
        fall.value         = g_parts_fall_off.load() ? 1 : 0;
        fall.needs_restart = 0;
        fall.on_change     = on_setting_changed;
        api->add_setting(g_mod, &fall);
#endif

#if FH2DEFORM_TEST_KEYS
        FH2Setting keys{};
        keys.size          = sizeof(keys);
        keys.key           = "HitKeys";
        keys.label         = "Hit Keys (testing)";
        keys.description   = "Numpad 8 / 2 / 4 / 6 hit your car at the front / back / left / right, as a crash there "
                             "would, so dents can be tried without a wall.";
        keys.value         = g_hit_keys.load() ? 1 : 0;
        keys.needs_restart = 0;
        keys.on_change     = on_setting_changed;
        api->add_setting(g_mod, &keys);
#endif
    }
    if (api->api_version >= 5) {
        FH2Setting mult{};
        mult.size          = sizeof(mult);
        mult.key           = "ImpactMultiplier";
        // L10N(slider-label): slider for how hard every hit counts
        mult.label         = FH2_TR(api, g_mod, "row.ImpactMultiplier.label", "Impact Multiplier");
        // L10N(description): what the slider does
        mult.description   = FH2_TR(api, g_mod, "row.ImpactMultiplier.description",
                                    "How much harder every hit counts for the game's own damage: 2.0 is like a hit twice as hard. "
                                    "A dent can't go deeper than the car's own crushed shape. Applies at the next start.");
        mult.kind          = FH2_SETTING_SLIDER;
        mult.min           = kMinMult;
        mult.max           = kMaxMult;
        mult.step          = 0.25f;
        mult.slider_value  = g_impact_mult.load();
        mult.display_scale = 1.0f;
        mult.decimals      = 2;
        mult.needs_restart = 1;
        mult.on_slide      = on_slider_changed;
        api->add_setting(g_mod, &mult);

        // L10N(slider-label): slider for how deep dents go
        add_slider(api, "DentDepth", FH2_TR(api, g_mod, "row.DentDepth.label", "Dent Depth"),
                   // L10N(description): what the slider does
                   FH2_TR(api, g_mod, "row.DentDepth.description",
                          "How deep a dent goes: 2.0 pushes it in twice as far as the car's own crushed shape."),
                   kMinDepth, kMaxDepth, g_dent_depth.load());
        // L10N(slider-label): slider for dents where the car was hit
        add_slider(api, "DentsWhereHit", FH2_TR(api, g_mod, "row.DentsWhereHit.label", "Dents Where Hit"),
                   // L10N(description): what the slider does
                   FH2_TR(api, g_mod, "row.DentsWhereHit.description",
                          "Pushes the body in wherever it was hit, also where the car has no crushed shape of its own. "
                          "0 is off."), kMinExtra, kMaxExtra, g_dents_hit.load());
        // L10N(slider-label): slider for ripples in damaged panels
        add_slider(api, "Crumple", FH2_TR(api, g_mod, "row.Crumple.label", "Crumple"),
                   // L10N(description): what the slider does
                   FH2_TR(api, g_mod, "row.Crumple.description",
                          "Ripples in damaged panels, so they look crumpled rather than evenly pushed in. 0 is off."),
                   kMinExtra, kMaxExtra, g_crumple.load());
        // L10N(slider-label): slider for dents from scraping along a wall
        add_slider(api, "ScrapeDents", FH2_TR(api, g_mod, "row.ScrapeDents.label", "Scrape Dents"),
                   // L10N(description): what the slider does
                   FH2_TR(api, g_mod, "row.ScrapeDents.description",
                          "Scrapes flatten the body a little too, not only the paint. 0 is off."), kMinExtra, kMaxExtra,
                   g_scrape.load());
        // L10N(slider-label): slider for when windows pop out
        add_slider(api, "GlassBreaksAt", FH2_TR(api, g_mod, "row.GlassBreaksAt.label", "Glass Breaks At"),
                   // L10N(description): what the slider does
                   FH2_TR(api, g_mod, "row.GlassBreaksAt.description",
                          "Windows pop out where the damage there is over this (0.05 to 1): lower breaks them sooner. 1.00: they stay."),
                   kMinGlass, kMaxGlass, g_glass_breaks.load());
#if FH2DEFORM_TEST_KEYS
        {
            FH2Setting st{};
            st.size          = sizeof(st);
            st.key           = "HitStrength";
            st.label         = "Hit Strength (testing)";
            st.description   = "How hard the hit keys hit.";
            st.kind          = FH2_SETTING_SLIDER;
            st.min           = 0.5f;
            st.max           = 30.0f;
            st.step          = 0.5f;
            st.slider_value  = g_hit_strength.load();
            st.display_scale = 1.0f;
            st.decimals      = 1;
            st.needs_restart = 0;
            st.on_slide      = on_slider_changed;
            api->add_setting(g_mod, &st);
        }
#endif
    }
    return 0;
}
