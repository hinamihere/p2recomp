// PSP kernel emulation for the recompiled game: threads, scheduling, sync
// objects, interrupts/callbacks and the user memory partition.
//
// Execution model. The PSPRecomp runtime has exactly one live register
// context (Runtime::cpu()). A PSP thread switch saves that context into the
// outgoing Thread and loads the incoming one; a new thread identity bumps the
// runtime's switch generation so generated code never resumes a stale native
// frame. Blocking HLE calls save the caller with pc = $ra (the instruction
// after the import call) and v0 = the eventual result.
//
// Guest code invoked by the kernel (interrupt handlers, callbacks, thread
// entry) runs with $ra pointing at a trampoline address registered as a host
// function, which restores the interrupted context.
#pragma once

#include "hle.hpp"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace p2 {

class StateWriter;
class StateReader;

namespace err {
constexpr u32 ERROR = 0x80020001u;
constexpr u32 ILLEGAL_CONTEXT = 0x80020064u;
constexpr u32 UNKNOWN_UID = 0x800200cbu;
constexpr u32 ILLEGAL_ARGUMENT = 0x800200d2u;
constexpr u32 ILLEGAL_ADDR = 0x800200d3u;
constexpr u32 ILLEGAL_PARTITION = 0x800200d6u;
constexpr u32 NO_MEMORY = 0x80020190u;
constexpr u32 ILLEGAL_ATTR = 0x80020191u;
constexpr u32 ILLEGAL_PRIORITY = 0x80020193u;
constexpr u32 ILLEGAL_STACK_SIZE = 0x80020194u;
constexpr u32 ILLEGAL_MODE = 0x80020195u;
constexpr u32 ILLEGAL_THID = 0x80020197u;
constexpr u32 UNKNOWN_THID = 0x80020198u;
constexpr u32 UNKNOWN_SEMID = 0x80020199u;
constexpr u32 UNKNOWN_EVFID = 0x8002019au;
constexpr u32 UNKNOWN_CBID = 0x800201a1u;
constexpr u32 DORMANT = 0x800201a2u;
constexpr u32 NOT_DORMANT = 0x800201a4u;
constexpr u32 CAN_NOT_WAIT = 0x800201a7u;
constexpr u32 THREAD_TERMINATED_VALUE = 0x800201acu;
constexpr u32 WAIT_TIMEOUT = 0x800201a8u;
constexpr u32 WAIT_CANCEL = 0x800201a9u;
constexpr u32 SEMA_ZERO = 0x800201adu;
constexpr u32 SEMA_OVF = 0x800201aeu;
constexpr u32 EVF_COND = 0x800201afu;
constexpr u32 EVF_MULTI = 0x800201b0u;
constexpr u32 EVF_ILPAT = 0x800201b1u;
constexpr u32 WAIT_DELETE = 0x800201b5u;
constexpr u32 ILLEGAL_MEMBLOCK = 0x800201b6u;
constexpr u32 ILLEGAL_MEMSIZE = 0x800201b7u;
constexpr u32 ILLEGAL_COUNT = 0x800201bdu;
constexpr u32 LWMUTEX_NOT_FOUND = 0x800201cau;
constexpr u32 LWMUTEX_LOCKED = 0x800201cbu;
constexpr u32 LWMUTEX_UNLOCKED = 0x800201ccu;
constexpr u32 LWMUTEX_LOCK_OVERFLOW = 0x800201cdu;
constexpr u32 LWMUTEX_UNLOCK_UNDERFLOW = 0x800201ceu;
constexpr u32 LWMUTEX_RECURSIVE_NOT_ALLOWED = 0x800201cfu;
constexpr u32 FILE_NOT_FOUND = 0x80010002u;
constexpr u32 BADF = 0x80020323u;
constexpr u32 ASYNC_BUSY = 0x80020329u;
} // namespace err

// Host trampolines and the interrupt stack live in the first 16 KiB of the user
// partition, below the module image at 0x08804000, which the game never uses.
constexpr u32 kThreadReturnAddr = 0x08800100u;
constexpr u32 kNestedReturnAddr = 0x08800110u;
constexpr u32 kIdleAddr = 0x08800120u;
constexpr u32 kInterruptStackTop = 0x08803F00u; // grows down to 0x08801000

constexpr u32 kUserMemoryStart = 0x08800000u;
constexpr u32 kUserMemoryEnd = 0x0A000000u;

constexpr int kVblankInterrupt = 30;
constexpr int kGeInterrupt = 25;

enum class WaitType : u8 {
    None,
    Delay,
    Sema,
    EventFlag,
    ThreadEnd,
    LwMutex,
    Vblank,
    Ctrl,
    Audio,
    AsyncIo,
    GeList,
    GeDraw,
    Umd,
    Utility,
};
const char *wait_type_name(WaitType type);

