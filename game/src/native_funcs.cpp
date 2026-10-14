// Hand-written replacements for guest functions (see native.hpp). Each function documents the
// guest address it replaces; the list of replaced addresses lives in game/overrides.txt.
#include "native.hpp"

namespace {

// Controller state kept by the game's input layer.
constexpr p2::u32 kPadHeld = 0x08CA29A8u;    // buttons down this frame
constexpr p2::u32 kPadPrevious = 0x08CA29A4u; // buttons down the frame before

// Button remapping: twelve enable flags (two sets, A and B) select which of the twelve configured button
// masks in kButtonMasks count as "down" for the logical-button queries below.
constexpr p2::u32 kFlagsA = 0x08CA292Cu;
constexpr p2::u32 kFlagsB = 0x08D26F68u;
constexpr p2::u32 kButtonMasks = 0x08BD5FA0u;
constexpr int kButtonCount = 12;

p2::u32 mapped_buttons(p2::u32 flags) {
    p2::u32 mask = 0;
    for (int i = 0; i < kButtonCount; ++i)
        if (p2::mem().load8(flags + i) != 0u) mask |= p2::mem().load32(kButtonMasks + 4u * i);
    return mask;
}

// A d-pad direction only counts while none of the other three d-pad buttons is down (no diagonals).
bool dpad_blocked(p2::u32 mask) {
    constexpr p2::u32 kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80;
    const p2::u32 held = p2::mem().load32(kPadHeld);
    switch (mask) {
    case kUp: return (held & (kDown | kLeft | kRight)) != 0u;
    case kRight: return (held & (kDown | kLeft | kUp)) != 0u;
    case kDown: return (held & (kUp | kLeft | kRight)) != 0u;
    case kLeft: return (held & (kDown | kUp | kRight)) != 0u;
    default: return false;
    }
}

} // namespace

namespace p2 {

// Random inputs for the differential fuzzer (P2_NATIVE_FUZZ): arguments are already random; here the RAM
// the replaced function reads is randomized too, biased so every branch gets exercised.
void native_fuzz_setup(u32 address, std::mt19937 &rng, AllegrexContext &c) {
    auto bits = [&](u32 count) { // random word with about `count` bits set
        u32 w = 0;
        for (u32 i = 0; i < count; ++i) w |= 1u << (rng() % 32u);
        return w;
    };
    // d-pad bits are 0x10-0x80; keep them frequent in both pad words and in the mask argument
    mem().store32(kPadHeld, (rng() & 1u) ? (bits(2) & 0xF0u) | bits(2) : bits(4));
    mem().store32(kPadPrevious, (rng() & 1u) ? (bits(2) & 0xF0u) | bits(2) : bits(4));
    for (int i = 0; i < kButtonCount; ++i) {
        mem().store8(kFlagsA + i, rng() & 1u ? static_cast<u8>(rng()) : 0u);
        mem().store8(kFlagsB + i, rng() & 1u ? static_cast<u8>(rng()) : 0u);
        mem().store32(kButtonMasks + 4u * i, bits(1 + rng() % 3u));
    }
    switch (rng() % 4u) {
    case 0: c.gpr[4] = 0x10u << (rng() % 4u); break; // a single d-pad direction
    case 1: c.gpr[4] = bits(1 + rng() % 3u); break;
    default: break;                                    // fully random
    }
    mem().store32(0x08CA29DCu, rng());
    (void)address;
}

} // namespace p2

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

// 08807968: bool pad_mapped_b(mask) -- a button of `mask` is mapped (flag set B).
void p2_native_08807968(Runtime &, AllegrexContext &c) {
    c.gpr[2] = (mapped_buttons(kFlagsB) & c.gpr[4]) != 0u ? 1u : 0u;
}

// 08807AC4: bool pad_mapped_b(mask) -- identical copy of 08807968 (the compiler emitted both).
void p2_native_08807AC4(Runtime &, AllegrexContext &c) {
    c.gpr[2] = (mapped_buttons(kFlagsB) & c.gpr[4]) != 0u ? 1u : 0u;
}

// 088079AC: u32 pad_dir_a(mask) -- mapped buttons (set A) & mask, with d-pad directions made exclusive.
void p2_native_088079AC(Runtime &, AllegrexContext &c) {
    const p2::u32 mask = c.gpr[4];
    c.gpr[2] = dpad_blocked(mask) ? 0u : (mapped_buttons(kFlagsA) & mask);
}

// 08807B08: bool pad_dir_b(mask) -- same with set B, returning 0/1.
void p2_native_08807B08(Runtime &, AllegrexContext &c) {
    const p2::u32 mask = c.gpr[4];
    c.gpr[2] = (!dpad_blocked(mask) && (mapped_buttons(kFlagsB) & mask) != 0u) ? 1u : 0u;
}

// 08807C58: u32 get_map_flag() -- returns the word at 08CA29DC (read by the town-map code).
void p2_native_08807C58(Runtime &, AllegrexContext &c) {
    c.gpr[2] = p2::mem().load32(0x08CA29DCu);
}

} // namespace psprecomp
