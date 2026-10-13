// sceDisplay and sceCtrl: framebuffer presentation at vblank and pad sampling.
#include "display.hpp"
#include "savestate.hpp"

#include "host.hpp"
#include "kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <thread>
#include <vector>

namespace p2 {
namespace {

Kernel &K = g_kernel;

struct DisplayState {
    u32 fb_addr{};
    u32 stride{512};
    u32 format{3};
    u32 mode{};
    bool quit_requested{};
} g_display;

std::vector<u32> g_frame(480 * 272);
u64 g_frames_presented{};
u64 g_fps_window_start{};
u64 g_last_fps{};
std::string g_notice;
u64 g_notice_until{};

void update_title() {
    std::string text = "- " + std::to_string(g_last_fps) + " fps";
    if (!g_notice.empty() && Kernel::now_us() < g_notice_until) text += " - " + g_notice;
    host::set_title_suffix(text);
}

// Optional frame dumping (--dump-frames N): every Nth frame as BMP in build/frames/.
u32 g_dump_every{};
u64 g_frame_counter{};

void dump_bmp(const std::vector<u32> &pixels, u64 index) {
    char name[96];
    std::snprintf(name, sizeof(name), "build/frames/frame_%06llu.bmp", static_cast<unsigned long long>(index));
    std::FILE *f = std::fopen(name, "wb");
    if (f == nullptr) return;
    const u32 w = 480, h = 272, image = w * h * 4, file = 54 + image;
    u8 header[54]{'B', 'M'};
    auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; ++i) header[at + i] = static_cast<u8>(v >> (8 * i)); };
    put32(2, file); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    header[26] = 1; header[28] = 32; put32(34, image);
    std::fwrite(header, 1, sizeof(header), f);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 p = pixels[static_cast<std::size_t>(y) * w + x];
            const u8 bgra[4] = {static_cast<u8>(p >> 16), static_cast<u8>(p >> 8), static_cast<u8>(p), 255};
            std::fwrite(bgra, 1, 4, f);
        }
    }
    std::fclose(f);
}

