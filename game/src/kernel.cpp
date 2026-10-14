#include "kernel.hpp"

#include "gpu_state.hpp"
#include "savestate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace p2 {

Kernel g_kernel;

namespace {
constexpr u64 kPreemptedSeqBase = 1ull << 62;
u64 g_front_seq = kPreemptedSeqBase;
u64 g_back_seq = kPreemptedSeqBase;

u32 align_up(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }
} // namespace

const char *wait_type_name(WaitType type) {
    switch (type) {
    case WaitType::None: return "none";
    case WaitType::Delay: return "delay";
    case WaitType::Sema: return "sema";
    case WaitType::EventFlag: return "eventflag";
    case WaitType::ThreadEnd: return "threadend";
    case WaitType::LwMutex: return "lwmutex";
    case WaitType::Vblank: return "vblank";
    case WaitType::Ctrl: return "ctrl";
    case WaitType::Audio: return "audio";
    case WaitType::AsyncIo: return "asyncio";
    case WaitType::GeList: return "gelist";
    case WaitType::GeDraw: return "gedraw";
    case WaitType::Umd: return "umd";
    case WaitType::Utility: return "utility";
    }
    return "?";
}

u64 Kernel::time_offset_us_ = 0;
u64 Kernel::turbo_until_ = 0;
bool Kernel::turbo_key_ = false;

u64 Kernel::now_us() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<u64>(duration_cast<microseconds>(steady_clock::now() - start).count()) + 1'000'000u + time_offset_us_;
}

bool Kernel::turbo() const { return turbo_key_ || vcount_ < turbo_until_; }

void Kernel::skip_time_to(u64 target) {
    const u64 now = now_us();
    if (target > now) time_offset_us_ += target - now;
}

void Kernel::init(u32 module_gp, u32 module_end) {
    module_gp_ = module_gp;
    boot_us_ = now_us();
    next_vblank_us_ = boot_us_ + kVblankPeriodUs;
    free_.clear();
    free_.push_back({align_up(module_end, 0x100u), kUserMemoryEnd});
}

// --- memory ------------------------------------------------------------------

s32 Kernel::alloc_block(const std::string &name, int partition, int type, u32 size, u32 addr, u32 alignment) {
    if (size == 0u) return static_cast<s32>(err::ILLEGAL_MEMSIZE);
    if (partition != 2 && partition != 6 && partition != 5)
        return static_cast<s32>(err::ILLEGAL_PARTITION);
    size = align_up(size, 0x100u);
    if (type == 3 || type == 4) {
        alignment = addr;
        if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) return static_cast<s32>(err::ILLEGAL_ARGUMENT);
    }
    alignment = std::max<u32>(alignment, 0x100u);

    u32 chosen = 0u;
    bool found = false;
    if (type == 2) {
        const u32 want = addr & ~0xFFu;
        for (const auto &r : free_) {
            if (want >= r.start && want + size <= r.end) { chosen = want; found = true; break; }
        }
    } else if (type == 1 || type == 4) {
        for (auto it = free_.rbegin(); it != free_.rend(); ++it) {
            if (it->end - it->start < size) continue;
            const u32 candidate = (it->end - size) & ~(alignment - 1u);
            if (candidate >= it->start) { chosen = candidate; found = true; break; }
        }
    } else {
        for (const auto &r : free_) {
            const u32 candidate = align_up(r.start, alignment);
            if (candidate >= r.start && candidate + size <= r.end && candidate + size > candidate) {
                chosen = candidate; found = true; break;
            }
        }
    }
    if (!found) {
        LOG_W("mem", "alloc '%s' size=%08x type=%d failed (max free %08x)", name.c_str(), size, type, max_free());
        return static_cast<s32>(err::NO_MEMORY);
    }
    // Carve [chosen, chosen+size) out of its free range.
    for (std::size_t i = 0; i < free_.size(); ++i) {
        FreeRange r = free_[i];
        if (chosen < r.start || chosen + size > r.end) continue;
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        if (chosen + size < r.end) free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(i), FreeRange{chosen + size, r.end});
        if (r.start < chosen) free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(i), FreeRange{r.start, chosen});
        break;
    }
    const s32 uid = new_uid();
    blocks_[uid] = MemoryBlock{uid, name, chosen, size, partition};
    LOG_D("mem", "alloc '%s' -> uid=%x addr=%08x size=%08x", name.c_str(), uid, chosen, size);
    return uid;
}

