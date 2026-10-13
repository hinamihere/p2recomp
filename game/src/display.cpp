// sceDisplay and sceCtrl: framebuffer presentation at vblank and pad sampling.
#include "display.hpp"

#include "host.hpp"
#include "kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
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
    host::present(g_frame.data(), 480, 272);
    ++g_frames_presented;
    const u64 now = Kernel::now_us();
    if (now - g_fps_window_start >= 1'000'000u) {
        char text[64];
        std::snprintf(text, sizeof(text), "- %llu fps", static_cast<unsigned long long>(g_frames_presented));
        host::set_title_suffix(text);
        g_frames_presented = 0u;
        g_fps_window_start = now;
    }
}

// --- controller ------------------------------------------------------------------

u64 g_ctrl_samples{};      // incremented every vblank
u64 g_ctrl_last_read{};

void write_ctrl_data(u32 address, u32 count) {
    auto &m = mem();
    const u32 buttons = host::buttons();
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
    present_frame();
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
    if (g_display.fb_addr != arg(c, 0))
        LOG_T("display", "framebuffer %08x stride %u format %u", arg(c, 0), arg(c, 1), arg(c, 2));
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
void DisplayGetVcount(AllegrexContext &c) { ret(c, static_cast<u32>(K.vblank_count())); }
void DisplayGetFramePerSec(AllegrexContext &c) { retf(c, 59.9400375f); ret(c, 0u); }

} // namespace

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

} // namespace p2
