// sceUtilitySavedata: saves live on the host under <ms>/PSP/SAVEDATA/<game><save>/
// as plain (unencrypted) files plus a PARAM.SFO. Dialog modes complete
// immediately; list dialogs pick the most sensible entry automatically.
#include "savedata.hpp"
#include "savestate.hpp"

#include "io.hpp"
#include "kernel.hpp"
#include "overlay.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace p2 {
namespace fs = std::filesystem;
namespace {

std::filesystem::path g_ms_root{"memstick"};

// SceUtilitySavedataParam field offsets.
constexpr u32 kResult = 0x1C, kMode = 0x30, kGameName = 0x3C, kSaveName = 0x4C, kSaveNameList = 0x60,
              kFileName = 0x64, kDataBuf = 0x74, kDataBufSize = 0x78, kDataSize = 0x7C, kSfo = 0x80,
              kIcon0 = 0x584, kIcon1 = 0x594, kPic1 = 0x5A4, kSnd0 = 0x5B4, kFocus = 0x5C8, kMsFree = 0x5D0,
              kMsData = 0x5D4, kUtilityData = 0x5D8, kIdList = 0x5F4, kFileList = 0x5F8, kSizeInfo = 0x5FC;

constexpr u32 ERR_LOAD_NO_DATA = 0x80110307u;
constexpr u32 ERR_LOAD_FILE_NOT_FOUND = 0x80110309u;
constexpr u32 ERR_RW_NO_DATA = 0x80110327u;
constexpr u32 ERR_RW_FILE_NOT_FOUND = 0x80110329u;
constexpr u32 ERR_DELETE_NO_DATA = 0x80110347u;
constexpr u32 ERR_SIZES_NO_DATA = 0x801103C7u;
constexpr u32 ERR_SAVE_PARAM = 0x80110388u;

std::string fixed_string(u32 address, u32 size) {
    std::string out;
    for (u32 i = 0; i < size; ++i) {
        const char ch = static_cast<char>(mem().load8(address + i));
        if (ch == 0) break;
        out.push_back(ch);
    }
    return out;
}

void put_string(u32 address, const std::string &value, u32 size) {
    for (u32 i = 0; i < size; ++i) mem().store8(address + i, i < value.size() ? static_cast<u8>(value[i]) : 0u);
}

std::string safe(std::string s) {
    for (char &ch : s)
        if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
    if (s == "." || s == "..") s = "_";
    return s;
}

fs::path save_dir(const std::string &game, const std::string &save) {
    return g_ms_root / "PSP" / "SAVEDATA" / safe(game + save);
}

std::vector<u8> read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path &p, const u8 *data, std::size_t size) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

// --- PARAM.SFO ------------------------------------------------------------------

struct SfoValue { bool is_int; u32 i; std::string s; u32 max_len; };