u32 Kernel::free_block(s32 uid) {
    auto it = blocks_.find(uid);
    if (it == blocks_.end()) return err::UNKNOWN_UID;
    FreeRange r{it->second.address, it->second.address + it->second.size};
    blocks_.erase(it);
    auto pos = std::lower_bound(free_.begin(), free_.end(), r, [](const FreeRange &a, const FreeRange &b) { return a.start < b.start; });
    pos = free_.insert(pos, r);
    // Merge neighbours.
    std::size_t i = static_cast<std::size_t>(pos - free_.begin());
    if (i + 1 < free_.size() && free_[i].end == free_[i + 1].start) {
        free_[i].end = free_[i + 1].end;
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i + 1));
    }
    if (i > 0 && free_[i - 1].end == free_[i].start) {
        free_[i - 1].end = free_[i].end;
        free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return 0u;
}

MemoryBlock *Kernel::block(s32 uid) {
    auto it = blocks_.find(uid);
    return it != blocks_.end() ? &it->second : nullptr;
}

u32 Kernel::max_free() const {
    u32 best = 0u;
    for (const auto &r : free_) best = std::max(best, r.end - r.start);
    return best;
}

u32 Kernel::total_free() const {
    u32 total = 0u;
    for (const auto &r : free_) total += r.end - r.start;
    return total;
}

// --- threads -----------------------------------------------------------------

Thread *Kernel::thread(s32 uid) {
    if (uid == 0) return current_;
    auto it = threads_.find(uid);
    if (it == threads_.end() || it->second->state == ThreadState::Dead) return nullptr;
    return it->second.get();
}

void Kernel::set_identity() {
    if (interrupt_depth_ > 0) return; // identity is managed by start_interrupt/nested_return
    if (idle_ || current_ == nullptr) psprecomp::set_runtime_thread_identity(0, "idle");
    else psprecomp::set_runtime_thread_identity(current_->uid, current_->name);
}

s32 Kernel::create_thread(const std::string &name, u32 entry, s32 priority, u32 stack_size, u32 attr) {
    if (priority < 0x08 || priority > 0x77) {
        LOG_W("thread", "create '%s': clamping priority %x", name.c_str(), priority);
        priority = std::clamp(priority, 0x08, 0x77);
    }
    if (stack_size < 0x200u) return static_cast<s32>(err::ILLEGAL_STACK_SIZE);
    stack_size = align_up(stack_size, 0x100u);
    const s32 block = alloc_block("stack:" + name, 2, 1, stack_size, 0u);
    if (block < 0) return block;
    auto t = std::make_unique<Thread>();
    t->uid = new_uid();
    t->name = name;
    t->entry = entry;
    t->attr = attr;
    t->gp = module_gp_;
    t->init_priority = priority;
    t->priority = priority;
    t->stack_block = static_cast<u32>(block);
    t->stack_bottom = blocks_[block].address;
    t->stack_size = stack_size;
    t->state = ThreadState::Dormant;
    const s32 uid = t->uid;
    LOG_I("thread", "create '%s' uid=%x entry=%08x prio=%x stack=%08x+%x attr=%x", name.c_str(), uid, entry,
          priority, t->stack_bottom, stack_size, attr);
    threads_[uid] = std::move(t);
    return uid;
}

namespace {
void reset_context(Thread &t) {
    t.ctx = AllegrexContext{};
    t.ctx.vfpu_ctrl[0] = 0xE4u;
    t.ctx.vfpu_ctrl[1] = 0xE4u;
    t.ctx.vfpu_ctrl[2] = 0u;
    t.ctx.vfpu_ctrl[3] = 0x3Fu;
    t.ctx.vfpu_ctrl[8] = 0x3F800001u; // VRNG seeds, as on hardware
    t.ctx.vfpu_ctrl[9] = 0x3F800002u;
    t.ctx.vfpu_ctrl[10] = 0x3F800003u;
    t.ctx.vfpu_ctrl[11] = 0x3F800004u;
    t.ctx.vfpu_ctrl[12] = 0x3F800005u;
    t.ctx.vfpu_ctrl[13] = 0x3F800006u;
    t.ctx.vfpu_ctrl[14] = 0x3F800007u;
    t.ctx.vfpu_ctrl[15] = 0x3F800008u;
}
} // namespace

static void setup_thread_stack(Thread &t, const u8 *args, u32 arg_len) {
    auto &m = mem();
    reset_context(t);
    if ((t.attr & 0x00100000u) == 0u) {
        for (u32 off = 0; off < t.stack_size; off += 4u) m.store32(t.stack_bottom + off, 0xFFFFFFFFu);
    }
    u32 sp = t.stack_bottom + t.stack_size;
    sp -= 256u;
    const u32 k0 = sp;
    for (u32 off = 0; off < 0x100u; off += 4u) m.store32(k0 + off, 0u);
    m.store32(k0 + 0xc0u, static_cast<u32>(t.uid));
    m.store32(k0 + 0xc8u, t.stack_bottom);
    m.store32(k0 + 0xf8u, 0xFFFFFFFFu);
    m.store32(k0 + 0xfcu, 0xFFFFFFFFu);
    m.store32(t.stack_bottom, static_cast<u32>(t.uid));
    t.ctx.gpr[26] = k0;
    if (args != nullptr && arg_len > 0u) {
        sp -= (arg_len + 0xFu) & ~0xFu;
        m.copy_in(sp, std::span<const u8>(args, arg_len));
        t.ctx.gpr[4] = arg_len;
        t.ctx.gpr[5] = sp;
    }
    sp -= 64u;
    t.ctx.gpr[29] = sp;
    t.ctx.gpr[30] = sp;
    t.ctx.gpr[31] = kThreadReturnAddr;
    t.ctx.gpr[28] = t.gp;
    t.ctx.pc = t.entry;
}

