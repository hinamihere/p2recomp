// Savestates: every host module serializes its own state into a flat binary
// stream; guest RAM and VRAM are stored verbatim.
#pragma once

#include "hle.hpp"

#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace p2 {

class StateWriter {
public:
    void bytes(const void *data, std::size_t size) {
        const auto *p = static_cast<const u8 *>(data);
        buf.insert(buf.end(), p, p + size);
    }
    template <typename T>
    void pod(const T &value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&value, sizeof(T));
    }
    void str(const std::string &s) {
        pod(static_cast<u32>(s.size()));
        bytes(s.data(), s.size());
    }
    template <typename T>
    void vec(const std::vector<T> &v) {
        pod(static_cast<u32>(v.size()));
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
    void section(const char *tag) { bytes(tag, 4); }
    std::vector<u8> buf;
};

class StateReader {
public:
    explicit StateReader(std::vector<u8> data) : buf(std::move(data)) {}
    void bytes(void *out, std::size_t size) {
        if (pos + size > buf.size()) throw std::runtime_error("savestate truncated");
        std::memcpy(out, buf.data() + pos, size);
        pos += size;
    }
    template <typename T>
    T pod() {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        bytes(&value, sizeof(T));
        return value;
    }
    template <typename T>
    void pod(T &value) { value = pod<T>(); }
    std::string str() {
        std::string s(pod<u32>(), '\0');
        bytes(s.data(), s.size());
        return s;
    }
    template <typename T>
    void vec(std::vector<T> &v) {
        v.resize(pod<u32>());
        if (!v.empty()) bytes(v.data(), v.size() * sizeof(T));
    }
    void section(const char *tag) {
        char got[4];
        bytes(got, 4);
        if (std::memcmp(got, tag, 4) != 0) throw std::runtime_error(std::string("savestate section mismatch, expected ") + tag);
    }
    std::vector<u8> buf;
    std::size_t pos{};
};

// Per-module hooks (defined next to each module's state).
void kernel_save(StateWriter &w);
void kernel_load(StateReader &r);
void ge_save(StateWriter &w);
void ge_load(StateReader &r);
void display_save(StateWriter &w);
void display_load(StateReader &r);
void audio_save(StateWriter &w);
void audio_load(StateReader &r);
void sas_save(StateWriter &w);
void sas_load(StateReader &r);
void atrac_save(StateWriter &w);
void atrac_load(StateReader &r);
void io_save(StateWriter &w);
void io_load(StateReader &r);
void utility_save(StateWriter &w);
void utility_load(StateReader &r);
void savedata_save(StateWriter &w);
void savedata_load(StateReader &r);
void mpeg_save(StateWriter &w);
void mpeg_load(StateReader &r);
// Returns a reason the game cannot be saved right now, or empty.
std::string kernel_save_blocker();

// Requested from the UI; performed at the next safe dispatch boundary.
void savestate_request_save(int slot);
void savestate_request_load(int slot);
void savestate_configure(const std::filesystem::path &dir);
// Saves `slot` once the vblank counter reaches `vblank` (for scripted tests).
void savestate_schedule_save(int slot, u64 vblank);
void savestate_schedule_load(int slot, u64 vblank);
// Called from the outer dispatch hook; returns true if a state was loaded.
bool savestate_service();
// True while a save/load is requested or scheduled (cheap; checked after every import).
bool savestate_requested();
// Services a request right after an import returned: the saved thread resumes at ra.
void savestate_service_at_hle_return();
bool savestate_at_hle_return();

} // namespace p2
