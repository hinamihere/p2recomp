// HLE for ThreadManForUser, Kernel_Library, SysMem, interrupts, module and
// system services.
#include "kernel.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace p2 {
namespace {

Kernel &K = g_kernel;

u64 deadline_from(u32 timeout_ptr) {
    if (timeout_ptr == 0u || !valid_range(timeout_ptr, 4u)) return 0u;
    return Kernel::now_us() + mem().load32(timeout_ptr);
}

// --- threads -------------------------------------------------------------------

void CreateThread(AllegrexContext &c) {
    const std::string name = guest_string(arg(c, 0), 32);
    ret(c, static_cast<u32>(K.create_thread(name, arg(c, 1), static_cast<s32>(arg(c, 2)), arg(c, 3), arg(c, 4))));
}

void StartThread(AllegrexContext &c) {
    const u32 r = K.start_thread(c, static_cast<s32>(arg(c, 0)), arg(c, 1), arg(c, 2));
    if (r != 0u) ret(c, r);
}

void ExitThread(AllegrexContext &c) { K.exit_current_thread(c, arg(c, 0), false); }
void ExitDeleteThread(AllegrexContext &c) { K.exit_current_thread(c, arg(c, 0), true); }

void TerminateDeleteThread(AllegrexContext &c) {
    const u32 r = K.terminate_thread(c, static_cast<s32>(arg(c, 0)), true);
    if (r != 0u) ret(c, r);
}

void DeleteThread(AllegrexContext &c) { ret(c, K.delete_thread(static_cast<s32>(arg(c, 0)))); }

void wait_thread_end(AllegrexContext &c, bool cb) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    Thread *t = K.thread(uid);
    if (t == nullptr) { ret(c, err::UNKNOWN_THID); return; }
    if (t == K.current()) { ret(c, err::ILLEGAL_THID); return; }
    if (t->state == ThreadState::Dormant || t->state == ThreadState::Dead) { ret(c, t->exit_status); return; }
    ret(c, 0u);
    K.wait_current(c, WaitType::ThreadEnd, uid, deadline_from(arg(c, 1)), arg(c, 1), cb);
}
void WaitThreadEnd(AllegrexContext &c) { wait_thread_end(c, false); }
void WaitThreadEndCB(AllegrexContext &c) { wait_thread_end(c, true); }

void delay_thread(AllegrexContext &c, bool cb) {
    const u64 us = std::max<u32>(arg(c, 0), 200u);
    ret(c, 0u);
    if (cb && K.check_callbacks(c)) return;
    K.wait_current(c, WaitType::Delay, 0, Kernel::now_us() + us, 0u, cb);
}
void DelayThread(AllegrexContext &c) { delay_thread(c, false); }
void DelayThreadCB(AllegrexContext &c) { delay_thread(c, true); }

void ChangeThreadPriority(AllegrexContext &c) {
    s32 uid = static_cast<s32>(arg(c, 0));
    s32 priority = static_cast<s32>(arg(c, 1));
    Thread *t = K.thread(uid);
    if (t == nullptr) { ret(c, err::UNKNOWN_THID); return; }
    if (priority == 0) priority = K.current() != nullptr ? K.current()->priority : t->priority;
    if (priority < 0x08 || priority > 0x77) { ret(c, err::ILLEGAL_PRIORITY); return; }
    ret(c, 0u);
    K.change_priority(c, t->uid, priority);
}

void ChangeCurrentThreadAttr(AllegrexContext &c) {
    Thread *t = K.current();
    if (t != nullptr) t->attr = (t->attr & ~arg(c, 0)) | arg(c, 1);
    ret(c, 0u);
}

void GetThreadId(AllegrexContext &c) { ret(c, static_cast<u32>(K.current_uid())); }

void GetThreadCurrentPriority(AllegrexContext &c) {
    ret(c, K.current() != nullptr ? static_cast<u32>(K.current()->priority) : 0x20u);
}

// --- callbacks -----------------------------------------------------------------