u32 Kernel::start_thread(AllegrexContext &c, s32 uid, u32 arg_len, u32 arg_ptr) {
    Thread *t = thread(uid);
    if (t == nullptr) return err::UNKNOWN_THID;
    if (t->state != ThreadState::Dormant) return err::NOT_DORMANT;
    std::vector<u8> args;
    if (arg_ptr != 0u && arg_len > 0u && valid_range(arg_ptr, arg_len)) {
        args.resize(arg_len);
        mem().copy_out(arg_ptr, args);
    }
    t->priority = t->init_priority;
    setup_thread_stack(*t, args.empty() ? nullptr : args.data(), static_cast<u32>(args.size()));
    LOG_I("thread", "start '%s' uid=%x args=%u@%08x", t->name.c_str(), uid, arg_len, arg_ptr);
    make_ready(*t);
    ret(c, 0u);
    reschedule(c);
    return 0u;
}

void Kernel::boot_main_thread(u32 entry, u32 priority, u32 stack_size, u32 attr, const std::string &argument) {
    const s32 uid = create_thread("root", entry, static_cast<s32>(priority), stack_size, attr);
    Thread *t = thread(uid);
    std::vector<u8> args(argument.begin(), argument.end());
    args.push_back(0u);
    setup_thread_stack(*t, args.data(), static_cast<u32>(args.size()));
    make_ready(*t);
    AllegrexContext &c = rt().cpu();
    idle_ = true;
    current_ = nullptr;
    switch_to(c, best_ready(), false);
}

void Kernel::make_ready(Thread &t) {
    t.state = ThreadState::Ready;
    t.ready_seq = ++g_back_seq;
}

Thread *Kernel::best_ready() {
    Thread *best = nullptr;
    for (auto &[uid, t] : threads_) {
        if (t->state != ThreadState::Ready) continue;
        if (best == nullptr || t->priority < best->priority ||
            (t->priority == best->priority && t->ready_seq < best->ready_seq))
            best = t.get();
    }
    return best;
}

void Kernel::save_current(AllegrexContext &c) {
    if (idle_ || current_ == nullptr) return;
    current_->ctx = c;
    if (in_hle_call) current_->ctx.pc = c.gpr[31];
}

void Kernel::switch_to(AllegrexContext &c, Thread *next, bool save_old) {
    if (!idle_ && next == current_ && current_ != nullptr) {
        current_->state = ThreadState::Running;
        return;
    }
    if (save_old) save_current(c);
    if (next != nullptr) {
        LOG_T("sched", "switch -> '%s' uid=%x pc=%08x", next->name.c_str(), next->uid, next->ctx.pc);
        c = next->ctx;
        current_ = next;
        next->state = ThreadState::Running;
        idle_ = false;
    } else {
        LOG_T("sched", "switch -> idle");
        AllegrexContext idle_ctx{};
        idle_ctx.pc = kIdleAddr;
        idle_ctx.gpr[29] = kInterruptStackTop - 0x400u;
        c = idle_ctx;
        current_ = nullptr;
        idle_ = true;
    }
    set_identity();
}

void Kernel::reschedule(AllegrexContext &c) {
    if (interrupt_depth_ > 0) return;
    Thread *best = best_ready();
    if (idle_ || current_ == nullptr) {
        if (best != nullptr) switch_to(c, best, false);
        return;
    }
    if (current_->state != ThreadState::Running) {
        // The current thread stopped running without switching; pick another.
        switch_to(c, best, current_->state == ThreadState::Ready);
        return;
    }
    if (best != nullptr && best->priority < current_->priority) {
        Thread *old = current_;
        save_current(c);
        old->state = ThreadState::Ready;
        old->ready_seq = --g_front_seq; // preempted threads keep their turn
        switch_to(c, best, false);
    }
}

void Kernel::yield_current(AllegrexContext &c) {
    if (current_ == nullptr || idle_ || interrupt_depth_ > 0) return;
    Thread *old = current_;
    save_current(c);
    make_ready(*old);
    switch_to(c, best_ready(), false);
}

