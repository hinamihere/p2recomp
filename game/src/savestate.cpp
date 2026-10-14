#include "savestate.hpp"
#include "ge.hpp"

#include "display.hpp"
#include "kernel.hpp"

#include <fstream>

namespace p2 {
namespace {

constexpr u32 kMagic = 0x53533250u; // "P2SS"
constexpr u32 kVersion = 5u;
constexpr u32 kRamBase = 0x08000000u, kRamSize = 32u * 1024u * 1024u;
constexpr u32 kVramBase = 0x04000000u, kVramSize = 2u * 1024u * 1024u;

std::filesystem::path g_dir{"states"};
int g_pending_save = -1;
int g_pending_load = -1;
int g_scheduled_slot = -1;
u64 g_scheduled_vblank = 0;
int g_scheduled_load_slot = -1;
u64 g_scheduled_load_vblank = 0;
u64 g_blocked_since = 0;
bool g_at_hle_return = false;
u32 g_load_version = 0;
bool g_load_required = false;
bool g_exit_after_save = false;
bool g_scheduled_pending = false;

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
    if (b.empty()) b = savedata_save_blocker();
    return b;
}

bool do_save(int slot) {
    ge_flush();
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
    mpeg_save(w);
    io_save(w);
    utility_save(w);
    savedata_save(w);
    w.section("END ");
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::ofstream out(slot_path(slot), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
    display_notify(out ? "state " + std::to_string(slot) + " saved" : "saving state " + std::to_string(slot) + " failed");
    LOG_I("state", "saved slot %d (%u KiB) at vblank %llu", slot, static_cast<unsigned>(w.buf.size() / 1024u),
          static_cast<unsigned long long>(g_kernel.vblank_count()));
    return static_cast<bool>(out);
}

bool do_load(int slot) {
    std::ifstream in(slot_path(slot), std::ios::binary);
    if (!in) {
        LOG_W("state", "no savestate in slot %d", slot);
        display_notify("no state in slot " + std::to_string(slot));
        return false;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    StateReader r(std::move(data));
    ge_flush();
    // Header problems are refused before anything is touched.
    const char *refused = nullptr;
    if (r.buf.size() < 20u) refused = "not a savestate";
    else if (r.pod<u32>() != kMagic) refused = "not a savestate";
    else if ((g_load_version = r.pod<u32>()) < 3u || g_load_version > kVersion) refused = "made by another version";
    else if (r.pod<u32>() != g_kernel.module_gp()) refused = "made for a different executable";
    if (refused != nullptr) {
        LOG_W("state", "slot %d: %s", slot, refused);
        display_notify("state " + std::to_string(slot) + ": " + refused);
        return false;
    }
    try {
        r.pod<u64>();
        load_memory(r);
        kernel_load(r);
        ge_load(r);
        display_load(r);
        audio_load(r);
        sas_load(r);
        atrac_load(r);
        mpeg_load(r);
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
    display_notify("state " + std::to_string(slot) + " loaded");
    return true;
}

} // namespace

void savestate_configure(const std::filesystem::path &dir) { g_dir = dir; }
void savestate_request_save(int slot) { g_pending_save = slot; }
void savestate_request_load(int slot) { g_pending_load = slot; }
void savestate_request_load_required(int slot) {
    g_pending_load = slot;
    g_load_required = true;
}
void savestate_exit_after_save(bool on) { g_exit_after_save = on; }

void savestate_schedule_save(int slot, u64 vblank) {
    g_scheduled_slot = slot;
    g_scheduled_vblank = vblank;
}
void savestate_schedule_load(int slot, u64 vblank) {
    g_scheduled_load_slot = slot;
    g_scheduled_load_vblank = vblank;
}

bool savestate_requested() {
    return g_pending_save >= 0 || g_pending_load >= 0 || g_scheduled_slot >= 0 || g_scheduled_load_slot >= 0;
}

void savestate_service_at_hle_return() {
    g_at_hle_return = true;
    savestate_service();
    g_at_hle_return = false;
}

bool savestate_at_hle_return() { return g_at_hle_return; }
u32 savestate_load_version() { return g_load_version; }

bool savestate_service() {
    if (g_scheduled_slot >= 0 && g_kernel.vblank_count() >= g_scheduled_vblank) {
        g_pending_save = g_scheduled_slot;
        g_scheduled_slot = -1;
        g_scheduled_pending = true;
    }
    if (g_scheduled_load_slot >= 0 && g_kernel.vblank_count() >= g_scheduled_load_vblank) {
        g_pending_load = g_scheduled_load_slot;
        g_scheduled_load_slot = -1;
    }
    if (g_pending_save < 0 && g_pending_load < 0) return false;
    // Wait for a point where no interrupt, callback or movie is in flight; give up after ~2 s.
    const std::string b = blocker();
    if (!b.empty()) {
        const u64 now = g_kernel.vblank_count();
        if (g_blocked_since == 0u) g_blocked_since = now;
        if (now - g_blocked_since > 120u) {
            LOG_W("state", "cannot %s now: %s", g_pending_load >= 0 ? "load" : "save", b.c_str());
            display_notify(std::string("cannot ") + (g_pending_load >= 0 ? "load" : "save") + " a state right now");
            g_pending_save = g_pending_load = -1;
            g_blocked_since = 0u;
        }
        return false;
    }
    g_blocked_since = 0u;
    if (g_pending_save >= 0) {
        const int slot = g_pending_save;
        g_pending_save = -1;
        const bool saved = do_save(slot);
        if (g_scheduled_pending && g_exit_after_save && saved) rt().stop("exit-after reached");
        g_scheduled_pending = false;
    }
    if (g_pending_load >= 0) {
        const int slot = g_pending_load;
        g_pending_load = -1;
        const bool loaded = do_load(slot);
        if (!loaded && g_load_required) rt().stop("savestate load failed");
        g_load_required = false;
        return loaded;
    }
    return false;
}

} // namespace p2
