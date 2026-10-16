// Hand-written replacements for guest functions (see native.hpp). The list of replaced entries lives in
// game/overrides.txt; each P2_NATIVE line at the bottom binds a C++ function to one of them. Names are
// guesses from reading the code: the game ships without symbols.
#include "native.hpp"
#include "kernel.hpp"
#include "savestate.hpp"

#include <array>
#include <cstdlib>
#include <map>

namespace p2::game {

// ===== game state ===========================================================================================

// Controller. Buttons are PSP bits; the d-pad is 0x10 up, 0x20 right, 0x40 down, 0x80 left.
constexpr u32 PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80;
Global<u32, 0x08CA29A8> g_pad_down;      // buttons down this frame
Global<u32, 0x08CA29A4> g_pad_down_prev; // buttons down the frame before

// Button remapping: twelve configured button masks, and two sets of twelve enable flags choosing which of
// them count for the logical-button queries.
constexpr int BUTTON_SLOTS = 12;
using ButtonFlags = std::array<u8, BUTTON_SLOTS>;
Global<ButtonFlags, 0x08CA292C> g_button_enabled_a;
Global<ButtonFlags, 0x08D26F68> g_button_enabled_b;
Global<std::array<u32, BUTTON_SLOTS>, 0x08BD5FA0> g_button_masks;

Global<u32, 0x08E34150> g_rng_seed;
Global<u32, 0x08CA29DC> g_map_flag;
Global<u32, 0x08CE2CF4> g_pool_ptr;
Global<std::array<u32, 64>, 0x08E4452C> g_map_slots;

// 4.12 fixed-point sine/cosine table, indexed by a 12-bit angle.
struct TrigEntry { u16 sin, cos; };
Global<std::array<TrigEntry, 4096>, 0x08BB6584> g_trig_table;

// ===== structures ===========================================================================================

struct Vec3 { s16 x, y, z; };
struct Mat3 { s16 m[9]; };

// A fixed-stride slot pool: header, then `count` slots of `stride` bytes starting at +0x10.
struct Pool {
    s16 count, stride;
    u32 reserved;
    u32 user8, userC;
};
u32 pool_slot(const Pool *pool, s32 index) {
    return guest_address(pool) + sizeof(Pool) + static_cast<u32>(index * pool->stride);
}

// ===== controller ===========================================================================================

// 08807918: any button of `mask` is down.
bool pad_held(u32 mask) { return (mask & g_pad_down) != 0; }

// 0880792C: a button of `mask` went down this frame (down now, not on the previous frame).
bool pad_pressed(u32 mask) { return (mask & g_pad_down) != 0 && (mask & g_pad_down_prev) == 0; }

u32 mapped_buttons(const ButtonFlags &enabled) {
    u32 mask = 0;
    for (int i = 0; i < BUTTON_SLOTS; ++i)
        if (enabled[i]) mask |= g_button_masks.get()[i];
    return mask;
}

// A d-pad direction only counts while none of the other three d-pad buttons is down (no diagonals).
bool dpad_blocked(u32 mask) {
    switch (mask) {
    case PAD_UP: return (g_pad_down & (PAD_DOWN | PAD_LEFT | PAD_RIGHT)) != 0;
    case PAD_RIGHT: return (g_pad_down & (PAD_DOWN | PAD_LEFT | PAD_UP)) != 0;
    case PAD_DOWN: return (g_pad_down & (PAD_UP | PAD_LEFT | PAD_RIGHT)) != 0;
    case PAD_LEFT: return (g_pad_down & (PAD_DOWN | PAD_UP | PAD_RIGHT)) != 0;
    default: return false;
    }
}

// 08807968 and 08807AC4 (the compiler emitted two identical copies): a button of `mask` is mapped (set B).
bool pad_mapped_b(u32 mask) { return (mapped_buttons(g_button_enabled_b) & mask) != 0; }

// 088079AC: mapped buttons (set A) & mask, with d-pad directions made exclusive.
u32 pad_dir_a(u32 mask) { return dpad_blocked(mask) ? 0u : (mapped_buttons(g_button_enabled_a) & mask); }

// 08807B08: the same for set B, as a boolean.
bool pad_dir_b(u32 mask) { return !dpad_blocked(mask) && (mapped_buttons(g_button_enabled_b) & mask) != 0; }

// ===== misc =================================================================================================

// 08851C30: the game's random number generator; returns 31 random bits.
u32 game_rand() {
    static const bool log_callers = std::getenv("P2_RANDLOG") != nullptr;
    if (log_callers) {
        const u32 ra = rt().cpu().gpr[31];
        if (ra != 0x0880517Cu && ra != 0x089626E4u)
            LOG_I("rand", "call ra=%08x v=%llu", ra, static_cast<unsigned long long>(g_kernel.vblank_count()));
    }
    const u32 seed = g_rng_seed;
    const u32 t = seed * 41u;
    const u32 hi = t >> 16;
    const u32 next = seed == 0u ? 0x2A6D365Au : (hi | ((t + hi) << 16));
    g_rng_seed = next;
    return next >> 1;
}

// 08863DF4: out = v * m in 4.12 fixed point (the rows of m are the basis vectors). The sums wrap at 32 bits
// like the original mult/madd chain. `out` may alias `v`.
Vec3 *vec3_mul_mat3(const Mat3 *m, const Vec3 *v, Vec3 *out) {
    const s32 in[3] = {v->x, v->y, v->z};
    s16 result[3];
    for (int col = 0; col < 3; ++col) {
        const u32 sum = static_cast<u32>(in[0] * m->m[col]) + static_cast<u32>(in[1] * m->m[3 + col]) +
                        static_cast<u32>(in[2] * m->m[6 + col]);
        result[col] = static_cast<s16>(static_cast<s32>(sum) >> 12);
    }
    out->x = result[0];
    out->y = result[1];
    out->z = result[2];
    return out;
}

// 088FE140: number of pool slots whose first halfword has bit 0 set.
u32 pool_count_flagged(const Pool *pool) {
    u32 flagged = 0;
    for (s32 i = 0; i < pool->count; ++i) flagged += *guest<u16>(pool_slot(pool, i)) & 1u;
    return flagged;
}

// 088FDF74: sets up a pool and clears the first halfword of each slot.
void pool_init(Pool *pool, s16 count, s16 stride, u32 user8, u32 userC) {
    pool->count = count;
    pool->stride = stride;
    pool->reserved = 0;
    pool->user8 = user8;
    pool->userC = userC;
    for (s32 i = 0; i < count; ++i) *guest<u16>(pool_slot(pool, i)) = 0;
}

// 08866818 / 08866850: sine and cosine of a 12-bit angle as 4.12 fixed point. Table entries with bit 15 set
// encode negative values as 0x8000 | (0x1000 + x).
s32 trig_value(u16 raw) { return raw > 0x7FFF ? static_cast<s32>(raw & 0xFFF) - 0x1000 : raw; }
s32 sin_fixed(u32 angle) { return trig_value(g_trig_table.get()[angle & 0xFFF].sin); }
s32 cos_fixed(u32 angle) { return trig_value(g_trig_table.get()[angle & 0xFFF].cos); }

// ===== entities =============================================================================================

// The object table: a pointer to a header followed by `count` 0x54-byte entities. A zero `flags` word means
// the slot is free.
struct Entity {
    u32 flags;
    u8 rest[0x50];
};
struct EntityPool {
    u32 unknown0;
    s16 count;
    u8 unknown6[0x4E];
};
static_assert(sizeof(Entity) == 0x54 && sizeof(EntityPool) == 0x54);
Global<u32, 0x08CDD898> g_entity_pool_ptr;

Entity *entity_at(s32 index) {
    return guest<Entity>(g_entity_pool_ptr + sizeof(EntityPool) + static_cast<u32>(index) * sizeof(Entity));
}

// 089228B0: marks entity `index` with flag bits 1 and 2.
void entity_set_flags6(s16 index) { entity_at(index)->flags |= 6u; }

// 08922938: entity `index` is in use.
bool entity_active(s16 index) { return entity_at(index)->flags != 0u; }

// 089228E8: at least one entity is in use. (For a negative count the original answers true.)
bool entity_any_active() {
    const s32 count = guest<EntityPool>(g_entity_pool_ptr)->count;
    s32 index = 0;
    while (index < count && entity_at(index)->flags == 0u) ++index;
    return index != count;
}

// ===== small tables and copies ==============================================================================

Global<std::array<u32, 128>, 0x08CCC868> g_table_c868;
Global<std::array<u32, 128>, 0x08CCC668> g_table_c668;
Global<u32, 0x08CD80E4> g_word_80e4;
constexpr u32 kObjectAt60e4 = 0x08CD60E4;
struct Point16 { u16 x, y; };
Global<Point16, 0x08E41274> g_view_point;

u32 table_c868_get(u32 index) { return g_table_c868.get()[index]; } // 088523F0
u32 table_c668_get(u32 index) { return g_table_c668.get()[index]; } // 0885244C
u32 get_word_80e4() { return g_word_80e4; }                          // 088EB73C
u32 get_object_60e4() { return kObjectAt60e4; }                      // 088EB8DC
void get_view_point(Point16 *out) { *out = g_view_point.get(); }     // 08864A24

// ===== town map cursor ======================================================================================

// The town map is a graph. Nodes are road junctions, places and event spots; up to four edges leave each node.
// Edge ids in a node's list have bit 7 set when the node is the edge's far end (node_b), so the edge is
// walked backwards. Angles use 4096 units per turn, 0 = east, 1024 = north.
struct MapNode {
    u16 x, y;
    u8 type; // 01 road, 04 place (IN), 10 event gate, 20 map exit, 40 person event
    u8 unknown5[3];
    u8 place;
    u8 unknown9;
    u8 edges[4]; // 0xFF = none
    u8 unknown14[2];
};
struct MapEdge {
    u16 length;
    s16 angle; // direction from node_a to node_b
    u8 node_a, node_b;
    u16 unknown6;
};
struct MapCursor {
    u32 x, y;       // 20.12 fixed point
    s32 ix, iy;     // integer position
    u32 progress;   // 20.12 distance along the current edge, measured from node_b
    u8 unknown14[2];
    u8 edge;        // current edge while travelling
    u8 node;        // current node while standing on one, otherwise 0xFF
};
static_assert(sizeof(MapNode) == 16 && sizeof(MapEdge) == 8 && sizeof(MapCursor) == 0x18);

// For each of the eight stick directions: the pad bits, and two angle windows [w0,w1) and [w2,w3) of the
// edges that direction may select (+-67.5 degrees around its heading; the second window handles wrap-around).
struct DirWindow { u16 w[4]; };
Global<std::array<u8, 8>, 0x08BBCA80> g_stick_dirs; // up, right, down, left, up+right, right+down, down+left, left+up
Global<std::array<DirWindow, 8>, 0x08C1FDEC> g_dir_windows;
Global<std::array<u8, 4>, 0x08CD3D30> g_dir_history; // last four directions taken (index into g_stick_dirs), newest last
Global<u32, 0x08E4435C> g_map_nodes_ptr;
Global<u32, 0x08E44514> g_map_edges_ptr;
Global<MapCursor, 0x08E444F8> g_map_cursor;
Global<u16, 0x08E43578> g_map_cursor_idle;

// The edge slot of `node` that stick direction `index` (0..7, as in g_stick_dirs) selects, or -1 if none lies
// inside that direction's window.
int pick_slot(const MapNode &node, int index) {
    auto edge_entry = [](u32 id) { return guest<MapEdge>(g_map_edges_ptr + id * sizeof(MapEdge)); };

    // Heading of each outgoing edge, away from this node (-1 where the slot is empty).
    s32 heading[4];
    for (int k = 0; k < 4; ++k) {
        const u8 id = node.edges[k];
        if (id == 0xFF) {
            heading[k] = -1;
        } else if (id & 0x80) { // walked backwards: turn around, as signed modulo 4096
            const s32 turned = edge_entry(id & 0x7F)->angle + 0x1800;
            const s32 bias = static_cast<s32>(static_cast<u32>(turned >> 31) >> 20);
            heading[k] = ((turned + bias) & 0xFFF) - bias;
        } else {
            heading[k] = edge_entry(id)->angle;
        }
    }

    // Distance of each heading to the window centres (w1 and w2); the nearest edge wins for each centre, and
    // the nearer of the two winners is taken. Empty slots have the largest possible distance.
    const DirWindow &window = g_dir_windows.get()[index];
    u32 best_distance[2];
    int best_slot[2];
    for (int j = 0; j < 2; ++j) {
        const s32 centre = window.w[1 + j];
        u32 distance[4];
        for (int k = 0; k < 4; ++k) {
            const s32 d = heading[k] - centre;
            distance[k] = heading[k] == -1 ? 0xFFFFFFFFu : static_cast<u32>(d > -d ? d : -d);
        }
        best_slot[j] = 0;
        best_distance[j] = distance[0];
        for (int k = 1; k < 4; ++k) {
            if (distance[k] < best_distance[j]) {
                best_distance[j] = distance[k];
                best_slot[j] = k;
            }
        }
    }
    const int slot = best_distance[1] < best_distance[0] ? best_slot[1] : best_slot[0];
    const s32 angle = heading[slot];
    const bool inside = (angle >= window.w[0] && angle < window.w[1]) || (angle >= window.w[2] && angle < window.w[3]);
    if (!inside || node.edges[slot] == 0xFF) return -1;
    return slot;
}

// 0889E76C: the cursor stands on a node and the stick points somewhere: pick the edge to walk. Of the node's
// four edges the one whose angle is closest to the stick's heading is chosen, if it lies inside that
// direction's window; the cursor then leaves the node along it.
void map_cursor_pick_edge(u32 stick) {
    u32 dir = stick & 0xFF;
    if ((dir & PAD_UP) && (dir & PAD_DOWN)) dir &= 0xBF;  // up+down: up wins
    if ((dir & PAD_RIGHT) && (dir & PAD_LEFT)) dir &= 0x7F; // right+left: right wins

    MapCursor &cursor = g_map_cursor;
    if (cursor.node == 0xFF) return; // already travelling

    const auto &dirs = g_stick_dirs.get();
    int index = 0;
    while (index < 8 && dirs[index] != dir) ++index;
    if (index == 8) return; // not one of the eight stick directions

    // Pressing towards the opposite of the previous move only counts if that opposite was already part of
    // the recent history; then it is taken as the heading instead of the stick's own direction.
    const auto &history = g_dir_history.get();
    u32 opposite = (history[3] + 2u) & 0xFF;
    if (opposite >= 4) opposite &= 1;
    if ((dir & dirs[opposite]) && (history[0] == opposite || history[1] == opposite || history[2] == opposite))
        index = static_cast<int>(opposite);

    const MapNode &node = *guest<MapNode>(g_map_nodes_ptr + cursor.node * sizeof(MapNode));
    const int slot = pick_slot(node, index);
    if (slot < 0) return;
    const u8 chosen = node.edges[slot];

    // Leave the node: progress counts from node_b, so an edge listed without the flag starts at full length.
    auto edge_entry = [](u32 id) { return guest<MapEdge>(g_map_edges_ptr + id * sizeof(MapEdge)); };
    cursor.progress = (chosen & 0x80) ? 0u : static_cast<u32>(edge_entry(chosen)->length) << 12;
    cursor.node = 0xFF;
    cursor.edge = chosen & 0x7F;
    g_map_cursor_idle = 0;
}

u32 get_map_flag() { return g_map_flag; }                        // 08807C58
u32 get_pool_ptr() { return g_pool_ptr; }                        // 08963E4C
u32 get_map_slot(u32 index) { return g_map_slots.get()[index]; } // 088A0508

} // namespace p2::game

