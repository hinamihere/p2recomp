#pragma once

#include "hle.hpp"

#include <string>

namespace p2 {

void register_audio_imports();
void audio_start();
// Records the final mix as raw s16 stereo 44.1 kHz.
void audio_dump_to(const std::string &path);

// Not yet implemented libraries (SAS, ATRAC3+, MPEG, utility dialogs).
void register_stub_imports();

} // namespace p2
