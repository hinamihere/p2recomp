// Host window, input and audio output (SDL2).
#pragma once

#include "hle.hpp"

#include <string>

namespace p2::host {

bool init(const std::string &title, int scale, bool smooth);
void shutdown();

// Presents a 480x272 RGBA8888 frame (R,G,B,A byte order).
void present(const u32 *pixels, int width, int height);
// Processes window/input events; returns false when the user closed the window.
bool pump_events();

// PSP button bitmask and analog stick (0..255, 128 = centre).
u32 buttons();
// True while the fast-forward key (Tab) is held.
bool fast_forward_held();
u8 analog_x();
u8 analog_y();

void set_title_suffix(const std::string &text);

// Audio: the mixer pulls stereo s16 frames from this callback on the audio thread.
using AudioPull = void (*)(s16 *out, int frames);
bool start_audio(AudioPull pull, int sample_rate);

} // namespace p2::host
