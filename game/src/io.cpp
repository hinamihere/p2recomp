// IoFileMgrForUser: disc files come from the extracted ISO tree (or raw ISO
// sectors for sce_lbn paths); ms0: maps to a host directory.
#include "io.hpp"
#include "savestate.hpp"

#include "kernel.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

namespace p2 {
namespace fs = std::filesystem;

namespace {

struct OpenFile {
    std::FILE *file{};
    u64 base{};      // byte offset of this file inside `file` (ISO ranges)
    u64 size{};      // 0 = use the host file size
    u64 position{};
    bool writable{};
    bool is_dir{};
    std::string path;
    s64 async_result{};
    bool async_pending{};
    u32 flags{};
};

IoConfig g_config;
std::map<s32, std::unique_ptr<OpenFile>> g_files;
s32 g_next_fd = 3;

constexpr u32 PSP_O_WRONLY = 0x0002u;
constexpr u32 PSP_O_APPEND = 0x0100u;
constexpr u32 PSP_O_CREAT = 0x0200u;
constexpr u32 PSP_O_TRUNC = 0x0400u;

int fseek64(std::FILE *f, u64 off) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(off), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET);
#endif
}

std::string lower(std::string s) {
    for (char &ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

enum class Device { Disc, MemoryStick, Unknown };

// Splits "dev0:/a/b" into device and a clean relative path (no "..").
Device parse_path(const std::string &psp_path, std::string &relative) {
    const auto colon = psp_path.find(':');
    const std::string device = colon == std::string::npos ? "disc0" : lower(psp_path.substr(0, colon));
    std::string rest = colon == std::string::npos ? psp_path : psp_path.substr(colon + 1);
    std::replace(rest.begin(), rest.end(), '\\', '/');
    fs::path clean;
    for (const auto &part : fs::path(rest)) {
        const std::string p = part.string();
        if (p.empty() || p == "/" || p == ".") continue;
        if (p == "..") { if (!clean.empty()) clean = clean.parent_path(); continue; }
        clean /= part;
    }
    relative = clean.generic_string();
    if (device.starts_with("disc") || device.starts_with("umd") || device.starts_with("host")) return Device::Disc;
    if (device.starts_with("ms") || device.starts_with("fatms")) return Device::MemoryStick;
    return Device::Unknown;
}

fs::path host_path(Device dev, const std::string &relative) {
    return (dev == Device::Disc ? g_config.disc_root : g_config.ms_root) / fs::path(reinterpret_cast<const char8_t *>(relative.c_str()));
}

bool parse_lbn(const std::string &relative, u64 &lbn, u64 &size) {
    // sce_lbn0x<lbn>_size0x<size>
    const std::string r = lower(relative);
    if (!r.starts_with("sce_lbn")) return false;
    unsigned long long a = 0, b = 0;
    if (std::sscanf(r.c_str(), "sce_lbn0x%llx_size0x%llx", &a, &b) != 2 &&
        std::sscanf(r.c_str(), "sce_lbn%llu_size%llu", &a, &b) != 2)
        return false;
    lbn = a;
    size = b;
    return true;
}

OpenFile *file(s32 fd) {
    auto it = g_files.find(fd);
    return it != g_files.end() ? it->second.get() : nullptr;
}

u64 file_size(OpenFile &f) {
    if (f.size != 0u || f.base != 0u) return f.size;
    const auto pos = std::ftell(f.file);
    std::fseek(f.file, 0, SEEK_END);
#if defined(_WIN32)
    const u64 size = static_cast<u64>(_ftelli64(f.file));
#else
    const u64 size = static_cast<u64>(ftello(f.file));
#endif
    std::fseek(f.file, pos, SEEK_SET);
    return size;
}

s32 open_file(const std::string &psp_path, u32 flags) {
    std::string relative;
    const Device dev = parse_path(psp_path, relative);
    auto f = std::make_unique<OpenFile>();
    f->path = psp_path;
    f->flags = flags;
    if (dev == Device::Disc) {
        u64 lbn = 0, size = 0;
        if (parse_lbn(relative, lbn, size)) {
            f->file = _wfopen(g_config.iso_path.wstring().c_str(), L"rb");
            f->base = lbn * 2048u;
            f->size = size;
        } else {
            f->file = _wfopen(host_path(dev, relative).wstring().c_str(), L"rb");
        }
    } else if (dev == Device::MemoryStick) {
        const fs::path p = host_path(dev, relative);
        const bool write = (flags & PSP_O_WRONLY) != 0u;
        if (write) {
            fs::create_directories(p.parent_path());
            const bool truncate = (flags & PSP_O_TRUNC) != 0u || !fs::exists(p);
            if ((flags & PSP_O_CREAT) == 0u && !fs::exists(p)) return static_cast<s32>(err::FILE_NOT_FOUND);
            f->file = _wfopen(p.wstring().c_str(), truncate ? L"w+b" : L"r+b");
            f->writable = true;
            if (f->file != nullptr && (flags & PSP_O_APPEND) != 0u) f->position = file_size(*f);
        } else {
            f->file = _wfopen(p.wstring().c_str(), L"rb");
        }
    }
    if (f->file == nullptr) {
        LOG_I("io", "open '%s' -> not found", psp_path.c_str());
        return static_cast<s32>(err::FILE_NOT_FOUND);
    }
    const s32 fd = g_next_fd++;
    LOG_I("io", "open '%s' flags=%x -> fd %d", psp_path.c_str(), flags, fd);
    g_files[fd] = std::move(f);
    return fd;
}

s64 read_file(OpenFile &f, u32 buffer, u32 size) {
    const u64 total = file_size(f);
    if (f.position >= total) return 0;
    const u32 count = static_cast<u32>(std::min<u64>(size, total - f.position));
    u8 *dst = mem().raw_pointer(buffer, count);
    if (dst == nullptr) return static_cast<s64>(static_cast<s32>(err::ILLEGAL_ADDR));
    fseek64(f.file, f.base + f.position);
    const std::size_t got = std::fread(dst, 1, count, f.file);
    f.position += got;
    return static_cast<s64>(got);
}

s64 seek_file(OpenFile &f, s64 offset, u32 whence) {
    s64 base = 0;
    if (whence == 1) base = static_cast<s64>(f.position);
    else if (whence == 2) base = static_cast<s64>(file_size(f));
    const s64 target = base + offset;
    if (target < 0) return static_cast<s64>(static_cast<s32>(err::ERROR));
    f.position = static_cast<u64>(target);
    return target;
}

// --- imports -------------------------------------------------------------------

void IoOpen(AllegrexContext &c) { ret(c, static_cast<u32>(open_file(guest_string(arg(c, 0)), arg(c, 1)))); }

void IoOpenAsync(AllegrexContext &c) {
    const s32 fd = open_file(guest_string(arg(c, 0)), arg(c, 1));
    if (fd < 0) { ret(c, static_cast<u32>(fd)); return; }
    file(fd)->async_result = fd;
    ret(c, static_cast<u32>(fd));
}

void IoClose(AllegrexContext &c) {
    const s32 fd = static_cast<s32>(arg(c, 0));
    auto it = g_files.find(fd);
    if (it == g_files.end()) { ret(c, err::BADF); return; }
    if (it->second->file != nullptr) std::fclose(it->second->file);
    g_files.erase(it);
    LOG_D("io", "close fd %d", fd);
    ret(c, 0u);
}

void IoRead(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr || f->is_dir) { ret(c, err::BADF); return; }
    ret(c, static_cast<u32>(read_file(*f, arg(c, 1), arg(c, 2))));
}

void IoReadAsync(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr || f->is_dir) { ret(c, err::BADF); return; }
    f->async_result = read_file(*f, arg(c, 1), arg(c, 2));
    LOG_T("io", "readasync fd %d size %x -> %lld", arg(c, 0), arg(c, 2), static_cast<long long>(f->async_result));
    ret(c, 0u);
}

