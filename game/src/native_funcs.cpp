// Hand-written replacements for guest functions (see native.hpp). Each function documents the
// guest address it replaces; the list of replaced addresses lives in game/overrides.txt.
#include "native.hpp"

namespace {

// Controller state kept by the game's input layer.
constexpr p2::u32 kPadHeld = 0x08CA29A8u;    // buttons down this frame
constexpr p2::u32 kPadPrevious = 0x08CA29A4u; // buttons down the frame before

} // namespace

namespace psprecomp {

// 08807918: bool pad_held(mask) -- any of the buttons in `mask` is down.
void p2_native_08807918(Runtime &, AllegrexContext &c) {
    c.gpr[2] = (c.gpr[4] & p2::mem().load32(kPadHeld)) != 0u ? 1u : 0u;
}

// 0880792C: bool pad_pressed(mask) -- one of the buttons went down this frame (down now, not before).
void p2_native_0880792C(Runtime &, AllegrexContext &c) {
    const p2::u32 mask = c.gpr[4];
    c.gpr[2] = ((mask & p2::mem().load32(kPadHeld)) != 0u && (mask & p2::mem().load32(kPadPrevious)) == 0u) ? 1u : 0u;
}

// 08807C58: u32 get_map_flag() -- returns the word at 08CA29DC (read by the town-map code).
void p2_native_08807C58(Runtime &, AllegrexContext &c) {
    c.gpr[2] = p2::mem().load32(0x08CA29DCu);
}

} // namespace psprecomp
