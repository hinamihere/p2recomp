// sceGe_user: display lists are executed synchronously when enqueued or when
// the stall address advances. Draw commands go to the software rasterizer.
#include "ge.hpp"

#include "gpu_state.hpp"
#include "kernel.hpp"

#include <array>
#include <map>

namespace p2 {
namespace {

Kernel &K = g_kernel;
gpu::State g_state;

struct GeCallbackData {
    u32 signal_func{}, signal_arg{}, finish_func{}, finish_arg{};
    bool used{};
};
std::array<GeCallbackData, 16> g_callbacks;

enum class ListState { Queued, Running, Stalled, Completed, Paused };

struct DisplayList {
    s32 id{};
    u32 start{};
    u32 pc{};
    u32 stall{};
    s32 cbid{-1};
    ListState state{ListState::Queued};
    struct Frame { u32 pc, offset_addr, base; };
    std::array<Frame, 32> stack{};
    u32 depth{};
    u32 last_cmd{};
    u32 signal_data{};
    u64 commands{};
};

std::map<s32, DisplayList> g_lists;
s32 g_next_list = 1;
bool g_processing = false;

void queue_ge_interrupt(const DisplayList &dl, bool signal, u32 token) {
    if (dl.cbid < 0 || dl.cbid >= 16 || !g_callbacks[dl.cbid].used) return;
    const auto &cb = g_callbacks[dl.cbid];
    const u32 func = signal ? cb.signal_func : cb.finish_func;
    if (func == 0u) return;
    GuestCall call;
    call.entry = func;
    call.args[0] = token & 0xFFFFu;
    call.args[1] = signal ? cb.signal_arg : cb.finish_arg;
    call.args[2] = dl.pc + 4u;
    call.what = signal ? "ge-signal" : "ge-finish";
    K.queue_interrupt(std::move(call));
}

void set_matrix_data(float *matrix, u32 &index, u32 limit, u32 data) {
    if (index < limit) matrix[index] = gpu::float24(data);
    ++index;
}

// Executes `dl` until it stalls, completes or pauses.
void run_list(DisplayList &dl) {
    auto &m = mem();
    gpu::State &s = g_state;
    dl.state = ListState::Running;
    for (;;) {
        if (dl.stall != 0u && dl.pc == dl.stall) {
            dl.state = ListState::Stalled;
            return;
        }
        const u32 op = m.load32(dl.pc);
        const u32 cmd = op >> 24;
        const u32 data = op & 0x00FFFFFFu;
        ++dl.commands;
        u32 next_pc = dl.pc + 4u;
        switch (cmd) {
        case gpu::NOP: break;
        case gpu::VADDR: s.vertex_addr = s.relative(data); break;
        case gpu::IADDR: s.index_addr = s.relative(data); break;
        case gpu::PRIM:
            s.reg[cmd] = data;
            gpu::draw_prim(s, (data >> 16) & 7u, data & 0xFFFFu);
            break;
        case gpu::BEZIER:
        case gpu::SPLINE:
            s.reg[cmd] = data;
            gpu::draw_patch(s, cmd == gpu::SPLINE, data);
            break;
        case gpu::BOUNDINGBOX: s.reg[cmd] = data; break;
        case gpu::JUMP: next_pc = s.relative(data & 0x00FFFFFCu); break;
        case gpu::BJUMP: break; // bounding box test always passes: never take the skip
        case gpu::CALL:
            if (dl.depth < dl.stack.size()) {
                dl.stack[dl.depth++] = {dl.pc + 4u, s.offset_addr, s.base};
                next_pc = s.relative(data & 0x00FFFFFCu);
            } else {
                LOG_E("ge", "list %d call stack overflow", dl.id);
            }
            break;
        case gpu::RET:
            if (dl.depth > 0u) {
                const auto &f = dl.stack[--dl.depth];
                next_pc = f.pc;
                s.offset_addr = f.offset_addr;
                s.base = f.base;
            }
            break;
        case gpu::SIGNAL:
            dl.signal_data = data;
            break;
        case gpu::FINISH:
            dl.signal_data = data;
            break;
        case gpu::END: {
            const u32 prev = dl.last_cmd;
            if (prev == gpu::FINISH) {
                gpu::flush(s);
                dl.state = ListState::Completed;
                dl.pc = next_pc;
                queue_ge_interrupt(dl, false, dl.signal_data);
                return;
            }
            if (prev == gpu::SIGNAL) {
                const u32 behaviour = (dl.signal_data >> 16) & 0xFFu;
                const u32 target = ((dl.signal_data & 0xFFFFu) << 16) | (data & 0xFFFFu);
                switch (behaviour) {
                case 0x01: case 0x02: case 0x03: // handler suspend/continue/pause
                    queue_ge_interrupt(dl, true, dl.signal_data);
                    break;
                case 0x10: next_pc = s.relative(target & 0x00FFFFFCu); break; // jump
                case 0x11:
                    if (dl.depth < dl.stack.size()) {
                        dl.stack[dl.depth++] = {dl.pc + 4u, s.offset_addr, s.base};
                        next_pc = s.relative(target & 0x00FFFFFCu);
                    }
                    break;
                case 0x12:
                    if (dl.depth > 0u) {
                        const auto &f = dl.stack[--dl.depth];
                        next_pc = f.pc;
                        s.offset_addr = f.offset_addr;
                        s.base = f.base;
                    }
                    break;
                default:
                    LOG_D("ge", "list %d signal behaviour %x", dl.id, behaviour);
                    break;
                }
                break;
            }
            // A bare END stops the list.
            gpu::flush(s);
            dl.state = ListState::Completed;
            dl.pc = next_pc;
            return;
        }
        case gpu::BASE: s.base = op; break;
        case gpu::OFFSETADDR: s.offset_addr = data << 8; break;
        case gpu::ORIGIN: s.offset_addr = dl.pc; break;
        case gpu::BONEMATRIXNUMBER: s.bone_index = data & 0x7Fu; break;
        case gpu::BONEMATRIXDATA: set_matrix_data(s.bone, s.bone_index, 96u, data); break;
        case gpu::WORLDMATRIXNUMBER: s.world_index = data & 0xFu; break;
        case gpu::WORLDMATRIXDATA: set_matrix_data(s.world, s.world_index, 12u, data); break;
        case gpu::VIEWMATRIXNUMBER: s.view_index = data & 0xFu; break;
        case gpu::VIEWMATRIXDATA: set_matrix_data(s.view, s.view_index, 12u, data); break;
        case gpu::PROJMATRIXNUMBER: s.proj_index = data & 0xFu; break;
        case gpu::PROJMATRIXDATA: set_matrix_data(s.proj, s.proj_index, 16u, data); break;
        case gpu::TGENMATRIXNUMBER: s.tgen_index = data & 0xFu; break;
        case gpu::TGENMATRIXDATA: set_matrix_data(s.tgen, s.tgen_index, 12u, data); break;
        case gpu::LOADCLUT:
            s.reg[cmd] = data;
            gpu::load_clut(s);
            break;
        case gpu::TRANSFERSTART:
            s.reg[cmd] = data;
            gpu::block_transfer(s, data);
            break;
        case gpu::FRAMEBUFPTR:
        case gpu::FRAMEBUFWIDTH:
        case gpu::FRAMEBUFPIXFORMAT:
            if (s.reg[cmd] != data) gpu::flush(s);
            s.reg[cmd] = data;
            break;
        default:
            if (cmd >= gpu::MORPHWEIGHT0 && cmd < gpu::MORPHWEIGHT0 + 8u) s.morph[cmd - gpu::MORPHWEIGHT0] = gpu::float24(data);
            s.reg[cmd] = data;
            break;
        }
        dl.last_cmd = cmd;
        dl.pc = next_pc;
    }
}

void process_lists() {
    if (g_processing) return;
    g_processing = true;
    const u64 start = Kernel::now_us();
    for (auto &[id, dl] : g_lists) {
        if (dl.state == ListState::Queued || dl.state == ListState::Running) run_list(dl);
        if (dl.state == ListState::Stalled) break; // later lists wait behind a stalled one
    }
    g_kernel.perf_gpu_us += Kernel::now_us() - start;
    g_processing = false;
}

s32 enqueue(AllegrexContext &c, bool head) {
    const u32 list = arg(c, 0), stall = arg(c, 1);
    const s32 cbid = static_cast<s32>(arg(c, 2));
    // Drop finished lists, keeping the id space small.
    for (auto it = g_lists.begin(); it != g_lists.end();)
        it = it->second.state == ListState::Completed ? g_lists.erase(it) : std::next(it);
    DisplayList dl;
    dl.id = g_next_list++;
    if (g_next_list > 0x7FFF) g_next_list = 1;
    dl.start = dl.pc = list & 0x0FFFFFFFu;
    dl.stall = stall & 0x0FFFFFFFu;
    dl.cbid = cbid;
    LOG_T("ge", "enqueue%s list=%08x stall=%08x cb=%d -> %d", head ? "head" : "", list, stall, cbid, dl.id);
    g_lists[dl.id] = dl;
    (void)head;
    process_lists();
    return dl.id;
}

void GeListEnQueue(AllegrexContext &c) { ret(c, static_cast<u32>(enqueue(c, false))); }
void GeListEnQueueHead(AllegrexContext &c) { ret(c, static_cast<u32>(enqueue(c, true))); }

void GeListUpdateStallAddr(AllegrexContext &c) {
    auto it = g_lists.find(static_cast<s32>(arg(c, 0)));
    if (it == g_lists.end()) { ret(c, 0x80000100u); return; }
    it->second.stall = arg(c, 1) & 0x0FFFFFFFu;
    if (it->second.state == ListState::Stalled) it->second.state = ListState::Running;
    process_lists();
    ret(c, 0u);
}

u32 list_status(const DisplayList &dl) {
    switch (dl.state) {
    case ListState::Completed: return 0u;
    case ListState::Queued: return 1u;
    case ListState::Running: return 2u;
    case ListState::Stalled: return 3u;
    case ListState::Paused: return 4u;
    }
    return 0u;
}

void GeListSync(AllegrexContext &c) {
    auto it = g_lists.find(static_cast<s32>(arg(c, 0)));
    if (it == g_lists.end()) { ret(c, 0u); return; }
    ret(c, arg(c, 1) == 1u ? list_status(it->second) : 0u);
}

void GeDrawSync(AllegrexContext &c) {
    if (arg(c, 0) == 1u) {
        for (const auto &[id, dl] : g_lists) {
            if (dl.state != ListState::Completed) { ret(c, list_status(dl) == 3u ? 3u : 2u); return; }
        }
    }
    ret(c, 0u);
}

void GeSetCallback(AllegrexContext &c) {
    const u32 p = arg(c, 0);
    for (s32 i = 0; i < 16; ++i) {
        if (g_callbacks[i].used) continue;
        auto &m = mem();
        g_callbacks[i] = {m.load32(p), m.load32(p + 4u), m.load32(p + 8u), m.load32(p + 12u), true};
        LOG_I("ge", "set callback %d signal=%08x finish=%08x", i, g_callbacks[i].signal_func, g_callbacks[i].finish_func);
        ret(c, static_cast<u32>(i));
        return;
    }
    ret(c, 0x80000022u);
}

void GeUnsetCallback(AllegrexContext &c) {
    const u32 id = arg(c, 0);
    if (id < 16u) g_callbacks[id].used = false;
    ret(c, 0u);
}

void GeBreak(AllegrexContext &c) { ret(c, 0u); }
void GeContinue(AllegrexContext &c) { process_lists(); ret(c, 0u); }
void GeEdramGetAddr(AllegrexContext &c) { ret(c, 0x04000000u); }

} // namespace

void register_ge_imports() {
    const char *G = "sceGe_user";
    hle(G, "sceGeListEnQueue", GeListEnQueue);
    hle(G, "sceGeListEnQueueHead", GeListEnQueueHead);
    hle(G, "sceGeListUpdateStallAddr", GeListUpdateStallAddr);
    hle(G, "sceGeListSync", GeListSync);
    hle(G, "sceGeDrawSync", GeDrawSync);
    hle(G, "sceGeSetCallback", GeSetCallback);
    hle(G, "sceGeUnsetCallback", GeUnsetCallback);
    hle(G, "sceGeBreak", GeBreak);
    hle(G, "sceGeContinue", GeContinue);
    hle(G, "sceGeEdramGetAddr", GeEdramGetAddr);
}

} // namespace p2
