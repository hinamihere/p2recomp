#include "overlay.hpp"

#include <algorithm>

namespace p2::overlay {
namespace {

// The upstream header declares plain char arrays with bytes >= 0x80 (a narrowing error in C++).
#define char unsigned char
#include "../third_party/font8x8/font8x8_basic.h"
#undef char

u32 blend(u32 dst, u32 src, int alpha) {
    const u32 inv = 255u - static_cast<u32>(alpha), a = static_cast<u32>(alpha);
    u32 out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const u32 d = (dst >> shift) & 0xFFu, s = (src >> shift) & 0xFFu;
        out |= ((d * inv + s * a) / 255u) << shift;
    }
    return out;
}

} // namespace

void dim(u32 *frame, int amount) {
    for (int i = 0; i < kWidth * kHeight; ++i) {
        const u32 p = frame[i];
        u32 out = 0xFF000000u;
        for (int shift = 0; shift < 24; shift += 8) out |= ((((p >> shift) & 0xFFu) * static_cast<u32>(amount)) >> 8) << shift;
        frame[i] = out;
    }
}

void fill(u32 *frame, int x, int y, int w, int h, u32 color, int alpha) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0), x1 = std::min(x + w, kWidth), y1 = std::min(y + h, kHeight);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            u32 &p = frame[yy * kWidth + xx];
            p = alpha >= 255 ? color : blend(p, color, alpha);
        }
}

void frame_rect(u32 *frame, int x, int y, int w, int h, u32 color) {
    fill(frame, x, y, w, 1, color);
    fill(frame, x, y + h - 1, w, 1, color);
    fill(frame, x, y, 1, h, color);
    fill(frame, x + w - 1, y, 1, h, color);
}

int text(u32 *frame, int x, int y, const std::string &s, u32 color, int max_chars) {
    int drawn = 0;
    for (char ch : s) {
        if (drawn >= max_chars) break;
        const unsigned c = static_cast<unsigned char>(ch) < 128u ? static_cast<unsigned char>(ch) : static_cast<unsigned>('?');
        const int gx = x + drawn * 8;
        for (int row = 0; row < 8; ++row) {
            const int py = y + row;
            if (py < 0 || py >= kHeight) continue;
            const unsigned bits = static_cast<unsigned char>(font8x8_basic[c][row]);
            for (int col = 0; col < 8; ++col) {
                const int px = gx + col;
                if ((bits >> col) & 1u && px >= 0 && px < kWidth) frame[py * kWidth + px] = color;
            }
        }
        ++drawn;
    }
    return drawn * 8;
}

} // namespace p2::overlay
