#include "dxbc_patch.h"

#include <cstring>
#include <initializer_list>
#include <algorithm>
#include <utility>

namespace {

// ---- MD5 (RFC 1321) block transform ----
inline uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void md5_transform(uint32_t s[4], const unsigned char block[64]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = block[i * 4] | (block[i * 4 + 1] << 8) | (block[i * 4 + 2] << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        const uint32_t t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

inline uint32_t rd(const std::vector<unsigned char>& d, size_t o) {
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}
inline void wr(std::vector<unsigned char>& d, size_t o, uint32_t v) {
    d[o] = static_cast<unsigned char>(v);
    d[o + 1] = static_cast<unsigned char>(v >> 8);
    d[o + 2] = static_cast<unsigned char>(v >> 16);
    d[o + 3] = static_cast<unsigned char>(v >> 24);
}
// ---- SM5 token stream ----
enum : uint32_t {
    OP_ADD = 0x00, OP_DIV = 0x0E, OP_DP3 = 0x10, OP_ELSE = 0x12, OP_ENDIF = 0x15, OP_ENDLOOP = 0x16,
    OP_ENDSWITCH = 0x17, OP_IF = 0x1F, OP_IGE = 0x21, OP_LABEL = 0x2C, OP_LOOP = 0x30, OP_MAD = 0x32,
    OP_MAX = 0x34, OP_CUSTOMDATA = 0x35, OP_MOV = 0x36, OP_MOVC = 0x37, OP_MUL = 0x38, OP_RSQ = 0x44,
    OP_RET = 0x3E, OP_LT = 0x31, OP_IFNZ_BIT = 1u << 18, OP_DCL_OUTPUT_SIV = 0x67,
    OP_SAMPLE_L = 0x48, OP_DISCARD = 0x0D, OP_SWITCH = 0x4C, OP_SINCOS = 0x4D, OP_DCL_CONSTANT_BUFFER = 0x59, OP_DCL_TEMPS = 0x68,
    T_TEMP = 0, T_INPUT = 1, T_IMM32 = 4, T_IMM64 = 5, T_RESOURCE = 7, T_CONSTANT_BUFFER = 8,
};

struct Operand {
    size_t at = 0, len = 0;   // token index in the shader, tokens
    uint32_t token = 0;
    uint32_t type = 0, index0 = 0;
    bool plain_imm_index = false;   // 1D, immediate index, no extended token
};

struct Instr {
    size_t at = 0, len = 0;
    uint32_t op = 0;
    int depth = 0;   // inside how many if / loop / switch blocks (an else: one less, like its if)
    bool sat = false;
    std::vector<Operand> ops;
};

// The length of an operand at tokens[i] (0 if it doesn't fit).
size_t operand_len(const std::vector<uint32_t>& t, size_t i, size_t end) {
    if (i >= end) return 0;
    const uint32_t tok = t[i];
    size_t n = 1;
    bool ext = (tok >> 31) != 0;
    while (ext) {
        if (i + n >= end) return 0;
        ext = (t[i + n] >> 31) != 0;
        ++n;
    }
    const uint32_t nc = tok & 3, type = (tok >> 12) & 0xFF, dim = (tok >> 20) & 3;
    for (uint32_t k = 0; k < dim; ++k) {
        const uint32_t rep = (tok >> (22 + 3 * k)) & 7;
        if (rep == 0) n += 1;
        else if (rep == 1) n += 2;
        else if (rep == 2 || rep == 3 || rep == 4) {
            n += (rep == 3 ? 1 : rep == 4 ? 2 : 0);
            const size_t r = operand_len(t, i + n, end);
            if (!r) return 0;
            n += r;
        } else return 0;
    }
    const uint32_t comps = nc == 0 ? 0 : nc == 1 ? 1 : 4;
    if (type == T_IMM32) n += comps;
    else if (type == T_IMM64) n += 2 * comps;
    return i + n <= end ? n : 0;
}

// Ops whose tokens after the opcode are operands (arithmetic and friends). Declarations, custom data and labels
// are left undecoded.
bool has_plain_operands(uint32_t op) {
    if (op >= 0x58 && op <= 0x6A) return false;                    // declarations (SM4)
    if ((op >= 0x8F && op <= 0xA2) || op == 0xCE) return false;   // declarations (SM5)
    if (op == OP_CUSTOMDATA || op == OP_LABEL) return false;
    return true;
}

bool decode(const std::vector<uint32_t>& t, std::vector<Instr>& out) {
    // t[0] version, t[1] length in tokens
    size_t i = 2;
    const size_t end = t.size();
    int depth = 0;
    while (i < end) {
        Instr in;
        in.at = i;
        const uint32_t tok = t[i];
        in.op = tok & 0x7FF;
        if (in.op == OP_CUSTOMDATA) {
            if (i + 1 >= end) return false;
            in.len = t[i + 1];
        } else {
            in.len = (tok >> 24) & 0x7F;
        }
        if (in.len == 0 || i + in.len > end) return false;
        if (in.op == OP_ENDIF || in.op == OP_ENDLOOP || in.op == OP_ENDSWITCH) --depth;
        if (depth < 0) return false;
        in.depth = in.op == OP_ELSE ? depth - 1 : depth;
        if (in.op == OP_IF || in.op == OP_LOOP || in.op == OP_SWITCH) ++depth;
        in.sat = ((tok >> 13) & 1) != 0;
        if (in.op != OP_CUSTOMDATA && has_plain_operands(in.op)) {
            size_t j = i + 1;
            bool ext = (tok >> 31) != 0;
            while (ext && j < i + in.len) {   // extended opcode tokens
                ext = (t[j] >> 31) != 0;
                ++j;
            }
            while (j < i + in.len) {
                const size_t n = operand_len(t, j, i + in.len);
                if (!n) {
                    in.ops.clear();
                    break;
                }
                Operand o;
                o.at    = j;
                o.len   = n;
                o.token = t[j];
                o.type  = (t[j] >> 12) & 0xFF;
                o.plain_imm_index = ((t[j] >> 20) & 3) == 1 && ((t[j] >> 22) & 7) == 0 && (t[j] >> 31) == 0;
                if (o.plain_imm_index) o.index0 = t[j + 1];
                in.ops.push_back(o);
                j += n;
            }
        }
        out.push_back(std::move(in));
        i += out.back().len;
    }
    return true;
}

// r<reg>.xxxx as a plain source operand
bool is_reg_xxxx(const Operand& o, uint32_t* reg) {
    if (o.token != 0x00100006u || !o.plain_imm_index) return false;   // 4 comps, swizzle xxxx, temp, 1D imm index
    *reg = o.index0;
    return true;
}

// A plain temp: 4 components, 1D immediate index, no modifier; `mode` 0 mask, 1 swizzle, 2 select one
bool is_plain_temp(const Operand& o, uint32_t mode) {
    return o.type == T_TEMP && o.plain_imm_index && o.len == 2 && (o.token & 3) == 2 && ((o.token >> 2) & 3) == mode;
}

// Does instruction `s` run on every path that reaches instruction `m` (m after s)? With structured control flow:
// yes unless the walk from s to m leaves s's block (an else or the end of it, or of one around it).
bool dominates(const std::vector<Instr>& ins, size_t s, size_t m) {
    if (m <= s) return false;
    for (size_t k = s + 1; k <= m; ++k)
        if (ins[k].depth < ins[s].depth) return false;
    return true;
}

// ---- writing instructions ----
using Toks = std::vector<uint32_t>;

constexpr uint32_t X = 0, Y = 1, Z = 2, W = 3;
constexpr uint32_t sw(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return a | (b << 2) | (c << 4) | (d << 6); }
constexpr uint32_t XYZ = 7, MX = 1, MY = 2, MW = 8;   // write masks
constexpr uint32_t SXYZW = sw(X, Y, Z, W), SXYZX = sw(X, Y, Z, X), SYZXY = sw(Y, Z, X, Y);
constexpr uint32_t SXXXX = sw(X, X, X, X), SYYYY = sw(Y, Y, Y, Y), SZZZZ = sw(Z, Z, Z, Z), SWWWW = sw(W, W, W, W);

uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
Toks dst(uint32_t r, uint32_t m) { return {0x00100002u | (m << 4), r}; }
Toks src(uint32_t r, uint32_t s) { return {0x00100006u | (s << 4), r}; }
Toks one(uint32_t r, uint32_t c) { return {0x0010000Au | (c << 4), r}; }
Toks cb(uint32_t slot, uint32_t idx, uint32_t s) { return {0x00208006u | (s << 4), slot, idx}; }
Toks cb1(uint32_t slot, uint32_t idx, uint32_t c) { return {0x0020800Au | (c << 4), slot, idx}; }
Toks imm(float a) { return {0x00004001u, fbits(a)}; }
Toks imm(float a, float b, float c, float d) { return {0x00004002u, fbits(a), fbits(b), fbits(c), fbits(d)}; }
Toks odst(uint32_t r, uint32_t m) { return {0x00102002u | (m << 4), r}; }   // an output register
Toks null_dst() { return {0x0000D000u}; }

struct Emit {
    Toks t;
    void op(uint32_t opc, std::initializer_list<Toks> ops, bool sat = false) {
        size_t n = 1;
        for (const Toks& o : ops) n += o.size();
        t.push_back(opc | (sat ? 1u << 13 : 0u) | (static_cast<uint32_t>(n) << 24));
        for (const Toks& o : ops) t.insert(t.end(), o.begin(), o.end());
    }
};

// The crumple: n(p) = (sin(a.x) + sin(a.y) + sin(a.z)) / 3, a = F * p + G * p.yzx (p in m, car space): ripples
// 20-50 cm apart, the same for every part at the same spot.
const float kF[3] = {23.0f, 17.0f, 29.0f}, kG[3] = {13.0f, 31.0f, 19.0f};

struct Regs {
    uint32_t K, C, D, E, T, H;   // the mad's scaled damage; cage coord; work; H.x impact x depth, H.y scrape
};

// rD.xyz = the vertex in car space, from the car's middle: (cage coord - 0.5) * box size
void car_pos(Emit& e, const Regs& r) {
    e.op(OP_ADD, {dst(r.D, XYZ), src(r.C, SXYZX), imm(-0.5f, -0.5f, -0.5f, 0.0f)});
    e.op(OP_MAD, {dst(r.E, XYZ), cb(4, 24, SXYZX), imm(-1.0f, -1.0f, -1.0f, 0.0f), cb(4, 25, SXYZX)});
    e.op(OP_MUL, {dst(r.D, XYZ), src(r.D, SXYZX), src(r.E, SXYZX)});
}

// rE.xyz = a (the crumple's sine arguments) for rD.xyz
void crumple_args(Emit& e, const Regs& r) {
    e.op(OP_MUL, {dst(r.E, XYZ), src(r.D, SXYZX), imm(kF[0], kF[1], kF[2], 0.0f)});
    e.op(OP_MAD, {dst(r.E, XYZ), src(r.D, SYZXY), imm(kG[0], kG[1], kG[2], 0.0f), src(r.E, SXYZX)});
}

// rD.xyz, a vector in car space, into the part's own space: car -> world with the car's inverse world transform
// (cb4[20..22]) transposed, world -> part with the part's transform (cb9[0..2]) transposed, each divided by its
// scale squared (so a uniform scale comes out right too).
void car_to_part(Emit& e, const Regs& r) {
    e.op(OP_MUL, {dst(r.T, XYZ), src(r.D, SXXXX), cb(4, 20, SXYZX)});
    e.op(OP_MAD, {dst(r.T, XYZ), src(r.D, SYYYY), cb(4, 21, SXYZX), src(r.T, SXYZX)});
    e.op(OP_MAD, {dst(r.T, XYZ), src(r.D, SZZZZ), cb(4, 22, SXYZX), src(r.T, SXYZX)});
    e.op(OP_DP3, {dst(r.T, MW), cb(4, 20, SXYZX), cb(4, 20, SXYZX)});
    e.op(OP_MAX, {dst(r.T, MW), one(r.T, W), imm(1e-12f)});
    e.op(OP_DIV, {dst(r.T, XYZ), src(r.T, SXYZX), src(r.T, SWWWW)});
    e.op(OP_MUL, {dst(r.D, XYZ), src(r.T, SXXXX), cb(9, 0, SXYZX)});
    e.op(OP_MAD, {dst(r.D, XYZ), src(r.T, SYYYY), cb(9, 1, SXYZX), src(r.D, SXYZX)});
    e.op(OP_MAD, {dst(r.D, XYZ), src(r.T, SZZZZ), cb(9, 2, SXYZX), src(r.D, SXYZX)});
    e.op(OP_DP3, {dst(r.D, MW), cb(9, 0, SXYZX), cb(9, 0, SXYZX)});
    e.op(OP_MAX, {dst(r.D, MW), one(r.D, W), imm(1e-12f)});
    e.op(OP_DIV, {dst(r.D, XYZ), src(r.D, SXYZX), src(r.D, SWWWW)});
}

} // namespace

void dxbc_checksum(const unsigned char* dxbc, size_t size, uint32_t out[4]) {
    // As d3dcompiler does it: MD5 of everything from byte 20 on, with its own padding (the bit length first in the
    // last block and (bits >> 2) | 1 at its end).
    uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const unsigned char* p = dxbc + 20;
    const size_t n = size - 20;
    size_t done = 0;
    for (; done + 64 <= n; done += 64) md5_transform(s, p + done);
    const size_t left = n - done;
    const uint32_t bits = static_cast<uint32_t>(n * 8), tail = (bits >> 2) | 1;
    unsigned char block[64];
    if (left >= 56) {
        std::memset(block, 0, 64);
        std::memcpy(block, p + done, left);
        block[left] = 0x80;
        md5_transform(s, block);
        unsigned char next[64];
        std::memset(next, 0, 64);
        std::memcpy(next, &bits, 4);
        std::memcpy(next + 60, &tail, 4);
        md5_transform(s, next);
    } else {
        std::memset(block, 0, 64);
        std::memcpy(block, &bits, 4);
        std::memcpy(block + 4, p + done, left);
        block[4 + left] = 0x80;
        std::memcpy(block + 60, &tail, 4);
        md5_transform(s, block);
    }
    std::memcpy(out, s, 16);
}

bool dxbc_checksum_ok(const std::vector<unsigned char>& d) {
    if (d.size() < 32 || std::memcmp(d.data(), "DXBC", 4) != 0) return false;
    uint32_t h[4];
    dxbc_checksum(d.data(), d.size(), h);
    return std::memcmp(h, d.data() + 4, 16) == 0;
}

MorphPatch patch_morph_shader(std::vector<unsigned char>& d, const MorphParams& P, MorphPatchInfo* info) {
    MorphPatchInfo dummy;
    MorphPatchInfo& inf = info ? *info : dummy;
    inf = {};
    if (d.size() < 32 || std::memcmp(d.data(), "DXBC", 4) != 0 || rd(d, 24) != d.size()) return MorphPatch::Unsupported;
    const uint32_t nchunks = rd(d, 28);
    if (32 + 4ull * nchunks > d.size()) return MorphPatch::Unsupported;
    size_t shex = 0, shex_size = 0;
    for (uint32_t c = 0; c < nchunks; ++c) {
        const size_t at = rd(d, 32 + 4 * c);
        if (at + 8 > d.size()) return MorphPatch::Unsupported;
        if (std::memcmp(&d[at], "SHEX", 4) == 0 || std::memcmp(&d[at], "SHDR", 4) == 0) {
            shex = at;
            shex_size = rd(d, at + 4);
        }
    }
    if (!shex || shex + 8 + shex_size > d.size() || shex_size < 8 || (shex_size & 3)) return MorphPatch::Unsupported;

    std::vector<uint32_t> t(shex_size / 4);
    for (size_t i = 0; i < t.size(); ++i) t[i] = rd(d, shex + 8 + 4 * i);
    if (t[1] != t.size() || ((t[0] >> 16) & 0xFFFF) != 1 /* vertex shader */) return MorphPatch::Unsupported;
    std::vector<Instr> ins;
    if (!decode(t, ins)) return MorphPatch::Unsupported;

    // dcl_temps, and the constant buffers' sizes (dcl_constantbuffer cb<slot>[<size>])
    uint32_t cb_size[16] = {};
    size_t dcl = SIZE_MAX;
    for (const Instr& in : ins) {
        if (in.op == OP_DCL_TEMPS && in.len == 2) dcl = in.at;
        if (in.op == OP_DCL_CONSTANT_BUFFER && in.len >= 4) {
            const uint32_t tok = t[in.at + 1];
            if (((tok >> 12) & 0xFF) == T_CONSTANT_BUFFER && ((tok >> 20) & 3) == 2 && t[in.at + 2] < 16)
                cb_size[t[in.at + 2]] = t[in.at + 3];
        }
    }

    // The morph mads: mad <dst>, rA.xxxx, <offset>, v0 or v1, with a mul_sat (the cage times
    // g_HoodMorphDamageScalar) somewhere before them.
    struct Mad { size_t i; uint32_t reg; bool position; };
    std::vector<Mad> mads;
    bool saw_sat_mul = false;
    for (size_t i = 0; i < ins.size(); ++i) {
        const Instr& m = ins[i];
        if (m.op == OP_MUL && m.sat) saw_sat_mul = true;
        uint32_t r;
        if (!saw_sat_mul || m.op != OP_MAD || m.sat || m.ops.size() != 4 || !is_reg_xxxx(m.ops[1], &r)) continue;
        const Operand& src2 = m.ops[3];
        if (src2.type != T_INPUT || !src2.plain_imm_index || src2.index0 > 1) continue;
        mads.push_back({i, r, src2.index0 == 0});
    }
    if (mads.empty()) return MorphPatch::NoMorph;
    if (dcl == SIZE_MAX || t[dcl + 1] > 4000) return MorphPatch::Unsupported;
    inf.mads = static_cast<int>(mads.size());

    // For the extras: the cage sample (sample_l of t15) that runs before every one of those mads, the constants
    // they read, and mads whose result can be added to.
    size_t s = SIZE_MAX;
    bool extras = P.extras();
    if (extras) {
        for (size_t i = 0; i < mads.front().i; ++i) {
            const Instr& in = ins[i];
            if (in.op != OP_SAMPLE_L || in.ops.size() != 5) continue;
            const Operand& res = in.ops[2];
            if (res.type != T_RESOURCE || !res.plain_imm_index || res.index0 != 15 || ((res.token >> 2) & 3) != 1) continue;
            if (!is_plain_temp(in.ops[0], 0)) continue;
            bool all = true;
            for (const Mad& m : mads) all = all && dominates(ins, i, m.i);
            if (all) s = i;
        }
        if (s == SIZE_MAX) inf.no_extras = "no damage cage sample before the morph";
        else if (cb_size[4] < 39 || cb_size[9] < 3) inf.no_extras = "the car's or the part's constants aren't there";
        for (const Mad& m : mads)
            if (!is_plain_temp(ins[m.i].ops[0], 0) || ((ins[m.i].ops[3].token >> 2) & 3) != 1)
                inf.no_extras = "a morph mad of an unexpected form";
        if (inf.no_extras) extras = false;
    }

    const uint32_t base = t[dcl + 1];
    const Regs R{base, base + 1, base + 2, base + 3, base + 4, base + 5};
    t[dcl + 1] = base + (extras ? 6 : 1);

    // Where the push goes for a position mad: after the if/else around it has joined (so a part without morph data,
    // which takes the other branch, is pushed too), when every branch writes the same register and the cage sample
    // runs before it all; otherwise right after the mad.
    auto writes = [&](size_t from, size_t to, uint32_t reg, uint32_t msk) {
        for (size_t k = from; k < to; ++k)
            if (!ins[k].ops.empty() && is_plain_temp(ins[k].ops[0], 0) && ins[k].ops[0].index0 == reg &&
                ((ins[k].ops[0].token >> 4) & 0xF & msk) == msk)
                return true;
        return false;
    };
    auto push_point = [&](size_t mi) -> size_t {
        const Instr& in = ins[mi];
        const size_t after_mad = in.at + in.len;
        const int D = ins[s].depth;
        if (in.depth <= D) return after_mad;
        const uint32_t reg = in.ops[0].index0, msk = (in.ops[0].token >> 4) & 0xF;
        size_t j = SIZE_MAX;   // the if at the sample's depth around the mad
        for (size_t k = mi; k-- > s;) {
            if (ins[k].depth > D || (ins[k].depth == D && ins[k].op == OP_ELSE)) continue;
            if (ins[k].depth == D && ins[k].op == OP_IF) j = k;
            break;
        }
        if (j == SIZE_MAX) return after_mad;
        std::vector<size_t> cuts{j};   // the if, its else, its endif
        size_t end = SIZE_MAX;
        for (size_t k = j + 1; k < ins.size(); ++k) {
            if (ins[k].depth > D) continue;
            if (ins[k].depth == D && ins[k].op == OP_ELSE) { cuts.push_back(k); continue; }
            if (ins[k].depth == D && ins[k].op == OP_ENDIF) end = k;
            break;
        }
        if (end == SIZE_MAX || cuts.size() != 2 || !dominates(ins, s, end)) return after_mad;
        cuts.push_back(end);
        for (size_t c = 0; c + 1 < cuts.size(); ++c)
            if (!writes(cuts[c] + 1, cuts[c + 1], reg, msk)) return after_mad;
        return ins[end].at + ins[end].len;
    };

    // What goes where, in program order; put in from the back so the positions stay right.
    std::vector<std::pair<size_t, Toks>> adds;
    if (extras) {
        const Instr& smp = ins[s];
        Emit e;
        // rC.xyz = the cage coordinate (the sample's own operand)
        Toks coord(t.begin() + static_cast<std::ptrdiff_t>(smp.ops[1].at),
                   t.begin() + static_cast<std::ptrdiff_t>(smp.ops[1].at + smp.ops[1].len));
        e.op(OP_MOV, {dst(R.C, XYZ), coord});
        // rH.xy = the cage there, impact and scrape: the same sample into rH.xy, with the resource swizzle as is
        Toks copy(t.begin() + static_cast<std::ptrdiff_t>(smp.at), t.begin() + static_cast<std::ptrdiff_t>(smp.at + smp.len));
        const size_t od = smp.ops[0].at - smp.at, orr = smp.ops[2].at - smp.at;
        copy[od]     = 0x00100002u | (3u << 4);   // rH.xy
        copy[od + 1] = R.H;
        copy[orr]    = (copy[orr] & ~0xFF0u) | (SXYZW << 4);
        e.t.insert(e.t.end(), copy.begin(), copy.end());
        // x g_HoodMorphDamageScalar / g_HoodScrapeDamageScalar, clamped, as the game does
        e.op(OP_MUL, {dst(R.H, 3), src(R.H, sw(X, Y, X, X)), cb(4, 38, sw(X, Y, X, X))}, true);
        if (cb_size[11] >= 1) {
            // none on the parts that turn (cb11[0].x >= 0: the shader rotates them, and gives them no impact damage)
            e.op(OP_IGE, {dst(R.T, MX), cb1(11, 0, X), imm(0.0f)});
            e.op(OP_MOVC, {dst(R.H, 3), src(R.T, SXXXX), imm(0.0f, 0.0f, 0.0f, 0.0f), src(R.H, sw(X, Y, X, X))});
        }
        e.op(OP_MUL, {dst(R.H, MX), one(R.H, X), imm(P.depth)});
        adds.push_back({smp.at, std::move(e.t)});
    }
    for (const Mad& m : mads) {
        const Instr& in = ins[m.i];
        Emit before;
        before.op(OP_MUL, {dst(R.K, MX), one(m.reg, X), imm(P.depth)});
        adds.push_back({in.at, std::move(before.t)});
        t[in.ops[1].at + 1] = R.K;   // the mad reads rK.x
        if (!extras || (!m.position && P.crumple == 0.0f)) continue;

        // the mad's result: its destination as it is, and the same register with every component as a source
        const uint32_t res = in.ops[0].index0, msk = (in.ops[0].token >> 4) & 0xF;
        // Right after the mad, as in 0.3.0 (tested good). Pushing after the branches join (push_point) was tried in
        // 0.4.2-0.6.1, and with it a crashed car's paint went black: off until that's understood.
        const size_t where = (P.join_push && m.position) ? push_point(m.i) : in.at + in.len;
        const uint32_t dmg = P.join_push ? R.H : R.K;   // the damage the push goes by
        const uint32_t swz = (in.ops[3].token >> 4) & 0xFF;   // which of v0's / v1's components go where
        Emit e;
        car_pos(e, R);
        crumple_args(e, R);
        if (m.position) {
            // push = -(rH.x * (dent + crumple * n) + scrape * rH.y), toward the car's middle
            e.op(OP_SINCOS, {dst(R.E, XYZ), null_dst(), src(R.E, SXYZX)});
            e.op(OP_DP3, {dst(R.E, MX), src(R.E, SXYZX), imm(1.0f / 3, 1.0f / 3, 1.0f / 3, 0.0f)});
            e.op(OP_MAD, {dst(R.E, MX), one(R.E, X), imm(-P.crumple), imm(-P.dent)});
            e.op(OP_MUL, {dst(R.E, MX), one(R.E, X), one(dmg, X)});   // the damage there x depth
            e.op(OP_MAD, {dst(R.E, MX), one(R.H, Y), imm(-P.scrape), one(R.E, X)});
            e.op(OP_DP3, {dst(R.E, MY), src(R.D, SXYZX), src(R.D, SXYZX)});
            e.op(OP_MAX, {dst(R.E, MY), one(R.E, Y), imm(1e-6f)});
            e.op(OP_RSQ, {dst(R.E, MY), one(R.E, Y)});
            e.op(OP_MUL, {dst(R.E, MX), one(R.E, X), one(R.E, Y)});
            e.op(OP_MUL, {dst(R.D, XYZ), src(R.D, SXYZX), src(R.E, SXXXX)});
        } else {
            // the normal tilts by the push's slope: rH.x * crumple * grad n
            e.op(OP_SINCOS, {null_dst(), dst(R.E, XYZ), src(R.E, SXYZX)});
            e.op(OP_MUL, {dst(R.D, XYZ), src(R.E, SXXXX), imm(kF[0], kG[0], 0.0f, 0.0f)});
            e.op(OP_MAD, {dst(R.D, XYZ), src(R.E, SYYYY), imm(0.0f, kF[1], kG[1], 0.0f), src(R.D, SXYZX)});
            e.op(OP_MAD, {dst(R.D, XYZ), src(R.E, SZZZZ), imm(kG[2], 0.0f, kF[2], 0.0f), src(R.D, SXYZX)});
            e.op(OP_MUL, {dst(R.D, XYZ), src(R.D, SXYZX), src(dmg, SXXXX)});
            e.op(OP_MUL, {dst(R.D, XYZ), src(R.D, SXYZX), imm(P.crumple / 3.0f, P.crumple / 3.0f, P.crumple / 3.0f, 0.0f)});
        }
        car_to_part(e, R);
        e.op(OP_ADD, {dst(res, msk), src(res, SXYZW), src(R.D, swz)});
        bool dup = false;   // two position mads joining at the same place: push once
        for (const auto& a : adds) dup = dup || (a.first == where && m.position && where != in.at + in.len);
        if (!dup) adds.push_back({where, std::move(e.t)});
    }
    std::stable_sort(adds.begin(), adds.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // Popping glass: before the last ret, "if (glass_hide < damage) position = 0" (the triangles collapse).
    if (extras && P.glass_hide != 0.0f && ins[s].depth == 0) {
        size_t ret = SIZE_MAX;
        uint32_t pos = UINT32_MAX;
        for (size_t i = 0; i < ins.size(); ++i) {
            if (ins[i].op == OP_RET && ins[i].depth == 0) ret = i;
            if (ins[i].op == OP_DCL_OUTPUT_SIV && ins[i].len == 4 && t[ins[i].at + 3] == 1) pos = t[ins[i].at + 2];
        }
        if (ret != SIZE_MAX && pos != UINT32_MAX) {
            Emit e;
            e.op(OP_LT, {dst(R.E, MX), imm(P.glass_hide), one(R.H, X)});
            e.t.push_back(OP_IF | OP_IFNZ_BIT | (3u << 24));
            for (uint32_t v : one(R.E, X)) e.t.push_back(v);
            e.op(OP_MOV, {odst(pos, 0xF), imm(0.0f, 0.0f, 0.0f, 0.0f)});
            e.t.push_back(OP_ENDIF | (1u << 24));
            adds.push_back({ins[ret].at, std::move(e.t)});
            inf.glass = true;
        }
    }
    std::stable_sort(adds.begin(), adds.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t grow_tokens = 0;
    for (size_t n = adds.size(); n-- > 0;) {
        t.insert(t.begin() + static_cast<std::ptrdiff_t>(adds[n].first), adds[n].second.begin(), adds[n].second.end());
        grow_tokens += adds[n].second.size();
    }
    t[1] = static_cast<uint32_t>(t.size());
    inf.extras = extras;

    // the container around the longer SHEX
    const size_t grow = grow_tokens * 4;
    std::vector<unsigned char> out;
    out.reserve(d.size() + grow);
    out.insert(out.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8));
    for (uint32_t v : t) {
        out.push_back(static_cast<unsigned char>(v));
        out.push_back(static_cast<unsigned char>(v >> 8));
        out.push_back(static_cast<unsigned char>(v >> 16));
        out.push_back(static_cast<unsigned char>(v >> 24));
    }
    out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8 + shex_size), d.end());
    wr(out, shex + 4, static_cast<uint32_t>(shex_size + grow));
    wr(out, 24, static_cast<uint32_t>(out.size()));
    for (uint32_t c = 0; c < nchunks; ++c) {
        const uint32_t o = rd(out, 32 + 4 * c);
        if (o > shex) wr(out, 32 + 4 * c, static_cast<uint32_t>(o + grow));
    }
    uint32_t h[4];
    dxbc_checksum(out.data(), out.size(), h);
    std::memcpy(out.data() + 4, h, 16);
    d.swap(out);
    return MorphPatch::Patched;
}

bool patch_glass_pixel_shader(std::vector<unsigned char>& d, bool always) {
    if (d.size() < 32 || std::memcmp(d.data(), "DXBC", 4) != 0 || rd(d, 24) != d.size()) return false;
    const uint32_t nchunks = rd(d, 28);
    if (32 + 4ull * nchunks > d.size()) return false;
    size_t shex = 0, shex_size = 0;
    for (uint32_t c = 0; c < nchunks; ++c) {
        const size_t at = rd(d, 32 + 4 * c);
        if (at + 8 > d.size()) return false;
        if (std::memcmp(&d[at], "SHEX", 4) == 0 || std::memcmp(&d[at], "SHDR", 4) == 0) {
            shex = at;
            shex_size = rd(d, at + 4);
        }
    }
    if (!shex || shex + 8 + shex_size > d.size() || shex_size < 8 || (shex_size & 3)) return false;
    std::vector<uint32_t> t(shex_size / 4);
    for (size_t i = 0; i < t.size(); ++i) t[i] = rd(d, shex + 8 + 4 * i);
    if (t[1] != t.size() || ((t[0] >> 16) & 0xFFFF) != 0 /* pixel shader */) return false;
    std::vector<Instr> ins;
    if (!decode(t, ins)) return false;
    // the cracks are drawn inside "if (cb4[45].w > 0)": find that test, and discard the pixel instead
    uint32_t cbslot = UINT32_MAX, cbidx = 0;
    size_t dcl = SIZE_MAX;
    for (const Instr& in : ins)
        if (in.op == OP_DCL_TEMPS && in.len == 2) dcl = in.at;
    if (dcl == SIZE_MAX) return false;
    for (size_t i = 0; i + 1 < ins.size(); ++i) {
        const Instr& m = ins[i];
        if (m.op != OP_LT || m.len != 8 || ins[i + 1].op != OP_IF) continue;   // lt r.w, l(0), cb4[45].w
        if (t[m.at + 3] != 0x00004001u || t[m.at + 4] != 0 /* 0.0f */) continue;
        if ((t[m.at + 5] & 0x000FF000u) != (T_CONSTANT_BUFFER << 12)) continue;
        cbslot = t[m.at + 6];
        cbidx = t[m.at + 7];
        break;
    }
    if (cbslot != 4 || cbidx != 45) return false;
    const uint32_t reg = t[dcl + 1];
    t[dcl + 1] = reg + 1;
    Emit e;
    e.op(OP_LT, {dst(reg, 1u), imm(0.0f), cb1(cbslot, cbidx, 3u)});
    if (!always) {
        e.t.push_back(OP_DISCARD | OP_IFNZ_BIT | (3u << 24));
        for (uint32_t v : one(reg, 0u)) e.t.push_back(v);
    }
    const size_t where = ins[dcl].at + ins[dcl].len;
    size_t extra_tokens = 0;
    if (always) {   // test: also paint the glass magenta, to see it
        size_t ret = SIZE_MAX;
        for (const Instr& in : ins)
            if (in.op == OP_RET && in.depth == 0) ret = in.at;
        if (ret != SIZE_MAX) {
            Emit m;
            m.t.push_back(OP_IF | OP_IFNZ_BIT | (3u << 24));
            for (uint32_t v : one(reg, 0u)) m.t.push_back(v);
            m.op(OP_MOV, {odst(0, 0xF), imm(8.0f, 0.0f, 8.0f, 1.0f)});
            m.t.push_back(OP_ENDIF | (1u << 24));
            t.insert(t.begin() + static_cast<std::ptrdiff_t>(ret), m.t.begin(), m.t.end());
            extra_tokens = m.t.size();
        }
    }
    t.insert(t.begin() + static_cast<std::ptrdiff_t>(where), e.t.begin(), e.t.end());
    t[1] = static_cast<uint32_t>(t.size());
    const size_t grow = (e.t.size() + extra_tokens) * 4;
    std::vector<unsigned char> out;
    out.reserve(d.size() + grow);
    out.insert(out.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8));
    for (uint32_t v : t)
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<unsigned char>(v >> (8 * b)));
    out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8 + shex_size), d.end());
    wr(out, shex + 4, static_cast<uint32_t>(shex_size + grow));
    wr(out, 24, static_cast<uint32_t>(out.size()));
    for (uint32_t c = 0; c < nchunks; ++c) {
        const uint32_t o = rd(out, 32 + 4 * c);
        if (o > shex) wr(out, 32 + 4 * c, static_cast<uint32_t>(o + grow));
    }
    uint32_t h[4];
    dxbc_checksum(out.data(), out.size(), h);
    std::memcpy(out.data() + 4, h, 16);
    d.swap(out);
    return true;
}

