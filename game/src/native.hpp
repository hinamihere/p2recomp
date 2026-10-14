// Hand-written (decompiled) replacements for guest functions.
//
// game/overrides.txt lists the guest entries to replace; tools/regen.sh hands that list to the
// code generator (P2_OVERRIDES), which emits at each listed entry a call to
// psprecomp::p2_native_<ADDR>(Runtime&, AllegrexContext&) followed by a normal `jr $ra`, and keeps the
// original body reachable as recomp_orig_<ADDR>. A native function follows the guest ABI: arguments
// in a0.., result in v0 (v1), and it must not switch threads. It may call other guest functions
// through guest_call().
//
// P2_NATIVE=verify  run original and native on identical state and compare registers + RAM
//          =off     run the original code only (to bisect a suspected regression)
//          unset    run the native code
// P2_NATIVE_VERIFY_LIMIT=<n> calls verified per function (default 300), P2_NATIVE_STRICT=1 also
// compares caller-saved temporaries.
#pragma once

#include "hle.hpp"

#include <random>

namespace p2 {

void native_init();
void native_report();

// P2_NATIVE_FUZZ=<n>: before booting, run every replaced function n times on random arguments and
// random RAM (shaped by native_fuzz_setup in native_funcs.cpp) against the original, then exit.
void native_fuzz(u64 iterations);
void native_fuzz_setup(u32 address, std::mt19937 &rng, AllegrexContext &c);

// Runs the guest function at `address` to completion and returns with its results in `c`.
// Only for functions that do not block or switch threads. Returns false if it did not complete.
bool guest_call(Runtime &r, AllegrexContext &c, u32 address);

} // namespace p2
