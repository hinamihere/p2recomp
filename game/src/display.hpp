#pragma once

#include "hle.hpp"

#include <string>

namespace p2 {

void register_display_imports();
// Saves every `every`-th presented frame to build/frames/ (0 = off).
void display_set_frame_dump(u32 every);
// Scripted button presses for unattended testing (see --input).
void display_set_input_script(const std::string &script);

} // namespace p2