std::vector<u8> build_sfo(const std::map<std::string, SfoValue> &values) {
    std::vector<u8> keys, data, index;
    auto put16 = [](std::vector<u8> &v, u16 x) { v.push_back(static_cast<u8>(x)); v.push_back(static_cast<u8>(x >> 8)); };
    auto put32 = [](std::vector<u8> &v, u32 x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i))); };
    for (const auto &[key, value] : values) {
        put16(index, static_cast<u16>(keys.size()));
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        put16(index, value.is_int ? 0x0404 : 0x0204);
        const u32 len = value.is_int ? 4u : static_cast<u32>(value.s.size() + 1);
        const u32 max = value.is_int ? 4u : std::max(value.max_len, (len + 3u) & ~3u);
        put32(index, len);
        put32(index, max);
        put32(index, static_cast<u32>(data.size()));
        if (value.is_int) put32(data, value.i);
        else {
            data.insert(data.end(), value.s.begin(), value.s.end());
            data.resize(data.size() + (max - static_cast<u32>(value.s.size())), 0);
        }
    }
    while (keys.size() % 4u != 0u) keys.push_back(0);
    std::vector<u8> out;
    put32(out, 0x46535000u);
    put32(out, 0x00000101u);
    const u32 key_table = 20u + static_cast<u32>(index.size());
    put32(out, key_table);
    put32(out, key_table + static_cast<u32>(keys.size()));
    put32(out, static_cast<u32>(values.size()));
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::map<std::string, std::string> parse_sfo(const std::vector<u8> &d) {
    std::map<std::string, std::string> out;
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P') return out;
    auto rd32 = [&](std::size_t o) { return static_cast<u32>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24)); };
    const u32 key_table = rd32(8), data_table = rd32(12), count = rd32(16);
    for (u32 i = 0; i < count && 20u + i * 16u + 16u <= d.size(); ++i) {
        const std::size_t e = 20u + i * 16u;
        const u32 key_off = d[e] | (d[e + 1] << 8);
        const u32 len = rd32(e + 4), off = rd32(e + 12);
        std::string key(reinterpret_cast<const char *>(d.data() + key_table + key_off));
        if (data_table + off + len > d.size()) continue;
        std::string value(reinterpret_cast<const char *>(d.data() + data_table + off), len);
        while (!value.empty() && value.back() == '\0') value.pop_back();
        out[key] = value;
    }
    return out;
}

// --- helpers ----------------------------------------------------------------------

struct Request {
    u32 p;
    int mode;
    std::string game, save, file;
};

std::vector<std::string> save_name_list(u32 p) {
    std::vector<std::string> names;
    const u32 list = mem().load32(p + kSaveNameList);
    if (list == 0u) return names;
    for (u32 i = 0; i < 256u; ++i) {
        const std::string name = fixed_string(list + i * 20u, 20u);
        if (name.empty()) break;
        names.push_back(name);
    }
    return names;
}

// Chooses a save for list/dialog modes: latest existing for loads, first free
// (or the requested one) for saves.
std::string choose_save(const Request &r, bool for_load) {
    if (!r.save.empty() && r.save != "<>") return r.save;
    const auto names = save_name_list(r.p);
    std::string best;
    fs::file_time_type best_time{};
    for (const auto &name : names) {
        std::error_code ec;
        const fs::path dir = save_dir(r.game, name);
        const bool exists = fs::exists(dir / r.file, ec);
        if (for_load && exists) {
            const auto t = fs::last_write_time(dir / r.file, ec);
            if (best.empty() || t > best_time) { best = name; best_time = t; }
        } else if (!for_load && !exists && best.empty()) {
            best = name;
        }
    }
    if (best.empty() && !names.empty()) best = names.front();
    return best;
}

void write_file_data(u32 p, u32 field, const fs::path &path) {
    auto &m = mem();
    const u32 buf = m.load32(p + field), size = m.load32(p + field + 8u);
    if (buf == 0u || size == 0u) return;
    if (const u8 *src = m.raw_pointer(buf, size)) write_file(path, src, size);
}

void read_file_data(u32 p, u32 field, const fs::path &path) {
    auto &m = mem();
    const u32 buf = m.load32(p + field), buf_size = m.load32(p + field + 4u);
    if (buf == 0u || buf_size == 0u) return;
    std::error_code ec;
    if (!fs::exists(path, ec)) { m.store32(p + field + 8u, 0u); return; }
    const auto data = read_file(path);
    const u32 n = std::min<u32>(buf_size, static_cast<u32>(data.size()));
    if (u8 *dst = m.raw_pointer(buf, n)) std::memcpy(dst, data.data(), n);
    m.store32(p + field + 8u, n);
}