void Kernel::change_priority(AllegrexContext &c, s32 uid, s32 priority) {
    Thread *t = thread(uid);
    if (t == nullptr) return;
    t->priority = priority;
    if (t->state == ThreadState::Ready) t->ready_seq = ++g_back_seq;
    reschedule(c);
}

void Kernel::wait_current(AllegrexContext &c, WaitType type, s32 id, u64 deadline, u32 timeout_ptr, bool cb) {
    if (current_ == nullptr || idle_ || interrupt_depth_ > 0) {
        LOG_E("thread", "wait %s outside of a thread context", wait_type_name(type));
        return;
    }
    Thread *t = current_;
    t->wait = type;
    t->wait_id = id;
    t->deadline = deadline;
    t->timeout_ptr = timeout_ptr;
    t->wait_cb = cb;
    t->wait_seq = ++seq_;
    save_current(c);
    t->state = ThreadState::Waiting;
    LOG_T("sched", "'%s' waits %s id=%x deadline=%llu", t->name.c_str(), wait_type_name(type), id,
          static_cast<unsigned long long>(deadline));
    switch_to(c, best_ready(), false);
}

void Kernel::wake(Thread &t, u32 result) {
    if (t.state != ThreadState::Waiting) return;
    t.ctx.gpr[2] = result;
    if (t.timeout_ptr != 0u && t.deadline != 0u) {
        const u64 now = now_us();
        mem().store32(t.timeout_ptr, t.deadline > now ? static_cast<u32>(t.deadline - now) : 0u);
    }
    t.wait = WaitType::None;
    t.deadline = 0u;
    t.timeout_ptr = 0u;
    make_ready(t);
}

std::vector<Thread *> Kernel::waiters(WaitType type, s32 id, bool priority_order) {
    std::vector<Thread *> out;
    for (auto &[uid, t] : threads_)
        if (t->state == ThreadState::Waiting && t->wait == type && t->wait_id == id) out.push_back(t.get());
    std::sort(out.begin(), out.end(), [priority_order](const Thread *a, const Thread *b) {
        if (priority_order && a->priority != b->priority) return a->priority < b->priority;
        return a->wait_seq < b->wait_seq;
    });
    return out;
}

void Kernel::exit_current_thread(AllegrexContext &c, u32 status, bool delete_it) {
    Thread *t = current_;
    if (t == nullptr || idle_) return;
    LOG_I("thread", "exit '%s' uid=%x status=%x%s", t->name.c_str(), t->uid, status, delete_it ? " (delete)" : "");
    t->exit_status = status;
    t->state = ThreadState::Dormant;
    for (Thread *w : waiters(WaitType::ThreadEnd, t->uid, false)) wake(*w, status);
    if (delete_it) {
        if (t->stack_block != 0u) free_block(static_cast<s32>(t->stack_block));
        t->stack_block = 0u;
        t->state = ThreadState::Dead;
    }
    switch_to(c, best_ready(), false);
}

u32 Kernel::terminate_thread(AllegrexContext &c, s32 uid, bool delete_it) {
    Thread *t = thread(uid);
    if (t == nullptr) return err::UNKNOWN_THID;
    if (t == current_) return err::ILLEGAL_THID;
    LOG_I("thread", "terminate '%s' uid=%x%s", t->name.c_str(), t->uid, delete_it ? " (delete)" : "");
    t->state = ThreadState::Dormant;
    t->wait = WaitType::None;
    t->exit_status = err::THREAD_TERMINATED_VALUE;
    for (Thread *w : waiters(WaitType::ThreadEnd, t->uid, false)) wake(*w, err::THREAD_TERMINATED_VALUE);
    if (delete_it) {
        if (t->stack_block != 0u) free_block(static_cast<s32>(t->stack_block));
        t->stack_block = 0u;
        t->state = ThreadState::Dead;
    }
    ret(c, 0u);
    reschedule(c);
    return 0u;
}

u32 Kernel::delete_thread(s32 uid) {
    Thread *t = thread(uid);
    if (t == nullptr) return err::UNKNOWN_THID;
    if (t->state != ThreadState::Dormant) return err::NOT_DORMANT;
    if (t->stack_block != 0u) free_block(static_cast<s32>(t->stack_block));
    t->stack_block = 0u;
    t->state = ThreadState::Dead;
    return 0u;
}

void Kernel::thread_return(AllegrexContext &c) {
    exit_current_thread(c, c.gpr[2], false);
}