void CreateCallback(AllegrexContext &c) {
    ret(c, static_cast<u32>(K.create_callback(guest_string(arg(c, 0), 32), arg(c, 1), arg(c, 2))));
}
void DeleteCallback(AllegrexContext &c) { ret(c, K.delete_callback(static_cast<s32>(arg(c, 0)))); }
void CheckCallback(AllegrexContext &c) {
    ret(c, 1u);
    if (!K.check_callbacks(c)) ret(c, 0u);
}

// --- semaphores ----------------------------------------------------------------

void CreateSema(AllegrexContext &c) {
    const s32 uid = K.new_uid();
    Semaphore s{uid, guest_string(arg(c, 0), 32), arg(c, 1), static_cast<s32>(arg(c, 2)), static_cast<s32>(arg(c, 3))};
    LOG_D("sema", "create '%s' uid=%x init=%d max=%d attr=%x", s.name.c_str(), uid, s.count, s.max, s.attr);
    K.semaphores[uid] = s;
    ret(c, static_cast<u32>(uid));
}

void DeleteSema(AllegrexContext &c) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    if (K.semaphores.erase(uid) == 0u) { ret(c, err::UNKNOWN_SEMID); return; }
    for (Thread *t : K.waiters(WaitType::Sema, uid, false)) K.wake(*t, err::WAIT_DELETE);
    ret(c, 0u);
    K.reschedule(c);
}

void SignalSema(AllegrexContext &c) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    const s32 count = static_cast<s32>(arg(c, 1));
    auto it = K.semaphores.find(uid);
    if (it == K.semaphores.end()) { ret(c, err::UNKNOWN_SEMID); return; }
    Semaphore &s = it->second;
    if (count <= 0) { ret(c, err::ILLEGAL_COUNT); return; }
    if (s.count + count > s.max) { ret(c, err::SEMA_OVF); return; }
    s.count += count;
    for (Thread *t : K.waiters(WaitType::Sema, uid, (s.attr & 0x100u) != 0u)) {
        const s32 need = static_cast<s32>(t->wait_value[0]);
        if (need > s.count) continue;
        s.count -= need;
        K.wake(*t, 0u);
    }
    ret(c, 0u);
    K.reschedule(c);
}

void wait_sema(AllegrexContext &c, bool cb) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    const s32 count = static_cast<s32>(arg(c, 1));
    auto it = K.semaphores.find(uid);
    if (it == K.semaphores.end()) { ret(c, err::UNKNOWN_SEMID); return; }
    Semaphore &s = it->second;
    if (count <= 0 || count > s.max) { ret(c, err::ILLEGAL_COUNT); return; }
    if (s.count >= count && K.waiters(WaitType::Sema, uid, false).empty()) {
        s.count -= count;
        ret(c, 0u);
        return;
    }
    ret(c, 0u);
    K.current()->wait_value[0] = static_cast<u32>(count);
    K.wait_current(c, WaitType::Sema, uid, deadline_from(arg(c, 2)), arg(c, 2), cb);
}
void WaitSema(AllegrexContext &c) { wait_sema(c, false); }
void WaitSemaCB(AllegrexContext &c) { wait_sema(c, true); }

// --- event flags -----------------------------------------------------------------

constexpr u32 kEvfWaitOr = 0x01u;
constexpr u32 kEvfClearAll = 0x10u;
constexpr u32 kEvfClear = 0x20u;

bool evf_try(EventFlag &e, u32 bits, u32 mode, u32 out_ptr) {
    const bool ok = (mode & kEvfWaitOr) != 0u ? (e.pattern & bits) != 0u : (e.pattern & bits) == bits;
    if (!ok) return false;
    if (out_ptr != 0u) mem().store32(out_ptr, e.pattern);
    if ((mode & kEvfClearAll) != 0u) e.pattern = 0u;
    if ((mode & kEvfClear) != 0u) e.pattern &= ~bits;
    return true;
}

void CreateEventFlag(AllegrexContext &c) {
    const s32 uid = K.new_uid();
    EventFlag e{uid, guest_string(arg(c, 0), 32), arg(c, 1), arg(c, 2)};
    LOG_D("evf", "create '%s' uid=%x attr=%x init=%x", e.name.c_str(), uid, e.attr, e.pattern);
    K.event_flags[uid] = e;
    ret(c, static_cast<u32>(uid));
}