u32 do_save(const Request &r, const std::string &save) {
    auto &m = mem();
    if (save.empty()) return ERR_SAVE_PARAM;
    const fs::path dir = save_dir(r.game, save);
    const u32 buf = m.load32(r.p + kDataBuf), size = m.load32(r.p + kDataSize);
    if (buf != 0u && !r.file.empty()) {
        const u8 *src = m.raw_pointer(buf, size);
        if (src == nullptr || !write_file(dir / r.file, src, size)) return 0x80110385u;
    }
    const u32 sfo = r.p + kSfo;
    std::map<std::string, SfoValue> values;
    values["CATEGORY"] = {false, 0, "MS", 4};
    values["PARENTAL_LEVEL"] = {true, m.load8(sfo + 0x500u), "", 4};
    values["SAVEDATA_DETAIL"] = {false, 0, fixed_string(sfo + 0x100u, 0x400u), 0x400};
    values["SAVEDATA_DIRECTORY"] = {false, 0, r.game + save, 0x40};
    values["SAVEDATA_TITLE"] = {false, 0, fixed_string(sfo + 0x80u, 0x80u), 0x80};
    values["TITLE"] = {false, 0, fixed_string(sfo, 0x80u), 0x80};
    const auto sfo_bytes = build_sfo(values);
    write_file(dir / "PARAM.SFO", sfo_bytes.data(), sfo_bytes.size());
    write_file_data(r.p, kIcon0, dir / "ICON0.PNG");
    write_file_data(r.p, kIcon1, dir / "ICON1.PMF");
    write_file_data(r.p, kPic1, dir / "PIC1.PNG");
    write_file_data(r.p, kSnd0, dir / "SND0.AT3");
    put_string(r.p + kSaveName, save, 20u);
    LOG_I("savedata", "saved %s/%s (%u bytes)", (r.game + save).c_str(), r.file.c_str(), size);
    return 0u;
}

u32 do_load(const Request &r, const std::string &save, bool rw_errors) {
    auto &m = mem();
    const fs::path dir = save_dir(r.game, save);
    std::error_code ec;
    if (save.empty() || !fs::exists(dir, ec)) return rw_errors ? ERR_RW_NO_DATA : ERR_LOAD_NO_DATA;
    if (!r.file.empty() && !fs::exists(dir / r.file, ec)) return rw_errors ? ERR_RW_FILE_NOT_FOUND : ERR_LOAD_FILE_NOT_FOUND;
    if (!r.file.empty()) {
        const auto data = read_file(dir / r.file);
        const u32 buf = m.load32(r.p + kDataBuf), buf_size = m.load32(r.p + kDataBufSize);
        const u32 n = std::min<u32>(buf_size, static_cast<u32>(data.size()));
        if (buf != 0u && n != 0u)
            if (u8 *dst = m.raw_pointer(buf, n)) std::memcpy(dst, data.data(), n);
        m.store32(r.p + kDataSize, n);
    }
    const auto sfo = parse_sfo(read_file(dir / "PARAM.SFO"));
    if (auto it = sfo.find("TITLE"); it != sfo.end()) put_string(r.p + kSfo, it->second, 0x80u);
    if (auto it = sfo.find("SAVEDATA_TITLE"); it != sfo.end()) put_string(r.p + kSfo + 0x80u, it->second, 0x80u);
    if (auto it = sfo.find("SAVEDATA_DETAIL"); it != sfo.end()) put_string(r.p + kSfo + 0x100u, it->second, 0x400u);
    read_file_data(r.p, kIcon0, dir / "ICON0.PNG");
    read_file_data(r.p, kIcon1, dir / "ICON1.PMF");
    read_file_data(r.p, kPic1, dir / "PIC1.PNG");
    read_file_data(r.p, kSnd0, dir / "SND0.AT3");
    put_string(r.p + kSaveName, save, 20u);
    LOG_I("savedata", "loaded %s/%s", (r.game + save).c_str(), r.file.c_str());
    return 0u;
}