u32 rgb565(u16 p) {
    const u32 r = (p & 0x1Fu) << 3, g = ((p >> 5) & 0x3Fu) << 2, b = ((p >> 11) & 0x1Fu) << 3;
    return (r | (r >> 5)) | ((g | (g >> 6)) << 8) | ((b | (b >> 5)) << 16) | 0xFF000000u;
}
u32 rgb5551(u16 p) {
    const u32 r = (p & 0x1Fu) << 3, g = ((p >> 5) & 0x1Fu) << 3, b = ((p >> 10) & 0x1Fu) << 3;
    return (r | (r >> 5)) | ((g | (g >> 5)) << 8) | ((b | (b >> 5)) << 16) | 0xFF000000u;
}
u32 rgb4444(u16 p) {
    const u32 r = (p & 0xFu) * 17u, g = ((p >> 4) & 0xFu) * 17u, b = ((p >> 8) & 0xFu) * 17u;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

void present_frame() {
    const u32 bpp = g_display.format == 3u ? 4u : 2u;
    const u8 *src = g_display.fb_addr != 0u
        ? mem().raw_pointer(g_display.fb_addr, g_display.stride * 271u * bpp + 480u * bpp)
        : nullptr;
    if (src == nullptr) {
        std::fill(g_frame.begin(), g_frame.end(), 0xFF000000u);
    } else {
        for (int y = 0; y < 272; ++y) {
            const u8 *row = src + static_cast<std::size_t>(y) * g_display.stride * bpp;
            u32 *out = g_frame.data() + y * 480;
            if (bpp == 4u) {
                for (int x = 0; x < 480; ++x) {
                    u32 p;
                    std::memcpy(&p, row + x * 4, 4);
                    out[x] = p | 0xFF000000u;
                }
            } else {
                for (int x = 0; x < 480; ++x) {
                    u16 p;
                    std::memcpy(&p, row + x * 2, 2);
                    out[x] = g_display.format == 0u ? rgb565(p) : g_display.format == 1u ? rgb5551(p) : rgb4444(p);
                }
            }
        }
    }
    const u64 present_start = Kernel::now_us();
    host::present(g_frame.data(), 480, 272);
    K.perf_present_us += Kernel::now_us() - present_start;
    if (g_dump_every != 0u && (g_frame_counter++ % g_dump_every) == 0u) dump_bmp(g_frame, g_frame_counter - 1u);
    ++g_frames_presented;
    const u64 now = Kernel::now_us();
    if (now - g_fps_window_start >= 1'000'000u) {
        g_last_fps = g_frames_presented;
        update_title();
        g_frames_presented = 0u;
        g_fps_window_start = now;
    }
}

// --- controller ------------------------------------------------------------------

u64 g_ctrl_samples{};      // incremented every vblank

// Scripted input (--input): buttons held from a vblank for a number of frames.
struct ScriptedPress { u64 start; u64 frames; u32 buttons; };
std::vector<ScriptedPress> g_script;

u32 scripted_buttons() {
    const u64 now = g_kernel.vblank_count();
    u32 b = 0u;
    for (const auto &p : g_script)
        if (now >= p.start && now < p.start + p.frames) b |= p.buttons;
    return b;
}
u64 g_ctrl_last_read{};

void write_ctrl_data(u32 address, u32 count) {
    auto &m = mem();
    const u32 buttons = host::buttons() | scripted_buttons();
    for (u32 i = 0; i < count; ++i) {
        const u32 p = address + i * 16u;
        m.store32(p + 0u, static_cast<u32>(Kernel::now_us()));
        m.store32(p + 4u, buttons);
        m.store8(p + 8u, host::analog_x());
        m.store8(p + 9u, host::analog_y());
        for (u32 j = 10u; j < 16u; ++j) m.store8(p + j, 0u);
    }
}

void on_vblank() {
    if (!host::pump_events()) {
        g_display.quit_requested = true;
        rt().stop("window closed");
        return;
    }
    K.set_turbo_key(host::fast_forward_held());
    // While fast-forwarding only every 8th vblank is shown (and rasterized, see gpu_soft).
    if (!K.turbo() || (K.vblank_count() & 7u) == 0u) present_frame();
    ++g_ctrl_samples;
    for (Thread *t : K.waiters(WaitType::Ctrl, 0, false)) {
        write_ctrl_data(t->wait_value[0], t->wait_value[1]);
        g_ctrl_last_read = g_ctrl_samples;
        K.wake(*t, t->wait_value[1]);
    }
}

void CtrlReadBufferPositive(AllegrexContext &c) {
    const u32 data = arg(c, 0);
    const u32 count = std::clamp<u32>(arg(c, 1), 1u, 64u);
    if (g_ctrl_last_read != g_ctrl_samples) {
        g_ctrl_last_read = g_ctrl_samples;
        write_ctrl_data(data, count);
        ret(c, count);
        return;
    }
    ret(c, count);
    K.current()->wait_value[0] = data;
    K.current()->wait_value[1] = count;
    K.wait_current(c, WaitType::Ctrl, 0, 0u, 0u, false);
}

u32 g_sampling_cycle{}, g_sampling_mode{};
void CtrlSetSamplingCycle(AllegrexContext &c) { ret(c, g_sampling_cycle); g_sampling_cycle = arg(c, 0); }
void CtrlSetSamplingMode(AllegrexContext &c) { ret(c, g_sampling_mode); g_sampling_mode = arg(c, 0); }
void CtrlSetIdleCancelThreshold(AllegrexContext &c) { ret(c, 0u); }

// --- display -----------------------------------------------------------------------

void DisplaySetMode(AllegrexContext &c) { g_display.mode = arg(c, 0); ret(c, 0u); }

void DisplaySetFrameBuf(AllegrexContext &c) {
    if (g_display.fb_addr != arg(c, 0)) {
        LOG_T("display", "framebuffer %08x stride %u format %u", arg(c, 0), arg(c, 1), arg(c, 2));
        ++K.perf_flips;
        ++K.total_flips;
    }
    g_display.fb_addr = arg(c, 0);
    if (arg(c, 1) != 0u) g_display.stride = arg(c, 1);
    g_display.format = arg(c, 2);
    ret(c, 0u);
}

void wait_vblanks(AllegrexContext &c, u32 count, bool cb) {
    ret(c, 0u);
    if (count == 0u) return;
    if (cb && K.check_callbacks(c)) return;
    K.current()->wait_value[0] = count;
    K.wait_current(c, WaitType::Vblank, 0, 0u, 0u, cb);
}
void DisplayWaitVblankStartCB(AllegrexContext &c) { wait_vblanks(c, 1u, true); }
void DisplayWaitVblankCB(AllegrexContext &c) { wait_vblanks(c, 1u, true); }
void DisplayWaitVblankStartMultiCB(AllegrexContext &c) { wait_vblanks(c, arg(c, 0), true); }
void DisplayIsVblank(AllegrexContext &c) { ret(c, 0u); }
// The game busy-waits on the vblank counter as a frame limiter. Polling the
// same value repeatedly can only end at the next vblank, so sleep the host until
// then instead of spinning (the guest observes the same sequence of values).
void DisplayGetVcount(AllegrexContext &c) {
    static u64 last = ~0ull;
    static u32 repeats = 0;
    const u64 now_count = K.vblank_count();
    if (now_count == last) {
        if (++repeats >= 256u) {
            const u64 now = Kernel::now_us(), next = K.next_vblank_us();
            if (K.turbo()) K.skip_time_to(next);
            else if (next > now + 300u) std::this_thread::sleep_for(std::chrono::microseconds(std::min<u64>(next - now - 200u, 2000u)));
            repeats = 0;
        }
    } else {
        last = now_count;
        repeats = 0;
    }
    ret(c, static_cast<u32>(now_count));
}
void DisplayGetFramePerSec(AllegrexContext &c) { retf(c, 59.9400375f); ret(c, 0u); }

} // namespace