void Kernel::dump_threads() const {
    for (const auto &[uid, t] : threads_) {
        static const char *states[] = {"ready", "running", "waiting", "dormant", "dead"};
        LOG_E("thread", "  uid=%x '%s' prio=%x %s wait=%s id=%x pc=%08x ra=%08x", uid, t->name.c_str(), t->priority,
              states[static_cast<int>(t->state)], wait_type_name(t->wait), t->wait_id, t->ctx.pc, t->ctx.gpr[31]);
    }
}

// --- callbacks ---------------------------------------------------------------

s32 Kernel::create_callback(const std::string &name, u32 entry, u32 common) {
    const s32 uid = new_uid();
    callbacks[uid] = Callback{uid, name, current_uid(), entry, common, 0, 0};
    LOG_I("thread", "create callback '%s' uid=%x entry=%08x", name.c_str(), uid, entry);
    return uid;
}

u32 Kernel::delete_callback(s32 uid) {
    return callbacks.erase(uid) != 0u ? 0u : err::UNKNOWN_CBID;
}

void Kernel::notify_callback(s32 uid, s32 arg) {
    auto it = callbacks.find(uid);
    if (it == callbacks.end()) return;
    it->second.notify_count++;
    it->second.notify_arg = arg;
}

void Kernel::begin_callback(AllegrexContext &c, Thread &t, u32 entry, std::initializer_list<u32> args,
                            std::function<void(u32)> done) {
    auto frame = std::make_unique<CallbackFrame>();
    frame->saved = c;
    if (in_hle_call) frame->saved.pc = c.gpr[31];
    frame->done = std::move(done);
    t.callback_frames.push_back(std::move(frame));
    int i = 0;
    for (u32 a : args) c.gpr[4 + i++] = a;
    c.gpr[31] = kNestedReturnAddr;
    c.gpr[29] = (c.gpr[29] - 0x100u) & ~0xFu;
    c.gpr[28] = module_gp_;
    c.pc = entry;
}

bool Kernel::check_callbacks(AllegrexContext &c) {
    Thread *t = current_;
    if (t == nullptr || idle_ || interrupt_depth_ > 0) return false;
    for (auto &[uid, cb] : callbacks) {
        if (cb.thread_uid != t->uid || cb.notify_count <= 0) continue;
        const s32 count = cb.notify_count;
        cb.notify_count = 0;
        const s32 cbuid = uid;
        LOG_I("thread", "run callback '%s' on '%s'", cb.name.c_str(), t->name.c_str());
        begin_callback(c, *t, cb.entry, {static_cast<u32>(count), static_cast<u32>(cb.notify_arg), cb.common},
                       [this, cbuid](u32 v0) { if (v0 != 0u) delete_callback(cbuid); });
        return true;
    }
    return false;
}

// --- interrupts --------------------------------------------------------------

void Kernel::register_subintr(int intno, int sub, u32 handler, u32 arg) {
    subintrs_[{intno, sub}] = SubIntr{handler, arg, false};
    LOG_I("intr", "register subintr %d/%d handler=%08x arg=%08x", intno, sub, handler, arg);
}

void Kernel::release_subintr(int intno, int sub) { subintrs_.erase({intno, sub}); }

void Kernel::enable_subintr(int intno, int sub, bool enabled) {
    auto it = subintrs_.find({intno, sub});
    if (it != subintrs_.end()) it->second.enabled = enabled;
}

void Kernel::queue_interrupt(GuestCall call) { pending_interrupts_.push_back(std::move(call)); }

void Kernel::start_interrupt(AllegrexContext &c) {
    GuestCall call = std::move(pending_interrupts_.front());
    pending_interrupts_.pop_front();
    InterruptFrame frame;
    frame.saved = c;
    if (in_hle_call) frame.saved.pc = c.gpr[31];
    frame.done = std::move(call.done);
    frame.saved_current = current_;
    frame.saved_idle = idle_;
    interrupt_frames_.push_back(std::move(frame));
    ++interrupt_depth_;
    for (int i = 0; i < 4; ++i) c.gpr[4 + i] = call.args[i];
    c.gpr[31] = kNestedReturnAddr;
    c.gpr[29] = kInterruptStackTop - 0x40u;
    c.gpr[28] = module_gp_;
    c.pc = call.entry;
    psprecomp::set_runtime_thread_identity(nested_identity_--, call.what);
    LOG_T("intr", "start %s at %08x", call.what, call.entry);
}

void Kernel::nested_return(AllegrexContext &c) {
    const u32 v0 = c.gpr[2];
    if (interrupt_depth_ > 0) {
        InterruptFrame frame = std::move(interrupt_frames_.back());
        interrupt_frames_.pop_back();
        --interrupt_depth_;
        c = frame.saved;
        current_ = frame.saved_current;
        idle_ = frame.saved_idle;
        set_identity();
        if (frame.done) frame.done(v0);
        if (interrupt_depth_ == 0 && interrupts_enabled_ && !pending_interrupts_.empty()) start_interrupt(c);
        else reschedule(c);
        return;
    }
    Thread *t = current_;
    if (t == nullptr || t->callback_frames.empty()) {
        rt().stop("nested return with no active frame");
        return;
    }
    auto frame = std::move(t->callback_frames.back());
    t->callback_frames.pop_back();
    c = frame->saved;
    if (frame->done) frame->done(v0);
    if (!check_callbacks(c)) reschedule(c);
}

