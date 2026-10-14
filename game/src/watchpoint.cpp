// P2_WATCHPOINT=<hex guest addr>[,<hex guest addr>...]: logs every write to those guest 32-bit words
// (old -> new, plus the host address of the writing instruction and the guest thread).
//
// Implemented with page protection: the page holding a watched word is made read-only; the
// vectored exception handler lets one write through (single-step), compares the value and
// logs a change, and re-protects the page. It needs no hook in generated code. Map the host
// addresses to guest functions with tools/profile.py's symbolization (a -g1 build).
#include "watchpoint.hpp"

#include "hle.hpp"
#include "kernel.hpp"

#if defined(_WIN32)
#include <windows.h>

#include <cstdlib>
#include <vector>

namespace p2 {
namespace {

struct Watch { u32 guest; volatile u32 *host; u32 last; };
std::vector<Watch> g_watches;
std::vector<std::pair<void *, SIZE_T>> g_pages;
DWORD64 g_fault_rip{};
SIZE_T g_page_size{4096};
unsigned long long g_events{};

void protect(bool on) {
    for (auto &[page, size] : g_pages) {
        DWORD old;
        VirtualProtect(page, size, on ? PAGE_READONLY : PAGE_READWRITE, &old);
    }
}

LONG CALLBACK handler(PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->ExceptionInformation[0] == 1) {
        const auto addr = reinterpret_cast<void *>(info->ExceptionRecord->ExceptionInformation[1]);
        for (auto &[page, size] : g_pages) {
            if (addr >= page && addr < static_cast<char *>(page) + size) {
                g_fault_rip = info->ContextRecord->Rip;
                protect(false);                          // let this one write through
                info->ContextRecord->EFlags |= 0x100u;   // trap flag: single step
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    } else if (code == EXCEPTION_SINGLE_STEP && g_fault_rip != 0u) {
        for (auto &w : g_watches) {
            const u32 now = *w.host;
            if (now != w.last) {
                ++g_events;
                if (g_events <= 4000u) {
                    char line[160];
                    const int n = std::snprintf(line, sizeof(line), "[watchpoint] vblank %llu guest %08x: %08x -> %08x  host-rip %llx thread %x\n",
                                                static_cast<unsigned long long>(g_kernel.vblank_count()), w.guest, w.last, now,
                                                static_cast<unsigned long long>(g_fault_rip - reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr))),
                                                static_cast<unsigned>(g_kernel.current_uid()));
                    DWORD written;
                    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
                    if (n > 0) WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
                }
                w.last = now;
            }
        }
        g_fault_rip = 0u;
        protect(true);
        info->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void watchpoint_init() {
    const char *e = std::getenv("P2_WATCHPOINT");
    if (e == nullptr) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size = si.dwPageSize;
    for (const char *p = e; *p != '\0';) {
        char *end = nullptr;
        const u32 addr = static_cast<u32>(std::strtoul(p, &end, 16));
        auto *host = reinterpret_cast<volatile u32 *>(mem().raw_pointer(addr, 4));
        if (host != nullptr) {
            g_watches.push_back({addr, host, *host});
            void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(host) & ~(static_cast<uintptr_t>(g_page_size) - 1u));
            bool have = false;
            for (auto &[pg, sz] : g_pages) have |= pg == page;
            if (!have) g_pages.emplace_back(page, g_page_size);
        }
        p = (end != nullptr && *end == ',') ? end + 1 : (end == p ? p + 1 : end);
        if (end == nullptr || *end == '\0') break;
    }
    if (g_watches.empty()) return;
    AddVectoredExceptionHandler(1, handler);
    protect(true);
    LOG_I("watch", "write watchpoints on %u words", static_cast<unsigned>(g_watches.size()));
}

} // namespace p2
#else
namespace p2 { void watchpoint_init() {} }
#endif