namespace p2 {

// Random inputs for the differential fuzzer (P2_NATIVE_FUZZ): arguments are already random; here the RAM a
// replaced function reads is randomized too, biased so every branch gets exercised.
void native_fuzz_setup(u32 address, std::mt19937 &rng, AllegrexContext &c) {
    using namespace game;
    auto bits = [&](u32 count) { // random word with about `count` bits set
        u32 w = 0;
        for (u32 i = 0; i < count; ++i) w |= 1u << (rng() % 32u);
        return w;
    };
    // d-pad bits are 0x10-0x80; keep them frequent in both pad words and in the mask argument
    g_pad_down = (rng() & 1u) ? (bits(2) & 0xF0u) | bits(2) : bits(4);
    g_pad_down_prev = (rng() & 1u) ? (bits(2) & 0xF0u) | bits(2) : bits(4);
    for (int i = 0; i < BUTTON_SLOTS; ++i) {
        g_button_enabled_a.get()[i] = rng() & 1u ? static_cast<u8>(rng()) : 0u;
        g_button_enabled_b.get()[i] = rng() & 1u ? static_cast<u8>(rng()) : 0u;
        g_button_masks.get()[i] = bits(1 + rng() % 3u);
    }
    switch (rng() % 4u) {
    case 0: c.gpr[4] = PAD_UP << (rng() % 4u); break; // a single d-pad direction
    case 1: c.gpr[4] = bits(1 + rng() % 3u); break;
    default: break;                                     // fully random
    }
    g_map_flag = rng();

    // Scratch buffers for pointer arguments: random bytes, three disjoint areas.
    constexpr u32 kScratch = 0x09E00000u;
    auto fill = [&](u32 base, u32 bytes) {
        for (u32 i = 0; i < bytes; i += 4) mem().store32(base + i, rng());
    };
    switch (address) {
    case 0x08851C30u:
        g_rng_seed = (rng() & 7u) == 0u ? 0u : rng();
        break;
    case 0x08863DF4u:
        fill(kScratch, 0x300);
        c.gpr[4] = kScratch;
        c.gpr[5] = kScratch + 0x100;
        c.gpr[6] = (rng() & 3u) == 0u ? kScratch + 0x100 : kScratch + 0x200; // sometimes in place
        break;
    case 0x088FE140u:
        fill(kScratch, 0x400);
        mem().store16(kScratch, rng() % 12u);
        mem().store16(kScratch + 2, 2u * (1 + rng() % 16u));
        c.gpr[4] = kScratch;
        break;
    case 0x088FDF74u:
        fill(kScratch, 0x400);
        c.gpr[4] = kScratch;
        c.gpr[5] = static_cast<u32>(static_cast<s32>(rng() % 24u) - 3);
        c.gpr[6] = 2u * (1 + rng() % 16u);
        break;
    case 0x08866818u:
    case 0x08866850u:
        fill(0x08BB6584u, 4096u * 4u);
        break;
    case 0x08963E4Cu:
        g_pool_ptr = rng();
        break;
    case 0x088A0508u:
        c.gpr[4] &= 0x3Fu;
        fill(0x08E4452Cu, 0x100);
        break;
    case 0x089228B0u:
    case 0x08922938u:
    case 0x089228E8u: {
        fill(kScratch, 0x800);
        g_entity_pool_ptr = kScratch;
        guest<EntityPool>(kScratch)->count = static_cast<s16>(rng() % 12u) - 2;
        for (u32 i = 0; i < 10; ++i)
            if (rng() & 1u) guest<Entity>(kScratch + sizeof(EntityPool) + i * sizeof(Entity))->flags = 0;
        c.gpr[4] = static_cast<u32>(static_cast<s32>(rng() % 10u));
        break;
    }
    case 0x0889E76Cu: {
        // A random but well-formed map: 16 nodes (kScratch), 128 edges (kScratch + 0x1000), the real stick tables.
        constexpr u32 kNodes = kScratch, kEdges = kScratch + 0x1000;
        fill(kNodes, 16 * 16);
        fill(kEdges, 128 * 8);
        for (u32 n = 0; n < 16; ++n)
            for (u32 k = 0; k < 4; ++k) {
                const u32 r = rng() % 6u;
                guest<MapNode>(kNodes + n * 16)->edges[k] = r == 0 ? 0xFF : static_cast<u8>((rng() % 128u) | ((rng() & 1u) << 7));
            }
        for (u32 e = 0; e < 128; ++e) {
            guest<MapEdge>(kEdges + e * 8)->length = static_cast<u16>(rng() % 100u);
            guest<MapEdge>(kEdges + e * 8)->angle = (rng() % 20u == 0u) ? static_cast<s16>(rng()) : static_cast<s16>(rng() % 4096u);
        }
        g_map_nodes_ptr = kNodes;
        g_map_edges_ptr = kEdges;
        const u8 dirs[8] = {0x10, 0x20, 0x40, 0x80, 0x30, 0x60, 0xC0, 0x90};
        const u16 windows[8][4] = {{256, 1024, 1024, 1792}, {3328, 4096, 0, 768}, {2304, 3072, 3072, 3840}, {1280, 2048, 2048, 2816},
                                   {0, 512, 512, 1024}, {3072, 3584, 3584, 4096}, {2048, 2560, 2560, 3072}, {1024, 1536, 1536, 2048}};
        for (int i = 0; i < 8; ++i) {
            g_stick_dirs.get()[i] = dirs[i];
            for (int w = 0; w < 4; ++w) g_dir_windows.get()[i].w[w] = windows[i][w];
        }
        for (int i = 0; i < 4; ++i) g_dir_history.get()[i] = static_cast<u8>(rng() % 8u);
        MapCursor &cursor = g_map_cursor;
        cursor.node = (rng() % 6u == 0u) ? 0xFF : static_cast<u8>(rng() % 16u);
        cursor.edge = static_cast<u8>(rng());
        cursor.progress = rng();
        g_map_cursor_idle = static_cast<u16>(rng());
        c.gpr[4] = (rng() % 5u == 0u) ? rng() : (rng() % 3u == 0u ? (dirs[rng() % 8u] | dirs[rng() % 8u]) : dirs[rng() % 8u]);
        break;
    }
    case 0x088523F0u:
    case 0x0885244Cu:
        c.gpr[4] &= 0x7Fu;
        fill(0x08CCC668u, 0x400);
        break;
    case 0x088EB73Cu:
        g_word_80e4 = rng();
        break;
    case 0x08864A24u:
        fill(kScratch, 0x10);
        c.gpr[4] = kScratch;
        g_view_point = Point16{static_cast<u16>(rng()), static_cast<u16>(rng())};
        break;
    default: break;
    }
}

// P2_MAPNAV=<node>[,<save-slot>]: town-map navigator. Reads the node graph, plans a shortest path from the
// cursor to node <node> and presses the stick direction that makes the game pick the next edge at every
// junction; holds it while the cursor walks the (straight) edge. When it arrives it logs the node and,
// with a slot, requests a savestate. Used to reach a place node (type 04) on a town map unattended.
u32 mapnav_buttons() {
    using namespace game;
    static const char *env = std::getenv("P2_MAPNAV");
    if (env == nullptr) return 0u;
    static int target = -1, save_slot = -1;
    static bool done = false;
    static int held = -1;       // stick index being held
    static int walking_to = -1; // node we are heading for on the current edge
    static u32 last_ix = 0, last_iy = 0;
    static u64 last_move = 0;
    static int stall_cycle = 0;
    if (target < 0) {
        char *end = nullptr;
        target = static_cast<int>(std::strtol(env, &end, 10));
        if (end != nullptr && *end == ',') save_slot = std::atoi(end + 1);
        LOG_I("mapnav", "target node %d, save slot %d", target, save_slot);
    }
    if (done || g_map_nodes_ptr == 0u || g_map_edges_ptr == 0u) return 0u;
    const u64 now = g_kernel.vblank_count();

    // Number of valid nodes: stop at the first with an impossible position or type.
    int count = 0;
    auto node_at = [](int i) { return guest<MapNode>(g_map_nodes_ptr + static_cast<u32>(i) * sizeof(MapNode)); };
    for (; count < 90; ++count) {
        const MapNode *n = node_at(count);
        const u8 t = n->type;
        const bool known = t == 0x00 || t == 0x01 || t == 0x04 || t == 0x10 || t == 0x20 || t == 0x40;
        if (!known || n->x > 400 || n->y > 300) break;
    }
    if (target >= count) {
        LOG_E("mapnav", "target %d outside the %d nodes", target, count);
        done = true;
        return 0u;
    }

    const MapCursor &cur = g_map_cursor;
    auto other_end = [&](u8 id, int from) -> int {
        const MapEdge *e = guest<MapEdge>(g_map_edges_ptr + static_cast<u32>(id & 0x7F) * sizeof(MapEdge));
        return (id & 0x80) ? e->node_a : (e->node_b == from ? e->node_a : e->node_b);
    };
    // Distance (in edges) from every node to the target.
    std::array<int, 90> dist;
    dist.fill(1000);
    dist[static_cast<std::size_t>(target)] = 0;
    for (int pass = 0; pass < count; ++pass)
        for (int i = 0; i < count; ++i)
            for (int k = 0; k < 4; ++k) {
                const u8 id = node_at(i)->edges[k];
                if (id == 0xFF) continue;
                const int j = other_end(id, i);
                if (j < count && dist[static_cast<std::size_t>(j)] + 1 < dist[static_cast<std::size_t>(i)])
                    dist[static_cast<std::size_t>(i)] = dist[static_cast<std::size_t>(j)] + 1;
            }

    static constexpr s32 kStickHeading[8] = {1024, 0, 3072, 2048, 512, 3584, 2560, 1536};
    auto stick_toward = [&](s32 angle) {
        int best = 0;
        s32 best_d = 1 << 20;
        for (int i = 0; i < 8; ++i) {
            const s32 d = std::abs(((angle - kStickHeading[i]) % 4096 + 4096 + 2048) % 4096 - 2048);
            if (d < best_d) { best_d = d; best = i; }
        }
        return best;
    };
    const auto &dirs = g_stick_dirs.get();

    if (cur.ix != static_cast<s32>(last_ix) || cur.iy != static_cast<s32>(last_iy)) {
        last_ix = static_cast<u32>(cur.ix);
        last_iy = static_cast<u32>(cur.iy);
        last_move = now;
    }

    // Where are we? On a node (cursor.node), or at an end of the edge being walked (the game keeps node = 0xFF
    // there when the node has just two edges and the walk simply continues), or in the middle of an edge.
    int here = cur.node != 0xFF ? cur.node : -1;
    const MapEdge *edge = cur.node != 0xFF ? nullptr : guest<MapEdge>(g_map_edges_ptr + static_cast<u32>(cur.edge) * sizeof(MapEdge));
    if (here < 0) {
        if (cur.progress == 0u) here = edge->node_b;
        else if (cur.progress >= (static_cast<u32>(edge->length) << 12)) here = edge->node_a;
    }
    static u64 pause_until = 0;
    if (now < pause_until) return 0u;
    if (now - last_move > 30u) { // not moving although the stick is held: let go for a few frames, then try another direction
        pause_until = now + 10u;
        last_move = pause_until;
        ++stall_cycle;
        LOG_I("mapnav", "stalled at (%d,%d) node %d, cycle %d", cur.ix, cur.iy, here, stall_cycle);
        return 0u;
    }
    static u64 last_log = 0;
    auto log_state = [&](const char *what) {
        if (now - last_log < 60u) return;
        last_log = now;
        LOG_I("mapnav", "%s at (%d,%d) node %d, stick %d, progress %u", what, cur.ix, cur.iy, here, held, static_cast<unsigned>(cur.progress));
    };

    if (here >= 0) {
        if (here == target && cur.node != target && edge != nullptr) {
            // at the end of the edge but the game has not put the cursor on the node yet: keep pressing along the edge a little longer
            static u64 settle_since = 0;
            if (settle_since == 0) settle_since = now;
            if (now - settle_since < 90u) {
                const s32 into = (cur.progress == 0u) ? edge->angle : edge->angle + 2048;
                return dirs[static_cast<std::size_t>(stick_toward(into))];
            }
        }
        if (here == target) {
            LOG_I("mapnav", "arrived at node %d (%d,%d) vblank %llu%s", here, cur.ix, cur.iy, static_cast<unsigned long long>(now),
                  cur.node == target ? "" : " (cursor not on the node)");
            done = true;
            if (save_slot >= 0) savestate_request_save(save_slot);
            return 0u;
        }
        const MapNode &node = *node_at(here);
        int want_slot = -1, best = 1000;
        for (int k = 0; k < 4; ++k) {
            const u8 id = node.edges[k];
            if (id == 0xFF) continue;
            const int j = other_end(id, here);
            if (j < count && dist[static_cast<std::size_t>(j)] < best) {
                best = dist[static_cast<std::size_t>(j)];
                want_slot = k;
                walking_to = j;
            }
        }
        if (want_slot < 0) {
            LOG_E("mapnav", "no route from node %d", here);
            done = true;
            return 0u;
        }
        // Which stick direction makes the game take that slot? Rotate the start of the search while stalled.
        held = -1;
        for (int n = 0; n < 8; ++n) {
            const int index = (n + stall_cycle) % 8;
            if (pick_slot(node, index) == want_slot) { held = index; break; }
        }
        if (held < 0) held = 0;
        log_state("at node");
        return dirs[static_cast<std::size_t>(held)];
    }
    // In the middle of an edge: walk to the end nearer the target.
    walking_to = dist[edge->node_a] <= dist[edge->node_b] ? edge->node_a : edge->node_b;
    const s32 tangent = (walking_to == edge->node_b) ? edge->angle : edge->angle + 2048;
    held = stick_toward(tangent);
    log_state("on edge");
    return dirs[static_cast<std::size_t>(held)];
}

} // namespace p2