void DeleteEventFlag(AllegrexContext &c) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    if (K.event_flags.erase(uid) == 0u) { ret(c, err::UNKNOWN_EVFID); return; }
    for (Thread *t : K.waiters(WaitType::EventFlag, uid, false)) K.wake(*t, err::WAIT_DELETE);
    ret(c, 0u);
    K.reschedule(c);
}

void SetEventFlag(AllegrexContext &c) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    auto it = K.event_flags.find(uid);
    if (it == K.event_flags.end()) { ret(c, err::UNKNOWN_EVFID); return; }
    EventFlag &e = it->second;
    e.pattern |= arg(c, 1);
    for (Thread *t : K.waiters(WaitType::EventFlag, uid, (e.attr & 0x100u) != 0u)) {
        if (evf_try(e, t->wait_value[0], t->wait_value[1], t->wait_value[2])) K.wake(*t, 0u);
    }
    ret(c, 0u);
    K.reschedule(c);
}

void ClearEventFlag(AllegrexContext &c) {
    auto it = K.event_flags.find(static_cast<s32>(arg(c, 0)));
    if (it == K.event_flags.end()) { ret(c, err::UNKNOWN_EVFID); return; }
    it->second.pattern &= arg(c, 1);
    ret(c, 0u);
}

void PollEventFlag(AllegrexContext &c) {
    auto it = K.event_flags.find(static_cast<s32>(arg(c, 0)));
    if (it == K.event_flags.end()) { ret(c, err::UNKNOWN_EVFID); return; }
    const u32 bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3);
    if (bits == 0u) { ret(c, err::EVF_ILPAT); return; }
    if (mode > 0x21u) { ret(c, err::ILLEGAL_MODE); return; }
    if (evf_try(it->second, bits, mode, out)) { ret(c, 0u); return; }
    if (out != 0u) mem().store32(out, it->second.pattern);
    ret(c, err::EVF_COND);
}

void wait_event_flag(AllegrexContext &c, bool cb) {
    const s32 uid = static_cast<s32>(arg(c, 0));
    auto it = K.event_flags.find(uid);
    if (it == K.event_flags.end()) { ret(c, err::UNKNOWN_EVFID); return; }
    const u32 bits = arg(c, 1), mode = arg(c, 2), out = arg(c, 3), timeout = arg(c, 4);
    if (bits == 0u) { ret(c, err::EVF_ILPAT); return; }
    if (mode > 0x21u) { ret(c, err::ILLEGAL_MODE); return; }
    EventFlag &e = it->second;
    if (evf_try(e, bits, mode, out)) { ret(c, 0u); return; }
    if ((e.attr & 0x200u) == 0u && !K.waiters(WaitType::EventFlag, uid, false).empty()) {
        ret(c, err::EVF_MULTI);
        return;
    }
    ret(c, 0u);
    Thread *t = K.current();
    t->wait_value[0] = bits;
    t->wait_value[1] = mode;
    t->wait_value[2] = out;
    K.wait_current(c, WaitType::EventFlag, uid, deadline_from(timeout), timeout, cb);
}
void WaitEventFlag(AllegrexContext &c) { wait_event_flag(c, false); }
void WaitEventFlagCB(AllegrexContext &c) { wait_event_flag(c, true); }

void ReferEventFlagStatus(AllegrexContext &c) {
    auto it = K.event_flags.find(static_cast<s32>(arg(c, 0)));
    if (it == K.event_flags.end()) { ret(c, err::UNKNOWN_EVFID); return; }
    const u32 info = arg(c, 1);
    if (info != 0u) {
        const EventFlag &e = it->second;
        auto &m = mem();
        for (u32 i = 0; i < 32u; ++i) m.store8(info + 4u + i, i < e.name.size() ? static_cast<u8>(e.name[i]) : 0u);
        m.store32(info + 36u, e.attr);
        m.store32(info + 40u, e.pattern);
        m.store32(info + 44u, e.pattern);
        m.store32(info + 48u, static_cast<u32>(K.waiters(WaitType::EventFlag, e.uid, false).size()));
    }
    ret(c, 0u);
}

