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
#include <type_traits>
#include <utility>

namespace p2 {

void native_init();
void native_report();

// P2_NATIVE_FUZZ=<n>: before booting, run every replaced function n times on random arguments and
// random RAM (shaped by native_fuzz_setup in native_funcs.cpp) against the original, then exit.
void native_fuzz(u64 iterations);
void native_fuzz_setup(u32 address, std::mt19937 &rng, AllegrexContext &c);

// ---- typed replacements ---------------------------------------------------------------------------------
// A replacement is an ordinary C++ function; P2_NATIVE(<ADDR>, function) binds it to the guest entry:
// arguments are taken from a0.. by their C++ types (integers are truncated or sign-extended, pointers are
// guest addresses turned into host pointers into guest RAM), the result goes to v0. P2_NATIVE_VOID marks a
// function whose v0 is not a result. Guest memory is little-endian like the host, so guest structs can be
// declared as plain C++ structs and read through guest<T>().

// Host pointer for a guest address (RAM or VRAM); a bad address stops the run and returns a dummy buffer.
void *guest_raw(u32 address, std::size_t size);
u32 guest_address(const void *host);
template <class T> T *guest(u32 address) { return static_cast<T *>(guest_raw(address, sizeof(T))); }

// A value at a fixed guest address, usable like a variable: g_rng_seed = g_rng_seed + 1;
template <class T, u32 Address> struct Global {
    T &get() const { return *guest<T>(Address); }
    operator T &() const { return get(); }
    T *operator->() const { return &get(); }
    Global &operator=(const T &value) { get() = value; return *this; }
};

namespace detail {
template <class T> T from_reg(u32 reg) {
    if constexpr (std::is_pointer_v<T>) return guest<std::remove_pointer_t<T>>(reg);
    else return static_cast<T>(reg);
}
template <class T> u32 to_reg(T value) {
    if constexpr (std::is_pointer_v<T>) return guest_address(value);
    else return static_cast<u32>(value);
}
template <auto Function> struct Thunk;
template <class R, class... Args, R (*Function)(Args...)> struct Thunk<Function> {
    static void call(Runtime &, AllegrexContext &c) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (std::is_void_v<R>) Function(from_reg<Args>(c.gpr[4 + I])...);
            else c.gpr[2] = to_reg<R>(Function(from_reg<Args>(c.gpr[4 + I])...));
        }(std::index_sequence_for<Args...>{});
    }
};
struct VoidMark { explicit VoidMark(u32 address); };
} // namespace detail

} // namespace p2

#define P2_NATIVE(ADDR, FUNCTION)     namespace psprecomp { void p2_native_##ADDR(Runtime &r, AllegrexContext &c) { ::p2::detail::Thunk<&FUNCTION>::call(r, c); } }
#define P2_NATIVE_VOID(ADDR, FUNCTION)     P2_NATIVE(ADDR, FUNCTION)     namespace { const ::p2::detail::VoidMark p2_void_##ADDR{0x##ADDR}; }

namespace p2 {

// Runs the guest function at `address` to completion and returns with its results in `c`.
// Only for functions that do not block or switch threads. Returns false if it did not complete.
bool guest_call(Runtime &r, AllegrexContext &c, u32 address);

} // namespace p2
