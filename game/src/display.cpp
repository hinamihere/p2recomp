// sceDisplay and sceCtrl: framebuffer presentation at vblank and pad sampling.
#include "display.hpp"
#include "ge.hpp"
#include "savedata.hpp"
#include "savestate.hpp"
#include "watchpoint.hpp"

#include "host.hpp"
#include "kernel.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <thread>
#include <array>
#include <climits>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <tuple>
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
    // P2_DUMP_VBLANK=1: name frames by the vblank they were presented at (fast-forward runs skip
    // frames, so the index alone does not give the time).
    static const bool by_vblank = std::getenv("P2_DUMP_VBLANK") != nullptr;
    std::snprintf(name, sizeof(name), "build/frames/frame_%06llu.bmp",
                  static_cast<unsigned long long>(by_vblank ? g_kernel.vblank_count() : index));
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

// Battle: Triangle on the Tactics menu starts Auto-Battle (the party fights on its own until the end); Cross pages
// through the result screens.
bool battle_panels_visible(); // red party panels along the bottom: a battle, not a field menu
u32 battle_taps(u64 now) {
    const u64 t = now % 120u;
    if (t < 3u && battle_panels_visible()) return 0x1000u; // Triangle (never in the field: it opens the command menu)
    if (t >= 40u && t < 43u) return 0x4000u;               // Cross
    if (t >= 80u && t < 83u) return 0x4000u;
    return 0u;
}

// Heuristic used by the exploration bot: battle and menu screens carry a red UI bar along the top (right of the
// location banner) or red party panels along the bottom; the field view has neither.
bool battle_like_screen() {
    auto red_fraction = [](int x0, int x1, int y0, int y1) {
        int red = 0, total = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const u32 p = g_frame[static_cast<std::size_t>(y) * 480 + x];
                const u32 r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
                red += (r > 110 && g < 70 && b < 70) ? 1 : 0;
                ++total;
            }
        return static_cast<float>(red) / static_cast<float>(total);
    };
    return red_fraction(210, 470, 3, 21) > 0.35f || red_fraction(30, 300, 212, 266) > 0.12f;
}

bool battle_panels_visible() {
    int red = 0, total = 0;
    for (int y = 212; y < 266; ++y)
        for (int x = 30; x < 300; ++x) {
            const u32 p = g_frame[static_cast<std::size_t>(y) * 480 + x];
            red += ((p & 0xFF) > 110 && ((p >> 8) & 0xFF) < 70 && ((p >> 16) & 0xFF) < 70) ? 1 : 0;
            ++total;
        }
    return red * 100 > total * 12;
}

