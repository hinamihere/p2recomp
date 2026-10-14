#pragma once

#include "hle.hpp"

namespace p2 {

void register_ge_imports();
// Finishes all queued drawing (the renderer batches primitives across stalled lists).
void ge_flush();

} // namespace p2
