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

    // Scratch buffers for pointer arguments: random bytes, three disjoint areas.
    constexpr u32 kScratch = 0x09E00000u;
    auto fill = [&](u32 base, u32 bytes) {
        for (u32 i = 0; i < bytes; i += 4) mem().store32(base + i, rng());
    };
    switch (address) {
    case 0x08851C30u:
        mem().store32(0x08E34150u, (rng() & 7u) == 0u ? 0u : rng());
        break;
    case 0x08863DF4u:
        fill(kScratch, 0x300);
        c.gpr[4] = kScratch;
        c.gpr[5] = kScratch + 0x100;
        c.gpr[6] = (rng() & 3u) == 0u ? kScratch + 0x100 : kScratch + 0x200; // sometimes in place
        break;
    case 0x088FE140u:
        fill(kScratch, 0x400);
        mem().store16(kScratch, rng() % 12u);
        mem().store16(kScratch + 2, 2u * (1 + rng() % 16u));
        c.gpr[4] = kScratch;
        break;
    case 0x088FDF74u:
        fill(kScratch, 0x400);
        c.gpr[4] = kScratch;
        c.gpr[5] = static_cast<u32>(static_cast<s32>(rng() % 24u) - 3);
        c.gpr[6] = 2u * (1 + rng() % 16u);
        break;
    case 0x08866818u:
    case 0x08866850u:
        fill(0x08BB6584u, 4096u * 4u);
        break;
    case 0x08963E4Cu:
        mem().store32(0x08CE2CF4u, rng());
        break;
    case 0x088A0508u:
        c.gpr[4] &= 0x3Fu;
        fill(0x08E4452Cu, 0x100);
        break;
    default: break;
    }
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

// 08851C30: u32 rand() -- the game's random number generator (31-bit result, state word at 08E34150).
void p2_native_08851C30(Runtime &, AllegrexContext &c) {
    constexpr p2::u32 kSeed = 0x08E34150u;
    const p2::u32 seed = p2::mem().load32(kSeed);
    const p2::u32 t = seed * 41u;
    const p2::u32 hi = t >> 16;
    const p2::u32 next = seed == 0u ? 0x2A6D365Au : (hi | ((t + hi) << 16));
    p2::mem().store32(kSeed, next);
    c.gpr[2] = next >> 1;
}

// 08863DF4: s16 *vec3_mul_mat3(const s16 m[9], const s16 v[3], s16 out[3]) -- out = v * m in 4.12 fixed
// point (rows of m are the basis vectors; the sums wrap at 32 bits like the original mult/madd chain).
void p2_native_08863DF4(Runtime &, AllegrexContext &c) {
    auto s16at = [](p2::u32 a) { return static_cast<p2::s32>(static_cast<p2::s16>(p2::mem().load16(a))); };
    p2::s32 m[9], v[3];
    for (int i = 0; i < 9; ++i) m[i] = s16at(c.gpr[4] + 2u * i);
    for (int i = 0; i < 3; ++i) v[i] = s16at(c.gpr[5] + 2u * i);
    for (int col = 0; col < 3; ++col) {
        const p2::u32 sum = static_cast<p2::u32>(v[0] * m[col]) + static_cast<p2::u32>(v[1] * m[3 + col]) +
                            static_cast<p2::u32>(v[2] * m[6 + col]);
        p2::mem().store16(c.gpr[6] + 2u * col, static_cast<p2::u16>(static_cast<p2::s32>(sum) >> 12));
    }
    c.gpr[2] = c.gpr[6];
}

// 088FE140: int pool_count_flagged(pool) -- pool = {s16 count, s16 stride, ..., slots from +0x10};
// counts the slots whose first halfword has bit 0 set.
void p2_native_088FE140(Runtime &, AllegrexContext &c) {
    const p2::u32 pool = c.gpr[4];
    const p2::s32 count = static_cast<p2::s16>(p2::mem().load16(pool));
    p2::u32 flagged = 0;
    if (count > 0) {
        const p2::s32 stride = static_cast<p2::s16>(p2::mem().load16(pool + 2));
        p2::u32 slot = pool + 16;
        for (p2::s32 i = 0; i < count; ++i, slot += static_cast<p2::u32>(stride)) flagged += p2::mem().load16(slot) & 1u;
    }
    c.gpr[2] = flagged;
}

// 088FDF74: void pool_init(pool, s16 count, s16 stride, a3, t0) -- header {count, stride, 0, a3, t0}, then
// clears the first halfword of each slot. Returns (by accident) the address past the last slot.
void p2_native_088FDF74(Runtime &, AllegrexContext &c) {
    const p2::u32 pool = c.gpr[4];
    const p2::s32 count = static_cast<p2::s16>(c.gpr[5]);
    const p2::s32 stride = static_cast<p2::s16>(c.gpr[6]);
    p2::mem().store32(pool + 0xC, c.gpr[8]);
    p2::mem().store32(pool + 8, c.gpr[7]);
    p2::mem().store16(pool, static_cast<p2::u16>(count));
    p2::mem().store16(pool + 2, static_cast<p2::u16>(stride));
    p2::mem().store32(pool + 4, 0);
    p2::u32 slot = pool + 16;
    for (p2::s32 i = 0; i < count; ++i, slot += static_cast<p2::u32>(stride)) p2::mem().store16(slot, 0);
    c.gpr[2] = slot;
}

// 08866818 / 08866850: s32 sin_table(angle) / cos_table(angle) -- 12-bit angle, 4.12 fixed-point result from a
// table of 16-bit entries (values with bit 15 set encode negative numbers as 0x8000 | (0x1000 + x)).
p2::u32 trig_entry(p2::u32 angle, p2::u32 offset) {
    const p2::u32 v = p2::mem().load16(0x08BB6584u + (angle & 0xFFFu) * 4u + offset);
    return v > 0x7FFFu ? (v & 0xFFFu) - 0x1000u : v;
}
void p2_native_08866818(Runtime &, AllegrexContext &c) { c.gpr[2] = trig_entry(c.gpr[4], 0); }
void p2_native_08866850(Runtime &, AllegrexContext &c) { c.gpr[2] = trig_entry(c.gpr[4], 2); }

// 08963E4C: u32 get_pool_ptr() -- word at 08CE2CF4.
void p2_native_08963E4C(Runtime &, AllegrexContext &c) { c.gpr[2] = p2::mem().load32(0x08CE2CF4u); }

// 088A0508: u32 get_map_slot(i) -- i-th word of the array at 08E4452C.
void p2_native_088A0508(Runtime &, AllegrexContext &c) { c.gpr[2] = p2::mem().load32(0x08E4452Cu + (c.gpr[4] << 2)); }

// 08807C58: u32 get_map_flag() -- returns the word at 08CA29DC (read by the town-map code).
void p2_native_08807C58(Runtime &, AllegrexContext &c) {
    c.gpr[2] = p2::mem().load32(0x08CA29DCu);
}

} // namespace psprecomp