void IoWrite(AllegrexContext &c) {
    const s32 fd = static_cast<s32>(arg(c, 0));
    const u32 buffer = arg(c, 1), size = arg(c, 2);
    if (fd == 1 || fd == 2) {
        std::string text(size, '\0');
        for (u32 i = 0; i < size; ++i) text[i] = static_cast<char>(mem().load8(buffer + i));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        LOG_I("guest", "%s", text.c_str());
        ret(c, size);
        return;
    }
    OpenFile *f = file(fd);
    if (f == nullptr || !f->writable) { ret(c, err::BADF); return; }
    const u8 *src = mem().raw_pointer(buffer, size);
    if (src == nullptr) { ret(c, err::ILLEGAL_ADDR); return; }
    fseek64(f->file, f->base + f->position);
    const std::size_t wrote = std::fwrite(src, 1, size, f->file);
    f->position += wrote;
    ret(c, static_cast<u32>(wrote));
}

void IoLseek(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr) { ret64(c, static_cast<u64>(static_cast<s64>(static_cast<s32>(err::BADF)))); return; }
    ret64(c, static_cast<u64>(seek_file(*f, static_cast<s64>(arg64(c, 2)), arg(c, 4))));
}

void IoLseekAsync(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr) { ret(c, err::BADF); return; }
    f->async_result = seek_file(*f, static_cast<s64>(arg64(c, 2)), arg(c, 4));
    ret(c, 0u);
}

