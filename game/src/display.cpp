// sceDisplay and sceCtrl: framebuffer presentation at vblank and pad sampling.
#include "display.hpp"
#include "ge.hpp"
#include "savedata.hpp"
#include "savestate.hpp"

#include "host.hpp"
#include "kernel.hpp"

#include <cmath>
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
    savedata_dialog_draw(g_frame.data());
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

// P2_GOTO="<px>,<py>,<pz>;<x>,<z>[;<x>,<z>...][|<buttons script>]": debugging autopilot. Reads the
// player position (float32 x and z at guest addresses px and pz, hex) and holds the d-pad
// direction that best heads for the next waypoint; P2_GOTO_AXES gives the world (dx,dz) of
// stick UP and RIGHT for the current camera ("ux,uz,rx,rz"). When the last waypoint is
// reached the autopilot logs it and stops steering. Positions are logged every 8 vblanks.
struct Autopilot {
    bool on{};
    u32 px{}, pz{};
    std::vector<std::pair<float, float>> points;
    std::size_t next{};
    u64 since{};  // vblank the current waypoint was started
    float ux{1}, uz{-1}, rx{1}, rz{1};
};

Autopilot &autopilot() {
    static Autopilot a = [] {
        Autopilot r;
        const char *e = std::getenv("P2_GOTO");
        if (e == nullptr) return r;
        std::string spec = e;
        u32 py = 0;
        if (std::sscanf(spec.c_str(), "%x,%x,%x", &r.px, &py, &r.pz) != 3) return r;
        std::size_t pos = spec.find(';');
        while (pos != std::string::npos) {
            float x = 0, z = 0;
            if (std::sscanf(spec.c_str() + pos + 1, "%f,%f", &x, &z) == 2) r.points.emplace_back(x, z);
            pos = spec.find(';', pos + 1);
        }
        if (const char *ax = std::getenv("P2_GOTO_AXES")) std::sscanf(ax, "%f,%f,%f,%f", &r.ux, &r.uz, &r.rx, &r.rz);
        r.on = !r.points.empty();
        return r;
    }();
    return a;
}

