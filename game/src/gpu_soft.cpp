// Software GE renderer: vertex decoding, transform/lighting, clipping and
// rasterization straight into guest VRAM, so render-to-texture, CPU reads of
// the framebuffer and block transfers all observe the same memory.
#include "gpu_state.hpp"

#include "kernel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace p2::gpu {
namespace {

// P2_GE_TRACE=<vblank>[-<vblank>]: log every draw issued in that range to build/ge_trace.txt.
std::FILE *trace_file() {
    static long first = -1, last = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *e = std::getenv("P2_GE_TRACE")) {
            first = std::atol(e);
            const char *dash = std::strchr(e, '-');
            last = dash != nullptr ? std::atol(dash + 1) : first;
        }
    }
    static std::FILE *f = nullptr;
    const long now = static_cast<long>(g_kernel.vblank_count());
    if (first < 0 || now < first || now > last) {
        if (f != nullptr && now > last) { std::fclose(f); f = nullptr; first = -1; }
        return nullptr;
    }
    if (f == nullptr) {
        f = std::fopen("build/ge_trace.txt", "w");
        // Runs usually end with --exit-after, which does not flush stdio.
        if (f != nullptr) std::setvbuf(f, nullptr, _IONBF, 0);
    }
    static long last_frame = -1;
    if (f != nullptr && now != last_frame) { std::fprintf(f, "=== vblank %ld\n", now); last_frame = now; }
    return f;
}

struct VertexLayout {
    u32 size{};
    u32 tc_off{}, col_off{}, nrm_off{}, pos_off{}, weight_off{};
    u32 tc{}, col{}, nrm{}, pos{}, weight{}, weights{}, morphs{}, index{};
    bool through{};
};

VertexLayout vertex_layout(u32 vtype) {
    static constexpr u32 tc_size[4] = {0, 1, 2, 4};
    static constexpr u32 col_size[8] = {0, 0, 0, 0, 2, 2, 2, 4};
    static constexpr u32 nrm_size[4] = {0, 1, 2, 4};
    static constexpr u32 pos_size[4] = {4, 1, 2, 4};
    static constexpr u32 w_size[4] = {0, 1, 2, 4};
    VertexLayout l;
    l.tc = vtype & 3u;
    l.col = (vtype >> 2) & 7u;
    l.nrm = (vtype >> 5) & 3u;
    l.pos = (vtype >> 7) & 3u;
    l.weight = (vtype >> 9) & 3u;
    l.index = (vtype >> 11) & 3u;
    l.weights = ((vtype >> 14) & 7u) + 1u;
    l.morphs = ((vtype >> 18) & 7u) + 1u;
    l.through = (vtype & 0x00800000u) != 0u;
    u32 offset = 0u, biggest = 1u;
    auto place = [&](u32 component_size, u32 count) {
        if (component_size == 0u) return offset;
        offset = (offset + component_size - 1u) & ~(component_size - 1u);
        const u32 at = offset;
        offset += component_size * count;
        biggest = std::max(biggest, component_size);
        return at;
    };
    if (l.weight != 0u) l.weight_off = place(w_size[l.weight], l.weights);
    if (l.tc != 0u) l.tc_off = place(tc_size[l.tc], 2u);
    if (l.col != 0u) l.col_off = place(col_size[l.col], 1u);
    if (l.nrm != 0u) l.nrm_off = place(nrm_size[l.nrm], 3u);
    l.pos_off = place(pos_size[l.pos], 3u);
    l.size = (offset + biggest - 1u) & ~(biggest - 1u);
    return l;
}

struct Vec4 { float x{}, y{}, z{}, w{}; };

// Decoded vertex before transform.
struct RawVertex {
    float pos[3]{};
    float uv[2]{};
    float color[4]{1, 1, 1, 1};
    float normal[3]{};
    float weights[8]{};
    bool has_color{};
};

// Vertex after transform, ready to rasterize.
struct ScreenVertex {
    float x{}, y{};      // pixels
    float z{};           // 0..65535
    float invw{1.0f};    // 1/w for perspective correction (1 in through mode)
    float u{}, v{};      // texture coordinates (texels in through mode, normalized otherwise), pre-divided by w
    float color[4]{};    // 0..1
    float fog{1.0f};
    Vec4 clip{};         // clip-space position (3D only)
};

const u8 *ptr(u32 addr, u32 len) { return mem().raw_pointer(addr, len); }