// --- event loop --------------------------------------------------------------

void Kernel::expire_timeouts(u64 now) {
    for (auto &[uid, t] : threads_) {
        if (t->state != ThreadState::Waiting || t->deadline == 0u || t->deadline > now) continue;
        const bool is_delay = t->wait == WaitType::Delay;
        if (t->wait == WaitType::LwMutex) {
            auto it = lw_mutexes.find(t->wait_id);
            if (it != lw_mutexes.end()) {
                const u32 wa = it->second.workarea;
                mem().store32(wa + 12u, mem().load32(wa + 12u) - 1u);
            }
        }
        if (t->timeout_ptr != 0u) mem().store32(t->timeout_ptr, 0u);
        t->timeout_ptr = 0u;
        wake(*t, is_delay ? 0u : err::WAIT_TIMEOUT);
    }
}

void Kernel::on_vblank() {
    for (auto &[uid, t] : threads_) {
        if (t->state != ThreadState::Waiting) continue;
        if (t->wait == WaitType::Vblank) {
            if (t->wait_value[0] <= 1u) wake(*t, 0u);
            else --t->wait_value[0];
        }
    }
    for (auto &listener : vblank_listeners) listener();
    for (auto &[key, sub] : subintrs_) {
        if (key.first != kVblankInterrupt || !sub.enabled) continue;
        GuestCall call;
        call.entry = sub.handler;
        call.args[0] = static_cast<u32>(key.second);
        call.args[1] = sub.arg;
        call.what = "vblank";
        queue_interrupt(std::move(call));
    }
}

u64 Kernel::next_deadline() const {
    u64 next = next_vblank_us_;
    for (const auto &[uid, t] : threads_)
        if (t->state == ThreadState::Waiting && t->deadline != 0u) next = std::min(next, t->deadline);
    if (listener_deadline != 0u) next = std::min(next, listener_deadline);
    return next;
}

void Kernel::process_events(AllegrexContext &c) {
    static bool processing = false;
    if (processing) return;
    processing = true;
    const u64 now = now_us();
    expire_timeouts(now);
    if (now >= next_vblank_us_) {
        const u64 missed = (now - next_vblank_us_) / kVblankPeriodUs;
        if (missed > 4u) {
            // Fell far behind (debugger, long load): drop the backlog.
            vcount_ += missed;
            next_vblank_us_ += missed * kVblankPeriodUs;
        }
        ++vcount_;
        next_vblank_us_ += kVblankPeriodUs;
        on_vblank();
        report_performance();
        static const u64 dump_at = [] { const char *e = std::getenv("P2_THREAD_DUMP"); return e ? std::strtoull(e, nullptr, 0) : 0ull; }();
        if (dump_at != 0u && vcount_ == dump_at) {
            LOG_E("thread", "thread dump at vblank %llu:", static_cast<unsigned long long>(vcount_));
            dump_threads();
        }
    }
    listener_deadline = 0u;
    for (auto &listener : event_listeners) listener(now);
    if (interrupt_depth_ == 0 && interrupts_enabled_ && !pending_interrupts_.empty()) start_interrupt(c);
    else reschedule(c);
    processing = false;
}

void Kernel::on_dispatch(AllegrexContext &c) {
    if (savestate_service()) return;
    if ((++dispatch_counter_ & 31u) != 0u) return;
    process_events(c);
}

void Kernel::idle(AllegrexContext &c) {
    // With every thread waiting, idle is the only boundary outside interrupts.
    if (savestate_service()) return;
    process_events(c);
    if (c.pc != kIdleAddr) return;
    const u64 now = now_us();
    const u64 next = next_deadline();
    if (next > now) {
        if (turbo()) {
            // Fast-forward: nothing to run until the next event, so jump the clock there.
            skip_time_to(next);
            return;
        }
        const u64 wait = std::min<u64>(next - now, 2000u);
        if (wait > 200u) std::this_thread::sleep_for(std::chrono::microseconds(wait - 100u));
        else std::this_thread::yield();
        idle_us_ += now_us() - now;
    }
}

