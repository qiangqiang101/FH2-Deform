// Patching FH2's car morphing vertex shaders (DXBC, shader model 5) for more, and more realistic, deformation.
//
// In every ...LightScenarioMorphing shader:
//     <cage coord>  = the vertex in car space, normalised to the car's box (g_BBMin cb4[24], g_BBMax cb4[25])
//     sample_l <dmg>, <cage coord>, t15, s0, l(0)          (DamageCageTexture: x impact, y scrape)
//     mul_sat rA.x, <dmg impact>, cb4[38]                  (g_HoodMorphDamageScalar, clamped to 1)
// and, with one morph target, the position and the normal are morphed by it:
//     mad rP.xyz, rA.xxxx, <authored offset>, v0           (position)
//     mad rN.xyz, rA.xxxx, <authored normal offset>, v1    (normal)
// (the compiler moves these around: before the branches or inside one, with a movc between).
//
// The same code is in the plain ...LightScenario variants too (not only ...Morphing): both are patched.
// The patch (new temp registers only; rA.x also goes to the pixel shader, for scratches and paint, and stays as is):
//   - depth: "mul rK.x, rA.x, l(depth)" right before each of those mads, which read rK.x instead: the authored dent,
//     deeper;
//   - before the cage sample: the cage coordinate kept (rC.xyz) and the cage sampled once more (rH.x impact x depth,
//     rH.y scrape: the same in every shader, whichever channels the shader itself reads);
//   - after the position morph, where its branches join (so parts without morph data, the windows say, which take
//     the other branch, move with the body), or else right after the mad: the vertex pushed toward the car's middle
//     by  rH.x * (dent + crumple * noise(p)) + scrape * rH.y
//     with p the car-space position and noise a sum of sines of it. The push is worked out in car space and turned
//     into the part's own space with the game's transforms (car: cb4[20..22], part: cb9[0..2]), so two parts that
//     touch get the same push and stay together;
//   - after the normal mad: the normal tilted by the crumple's slope, so the light shows the ripples.
// DXBC has no jump offsets (its control flow is structured), so only the sizes and the checksum change.
#pragma once

#include <cstdint>
#include <vector>

// The DXBC checksum (a modified MD5 over everything after the checksum itself).
void dxbc_checksum(const unsigned char* dxbc, size_t size, uint32_t out[4]);
bool dxbc_checksum_ok(const std::vector<unsigned char>& dxbc);

struct MorphParams {
    float depth   = 2.0f;    // the authored dent x this
    float dent    = 0.05f;   // m pushed in where fully damaged (x depth)
    float crumple = 0.012f;  // m of ripple where fully damaged (x depth)
    float scrape  = 0.01f;   // m pushed in where fully scraped
    float glass_hide = 0.0f; // > 0: where the (depth-scaled) damage is over this the vertex is collapsed: the glass pops out
    bool join_push = false;  // test: push after the morph's branches join, by the cage's own damage (0.4.2-0.6.1)
    bool extras() const { return dent != 0.0f || crumple != 0.0f || scrape != 0.0f; }
};

enum class MorphPatch {
    Patched,      // done
    NoMorph,      // no mad of v0/v1 by a clamped damage value: nothing to do
    Unsupported,  // not a DXBC shader this understands: left as it was
};

struct MorphPatchInfo {
    int mads = 0;           // mads now reading the scaled damage
    bool extras = false;    // dents where hit, crumple and scrapes added too
    bool glass = false;     // the glass-popping code went in too
    const char* no_extras = nullptr;   // why not, when they were asked for
};

MorphPatch patch_morph_shader(std::vector<unsigned char>& dxbc, const MorphParams& p, MorphPatchInfo* info = nullptr);

// A car glass pixel shader (...windsheild...LightScenario .pso): discards the pixels where it would draw the cracks
// (cb4[45].w > 0). false if it isn't one of those.
bool patch_glass_pixel_shader(std::vector<unsigned char>& dxbc, bool always = false);

// Test: a vertex shader of any kind with its position set to 0 before the last ret (nothing is drawn). false if not one.
bool patch_hide_vertex_shader(std::vector<unsigned char>& dxbc);