void display_set_input_script(const std::string &script) {
    // Format: "<vblank>=<BUTTON>[+<BUTTON>...][:<frames>]" separated by commas.
    static const struct { const char *name; u32 bit; } names[] = {
        {"SELECT", 0x1u}, {"START", 0x8u}, {"UP", 0x10u}, {"RIGHT", 0x20u}, {"DOWN", 0x40u}, {"LEFT", 0x80u},
        {"L", 0x100u}, {"R", 0x200u}, {"TRIANGLE", 0x1000u}, {"CIRCLE", 0x2000u}, {"CROSS", 0x4000u}, {"SQUARE", 0x8000u}};
    std::size_t pos = 0;
    while (pos < script.size()) {
        std::size_t end = script.find(',', pos);
        if (end == std::string::npos) end = script.size();
        const std::string item = script.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        ScriptedPress press{std::stoull(item.substr(0, eq)), 6u, 0u};
        std::string rest = item.substr(eq + 1);
        if (const auto colon = rest.find(':'); colon != std::string::npos) {
            press.frames = std::stoull(rest.substr(colon + 1));
            rest = rest.substr(0, colon);
        }
        std::size_t bp = 0;
        while (bp <= rest.size()) {
            std::size_t plus = rest.find('+', bp);
            if (plus == std::string::npos) plus = rest.size();
            const std::string name = rest.substr(bp, plus - bp);
            for (const auto &n : names)
                if (name == n.name) press.buttons |= n.bit;
            bp = plus + 1;
        }
        g_script.push_back(press);
    }
    LOG_I("display", "input script: %u presses", static_cast<unsigned>(g_script.size()));
}

void display_set_frame_dump(u32 every) {
    g_dump_every = every;
    if (every != 0u) std::filesystem::create_directories("build/frames");
}

void register_display_imports() {
    K.vblank_listeners.push_back(on_vblank);
    const char *D = "sceDisplay";
    hle(D, "sceDisplaySetMode", DisplaySetMode);
    hle(D, "sceDisplaySetFrameBuf", DisplaySetFrameBuf);
    hle(D, "sceDisplayWaitVblankStartCB", DisplayWaitVblankStartCB);
    hle(D, "sceDisplayWaitVblankCB", DisplayWaitVblankCB);
    hle(D, "sceDisplayWaitVblankStartMultiCB", DisplayWaitVblankStartMultiCB);
    hle(D, "sceDisplayIsVblank", DisplayIsVblank);
    hle(D, "sceDisplayGetVcount", DisplayGetVcount);
    hle(D, "sceDisplayGetFramePerSec", DisplayGetFramePerSec);
    const char *C = "sceCtrl";
    hle(C, "sceCtrlReadBufferPositive", CtrlReadBufferPositive);
    hle(C, "sceCtrlSetSamplingCycle", CtrlSetSamplingCycle);
    hle(C, "sceCtrlSetSamplingMode", CtrlSetSamplingMode);
    hle(C, "sceCtrlSetIdleCancelThreshold", CtrlSetIdleCancelThreshold);
}

void display_save(StateWriter &w) {
    w.section("DISP");
    w.pod(g_display.fb_addr);
    w.pod(g_display.stride);
    w.pod(g_display.format);
    w.pod(g_display.mode);
    w.pod(g_ctrl_samples);
    w.pod(g_ctrl_last_read);
    w.pod(g_sampling_cycle);
    w.pod(g_sampling_mode);
}

void display_load(StateReader &r) {
    r.section("DISP");
    r.pod(g_display.fb_addr);
    r.pod(g_display.stride);
    r.pod(g_display.format);
    r.pod(g_display.mode);
    r.pod(g_ctrl_samples);
    r.pod(g_ctrl_last_read);
    r.pod(g_sampling_cycle);
    r.pod(g_sampling_mode);
}

void display_notify(const std::string &text) {
    g_notice = text;
    g_notice_until = Kernel::now_us() + 3'000'000u;
    update_title();
}

} // namespace p2