bool patch_hide_vertex_shader(std::vector<unsigned char>& d) {
    if (d.size() < 32 || std::memcmp(d.data(), "DXBC", 4) != 0 || rd(d, 24) != d.size()) return false;
    const uint32_t nchunks = rd(d, 28);
    if (32 + 4ull * nchunks > d.size()) return false;
    size_t shex = 0, shex_size = 0;
    for (uint32_t c = 0; c < nchunks; ++c) {
        const size_t at = rd(d, 32 + 4 * c);
        if (at + 8 > d.size()) return false;
        if (std::memcmp(&d[at], "SHEX", 4) == 0 || std::memcmp(&d[at], "SHDR", 4) == 0) {
            shex = at;
            shex_size = rd(d, at + 4);
        }
    }
    if (!shex || shex + 8 + shex_size > d.size() || shex_size < 8 || (shex_size & 3)) return false;
    std::vector<uint32_t> t(shex_size / 4);
    for (size_t i = 0; i < t.size(); ++i) t[i] = rd(d, shex + 8 + 4 * i);
    if (t[1] != t.size() || ((t[0] >> 16) & 0xFFFF) != 1 /* vertex shader */) return false;
    std::vector<Instr> ins;
    if (!decode(t, ins)) return false;
    size_t ret = SIZE_MAX;
    uint32_t pos = UINT32_MAX;
    for (const Instr& in : ins) {
        if (in.op == OP_RET && in.depth == 0) ret = in.at;
        if (in.op == OP_DCL_OUTPUT_SIV && in.len == 4 && t[in.at + 3] == 1) pos = t[in.at + 2];
    }
    if (ret == SIZE_MAX || pos == UINT32_MAX) return false;
    Emit e;
    e.op(OP_MOV, {odst(pos, 0xF), imm(0.0f, 0.0f, 0.0f, 0.0f)});
    t.insert(t.begin() + static_cast<std::ptrdiff_t>(ret), e.t.begin(), e.t.end());
    t[1] = static_cast<uint32_t>(t.size());
    const size_t grow = e.t.size() * 4;
    std::vector<unsigned char> out;
    out.reserve(d.size() + grow);
    out.insert(out.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8));
    for (uint32_t v : t)
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<unsigned char>(v >> (8 * b)));
    out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(shex + 8 + shex_size), d.end());
    wr(out, shex + 4, static_cast<uint32_t>(shex_size + grow));
    wr(out, 24, static_cast<uint32_t>(out.size()));
    for (uint32_t c = 0; c < nchunks; ++c) {
        const uint32_t o = rd(out, 32 + 4 * c);
        if (o > shex) wr(out, 32 + 4 * c, static_cast<uint32_t>(o + grow));
    }
    uint32_t hh[4];
    dxbc_checksum(out.data(), out.size(), hh);
    std::memcpy(out.data() + 4, hh, 16);
    d.swap(out);
    return true;
}