template <typename T>
T rd(const u8 *p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

void decode_color(u32 fmt, const u8 *p, float out[4]) {
    switch (fmt) {
    case 4: { const u16 c = rd<u16>(p); out[0] = (c & 31) / 31.f; out[1] = ((c >> 5) & 63) / 63.f; out[2] = ((c >> 11) & 31) / 31.f; out[3] = 1.f; break; }
    case 5: { const u16 c = rd<u16>(p); out[0] = (c & 31) / 31.f; out[1] = ((c >> 5) & 31) / 31.f; out[2] = ((c >> 10) & 31) / 31.f; out[3] = (c >> 15) ? 1.f : 0.f; break; }
    case 6: { const u16 c = rd<u16>(p); out[0] = (c & 15) / 15.f; out[1] = ((c >> 4) & 15) / 15.f; out[2] = ((c >> 8) & 15) / 15.f; out[3] = ((c >> 12) & 15) / 15.f; break; }
    case 7: out[0] = p[0] / 255.f; out[1] = p[1] / 255.f; out[2] = p[2] / 255.f; out[3] = p[3] / 255.f; break;
    default: break;
    }
}

void decode_vertex(const State &s, const VertexLayout &l, u32 addr, RawVertex &v) {
    const u32 stride = l.size;
    v = RawVertex{};
    for (u32 m = 0; m < l.morphs; ++m) {
        const u8 *p = ptr(addr + m * stride, stride);
        if (p == nullptr) return;
        const float w = l.morphs > 1u ? s.morph[m] : 1.0f;
        if (l.weight != 0u) {
            for (u32 i = 0; i < l.weights; ++i) {
                float x = 0;
                if (l.weight == 1u) x = p[l.weight_off + i] / 128.f;
                else if (l.weight == 2u) x = rd<u16>(p + l.weight_off + i * 2) / 32768.f;
                else x = rd<float>(p + l.weight_off + i * 4);
                v.weights[i] += x * w;
            }
        }
        if (l.tc != 0u) {
            float uv[2];
            for (int i = 0; i < 2; ++i) {
                if (l.tc == 1u) uv[i] = l.through ? p[l.tc_off + i] : p[l.tc_off + i] / 128.f;
                else if (l.tc == 2u) uv[i] = l.through ? rd<u16>(p + l.tc_off + i * 2) : rd<u16>(p + l.tc_off + i * 2) / 32768.f;
                else uv[i] = rd<float>(p + l.tc_off + i * 4);
            }
            v.uv[0] += uv[0] * w;
            v.uv[1] += uv[1] * w;
        }
        if (l.col != 0u) {
            float c[4]{};
            decode_color(l.col, p + l.col_off, c);
            for (int i = 0; i < 4; ++i) v.color[i] = (m == 0 ? 0.f : v.color[i]) + c[i] * w;
            v.has_color = true;
        }
        if (l.nrm != 0u) {
            for (int i = 0; i < 3; ++i) {
                float n;
                if (l.nrm == 1u) n = static_cast<s8>(p[l.nrm_off + i]) / 127.f;
                else if (l.nrm == 2u) n = rd<s16>(p + l.nrm_off + i * 2) / 32767.f;
                else n = rd<float>(p + l.nrm_off + i * 4);
                v.normal[i] += n * w;
            }
        }
        for (int i = 0; i < 3; ++i) {
            float x;
            if (l.pos == 1u) x = l.through ? static_cast<float>(static_cast<s8>(p[l.pos_off + i])) : static_cast<s8>(p[l.pos_off + i]) / 127.f;
            else if (l.pos == 2u) {
                // Through-mode z is unsigned 16-bit; x/y signed.
                if (l.through) x = i == 2 ? static_cast<float>(rd<u16>(p + l.pos_off + i * 2)) : static_cast<float>(rd<s16>(p + l.pos_off + i * 2));
                else x = rd<s16>(p + l.pos_off + i * 2) / 32767.f;
            } else x = rd<float>(p + l.pos_off + i * 4);
            v.pos[i] += x * w;
        }
    }
}

void mul43(const float m[12], const float in[3], float out[3], float w = 1.0f) {
    out[0] = m[0] * in[0] + m[3] * in[1] + m[6] * in[2] + m[9] * w;
    out[1] = m[1] * in[0] + m[4] * in[1] + m[7] * in[2] + m[10] * w;
    out[2] = m[2] * in[0] + m[5] * in[1] + m[8] * in[2] + m[11] * w;
}

void color_from_reg(u32 rgb, float out[3]) {
    out[0] = (rgb & 0xFF) / 255.f;
    out[1] = ((rgb >> 8) & 0xFF) / 255.f;
    out[2] = ((rgb >> 16) & 0xFF) / 255.f;
}

float clamp01(float f) { return f < 0.f ? 0.f : (f > 1.f ? 1.f : f); }

void light_vertex(const State &s, const float world_pos[3], const float world_normal[3], const RawVertex &raw, float out[4]) {
    const u32 update = s.reg[MATERIALUPDATE];
    float mat_amb[4], mat_diff[3], mat_spec[3], emissive[3];
    color_from_reg(s.reg[MATERIALAMBIENT], mat_amb);
    mat_amb[3] = (s.reg[MATERIALALPHA] & 0xFF) / 255.f;
    color_from_reg(s.reg[MATERIALDIFFUSE], mat_diff);
    color_from_reg(s.reg[MATERIALSPECULAR], mat_spec);
    color_from_reg(s.reg[MATERIALEMISSIVE], emissive);
    if (raw.has_color) {
        if (update & 1u) { for (int i = 0; i < 3; ++i) mat_amb[i] = raw.color[i]; mat_amb[3] = raw.color[3]; }
        if (update & 2u) for (int i = 0; i < 3; ++i) mat_diff[i] = raw.color[i];
        if (update & 4u) for (int i = 0; i < 3; ++i) mat_spec[i] = raw.color[i];
    }
    float amb_light[3];
    color_from_reg(s.reg[AMBIENTCOLOR], amb_light);
    const float amb_alpha = (s.reg[AMBIENTALPHA] & 0xFF) / 255.f;
    float c[3], spec[3]{};
    for (int i = 0; i < 3; ++i) c[i] = emissive[i] + amb_light[i] * mat_amb[i];
    const float alpha = amb_alpha * mat_amb[3];

    float n[3] = {world_normal[0], world_normal[1], world_normal[2]};
    if (s.reg[REVERSENORMAL] & 1u) for (float &x : n) x = -x;
    const float nlen = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nlen > 0.f) for (float &x : n) x /= nlen;
    const float shininess = float24(s.reg[MATERIALSPECULARCOEF]);

    for (int li = 0; li < 4; ++li) {
        if ((s.reg[LIGHTENABLE0 + li] & 1u) == 0u) continue;
        const u32 type_reg = s.reg[LIGHTTYPE0 + li];
        const u32 comp = type_reg & 3u, kind = (type_reg >> 8) & 3u;
        float lpos[3], ldir[3];
        for (int i = 0; i < 3; ++i) {
            lpos[i] = float24(s.reg[LX0 + li * 3 + i]);
            ldir[i] = float24(s.reg[LDX0 + li * 3 + i]);
        }
        float L[3];
        float atten = 1.f;
        if (kind == 0u) {
            for (int i = 0; i < 3; ++i) L[i] = lpos[i];
        } else {
            for (int i = 0; i < 3; ++i) L[i] = lpos[i] - world_pos[i];
            const float d = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            const float k0 = float24(s.reg[LKA0 + li * 3]), k1 = float24(s.reg[LKA0 + li * 3 + 1]), k2 = float24(s.reg[LKA0 + li * 3 + 2]);
            const float den = k0 + k1 * d + k2 * d * d;
            atten = den > 0.f ? clamp01(1.f / den) : 1.f;
        }
        const float llen = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (llen > 0.f) for (float &x : L) x /= llen;
        if (kind == 2u) {
            float d[3] = {ldir[0], ldir[1], ldir[2]};
            const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dl > 0.f) for (float &x : d) x /= dl;
            const float cosang = -(L[0] * d[0] + L[1] * d[1] + L[2] * d[2]);
            const float cutoff = float24(s.reg[LKO0 + li]);
            if (cosang < cutoff) atten = 0.f;
            else atten *= std::pow(std::max(cosang, 0.f), float24(s.reg[LKS0 + li]));
        }
        float lamb[3], ldiff[3], lspec[3];
        color_from_reg(s.reg[LAC0 + li * 3], lamb);
        color_from_reg(s.reg[LAC0 + li * 3 + 1], ldiff);
        color_from_reg(s.reg[LAC0 + li * 3 + 2], lspec);
        float ndotl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (comp == 2u) ndotl = ndotl > 0.f ? std::pow(ndotl, shininess) : 0.f;
        ndotl = std::max(ndotl, 0.f);
        for (int i = 0; i < 3; ++i) c[i] += atten * (lamb[i] * mat_amb[i] + ldiff[i] * mat_diff[i] * ndotl);
        if (comp == 1u && ndotl > 0.f) {
            float h[3] = {L[0], L[1], L[2] + 1.f};
            const float hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            if (hl > 0.f) for (float &x : h) x /= hl;
            const float ndoth = std::max(n[0] * h[0] + n[1] * h[1] + n[2] * h[2], 0.f);
            const float sp = std::pow(ndoth, shininess) * atten;
            for (int i = 0; i < 3; ++i) spec[i] += sp * lspec[i] * mat_spec[i];
        }
    }
    const bool separate_specular = (s.reg[LIGHTMODE] & 1u) != 0u;
    for (int i = 0; i < 3; ++i) out[i] = clamp01(c[i] + (separate_specular ? spec[i] : spec[i]));
    out[3] = clamp01(alpha);
}