// --- lightweight mutexes ---------------------------------------------------------
// Workarea: +0 lock level, +4 owner uid, +8 attr, +12 waiting threads, +16 uid.

void CreateLwMutex(AllegrexContext &c) {
    const u32 wa = arg(c, 0);
    const s32 uid = K.new_uid();
    const u32 attr = arg(c, 2);
    const s32 count = static_cast<s32>(arg(c, 3));
    auto &m = mem();
    m.store32(wa + 0u, static_cast<u32>(count));
    m.store32(wa + 4u, count > 0 ? static_cast<u32>(K.current_uid()) : 0u);
    m.store32(wa + 8u, attr);
    m.store32(wa + 12u, 0u);
    m.store32(wa + 16u, static_cast<u32>(uid));
    K.lw_mutexes[uid] = LwMutex{uid, guest_string(arg(c, 1), 32), wa, attr};
    ret(c, 0u);
}

void DeleteLwMutex(AllegrexContext &c) {
    const u32 wa = arg(c, 0);
    const s32 uid = static_cast<s32>(mem().load32(wa + 16u));
    if (K.lw_mutexes.erase(uid) == 0u) { ret(c, err::LWMUTEX_NOT_FOUND); return; }
    for (Thread *t : K.waiters(WaitType::LwMutex, uid, false)) K.wake(*t, err::WAIT_DELETE);
    mem().store32(wa + 16u, 0u);
    ret(c, 0u);
    K.reschedule(c);
}

void lock_lw_mutex(AllegrexContext &c, bool cb) {
    const u32 wa = arg(c, 0);
    const s32 count = static_cast<s32>(arg(c, 1));
    auto &m = mem();
    const s32 uid = static_cast<s32>(m.load32(wa + 16u));
    if (!K.lw_mutexes.contains(uid)) { ret(c, err::LWMUTEX_NOT_FOUND); return; }
    if (count <= 0) { ret(c, err::ILLEGAL_COUNT); return; }
    const s32 level = static_cast<s32>(m.load32(wa));
    const s32 owner = static_cast<s32>(m.load32(wa + 4u));
    const s32 self = K.current_uid();
    if (level == 0) {
        m.store32(wa, static_cast<u32>(count));
        m.store32(wa + 4u, static_cast<u32>(self));
        ret(c, 0u);
        return;
    }
    if (owner == self) {
        if ((m.load32(wa + 8u) & 0x200u) == 0u) { ret(c, err::LWMUTEX_RECURSIVE_NOT_ALLOWED); return; }
        m.store32(wa, static_cast<u32>(level + count));
        ret(c, 0u);
        return;
    }
    m.store32(wa + 12u, m.load32(wa + 12u) + 1u);
    ret(c, 0u);
    K.current()->wait_value[0] = static_cast<u32>(count);
    K.wait_current(c, WaitType::LwMutex, uid, deadline_from(arg(c, 2)), arg(c, 2), cb);
}
void LockLwMutex(AllegrexContext &c) { lock_lw_mutex(c, false); }
void LockLwMutexCB(AllegrexContext &c) { lock_lw_mutex(c, true); }

void UnlockLwMutex(AllegrexContext &c) {
    const u32 wa = arg(c, 0);
    const s32 count = static_cast<s32>(arg(c, 1));
    auto &m = mem();
    const s32 uid = static_cast<s32>(m.load32(wa + 16u));
    if (!K.lw_mutexes.contains(uid)) { ret(c, err::LWMUTEX_NOT_FOUND); return; }
    const s32 level = static_cast<s32>(m.load32(wa));
    if (level == 0 || static_cast<s32>(m.load32(wa + 4u)) != K.current_uid()) { ret(c, err::LWMUTEX_UNLOCKED); return; }
    if (count <= 0 || level < count) { ret(c, err::LWMUTEX_UNLOCK_UNDERFLOW); return; }
    m.store32(wa, static_cast<u32>(level - count));
    ret(c, 0u);
    if (level - count != 0) return;
    m.store32(wa + 4u, 0u);
    const auto waiting = K.waiters(WaitType::LwMutex, uid, (m.load32(wa + 8u) & 0x100u) != 0u);
    if (waiting.empty()) return;
    Thread *next = waiting.front();
    m.store32(wa, next->wait_value[0]);
    m.store32(wa + 4u, static_cast<u32>(next->uid));
    m.store32(wa + 12u, m.load32(wa + 12u) - 1u);
    K.wake(*next, 0u);
    K.reschedule(c);
}

