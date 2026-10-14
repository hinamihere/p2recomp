#pragma once

namespace p2 {

// P2_WATCHPOINT=<hex addr>,...: log writes to guest words (see watchpoint.cpp).
void watchpoint_init();

} // namespace p2
