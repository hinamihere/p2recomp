#include "native.hpp"

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <random>
#include <vector>

namespace psprecomp {
std::uint32_t g_p2_native_mode = 0; // 0 native, 1 verify, 2 original only
}

namespace p2 {
namespace {

using NativeFn = void (*)(Runtime &, AllegrexContext &);

constexpr u32 kReturnSentinel = 0x08800130u; // outside every generated unit and every HLE address
constexpr u32 kRamBase = 0x08000000u;

struct Stats {
    u64 verified{};
    u64 mismatches{};
    bool announced{};
};
struct Registered { u32 addr; NativeFn native; NativeFn original; };
std::vector<Registered> &registry() {
    static std::vector<Registered> list;
    return list;
}

std::map<u32, Stats> g_stats;
std::set<u32> &void_functions() {
    static std::set<u32> set;
    return set;
}
u64 g_limit = 300;
bool g_strict = false;
std::vector<u8> g_before, g_after;

// Runs `entry` (a unit entry or an original wrapper) until it returns to the sentinel.
bool run_to_sentinel(Runtime &r, AllegrexContext &c, NativeFn entry) {
    const u32 saved_ra = c.gpr[31];
    const u64 generation = psprecomp::runtime_thread_switch_generation();
    c.gpr[31] = kReturnSentinel;
    entry(r, c);
    u32 guard = 0;
    while (c.pc != kReturnSentinel && !r.stopped()) {
        if (++guard > 2'000'000u || !psprecomp::runtime_thread_switch_generation_matches(generation) ||
            !r.invoke_isolated_aot(c.pc, c)) {
            LOG_E("native", "guest call from %08x did not complete (pc %08x, steps %u)", saved_ra, c.pc, guard);
            c.gpr[31] = saved_ra;
            return false;
        }
    }
    c.gpr[31] = saved_ra;
    return !r.stopped();
}

bool same_reg(const AllegrexContext &a, const AllegrexContext &b, int i) { return a.gpr[i] == b.gpr[i]; }

// Compares what a caller may rely on: v0, callee-saved registers, sp/ra, f0 and f20-f31 (v1 only in strict mode: most functions return 32 bits).
bool compare_state(u32 addr, const AllegrexContext &want, const AllegrexContext &got) {
    static const int kChecked[] = {2, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    bool ok = true;
    auto report = [&](const char *what, int index, u32 w, u32 g) {
        ok = false;
        LOG_E("native", "%08x MISMATCH %s%d: original %08x native %08x", addr, what, index, w, g);
    };
    const bool has_result = !void_functions().contains(addr);
    for (int i : kChecked) if ((i != 2 || has_result) && !same_reg(want, got, i)) report("r", i, want.gpr[i], got.gpr[i]);
    if (g_strict)
        for (int i = 1; i < 32; ++i)
            if (!same_reg(want, got, i)) report("strict r", i, want.gpr[i], got.gpr[i]);
    for (int i = 0; i < 32; ++i) {
        if (!(g_strict || i == 0 || i >= 20)) continue;
        if (want.fpr_bits(i) != got.fpr_bits(i)) report("f", i, want.fpr_bits(i), got.fpr_bits(i));
    }
    return ok;
}

// The callee's own stack frame (below the caller's $sp) is dead memory afterwards: a replacement does not
// have to reproduce the original's spills, so differences there are ignored.
bool compare_ram(u32 addr, const u8 *want, const u8 *got, size_t size, u32 dead_lo, u32 dead_hi) {
    if (std::memcmp(want, got, size) == 0) return true;
    bool same = true;
    int shown = 0;
    for (size_t i = 0; i + 4 <= size; i += 4) {
        const u32 guest_addr = kRamBase + static_cast<u32>(i);
        if (guest_addr >= dead_lo && guest_addr < dead_hi) continue;
        if (std::memcmp(want + i, got + i, 4) != 0) {
            same = false;
            if (shown >= 6) continue;
            u32 w, g;
            std::memcpy(&w, want + i, 4);
            std::memcpy(&g, got + i, 4);
            LOG_E("native", "%08x MISMATCH ram %08x: original %08x native %08x", addr, kRamBase + static_cast<u32>(i), w, g);
            ++shown;
        }
    }
    return same;
}

// Runs original and native on identical state and compares them. Leaves `c` and RAM in the original's
// state on a mismatch, otherwise in the native's (identical) state.
bool verify_call(Runtime &r, AllegrexContext &c, u32 addr, void (*native)(Runtime &, AllegrexContext &),
                 void (*original)(Runtime &, AllegrexContext &)) {
    const size_t size = r.memory().size();
    u8 *ram = r.memory().raw_pointer(kRamBase, size);
    g_before.assign(ram, ram + size);
    const AllegrexContext start = c;

    const bool completed = run_to_sentinel(r, c, original);
    const AllegrexContext original_result = c;
    g_after.assign(ram, ram + size);
    if (!completed) return false;

    std::memcpy(ram, g_before.data(), size);
    c = start;
    native(r, c);

    bool ok = compare_state(addr, original_result, c);
    ok &= compare_ram(addr, g_after.data(), ram, size, start.gpr[29] - 0x2000u, start.gpr[29]);
    if (!ok) {
        std::memcpy(ram, g_after.data(), size); // keep the game on the original's behaviour
        c = original_result;
        c.gpr[31] = start.gpr[31];
    }
    return ok;
}

} // namespace

namespace detail {
VoidMark::VoidMark(u32 address) { void_functions().insert(address); }
} // namespace detail

void *guest_raw(u32 address, std::size_t size) {
    if (void *p = mem().raw_pointer(address, size)) return p;
    alignas(16) static u8 dummy[4096];
    LOG_E("native", "bad guest address %08x (%zu bytes) in native code", address, size);
    rt().stop("bad guest pointer in native code");
    return dummy;
}

u32 guest_address(const void *host) {
    const auto *p = static_cast<const u8 *>(host);
    const u8 *ram = mem().raw_pointer(kRamBase, 1);
    if (p >= ram && p < ram + mem().size()) return kRamBase + static_cast<u32>(p - ram);
    const u8 *vram = mem().raw_pointer(0x04000000u, 1);
    if (vram && p >= vram && p < vram + mem().vram_size()) return 0x04000000u + static_cast<u32>(p - vram);
    return 0;
}

bool call_original(Runtime &r, AllegrexContext &c, u32 address) {
    for (const auto &entry : registry())
        if (entry.addr == address) return run_to_sentinel(r, c, entry.original);
    LOG_E("native", "call_original: %08x is not a replaced function", address);
    return false;
}

bool guest_call(Runtime &r, AllegrexContext &c, u32 address) {
    if (!r.has_function(address)) {
        LOG_E("native", "guest_call: no code at %08x", address);
        return false;
    }
    c.pc = address;
    return run_to_sentinel(r, c, [](Runtime &rr, AllegrexContext &cc) { (void)rr.invoke_isolated_aot(cc.pc, cc); });
}

void native_init() {
    if (const char *mode = std::getenv("P2_NATIVE")) {
        if (std::strcmp(mode, "verify") == 0) psprecomp::g_p2_native_mode = 1;
        else if (std::strcmp(mode, "off") == 0) psprecomp::g_p2_native_mode = 2;
    }
    if (const char *limit = std::getenv("P2_NATIVE_VERIFY_LIMIT")) g_limit = std::strtoull(limit, nullptr, 10);
    g_strict = std::getenv("P2_NATIVE_STRICT") != nullptr;
    if (psprecomp::g_p2_native_mode != 0u)
        LOG_I("native", "mode: %s", psprecomp::g_p2_native_mode == 1u ? "verify" : "original code only");
}

void native_report() {
    for (const auto &[addr, s] : g_stats)
        LOG_I("native", "%08x: %llu verified, %llu mismatches", addr, static_cast<unsigned long long>(s.verified),
              static_cast<unsigned long long>(s.mismatches));
}

} // namespace p2

namespace psprecomp {

void p2_native_register(std::uint32_t addr, void (*native)(Runtime &, AllegrexContext &),
                        void (*original)(Runtime &, AllegrexContext &)) {
    p2::registry().push_back({addr, native, original});
}

// Slow path of every replaced entry (mode != 0): run the original, or compare native against it.
void p2_native_slow(Runtime &r, AllegrexContext &c, std::uint32_t addr, void (*native)(Runtime &, AllegrexContext &),
                    void (*original)(Runtime &, AllegrexContext &)) {
    using namespace p2;
    if (g_p2_native_mode == 2u) {
        run_to_sentinel(r, c, original);
        return;
    }
    Stats &stats = g_stats[addr];
    if (stats.verified >= g_limit) {
        if (!stats.announced) {
            stats.announced = true;
            LOG_I("native", "%08x: %llu calls verified, %llu mismatches; native from now on", addr,
                  static_cast<unsigned long long>(stats.verified), static_cast<unsigned long long>(stats.mismatches));
        }
        native(r, c);
        return;
    }
    ++stats.verified;
    if (!verify_call(r, c, addr, native, original)) ++stats.mismatches;
}

} // namespace psprecomp

namespace p2 {

void native_fuzz(u64 iterations) {
    std::mt19937 rng(0x5EEDu);
    Runtime &r = rt();
    int failures = 0;
    for (const auto &entry : registry()) {
        u64 bad = 0;
        for (u64 i = 0; i < iterations; ++i) {
            AllegrexContext c{};
            c.gpr[29] = 0x09F00000u; // scratch stack inside RAM
            c.gpr[31] = 0x08804000u;
            for (int a = 4; a < 12; ++a) c.gpr[a] = rng();
            native_fuzz_setup(entry.addr, rng, c);
            // The comparison functions return to the sentinel, so $ra only needs to survive.
            if (!verify_call(r, c, entry.addr, entry.native, entry.original)) ++bad;
        }
        LOG_I("native", "fuzz %08x: %llu iterations, %llu mismatches", entry.addr,
              static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(bad));
        failures += bad != 0u;
    }
    LOG_I("native", "fuzz done: %zu functions, %d with mismatches", registry().size(), failures);
}

} // namespace p2