// --- time ------------------------------------------------------------------------

void GetSystemTimeLow(AllegrexContext &c) { ret(c, static_cast<u32>(Kernel::now_us())); }
void GetSystemTimeWide(AllegrexContext &c) { ret64(c, Kernel::now_us()); }

u64 unix_time_us() {
    using namespace std::chrono;
    return static_cast<u64>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

void LibcGettimeofday(AllegrexContext &c) {
    const u64 t = unix_time_us();
    if (arg(c, 0) != 0u) {
        mem().store32(arg(c, 0), static_cast<u32>(t / 1000000u));
        mem().store32(arg(c, 0) + 4u, static_cast<u32>(t % 1000000u));
    }
    ret(c, 0u);
}
void LibcClock(AllegrexContext &c) { ret(c, static_cast<u32>(Kernel::now_us() - 1'000'000u)); }
void LibcTime(AllegrexContext &c) {
    const u32 t = static_cast<u32>(unix_time_us() / 1000000u);
    if (arg(c, 0) != 0u) mem().store32(arg(c, 0), t);
    ret(c, t);
}

void RtcGetTick(AllegrexContext &c) {
    // Ticks are microseconds since 0001-01-01.
    write_u64(arg(c, 0), unix_time_us() + 62135596800000000ull);
    ret(c, 0u);
}

void RtcGetCurrentClockLocalTime(AllegrexContext &c) {
    const u64 us = unix_time_us();
    const std::time_t secs = static_cast<std::time_t>(us / 1000000u);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &secs);
#else
    localtime_r(&secs, &local);
#endif
    const u32 p = arg(c, 0);
    auto &m = mem();
    m.store16(p + 0u, static_cast<u16>(local.tm_year + 1900));
    m.store16(p + 2u, static_cast<u16>(local.tm_mon + 1));
    m.store16(p + 4u, static_cast<u16>(local.tm_mday));
    m.store16(p + 6u, static_cast<u16>(local.tm_hour));
    m.store16(p + 8u, static_cast<u16>(local.tm_min));
    m.store16(p + 10u, static_cast<u16>(local.tm_sec));
    m.store32(p + 12u, static_cast<u32>(us % 1000000u));
    ret(c, 0u);
}

// --- interrupts / misc kernel -------------------------------------------------------

void CpuSuspendIntr(AllegrexContext &c) { ret(c, K.suspend_interrupts()); }
void CpuResumeIntr(AllegrexContext &c) { K.resume_interrupts(arg(c, 0)); ret(c, 0u); }

void RegisterSubIntrHandler(AllegrexContext &c) {
    K.register_subintr(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)), arg(c, 2), arg(c, 3));
    ret(c, 0u);
}
void ReleaseSubIntrHandler(AllegrexContext &c) {
    K.release_subintr(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)));
    ret(c, 0u);
}
void EnableSubIntr(AllegrexContext &c) {
    K.enable_subintr(static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 1)), true);
    ret(c, 0u);
}

void KernelMemset(AllegrexContext &c) {
    const u32 dst = arg(c, 0), value = arg(c, 1), size = arg(c, 2);
    auto &m = mem();
    if (u8 *p = m.raw_pointer(dst, size)) std::memset(p, static_cast<int>(value & 0xFFu), size);
    else for (u32 i = 0; i < size; ++i) m.store8(dst + i, static_cast<u8>(value));
    ret(c, dst);
}

void KernelMemcpy(AllegrexContext &c) {
    const u32 dst = arg(c, 0), src = arg(c, 1), size = arg(c, 2);
    auto &m = mem();
    u8 *d = m.raw_pointer(dst, size);
    const u8 *s = m.raw_pointer(src, size);
    if (d != nullptr && s != nullptr) std::memmove(d, s, size);
    else for (u32 i = 0; i < size; ++i) m.store8(dst + i, m.load8(src + i));
    ret(c, dst);
}