void Kernel::report_performance() {
    // Every 10 s: share of wall time the guest spent idle (headroom).
    const u64 now = now_us();
    if (perf_window_start_ == 0u) { perf_window_start_ = now; idle_us_ = 0u; return; }
    const u64 elapsed = now - perf_window_start_;
    if (elapsed < 10'000'000u) return;
    const double secs = static_cast<double>(elapsed) / 1e6;
    const double frags = static_cast<double>(gpu::take_fragment_count());
    LOG_I("perf", "%.1f game fps, %.0fk frags/s, %.1f ns/frag, present %.0f%%, idle %.0f%%, gpu %.0f%% (vblank %llu)", static_cast<double>(perf_flips) / secs,
          frags / secs / 1000.0, frags > 0 ? static_cast<double>(perf_gpu_us) * 1000.0 / frags : 0.0,
          100.0 * static_cast<double>(perf_present_us) / static_cast<double>(elapsed),
          100.0 * static_cast<double>(idle_us_) / static_cast<double>(elapsed),
          100.0 * static_cast<double>(perf_gpu_us) / static_cast<double>(elapsed), static_cast<unsigned long long>(vcount_));
    perf_window_start_ = now;
    idle_us_ = 0u;
    perf_gpu_us = 0u;
    perf_present_us = 0u;
    perf_flips = 0u;
}

// --- savestates --------------------------------------------------------------------

std::string Kernel::save_blocker() const {
    if (in_hle_call) return "inside an HLE call";
    if (interrupt_depth_ > 0 || !pending_interrupts_.empty())
        return "an interrupt is in progress (depth " + std::to_string(interrupt_depth_) + ", " + std::to_string(pending_interrupts_.size()) +
               " pending, " + (interrupts_enabled_ ? "enabled" : "disabled") + ")";
    for (const auto &[uid, t] : threads_)
        if (!t->callback_frames.empty()) return "a callback is running";
    return {};
}

namespace {
void put_deadline(StateWriter &w, u64 deadline, u64 now) { w.pod<u64>(deadline == 0u ? 0u : (deadline > now ? deadline - now : 1u)); }
u64 get_deadline(StateReader &r, u64 now) { const u64 rel = r.pod<u64>(); return rel == 0u ? 0u : now + rel; }
} // namespace

void Kernel::save(StateWriter &w, const AllegrexContext &live) {
    const u64 now = now_us();
    w.section("KERN");
    w.pod(next_uid_);
    w.pod(seq_);
    w.pod(g_front_seq);
    w.pod(g_back_seq);
    w.pod(vcount_);
    put_deadline(w, next_vblank_us_, now);
    w.pod(interrupts_enabled_);
    w.pod(idle_);
    w.pod(current_ != nullptr ? current_->uid : 0);

    w.pod(static_cast<u32>(threads_.size()));
    for (const auto &[uid, t] : threads_) {
        w.pod(t->uid);
        w.str(t->name);
        w.pod(t->entry);
        w.pod(t->attr);
        w.pod(t->gp);
        w.pod(t->init_priority);
        w.pod(t->priority);
        w.pod(t->stack_block);
        w.pod(t->stack_bottom);
        w.pod(t->stack_size);
        w.pod(t->state);
        // The running thread's registers live in the runtime context.
        w.pod(t.get() == current_ && !idle_ ? live : t->ctx);
        w.pod(t->wait);
        w.pod(t->wait_id);
        put_deadline(w, t->deadline, now);
        w.pod(t->timeout_ptr);
        w.pod(t->wait_cb);
        w.bytes(t->wait_value, sizeof(t->wait_value));
        w.pod(t->ready_seq);
        w.pod(t->wait_seq);
        w.pod(t->exit_status);
        w.pod(t->running_callbacks);
    }

    w.pod(static_cast<u32>(callbacks.size()));
    for (const auto &[uid, cb] : callbacks) {
        w.pod(cb.uid); w.str(cb.name); w.pod(cb.thread_uid); w.pod(cb.entry); w.pod(cb.common); w.pod(cb.notify_count); w.pod(cb.notify_arg);
    }
    w.pod(static_cast<u32>(semaphores.size()));
    for (const auto &[uid, sem] : semaphores) {
        w.pod(sem.uid); w.str(sem.name); w.pod(sem.attr); w.pod(sem.count); w.pod(sem.max);
    }
    w.pod(static_cast<u32>(event_flags.size()));
    for (const auto &[uid, e] : event_flags) {
        w.pod(e.uid); w.str(e.name); w.pod(e.attr); w.pod(e.pattern);
    }
    w.pod(static_cast<u32>(lw_mutexes.size()));
    for (const auto &[uid, mx] : lw_mutexes) {
        w.pod(mx.uid); w.str(mx.name); w.pod(mx.workarea); w.pod(mx.attr);
    }
    w.pod(static_cast<u32>(blocks_.size()));
    for (const auto &[uid, b] : blocks_) {
        w.pod(b.uid); w.str(b.name); w.pod(b.address); w.pod(b.size); w.pod(b.partition);
    }
    w.pod(static_cast<u32>(free_.size()));
    for (const auto &f : free_) { w.pod(f.start); w.pod(f.end); }
    w.pod(static_cast<u32>(subintrs_.size()));
    for (const auto &[key, sub] : subintrs_) {
        w.pod(key.first); w.pod(key.second); w.pod(sub.handler); w.pod(sub.arg); w.pod(sub.enabled);
    }
}