void IoPollAsync(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr) { ret(c, err::BADF); return; }
    if (arg(c, 1) != 0u) write_u64(arg(c, 1), static_cast<u64>(f->async_result));
    ret(c, 0u);
}

void IoWaitAsyncCB(AllegrexContext &c) {
    OpenFile *f = file(static_cast<s32>(arg(c, 0)));
    if (f == nullptr) { ret(c, err::BADF); return; }
    if (arg(c, 1) != 0u) write_u64(arg(c, 1), static_cast<u64>(f->async_result));
    ret(c, 0u);
}

void write_stat(u32 out, bool dir, u64 size) {
    auto &m = mem();
    for (u32 i = 0; i < 0x58u; i += 4u) m.store32(out + i, 0u);
    m.store32(out + 0u, dir ? 0x11FFu : 0x21FFu);
    m.store32(out + 4u, dir ? 0x10u : 0x20u);
    write_u64(out + 8u, size);
}

void IoGetstat(AllegrexContext &c) {
    const std::string psp_path = guest_string(arg(c, 0));
    std::string relative;
    const Device dev = parse_path(psp_path, relative);
    u64 lbn = 0, size = 0;
    if (dev == Device::Disc && parse_lbn(relative, lbn, size)) {
        write_stat(arg(c, 1), false, size);
        ret(c, 0u);
        return;
    }
    std::error_code ec;
    const fs::path p = host_path(dev, relative);
    if (dev == Device::Unknown || !fs::exists(p, ec)) {
        LOG_D("io", "getstat '%s' -> not found", psp_path.c_str());
        ret(c, err::FILE_NOT_FOUND);
        return;
    }
    const bool dir = fs::is_directory(p, ec);
    write_stat(arg(c, 1), dir, dir ? 0u : static_cast<u64>(fs::file_size(p, ec)));
    ret(c, 0u);
}

void IoDopen(AllegrexContext &c) {
    const std::string psp_path = guest_string(arg(c, 0));
    std::string relative;
    const Device dev = parse_path(psp_path, relative);
    std::error_code ec;
    if (dev == Device::Unknown || !fs::is_directory(host_path(dev, relative), ec)) {
        LOG_D("io", "dopen '%s' -> not found", psp_path.c_str());
        ret(c, err::FILE_NOT_FOUND);
        return;
    }
    auto f = std::make_unique<OpenFile>();
    f->is_dir = true;
    f->path = psp_path;
    const s32 fd = g_next_fd++;
    g_files[fd] = std::move(f);
    ret(c, static_cast<u32>(fd));
}

void IoDclose(AllegrexContext &c) {
    ret(c, g_files.erase(static_cast<s32>(arg(c, 0))) != 0u ? 0u : err::BADF);
}

void IoRename(AllegrexContext &c) {
    std::string a, b;
    const Device da = parse_path(guest_string(arg(c, 0)), a);
    const Device db = parse_path(guest_string(arg(c, 1)), b);
    std::error_code ec;
    if (da != Device::MemoryStick || db != Device::MemoryStick) { ret(c, err::ERROR); return; }
    fs::rename(host_path(da, a), host_path(db, b), ec);
    ret(c, ec ? err::FILE_NOT_FOUND : 0u);
}

void IoDevctl(AllegrexContext &c) {
    const std::string dev = guest_string(arg(c, 0));
    const u32 cmd = arg(c, 1), out = arg(c, 4), outlen = arg(c, 5);
    LOG_D("io", "devctl '%s' cmd=%08x in=%08x/%u out=%08x/%u", dev.c_str(), cmd, arg(c, 2), arg(c, 3), out, outlen);
    auto &m = mem();
    switch (cmd) {
    case 0x02025806u: // memory stick inserted
    case 0x02425823u:
        if (out != 0u && outlen >= 4u) m.store32(out, 1u);
        break;
    case 0x02425818u: { // memory stick free space: in = pointer to struct
        const u32 info = arg(c, 2) != 0u ? m.load32(arg(c, 2)) : 0u;
        if (info != 0u) {
            m.store32(info + 0u, 0x10000u);  // max clusters
            m.store32(info + 4u, 0x8000u);   // free clusters
            m.store32(info + 8u, 0x8000u);   // max sectors?
            m.store32(info + 12u, 512u);     // sector size
            m.store32(info + 16u, 64u);      // sectors per cluster
        }
        break;
    }
    case 0x01F20001u: // UMD: disc type
        if (out != 0u && outlen >= 8u) { m.store32(out, 0xFFFFFFFFu); m.store32(out + 4u, 0x10u); }
        break;
    default: break;
    }
    ret(c, 0u);
}