enum class ThreadState : u8 { Ready, Running, Waiting, Dormant, Dead };

struct CallbackFrame;

struct Thread {
    s32 uid{};
    std::string name;
    u32 entry{};
    u32 attr{};
    u32 gp{};
    s32 init_priority{};
    s32 priority{};
    u32 stack_block{}; // memory block uid, 0 if none
    u32 stack_bottom{};
    u32 stack_size{};
    ThreadState state{ThreadState::Dormant};
    AllegrexContext ctx{};
    WaitType wait{WaitType::None};
    s32 wait_id{};
    u64 deadline{};     // host-microsecond absolute deadline, 0 = none
    u32 timeout_ptr{};  // guest pointer to update with remaining time
    bool wait_cb{};
    u32 wait_value[4]{};
    u64 ready_seq{};
    u64 wait_seq{};
    u32 exit_status{};
    std::vector<std::unique_ptr<CallbackFrame>> callback_frames;
    bool running_callbacks{};
};

struct Callback {
    s32 uid{};
    std::string name;
    s32 thread_uid{};
    u32 entry{};
    u32 common{};
    s32 notify_count{};
    s32 notify_arg{};
};

struct Semaphore {
    s32 uid{};
    std::string name;
    u32 attr{};
    s32 count{};
    s32 max{};
};

struct EventFlag {
    s32 uid{};
    std::string name;
    u32 attr{};
    u32 pattern{};
};

struct LwMutex {
    s32 uid{};
    std::string name;
    u32 workarea{};
    u32 attr{};
};

struct MemoryBlock {
    s32 uid{};
    std::string name;
    u32 address{};
    u32 size{};
    int partition{};
};

// A pending guest function invocation made by the kernel.
struct GuestCall {
    u32 entry{};
    u32 args[4]{};
    std::function<void(u32)> done; // receives guest v0
    const char *what{"guest call"};
};

struct CallbackFrame {
    AllegrexContext saved{};
    std::function<void(u32)> done;
};

class Kernel {
public:
    void init(u32 module_gp, u32 module_end);
    void boot_main_thread(u32 entry, u32 priority, u32 stack_size, u32 attr, const std::string &argument);

    // --- time ---
    static u64 now_us();
    // Fast-forward: while active, idle time is skipped instead of slept.
    bool turbo() const;
    void set_turbo_until(u64 vblank) { turbo_until_ = vblank; }
    void set_turbo_key(bool held) { turbo_key_ = held; }
    // Advances the virtual clock so that now_us() >= target.
    void skip_time_to(u64 target);
    // Host time the game spent waiting (sleeping) outside the idle loop, e.g. polling the vcount.
    void add_idle_us(u64 us) { idle_us_ += us; }
    u64 vblank_count() const { return vcount_; }
    u64 next_vblank_us() const { return next_vblank_us_; }
    static constexpr u64 kVblankPeriodUs = 16683u; // 59.94 Hz

    // --- objects ---
    s32 new_uid() { return next_uid_++; }
    Thread *thread(s32 uid);
    Thread *current() { return current_; }
    s32 current_uid() const { return current_ != nullptr ? current_->uid : 0; }

    // --- threads (called from HLE with the live context) ---
    s32 create_thread(const std::string &name, u32 entry, s32 priority, u32 stack_size, u32 attr);
    u32 start_thread(AllegrexContext &c, s32 uid, u32 arg_len, u32 arg_ptr);
    void exit_current_thread(AllegrexContext &c, u32 status, bool delete_it);
    u32 terminate_thread(AllegrexContext &c, s32 uid, bool delete_it);
    u32 delete_thread(s32 uid);

    // Puts the current thread to sleep. `ret(c, x)` must already hold the value
    // the call returns if it is woken normally (callers usually leave 0).
    void wait_current(AllegrexContext &c, WaitType type, s32 id, u64 timeout_us, u32 timeout_ptr, bool cb);
    // Makes a waiting thread ready with the given v0.
    void wake(Thread &t, u32 result);
    // Switches away from the current thread if a better one is ready.
    void reschedule(AllegrexContext &c);
    void yield_current(AllegrexContext &c);
    void change_priority(AllegrexContext &c, s32 uid, s32 priority);

    template <typename Fn>
    void for_each_waiter(WaitType type, s32 id, bool priority_order, Fn fn);
    std::vector<Thread *> waiters(WaitType type, s32 id, bool priority_order);

    // --- callbacks ---
    s32 create_callback(const std::string &name, u32 entry, u32 common);
    u32 delete_callback(s32 uid);
    void notify_callback(s32 uid, s32 arg);
    // Runs pending callbacks of the current thread. Returns true if a callback
    // was started (the context was switched to guest callback code).
    bool check_callbacks(AllegrexContext &c);
    // Runs a guest function on the current thread from inside an HLE call (e.g.
    // the MPEG ringbuffer read callback). `done` receives the guest v0 after the
    // HLE call's context is restored, and may set that call's return value via
    // rt().cpu() or start another guest call.
    void call_guest(AllegrexContext &c, u32 entry, std::initializer_list<u32> args, std::function<void(u32)> done) {
        begin_callback(c, *current_, entry, args, std::move(done));
    }