u64 dir_size(const fs::path &dir) {
    u64 total = 0;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

void write_used_info(u32 out, u64 bytes) {
    auto &m = mem();
    const u32 clusters = static_cast<u32>((bytes + 32767u) / 32768u);
    const u32 kb = clusters * 32u;
    m.store32(out + 0u, clusters);
    m.store32(out + 4u, kb);
    put_string(out + 8u, std::to_string(kb) + " KB", 8u);
    m.store32(out + 16u, kb);
    put_string(out + 20u, std::to_string(kb) + " KB", 8u);
}

u32 do_sizes(const Request &r) {
    auto &m = mem();
    const u32 free_info = m.load32(r.p + kMsFree);
    if (free_info != 0u) {
        m.store32(free_info + 0u, 32768u);
        m.store32(free_info + 4u, 0x8000u);
        m.store32(free_info + 8u, 0x8000u * 32u);
        put_string(free_info + 12u, "1 GB", 8u);
    }
    u32 result = 0u;
    const u32 ms_data = m.load32(r.p + kMsData);
    if (ms_data != 0u) {
        const std::string game = fixed_string(ms_data, 13u), save = fixed_string(ms_data + 16u, 20u);
        const fs::path dir = save_dir(game, save);
        std::error_code ec;
        if (fs::exists(dir, ec)) write_used_info(ms_data + 36u, dir_size(dir));
        else { write_used_info(ms_data + 36u, 0u); result = ERR_SIZES_NO_DATA; }
    }
    const u32 utility = m.load32(r.p + kUtilityData);
    if (utility != 0u) {
        u64 bytes = m.load32(r.p + kDataSize) + 0x1000u;
        for (u32 f : {kIcon0, kIcon1, kPic1, kSnd0}) bytes += m.load32(r.p + f + 8u);
        write_used_info(utility, bytes);
    }
    return result;
}

void write_datetime(u32 out, fs::file_time_type t) {
    using namespace std::chrono;
    const auto sys = time_point_cast<seconds>(clock_cast<system_clock>(t));
    const std::time_t tt = system_clock::to_time_t(sys);
    std::tm tm{};
    localtime_s(&tm, &tt);
    auto &m = mem();
    m.store16(out + 0u, static_cast<u16>(tm.tm_year + 1900));
    m.store16(out + 2u, static_cast<u16>(tm.tm_mon + 1));
    m.store16(out + 4u, static_cast<u16>(tm.tm_mday));
    m.store16(out + 6u, static_cast<u16>(tm.tm_hour));
    m.store16(out + 8u, static_cast<u16>(tm.tm_min));
    m.store16(out + 10u, static_cast<u16>(tm.tm_sec));
    m.store32(out + 12u, 0u);
}

u32 do_list(const Request &r) {
    auto &m = mem();
    const u32 list = m.load32(r.p + kIdList);
    if (list == 0u) return 0u;
    const u32 max = m.load32(list), entries = m.load32(list + 8u);
    // saveName may contain '*' wildcard; match on prefix gameName + prefix of saveName before '*'.
    std::string prefix = r.save;
    if (const auto star = prefix.find('*'); star != std::string::npos) prefix = prefix.substr(0, star);
    u32 count = 0;
    std::error_code ec;
    const fs::path root = g_ms_root / "PSP" / "SAVEDATA";
    for (const auto &e : fs::directory_iterator(root, ec)) {
        if (count >= max || !e.is_directory(ec)) continue;
        const std::string dirname = e.path().filename().string();
        if (dirname.rfind(r.game + prefix, 0) != 0) continue;
        const u32 out = entries + count * 72u;
        m.store32(out, 0x11FFu);
        const auto t = fs::last_write_time(e.path(), ec);
        write_datetime(out + 4u, t);
        write_datetime(out + 20u, t);
        write_datetime(out + 36u, t);
        put_string(out + 52u, dirname.substr(r.game.size()), 20u);
        ++count;
    }
    m.store32(list + 4u, count);
    return 0u;
}

u32 do_files(const Request &r) {
    auto &m = mem();
    const u32 info = m.load32(r.p + kFileList);
    if (info == 0u) return 0u;
    const fs::path dir = save_dir(r.game, r.save);
    std::error_code ec;
    if (!fs::exists(dir, ec)) return ERR_RW_NO_DATA;
    const u32 max_normal = m.load32(info + 4u), normal = m.load32(info + 28u);
    u32 count = 0;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name == "PARAM.SFO" || name == "ICON0.PNG" || name == "ICON1.PMF" || name == "PIC1.PNG" || name == "SND0.AT3") continue;
        if (count >= max_normal || normal == 0u) { ++count; continue; }
        const u32 out = normal + count * 80u;
        m.store32(out, 0x21FFu);
        m.store32(out + 4u, 0u);
        write_u64(out + 8u, e.file_size(ec));
        const auto t = e.last_write_time(ec);
        write_datetime(out + 16u, t);
        write_datetime(out + 32u, t);
        write_datetime(out + 48u, t);
        put_string(out + 64u, name, 16u);
        ++count;
    }
    m.store32(info + 12u, 0u);
    m.store32(info + 16u, std::min(count, max_normal));
    m.store32(info + 20u, 0u);
    return 0u;
}