// P2_EXPLORE=1 (or "<xaddr>,<zaddr>" in hex): dungeon exploration bot. It reads the position of the party
// leader (defaults: x at 08CD4190, z at 08CD4198), walks along the four d-pad directions, prefers the
// neighbouring grid cell it has visited least, turns away from walls it bumped into, and taps Cross every
// second (doors, dialogue). Battles and menu screens are recognised from the screen contents: there it only taps
// Cross (default commands, victory screens) and never pushes a direction. When a party member is below
// half HP outside battle it drinks Medicine through the Triangle menu (at most three times). While the
// position does not change for a second (dialogue, loading) it taps Cross quickly and probes directions.
// P2_EXPLORE_SEED selects the random tie-breaking; P2_EXPLORE_LOG=1 logs the interaction events;
// P2_EXPLORE_AVOID="x0,z0,x1,z1" keeps it out of a world rectangle (e.g. the exit door of the school).
u32 explore_buttons() {
    static const char *env = std::getenv("P2_EXPLORE");
    if (env == nullptr) return 0u;
    constexpr u32 kEventFlag = 0x08CA269Cu, kParty = 0x08E3CCBCu;
    struct Explorer {
        u32 xaddr{0x08CD4190u}, zaddr{0x08CD4198u};
        std::map<std::pair<int, int>, int> visits;
        std::set<std::tuple<int, int, int>> blocked;
        std::pair<int, int> cell{INT_MIN, 0};
        int heading{3}; // 0 up (z-), 1 down (z+), 2 left (x-), 3 right (x+)
        float last_x{}, last_z{};
        float avoid_x0{1e9f}, avoid_z0{1e9f}, avoid_x1{-1e9f}, avoid_z1{-1e9f}; // P2_EXPLORE_AVOID: never enter this world rectangle
        u64 last_check{}, last_move{}, last_log{}, battle_since{}, last_heal{}, last_event_press{};
        bool was_battle{}, event_seen{};
        int heals{};
        int checkpoint_slot{-1};          // P2_EXPLORE_CHECKPOINT: savestate slot used for rollbacks
        u64 last_checkpoint{}, last_now{};
        int checkpoint_heals{}, rollbacks{};
        u64 load_vblank{}; // vblank right after the last savestate load (rollback cooldown)
        std::vector<std::pair<u64, u32>> macro; // (start vblank, buttons) taps of a running macro
        u64 macro_end{};
        std::mt19937 rng;
    };
    static Explorer e = [] {
        Explorer r;
        unsigned xa = 0, za = 0;
        if (std::sscanf(env, "%x,%x", &xa, &za) == 2) { r.xaddr = xa; r.zaddr = za; }
        if (const char *slot = std::getenv("P2_EXPLORE_CHECKPOINT")) r.checkpoint_slot = std::atoi(slot);
        if (const char *avoid = std::getenv("P2_EXPLORE_AVOID"))
            std::sscanf(avoid, "%f,%f,%f,%f", &r.avoid_x0, &r.avoid_z0, &r.avoid_x1, &r.avoid_z1);
        const char *seed = std::getenv("P2_EXPLORE_SEED");
        r.rng.seed(seed ? static_cast<unsigned>(std::strtoul(seed, nullptr, 10)) : 1u);
        return r;
    }();
    static const bool verbose = std::getenv("P2_EXPLORE_LOG") != nullptr;
    static constexpr u32 kHeadingButtons[4] = {0x10u, 0x40u, 0x80u, 0x20u};
    static constexpr int kDx[4] = {0, 0, -1, 1}, kDz[4] = {-1, 1, 0, 0};
    static constexpr u32 kCross = 0x4000u, kCircle = 0x2000u, kTriangle = 0x1000u, kDown = 0x40u;
    const u64 now = g_kernel.vblank_count();
    const float x = guest_float(e.xaddr), z = guest_float(e.zaddr);
    static u64 battle_hold_until = 0;
    if (battle_like_screen()) battle_hold_until = now + 150u; // dimmed dialogs between battle screens must not end it
    const bool battle = now < battle_hold_until;

    // A savestate load rewinds the vblank counter: forget all timers.
    if (now < e.last_now) {
        e.macro.clear();
        e.last_check = 0; e.last_move = 0; e.last_log = 0; e.battle_since = now; e.last_heal = 0; e.last_event_press = 0;
        e.cell = {INT_MIN, 0};
        e.was_battle = false;
        battle_hold_until = 0;
        e.load_vblank = now;
    }
    e.last_now = now;

    // Checkpoints and rollbacks (P2_EXPLORE_CHECKPOINT=<slot>): while the whole party is healthy outside a fight a
    // checkpoint is saved every 1500 vblanks; if two members are knocked out, or the Medicine is used up and someone
    // is below a third of their HP, the run rolls back to the checkpoint and tries a different random route.
    if (e.checkpoint_slot >= 0 && e.macro.empty()) {
        int down = 0, hurt = 0, healthy = 0;
        for (int i = 0; i < 3; ++i) {
            const u32 base = kParty + 0x54u * static_cast<u32>(i);
            const float hp = static_cast<float>(mem().load16(base)), max_hp = static_cast<float>(mem().load16(base + 2));
            if (max_hp <= 0.f) continue;
            down += hp == 0.f ? 1 : 0;
            hurt += hp / max_hp < 0.34f ? 1 : 0;
            healthy += hp / max_hp >= 0.6f ? 1 : 0;
        }
        const bool bad = now - e.load_vblank > 900u && (down >= 2 || (e.heals >= 3 && hurt >= 1 && !battle));
        if (bad && e.last_checkpoint != 0u && now - e.last_checkpoint > 300u && e.rollbacks < 40) {
            ++e.rollbacks;
            LOG_I("explore", "rollback %d: %d knocked out, %d badly hurt, heals used %d (vblank %llu)", e.rollbacks, down, hurt, e.heals,
                  static_cast<unsigned long long>(now));
            e.heals = e.checkpoint_heals;
            e.rng.seed(static_cast<unsigned>(e.rollbacks * 7919 + 13));
            e.last_checkpoint = 0u;
            savestate_request_load(e.checkpoint_slot);
            return 0u;
        }
        if (!battle && healthy == 3 && (e.last_checkpoint == 0u || now - e.last_checkpoint > 1500u)) {
            savestate_request_save(e.checkpoint_slot);
            e.last_checkpoint = now;
            e.checkpoint_heals = e.heals;
        }
    }
    const std::pair<int, int> cell{static_cast<int>(std::floor(x / 200.f)), static_cast<int>(std::floor(z / 200.f))};

    if (battle != e.was_battle) {
        e.was_battle = battle;
        e.battle_since = now;
        e.last_move = now; // walking resumes with a fresh stuck timer after the fight
        e.last_x = x; e.last_z = z; e.last_check = now;
        if (verbose) LOG_I("explore", "%s at vblank %llu", battle ? "battle started" : "battle over", static_cast<unsigned long long>(now));
    }

    // Macro in progress (healing): it owns the buttons until it is over.
    if (!e.macro.empty()) {
        if (now >= e.macro_end) {
            LOG_I("explore", "medicine macro finished: HP %u/%u %u/%u %u/%u", mem().load16(kParty), mem().load16(kParty + 2),
                  mem().load16(kParty + 0x54), mem().load16(kParty + 0x56), mem().load16(kParty + 0xA8), mem().load16(kParty + 0xAA));
            e.macro.clear();
            e.last_move = now; e.last_check = now; e.last_x = x; e.last_z = z;
        } else {
            u32 b = 0u;
            for (const auto &[start, buttons] : e.macro) if (now >= start && now < start + 3u) b |= buttons;
            return b;
        }
    }
    if (battle) {
        // Cross confirms default commands. A battle sequence needs many Crosses in a row, so Circle (back) is only
        // used in bursts once a battle has lasted very long, to leave status pages and similar dead ends.
        u32 b = battle_taps(now);
        const u64 age = now - e.battle_since;
        if (age > 4000u && age % 2000u < 120u && now % 30u < 3u) b = kCircle; // stuck for a long time: back out
        return b;
    }

    // Heal: lowest HP fraction among the three party members.
    {
        int worst = -1;
        float worst_fraction = 0.5f;
        for (int i = 0; i < 3; ++i) {
            const u32 base = kParty + 0x54u * static_cast<u32>(i);
            const float hp = static_cast<float>(mem().load16(base)), max_hp = static_cast<float>(mem().load16(base + 2));
            if (max_hp > 0.f && hp / max_hp < worst_fraction) { worst_fraction = hp / max_hp; worst = i; }
        }
        if (worst >= 0 && e.heals < 3 && now - e.last_heal > 1500u && now - e.battle_since > 400u && now - e.last_move < 90u && e.last_move != 0u) {
            e.heals++;
            e.last_heal = now;
            LOG_I("explore", "party member %d is hurt: using Medicine (%d/3), HP %u/%u %u/%u %u/%u", worst, e.heals,
                  mem().load16(kParty), mem().load16(kParty + 2), mem().load16(kParty + 0x54), mem().load16(kParty + 0x56),
                  mem().load16(kParty + 0xA8), mem().load16(kParty + 0xAA));
            u64 t = now + 5u;
            auto tap = [&](u32 buttons, u64 gap) { e.macro.emplace_back(t, buttons); t += gap; };
            tap(kTriangle, 60); tap(kDown, 40); tap(kCross, 60); // Command menu, Items, open the item categories
            tap(kCross, 60); tap(kCross, 60);                  // Expendable items, Medicine
            for (int i = 0; i < worst; ++i) tap(kDown, 40);    // choose who to heal
            tap(kCross, 80);                                   // apply
            for (int i = 0; i < 5; ++i) tap(kCircle, 50);      // back out of the menus
            e.macro_end = t + 10u;
            return 0u;
        }
    }

    const bool event = mem().load32(kEventFlag) != 0u;
    if (event != e.event_seen) {
        e.event_seen = event;
        if (event && verbose)
            LOG_I("explore", "interaction available at (%.0f, %.0f) cell (%d,%d) heading %d", x, z, cell.first, cell.second, e.heading);
    }

    auto choose = [&] {
        float best = -1e9f;
        int pick = e.heading;
        for (int d = 0; d < 4; ++d) {
            if (e.blocked.count({cell.first, cell.second, d})) continue;
            const float nx = (static_cast<float>(cell.first + kDx[d]) + 0.5f) * 200.f, nz = (static_cast<float>(cell.second + kDz[d]) + 0.5f) * 200.f;
            if (nx >= e.avoid_x0 && nx <= e.avoid_x1 && nz >= e.avoid_z0 && nz <= e.avoid_z1) continue; // forbidden area
            const auto it = e.visits.find({cell.first + kDx[d], cell.second + kDz[d]});
            const float visited = it == e.visits.end() ? 0.f : static_cast<float>(it->second);
            const float score = -visited + (d == e.heading ? 0.6f : 0.f) + static_cast<float>(e.rng() % 100u) / 250.f;
            if (score > best) { best = score; pick = d; }
        }
        e.heading = pick;
    };
    if (cell != e.cell) {
        e.cell = cell;
        ++e.visits[cell];
        choose();
    }
    if (e.last_check == 0u) { e.last_check = now; e.last_x = x; e.last_z = z; e.last_move = now; }
    if (now - e.last_check >= 8u) {
        const float moved = std::fabs(x - e.last_x) + std::fabs(z - e.last_z);
        if (moved >= 30.f) {
            e.last_move = now;
        } else if (now - e.last_move < 60u) {
            e.blocked.insert({cell.first, cell.second, e.heading}); // bumped into a wall
            choose();
        }
        e.last_x = x; e.last_z = z; e.last_check = now;
    }
    if (now - e.last_log >= 1200u) {
        e.last_log = now;
        LOG_I("explore", "vblank %llu at (%.0f, %.0f) cell (%d,%d): %zu cells visited, %zu walls, battle word %u, event word %u",
              static_cast<unsigned long long>(now), x, z, cell.first, cell.second, e.visits.size(), e.blocked.size(),
              battle ? 1u : 0u, mem().load32(kEventFlag));
    }
    u32 buttons = 0u;
    const bool idle = now - e.last_move >= 60u; // dialogue or loading: confirm quickly
    if (now % (idle ? 18u : 60u) < 3u) buttons |= kCross;
    const bool in_avoid = x >= e.avoid_x0 - 300.f && x <= e.avoid_x1 + 300.f && z >= e.avoid_z0 - 300.f && z <= e.avoid_z1 + 300.f;
    if (in_avoid) buttons &= ~kCross; // never trigger an event near the forbidden area (the school exit)
    if (event && !in_avoid && now - e.last_event_press > 90u) { buttons |= kCross; e.last_event_press = now; }
    // Long idle: also back out of whatever menu or status screen the confirming wandered into.
    if (now - e.last_move > 300u && now % 240u >= 120u && now % 240u < 124u) buttons |= kCircle;
    // While idle, push a direction for a few frames now and then so it notices when it is free to walk again.
    if (!idle || now % 90u < 16u) buttons |= kHeadingButtons[e.heading];
    if (idle && now % 90u == 0u) e.heading = static_cast<int>(e.rng() % 4u);
    return buttons;
}

