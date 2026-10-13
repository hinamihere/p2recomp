#pragma once

#include "hle.hpp"

namespace p2 {

void register_display_imports();
// Saves every `every`-th presented frame to build/frames/ (0 = off).
void display_set_frame_dump(u32 every);

} // namespace p2