u32 do_delete(const Request &r, const std::string &save) {
    const fs::path dir = save_dir(r.game, save);
    std::error_code ec;
    if (save.empty() || !fs::exists(dir, ec)) return ERR_DELETE_NO_DATA;
    fs::remove_all(dir, ec);
    LOG_I("savedata", "deleted %s", (r.game + save).c_str());
    return 0u;
}

u32 execute(u32 p) {
    auto &m = mem();
    Request r{p, static_cast<int>(m.load32(p + kMode)), fixed_string(p + kGameName, 13u), fixed_string(p + kSaveName, 20u),
              fixed_string(p + kFileName, 13u)};
    LOG_I("savedata", "mode %d game=%s save=%s file=%s focus=%u list=%u", r.mode, r.game.c_str(), r.save.c_str(), r.file.c_str(),
          m.load32(p + kFocus), static_cast<unsigned>(save_name_list(p).size()));
    switch (r.mode) {
    case 0: case 2: case 4: return do_load(r, choose_save(r, true), false);       // auto/dialog/list load
    case 1: case 3: case 5: return do_save(r, choose_save(r, false));             // auto/dialog/list save
    case 6: case 7: return do_delete(r, choose_save(r, true));                    // list/single delete
    case 8: return do_sizes(r);
    case 11: return do_list(r);
    case 12: return do_files(r);
    case 13: case 14: case 17: case 18: return do_save(r, r.save);               // make/write data
    case 15: case 16: return do_load(r, r.save, true);                           // read data
    case 19: case 20: {                                                           // erase one file
        std::error_code ec;
        const fs::path f = save_dir(r.game, r.save) / r.file;
        if (!fs::exists(f, ec)) return ERR_RW_FILE_NOT_FOUND;
        fs::remove(f, ec);
        return 0u;
    }
    case 21: return do_delete(r, r.save);
    case 22: {                                                                    // getsize
        const u32 info = m.load32(p + kSizeInfo);
        if (info != 0u) {
            // PspUtilitySavedataSizeInfo: numSecure, numNormal, secure*, normal*, sectorSize, freeSectors, freeKB, freeString[8], neededKB, neededString[8], overwriteKB, overwriteString[8]
            m.store32(info + 16u, 0x8000u);
            m.store32(info + 20u, 0x8000u);
            m.store32(info + 24u, 0x8000u * 32u);
            put_string(info + 28u, "1 GB", 8u);
        }
        return 0u;
    }
    default:
        LOG_W("savedata", "unsupported mode %d", r.mode);
        return 0u;
    }
}

// Utility dialog status machine: 1 init -> 2 running -> 3 finished; shutdown -> 4 -> 0.
struct Dialog { u32 status{}; u32 param{}; };
Dialog g_dialog;

