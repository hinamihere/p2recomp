#pragma once

namespace p2 {

// P2_WATCHPOINT=<hex addr>,...: log writes to guest words (see watchpoint.cpp).
void watchpoint_init();
void readwatch_tick(); // arms the read watch at P2_READWATCH_AT (called every vblank)
void readwatch_init(); // P2_READWATCH=<addr>,<len>: log the first access of each page of a guest range

} // namespace p2
