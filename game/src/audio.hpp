#pragma once

#include "hle.hpp"

namespace p2 {

void register_audio_imports();
void audio_start();

// Not yet implemented libraries (SAS, ATRAC3+, MPEG, utility dialogs).
void register_stub_imports();

} // namespace p2