// --- save slot picker (list load/save/delete) -------------------------------------
//
// The PSP shows a system dialog listing the offered slots for these modes. It is drawn
// over the presented frame (savedata_dialog_draw) and driven by the controller once per
// vblank (savedata_dialog_input); the game sees no buttons while it is open.

struct Slot {
    std::string name;
    bool exists{};
    std::string title, detail, date;
    fs::file_time_type time{};
};

struct Picker {
    bool open{};
    int mode{};
    std::vector<Slot> slots;
    int cursor{}, top{};
    bool confirm{};      // asking "overwrite?" / "delete?"
    u32 prev_buttons{};
    u64 held_since{}, last_repeat{};
};
Picker g_picker;

constexpr int kRows = 6;

bool picker_mode(int mode) { return mode == 4 || mode == 5 || mode == 6; }
bool picker_saves() { return g_picker.mode == 5; }
bool picker_loads() { return g_picker.mode == 4; }

std::string format_time(fs::file_time_type t) {
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sys);
    char buf[32] = "";
    if (const std::tm *lt = std::localtime(&tt)) std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", lt);
    return buf;
}

void open_picker(u32 p, int mode) {
    Picker &pk = g_picker;
    pk = Picker{};
    pk.open = true;
    pk.mode = mode;
    const std::string game = fixed_string(p + kGameName, 13u), file = fixed_string(p + kFileName, 13u);
    for (const auto &name : save_name_list(p)) {
        Slot s;
        s.name = name;
        const fs::path dir = save_dir(game, name);
        std::error_code ec;
        s.exists = fs::exists(dir / (file.empty() ? "PARAM.SFO" : file), ec);
        if (s.exists) {
            const auto sfo = parse_sfo(read_file(dir / "PARAM.SFO"));
            if (auto it = sfo.find("SAVEDATA_TITLE"); it != sfo.end()) s.title = it->second;
            if (auto it = sfo.find("SAVEDATA_DETAIL"); it != sfo.end()) s.detail = it->second;
            s.time = fs::last_write_time(dir / "PARAM.SFO", ec);
            if (!ec) s.date = format_time(s.time);
        }
        pk.slots.push_back(std::move(s));
    }
    if (pk.slots.empty()) {
        Slot s;
        s.name = fixed_string(p + kSaveName, 20u);
        pk.slots.push_back(s);
    }
    // Initial focus: the newest save; when saving with no saves yet, the first slot.
    int newest = -1;
    for (int i = 0; i < static_cast<int>(pk.slots.size()); ++i)
        if (pk.slots[i].exists && (newest < 0 || pk.slots[i].time > pk.slots[newest].time)) newest = i;
    pk.cursor = std::max(newest, 0);
    pk.top = std::max(0, pk.cursor - kRows / 2);
    LOG_I("savedata", "slot picker open (mode %d, %u slots)", mode, static_cast<unsigned>(pk.slots.size()));
}

void close_picker(u32 result, const std::string &save) {
    auto &m = mem();
    if (!save.empty()) put_string(g_dialog.param + kSaveName, save, 20u);
    m.store32(g_dialog.param + kResult, result);
    g_picker.open = false;
    g_dialog.status = 3u;
    LOG_I("savedata", "slot picker closed: %s result %08x", save.empty() ? "cancelled" : save.c_str(), result);
}

void picker_confirm() {
    Picker &pk = g_picker;
    const Slot &s = pk.slots[static_cast<std::size_t>(pk.cursor)];
    if ((picker_loads() || pk.mode == 6) && !s.exists) return; // nothing to load/delete here
    if (!pk.confirm && (pk.mode == 6 || (picker_saves() && s.exists))) {
        pk.confirm = true; // ask before overwriting or deleting
        return;
    }
    // Run the request on the chosen slot.
    put_string(g_dialog.param + kSaveName, s.name, 20u);
    const u32 result = execute(g_dialog.param);
    close_picker(result, s.name);
}

