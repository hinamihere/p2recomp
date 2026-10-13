// GE register state shared by the display-list processor and the renderer.
#pragma once

#include "hle.hpp"

#include <array>
#include <cstring>

namespace p2::gpu {

enum Cmd : u8 {
    NOP = 0x00, VADDR = 0x01, IADDR = 0x02, PRIM = 0x04, BEZIER = 0x05, SPLINE = 0x06, BOUNDINGBOX = 0x07,
    JUMP = 0x08, BJUMP = 0x09, CALL = 0x0A, RET = 0x0B, END = 0x0C, SIGNAL = 0x0E, FINISH = 0x0F,
    BASE = 0x10, VERTEXTYPE = 0x12, OFFSETADDR = 0x13, ORIGIN = 0x14, REGION1 = 0x15, REGION2 = 0x16,
    LIGHTINGENABLE = 0x17, LIGHTENABLE0 = 0x18, DEPTHCLIPENABLE = 0x1C, CULLFACEENABLE = 0x1D,
    TEXTUREMAPENABLE = 0x1E, FOGENABLE = 0x1F, DITHERENABLE = 0x20, ALPHABLENDENABLE = 0x21,
    ALPHATESTENABLE = 0x22, ZTESTENABLE = 0x23, STENCILTESTENABLE = 0x24, ANTIALIASENABLE = 0x25,
    PATCHCULLENABLE = 0x26, COLORTESTENABLE = 0x27, LOGICOPENABLE = 0x28,
    BONEMATRIXNUMBER = 0x2A, BONEMATRIXDATA = 0x2B, MORPHWEIGHT0 = 0x2C,
    PATCHDIVISION = 0x36, PATCHPRIMITIVE = 0x37, PATCHFACING = 0x38,
    WORLDMATRIXNUMBER = 0x3A, WORLDMATRIXDATA = 0x3B, VIEWMATRIXNUMBER = 0x3C, VIEWMATRIXDATA = 0x3D,
    PROJMATRIXNUMBER = 0x3E, PROJMATRIXDATA = 0x3F, TGENMATRIXNUMBER = 0x40, TGENMATRIXDATA = 0x41,
    VIEWPORTXSCALE = 0x42, VIEWPORTYSCALE = 0x43, VIEWPORTZSCALE = 0x44,
    VIEWPORTXCENTER = 0x45, VIEWPORTYCENTER = 0x46, VIEWPORTZCENTER = 0x47,
    TEXSCALEU = 0x48, TEXSCALEV = 0x49, TEXOFFSETU = 0x4A, TEXOFFSETV = 0x4B, OFFSETX = 0x4C, OFFSETY = 0x4D,
    SHADEMODE = 0x50, REVERSENORMAL = 0x51, MATERIALUPDATE = 0x53, MATERIALEMISSIVE = 0x54,
    MATERIALAMBIENT = 0x55, MATERIALDIFFUSE = 0x56, MATERIALSPECULAR = 0x57, MATERIALALPHA = 0x58,
    MATERIALSPECULARCOEF = 0x5B, AMBIENTCOLOR = 0x5C, AMBIENTALPHA = 0x5D, LIGHTMODE = 0x5E,
    LIGHTTYPE0 = 0x5F, LX0 = 0x63, LDX0 = 0x6F, LKA0 = 0x7B, LKS0 = 0x87, LKO0 = 0x8B, LAC0 = 0x8F,
    LDC0 = 0x90, LSC0 = 0x91,
    CULL = 0x9B, FRAMEBUFPTR = 0x9C, FRAMEBUFWIDTH = 0x9D, ZBUFPTR = 0x9E, ZBUFWIDTH = 0x9F,
    TEXADDR0 = 0xA0, TEXBUFWIDTH0 = 0xA8, CLUTADDR = 0xB0, CLUTADDRUPPER = 0xB1,
    TRANSFERSRC = 0xB2, TRANSFERSRCW = 0xB3, TRANSFERDST = 0xB4, TRANSFERDSTW = 0xB5,
    TEXSIZE0 = 0xB8, TEXMAPMODE = 0xC0, TEXSHADELS = 0xC1, TEXMODE = 0xC2, TEXFORMAT = 0xC3,
    LOADCLUT = 0xC4, CLUTFORMAT = 0xC5, TEXFILTER = 0xC6, TEXWRAP = 0xC7, TEXLEVEL = 0xC8,
    TEXFUNC = 0xC9, TEXENVCOLOR = 0xCA, TEXFLUSH = 0xCB, TEXSYNC = 0xCC, FOG1 = 0xCD, FOG2 = 0xCE,
    FOGCOLOR = 0xCF, TEXLODSLOPE = 0xD0, FRAMEBUFPIXFORMAT = 0xD2, CLEARMODE = 0xD3,
    SCISSOR1 = 0xD4, SCISSOR2 = 0xD5, MINZ = 0xD6, MAXZ = 0xD7, COLORTEST = 0xD8, COLORREF = 0xD9,
    COLORTESTMASK = 0xDA, ALPHATEST = 0xDB, STENCILTEST = 0xDC, STENCILOP = 0xDD, ZTEST = 0xDE,
    BLENDMODE = 0xDF, BLENDFIXEDA = 0xE0, BLENDFIXEDB = 0xE1, DITH0 = 0xE2, LOGICOP = 0xE6,
    ZWRITEDISABLE = 0xE7, MASKRGB = 0xE8, MASKALPHA = 0xE9, TRANSFERSTART = 0xEA,
    TRANSFERSRCPOS = 0xEB, TRANSFERDSTPOS = 0xEC, TRANSFERSIZE = 0xEE,
};

inline float float24(u32 data) {
    const u32 bits = data << 8;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

struct State {
    std::array<u32, 256> reg{}; // last 24-bit data written per command
    float world[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float view[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float proj[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float tgen[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float bone[8 * 12]{};
    float morph[8]{1, 0, 0, 0, 0, 0, 0, 0};
    u32 world_index{}, view_index{}, proj_index{}, tgen_index{}, bone_index{};
    u32 base{};          // BASE register (upper address bits)
    u32 offset_addr{};   // OFFSETADDR / ORIGIN
    u32 vertex_addr{};
    u32 index_addr{};
    u8 clut[1024]{};     // loaded palette (up to 256 x 32-bit)

    u32 relative(u32 data) const { return (((base & 0x000F0000u) << 8) | data) + offset_addr; }

    u32 framebuf() const { return 0x04000000u | (reg[FRAMEBUFPTR] & 0x1FFFF0u); }
    u32 framebuf_stride() const { return reg[FRAMEBUFWIDTH] & 0x7FCu; }
    u32 framebuf_format() const { return reg[FRAMEBUFPIXFORMAT] & 3u; }
    u32 zbuf() const { return 0x04000000u | (reg[ZBUFPTR] & 0x1FFFF0u); }
    u32 zbuf_stride() const { return reg[ZBUFWIDTH] & 0x7FCu; }
    u32 texaddr(int level) const {
        return (reg[TEXADDR0 + level] & 0xFFFFF0u) | ((reg[TEXBUFWIDTH0 + level] << 8) & 0x0F000000u);
    }
    u32 texbufwidth(int level) const { return reg[TEXBUFWIDTH0 + level] & 0x7FFu; }
    u32 texwidth(int level) const { return 1u << (reg[TEXSIZE0 + level] & 0xFu); }
    u32 texheight(int level) const { return 1u << ((reg[TEXSIZE0 + level] >> 8) & 0xFu); }
};

// Renderer entry points implemented by the rasterizer.
void draw_prim(State &s, u32 prim_type, u32 count);
void draw_patch(State &s, bool spline, u32 data);
void load_clut(State &s);
void block_transfer(State &s, u32 data);
// Called when the frame buffer configuration may have changed or a list ends.
void flush(State &s);

} // namespace p2::gpu