void Kernel::load(StateReader &r, AllegrexContext &live) {
    const u64 now = now_us();
    r.section("KERN");
    r.pod(next_uid_);
    r.pod(seq_);
    r.pod(g_front_seq);
    r.pod(g_back_seq);
    r.pod(vcount_);
    next_vblank_us_ = get_deadline(r, now);
    r.pod(interrupts_enabled_);
    r.pod(idle_);
    const s32 current_uid = r.pod<s32>();

    threads_.clear();
    const u32 nthreads = r.pod<u32>();
    for (u32 i = 0; i < nthreads; ++i) {
        auto t = std::make_unique<Thread>();
        r.pod(t->uid);
        t->name = r.str();
        r.pod(t->entry);
        r.pod(t->attr);
        r.pod(t->gp);
        r.pod(t->init_priority);
        r.pod(t->priority);
        r.pod(t->stack_block);
        r.pod(t->stack_bottom);
        r.pod(t->stack_size);
        r.pod(t->state);
        r.pod(t->ctx);
        r.pod(t->wait);
        r.pod(t->wait_id);
        t->deadline = get_deadline(r, now);
        r.pod(t->timeout_ptr);
        r.pod(t->wait_cb);
        r.bytes(t->wait_value, sizeof(t->wait_value));
        r.pod(t->ready_seq);
        r.pod(t->wait_seq);
        r.pod(t->exit_status);
        r.pod(t->running_callbacks);
        const s32 uid = t->uid;
        threads_[uid] = std::move(t);
    }

    callbacks.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        Callback cb;
        r.pod(cb.uid); cb.name = r.str(); r.pod(cb.thread_uid); r.pod(cb.entry); r.pod(cb.common); r.pod(cb.notify_count); r.pod(cb.notify_arg);
        callbacks[cb.uid] = cb;
    }
    semaphores.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        Semaphore sem;
        r.pod(sem.uid); sem.name = r.str(); r.pod(sem.attr); r.pod(sem.count); r.pod(sem.max);
        semaphores[sem.uid] = sem;
    }
    event_flags.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        EventFlag e;
        r.pod(e.uid); e.name = r.str(); r.pod(e.attr); r.pod(e.pattern);
        event_flags[e.uid] = e;
    }
    lw_mutexes.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        LwMutex mx;
        r.pod(mx.uid); mx.name = r.str(); r.pod(mx.workarea); r.pod(mx.attr);
        lw_mutexes[mx.uid] = mx;
    }
    blocks_.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        MemoryBlock b;
        r.pod(b.uid); b.name = r.str(); r.pod(b.address); r.pod(b.size); r.pod(b.partition);
        blocks_[b.uid] = b;
    }
    free_.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        FreeRange f;
        r.pod(f.start); r.pod(f.end);
        free_.push_back(f);
    }
    subintrs_.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        std::pair<int, int> key;
        SubIntr sub;
        r.pod(key.first); r.pod(key.second); r.pod(sub.handler); r.pod(sub.arg); r.pod(sub.enabled);
        subintrs_[key] = sub;
    }

    pending_interrupts_.clear();
    interrupt_frames_.clear();
    interrupt_depth_ = 0;
    in_hle_call = false;
    auto it = threads_.find(current_uid);
    current_ = it != threads_.end() ? it->second.get() : nullptr;
    if (idle_ || current_ == nullptr) {
        AllegrexContext idle_ctx{};
        idle_ctx.pc = kIdleAddr;
        idle_ctx.gpr[29] = kInterruptStackTop - 0x400u;
        live = idle_ctx;
        idle_ = true;
        current_ = nullptr;
    } else {
        live = current_->ctx;
    }
    // A fresh identity makes sure no native frame from before the load resumes.
    psprecomp::set_runtime_thread_identity(nested_identity_--, "savestate");
    set_identity();
}

} // namespace p2

void p2::kernel_save(StateWriter &w) {
    AllegrexContext live = rt().cpu();
    // Taken right after an import returned: the thread continues at the return address.
    if (savestate_at_hle_return()) live.pc = live.gpr[31];
    g_kernel.save(w, live);
}
void p2::kernel_load(StateReader &r) { g_kernel.load(r, rt().cpu()); }
std::string p2::kernel_save_blocker() { return g_kernel.save_blocker(); }