void SavedataInitStart(AllegrexContext &c) {
    g_dialog.param = arg(c, 0);
    g_dialog.status = 1u;
    const int mode = static_cast<int>(mem().load32(g_dialog.param + kMode));
    static const bool auto_pick = std::getenv("P2_SAVEDATA_AUTO") != nullptr;
    if (picker_mode(mode) && !auto_pick) {
        open_picker(g_dialog.param, mode);
        ret(c, 0u);
        return;
    }
    const u32 result = execute(g_dialog.param);
    mem().store32(g_dialog.param + kResult, result);
    if (result != 0u) LOG_I("savedata", "result %08x", result);
    ret(c, 0u);
}

void SavedataGetStatus(AllegrexContext &c) {
    const u32 s = g_dialog.status;
    if (g_dialog.status == 1u) g_dialog.status = 2u;
    else if (g_dialog.status == 2u && !g_picker.open) g_dialog.status = 3u;
    else if (g_dialog.status == 4u) g_dialog.status = 0u;
    ret(c, s);
}

void SavedataUpdate(AllegrexContext &c) { ret(c, 0u); }
void SavedataShutdownStart(AllegrexContext &c) { g_dialog.status = 4u; ret(c, 0u); }

} // namespace

void savedata_configure(const std::filesystem::path &ms_root) { g_ms_root = ms_root; }

bool savedata_dialog_active() { return g_picker.open; }

void savedata_dialog_input(u32 buttons) {
    Picker &pk = g_picker;
    if (!pk.open) return;
    constexpr u32 kUp = 0x0010u, kRight = 0x0020u, kDown = 0x0040u, kLeft = 0x0080u, kCircle = 0x2000u, kCross = 0x4000u;
    const u32 pressed = buttons & ~pk.prev_buttons;
    if (buttons != pk.prev_buttons) LOG_D("savedata", "picker buttons %04x (pressed %04x)", buttons, pressed);
    pk.prev_buttons = buttons;
    // Held directions repeat after 20 vblanks, every 4.
    const u64 now = g_kernel.vblank_count();
    u32 dirs = pressed & (kUp | kDown | kLeft | kRight);
    if ((buttons & (kUp | kDown | kLeft | kRight)) != 0u) {
        if (pressed & (kUp | kDown | kLeft | kRight)) pk.held_since = now;
        else if (now - pk.held_since > 20u && now - pk.last_repeat >= 4u) {
            dirs = buttons & (kUp | kDown | kLeft | kRight);
            pk.last_repeat = now;
        }
    }
    const int n = static_cast<int>(pk.slots.size());
    int move = 0;
    if (dirs & kUp) move = -1;
    if (dirs & kDown) move = 1;
    if (dirs & kLeft) move = -kRows;
    if (dirs & kRight) move = kRows;
    if (move != 0) {
        pk.confirm = false;
        pk.cursor = std::clamp(pk.cursor + move, 0, n - 1);
        if (pk.cursor < pk.top) pk.top = pk.cursor;
        if (pk.cursor >= pk.top + kRows) pk.top = pk.cursor - kRows + 1;
    }
    if (pressed & kCircle) {
        if (pk.confirm) pk.confirm = false;
        else close_picker(1u, {}); // SCE_UTILITY_DIALOG_RESULT_CANCEL
    } else if (pressed & kCross) {
        picker_confirm();
    }
}