ScreenVertex transform(const State &s, const VertexLayout &l, const RawVertex &raw) {
    ScreenVertex out;
    float prim[4];
    if (raw.has_color) for (int i = 0; i < 4; ++i) prim[i] = raw.color[i];
    else {
        color_from_reg(s.reg[MATERIALAMBIENT], prim);
        prim[3] = (s.reg[MATERIALALPHA] & 0xFF) / 255.f;
    }
    if (l.through) {
        out.x = raw.pos[0];
        out.y = raw.pos[1];
        out.z = raw.pos[2];
        out.invw = 1.f;
        out.u = raw.uv[0];
        out.v = raw.uv[1];
        for (int i = 0; i < 4; ++i) out.color[i] = prim[i];
        return out;
    }
    float pos[3] = {raw.pos[0], raw.pos[1], raw.pos[2]};
    float nrm[3] = {raw.normal[0], raw.normal[1], raw.normal[2]};
    if (l.weight != 0u) {
        float p2[3]{}, n2[3]{};
        for (u32 b = 0; b < l.weights; ++b) {
            if (raw.weights[b] == 0.f) continue;
            float tp[3], tn[3];
            mul43(&s.bone[b * 12], pos, tp);
            mul43(&s.bone[b * 12], nrm, tn, 0.f);
            for (int i = 0; i < 3; ++i) { p2[i] += tp[i] * raw.weights[b]; n2[i] += tn[i] * raw.weights[b]; }
        }
        std::copy(p2, p2 + 3, pos);
        std::copy(n2, n2 + 3, nrm);
    }
    float world_pos[3], world_nrm[3], view_pos[3];
    mul43(s.world, pos, world_pos);
    mul43(s.world, nrm, world_nrm, 0.f);
    mul43(s.view, world_pos, view_pos);
    const float *P = s.proj;
    out.clip.x = P[0] * view_pos[0] + P[4] * view_pos[1] + P[8] * view_pos[2] + P[12];
    out.clip.y = P[1] * view_pos[0] + P[5] * view_pos[1] + P[9] * view_pos[2] + P[13];
    out.clip.z = P[2] * view_pos[0] + P[6] * view_pos[1] + P[10] * view_pos[2] + P[14];
    out.clip.w = P[3] * view_pos[0] + P[7] * view_pos[1] + P[11] * view_pos[2] + P[15];

    if ((s.reg[LIGHTINGENABLE] & 1u) != 0u) light_vertex(s, world_pos, world_nrm, raw, out.color);
    else for (int i = 0; i < 4; ++i) out.color[i] = prim[i];

    // Texture coordinates.
    const u32 map_mode = s.reg[TEXMAPMODE] & 3u;
    if (map_mode == 0u) {
        out.u = raw.uv[0] * float24(s.reg[TEXSCALEU]) + float24(s.reg[TEXOFFSETU]);
        out.v = raw.uv[1] * float24(s.reg[TEXSCALEV]) + float24(s.reg[TEXOFFSETV]);
    } else if (map_mode == 1u) {
        const u32 src = (s.reg[TEXMAPMODE] >> 8) & 3u;
        float in[3];
        if (src == 0u) std::copy(pos, pos + 3, in);
        else if (src == 1u) { in[0] = raw.uv[0]; in[1] = raw.uv[1]; in[2] = 0.f; }
        else std::copy(nrm, nrm + 3, in);
        float t[3];
        mul43(s.tgen, in, t);
        out.u = t[2] != 0.f ? t[0] / t[2] : t[0];
        out.v = t[2] != 0.f ? t[1] / t[2] : t[1];
    } else {
        // Environment (shade) mapping from the light directions.
        const int l0 = s.reg[TEXSHADELS] & 3, l1 = (s.reg[TEXSHADELS] >> 8) & 3;
        auto dot_light = [&](int li) {
            float L[3] = {float24(s.reg[LX0 + li * 3]), float24(s.reg[LX0 + li * 3 + 1]), float24(s.reg[LX0 + li * 3 + 2])};
            const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            float n[3] = {world_nrm[0], world_nrm[1], world_nrm[2]};
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (ll == 0.f || nl == 0.f) return 0.5f;
            return ((n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / (ll * nl) + 1.f) * 0.5f;
        };
        out.u = dot_light(l0);
        out.v = dot_light(l1);
    }

    if ((s.reg[FOGENABLE] & 1u) != 0u) {
        out.fog = clamp01((view_pos[2] + float24(s.reg[FOG1])) * float24(s.reg[FOG2]));
    }
    return out;
}

// Projects a clip-space vertex to the screen.
void project(const State &s, ScreenVertex &v) {
    const float w = v.clip.w;
    const float invw = w != 0.f ? 1.f / w : 1.f;
    const float sx = float24(s.reg[VIEWPORTXSCALE]) * v.clip.x * invw + float24(s.reg[VIEWPORTXCENTER]);
    const float sy = float24(s.reg[VIEWPORTYSCALE]) * v.clip.y * invw + float24(s.reg[VIEWPORTYCENTER]);
    const float sz = float24(s.reg[VIEWPORTZSCALE]) * v.clip.z * invw + float24(s.reg[VIEWPORTZCENTER]);
    const float ox = static_cast<float>(s.reg[OFFSETX] & 0xFFFFu) / 16.f;
    const float oy = static_cast<float>(s.reg[OFFSETY] & 0xFFFFu) / 16.f;
    v.x = sx - ox;
    v.y = sy - oy;
    v.z = sz;
    v.invw = invw;
    v.u *= invw;
    v.v *= invw;
}

// --- textures ------------------------------------------------------------------

struct Rgba { u8 r, g, b, a; };

Rgba unpack16(u32 fmt, u16 c) {
    switch (fmt) {
    case 0: { const u8 r = (c & 31), g = (c >> 5) & 63, b = (c >> 11) & 31;
              return {static_cast<u8>((r << 3) | (r >> 2)), static_cast<u8>((g << 2) | (g >> 4)), static_cast<u8>((b << 3) | (b >> 2)), 255}; }
    case 1: { const u8 r = (c & 31), g = (c >> 5) & 31, b = (c >> 10) & 31;
              return {static_cast<u8>((r << 3) | (r >> 2)), static_cast<u8>((g << 3) | (g >> 2)), static_cast<u8>((b << 3) | (b >> 2)), static_cast<u8>((c >> 15) ? 255 : 0)}; }
    default: return {static_cast<u8>((c & 15) * 17), static_cast<u8>(((c >> 4) & 15) * 17), static_cast<u8>(((c >> 8) & 15) * 17), static_cast<u8>(((c >> 12) & 15) * 17)};
    }
}

Rgba clut_lookup(const State &s, u32 index) {
    const u32 cf = s.reg[CLUTFORMAT];
    const u32 fmt = cf & 3u, shift = (cf >> 2) & 31u, mask = (cf >> 8) & 0xFFu, start = ((cf >> 16) & 31u) * 16u;
    const u32 i = (((index >> shift) & mask) | start) & 0x1FFu;
    if (fmt == 3u) {
        const u32 off = (i * 4u) & 1023u;
        return {s.clut[off], s.clut[off + 1], s.clut[off + 2], s.clut[off + 3]};
    }
    const u32 off = (i * 2u) & 1023u;
    return unpack16(fmt, static_cast<u16>(s.clut[off] | (s.clut[off + 1] << 8)));
}

struct TexInfo {
    u32 addr{}, bufw{}, w{}, h{}, fmt{};
    bool swizzled{};
    const u8 *base{};
    u32 span{};
    const u32 *palette{}; // decoded CLUT for 4/8-bit formats
    bool clamp_u{}, clamp_v{};
};

inline Rgba unpack_rgba(u32 c) { return {static_cast<u8>(c), static_cast<u8>(c >> 8), static_cast<u8>(c >> 16), static_cast<u8>(c >> 24)}; }

void ensure_palette(State &s) {
    const u32 fmt = s.reg[CLUTFORMAT];
    if (!s.palette_dirty && s.palette_format == fmt) return;
    for (u32 i = 0; i < 256u; ++i) {
        const Rgba c = clut_lookup(s, i);
        s.palette[i] = c.r | (c.g << 8) | (c.b << 16) | (static_cast<u32>(c.a) << 24);
    }
    s.palette_format = fmt;
    s.palette_dirty = false;
}

// Byte address of texel (x,y) for a format with `bits` per texel.
inline u32 texel_offset(const TexInfo &t, u32 x, u32 y, u32 bits) {
    if (!t.swizzled) return (y * t.bufw + x) * bits / 8u;
    // Swizzled: 16-byte x 8-row blocks.
    const u32 row_bytes = t.bufw * bits / 8u;
    const u32 xb = x * bits / 8u;
    const u32 block = (y / 8u) * (row_bytes * 8u) + (xb / 16u) * 128u;
    return block + (y % 8u) * 16u + (xb % 16u);
}

Rgba decode_dxt(const TexInfo &t, u32 x, u32 y) {
    const u32 block_bytes = t.fmt == 8u ? 8u : 16u;
    const u32 blocks_per_row = t.bufw / 4u;
    const u32 off = ((y / 4u) * blocks_per_row + x / 4u) * block_bytes;
    if (off + block_bytes > t.span) return {0, 0, 0, 0};
    const u8 *b = t.base + off;
    const u8 *color = t.fmt == 8u ? b : b + 8;
    const u32 bits = rd<u32>(color);
    const u16 c0 = rd<u16>(color + 4), c1 = rd<u16>(color + 6);
    auto expand = [](u16 c) { // PSP DXT colors are BGR565
        return Rgba{static_cast<u8>(((c >> 11) & 31) * 255 / 31), static_cast<u8>(((c >> 5) & 63) * 255 / 63),
                    static_cast<u8>((c & 31) * 255 / 31), 255};
    };
    const Rgba a = expand(c0), bb = expand(c1);
    const u32 sel = (bits >> (2u * ((y % 4u) * 4u + x % 4u))) & 3u;
    Rgba out;
    if (sel == 0) out = a;
    else if (sel == 1) out = bb;
    else if (c0 > c1 || t.fmt != 8u) {
        out = sel == 2 ? Rgba{static_cast<u8>((2 * a.r + bb.r) / 3), static_cast<u8>((2 * a.g + bb.g) / 3), static_cast<u8>((2 * a.b + bb.b) / 3), 255}
                       : Rgba{static_cast<u8>((a.r + 2 * bb.r) / 3), static_cast<u8>((a.g + 2 * bb.g) / 3), static_cast<u8>((a.b + 2 * bb.b) / 3), 255};
    } else {
        out = sel == 2 ? Rgba{static_cast<u8>((a.r + bb.r) / 2), static_cast<u8>((a.g + bb.g) / 2), static_cast<u8>((a.b + bb.b) / 2), 255}
                       : Rgba{0, 0, 0, 0};
    }
    if (t.fmt == 9u) {
        const u64 alpha = rd<u64>(b);
        out.a = static_cast<u8>(((alpha >> (4u * ((y % 4u) * 4u + x % 4u))) & 15u) * 17u);
    } else if (t.fmt == 10u) {
        const u8 a0 = b[0], a1 = b[1];
        u64 abits = 0;
        for (int i = 0; i < 6; ++i) abits |= static_cast<u64>(b[2 + i]) << (8 * i);
        const u32 asel = (abits >> (3u * ((y % 4u) * 4u + x % 4u))) & 7u;
        u32 av;
        if (asel == 0) av = a0;
        else if (asel == 1) av = a1;
        else if (a0 > a1) av = ((8 - asel) * a0 + (asel - 1) * a1) / 7;
        else if (asel == 6) av = 0;
        else if (asel == 7) av = 255;
        else av = ((6 - asel) * a0 + (asel - 1) * a1) / 5;
        out.a = static_cast<u8>(av);
    }
    return out;
}

Rgba fetch_texel(const State &s, const TexInfo &t, u32 x, u32 y) {
    if (t.base == nullptr) return {255, 0, 255, 255};
    switch (t.fmt) {
    case 0: case 1: case 2: {
        const u32 o = texel_offset(t, x, y, 16);
        if (o + 2 > t.span) return {0, 0, 0, 0};
        return unpack16(t.fmt, rd<u16>(t.base + o));
    }
    case 3: {
        const u32 o = texel_offset(t, x, y, 32);
        if (o + 4 > t.span) return {0, 0, 0, 0};
        return {t.base[o], t.base[o + 1], t.base[o + 2], t.base[o + 3]};
    }
    case 4: {
        const u32 o = texel_offset(t, x, y, 4);
        if (o >= t.span) return {0, 0, 0, 0};
        return unpack_rgba(t.palette[(t.base[o] >> ((x & 1u) * 4u)) & 15u]);
    }
    case 5: {
        const u32 o = texel_offset(t, x, y, 8);
        if (o >= t.span) return {0, 0, 0, 0};
        return unpack_rgba(t.palette[t.base[o]]);
    }
    case 6: {
        const u32 o = texel_offset(t, x, y, 16);
        if (o + 2 > t.span) return {0, 0, 0, 0};
        return clut_lookup(s, rd<u16>(t.base + o));
    }
    case 7: {
        const u32 o = texel_offset(t, x, y, 32);
        if (o + 4 > t.span) return {0, 0, 0, 0};
        return clut_lookup(s, rd<u32>(t.base + o));
    }
    default: return decode_dxt(t, x, y);
    }
}

TexInfo texture_info(State &s) {
    TexInfo t;
    ensure_palette(s);
    t.palette = s.palette;
    t.clamp_u = (s.reg[TEXWRAP] & 1u) != 0u;
    t.clamp_v = (s.reg[TEXWRAP] & 0x100u) != 0u;
    t.addr = s.texaddr(0);
    t.bufw = std::max<u32>(s.texbufwidth(0), 1u);
    t.w = s.texwidth(0);
    t.h = s.texheight(0);
    t.fmt = s.reg[TEXFORMAT] & 0xFu;
    t.swizzled = (s.reg[TEXMODE] & 1u) != 0u;
    static constexpr u32 bits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
    const u32 rows = std::max<u32>(t.h, 8u);
    t.span = std::max<u32>(t.bufw, 4u) * rows * bits[t.fmt] / 8u;
    t.base = mem().raw_pointer(t.addr, t.span);
    if (t.base == nullptr) {
        // Clamp the span to the end of the memory region.
        for (u32 span = t.span; span >= 16u && t.base == nullptr; span /= 2u) {
            t.base = mem().raw_pointer(t.addr, span);
            if (t.base != nullptr) t.span = span;
        }
    }
    return t;
}

inline int fast_floor(float f) {
    const int i = static_cast<int>(f);
    return f < static_cast<float>(i) ? i - 1 : i;
}

Rgba sample(const State &s, const TexInfo &t, float u, float v, bool through, bool linear) {
    // Through mode: texel coordinates. Otherwise normalized.
    float fu = through ? u : u * static_cast<float>(t.w);
    float fv = through ? v : v * static_cast<float>(t.h);
    const bool clamp_u = t.clamp_u, clamp_v = t.clamp_v;
    auto wrap = [](int c, u32 size, bool clampit) {
        if (clampit) return static_cast<u32>(std::clamp(c, 0, static_cast<int>(size) - 1));
        return static_cast<u32>(c) & (size - 1u);
    };
    if (!linear) {
        const int x = fast_floor(fu), y = fast_floor(fv);
        return fetch_texel(s, t, wrap(x, t.w, clamp_u), wrap(y, t.h, clamp_v));
    }
    fu -= 0.5f;
    fv -= 0.5f;
    const int x0 = fast_floor(fu), y0 = fast_floor(fv);
    const float fx = fu - x0, fy = fv - y0;
    const Rgba a = fetch_texel(s, t, wrap(x0, t.w, clamp_u), wrap(y0, t.h, clamp_v));
    const Rgba b = fetch_texel(s, t, wrap(x0 + 1, t.w, clamp_u), wrap(y0, t.h, clamp_v));
    const Rgba c = fetch_texel(s, t, wrap(x0, t.w, clamp_u), wrap(y0 + 1, t.h, clamp_v));
    const Rgba d = fetch_texel(s, t, wrap(x0 + 1, t.w, clamp_u), wrap(y0 + 1, t.h, clamp_v));
    auto lerp2 = [&](u8 p, u8 q, u8 r, u8 w_) {
        const float top = p + (q - p) * fx, bot = r + (w_ - r) * fx;
        return static_cast<u8>(top + (bot - top) * fy + 0.5f);
    };
    return {lerp2(a.r, b.r, c.r, d.r), lerp2(a.g, b.g, c.g, d.g), lerp2(a.b, b.b, c.b, d.b), lerp2(a.a, b.a, c.a, d.a)};
}

// --- pixel pipeline -------------------------------------------------------------

struct Pipeline {
    const State *s{};
    u8 *fb{};
    u32 fb_stride{}, fb_fmt{};
    u16 *zb{};
    u32 zb_stride{};
    int sx0{}, sy0{}, sx1{}, sy1{}; // scissor, inclusive
    bool clear{}, clear_color{}, clear_alpha{}, clear_depth{};
    bool tex{}, through{};
    TexInfo texinfo{};
    // TEXFILTER: bits 0-2 minify, bits 8-10 magnify (odd modes are linear).
    bool linear_min{}, linear_mag{};
    bool linear{}; // chosen per primitive from its texel/pixel ratio
    bool alpha_test{}, z_test{}, z_write{}, blend{}, stencil_test{}, color_test{}, logic_op{}, fog{};
    u32 alpha_func{}, alpha_ref{}, alpha_mask{};
    u32 z_func{};
    u32 st_func{}, st_ref{}, st_mask{}, st_fail{}, st_zfail{}, st_zpass{};
    u32 blend_src{}, blend_dst{}, blend_eq{}, fix_a{}, fix_b{};
    u32 tex_func{};
    bool tex_alpha{}, tex_double{};
    u8 env[3]{};
    u8 fog_color[3]{};
    u32 mask_rgb{}, mask_alpha{};
    u32 logic{};
    u32 ct_func{}, ct_ref{}, ct_mask{};
    float minz{}, maxz{65535.f};
};

bool compare(u32 func, u32 a, u32 b) {
    switch (func) {
    case 0: return false;
    case 1: return true;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

void setup_pipeline(Pipeline &p, State &s, bool through) {
    p.s = &s;
    p.fb_fmt = s.framebuf_format();
    p.fb_stride = std::max<u32>(s.framebuf_stride(), 1u);
    const u32 bpp = p.fb_fmt == 3u ? 4u : 2u;
    p.fb = mem().raw_pointer(s.framebuf(), p.fb_stride * 272u * bpp);
    if (p.fb == nullptr) p.fb = mem().raw_pointer(s.framebuf(), p.fb_stride * 16u * bpp);
    p.zb_stride = std::max<u32>(s.zbuf_stride(), 1u);
    p.zb = reinterpret_cast<u16 *>(mem().raw_pointer(s.zbuf(), p.zb_stride * 272u * 2u));
    p.sx0 = static_cast<int>(s.reg[SCISSOR1] & 0x3FFu);
    p.sy0 = static_cast<int>((s.reg[SCISSOR1] >> 10) & 0x3FFu);
    p.sx1 = static_cast<int>(s.reg[SCISSOR2] & 0x3FFu);
    p.sy1 = static_cast<int>((s.reg[SCISSOR2] >> 10) & 0x3FFu);
    p.sx1 = std::min(p.sx1, static_cast<int>(p.fb_stride) - 1);
    p.sy1 = std::min(p.sy1, 511);
    p.through = through;
    p.clear = (s.reg[CLEARMODE] & 1u) != 0u;
    p.clear_color = (s.reg[CLEARMODE] & 0x100u) != 0u;
    p.clear_alpha = (s.reg[CLEARMODE] & 0x200u) != 0u;
    p.clear_depth = (s.reg[CLEARMODE] & 0x400u) != 0u;
    p.tex = !p.clear && (s.reg[TEXTUREMAPENABLE] & 1u) != 0u;
    if (p.tex) p.texinfo = texture_info(s);
    p.linear_min = (s.reg[TEXFILTER] & 1u) != 0u;
    p.linear_mag = (s.reg[TEXFILTER] & 0x100u) != 0u;
    p.linear = p.linear_mag;
    p.alpha_test = (s.reg[ALPHATESTENABLE] & 1u) != 0u;
    p.alpha_func = s.reg[ALPHATEST] & 7u;
    p.alpha_ref = (s.reg[ALPHATEST] >> 8) & 0xFFu;
    p.alpha_mask = (s.reg[ALPHATEST] >> 16) & 0xFFu;
    p.z_test = (s.reg[ZTESTENABLE] & 1u) != 0u && p.zb != nullptr;
    p.z_func = s.reg[ZTEST] & 7u;
    p.z_write = (s.reg[ZWRITEDISABLE] & 1u) == 0u && p.zb != nullptr && (s.reg[ZTESTENABLE] & 1u) != 0u;
    p.stencil_test = (s.reg[STENCILTESTENABLE] & 1u) != 0u && p.fb_fmt != 0u;
    p.st_func = s.reg[STENCILTEST] & 7u;
    p.st_ref = (s.reg[STENCILTEST] >> 8) & 0xFFu;
    p.st_mask = (s.reg[STENCILTEST] >> 16) & 0xFFu;
    p.st_fail = s.reg[STENCILOP] & 7u;
    p.st_zfail = (s.reg[STENCILOP] >> 8) & 7u;
    p.st_zpass = (s.reg[STENCILOP] >> 16) & 7u;
    p.blend = (s.reg[ALPHABLENDENABLE] & 1u) != 0u;
    p.blend_src = s.reg[BLENDMODE] & 0xFu;
    p.blend_dst = (s.reg[BLENDMODE] >> 4) & 0xFu;
    p.blend_eq = (s.reg[BLENDMODE] >> 8) & 7u;
    p.fix_a = s.reg[BLENDFIXEDA];
    p.fix_b = s.reg[BLENDFIXEDB];
    p.tex_func = s.reg[TEXFUNC] & 7u;
    p.tex_alpha = (s.reg[TEXFUNC] & 0x100u) != 0u;
    p.tex_double = (s.reg[TEXFUNC] & 0x10000u) != 0u;
    p.env[0] = s.reg[TEXENVCOLOR] & 0xFF; p.env[1] = (s.reg[TEXENVCOLOR] >> 8) & 0xFF; p.env[2] = (s.reg[TEXENVCOLOR] >> 16) & 0xFF;
    p.fog = !through && (s.reg[FOGENABLE] & 1u) != 0u;
    p.fog_color[0] = s.reg[FOGCOLOR] & 0xFF; p.fog_color[1] = (s.reg[FOGCOLOR] >> 8) & 0xFF; p.fog_color[2] = (s.reg[FOGCOLOR] >> 16) & 0xFF;
    p.mask_rgb = s.reg[MASKRGB] & 0xFFFFFFu;
    p.mask_alpha = s.reg[MASKALPHA] & 0xFFu;
    p.logic_op = (s.reg[LOGICOPENABLE] & 1u) != 0u;
    p.logic = s.reg[LOGICOP] & 0xFu;
    p.color_test = (s.reg[COLORTESTENABLE] & 1u) != 0u;
    p.ct_func = s.reg[COLORTEST] & 3u;
    p.ct_ref = s.reg[COLORREF] & 0xFFFFFFu;
    p.ct_mask = s.reg[COLORTESTMASK] & 0xFFFFFFu;
    p.minz = static_cast<float>(s.reg[MINZ] & 0xFFFFu);
    p.maxz = static_cast<float>(s.reg[MAXZ] & 0xFFFFu);
}

inline u32 read_fb(const Pipeline &p, int x, int y) {
    const std::size_t i = static_cast<std::size_t>(y) * p.fb_stride + static_cast<std::size_t>(x);
    if (p.fb_fmt == 3u) return rd<u32>(p.fb + i * 4);
    const u16 c = rd<u16>(p.fb + i * 2);
    const Rgba q = unpack16(p.fb_fmt, c);
    return q.r | (q.g << 8) | (q.b << 16) | (static_cast<u32>(q.a) << 24);
}

inline void write_fb(const Pipeline &p, int x, int y, u32 c) {
    const std::size_t i = static_cast<std::size_t>(y) * p.fb_stride + static_cast<std::size_t>(x);
    if (p.fb_fmt == 3u) { std::memcpy(p.fb + i * 4, &c, 4); return; }
    const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    u16 v;
    if (p.fb_fmt == 0u) v = static_cast<u16>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
    else if (p.fb_fmt == 1u) v = static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
    else v = static_cast<u16>((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
    std::memcpy(p.fb + i * 2, &v, 2);
}

u32 stencil_apply(u32 op, u32 stencil, u32 ref, u32 fb_fmt) {
    const u32 max = fb_fmt == 1u ? 0xFFu : (fb_fmt == 2u ? 0xFFu : 0xFFu);
    switch (op) {
    case 0: return stencil;
    case 1: return 0;
    case 2: return ref;
    case 3: return ~stencil & 0xFFu;
    case 4: return std::min(stencil + (fb_fmt == 2u ? 0x11u : 1u), max);
    case 5: return stencil >= (fb_fmt == 2u ? 0x11u : 1u) ? stencil - (fb_fmt == 2u ? 0x11u : 1u) : 0u;
    default: return stencil;
    }
}

u32 blend_factor_value(u32 f, bool src, const int s[4], const int d[4], u32 fix, int out[3]) {
    (void)src;
    switch (f) {
    case 0: for (int i = 0; i < 3; ++i) out[i] = src ? d[i] : s[i]; break;
    case 1: for (int i = 0; i < 3; ++i) out[i] = 255 - (src ? d[i] : s[i]); break;
    case 2: for (int i = 0; i < 3; ++i) out[i] = s[3]; break;
    case 3: for (int i = 0; i < 3; ++i) out[i] = 255 - s[3]; break;
    case 4: for (int i = 0; i < 3; ++i) out[i] = d[3]; break;
    case 5: for (int i = 0; i < 3; ++i) out[i] = 255 - d[3]; break;
    case 6: for (int i = 0; i < 3; ++i) out[i] = 2 * s[3]; break;
    case 7: for (int i = 0; i < 3; ++i) out[i] = 2 * (255 - s[3]); break;
    case 8: for (int i = 0; i < 3; ++i) out[i] = 2 * d[3]; break;
    case 9: for (int i = 0; i < 3; ++i) out[i] = 2 * (255 - d[3]); break;
    default: out[0] = fix & 0xFF; out[1] = (fix >> 8) & 0xFF; out[2] = (fix >> 16) & 0xFF; break;
    }
    return 0;
}

// Shades and writes one fragment. Colors are 0..255.
u64 g_fragments = 0;

inline void fragment(const Pipeline &p, int x, int y, float z, int r, int g, int b, int a, float u, float v, float fogf) {
    ++g_fragments;
    const std::size_t zi = static_cast<std::size_t>(y) * p.zb_stride + static_cast<std::size_t>(x);
    const u16 zval = static_cast<u16>(std::clamp(z, 0.f, 65535.f));
    if (p.clear) {
        const u32 old = read_fb(p, x, y);
        u32 c = old;
        if (p.clear_color) c = (c & 0xFF000000u) | static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16);
        if (p.clear_alpha) c = (c & 0x00FFFFFFu) | (static_cast<u32>(a) << 24);
        write_fb(p, x, y, c);
        if (p.clear_depth && p.zb != nullptr) p.zb[zi] = zval;
        return;
    }
    if (!p.through && (z < p.minz || z > p.maxz)) return;
    int s4[4] = {r, g, b, a};
    if (p.tex) {
        const Rgba t = sample(*p.s, p.texinfo, u, v, p.through, p.linear);
        const int tr = t.r, tg = t.g, tb = t.b, ta = t.a;
        switch (p.tex_func) {
        case 0: // modulate
            s4[0] = (r * tr) / 255; s4[1] = (g * tg) / 255; s4[2] = (b * tb) / 255;
            s4[3] = p.tex_alpha ? (a * ta) / 255 : a;
            break;
        case 1: // decal
            if (p.tex_alpha) {
                s4[0] = (tr * ta + r * (255 - ta)) / 255; s4[1] = (tg * ta + g * (255 - ta)) / 255; s4[2] = (tb * ta + b * (255 - ta)) / 255;
            } else { s4[0] = tr; s4[1] = tg; s4[2] = tb; }
            s4[3] = a;
            break;
        case 2: // blend
            s4[0] = (r * (255 - tr) + p.env[0] * tr) / 255; s4[1] = (g * (255 - tg) + p.env[1] * tg) / 255; s4[2] = (b * (255 - tb) + p.env[2] * tb) / 255;
            s4[3] = p.tex_alpha ? (a * ta) / 255 : a;
            break;
        case 3: // replace
            s4[0] = tr; s4[1] = tg; s4[2] = tb;
            s4[3] = p.tex_alpha ? ta : a;
            break;
        default: // add
            s4[0] = std::min(r + tr, 255); s4[1] = std::min(g + tg, 255); s4[2] = std::min(b + tb, 255);
            s4[3] = p.tex_alpha ? (a * ta) / 255 : a;
            break;
        }
        if (p.tex_double) for (int i = 0; i < 3; ++i) s4[i] = std::min(s4[i] * 2, 255);
    }
    if (p.fog) {
        for (int i = 0; i < 3; ++i) s4[i] = static_cast<int>(s4[i] * fogf + p.fog_color[i] * (1.f - fogf));
    }
    if (p.color_test) {
        const u32 col = static_cast<u32>(s4[0]) | (static_cast<u32>(s4[1]) << 8) | (static_cast<u32>(s4[2]) << 16);
        const bool eq = (col & p.ct_mask) == (p.ct_ref & p.ct_mask);
        const bool pass = p.ct_func == 0u ? false : p.ct_func == 1u ? true : p.ct_func == 2u ? eq : !eq;
        if (!pass) return;
    }
    if (p.alpha_test && !compare(p.alpha_func, static_cast<u32>(s4[3]) & p.alpha_mask, p.alpha_ref & p.alpha_mask)) return;

    const u32 dst = read_fb(p, x, y);
    u32 stencil = dst >> 24;
    if (p.stencil_test) {
        if (!compare(p.st_func, p.st_ref & p.st_mask, stencil & p.st_mask)) {
            stencil = stencil_apply(p.st_fail, stencil, p.st_ref, p.fb_fmt);
            write_fb(p, x, y, (dst & 0x00FFFFFFu) | (stencil << 24));
            return;
        }
    }
    if (p.z_test && !compare(p.z_func, zval, p.zb[zi])) {
        if (p.stencil_test) {
            stencil = stencil_apply(p.st_zfail, stencil, p.st_ref, p.fb_fmt);
            write_fb(p, x, y, (dst & 0x00FFFFFFu) | (stencil << 24));
        }
        return;
    }
    if (p.stencil_test) stencil = stencil_apply(p.st_zpass, stencil, p.st_ref, p.fb_fmt);
    // Without the stencil test the framebuffer alpha (= stencil) is preserved.
    if (p.z_write) p.zb[zi] = zval;

    int out[3] = {s4[0], s4[1], s4[2]};
    if (p.blend) {
        const int d4[4] = {static_cast<int>(dst & 0xFF), static_cast<int>((dst >> 8) & 0xFF), static_cast<int>((dst >> 16) & 0xFF),
                           static_cast<int>(dst >> 24)};
        int fa[3], fb_[3];
        blend_factor_value(p.blend_src, true, s4, d4, p.fix_a, fa);
        blend_factor_value(p.blend_dst, false, s4, d4, p.fix_b, fb_);
        for (int i = 0; i < 3; ++i) {
            const int sv = s4[i] * fa[i] / 255, dv = d4[i] * fb_[i] / 255;
            int res;
            switch (p.blend_eq) {
            case 0: res = sv + dv; break;
            case 1: res = sv - dv; break;
            case 2: res = dv - sv; break;
            case 3: res = std::min(s4[i], d4[i]); break;
            case 4: res = std::max(s4[i], d4[i]); break;
            default: res = std::abs(s4[i] - d4[i]); break;
            }
            out[i] = std::clamp(res, 0, 255);
        }
    }
    u32 c = static_cast<u32>(std::clamp(out[0], 0, 255)) | (static_cast<u32>(std::clamp(out[1], 0, 255)) << 8) |
            (static_cast<u32>(std::clamp(out[2], 0, 255)) << 16) | (stencil << 24);
    if (p.logic_op) {
        switch (p.logic) {
        case 0: c = 0; break;
        case 1: c &= dst; break;
        case 2: c &= ~dst; break;
        case 3: break;
        case 4: c = ~c & dst; break;
        case 5: c = dst; break;
        case 6: c ^= dst; break;
        case 7: c |= dst; break;
        case 8: c = ~(c | dst); break;
        case 9: c = ~(c ^ dst); break;
        case 10: c = ~dst; break;
        case 11: c |= ~dst; break;
        case 12: c = ~c; break;
        case 13: c = ~c | dst; break;
        case 14: c = ~(c & dst); break;
        default: c = 0xFFFFFFFFu; break;
        }
    }
    const u32 mask = p.mask_rgb | (p.mask_alpha << 24);
    if (mask != 0u) c = (c & ~mask) | (dst & mask);
    write_fb(p, x, y, c);
}

// --- rasterization ------------------------------------------------------------------

// Texels covered per screen pixel along each axis decides minify vs magnify.
void choose_filter(Pipeline &p, float texels_per_pixel) {
    p.linear = texels_per_pixel > 1.0f ? p.linear_min : p.linear_mag;
}

float texel_scale(const Pipeline &p) {
    return p.through ? 1.0f : static_cast<float>(std::max(p.texinfo.w, p.texinfo.h));
}

void raster_triangle(Pipeline &p, const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, bool flat,
                     const float flat_color[4]) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0.f || !std::isfinite(area)) return;
    if (p.tex) {
        // Compare texture-space area with screen-space area (perspective ignored).
        auto uv = [](const ScreenVertex &v, float &u, float &w) {
            const float iw = v.invw != 0.f ? 1.f / v.invw : 1.f;
            u = v.u * iw; w = v.v * iw;
        };
        float u0, v0, u1, v1, u2, v2;
        uv(a, u0, v0); uv(b, u1, v1); uv(c, u2, v2);
        const float sc = texel_scale(p);
        const float tex_area = std::fabs((u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0)) * sc * sc;
        choose_filter(p, std::sqrt(tex_area / std::fabs(area)));
    }
    int minx = static_cast<int>(std::floor(std::min({a.x, b.x, c.x})));
    int maxx = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
    int miny = static_cast<int>(std::floor(std::min({a.y, b.y, c.y})));
    int maxy = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
    minx = std::max(minx, p.sx0); miny = std::max(miny, p.sy0);
    maxx = std::min(maxx, p.sx1); maxy = std::min(maxy, p.sy1);
    if (minx > maxx || miny > maxy) return;
    const float inv_area = 1.f / area;
    // Top-left fill rule bias.
    auto edge_bias = [&](const ScreenVertex &p0, const ScreenVertex &p1) {
        const float dy = p1.y - p0.y, dx = p1.x - p0.x;
        const bool top_left = area > 0 ? (dy < 0 || (dy == 0 && dx > 0)) : (dy > 0 || (dy == 0 && dx < 0));
        return top_left ? 0.f : -1e-6f;
    };
    const float bias[3] = {edge_bias(b, c), edge_bias(c, a), edge_bias(a, b)};
    // Barycentric weights are linear in x and y: w_i(x, y) = e_i + dx_i * x + dy_i * y.
    const float dx0 = -(c.y - b.y) * inv_area, dy0 = (c.x - b.x) * inv_area;
    const float dx1 = -(a.y - c.y) * inv_area, dy1 = (a.x - c.x) * inv_area;
    const float e0 = ((c.x - b.x) * (-b.y) - (c.y - b.y) * (-b.x)) * inv_area;
    const float e1 = ((a.x - c.x) * (-c.y) - (a.y - c.y) * (-c.x)) * inv_area;

    // Attribute planes: value = A_a * w0 + A_b * w1 + A_c * w2. Perspective-correct
    // attributes are pre-multiplied by 1/w and divided back per pixel.
    constexpr int kAttrs = 9; // z, 1/w, u/w, v/w, r, g, b, a (x 1/w), fog
    float va_[kAttrs], vb_[kAttrs], vc_[kAttrs];
    auto load = [&](const ScreenVertex &v, float *out) {
        out[0] = v.z;
        out[1] = v.invw;
        out[2] = v.u;
        out[3] = v.v;
        for (int i = 0; i < 4; ++i) out[4 + i] = flat ? flat_color[i] * v.invw : v.color[i] * v.invw;
        out[8] = v.fog;
    };
    load(a, va_);
    load(b, vb_);
    load(c, vc_);
    // With w2 = 1 - w0 - w1: A = A_c + (A_a - A_c) w0 + (A_b - A_c) w1.
    float ddx[kAttrs], ddy[kAttrs], base[kAttrs];
    for (int i = 0; i < kAttrs; ++i) {
        const float ka = va_[i] - vc_[i], kb = vb_[i] - vc_[i];
        ddx[i] = ka * dx0 + kb * dx1;
        ddy[i] = ka * dy0 + kb * dy1;
        base[i] = vc_[i] + ka * e0 + kb * e1;
    }
    const bool perspective = !p.through;

    for (int y = miny; y <= maxy; ++y) {
        const float py = y + 0.5f;
        // Solve each edge for the covered x range on this row (pixel centres at x + 0.5).
        float w0r = e0 + dy0 * py, w1r = e1 + dy1 * py;
        float w2r = 1.f - w0r - w1r;
        const float dx2 = -dx0 - dx1;
        float lo = static_cast<float>(minx), hi = static_cast<float>(maxx);
        bool empty = false;
        auto clip = [&](float w_at_0, float d, float b_) {
            // w(x) = w_at_0 + d * (x + 0.5) >= b_
            if (d > 0.f) lo = std::max(lo, (b_ - w_at_0) / d - 0.5f);
            else if (d < 0.f) hi = std::min(hi, (b_ - w_at_0) / d - 0.5f);
            else if (w_at_0 < b_) empty = true;
        };
        clip(w0r, dx0, bias[0]);
        clip(w1r, dx1, bias[1]);
        clip(w2r, dx2, bias[2]);
        if (empty) continue;
        int xs = std::max(minx, static_cast<int>(std::ceil(lo - 1e-4f)));
        int xe = std::min(maxx, static_cast<int>(std::floor(hi + 1e-4f)));
        if (xs > xe) continue;
        float attr[kAttrs];
        const float sx = xs + 0.5f;
        for (int i = 0; i < kAttrs; ++i) attr[i] = base[i] + ddx[i] * sx + ddy[i] * py;
        float w0 = w0r + dx0 * sx, w1 = w1r + dx1 * sx;
        for (int x = xs; x <= xe; ++x) {
            const float w2 = 1.f - w0 - w1;
            // Exact edge test at the span borders guards against float error in the solve.
            if (w0 >= bias[0] && w1 >= bias[1] && w2 >= bias[2]) {
                const float wcorr = perspective && attr[1] != 0.f ? 1.f / attr[1] : 1.f;
                const float u = attr[2] * wcorr, v = attr[3] * wcorr;
                fragment(p, x, y, attr[0], static_cast<int>(clamp01(attr[4] * wcorr) * 255.f + 0.5f),
                         static_cast<int>(clamp01(attr[5] * wcorr) * 255.f + 0.5f), static_cast<int>(clamp01(attr[6] * wcorr) * 255.f + 0.5f),
                         static_cast<int>(clamp01(attr[7] * wcorr) * 255.f + 0.5f), u, v, attr[8]);
            }
            w0 += dx0;
            w1 += dx1;
            for (int i = 0; i < kAttrs; ++i) attr[i] += ddx[i];
        }
    }
}

void raster_sprite(Pipeline &p, const ScreenVertex &a, const ScreenVertex &b) {
    // The second vertex supplies color and depth.
    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    const float y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    const bool flip_x = a.x > b.x, flip_y = a.y > b.y;
    const float ua = flip_x ? b.u : a.u, ub = flip_x ? a.u : b.u;
    const float va = flip_y ? b.v : a.v, vb = flip_y ? a.v : b.v;
    const int ix0 = std::max(static_cast<int>(std::ceil(x0 - 0.5f)), p.sx0);
    const int ix1 = std::min(static_cast<int>(std::ceil(x1 - 0.5f)) - 1, p.sx1);
    const int iy0 = std::max(static_cast<int>(std::ceil(y0 - 0.5f)), p.sy0);
    const int iy1 = std::min(static_cast<int>(std::ceil(y1 - 0.5f)) - 1, p.sy1);
    if (ix0 > ix1 || iy0 > iy1) return;
    const float du = x1 > x0 ? (ub - ua) / (x1 - x0) : 0.f;
    const float dv = y1 > y0 ? (vb - va) / (y1 - y0) : 0.f;
    if (p.tex) choose_filter(p, std::max(std::fabs(du), std::fabs(dv)) * texel_scale(p) * (a.invw != 0.f ? 1.f / a.invw : 1.f));
    const float wa = a.invw != 0.f ? 1.f / a.invw : 1.f;
    const int r = static_cast<int>(clamp01(b.color[0]) * 255.f + 0.5f), g = static_cast<int>(clamp01(b.color[1]) * 255.f + 0.5f);
    const int bl = static_cast<int>(clamp01(b.color[2]) * 255.f + 0.5f), al = static_cast<int>(clamp01(b.color[3]) * 255.f + 0.5f);
    for (int y = iy0; y <= iy1; ++y) {
        const float v = (va + (y + 0.5f - y0) * dv) * wa;
        for (int x = ix0; x <= ix1; ++x) {
            const float u = (ua + (x + 0.5f - x0) * du) * wa;
            fragment(p, x, y, b.z, r, g, bl, al, u, v, b.fog);
        }
    }
}

void raster_line(const Pipeline &p, const ScreenVertex &a, const ScreenVertex &b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy)));
    // Like the GE, a line leaves out its end pixel (a point, steps == 0, is one pixel).
    // Positions are computed as a + d*i/steps, not a + d*(i/steps): the rounded t made
    // full-width lines skip every other pixel past the middle.
    const int last = steps > 0 ? steps - 1 : 0;
    for (int i = 0; i <= last; ++i) {
        const float t = steps > 0 ? static_cast<float>(i) / steps : 0.f;
        const int x = steps > 0 ? fast_floor(a.x + dx * static_cast<float>(i) / static_cast<float>(steps)) : fast_floor(a.x);
        const int y = steps > 0 ? fast_floor(a.y + dy * static_cast<float>(i) / static_cast<float>(steps)) : fast_floor(a.y);
        if (x < p.sx0 || x > p.sx1 || y < p.sy0 || y > p.sy1) continue;
        float col[4];
        for (int k = 0; k < 4; ++k) col[k] = a.color[k] + (b.color[k] - a.color[k]) * t;
        fragment(p, x, y, a.z + (b.z - a.z) * t, static_cast<int>(clamp01(col[0]) * 255.f), static_cast<int>(clamp01(col[1]) * 255.f),
                 static_cast<int>(clamp01(col[2]) * 255.f), static_cast<int>(clamp01(col[3]) * 255.f), a.u + (b.u - a.u) * t,
                 a.v + (b.v - a.v) * t, a.fog);
    }
}

