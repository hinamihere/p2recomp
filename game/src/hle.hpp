// Shared helpers for the Persona 2 host: guest argument access, logging and
// registration of HLE imports.
#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace p2 {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;

Runtime &rt();
inline GuestMemory &mem() { return rt().memory(); }

// --- logging ---------------------------------------------------------------
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };
extern LogLevel g_log_level;
void log_message(LogLevel level, const char *channel, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
#define P2_LOG(level, channel, ...)                                                   \
    do {                                                                              \
        if (static_cast<int>(level) <= static_cast<int>(::p2::g_log_level))           \
            ::p2::log_message(level, channel, __VA_ARGS__);                           \
    } while (0)
#define LOG_E(ch, ...) P2_LOG(::p2::LogLevel::Error, ch, __VA_ARGS__)
#define LOG_W(ch, ...) P2_LOG(::p2::LogLevel::Warn, ch, __VA_ARGS__)
#define LOG_I(ch, ...) P2_LOG(::p2::LogLevel::Info, ch, __VA_ARGS__)
#define LOG_D(ch, ...) P2_LOG(::p2::LogLevel::Debug, ch, __VA_ARGS__)
#define LOG_T(ch, ...) P2_LOG(::p2::LogLevel::Trace, ch, __VA_ARGS__)

// --- guest ABI ---------------------------------------------------------------
// Allegrex EABI: integer arguments in a0-a3 then t0-t3 (gpr 4..11).
inline u32 arg(const AllegrexContext &c, int i) { return c.gpr[4 + i]; }
// 64-bit arguments occupy an even-aligned register pair, low word first.
inline u64 arg64(const AllegrexContext &c, int first_reg_index) {
    return static_cast<u64>(c.gpr[4 + first_reg_index]) | (static_cast<u64>(c.gpr[5 + first_reg_index]) << 32);
}
inline void ret(AllegrexContext &c, u32 v) { c.gpr[2] = v; }
inline void ret64(AllegrexContext &c, u64 v) {
    c.gpr[2] = static_cast<u32>(v);
    c.gpr[3] = static_cast<u32>(v >> 32);
}
inline void retf(AllegrexContext &c, float v) { c.fpr[0] = v; }

std::string guest_string(u32 address, std::size_t max_length = 512);
bool valid_range(u32 address, u32 length);
void write_u64(u32 address, u64 value);
u64 read_u64(u32 address);

// Registers `fn` for the import `library::name` (NID looked up in nid_table.inc).
using HleFn = void (*)(AllegrexContext &);
void hle(const char *library, const char *name, HleFn fn);
// Reports imports of the EBOOT that have no handler yet.
void report_unimplemented_imports();

} // namespace p2