void savedata_dialog_draw(u32 *frame) {
    using namespace overlay;
    const Picker &pk = g_picker;
    if (!pk.open) return;
    const u32 white = rgb(240, 240, 240), grey = rgb(150, 150, 150), accent = rgb(230, 60, 60), panel = rgb(16, 16, 24);
    dim(frame, 90);
    const char *title = pk.mode == 5 ? "Save" : pk.mode == 4 ? "Load" : "Delete";
    fill(frame, 0, 0, kWidth, 22, rgb(110, 12, 16));
    text(frame, 12, 7, title, white);
    char count[32];
    std::snprintf(count, sizeof(count), "%d / %d", pk.cursor + 1, static_cast<int>(pk.slots.size()));
    text(frame, kWidth - 12 - static_cast<int>(std::strlen(count)) * 8, 7, count, grey);
    // Rows: two lines per slot plus a separator.
    const int row_h = 38, list_y = 28;
    for (int r = 0; r < kRows; ++r) {
        const int i = pk.top + r;
        if (i >= static_cast<int>(pk.slots.size())) break;
        const Slot &s = pk.slots[static_cast<std::size_t>(i)];
        const int y = list_y + r * row_h;
        const bool sel = i == pk.cursor;
        fill(frame, 8, y, kWidth - 16, row_h - 4, sel ? rgb(70, 20, 24) : panel, 220);
        if (sel) frame_rect(frame, 8, y, kWidth - 16, row_h - 4, accent);
        char label[16];
        std::snprintf(label, sizeof(label), "%02d", i + 1);
        text(frame, 16, y + 6, label, sel ? accent : grey);
        if (!s.exists) {
            text(frame, 48, y + 6, "New Save Data", sel ? white : grey);
            continue;
        }
        // SAVEDATA_DETAIL has up to three lines: name/level, location, play time.
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= s.detail.size()) {
            const std::size_t nl = s.detail.find('\n', start);
            lines.push_back(s.detail.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        text(frame, 48, y + 6, lines.empty() || lines[0].empty() ? s.title : lines[0], white, 34);
        text(frame, kWidth - 16 - 16 * 8, y + 6, s.date, grey, 16);
        std::string second;
        for (std::size_t k = 1; k < lines.size(); ++k) second += (k > 1 ? "   " : "") + lines[k];
        text(frame, 48, y + 19, second, grey, 51);
    }
    // Footer.
    fill(frame, 0, kHeight - 18, kWidth, 18, rgb(30, 30, 38));
    std::string hint;
    if (pk.confirm) hint = pk.mode == 6 ? "Delete this save?  X: Yes   O: No" : "Overwrite this save?  X: Yes   O: No";
    else hint = "Up/Down: Select  Left/Right: Page  X: Confirm  O: Back";
    text(frame, 12, kHeight - 13, hint, pk.confirm ? accent : white);
}

void register_savedata_imports() {
    const char *U = "sceUtility";
    hle(U, "sceUtilitySavedataInitStart", SavedataInitStart);
    hle(U, "sceUtilitySavedataGetStatus", SavedataGetStatus);
    hle(U, "sceUtilitySavedataUpdate", SavedataUpdate);
    hle(U, "sceUtilitySavedataShutdownStart", SavedataShutdownStart);
}

std::string savedata_save_blocker() { return {}; }

void savedata_save(StateWriter &w) {
    w.section("SAVE");
    w.pod(g_dialog);
    // The picker's slot list is rebuilt from the memory stick on load.
    w.pod(g_picker.open);
    w.pod(g_picker.mode);
    w.pod(g_picker.cursor);
    w.pod(g_picker.top);
    w.pod(g_picker.confirm);
}

void savedata_load(StateReader &r) {
    r.section("SAVE");
    r.pod(g_dialog);
    g_picker = Picker{};
    if (savestate_load_version() < 5u) return;
    bool open = false;
    int mode = 0, cursor = 0, top = 0;
    bool confirm = false;
    r.pod(open); r.pod(mode); r.pod(cursor); r.pod(top); r.pod(confirm);
    if (open) {
        open_picker(g_dialog.param, mode);
        g_picker.cursor = std::clamp(cursor, 0, static_cast<int>(g_picker.slots.size()) - 1);
        g_picker.top = top;
        g_picker.confirm = confirm;
        g_picker.prev_buttons = 0xFFFFFFFFu; // ignore buttons still held from before the load
    }
}

} // namespace p2