// Clips a clip-space triangle against the near plane (z > -w) and emits screen triangles.
void clip_and_draw(const State &s, Pipeline &p, ScreenVertex v0, ScreenVertex v1, ScreenVertex v2, bool flat,
                   const float flat_color[4], bool cull, bool cull_ccw) {
    ScreenVertex in[3] = {v0, v1, v2};
    ScreenVertex poly[8];
    int n = 0;
    auto inside = [](const ScreenVertex &v) { return v.clip.z > -v.clip.w; };
    auto lerp = [](const ScreenVertex &a, const ScreenVertex &b, float t) {
        ScreenVertex r = a;
        r.clip.x = a.clip.x + (b.clip.x - a.clip.x) * t;
        r.clip.y = a.clip.y + (b.clip.y - a.clip.y) * t;
        r.clip.z = a.clip.z + (b.clip.z - a.clip.z) * t;
        r.clip.w = a.clip.w + (b.clip.w - a.clip.w) * t;
        r.u = a.u + (b.u - a.u) * t;
        r.v = a.v + (b.v - a.v) * t;
        for (int i = 0; i < 4; ++i) r.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
        r.fog = a.fog + (b.fog - a.fog) * t;
        return r;
    };
    for (int i = 0; i < 3; ++i) {
        const ScreenVertex &a = in[i], &b = in[(i + 1) % 3];
        const bool ia = inside(a), ib = inside(b);
        if (ia) poly[n++] = a;
        if (ia != ib) {
            const float da = a.clip.z + a.clip.w, db = b.clip.z + b.clip.w;
            poly[n++] = lerp(a, b, da / (da - db));
        }
    }
    if (n < 3) return;
    for (int i = 0; i < n; ++i) project(s, poly[i]);
    for (int i = 1; i + 1 < n; ++i) {
        const ScreenVertex &a = poly[0], &b = poly[i], &c = poly[i + 1];
        if (cull) {
            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cull_ccw ? area < 0.f : area > 0.f) continue;
        }
        raster_triangle(p, a, b, c, flat, flat_color);
    }
}

