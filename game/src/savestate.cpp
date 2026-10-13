#include "savestate.hpp"

#include "kernel.hpp"

#include <fstream>

namespace p2 {
namespace {

constexpr u32 kMagic = 0x53533250u; // "P2SS"
constexpr u32 kVersion = 2u;
constexpr u32 kRamBase = 0x08000000u, kRamSize = 32u * 1024u * 1024u;
constexpr u32 kVramBase = 0x04000000u, kVramSize = 2u * 1024u * 1024u;

std::filesystem::path g_dir{"states"};
int g_pending_save = -1;
int g_pending_load = -1;
int g_scheduled_slot = -1;
u64 g_scheduled_vblank = 0;
u64 g_blocked_since = 0;

std::filesystem::path slot_path(int slot) { return g_dir / ("slot" + std::to_string(slot) + ".p2s"); }

void save_memory(StateWriter &w) {
    auto &m = mem();
    w.section("MEM ");
    w.bytes(m.raw_pointer(kRamBase, kRamSize), kRamSize);
    w.bytes(m.raw_pointer(kVramBase, kVramSize), kVramSize);
}

void load_memory(StateReader &r) {
    auto &m = mem();
    r.section("MEM ");
    r.bytes(m.raw_pointer(kRamBase, kRamSize), kRamSize);
    r.bytes(m.raw_pointer(kVramBase, kVramSize), kVramSize);
}

std::string blocker() {
    std::string b = kernel_save_blocker();
    if (b.empty()) b = mpeg_save_blocker();
    return b;
}

bool do_save(int slot) {
    StateWriter w;
    w.pod(kMagic);
    w.pod(kVersion);
    w.pod(g_kernel.module_gp());
    w.pod(g_kernel.vblank_count()); // informational, read by tools/drive.py
    save_memory(w);
    kernel_save(w);
    ge_save(w);
    display_save(w);
    audio_save(w);
    sas_save(w);
    atrac_save(w);
    io_save(w);
    utility_save(w);
    savedata_save(w);
    w.section("END ");
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::ofstream out(slot_path(slot), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
    LOG_I("state", "saved slot %d (%u KiB) at vblank %llu", slot, static_cast<unsigned>(w.buf.size() / 1024u),
          static_cast<unsigned long long>(g_kernel.vblank_count()));
    return static_cast<bool>(out);
}

bool do_load(int slot) {
    std::ifstream in(slot_path(slot), std::ios::binary);
    if (!in) {
        LOG_W("state", "no savestate in slot %d", slot);
        return false;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    StateReader r(std::move(data));
    try {
        if (r.pod<u32>() != kMagic || r.pod<u32>() != kVersion) throw std::runtime_error("not a compatible savestate");
        if (r.pod<u32>() != g_kernel.module_gp()) throw std::runtime_error("savestate is for a different executable");
        r.pod<u64>();
        load_memory(r);
        kernel_load(r);
        ge_load(r);
        display_load(r);
        audio_load(r);
        sas_load(r);
        atrac_load(r);
        io_load(r);
        utility_load(r);
        savedata_load(r);
        r.section("END ");
    } catch (const std::exception &e) {
        // Memory may be partially overwritten; there is no safe way to continue.
        LOG_E("state", "loading slot %d failed: %s", slot, e.what());
        rt().stop("savestate load failed");
        return false;
    }
    LOG_I("state", "loaded slot %d", slot);
    return true;
}

} // namespace

void savestate_configure(const std::filesystem::path &dir) { g_dir = dir; }
void savestate_request_save(int slot) { g_pending_save = slot; }
void savestate_request_load(int slot) { g_pending_load = slot; }
void savestate_schedule_save(int slot, u64 vblank) {
    g_scheduled_slot = slot;
    g_scheduled_vblank = vblank;
}

bool savestate_service() {
    if (g_scheduled_slot >= 0 && g_kernel.vblank_count() >= g_scheduled_vblank) {
        g_pending_save = g_scheduled_slot;
        g_scheduled_slot = -1;
    }
    if (g_pending_save < 0 && g_pending_load < 0) return false;
    // Wait for a point where no interrupt, callback or movie is in flight; give up after ~2 s.
    const std::string b = blocker();
    if (!b.empty()) {
        const u64 now = g_kernel.vblank_count();
        if (g_blocked_since == 0u) g_blocked_since = now;
        if (now - g_blocked_since > 120u) {
            LOG_W("state", "cannot %s now: %s", g_pending_load >= 0 ? "load" : "save", b.c_str());
            g_pending_save = g_pending_load = -1;
            g_blocked_since = 0u;
        }
        return false;
    }
    g_blocked_since = 0u;
    if (g_pending_save >= 0) {
        const int slot = g_pending_save;
        g_pending_save = -1;
        do_save(slot);
    }
    if (g_pending_load >= 0) {
        const int slot = g_pending_load;
        g_pending_load = -1;
        return do_load(slot);
    }
    return false;
}

} // namespace p2