void IoIoctl(AllegrexContext &c) {
    const s32 fd = static_cast<s32>(arg(c, 0));
    const u32 cmd = arg(c, 1), in = arg(c, 2), out = arg(c, 4);
    OpenFile *f = file(fd);
    LOG_D("io", "ioctl fd %d cmd=%08x in=%08x/%u out=%08x/%u", fd, cmd, in, arg(c, 3), out, arg(c, 5));
    if (f == nullptr) { ret(c, err::BADF); return; }
    auto &m = mem();
    switch (cmd) {
    case 0x01020001u: // UMD file: start sector
        if (out != 0u) m.store32(out, static_cast<u32>(f->base / 2048u));
        break;
    case 0x01020004u: // UMD file: seek (in: s64 offset)
        if (in != 0u) f->position = read_u64(in);
        break;
    case 0x01020006u: // UMD file: current position
        if (out != 0u) m.store32(out, static_cast<u32>(f->position));
        break;
    case 0x01020007u: // UMD file: size
        if (out != 0u) write_u64(out, file_size(*f));
        break;
    default: break;
    }
    ret(c, 0u);
}

} // namespace

void io_configure(const IoConfig &config) { g_config = config; }

void register_io_imports() {
    const char *L = "IoFileMgrForUser";
    hle(L, "sceIoOpen", IoOpen);
    hle(L, "sceIoOpenAsync", IoOpenAsync);
    hle(L, "sceIoClose", IoClose);
    hle(L, "sceIoRead", IoRead);
    hle(L, "sceIoReadAsync", IoReadAsync);
    hle(L, "sceIoWrite", IoWrite);
    hle(L, "sceIoLseek", IoLseek);
    hle(L, "sceIoLseekAsync", IoLseekAsync);
    hle(L, "sceIoPollAsync", IoPollAsync);
    hle(L, "sceIoWaitAsyncCB", IoWaitAsyncCB);
    hle(L, "sceIoGetstat", IoGetstat);
    hle(L, "sceIoDopen", IoDopen);
    hle(L, "sceIoDclose", IoDclose);
    hle(L, "sceIoRename", IoRename);
    hle(L, "sceIoDevctl", IoDevctl);
    hle(L, "sceIoIoctl", IoIoctl);
}

void io_save(StateWriter &w) {
    w.section("IO  ");
    w.pod(g_next_fd);
    w.pod(static_cast<u32>(g_files.size()));
    for (const auto &[fd, f] : g_files) {
        w.pod(fd);
        w.str(f->path);
        w.pod(f->flags);
        w.pod(f->is_dir);
        w.pod(f->position);
        w.pod(f->async_result);
        w.pod(f->async_pending);
    }
}

void io_load(StateReader &r) {
    r.section("IO  ");
    for (auto &[fd, f] : g_files)
        if (f->file != nullptr) std::fclose(f->file);
    g_files.clear();
    const s32 next_fd = r.pod<s32>();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        const s32 fd = r.pod<s32>();
        const std::string path = r.str();
        const u32 flags = r.pod<u32>();
        const bool is_dir = r.pod<bool>();
        const u64 position = r.pod<u64>();
        const s64 async_result = r.pod<s64>();
        const bool async_pending = r.pod<bool>();
        std::unique_ptr<OpenFile> f;
        if (is_dir) {
            f = std::make_unique<OpenFile>();
            f->is_dir = true;
            f->path = path;
        } else {
            // Reopen without truncating: the file now holds what was written before the save.
            const s32 tmp = open_file(path, flags & ~(PSP_O_TRUNC | PSP_O_APPEND));
            if (tmp < 0) {
                LOG_W("state", "could not reopen '%s' (fd %d)", path.c_str(), fd);
                continue;
            }
            f = std::move(g_files[tmp]);
            g_files.erase(tmp);
        }
        f->position = position;
        f->async_result = async_result;
        f->async_pending = async_pending;
        g_files[fd] = std::move(f);
    }
    g_next_fd = next_fd;
}

} // namespace p2
