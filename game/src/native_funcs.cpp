// Hand-written replacements for guest functions (see native.hpp). The list of replaced entries lives in
// game/overrides.txt; each P2_NATIVE line at the bottom binds a C++ function to one of them. Names are
// guesses from reading the code: the game ships without symbols.
#include "native.hpp"

#include <array>

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
    const u8 chosen = node.edges[slot];
    if (!inside || chosen == 0xFF) return;

    // Leave the node: progress counts from node_b, so an edge listed without the flag starts at full length.
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
