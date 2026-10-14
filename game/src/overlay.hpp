// Host-side drawing on the presented 480x272 frame (utility dialogs, notices).
// Pixels are 0xAABBGGRR, as produced by the display code.
#pragma once

#include "hle.hpp"

#include <string>

namespace p2::overlay {

constexpr int kWidth = 480, kHeight = 272;

inline u32 rgb(u32 r, u32 g, u32 b) { return 0xFF000000u | (b << 16) | (g << 8) | r; }

// Darkens the whole frame (amount 0..256, 256 = unchanged).
void dim(u32 *frame, int amount);
// Blends a rectangle of `color` over the frame with alpha 0..255.
void fill(u32 *frame, int x, int y, int w, int h, u32 color, int alpha = 255);
void frame_rect(u32 *frame, int x, int y, int w, int h, u32 color);
// 8x8 text (ASCII; other characters draw as '?'), clipped to max_chars. Returns the width drawn.
int text(u32 *frame, int x, int y, const std::string &s, u32 color, int max_chars = 1 << 20);

} // namespace p2::overlay