// A (nearly) black frame: the loading screen between the end of a battle and the field.
bool blackout_screen() {
    int dark = 0, total = 0;
    for (std::size_t i = 0; i < g_frame.size(); i += 97) {
        const u32 p = g_frame[i];
        dark += ((p & 0xFF) < 14 && ((p >> 8) & 0xFF) < 14 && ((p >> 16) & 0xFF) < 14) ? 1 : 0;
        ++total;
    }
    return dark * 100 > total * 88;
}

// A (nearly) white frame is the flash of a battle starting or a screen transition.
bool whiteout_screen() {
    int white = 0, total = 0;
    for (std::size_t i = 0; i < g_frame.size(); i += 97) {
        const u32 p = g_frame[i];
        white += ((p & 0xFF) > 225 && ((p >> 8) & 0xFF) > 225 && ((p >> 16) & 0xFF) > 225) ? 1 : 0;
        ++total;
    }
    return white * 100 > total * 55;
}

// P2_ENCDBG=1: finds the random-encounter counter. Keeps RAM snapshots every 30 vblanks while the field is shown;
// when a battle flash starts, it logs words that rose in each of the last snapshots and are lower now (a counter that
// was reset). Candidates seen at several battles are printed with their hit count.
void encounter_debug(u64 now, bool whiteout) {
    static const bool on = std::getenv("P2_ENCDBG") != nullptr;
    if (!on) return;
    static std::vector<u32> s0, s1, s2;
    static u64 last_snap = 0;
    static bool was_white = false;
    static std::map<u32, int> hits;
    static int battles = 0;
    const u32 words = static_cast<u32>(mem().size() / 4u);
    const u32 *ram = reinterpret_cast<const u32 *>(mem().raw_pointer(0x08000000u, mem().size()));
    if (!whiteout && now - last_snap >= 30u) {
        last_snap = now;
        s0.swap(s1);
        s1.swap(s2);
        s2.assign(ram, ram + words);
    }
    if (whiteout && !was_white && !s0.empty() && !s1.empty() && !s2.empty()) {
        ++battles;
        std::map<u32, int> now_hits;
        for (u32 i = 0; i < words; ++i) {
            const u32 a = s0[i], b = s1[i], c = s2[i], d = ram[i];
            const bool up = a < b && b < c && c < 100000u && d < c && c - a < 4000u;      // counts up, then reset
            const bool down = a > b && b > c && a < 100000u && d > c && a - c < 4000u;   // counts down, then reloaded
            if (up || down) now_hits[i]++;
        }
        for (const auto &[i, _] : now_hits) ++hits[i];
        LOG_I("encdbg", "battle %d at vblank %llu: %zu candidate words", battles, static_cast<unsigned long long>(now), now_hits.size());
        if (battles % 4 == 0) {
            for (const auto &[i, h] : hits)
                if (h * 10 >= battles * 6 && h >= 3) LOG_I("encdbg", "  %08x seen in %d of %d battles (now %u)", 0x08000000u + i * 4u, h, battles, ram[i]);
        }
    }
    was_white = whiteout;
}