u32 read_index(const VertexLayout &l, u32 index_addr, u32 i) {
    auto &m = mem();
    if (l.index == 1u) return m.load8(index_addr + i);
    if (l.index == 2u) return m.load16(index_addr + i * 2u);
    return m.load32(index_addr + i * 4u);
}

// Writes a decoded copy of the current texture (as the sampler sees it) to build/.
void dump_texture(const State &s, const TexInfo &t) {
    char name[96];
    std::snprintf(name, sizeof(name), "build/tex_%08x_%ux%u_f%u.bmp", t.addr, t.w, t.h, t.fmt);
    std::FILE *f = std::fopen(name, "wb");
    if (f == nullptr) return;
    const u32 w = std::min<u32>(t.w, 1024u), h = std::min<u32>(t.h, 1024u), image = w * h * 4u;
    u8 header[54]{'B', 'M'};
    auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<u8>(v >> (8 * i)); };
    put32(2, 54u + image); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    header[26] = 1; header[28] = 32; put32(34, image);
    std::fwrite(header, 1, sizeof(header), f);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (u32 x = 0; x < w; ++x) {
            const Rgba c = fetch_texel(s, t, x, static_cast<u32>(y));
            // Composite over magenta so transparent texels are visible.
            const u32 a = c.a;
            const u8 bgra[4] = {static_cast<u8>((c.b * a + 255u * (255u - a)) / 255u), static_cast<u8>((c.g * a) / 255u),
                                static_cast<u8>((c.r * a + 255u * (255u - a)) / 255u), 255};
            std::fwrite(bgra, 1, 4, f);
        }
    }
    std::fclose(f);
}

} // namespace