void Printf(AllegrexContext &c) {
    LOG_I("guest", "printf: %s", guest_string(arg(c, 0)).c_str());
    ret(c, 0u);
}

void AllocPartitionMemory(AllegrexContext &c) {
    const std::string name = guest_string(arg(c, 1), 32);
    ret(c, static_cast<u32>(K.alloc_block(name, static_cast<int>(arg(c, 0)), static_cast<int>(arg(c, 2)), arg(c, 3), arg(c, 4))));
}
void FreePartitionMemory(AllegrexContext &c) { ret(c, K.free_block(static_cast<s32>(arg(c, 0)))); }
void GetBlockHeadAddr(AllegrexContext &c) {
    MemoryBlock *b = K.block(static_cast<s32>(arg(c, 0)));
    ret(c, b != nullptr ? b->address : err::ILLEGAL_MEMBLOCK);
}

void ReturnZero(AllegrexContext &c) { ret(c, 0u); }
void ReturnOne(AllegrexContext &c) { ret(c, 1u); }

void ExitGame(AllegrexContext &) { rt().stop("sceKernelExitGame"); }
void StopUnloadSelf(AllegrexContext &) { rt().stop("module stopped itself"); }

constexpr u32 kModuleUid = 0x7001u;
void GetModuleId(AllegrexContext &c) { ret(c, kModuleUid); }

void Stdin(AllegrexContext &c) { ret(c, 0u); }
void Stdout(AllegrexContext &c) { ret(c, 1u); }
void Stderr(AllegrexContext &c) { ret(c, 2u); }

// UMD: the disc is always present and readable.
void UmdGetDriveStat(AllegrexContext &c) { ret(c, 0x32u); }

} // namespace