// The white "!" speech bubble above the party leader (something to interact with is in reach).
bool bubble_visible() {
    int white = 0;
    for (int y = 52; y < 80; ++y)
        for (int x = 224; x < 256; ++x) {
            const u32 p = g_frame[static_cast<std::size_t>(y) * 480 + x];
            white += ((p & 0xFF) > 225 && ((p >> 8) & 0xFF) > 225 && ((p >> 16) & 0xFF) > 225) ? 1 : 0;
        }
    return white > 60;
}

// P2_MAPPER=<output file>: dungeon mapping bot. It keeps a grid (cells of 40 world units) of what it has learned
// about the area: for every cell and direction whether walking out of the cell is possible, blocked or unknown.
// It repeatedly plans a route through known passable edges to the nearest cell that still has an unknown
// direction, walks there by steering to cell centres, and probes the direction by pushing into it. "!" interaction
// events (08CA269C) and area exits (a sudden jump of the position) are recorded too. Battles are fought with Cross;
// after an area change or when the party is in trouble the run rolls back to the savestate taken at the start
// (P2_MAPPER_SLOT, default 901). The map is written to the file every ~2000 vblanks and when it is complete:
//   "C <cx> <cz> <up> <down> <left> <right>" (1 passable, 0 blocked, -1 unknown), "V ..." visited cells,
//   "EV <cx> <cz> <dir>" interaction events, "EXIT <cx> <cz> <dir>" area exits.
// Taps that get through result pages, dialogues and level-up screens (Right adds a stat point, Cross confirms).
u32 confirm_taps(u64 now) {
    const u64 t = now % 36u;
    if (t < 3u) return 0x4000u;           // Cross
    if (t >= 12u && t < 15u) return 0x20u; // Right
    if (t >= 24u && t < 27u) return 0x4000u;
    return 0u;
}

