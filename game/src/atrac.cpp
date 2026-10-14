// sceAtrac3plus: ATRAC3+ playback from guest buffers, decoded with the
// standalone FFmpeg-derived decoder.
//
// Streaming model: the guest buffer is a byte ring. GetStreamDataInfo tells
// the game where to write the next bytes and which file offset they come from;
// frames are consumed sequentially from the ring. Seeking (loops, resets)
// empties the ring and asks the game for data from the new position.
#include "atrac.hpp"
#include "savestate.hpp"

#include "kernel.hpp"
#include "at3_decoders.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

namespace p2 {
namespace {

constexpr u32 ERR_NO_ATRACID = 0x80630003u;
constexpr u32 ERR_BAD_ATRACID = 0x80630005u;
constexpr u32 ERR_UNKNOWN_FORMAT = 0x80630006u;
constexpr u32 ERR_ALL_DATA_LOADED = 0x80630009u;
constexpr u32 ERR_NO_DATA = 0x80630010u;
constexpr u32 ERR_ADD_DATA_IS_TOO_BIG = 0x80630018u;
constexpr u32 ERR_NO_LOOP_INFORMATION = 0x80630021u;
constexpr u32 ERR_SECOND_BUFFER_NOT_NEEDED = 0x80630022u;
constexpr u32 ERR_BUFFER_IS_EMPTY = 0x80630023u;
constexpr u32 ERR_ALL_DATA_DECODED = 0x80630024u;
constexpr u32 ERR_BAD_SAMPLE = 0x80630015u;

constexpr int kAt3Plus = 0x1000, kAt3 = 0x1001;

struct Atrac {
    bool used{};
    int codec{};
    int channels{2};
    int block_align{};
    int samples_per_frame{2048};
    u32 data_off{}, data_size{}, file_size{};
    int first_sample_offset{}; // from the fact chunk
    int end_sample{};          // last sample index (inclusive), output coordinates
    int loop_start{-1}, loop_end{-1}; // output coordinates, inclusive end
    int loop_num{};
    int current_sample{};

    u32 buffer{}, buffer_size{};
    bool all_loaded{};
    // Streaming ring state.
    u32 read_pos{}, write_pos{}, avail{};
    u32 file_write{}; // next file offset the game should load
    int ring_frame{}; // frame index at read_pos
    int warmup{};     // frames to decode and discard after a seek

    ATRAC3PContext *ctx3p{};
    ATRAC3Context *ctx3{};
    std::vector<u8> frame_buf;
    std::vector<u8> extra; // ATRAC3 codec extradata (fmt chunk)
    std::vector<float> pcm[2];

    int offset_full() const { return (codec == kAt3Plus ? 0x170 : 0x45) + first_sample_offset; }
    u32 frame_file_offset(int frame) const { return data_off + static_cast<u32>(frame) * static_cast<u32>(block_align); }
    int frames_total() const { return static_cast<int>(data_size / static_cast<u32>(block_align)); }