    // --- interrupts ---
    void register_subintr(int intno, int sub, u32 handler, u32 arg);
    void release_subintr(int intno, int sub);
    void enable_subintr(int intno, int sub, bool enabled);
    void queue_interrupt(GuestCall call);
    bool interrupts_enabled() const { return interrupts_enabled_; }
    u32 suspend_interrupts() { const u32 old = interrupts_enabled_ ? 1u : 0u; interrupts_enabled_ = false; return old; }
    void resume_interrupts(u32 flag) { interrupts_enabled_ = flag != 0u; }
    bool in_interrupt() const { return interrupt_depth_ > 0; }

    // --- sync objects ---
    std::map<s32, Semaphore> semaphores;
    std::map<s32, EventFlag> event_flags;
    std::map<s32, LwMutex> lw_mutexes;
    std::map<s32, Callback> callbacks;

    // --- memory ---
    s32 alloc_block(const std::string &name, int partition, int type, u32 size, u32 addr, u32 alignment = 0x100);
    u32 free_block(s32 uid);
    MemoryBlock *block(s32 uid);
    u32 max_free() const;
    u32 total_free() const;

    // --- event loop ---
    // Advances timers and vblank, delivers interrupts and preempts if needed.
    void process_events(AllegrexContext &c);
    // Hooks from the runtime dispatch loop.
    void on_dispatch(AllegrexContext &c);
    void idle(AllegrexContext &c);
    void nested_return(AllegrexContext &c);
    void thread_return(AllegrexContext &c);

    // Performance counters, reported every 10 s.
    u64 perf_gpu_us{};
    u64 perf_present_us{};
    u64 perf_flips{};
    u64 total_flips{};

    // Set while an HLE import executes: saved contexts resume at $ra.
    bool in_hle_call{};

    // Host hooks fired at vblank (presentation, input).
    std::vector<std::function<void()>> vblank_listeners;
    // Extra wakeup checks run from process_events (audio etc.).
    std::vector<std::function<void(u64 now)>> event_listeners;
    // Earliest time some listener wants to be polled again.
    u64 listener_deadline{};

    u32 module_gp() const { return module_gp_; }

    // Savestates (see savestate.cpp). Only valid at an outer dispatch boundary.
    std::string save_blocker() const;
    void save(StateWriter &w, const AllegrexContext &live);
    void load(StateReader &r, AllegrexContext &live);
    void dump_threads() const;

private:
    void switch_to(AllegrexContext &c, Thread *next, bool save_old);
    Thread *best_ready();
    void save_current(AllegrexContext &c);
    void make_ready(Thread &t);
    void set_identity();
    void start_interrupt(AllegrexContext &c);
    void begin_callback(AllegrexContext &c, Thread &t, u32 entry, std::initializer_list<u32> args,
                        std::function<void(u32)> done);
    void expire_timeouts(u64 now);
    void on_vblank();
    u64 next_deadline() const;
    void report_performance();
    static u64 time_offset_us_;
    static u64 turbo_until_;
    static bool turbo_key_;
    u64 idle_us_{};
    u64 perf_window_start_{};

    s32 next_uid_{0x100};
    u64 seq_{};
    std::map<s32, std::unique_ptr<Thread>> threads_;
    Thread *current_{};
    bool idle_{};
    u32 module_gp_{};

    // interrupts
    struct SubIntr { u32 handler{}; u32 arg{}; bool enabled{}; };
    std::map<std::pair<int, int>, SubIntr> subintrs_;
    std::deque<GuestCall> pending_interrupts_;
    struct InterruptFrame { AllegrexContext saved{}; std::function<void(u32)> done; Thread *saved_current{}; bool saved_idle{}; };
    std::vector<InterruptFrame> interrupt_frames_;
    int interrupt_depth_{};
    bool interrupts_enabled_{true};
    s32 nested_identity_{-2};

    // vblank
    u64 boot_us_{};
    u64 next_vblank_us_{};
    u64 vcount_{};
    u32 dispatch_counter_{};

    // memory
    std::map<s32, MemoryBlock> blocks_;
    struct FreeRange { u32 start; u32 end; };
    std::vector<FreeRange> free_;
};

extern Kernel g_kernel;

template <typename Fn>
void Kernel::for_each_waiter(WaitType type, s32 id, bool priority_order, Fn fn) {
    for (Thread *t : waiters(type, id, priority_order)) fn(*t);
}

void register_kernel_imports();

} // namespace p2
