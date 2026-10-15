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
#include <map>
#include <tuple>
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


// P2_READWATCH=<hex addr>,<hex length>: logs the first read or write of every 4 KB page in that guest range (the
// pages are made inaccessible until touched), together with the guest registers at that moment. The return
// address and arguments identify the guest function doing the access (e.g. a decompressor reading a loaded file).
namespace {
char *g_read_lo{}, *g_read_hi{};   // host range of the watched guest range (page aligned)
char *g_read_host_start{};          // host address of the first watched guest byte
u32 g_read_guest_start{};
int g_read_logs{};
bool g_read_armed{}, g_read_pending{};
u64 g_read_at{};

thread_local char *g_read_step_page{};
unsigned long long g_read_av{}, g_read_ss{};
std::map<std::tuple<bool, unsigned long long, u32>, unsigned long long> g_read_seen; // (is_read, host rip, page) -> count

LONG CALLBACK read_handler(PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && g_read_step_page != nullptr) {
        DWORD old;
        ++g_read_ss;
        VirtualProtect(g_read_step_page, g_page_size, PAGE_NOACCESS, &old);  // re-arm after the one access
        g_read_step_page = nullptr;
        info->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    char *addr = reinterpret_cast<char *>(info->ExceptionRecord->ExceptionInformation[1]);
    if (addr < g_read_lo || addr >= g_read_hi) return EXCEPTION_CONTINUE_SEARCH;
    char *page = reinterpret_cast<char *>(reinterpret_cast<uintptr_t>(addr) & ~(static_cast<uintptr_t>(g_page_size) - 1u));
    DWORD old;
    VirtualProtect(page, g_page_size, PAGE_READWRITE, &old);
    g_read_step_page = page;
    ++g_read_av;
    info->ContextRecord->EFlags |= 0x100u; // single-step the faulting instruction, then protect again
    const bool is_read = info->ExceptionRecord->ExceptionInformation[0] == 0;
    const auto &c = rt().cpu();
    const unsigned long long rip = info->ContextRecord->Rip - reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
    const auto key = std::make_tuple(is_read, rip, static_cast<u32>(reinterpret_cast<uintptr_t>(page)));
    auto it = g_read_seen.find(key);
    if (it == g_read_seen.end() && g_read_seen.size() < 400000) {
        g_read_seen[key] = 1;
        const u32 guest = g_read_guest_start + static_cast<u32>(addr - g_read_host_start);
        char line[400];
        const int n = std::snprintf(line, sizeof(line),
            "[readwatch] vblank %llu %s file+%05x: rip %llx pc %08x ra %08x sp %08x a0-a3 %08x %08x %08x %08x v0 %08x\n",
            static_cast<unsigned long long>(g_kernel.vblank_count()), is_read ? "READ " : "WRITE", guest - g_read_guest_start, rip, c.pc, c.gpr[31],
            c.gpr[29], c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7], c.gpr[2]);
        DWORD written;
        if (n > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
    } else if (it != g_read_seen.end()) {
        ++it->second;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

void readwatch_init() {
    const char *e = std::getenv("P2_READWATCH");
    if (e == nullptr) return;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size = si.dwPageSize;
    char *end = nullptr;
    const u32 start = static_cast<u32>(std::strtoul(e, &end, 16));
    const u32 length = (end != nullptr && *end == ',') ? static_cast<u32>(std::strtoul(end + 1, nullptr, 16)) : 0x1000u;
    g_read_host_start = reinterpret_cast<char *>(mem().raw_pointer(start, length));
    if (g_read_host_start == nullptr) { LOG_E("watch", "bad read-watch range"); return; }
    g_read_guest_start = start;
    const uintptr_t mask = static_cast<uintptr_t>(g_page_size) - 1u;
    g_read_lo = reinterpret_cast<char *>((reinterpret_cast<uintptr_t>(g_read_host_start) + mask) & ~mask); // whole pages inside the range only
    g_read_hi = reinterpret_cast<char *>((reinterpret_cast<uintptr_t>(g_read_host_start) + length) & ~mask);
    AddVectoredExceptionHandler(1, read_handler);
    g_read_armed = false;
    g_read_pending = true;
    const char *at = std::getenv("P2_READWATCH_AT");
    g_read_at = at ? std::strtoull(at, nullptr, 10) : 0u;
    LOG_I("watch", "read/write watch on guest %08x, %u bytes will be armed at vblank %llu", start, length, static_cast<unsigned long long>(g_read_at));
}

// Called every vblank: arms the read watch once the requested vblank is reached (after a savestate load).
void readwatch_tick() {
    DWORD old;
    if (g_read_armed) {
        if ((g_kernel.vblank_count() & 63u) == 0u) {
            char line[160];
            const int n = std::snprintf(line, sizeof(line), "[readwatch] vblank %llu faults %llu steps %llu keys %u\n",
                static_cast<unsigned long long>(g_kernel.vblank_count()), g_read_av, g_read_ss, static_cast<unsigned>(g_read_seen.size()));
            DWORD written;
            if (n > 0) WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
        }
        // Re-protect the whole range every vblank: the single-step re-arm loses pages when another host
        // thread faults in between, which silenced the watch after a dozen vblanks.
        VirtualProtect(g_read_lo, static_cast<SIZE_T>(g_read_hi - g_read_lo), PAGE_NOACCESS, &old);
        return;
    }
    if (!g_read_pending || g_kernel.vblank_count() < g_read_at) return;
    g_read_pending = false;
    g_read_armed = true;
    VirtualProtect(g_read_lo, static_cast<SIZE_T>(g_read_hi - g_read_lo), PAGE_NOACCESS, &old);
    LOG_I("watch", "read/write watch armed at vblank %llu", static_cast<unsigned long long>(g_kernel.vblank_count()));
}

} // namespace p2
#else
namespace p2 { void watchpoint_init() {} void readwatch_init() {} void readwatch_tick() {} }
#endif
