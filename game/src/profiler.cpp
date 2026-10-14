// P2_PROFILE=1: a sampling profiler for the host process. A helper thread suspends the
// emulation thread about 1000 times a second, records its instruction pointer and resumes
// it; at exit the histogram (module-relative addresses) goes to build/profile_raw.txt for
// tools/profile.py to symbolize.
#include "profiler.hpp"

#include "hle.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace p2 {
namespace {

std::thread g_thread;
std::atomic<bool> g_running{false};
std::unordered_map<u64, u32> g_hist;
u64 g_samples{};

} // namespace

void profiler_start() {
    HANDLE target = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0)) {
        LOG_W("prof", "cannot open the main thread");
        return;
    }
    g_running = true;
    g_thread = std::thread([target] {
        while (g_running) {
            u64 rip = 0;
            if (SuspendThread(target) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(target, &ctx)) rip = ctx.Rip;
                ResumeThread(target);
            }
            // Allocate only after resuming: the target may have held the heap lock.
            if (rip != 0u) {
                ++g_hist[rip];
                ++g_samples;
            }
            Sleep(1);
        }
        CloseHandle(target);
    });
    LOG_I("prof", "sampling profiler running");
}

void profiler_stop() {
    if (!g_running) return;
    g_running = false;
    g_thread.join();
    const u64 base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    std::FILE *f = std::fopen("build/profile_raw.txt", "w");
    if (f == nullptr) return;
    std::fprintf(f, "# samples %llu base %llx\n", static_cast<unsigned long long>(g_samples), static_cast<unsigned long long>(base));
    for (const auto &[rip, count] : g_hist) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module) && module != nullptr) {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        const u64 mbase = reinterpret_cast<u64>(module);
        std::fprintf(f, "%llx %u %s\n", static_cast<unsigned long long>(rip - mbase), count, name);
    }
    std::fclose(f);
    LOG_I("prof", "%llu samples written to build/profile_raw.txt", static_cast<unsigned long long>(g_samples));
}

} // namespace p2
#else
namespace p2 {
void profiler_start() { LOG_W("prof", "the sampling profiler is Windows-only"); }
void profiler_stop() {}
} // namespace p2
#endif
