#pragma once

namespace p2 {

// Sampling profiler for the calling (emulation) thread; see profiler.cpp.
void profiler_start();
void profiler_stop();

} // namespace p2
