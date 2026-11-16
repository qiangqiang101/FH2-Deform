// Runs the morph patch over every ...Morphing shader in the game's _library\Shaders.zip, outside the game:
//   morph_scan <Shaders.zip> [depth [dent crumple scrape (m)]] [out folder]
// Checks the checksum code on the game's own files, says how many shaders it would patch and what it skips,
// and writes the patched files to the out folder (to disassemble with fxc -dumpbin).

#include "../src/dxbc_patch.h"
#include "../src/zip_read.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "morph_scan <Shaders.zip> [depth [dent crumple scrape]] [out folder]\n");
        return 2;
    }
    MorphParams p;
    int a = 2;
    if (argc > a) p.depth = static_cast<float>(_wtof(argv[a++]));
    if (argc > a + 2) {
        p.dent    = static_cast<float>(_wtof(argv[a++]));
        p.crumple = static_cast<float>(_wtof(argv[a++]));
        p.scrape  = static_cast<float>(_wtof(argv[a++]));
    }
    const std::wstring out = argc > a ? argv[a] : L"";
    std::printf("depth %.2f, dent %.3f m, crumple %.3f m, scrape %.3f m\n", p.depth, p.dent, p.crumple, p.scrape);
    ZipReader z;
    if (!z.open(argv[1])) {
        std::fprintf(stderr, "can't open the zip\n");
        return 1;
    }
    int total = 0, sum_ok = 0, patched = 0, nomorph = 0, unsupported = 0, bad_read = 0, sum_after = 0, extras = 0;
    std::map<int, int> mads_hist;
    std::map<std::string, int> no_extras;
    for (const ZipEntry& e : z.entries()) {
        if (e.name.find("LightScenario") == std::string::npos) continue;
        const bool durango = e.name.size() > 12 && e.name.compare(e.name.size() - 12, 12, ".durango.vso") == 0;
        if (!durango || e.name.find("DebugLighting") != std::string::npos) continue;
        ++total;
        std::vector<unsigned char> d;
        if (!z.read(e, d)) {
            ++bad_read;
            continue;
        }
        if (dxbc_checksum_ok(d)) ++sum_ok;
        else std::printf("checksum differs: %s\n", e.name.c_str());
        MorphPatchInfo info;
        switch (patch_morph_shader(d, p, &info)) {
        case MorphPatch::Patched:
            ++patched;
            ++mads_hist[info.mads];
            if (info.extras) ++extras;
            else if (info.no_extras) {
                ++no_extras[info.no_extras];
                std::printf("no extras (%s): %s\n", info.no_extras, e.name.c_str());
            }
            if (dxbc_checksum_ok(d)) ++sum_after;
            if (!out.empty()) {
                std::wstring path = out + L"\\";
                for (char c : e.name) path += c == '/' ? L'_' : static_cast<wchar_t>(c);
                HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                DWORD w = 0;
                if (f != INVALID_HANDLE_VALUE) {
                    WriteFile(f, d.data(), static_cast<DWORD>(d.size()), &w, nullptr);
                    CloseHandle(f);
                }
            }
            break;
        case MorphPatch::NoMorph:
            ++nomorph;
            { std::string l = e.name; for (char& c : l) c = (char)tolower((unsigned char)c);
              if (l.find("glass")!=std::string::npos||l.find("wind")!=std::string::npos||l.find("transparent")!=std::string::npos||l.find("gls_")!=std::string::npos)
                  if (l.find("shadowdepth")==std::string::npos && l.find("cubemap")==std::string::npos) std::printf("no morph (glass-like): %s\n", e.name.c_str()); }
            break;
        case MorphPatch::Unsupported:
            ++unsupported;
            std::printf("unsupported: %s\n", e.name.c_str());
            break;
        }
    }
    std::printf("%d shaders, %d read failures, checksum right on %d; patched %d (checksum right after on %d, "
                "with the extras %d), no morph %d, unsupported %d\n",
                total, bad_read, sum_ok, patched, sum_after, extras, nomorph, unsupported);
    for (auto& [m, n] : mads_hist) std::printf("  %d mads: %d shaders\n", m, n);
    for (auto& [why, n] : no_extras) std::printf("  no extras, %s: %d\n", why.c_str(), n);
    return 0;
}