void draw_prim(State &s, u32 prim_type, u32 count) {
    const VertexLayout l = vertex_layout(s.reg[VERTEXTYPE]);
    static constexpr u32 index_size[4] = {0, 1, 2, 4};
    if (count == 0u) return;

    std::vector<ScreenVertex> verts(count);
    u32 lowest = 0xFFFFFFFFu, highest = 0u;
    for (u32 i = 0; i < count; ++i) {
        const u32 idx = l.index != 0u ? read_index(l, s.index_addr, i) : i;
        lowest = std::min(lowest, idx);
        highest = std::max(highest, idx);
        RawVertex raw;
        decode_vertex(s, l, s.vertex_addr + idx * l.size * l.morphs, raw);
        verts[i] = transform(s, l, raw);
    }
    if (l.index != 0u) s.index_addr += count * index_size[l.index];
    else s.vertex_addr += count * l.size * l.morphs;

    // Fast-forward: rasterize only the frames that get presented (every 8th vblank).
    if (g_kernel.turbo() && (g_kernel.total_flips & 7u) != 0u) return;

    Pipeline p;
    setup_pipeline(p, s, l.through);
    if (std::FILE *tf = trace_file()) {
        std::fprintf(tf, "prim %u n=%u vtype=%06x fb=%08x/%u fmt=%u tex=%d addr=%08x %ux%u bufw=%u fmt=%u clut=%06x func=%05x "
                         "blend=%d mode=%04x fixa=%06x fixb=%06x atest=%d %06x ztest=%d %x clear=%06x scissor=%d,%d-%d,%d\n",
                     prim_type, count, s.reg[VERTEXTYPE], s.framebuf(), s.framebuf_stride(), s.framebuf_format(), p.tex ? 1 : 0,
                     p.texinfo.addr, p.texinfo.w, p.texinfo.h, p.texinfo.bufw, p.texinfo.fmt, s.reg[CLUTFORMAT], s.reg[TEXFUNC],
                     p.blend ? 1 : 0, s.reg[BLENDMODE], s.reg[BLENDFIXEDA], s.reg[BLENDFIXEDB], p.alpha_test ? 1 : 0, s.reg[ALPHATEST],
                     p.z_test ? 1 : 0, s.reg[ZTEST], s.reg[CLEARMODE], p.sx0, p.sy0, p.sx1, p.sy1);
        if (p.tex) dump_texture(s, p.texinfo);
        for (u32 i = 0; i < std::min<u32>(count, 8u); ++i) {
            const ScreenVertex &v = verts[i];
            std::fprintf(tf, "   v%u pos=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n", i, v.x, v.y, v.z, v.u, v.v,
                         v.color[0], v.color[1], v.color[2], v.color[3]);
        }
    }
    if (p.fb == nullptr) return;
    const bool flat = (s.reg[SHADEMODE] & 1u) == 0u;
    const bool cull = !l.through && (s.reg[CULLFACEENABLE] & 1u) != 0u && prim_type >= 3u && prim_type <= 5u;
    const bool cull_ccw = (s.reg[CULL] & 1u) == 0u;

    auto tri = [&](u32 i0, u32 i1, u32 i2, u32 provoking) {
        const float *fc = verts[provoking].color;
        if (l.through) {
            if (cull) return;
            raster_triangle(p, verts[i0], verts[i1], verts[i2], flat, fc);
        } else {
            clip_and_draw(s, p, verts[i0], verts[i1], verts[i2], flat, fc, cull, cull_ccw);
        }
    };
    auto to_screen = [&](ScreenVertex v) {
        if (!l.through) project(s, v);
        return v;
    };

    switch (prim_type) {
    case 0: // points
        for (u32 i = 0; i < count; ++i) {
            const ScreenVertex v = to_screen(verts[i]);
            raster_line(p, v, v);
        }
        break;
    case 1: // lines
        for (u32 i = 0; i + 1 < count; i += 2) raster_line(p, to_screen(verts[i]), to_screen(verts[i + 1]));
        break;
    case 2: // line strip
        for (u32 i = 0; i + 1 < count; ++i) raster_line(p, to_screen(verts[i]), to_screen(verts[i + 1]));
        break;
    case 3: // triangles
        for (u32 i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2, i + 2);
        break;
    case 4: // strip
        for (u32 i = 0; i + 2 < count; ++i) {
            if (i & 1u) tri(i + 1, i, i + 2, i + 2);
            else tri(i, i + 1, i + 2, i + 2);
        }
        break;
    case 5: // fan
        for (u32 i = 1; i + 1 < count; ++i) tri(0, i, i + 1, i + 1);
        break;
    case 6: // rectangles (sprites)
        for (u32 i = 0; i + 1 < count; i += 2) {
            ScreenVertex a = verts[i], b = verts[i + 1];
            if (!l.through) {
                if (a.clip.w <= 0.f || b.clip.w <= 0.f) continue;
                project(s, a);
                project(s, b);
            }
            raster_sprite(p, a, b);
        }
        break;
    default: break;
    }
}

