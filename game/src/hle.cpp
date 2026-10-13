#include "hle.hpp"
#include "savestate.hpp"

#include "kernel.hpp"

#include <cstring>
#include <set>
#include <stdexcept>

namespace p2 {

LogLevel g_log_level = LogLevel::Info;

void log_message(LogLevel level, const char *channel, const char *fmt, ...) {
    static const char *names[] = {"E", "W", "I", "D", "T"};
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    const u32 vblank = static_cast<u32>(g_kernel.vblank_count());
    std::fprintf(stderr, "[%s %6u %-6s] %s\n", names[static_cast<int>(level)], vblank, channel, buffer);
}

std::string guest_string(u32 address, std::size_t max_length) {
    if (address == 0u) return "(null)";
    try {
        return mem().read_c_string(address, max_length);
    } catch (const std::exception &) {
        return "(bad ptr)";
    }
}

bool valid_range(u32 address, u32 length) {
    return address != 0u && mem().contains(address, length == 0u ? 1u : length);
}

void write_u64(u32 address, u64 value) {
    mem().store32(address, static_cast<u32>(value));
    mem().store32(address + 4u, static_cast<u32>(value >> 32));
}

u64 read_u64(u32 address) {
    return static_cast<u64>(mem().load32(address)) | (static_cast<u64>(mem().load32(address + 4u)) << 32);
}

namespace {
struct NidEntry {
    const char *library;
    u32 nid;
    const char *name;
};
constexpr NidEntry kImports[] = {
#include "nid_table.inc"
};
std::set<std::pair<std::string, u32>> g_registered;
} // namespace

void hle(const char *library, const char *name, HleFn fn) {
    for (const auto &entry : kImports) {
        if (std::strcmp(entry.library, library) != 0 || std::strcmp(entry.name, name) != 0) continue;
        g_registered.insert({entry.library, entry.nid});
        const char *label = entry.name;
        rt().register_hle(entry.library, entry.nid, [fn, label](Runtime &, AllegrexContext &c) {
            LOG_T("hle", "%s(%08x, %08x, %08x, %08x) ra=%08x", label, c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7], c.gpr[31]);
            const u64 generation = psprecomp::runtime_thread_switch_generation();
            g_kernel.in_hle_call = true;
            fn(c);
            g_kernel.in_hle_call = false;
            // Code polling an import (e.g. the vcount) never returns to the dispatcher, so
            // savestates are also taken right after an import returns on the same thread.
            if (savestate_requested() && psprecomp::runtime_thread_switch_generation_matches(generation))
                savestate_service_at_hle_return();
        });
        return;
    }
    LOG_W("hle", "handler for %s::%s does not match any import", library, name);
}

void report_unimplemented_imports() {
    int missing = 0;
    for (const auto &entry : kImports) {
        if (g_registered.contains({entry.library, entry.nid})) continue;
        LOG_D("hle", "unimplemented: %s::%s", entry.library, entry.name);
        ++missing;
    }
    if (missing != 0) LOG_I("hle", "%d of %u imports have no handler", missing, static_cast<unsigned>(std::size(kImports)));
}

} // namespace p2