P2_NATIVE(08807918, p2::game::pad_held)
P2_NATIVE(0880792C, p2::game::pad_pressed)
P2_NATIVE(08807968, p2::game::pad_mapped_b)
P2_NATIVE(08807AC4, p2::game::pad_mapped_b)
P2_NATIVE(088079AC, p2::game::pad_dir_a)
P2_NATIVE(08807B08, p2::game::pad_dir_b)
P2_NATIVE(08807C58, p2::game::get_map_flag)
P2_NATIVE(08851C30, p2::game::game_rand)
P2_NATIVE(08863DF4, p2::game::vec3_mul_mat3)
P2_NATIVE(088FE140, p2::game::pool_count_flagged)
P2_NATIVE_VOID(088FDF74, p2::game::pool_init)
P2_NATIVE(08866818, p2::game::sin_fixed)
P2_NATIVE(08866850, p2::game::cos_fixed)
P2_NATIVE(08963E4C, p2::game::get_pool_ptr)
P2_NATIVE(088A0508, p2::game::get_map_slot)
P2_NATIVE_VOID(089228B0, p2::game::entity_set_flags6)
P2_NATIVE(08922938, p2::game::entity_active)
P2_NATIVE(089228E8, p2::game::entity_any_active)
P2_NATIVE(088523F0, p2::game::table_c868_get)
P2_NATIVE(0885244C, p2::game::table_c668_get)
P2_NATIVE(088EB73C, p2::game::get_word_80e4)
P2_NATIVE(088EB8DC, p2::game::get_object_60e4)
P2_NATIVE_VOID(08864A24, p2::game::get_view_point)
P2_NATIVE_VOID(0889E76C, p2::game::map_cursor_pick_edge)

// 08962BE8: the field random-encounter check (called with the distance walked this frame; it accumulates it, rolls
// once per 512 units and returns the encounter group or 0). With P2_NO_ENCOUNTERS set it never starts an
// encounter (used by the mapping bot); otherwise the original code runs unchanged.
namespace psprecomp {
void p2_native_08962BE8(Runtime &r, AllegrexContext &c) {
    static const bool off = std::getenv("P2_NO_ENCOUNTERS") != nullptr;
    if (off) {
        c.gpr[2] = 0;
        return;
    }
    p2::call_original(r, c, 0x08962BE8u);
}
} // namespace psprecomp
