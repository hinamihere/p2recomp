// P2_TRACE_LABELS=<first vblank>-<last vblank>: records labels executed in instrumented generated
// units (debug only: see tools/ and docs; the instrumentation is applied by hand to a unit).
#include "hle.hpp"
#include "kernel.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned long long g_first = 1, g_last = 0;
std::FILE *g_file = nullptr;
bool g_init = false;
} // namespace

extern "C" void p2_trace_label(unsigned label) {
    if (!g_init) {
        g_init = true;
        if (const char *e = std::getenv("P2_TRACE_LABELS")) {
            g_first = std::strtoull(e, nullptr, 0);
            const char *dash = std::strchr(e, '-');
            g_last = dash != nullptr ? std::strtoull(dash + 1, nullptr, 0) : g_first;
            g_file = std::fopen("build/label_trace.txt", "w");
        }
    }
    if (g_file == nullptr) return;
    const unsigned long long v = p2::g_kernel.vblank_count();
    if (v < g_first || v > g_last) return;
    std::fprintf(g_file, "%llu %08X\n", v, label);
}