float guest_float(u32 addr) {
    const u32 bits = mem().load32(addr);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

u32 autopilot_buttons() {
    Autopilot &a = autopilot();
    if (!a.on || a.next >= a.points.size()) return 0u;
    const float x = guest_float(a.px), z = guest_float(a.pz);
    const auto [tx, tz] = a.points[a.next];
    const float dx = tx - x, dz = tz - z;
    if ((g_kernel.vblank_count() & 7u) == 0u) LOG_I("goto", "at %.2f,%.2f -> %.2f,%.2f", x, z, tx, tz);
    const u64 now = g_kernel.vblank_count();
    if (a.since == 0u) a.since = now;
    const bool reached = dx * dx + dz * dz < 0.09f;
    // Unreachable targets (walls) are given up after 4 seconds.
    if (reached || now - a.since > 240u) {
        LOG_I("goto", "%s waypoint %u (%.2f,%.2f)", reached ? "reached" : "gave up on", static_cast<unsigned>(a.next), x, z);
        ++a.next;
        a.since = now;
        return 0u;
    }
    // Score the 8 stick directions by alignment with the wanted world direction.
    static constexpr u32 kUp = 0x10u, kRight = 0x20u, kDown = 0x40u, kLeft = 0x80u;
    struct Dir { u32 buttons; float wx, wz; };
    const Dir dirs[8] = {
        {kUp, a.ux, a.uz}, {kDown, -a.ux, -a.uz}, {kRight, a.rx, a.rz}, {kLeft, -a.rx, -a.rz},
        {kUp | kRight, a.ux + a.rx, a.uz + a.rz}, {kUp | kLeft, a.ux - a.rx, a.uz - a.rz},
        {kDown | kRight, -a.ux + a.rx, -a.uz + a.rz}, {kDown | kLeft, -a.ux - a.rx, -a.uz - a.rz},
    };
    u32 best = 0u;
    float best_score = -1e9f;
    for (const Dir &d : dirs) {
        const float len = std::sqrt(d.wx * d.wx + d.wz * d.wz);
        if (len <= 0.f) continue;
        const float score = (d.wx * dx + d.wz * dz) / len;
        if (score > best_score) { best_score = score; best = d.buttons; }
    }
    return best;
}

// P2_CLIMB=<hex addr of the player's height float>: holds a d-pad direction while the height
// rises and turns to the next of the 8 directions when it stops (spiral staircases).
u32 climb_buttons() {
    static const u32 addr = [] { const char *e = std::getenv("P2_CLIMB"); return e ? static_cast<u32>(std::strtoul(e, nullptr, 16)) : 0u; }();
    if (addr == 0u) return 0u;
    static constexpr u32 kDirs[8] = {0x10u, 0x30u, 0x20u, 0x60u, 0x40u, 0xC0u, 0x80u, 0x90u}; // clockwise from UP
    static int dir = 4; // DOWN
    static float prev = 0.f;
    static u64 checked = 0;
    const u64 now = g_kernel.vblank_count();
    const float h = guest_float(addr);
    if (checked == 0u) { checked = now; prev = h; }
    if (now - checked >= 10u) {
        if (h <= prev + 0.02f) {
            dir = (dir + 1) & 7;
            LOG_I("climb", "height %.2f, trying direction %d", h, dir);
        }
        prev = h;
        checked = now;
    }
    return kDirs[dir];
}

u32 scripted_buttons() {
    const u64 now = g_kernel.vblank_count();
    u32 b = autopilot_buttons() | climb_buttons();
    for (const auto &p : g_script)
        if (now >= p.start && now < p.start + p.frames) b |= p.buttons;
    return b;
}
u64 g_ctrl_last_read{};

void write_ctrl_data(u32 address, u32 count) {
    auto &m = mem();
    // While a system dialog is open the game sees no buttons (the dialog consumes them).
    const u32 buttons = savedata_dialog_active() ? 0u : host::buttons() | scripted_buttons();
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
    // P2_WATCH=<addr>[,<addr>...]: log these guest float32 values every vblank (debugging).
    static const std::vector<u32> watch = [] {
        std::vector<u32> v;
        if (const char *e = std::getenv("P2_WATCH")) {
            for (const char *p = e; *p != '\0';) {
                char *end = nullptr;
                v.push_back(static_cast<u32>(std::strtoul(p, &end, 16)));
                p = *end == ',' ? end + 1 : end;
                if (end == p && *p != '\0') break;
            }
        }
        return v;
    }();
    // P2_POKE=<vblank>,<hex addr>,<float>[,<hex addr>,<float>...]: writes guest float32
    // values once at that vblank (debugging, e.g. moving the player).
    static const std::vector<std::pair<u32, float>> pokes = [] {
        std::vector<std::pair<u32, float>> v;
        if (const char *e = std::getenv("P2_POKE")) {
            const char *p = std::strchr(e, ',');
            while (p != nullptr) {
                char *end = nullptr;
                const u32 addr = static_cast<u32>(std::strtoul(p + 1, &end, 16));
                if (end == nullptr || *end != ',') break;
                const float value = std::strtof(end + 1, &end);
                v.emplace_back(addr, value);
                p = end != nullptr && *end == ',' ? end : nullptr;
            }
        }
        return v;
    }();
    static const u64 poke_at = [] { const char *e = std::getenv("P2_POKE"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
    if (!pokes.empty() && K.vblank_count() == poke_at) {
        for (const auto &[addr, value] : pokes) {
            u32 bits;
            std::memcpy(&bits, &value, 4);
            mem().store32(addr, bits);
            LOG_I("poke", "%08x = %.3f", addr, static_cast<double>(value));
        }
    }
    if (!watch.empty()) {
        std::string line;
        for (u32 a : watch) {
            const u32 bits = mem().load32(a);
            float f;
            std::memcpy(&f, &bits, 4);
            char buf[32];
            std::snprintf(buf, sizeof(buf), " %.3f", static_cast<double>(f));
            line += buf;
        }
        LOG_I("watch", "%s", line.c_str());
    }
    // While fast-forwarding only every 8th vblank is shown (and rasterized, see gpu_soft).
    ge_flush(); // drawing still queued from stalled lists
    savedata_dialog_input(host::buttons() | scripted_buttons());
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
            else if (next > now + 300u) {
                std::this_thread::sleep_for(std::chrono::microseconds(std::min<u64>(next - now - 200u, 2000u)));
                K.add_idle_us(Kernel::now_us() - now);
            }
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