void register_kernel_imports() {
    const char *T = "ThreadManForUser";
    hle(T, "sceKernelCreateThread", CreateThread);
    hle(T, "sceKernelStartThread", StartThread);
    hle(T, "sceKernelExitThread", ExitThread);
    hle(T, "sceKernelExitDeleteThread", ExitDeleteThread);
    hle(T, "sceKernelTerminateDeleteThread", TerminateDeleteThread);
    hle(T, "sceKernelDeleteThread", DeleteThread);
    hle(T, "sceKernelWaitThreadEnd", WaitThreadEnd);
    hle(T, "sceKernelWaitThreadEndCB", WaitThreadEndCB);
    hle(T, "sceKernelDelayThread", DelayThread);
    hle(T, "sceKernelDelayThreadCB", DelayThreadCB);
    hle(T, "sceKernelChangeThreadPriority", ChangeThreadPriority);
    hle(T, "sceKernelChangeCurrentThreadAttr", ChangeCurrentThreadAttr);
    hle(T, "sceKernelGetThreadCurrentPriority", GetThreadCurrentPriority);
    hle(T, "sceKernelCreateCallback", CreateCallback);
    hle(T, "sceKernelDeleteCallback", DeleteCallback);
    hle(T, "sceKernelCheckCallback", CheckCallback);
    hle(T, "sceKernelCreateSema", CreateSema);
    hle(T, "sceKernelDeleteSema", DeleteSema);
    hle(T, "sceKernelSignalSema", SignalSema);
    hle(T, "sceKernelWaitSema", WaitSema);
    hle(T, "sceKernelWaitSemaCB", WaitSemaCB);
    hle(T, "sceKernelCreateEventFlag", CreateEventFlag);
    hle(T, "sceKernelDeleteEventFlag", DeleteEventFlag);
    hle(T, "sceKernelSetEventFlag", SetEventFlag);
    hle(T, "sceKernelClearEventFlag", ClearEventFlag);
    hle(T, "sceKernelPollEventFlag", PollEventFlag);
    hle(T, "sceKernelWaitEventFlag", WaitEventFlag);
    hle(T, "sceKernelWaitEventFlagCB", WaitEventFlagCB);
    hle(T, "sceKernelReferEventFlagStatus", ReferEventFlagStatus);
    hle(T, "sceKernelCreateLwMutex", CreateLwMutex);
    hle(T, "sceKernelDeleteLwMutex", DeleteLwMutex);
    hle(T, "sceKernelGetSystemTimeLow", GetSystemTimeLow);
    hle(T, "sceKernelGetSystemTimeWide", GetSystemTimeWide);

    const char *L = "Kernel_Library";
    hle(L, "sceKernelCpuSuspendIntr", CpuSuspendIntr);
    hle(L, "sceKernelCpuResumeIntr", CpuResumeIntr);
    hle(L, "sceKernelMemset", KernelMemset);
    hle(L, "sceKernelMemcpy", KernelMemcpy);
    hle(L, "sceKernelLockLwMutex", LockLwMutex);
    hle(L, "sceKernelLockLwMutexCB", LockLwMutexCB);
    hle(L, "sceKernelUnlockLwMutex", UnlockLwMutex);
    hle(L, "sceKernelGetThreadId", GetThreadId);

    const char *S = "SysMemUserForUser";
    hle(S, "sceKernelPrintf", Printf);
    hle(S, "sceKernelSetCompilerVersion", ReturnZero);
    hle(S, "sceKernelSetCompiledSdkVersion603_605", ReturnZero);
    hle(S, "sceKernelAllocPartitionMemory", AllocPartitionMemory);
    hle(S, "sceKernelFreePartitionMemory", FreePartitionMemory);
    hle(S, "sceKernelGetBlockHeadAddr", GetBlockHeadAddr);

    hle("InterruptManager", "sceKernelRegisterSubIntrHandler", RegisterSubIntrHandler);
    hle("InterruptManager", "sceKernelReleaseSubIntrHandler", ReleaseSubIntrHandler);
    hle("InterruptManager", "sceKernelEnableSubIntr", EnableSubIntr);

    hle("LoadExecForUser", "sceKernelExitGame", ExitGame);
    hle("LoadExecForUser", "sceKernelRegisterExitCallback", ReturnZero);
    hle("ModuleMgrForUser", "sceKernelGetModuleIdByAddress", GetModuleId);
    hle("ModuleMgrForUser", "sceKernelGetModuleId", GetModuleId);
    hle("ModuleMgrForUser", "sceKernelStopModule", ReturnZero);
    hle("ModuleMgrForUser", "sceKernelUnloadModule", ReturnZero);
    hle("ModuleMgrForUser", "sceKernelStopUnloadSelfModuleWithStatus", StopUnloadSelf);
    hle("StdioForUser", "sceKernelStdin", Stdin);
    hle("StdioForUser", "sceKernelStdout", Stdout);
    hle("StdioForUser", "sceKernelStderr", Stderr);

    const char *U = "UtilsForUser";
    hle(U, "sceKernelLibcGettimeofday", LibcGettimeofday);
    hle(U, "sceKernelDcacheWritebackAll", ReturnZero);
    hle(U, "sceKernelDcacheWritebackRange", ReturnZero);
    hle(U, "sceKernelLibcClock", LibcClock);
    hle(U, "sceKernelLibcTime", LibcTime);
    hle("sceSuspendForUser", "sceKernelPowerTick", ReturnZero);

    hle("sceRtc", "sceRtcGetTick", RtcGetTick);
    hle("sceRtc", "sceRtcGetCurrentClockLocalTime", RtcGetCurrentClockLocalTime);
    hle("scePower", "scePowerRegisterCallback", ReturnZero);
    hle("scePower", "scePowerSetClockFrequency630", ReturnZero);
    hle("sceImpose", "sceImposeSetLanguageMode", ReturnZero);

    const char *D = "sceUmdUser";
    hle(D, "sceUmdCheckMedium", ReturnOne);
    hle(D, "sceUmdActivate", ReturnZero);
    hle(D, "sceUmdGetDriveStat", UmdGetDriveStat);
    hle(D, "sceUmdWaitDriveStat", ReturnZero);
    hle(D, "sceUmdWaitDriveStatCB", ReturnZero);
    hle(D, "sceUmdCancelWaitDriveStat", ReturnZero);
}

} // namespace p2