    void free_decoder() {
        if (ctx3p != nullptr) atrac3p_free(ctx3p);
        if (ctx3 != nullptr) atrac3_free(ctx3);
        ctx3p = nullptr;
        ctx3 = nullptr;
    }
};

std::array<Atrac, 6> g_atrac;

u32 rd32(const u8 *p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }
u16 rd16(const u8 *p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

u32 parse_riff(Atrac &a, const u8 *buf, u32 size) {
    if (size < 12 || std::memcmp(buf, "RIFF", 4) != 0 || std::memcmp(buf + 8, "WAVE", 4) != 0) return ERR_UNKNOWN_FORMAT;
    a.file_size = rd32(buf + 4) + 8u;
    u32 off = 12;
    bool have_data = false;
    std::vector<u8> extra;
    int fact_end = -1;
    while (off + 8 <= size) {
        const u32 id = rd32(buf + off), len = rd32(buf + off + 4);
        off += 8;
        if (id == 0x20746D66u) { // "fmt "
            const u16 tag = rd16(buf + off);
            a.channels = rd16(buf + off + 2);
            a.block_align = rd16(buf + off + 12);
            if (tag == 0xFFFEu) a.codec = kAt3Plus;
            else if (tag == 0x270u) a.codec = kAt3;
            else return ERR_UNKNOWN_FORMAT;
            if (len > 18 && off + len <= size) extra.assign(buf + off + 18, buf + off + len);
        } else if (id == 0x74636166u) { // "fact"
            fact_end = static_cast<int>(rd32(buf + off));
            if (len >= 8) a.first_sample_offset = static_cast<int>(rd32(buf + off + 4));
        } else if (id == 0x6C706D73u) { // "smpl"
            if (len >= 36 && off + 36 + 24 <= size && rd32(buf + off + 28) > 0u) {
                // smpl loop points minus the fact offset give output-sample coordinates.
                a.loop_start = static_cast<int>(rd32(buf + off + 36 + 8));
                a.loop_end = static_cast<int>(rd32(buf + off + 36 + 12));
            }
        } else if (id == 0x61746164u) { // "data"
            a.data_off = off;
            a.data_size = len;
            have_data = true;
            break;
        }
        off += len;
    }
    if (!have_data || a.codec == 0 || a.block_align <= 0) return ERR_UNKNOWN_FORMAT;
    a.samples_per_frame = a.codec == kAt3Plus ? 2048 : 1024;
    if (fact_end > 0) a.end_sample = fact_end - 1;
    else a.end_sample = a.frames_total() * a.samples_per_frame - a.offset_full() - 1;
    if (a.loop_start >= 0) {
        a.loop_start = a.loop_start - a.first_sample_offset;
        a.loop_end = a.loop_end - a.first_sample_offset;
        // Some tracks place the loop end a few samples past the last sample; the
        // loop must still trigger at the real end instead of finishing the track.
        a.loop_end = std::min(a.loop_end, a.end_sample);
    }
    if (a.codec == kAt3Plus) {
        int align = a.block_align;
        a.ctx3p = atrac3p_alloc(a.channels, &align);
    } else {
        int align = a.block_align;
        a.ctx3 = atrac3_alloc(a.channels, &align, extra.data(), static_cast<int>(extra.size()));
    }
    a.extra = extra;
    if (a.ctx3p == nullptr && a.ctx3 == nullptr) return ERR_UNKNOWN_FORMAT;
    return 0u;
}

Atrac *get(u32 id) {
    if (id >= g_atrac.size() || !g_atrac[id].used) return nullptr;
    return &g_atrac[id];
}

// Empties the ring and positions the stream so the frame containing `sample`
// (and one warm-up frame before it) is requested next.
void seek_stream(Atrac &a, int sample) {
    const int frame = (sample + a.offset_full()) / a.samples_per_frame;
    const int start = std::max(frame - 1, 0);
    a.read_pos = a.write_pos = 0u;
    a.avail = 0u;
    a.ring_frame = start;
    a.file_write = a.frame_file_offset(start);
    a.warmup = frame - start;
    a.current_sample = sample;
    if (a.ctx3p != nullptr) atrac3p_flush_buffers(a.ctx3p);
    if (a.ctx3 != nullptr) atrac3_flush_buffers(a.ctx3);
}

// Looping streams are fed as a virtual sequence: frames up to the loop-end frame,
// then again from one frame before the loop start (a decoder warm-up frame), so
// the ring never runs dry at the loop point.
bool stream_loops(const Atrac &a) { return !a.all_loaded && a.loop_num != 0 && a.loop_start >= 0; }
int loop_end_frame(const Atrac &a) { return (a.loop_end + a.offset_full()) / a.samples_per_frame; }
int loop_restart_frame(const Atrac &a) { return std::max((a.loop_start + a.offset_full()) / a.samples_per_frame - 1, 0); }

u32 segment_end(const Atrac &a) {
    const u32 data_end = a.data_off + a.data_size;
    if (!stream_loops(a)) return data_end;
    return std::min(data_end, a.frame_file_offset(loop_end_frame(a) + 1));
}

int next_frame(const Atrac &a, int frame) {
    return stream_loops(a) && frame == loop_end_frame(a) ? loop_restart_frame(a) : frame + 1;
}

// Wraps the file read position back to the loop when the segment is fully requested.
void wrap_file_write(Atrac &a) {
    if (stream_loops(a) && a.file_write >= segment_end(a)) a.file_write = a.frame_file_offset(loop_restart_frame(a));
}

int remaining_frames(const Atrac &a) {
    if (a.all_loaded) return -1;
    const u32 data_end = a.data_off + a.data_size;
    if (!stream_loops(a) && a.file_write >= data_end) return -2;
    return static_cast<int>(a.avail / static_cast<u32>(a.block_align));
}

// Fetches the bytes of `frame` into a.frame_buf. Returns false if unavailable.
bool fetch_frame(Atrac &a, int frame) {
    auto &m = mem();
    a.frame_buf.resize(static_cast<std::size_t>(a.block_align));
    if (a.all_loaded) {
        const u32 off = a.frame_file_offset(frame);
        if (frame >= a.frames_total() || off + static_cast<u32>(a.block_align) > a.buffer_size) return false;
        const u8 *p = m.raw_pointer(a.buffer + off, static_cast<u32>(a.block_align));
        if (p == nullptr) return false;
        std::memcpy(a.frame_buf.data(), p, a.frame_buf.size());
        return true;
    }
    // Discard frames before the one we need (bounded by what is buffered).
    while (a.ring_frame != frame && a.avail >= static_cast<u32>(a.block_align)) {
        a.read_pos = (a.read_pos + static_cast<u32>(a.block_align)) % a.buffer_size;
        a.avail -= static_cast<u32>(a.block_align);
        a.ring_frame = next_frame(a, a.ring_frame);
    }
    if (a.ring_frame != frame || a.avail < static_cast<u32>(a.block_align)) return false;
    for (int i = 0; i < a.block_align; ++i)
        a.frame_buf[static_cast<std::size_t>(i)] = m.load8(a.buffer + (a.read_pos + static_cast<u32>(i)) % a.buffer_size);
    a.read_pos = (a.read_pos + static_cast<u32>(a.block_align)) % a.buffer_size;
    a.avail -= static_cast<u32>(a.block_align);
    a.ring_frame = next_frame(a, a.ring_frame);
    return true;
}

int decode_frame(Atrac &a, int frame) {
    if (!fetch_frame(a, frame)) return -1;
    float *out[2];
    for (int ch = 0; ch < 2; ++ch) {
        a.pcm[ch].resize(static_cast<std::size_t>(a.samples_per_frame));
        out[ch] = a.pcm[ch].data();
    }
    int nb = 0;
    int r;
    if (a.ctx3p != nullptr) r = atrac3p_decode_frame(a.ctx3p, out, &nb, a.frame_buf.data(), a.block_align);
    else r = atrac3_decode_frame(a.ctx3, out, &nb, a.frame_buf.data(), a.block_align);
    if (r < 0) {
        LOG_W("atrac", "frame %d decode error %d", frame, r);
        std::fill(a.pcm[0].begin(), a.pcm[0].end(), 0.f);
        std::fill(a.pcm[1].begin(), a.pcm[1].end(), 0.f);
    }
    if (a.channels == 1) a.pcm[1] = a.pcm[0];
    return a.samples_per_frame;
}

// --- imports --------------------------------------------------------------------

u32 set_data(u32 buffer, u32 size) {
    for (u32 id = 0; id < g_atrac.size(); ++id) {
        if (g_atrac[id].used) continue;
        Atrac &a = g_atrac[id];
        a = Atrac{};
        const u8 *p = mem().raw_pointer(buffer, std::min<u32>(size, 0x1000u));
        if (p == nullptr) return ERR_UNKNOWN_FORMAT;
        const u32 err = parse_riff(a, p, std::min<u32>(size, 0x1000u));
        if (err != 0u) { a.free_decoder(); return err; }
        a.used = true;
        a.buffer = buffer;
        a.buffer_size = size;
        a.all_loaded = size >= a.file_size;
        if (!a.all_loaded) {
            a.read_pos = a.data_off;
            a.avail = size - a.data_off;
            a.write_pos = 0u;
            a.file_write = size;
            a.ring_frame = 0;
        }
        LOG_I("atrac", "id %u: codec=%x ch=%d align=%d file=%u data=%u+%u end=%d loop=%d..%d buffer=%08x/%u%s", id, a.codec,
              a.channels, a.block_align, a.file_size, a.data_off, a.data_size, a.end_sample, a.loop_start, a.loop_end,
              buffer, size, a.all_loaded ? " (all loaded)" : " (streaming)");
        return id;
    }
    return ERR_NO_ATRACID;
}

void AtracSetDataAndGetID(AllegrexContext &c) { ret(c, set_data(arg(c, 0), arg(c, 1))); }

void AtracReleaseAtracID(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    a->free_decoder();
    *a = Atrac{};
    ret(c, 0u);
}

void AtracGetStreamDataInfo(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    u32 write_ptr = a->buffer, writable = 0u, read_offset = 0u;
    if (!a->all_loaded) {
        wrap_file_write(*a);
        const u32 end = segment_end(*a);
        const u32 free_bytes = a->buffer_size - a->avail;
        const u32 contiguous = a->buffer_size - a->write_pos;
        writable = std::min({free_bytes, contiguous, end > a->file_write ? end - a->file_write : 0u});
        write_ptr = a->buffer + a->write_pos;
        read_offset = a->file_write;
    }
    auto &m = mem();
    if (arg(c, 1) != 0u) m.store32(arg(c, 1), write_ptr);
    if (arg(c, 2) != 0u) m.store32(arg(c, 2), writable);
    if (arg(c, 3) != 0u) m.store32(arg(c, 3), read_offset);
    ret(c, 0u);
}

void AtracAddStreamData(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (a->all_loaded) { ret(c, ERR_ALL_DATA_LOADED); return; }
    const u32 bytes = arg(c, 1);
    if (bytes > a->buffer_size - a->avail) { ret(c, ERR_ADD_DATA_IS_TOO_BIG); return; }
    a->avail += bytes;
    a->write_pos = (a->write_pos + bytes) % a->buffer_size;
    a->file_write += bytes;
    wrap_file_write(*a);
    ret(c, 0u);
}

void AtracDecodeData(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    const u32 out = arg(c, 1), samples_ptr = arg(c, 2), finish_ptr = arg(c, 3), remain_ptr = arg(c, 4);
    auto &m = mem();
    auto finish_call = [&](u32 samples, u32 finish, u32 result) {
        if (result != 0u || finish != 0u)
            LOG_I("atrac", "decode id %u -> %08x finish=%u sample=%d/%d avail=%u file_write=%u remain=%d", arg(c, 0), result, finish,
                  a->current_sample, a->end_sample, a->avail, a->file_write, remaining_frames(*a));
        if (samples_ptr != 0u) m.store32(samples_ptr, samples);
        if (finish_ptr != 0u) m.store32(finish_ptr, finish);
        if (remain_ptr != 0u) m.store32(remain_ptr, static_cast<u32>(remaining_frames(*a)));
        ret(c, result);
    };
    if (a->current_sample > a->end_sample && a->loop_num == 0) { finish_call(0u, 1u, ERR_ALL_DATA_DECODED); return; }

    // Decode warm-up frames after a seek.
    const int full = a->current_sample + a->offset_full();
    const int frame = full / a->samples_per_frame;
    while (a->warmup > 0) {
        if (decode_frame(*a, frame - a->warmup) < 0) { finish_call(0u, 0u, ERR_BUFFER_IS_EMPTY); return; }
        --a->warmup;
    }
    if (decode_frame(*a, frame) < 0) { finish_call(0u, 0u, a->all_loaded ? ERR_ALL_DATA_DECODED : ERR_BUFFER_IS_EMPTY); return; }

    const int skip = full % a->samples_per_frame;
    int count = a->samples_per_frame - skip;
    const bool looping = a->loop_num != 0 && a->loop_start >= 0;
    const int limit = looping && a->current_sample <= a->loop_end ? a->loop_end : a->end_sample;
    count = std::max(0, std::min(count, limit + 1 - a->current_sample));
    if (out != 0u) {
        s16 *dst = reinterpret_cast<s16 *>(m.raw_pointer(out, static_cast<u32>(count) * 4u));
        if (dst != nullptr) {
            for (int i = 0; i < count; ++i) {
                for (int ch = 0; ch < 2; ++ch) {
                    const float v = a->pcm[ch][static_cast<std::size_t>(skip + i)] * 32768.f;
                    dst[i * 2 + ch] = static_cast<s16>(std::clamp(v, -32768.f, 32767.f));
                }
            }
        }
    }
    a->current_sample += count;
    u32 finish = 0u;
    if (looping && a->current_sample > a->loop_end) {
        if (a->loop_num > 0) --a->loop_num;
        // The next buffered frame is the warm-up frame before the loop start.
        a->current_sample = a->loop_start;
        const int lf = (a->loop_start + a->offset_full()) / a->samples_per_frame;
        a->warmup = lf > 0 ? 1 : 0;
        if (a->ctx3p != nullptr) atrac3p_flush_buffers(a->ctx3p);
        if (a->ctx3 != nullptr) atrac3_flush_buffers(a->ctx3);
    } else if (a->current_sample > a->end_sample) {
        finish = 1u;
    }
    finish_call(static_cast<u32>(count), finish, 0u);
}

void AtracGetRemainFrame(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (arg(c, 1) != 0u) mem().store32(arg(c, 1), static_cast<u32>(remaining_frames(*a)));
    ret(c, 0u);
}

void AtracSetLoopNum(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (a->loop_start < 0) { ret(c, ERR_NO_LOOP_INFORMATION); return; }
    a->loop_num = static_cast<int>(arg(c, 1));
    // Data already buffered past the loop end would break the virtual sequence: drop it.
    if (stream_loops(*a) && a->file_write > segment_end(*a) && a->ring_frame <= loop_end_frame(*a)) {
        const u32 excess = std::min(a->file_write - segment_end(*a), a->avail);
        a->avail -= excess;
        a->write_pos = (a->write_pos + a->buffer_size - excess % a->buffer_size) % a->buffer_size;
        a->file_write -= excess;
        wrap_file_write(*a);
    }
    ret(c, 0u);
}

void AtracGetSoundSample(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    auto &m = mem();
    if (arg(c, 1) != 0u) m.store32(arg(c, 1), static_cast<u32>(a->end_sample));
    if (arg(c, 2) != 0u) m.store32(arg(c, 2), static_cast<u32>(a->loop_start));
    if (arg(c, 3) != 0u) m.store32(arg(c, 3), static_cast<u32>(a->loop_end));
    ret(c, 0u);
}

void AtracGetNextDecodePosition(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (a->current_sample > a->end_sample) {
        if (arg(c, 1) != 0u) mem().store32(arg(c, 1), 0u);
        ret(c, ERR_ALL_DATA_DECODED);
        return;
    }
    if (arg(c, 1) != 0u) mem().store32(arg(c, 1), static_cast<u32>(a->current_sample));
    ret(c, 0u);
}

void write_buffer_info(const Atrac &a, u32 info, int sample) {
    auto &m = mem();
    for (u32 i = 0; i < 32u; i += 4u) m.store32(info + i, 0u);
    if (a.all_loaded) {
        m.store32(info + 0u, a.buffer);
        return;
    }
    const int frame = (sample + a.offset_full()) / a.samples_per_frame;
    const u32 file_pos = a.frame_file_offset(std::max(frame - 1, 0));
    const u32 data_end = a.data_off + a.data_size;
    m.store32(info + 0u, a.buffer);
    m.store32(info + 4u, std::min(a.buffer_size, data_end - file_pos));
    m.store32(info + 8u, static_cast<u32>(a.block_align) * 2u);
    m.store32(info + 12u, file_pos);
}

void AtracGetBufferInfoForResetting(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    const int sample = static_cast<int>(arg(c, 1));
    if (sample > a->end_sample) { ret(c, ERR_BAD_SAMPLE); return; }
    write_buffer_info(*a, arg(c, 2), sample);
    ret(c, 0u);
}

void AtracResetPlayPosition(AllegrexContext &c) {
    Atrac *a = get(arg(c, 0));
    if (a == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    const int sample = static_cast<int>(arg(c, 1));
    if (sample > a->end_sample) { ret(c, ERR_BAD_SAMPLE); return; }
    if (a->all_loaded) {
        a->current_sample = sample;
        const int frame = (sample + a->offset_full()) / a->samples_per_frame;
        a->warmup = frame > 0 ? 1 : 0;
        if (a->ctx3p != nullptr) atrac3p_flush_buffers(a->ctx3p);
        if (a->ctx3 != nullptr) atrac3_flush_buffers(a->ctx3);
    } else {
        seek_stream(*a, sample);
        const u32 written = arg(c, 2);
        a->avail = written;
        a->write_pos = written % a->buffer_size;
        a->file_write += written;
    }
    ret(c, 0u);
}

void AtracGetSecondBufferInfo(AllegrexContext &c) {
    if (get(arg(c, 0)) == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (arg(c, 1) != 0u) mem().store32(arg(c, 1), 0u);
    if (arg(c, 2) != 0u) mem().store32(arg(c, 2), 0u);
    ret(c, ERR_SECOND_BUFFER_NOT_NEEDED);
}

void AtracSetSecondBuffer(AllegrexContext &c) { ret(c, get(arg(c, 0)) != nullptr ? 0u : ERR_BAD_ATRACID); }

void AtracGetInternalErrorInfo(AllegrexContext &c) {
    if (get(arg(c, 0)) == nullptr) { ret(c, ERR_BAD_ATRACID); return; }
    if (arg(c, 1) != 0u) mem().store32(arg(c, 1), 0u);
    ret(c, 0u);
}

} // namespace

void register_atrac_imports() {
    (void)ERR_NO_DATA;
    const char *A = "sceAtrac3plus";
    hle(A, "sceAtracSetDataAndGetID", AtracSetDataAndGetID);
    hle(A, "sceAtracReleaseAtracID", AtracReleaseAtracID);
    hle(A, "sceAtracGetStreamDataInfo", AtracGetStreamDataInfo);
    hle(A, "sceAtracAddStreamData", AtracAddStreamData);
    hle(A, "sceAtracDecodeData", AtracDecodeData);
    hle(A, "sceAtracGetRemainFrame", AtracGetRemainFrame);
    hle(A, "sceAtracSetLoopNum", AtracSetLoopNum);
    hle(A, "sceAtracGetSoundSample", AtracGetSoundSample);
    hle(A, "sceAtracGetNextDecodePosition", AtracGetNextDecodePosition);
    hle(A, "sceAtracGetBufferInfoForResetting", AtracGetBufferInfoForResetting);
    hle(A, "sceAtracResetPlayPosition", AtracResetPlayPosition);
    hle(A, "sceAtracGetSecondBufferInfo", AtracGetSecondBufferInfo);
    hle(A, "sceAtracSetSecondBuffer", AtracSetSecondBuffer);
    hle(A, "sceAtracGetInternalErrorInfo", AtracGetInternalErrorInfo);
}

template <typename F>
void atrac_fields(Atrac &a, F &&f) {
    f(a.used); f(a.codec); f(a.channels); f(a.block_align); f(a.samples_per_frame);
    f(a.data_off); f(a.data_size); f(a.file_size); f(a.first_sample_offset); f(a.end_sample);
    f(a.loop_start); f(a.loop_end); f(a.loop_num); f(a.current_sample);
    f(a.buffer); f(a.buffer_size); f(a.all_loaded);
    f(a.read_pos); f(a.write_pos); f(a.avail); f(a.file_write); f(a.ring_frame); f(a.warmup);
}

void atrac_save(StateWriter &w) {
    w.section("AT3 ");
    for (auto &a : g_atrac) {
        atrac_fields(a, [&](auto &v) { w.pod(v); });
        w.vec(a.extra);
    }
}

void atrac_load(StateReader &r) {
    r.section("AT3 ");
    for (auto &a : g_atrac) {
        a.free_decoder();
        atrac_fields(a, [&](auto &v) { r.pod(v); });
        r.vec(a.extra);
        if (!a.used) continue;
        int align = a.block_align;
        if (a.codec == kAt3Plus) a.ctx3p = atrac3p_alloc(a.channels, &align);
        else a.ctx3 = atrac3_alloc(a.channels, &align, a.extra.data(), static_cast<int>(a.extra.size()));
        // The ring buffer itself is in guest RAM; only the decoder's overlap history is
        // lost, which costs one slightly off frame (the game cannot be asked to refill).
    }
}

} // namespace p2