u32 mapper_buttons() {
    static const char *out_path = std::getenv("P2_MAPPER");
    if (out_path == nullptr) return 0u;
    static const float kCell = [] { const char *e = std::getenv("P2_MAPPER_CELL"); return e ? static_cast<float>(std::atof(e)) : 48.f; }();
    // kCell is about one step of the party (48 units in the dungeon, ~0.6 in the courtyard-style maps: P2_MAPPER_CELL)
    const float kUnit = kCell / 48.f; // scale of all absolute thresholds below
    constexpr u32 kEventFlag = 0x08CA269Cu, kParty = 0x08E3CCBCu;
    static constexpr u32 kDirButtons[4] = {0x10u, 0x40u, 0x80u, 0x20u};
    static constexpr int kDx[4] = {0, 0, -1, 1}, kDz[4] = {-1, 1, 0, 0};
    using Cell = std::pair<int, int>;
    enum class Mode { Plan, Walk, Probe, Done };
    struct Mapper {
        u32 xaddr{0x08CD4190u}, zaddr{0x08CD4198u};
        std::map<Cell, std::array<int, 4>> edges;
        std::set<Cell> visited;
        std::set<std::tuple<int, int, int>> events, exits;
        Mode mode{Mode::Plan};
        std::vector<Cell> route;
        std::size_t route_i{};
        Cell probe_cell{};
        int probe_dir{};
        float start_x{}, start_z{}, last_x{}, last_z{};
        u64 probe_start{}, last_progress{}, last_save{}, last_check{}, start_vblank{};
        int checkpoint_slot{901};
        bool checkpoint_saved{};
        u64 load_vblank{}, last_now{};
        int rollbacks{};
        float prev_x{}, prev_z{};
        float good_x{}, good_z{};
        bool good_valid{};
        Cell last_cell{INT_MIN, 0};
        std::map<std::tuple<int, int, int>, int> blocked_seen; // how often (cell, dir) was found blocked
        u64 battle_hold{}, wait_until{}, step_start{};
        std::tuple<int, int, int> retry_key{INT_MIN, 0, 0}; // (cell, dir) of a probe that has already failed once
    };
    static Mapper m = [] {
        Mapper r;
        if (const char *slot = std::getenv("P2_MAPPER_SLOT")) r.checkpoint_slot = std::atoi(slot);
        return r;
    }();
    const u64 now = g_kernel.vblank_count();
    // The map is kept in "screen-aligned" coordinates: P2_MAPPER_AXES="ux,uz,rx,rz" gives the world direction of stick UP and
    // RIGHT (default 0,-1,1,0; the courtyard-style areas with a diagonal camera use -1,-1,1,-1).
    static const std::array<float, 4> axes = [] {
        std::array<float, 4> a{0.f, -1.f, 1.f, 0.f};
        if (const char *e = std::getenv("P2_MAPPER_AXES")) std::sscanf(e, "%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3]);
        const float un = std::sqrt(a[0] * a[0] + a[1] * a[1]), rn = std::sqrt(a[2] * a[2] + a[3] * a[3]);
        a[0] /= un; a[1] /= un; a[2] /= rn; a[3] /= rn;
        return a;
    }();
    const float raw_x = guest_float(m.xaddr), raw_z = guest_float(m.zaddr);
    const float x = raw_x * axes[2] + raw_z * axes[3];      // along stick RIGHT
    const float z = -(raw_x * axes[0] + raw_z * axes[1]);   // along stick DOWN
    const Cell here{static_cast<int>(std::floor(x / kCell)), static_cast<int>(std::floor(z / kCell))};
    auto centre = [&](Cell c) { return std::pair<float, float>{(static_cast<float>(c.first) + 0.5f) * kCell, (static_cast<float>(c.second) + 0.5f) * kCell}; };
    auto edge = [&](Cell c) -> std::array<int, 4> & {
        auto it = m.edges.find(c);
        if (it == m.edges.end()) it = m.edges.emplace(c, std::array<int, 4>{-1, -1, -1, -1}).first;
        return it->second;
    };
    auto neighbour = [&](Cell c, int d) { return Cell{c.first + kDx[d], c.second + kDz[d]}; };
    auto opposite = [](int d) { return d ^ 1; }; // 0<->1 (up/down), 2<->3 (left/right)
    auto save_map = [&] {
        std::FILE *f = std::fopen(out_path, "w");
        if (f == nullptr) return;
        for (const auto &[c, e] : m.edges) std::fprintf(f, "C %d %d %d %d %d %d\n", c.first, c.second, e[0], e[1], e[2], e[3]);
        for (const auto &c : m.visited) std::fprintf(f, "V %d %d\n", c.first, c.second);
        for (const auto &[cx, cz, d] : m.events) std::fprintf(f, "EV %d %d %d\n", cx, cz, d);
        for (const auto &[cx, cz, d] : m.exits) std::fprintf(f, "EXIT %d %d %d\n", cx, cz, d);
        std::fclose(f);
    };

    // A savestate load rewinds the vblank counter.
    if (now < m.last_now) {
        m.load_vblank = now;
        m.mode = Mode::Plan;
        m.battle_hold = 0;
        m.last_check = 0;
    }
    m.last_now = now;

    // First call: take the checkpoint used for rollbacks.
    if (!m.checkpoint_saved && now > 20u) {
        m.checkpoint_saved = true;
        savestate_request_save(m.checkpoint_slot);
        m.start_vblank = now;
        LOG_I("mapper", "checkpoint requested in slot %d at (%.0f, %.0f)", m.checkpoint_slot, x, z);
    }
    auto rollback = [&](const char *why) {
        ++m.rollbacks;
        LOG_I("mapper", "rollback %d (%s) at vblank %llu, %zu cells known", m.rollbacks, why, static_cast<unsigned long long>(now), m.edges.size());
        m.load_vblank = now + 1000000u; // block repeated requests until the load has happened
        savestate_request_load(m.checkpoint_slot);
    };

    // Battles and menus: confirm with Cross; the walker resumes afterwards.
    encounter_debug(now, whiteout_screen());
    if (battle_like_screen() || whiteout_screen() || blackout_screen()) m.battle_hold = now + 420u; // results, level-ups and the fade back to the field follow
    if (now < m.battle_hold + 360u && m.battle_hold != 0u) {
        // Still settling after a fight (results, level-ups, loading, fade-in): only confirm, never probe. Right (adds
        // stat points) is only used in the first part: later it would just walk the party around and draw encounters.
        if (now >= m.battle_hold) return now < m.battle_hold + 160u ? confirm_taps(now) : ((now % 60u < 3u) ? 0x4000u : 0u);
    }
    if (now < m.battle_hold) {
        m.last_check = 0;
        m.mode = Mode::Plan;
        m.retry_key = std::make_tuple(INT_MIN, 0, 0);
        m.probe_start = 0;
        int down = 0;
        for (int i = 0; i < 3; ++i) down += mem().load16(kParty + 0x54u * static_cast<u32>(i)) == 0u ? 1 : 0;
        if (down >= 2 && m.checkpoint_saved && now - m.start_vblank > 100u) rollback("party knocked out");
        return battle_taps(now);
    }

    // Large jump of the position since the last settled field frame: another area (or a door transition). Record the exit
    // and start over. (Loading screens in between hide the jump from frame-to-frame checks.)
    const bool settled = now >= m.battle_hold + 360u;
    if (settled && m.good_valid && (std::fabs(x - m.good_x) > 1500.f * kUnit || std::fabs(z - m.good_z) > 1500.f * kUnit)) {
        // The cell the party was in before the jump (last_cell, which is still the old area's cell) is the exit.
        const bool probing = m.mode == Mode::Probe && m.last_cell == m.probe_cell;
        const int dir = probing ? m.probe_dir : 4;
        m.exits.insert({m.last_cell.first, m.last_cell.second, dir});
        if (probing) edge(m.probe_cell)[static_cast<std::size_t>(m.probe_dir)] = 1; // walkable; it just leads elsewhere
        LOG_I("mapper", "area change at (%d,%d) direction %d: now at (%.0f,%.0f)", m.last_cell.first, m.last_cell.second, dir, x, z);
        save_map();
        m.last_cell = {INT_MIN, 0};
        m.good_valid = false;
        rollback("area change");
        return 0u;
    }
    if (settled && !m.good_valid && (std::fabs(x) > 1.f || std::fabs(z) > 1.f)) { m.good_x = x; m.good_z = z; m.good_valid = true; }
    if (settled && m.good_valid && std::fabs(x - m.good_x) < 400.f * kUnit && std::fabs(z - m.good_z) < 400.f * kUnit) { m.good_x = x; m.good_z = z; }
    if (m.last_check == 0u || now - m.last_check >= 8u) { m.prev_x = x; m.prev_z = z; m.last_check = now; }
    // A step between adjacent cells that actually happened is the ground truth for that edge.
    if (m.last_cell.first != INT_MIN && here != m.last_cell) {
        for (int d = 0; d < 4; ++d)
            if (neighbour(m.last_cell, d) == here) {
                edge(m.last_cell)[static_cast<std::size_t>(d)] = 1;
                edge(here)[static_cast<std::size_t>(opposite(d))] = 1;
            }
    }
    m.last_cell = here;
    m.visited.insert(here);
    if (bubble_visible()) m.events.insert({here.first, here.second, 4}); // 4 = reachable from this cell, direction unknown
    if (now - m.last_save > 2000u) { m.last_save = now; save_map(); LOG_I("mapper", "vblank %llu at (%.1f, %.1f): %zu cells, %zu visited, %zu events, %zu exits [mode %d, hold %lld, wait %lld]", static_cast<unsigned long long>(now), x, z, m.edges.size(), m.visited.size(), m.events.size(), m.exits.size(), static_cast<int>(m.mode), static_cast<long long>(m.battle_hold) - static_cast<long long>(now), static_cast<long long>(m.wait_until) - static_cast<long long>(now)); }
    if (m.mode == Mode::Done) {
        // Done only means "nothing reachable is unknown"; look again now and then (observed steps add edges).
        if (now % 200u != 0u) return 0u;
        m.mode = Mode::Plan;
    }

    // Plan: nearest cell (through known passable edges) with an unknown direction.
    auto plan = [&]() {
        std::map<Cell, Cell> parent;
        std::vector<Cell> queue{here};
        parent[here] = here;
        for (std::size_t qi = 0; qi < queue.size(); ++qi) {
            const Cell c = queue[qi];
            const auto &e = edge(c);
            for (int d = 0; d < 4; ++d)
                if (e[static_cast<std::size_t>(d)] == -1 ||
                    (e[static_cast<std::size_t>(d)] == 0 && m.blocked_seen[std::make_tuple(c.first, c.second, d)] < 2)) {
                    // reached a cell with something to learn
                    std::vector<Cell> path;
                    for (Cell p = c; p != here; p = parent[p]) path.push_back(p);
                    std::reverse(path.begin(), path.end());
                    m.route = path;
                    m.route_i = 0;
                    m.probe_cell = c;
                    m.probe_dir = d;
                    return true;
                }
            for (int d = 0; d < 4; ++d) {
                if (e[static_cast<std::size_t>(d)] != 1) continue;
                const Cell n = neighbour(c, d);
                if (parent.count(n)) continue;
                parent[n] = c;
                queue.push_back(n);
            }
        }
        return false;
    };

    if (now < m.wait_until) return (now % 36u < 3u) ? 0x2000u : 0u; // Circle: say no to whatever prompt blocks the walk (an exit door asks "leave?")
    if (m.mode == Mode::Plan) {
        bool found = plan();
        if (!found) {
            // Cut off from everything we know (a battle can put the party elsewhere): forget what we think about the
            // surroundings of this cell and probe it again.
            auto &e = edge(here);
            bool all_known = true;
            for (int d = 0; d < 4; ++d) all_known = all_known && e[static_cast<std::size_t>(d)] != -1;
            if (all_known) {
                for (int d = 0; d < 4; ++d) {
                    e[static_cast<std::size_t>(d)] = -1;
                    m.blocked_seen.erase(std::make_tuple(here.first, here.second, d));
                }
                LOG_I("mapper", "isolated at (%d,%d): probing it afresh", here.first, here.second);
                found = plan();
            }
        }
        if (!found) {
            { const auto &e = edge(here); LOG_I("mapper", "no unknown edges reachable from (%d,%d): %d %d %d %d, %zu cells", here.first, here.second, e[0], e[1], e[2], e[3], m.edges.size()); }
            LOG_I("mapper", "mapping complete: %zu cells, %zu events, %zu exits", m.edges.size(), m.events.size(), m.exits.size());
            save_map();
            m.mode = Mode::Done;
            return 0u;
        }
        m.mode = m.route.empty() ? Mode::Probe : Mode::Walk;
        m.last_progress = now;
        m.step_start = now;
        m.last_x = x; m.last_z = z;
        m.probe_start = 0; // a probe starts only once the cell is centred
    }

    if (m.mode == Mode::Walk) {
        const Cell target = m.route[m.route_i];
        static const bool walk_verbose = std::getenv("P2_MAPPER_LOG") != nullptr;
        if (walk_verbose && now % 100u == 0u) LOG_I("mapper", "walking: step %zu/%zu to (%d,%d), here (%d,%d), pos (%.2f,%.2f), progress %lld ago, step %lld old", m.route_i, m.route.size(), target.first, target.second, here.first, here.second, x, z, static_cast<long long>(now - m.last_progress), static_cast<long long>(now - m.step_start));
        const auto [tx, tz] = centre(target);
        const float dx = tx - x, dz = tz - z;
        if (here == target) {
            m.step_start = now;
            if (++m.route_i >= m.route.size()) {
                m.mode = Mode::Probe; // arrived in the probe cell
                m.probe_start = 0;
            }
            return 0u;
        }
        // no progress for a while: the map was wrong about this edge
        if (std::fabs(x - m.last_x) + std::fabs(z - m.last_z) > 0.12f * kCell) { m.last_x = x; m.last_z = z; m.last_progress = now; }
        if (now - m.last_progress > 40u || now - m.step_start > 360u) { // (jittering against a wall counts as progress, so the step has a time limit too)
            const Cell from = m.route_i == 0 ? here : m.route[m.route_i - 1];
            for (int d = 0; d < 4; ++d)
                if (neighbour(from, d) == target) {
                    edge(from)[static_cast<std::size_t>(d)] = 0;
                    edge(target)[static_cast<std::size_t>(opposite(d))] = 0;
                    m.blocked_seen[std::make_tuple(from.first, from.second, d)] += 2; // do not probe this edge again
                    m.blocked_seen[std::make_tuple(target.first, target.second, opposite(d))] += 2;
                }
            m.mode = Mode::Plan;
            return 0u;
        }
        // steer along the axis with the larger error (one axis at a time keeps to the corridor)
        if (std::fabs(dx) >= std::fabs(dz)) return dx < 0 ? kDirButtons[2] : kDirButtons[3];
        return dz < 0 ? kDirButtons[0] : kDirButtons[1];
    }

    // Probe: centre first, then push into the direction.
    {
        const auto [cx, cz] = centre(m.probe_cell);
        if (m.probe_start == 0u) {
            m.probe_start = now;
            m.start_x = x; m.start_z = z;
            m.last_x = x; m.last_z = z; m.last_progress = now;
        }
        const int d = m.probe_dir;
        static const bool verbose = std::getenv("P2_MAPPER_LOG") != nullptr;
        if (verbose && now % 20u == 0u) LOG_I("mapper", "probing (%d,%d) dir %d at (%.0f,%.0f) start (%.0f,%.0f)", m.probe_cell.first, m.probe_cell.second, d, x, z, m.start_x, m.start_z);
        const float moved = (x - m.start_x) * static_cast<float>(kDx[d]) + (z - m.start_z) * static_cast<float>(kDz[d]);
        if (bubble_visible()) m.events.insert({m.probe_cell.first, m.probe_cell.second, d});
        if (std::fabs(x - m.last_x) + std::fabs(z - m.last_z) > 0.12f * kCell) { m.last_x = x; m.last_z = z; m.last_progress = now; }
        const bool stalled = now - m.last_progress > 22u && now - m.probe_start > 22u; // turning to face the new direction takes a moment
        if (moved >= 0.75f * kCell) {
            if (verbose) LOG_I("mapper", "cell (%d,%d) dir %d: passable (moved %.0f)", m.probe_cell.first, m.probe_cell.second, d, moved);
            edge(m.probe_cell)[static_cast<std::size_t>(d)] = 1;
            edge(neighbour(m.probe_cell, d))[static_cast<std::size_t>(opposite(d))] = 1;
            m.mode = Mode::Plan;
            return 0u;
        }
        if (stalled || now - m.probe_start > 70u) {
            // A wall must be seen twice (a battle flash or a door prompt also stops the walk).
            const auto key = std::make_tuple(m.probe_cell.first, m.probe_cell.second, d);
            if (m.retry_key != key) {
                // Unresponsive: maybe a dialogue, a level-up page or a prompt. Confirm for a while, then try again.
                m.retry_key = key;
                m.probe_start = 0;
                m.last_progress = now;
                m.wait_until = now + 60u;
                return 0u;
            }
            m.retry_key = std::make_tuple(INT_MIN, 0, 0);
            if (verbose) LOG_I("mapper", "cell (%d,%d) dir %d: blocked (moved %.0f) at (%.0f,%.0f), battle_hold %llu now %llu", m.probe_cell.first, m.probe_cell.second, d, moved, x, z,
                               static_cast<unsigned long long>(m.battle_hold), static_cast<unsigned long long>(now));
            edge(m.probe_cell)[static_cast<std::size_t>(d)] = 0;
            edge(neighbour(m.probe_cell, d))[static_cast<std::size_t>(opposite(d))] = 0;
            ++m.blocked_seen[std::make_tuple(m.probe_cell.first, m.probe_cell.second, d)];
            ++m.blocked_seen[std::make_tuple(neighbour(m.probe_cell, d).first, neighbour(m.probe_cell, d).second, opposite(d))];
            m.mode = Mode::Plan;
            return 0u;
        }
        return kDirButtons[d];
    }
}

// The dialogue box: its red top border across the lower part of the screen (rows 192-193).
bool dialogue_visible() {
    int red = 0, total = 0;
    for (int y = 192; y < 194; ++y)
        for (int x = 20; x < 460; ++x) {
            const u32 p = g_frame[static_cast<std::size_t>(y) * 480 + x];
            red += ((p & 0xFF) > 120 && ((p >> 8) & 0xFF) < 60 && ((p >> 16) & 0xFF) < 60) ? 1 : 0;
            ++total;
        }
    return red * 10 > total * 8;
}

// P2_TOUR=<map file>[,first[,last]]: walks to the interaction spots recorded in a map (see P2_MAPPER), faces each one and
// presses Cross, then pages through whatever happens (dialogue, prompts are declined with Circle afterwards).
// Every visit is logged ("[tour] #i cell (x,z) dir d: interacting at vblank V") so the frames can be inspected.
// Spots are taken in the order of the file; first/last select a range. P2_TOUR_NO_CROSS=1 only walks and faces.
u32 tour_buttons() {
    static const char *env = std::getenv("P2_TOUR");
    if (env == nullptr) return 0u;
    static const float kCell = [] { const char *e = std::getenv("P2_MAPPER_CELL"); return e ? static_cast<float>(std::atof(e)) : 48.f; }();
    static constexpr u32 kDirButtons[4] = {0x10u, 0x40u, 0x80u, 0x20u};
    static constexpr int kDx[4] = {0, 0, -1, 1}, kDz[4] = {-1, 1, 0, 0};
    using Cell = std::pair<int, int>;
    enum class Phase { Goto, Face, Interact, Wait, Done };
    struct Tour {
        std::map<Cell, std::array<int, 4>> edges;
        std::vector<std::tuple<int, int, int>> targets; // (cell x, cell z, dir)
        std::size_t index{};
        std::size_t last{};
        std::vector<Cell> done_cells; // cells interacted from (nearby spots are the same object)
        Phase phase{Phase::Goto};
        u64 phase_start{};
        bool loaded{};
        bool no_cross{};
        Cell last_here{INT_MIN, 0};
        u64 last_move{};
        float last_x{}, last_z{};
    };
    static Tour t;
    const u64 now = g_kernel.vblank_count();
    if (!t.loaded) {
        t.loaded = true;
        char path[512] = {0};
        unsigned first = 0, last = 100000;
        std::sscanf(env, "%511[^,],%u,%u", path, &first, &last);
        t.no_cross = std::getenv("P2_TOUR_NO_CROSS") != nullptr;
        std::FILE *f = std::fopen(path, "r");
        if (f == nullptr) { LOG_E("tour", "cannot open map %s", path); t.phase = Phase::Done; return 0u; }
        char tag[16];
        int a, b, c, d, e, g;
        char line[256];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            if (std::sscanf(line, "%15s", tag) != 1) continue;
            if (std::string(tag) == "C" && std::sscanf(line, "C %d %d %d %d %d %d", &a, &b, &c, &d, &e, &g) == 6) t.edges[{a, b}] = {c, d, e, g};
            if (std::string(tag) == "EV" && std::sscanf(line, "EV %d %d %d", &a, &b, &c) == 3 && c >= 0 && c < 4) t.targets.emplace_back(a, b, c);
        }
        std::fclose(f);
        t.index = first;
        t.last = std::min<std::size_t>(last, t.targets.size());
        LOG_I("tour", "%zu cells, %zu interaction spots, tour %zu..%zu", t.edges.size(), t.targets.size(), t.index, t.last);
        t.phase_start = now;
    }
    // Dialogues, transitions: keep confirming and freeze all the tour's timers while they last.
    {
        static u64 busy_until = 0, last_now = 0;
        const u64 elapsed = last_now == 0 || now < last_now ? 0u : now - last_now; // (called more than once per vblank)
        last_now = now;
        if (dialogue_visible() || whiteout_screen() || blackout_screen()) busy_until = now + 60u;
        if (now < busy_until) {
            t.phase_start += elapsed;
            // Cross pages through the text; it must not be pressed once the box is gone (that talks to the person again).
            return (dialogue_visible() && now % 60u < 5u) ? 0x4000u : 0u; // (the game needs ~5-frame presses with a gap)
        }
    }
    if (t.phase == Phase::Done || t.index >= t.last) {
        if (t.phase != Phase::Done) { LOG_I("tour", "tour finished"); t.phase = Phase::Done; }
        return 0u;
    }
    static const std::array<float, 4> axes = [] {
        std::array<float, 4> a{0.f, -1.f, 1.f, 0.f};
        if (const char *e = std::getenv("P2_MAPPER_AXES")) std::sscanf(e, "%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3]);
        const float un = std::sqrt(a[0] * a[0] + a[1] * a[1]), rn = std::sqrt(a[2] * a[2] + a[3] * a[3]);
        a[0] /= un; a[1] /= un; a[2] /= rn; a[3] /= rn;
        return a;
    }();
    const float raw_x = guest_float(0x08CD4190u), raw_z = guest_float(0x08CD4198u);
    const float x = raw_x * axes[2] + raw_z * axes[3], z = -(raw_x * axes[0] + raw_z * axes[1]);
    const Cell here{static_cast<int>(std::floor(x / kCell)), static_cast<int>(std::floor(z / kCell))};
    const auto [tx, tz, td] = t.targets[t.index];
    const Cell target{tx, tz};
    if (t.phase == Phase::Goto && t.phase_start == t.phase_start) {
        static const int radius = [] { const char *e = std::getenv("P2_TOUR_SPREAD"); return e ? std::atoi(e) : 4; }();
        for (const Cell &c : t.done_cells)
            if (std::max(std::abs(c.first - tx), std::abs(c.second - tz)) < radius) { ++t.index; return 0u; }
    }

    if (t.phase == Phase::Goto) {
        if (here == target) { t.phase = Phase::Face; t.phase_start = now; return 0u; }
        // breadth-first search over passable edges; the first step of the shortest path is where to go next
        std::map<Cell, Cell> parent;
        std::vector<Cell> queue{target};
        parent[target] = target;
        bool reached = false;
        for (std::size_t qi = 0; qi < queue.size() && !reached; ++qi) {
            const Cell c = queue[qi];
            const auto it = t.edges.find(c);
            if (it == t.edges.end()) continue;
            for (int d = 0; d < 4; ++d) {
                if (it->second[static_cast<std::size_t>(d)] != 1) continue;
                const Cell n{c.first + kDx[d], c.second + kDz[d]};
                if (parent.count(n)) continue;
                parent[n] = c;
                if (n == here) { reached = true; break; }
                queue.push_back(n);
            }
        }
        if (!reached) {
            LOG_I("tour", "#%zu cell (%d,%d): no known route from (%d,%d), skipping", t.index, tx, tz, here.first, here.second);
            ++t.index;
            return 0u;
        }
        const Cell next = parent[here];
        // stuck? (the map says passable but the party does not move)
        if (std::fabs(x - t.last_x) + std::fabs(z - t.last_z) > 3.f * (kCell / 48.f)) { t.last_x = x; t.last_z = z; t.last_move = now; }
        if (now - t.phase_start > 3000u) { LOG_I("tour", "#%zu timed out, skipping", t.index); ++t.index; t.phase_start = now; return 0u; }
        for (int d = 0; d < 4; ++d)
            if (here.first + kDx[d] == next.first && here.second + kDz[d] == next.second) return kDirButtons[d];
        return 0u;
    }
    if (t.phase == Phase::Face) {
        if (now - t.phase_start < 14u) return kDirButtons[td];
        t.phase = Phase::Interact;
        t.phase_start = now;
        return 0u;
    }
    if (t.phase == Phase::Interact) {
        if (now == t.phase_start + 1u) t.done_cells.push_back(target);
        if (now == t.phase_start + 1u) LOG_I("tour", "#%zu cell (%d,%d) dir %d: interacting at vblank %llu", t.index, tx, tz, td, static_cast<unsigned long long>(now));
        if (now - t.phase_start < 3u && !t.no_cross) return 0x4000u;
        if (now - t.phase_start < 8u) return 0u;
        t.phase = Phase::Wait;
        t.phase_start = now;
        return 0u;
    }
    // Wait: page through the result (Cross twice), then back out with Circle
    const u64 age = now - t.phase_start;
    if (age < 180u) return (age % 40u < 3u) ? 0x4000u : 0u;
    if (age < 300u) return (age % 40u < 3u) ? 0x2000u : 0u;
    ++t.index;
    t.phase = Phase::Goto;
    t.phase_start = now;
    return 0u;
}

u32 scripted_buttons() {
    const u64 now = g_kernel.vblank_count();
    u32 b = autopilot_buttons() | climb_buttons() | explore_buttons() | mapper_buttons() | tour_buttons();
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
    readwatch_tick();
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
                // A value written as xHEX is stored as a raw 32-bit integer (e.g. x2).
                float value;
                if (end[1] == 'x') {
                    const u32 raw = static_cast<u32>(std::strtoul(end + 2, &end, 16));
                    std::memcpy(&value, &raw, 4);
                } else {
                    value = std::strtof(end + 1, &end);
                }
                v.emplace_back(addr, value);
                p = end != nullptr && *end == ',' ? end : nullptr;
            }
        }
        return v;
    }();
    static const u64 poke_at = [] { const char *e = std::getenv("P2_POKE"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
    static const bool poke_hold = std::getenv("P2_POKE_HOLD") != nullptr;
    if (!pokes.empty() && (poke_hold ? K.vblank_count() >= poke_at : K.vblank_count() == poke_at)) {
        for (const auto &[addr, value] : pokes) {
            u32 bits;
            std::memcpy(&bits, &value, 4);
            mem().store32(addr, bits);
            if (!poke_hold) LOG_I("poke", "%08x = %.3f", addr, static_cast<double>(value));
        }
    }
    if (!watch.empty()) {
        std::string line;
        for (u32 a : watch) {
            const u32 bits = mem().load32(a);
            float f;
            std::memcpy(&f, &bits, 4);
            char buf[32];
            static const bool as_int = std::getenv("P2_WATCH_INT") != nullptr;
            if (as_int) std::snprintf(buf, sizeof(buf), " %u", bits);
            else std::snprintf(buf, sizeof(buf), " %.3f", static_cast<double>(f));
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