void draw_patch(State &s, bool spline, u32 data) {
    static bool warned = false;
    if (!warned) { warned = true; LOG_W("gpu", "%s patches are not rendered yet", spline ? "spline" : "bezier"); }
    const u32 ucount = data & 0xFFu, vcount = (data >> 8) & 0xFFu;
    const VertexLayout l = vertex_layout(s.reg[VERTEXTYPE]);
    static constexpr u32 index_size[4] = {0, 1, 2, 4};
    if (l.index != 0u) s.index_addr += ucount * vcount * index_size[l.index];
    else s.vertex_addr += ucount * vcount * l.size * l.morphs;
}

void load_clut(State &s) {
    const u32 addr = (s.reg[CLUTADDR] & 0xFFFFF0u) | ((s.reg[CLUTADDRUPPER] << 8) & 0x0F000000u);
    const u32 bytes = std::min<u32>((s.reg[LOADCLUT] & 0x3Fu) * 32u, sizeof(s.clut));
    if (const u8 *src = mem().raw_pointer(addr, bytes)) std::memcpy(s.clut, src, bytes);
    s.palette_dirty = true;
}

void block_transfer(State &s, u32 data) {
    const u32 bpp = (data & 1u) != 0u ? 4u : 2u;
    const u32 src = (s.reg[TRANSFERSRC] & 0xFFFFF0u) | ((s.reg[TRANSFERSRCW] & 0xFF0000u) << 8);
    const u32 src_stride = s.reg[TRANSFERSRCW] & 0x7F8u;
    const u32 dst = (s.reg[TRANSFERDST] & 0xFFFFF0u) | ((s.reg[TRANSFERDSTW] & 0xFF0000u) << 8);
    const u32 dst_stride = s.reg[TRANSFERDSTW] & 0x7F8u;
    const u32 sx = s.reg[TRANSFERSRCPOS] & 0x3FFu, sy = (s.reg[TRANSFERSRCPOS] >> 10) & 0x3FFu;
    const u32 dx = s.reg[TRANSFERDSTPOS] & 0x3FFu, dy = (s.reg[TRANSFERDSTPOS] >> 10) & 0x3FFu;
    const u32 w = (s.reg[TRANSFERSIZE] & 0x3FFu) + 1u, h = ((s.reg[TRANSFERSIZE] >> 10) & 0x3FFu) + 1u;
    auto &m = mem();
    for (u32 y = 0; y < h; ++y) {
        const u32 from = src + ((sy + y) * src_stride + sx) * bpp;
        const u32 to = dst + ((dy + y) * dst_stride + dx) * bpp;
        const u8 *a = m.raw_pointer(from, w * bpp);
        u8 *b = m.raw_pointer(to, w * bpp);
        if (a != nullptr && b != nullptr) std::memmove(b, a, w * bpp);
    }
}

void flush(State &) {}

u64 take_fragment_count() {
    const u64 n = g_fragments;
    g_fragments = 0;
    return n;
}

} // namespace p2::gpu
