// sceMpeg: PSMF movie playback. The game streams the PMF file into a guest
// ringbuffer through its own read callback (invoked from RingbufferPut); we
// copy every 2048-byte pack, demux the MPEG program stream ourselves and decode
// H.264 with FFmpeg and ATRAC3+ with the at3_standalone decoder.
#include "mpeg.hpp"
#include "ge.hpp"
#include "savestate.hpp"

#include "kernel.hpp"
#include "at3_decoders.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace p2 {
namespace {

Kernel &K = g_kernel;

constexpr u32 ERROR_NO_DATA = 0x80618001u;
constexpr u32 ERROR_INVALID_VALUE = 0x806101FEu;
constexpr u32 ERROR_AVC_DECODE_FATAL = 0x80628002u;
constexpr u32 PSMF_MAGIC = 0x464D5350u;
constexpr s64 kVideoStep = 3003;  // 90 kHz ticks per frame at 29.97 fps
constexpr s64 kAudioStep = 4180;  // 2048 samples at 44.1 kHz in 90 kHz ticks

// Ringbuffer struct offsets (SceMpegRingBuffer).
constexpr u32 RB_PACKETS = 0, RB_READ = 4, RB_WRITEPOS = 8, RB_AVAIL = 12, RB_PACKETSIZE = 16, RB_DATA = 20,
              RB_CALLBACK = 24, RB_CBARG = 28, RB_UPPER = 32, RB_MPEG = 40;

u32 be32(const u8 *p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }

struct Context {
    u32 handle{};
    u32 ring{};
    u32 frame_width{};
    bool analyzed{};
    u32 stream_offset{};
    u32 stream_size{};
    s64 first_ts{90000};
    u64 bytes_received{};

    std::deque<std::vector<u8>> packs; // received, not yet demuxed
    std::vector<u8> video_es;
    std::vector<u8> audio_es;
    u64 packs_demuxed{};

    AVCodecContext *avc{};
    AVCodecParserContext *parser{};
    AVFrame *frame{};
    AVPacket *packet{};
    bool have_frame{};
    s64 video_frames{};
    bool video_end{};

    ATRAC3PContext *at3{};
    s64 audio_frames{};
    std::vector<s16> audio_pcm; // last decoded frame, interleaved stereo

    std::map<u32, u32> streams; // stream id -> type
    u32 next_stream{1};

    ~Context() {
        if (parser != nullptr) av_parser_close(parser);
        if (avc != nullptr) avcodec_free_context(&avc);
        if (frame != nullptr) av_frame_free(&frame);
        if (packet != nullptr) av_packet_free(&packet);
        if (at3 != nullptr) atrac3p_free(at3);
    }
};

std::map<u32, std::unique_ptr<Context>> g_contexts; // keyed by mpeg handle (dataPtr + 0x30)

Context *ctx_for(u32 mpeg_addr) {
    if (!valid_range(mpeg_addr, 4)) return nullptr;
    auto it = g_contexts.find(mem().load32(mpeg_addr));
    return it != g_contexts.end() ? it->second.get() : nullptr;
}

// --- program stream demux --------------------------------------------------------

s64 read_pts(const u8 *p) {
    return (s64(p[0] & 0x0E) << 29) | (s64(p[1]) << 22) | (s64(p[2] & 0xFE) << 14) | (s64(p[3]) << 7) | (s64(p[4]) >> 1);
}

// Demuxes one 2048-byte pack into the elementary stream buffers.
void demux_pack(Context &c, const std::vector<u8> &pack) {
    std::size_t i = 0;
    const std::size_t n = pack.size();
    while (i + 6 <= n) {
        if (pack[i] != 0 || pack[i + 1] != 0 || pack[i + 2] != 1) { ++i; continue; }
        const u8 code = pack[i + 3];
        if (code == 0xBA) { // pack header (MPEG-2): 14 bytes + stuffing
            if (i + 14 > n) return;
            i += 14 + (pack[i + 13] & 7u);
            continue;
        }
        const std::size_t len = (std::size_t(pack[i + 4]) << 8) | pack[i + 5];
        const std::size_t body = i + 6, end = std::min(n, body + len);
        if (code == 0xE0 || code == 0xBD) {
            std::size_t p = body;
            if (p + 3 > end) return;
            // MPEG-2 PES header
            const u8 flags = pack[p + 1];
            const std::size_t hlen = pack[p + 2];
            (void)flags;
            p += 3 + hlen;
            if (code == 0xE0) {
                if (p < end) c.video_es.insert(c.video_es.end(), pack.begin() + static_cast<std::ptrdiff_t>(p), pack.begin() + static_cast<std::ptrdiff_t>(end));
            } else {
                // Private stream 1: substream id + 3 bytes of PSP sub-header.
                if (p + 4 <= end) {
                    const u8 sub = pack[p];
                    p += 4;
                    if (sub == 0x00 && p < end) c.audio_es.insert(c.audio_es.end(), pack.begin() + static_cast<std::ptrdiff_t>(p), pack.begin() + static_cast<std::ptrdiff_t>(end));
                }
            }
        }
        i = end;
    }
}

bool demux_more(Context &c) {
    if (c.packs.empty()) return false;
    std::vector<u8> pack = std::move(c.packs.front());
    c.packs.pop_front();
    demux_pack(c, pack);
    ++c.packs_demuxed;
    return true;
}

void update_ring_avail(Context &c) {
    if (c.ring == 0u) return;
    auto &m = mem();
    const u32 packets = m.load32(c.ring + RB_PACKETS);
    m.store32(c.ring + RB_AVAIL, std::min<u32>(packets, static_cast<u32>(c.packs.size())));
}

bool stream_complete(const Context &c) { return c.stream_size != 0u && c.bytes_received >= c.stream_size; }

// --- video -----------------------------------------------------------------------

void ensure_video(Context &c) {
    if (c.avc != nullptr) return;
    av_log_set_level(AV_LOG_FATAL); // truncated streams (skipped movies) are expected
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    c.avc = avcodec_alloc_context3(codec);
    avcodec_open2(c.avc, codec, nullptr);
    c.parser = av_parser_init(AV_CODEC_ID_H264);
    c.frame = av_frame_alloc();
    c.packet = av_packet_alloc();
}

// Decodes the next picture. Returns false when no more pictures can be produced now.
bool decode_video_frame(Context &c) {
    ensure_video(c);
    for (int guard = 0; guard < 4096; ++guard) {
        if (avcodec_receive_frame(c.avc, c.frame) == 0) { c.have_frame = true; ++c.video_frames; return true; }
        // Feed more data through the parser.
        if (c.video_es.empty() && !demux_more(c)) {
            if (stream_complete(c)) {
                // Drain the decoder at the end of the stream.
                avcodec_send_packet(c.avc, nullptr);
                if (avcodec_receive_frame(c.avc, c.frame) == 0) { c.have_frame = true; ++c.video_frames; return true; }
                c.video_end = true;
            }
            return false;
        }
        if (c.video_es.empty()) continue;
        u8 *out = nullptr;
        int out_size = 0;
        const int used = av_parser_parse2(c.parser, c.avc, &out, &out_size, c.video_es.data(), static_cast<int>(c.video_es.size()),
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        c.video_es.erase(c.video_es.begin(), c.video_es.begin() + used);
        if (out_size > 0) {
            c.packet->data = out;
            c.packet->size = out_size;
            avcodec_send_packet(c.avc, c.packet);
        } else if (used == 0 && c.packs.empty()) {
            if (stream_complete(c)) {
                // Flush the parser's final picture.
                av_parser_parse2(c.parser, c.avc, &out, &out_size, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (out_size > 0) { c.packet->data = out; c.packet->size = out_size; avcodec_send_packet(c.avc, c.packet); continue; }
            }
            return false;
        }
    }
    return false;
}

void write_rgba(const Context &c, u32 dest, u32 stride, int x0, int y0, int w, int h) {
    if (!c.have_frame) return;
    const AVFrame *f = c.frame;
    auto &m = mem();
    for (int y = 0; y < h; ++y) {
        const int sy = y0 + y;
        if (sy >= f->height) break;
        u8 *row = m.raw_pointer(dest + static_cast<u32>(y) * stride * 4u, static_cast<u32>(w) * 4u);
        if (row == nullptr) continue;
        const u8 *Y = f->data[0] + sy * f->linesize[0];
        const u8 *U = f->data[1] + (sy / 2) * f->linesize[1];
        const u8 *V = f->data[2] + (sy / 2) * f->linesize[2];
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x;
            if (sx >= f->width) break;
            const int yy = (Y[sx] - 16) * 298, u = U[sx / 2] - 128, v = V[sx / 2] - 128;
            row[x * 4 + 0] = static_cast<u8>(std::clamp((yy + 409 * v + 128) >> 8, 0, 255));
            row[x * 4 + 1] = static_cast<u8>(std::clamp((yy - 100 * u - 208 * v + 128) >> 8, 0, 255));
            row[x * 4 + 2] = static_cast<u8>(std::clamp((yy + 516 * u + 128) >> 8, 0, 255));
            row[x * 4 + 3] = 255;
        }
    }
}

// --- audio -----------------------------------------------------------------------

// Pulls one ATRAC3+ frame ("0F D0 xx yy" header + payload) from the audio stream.
bool next_audio_frame(Context &c, std::vector<u8> &frame) {
    for (int guard = 0; guard < 64; ++guard) {
        // Resync to a header.
        std::size_t start = 0;
        while (start + 1 < c.audio_es.size() && !(c.audio_es[start] == 0x0F && c.audio_es[start + 1] == 0xD0)) ++start;
        if (start > 0) c.audio_es.erase(c.audio_es.begin(), c.audio_es.begin() + static_cast<std::ptrdiff_t>(start));
        if (c.audio_es.size() >= 4) {
            const int size = (((c.audio_es[2] & 3) << 8) | (c.audio_es[3] * 8)) + 0x10;
            if (static_cast<int>(c.audio_es.size()) >= size) {
                frame.assign(c.audio_es.begin() + 8, c.audio_es.begin() + size);
                c.audio_es.erase(c.audio_es.begin(), c.audio_es.begin() + size);
                return true;
            }
        }
        if (!demux_more(c)) return false;
    }
    return false;
}

bool decode_audio_frame(Context &c, u32 out) {
    std::vector<u8> frame;
    if (!next_audio_frame(c, frame)) return false;
    if (c.at3 == nullptr) {
        int align = static_cast<int>(frame.size());
        c.at3 = atrac3p_alloc(2, &align);
    }
    float left[2048]{}, right[2048]{};
    float *planes[2] = {left, right};
    int samples = 0;
    if (c.at3 != nullptr) atrac3p_decode_frame(c.at3, planes, &samples, frame.data(), static_cast<int>(frame.size()));
    if (samples <= 0) samples = 2048;
    s16 *dst = reinterpret_cast<s16 *>(mem().raw_pointer(out, 2048u * 4u));
    if (dst != nullptr) {
        for (int i = 0; i < 2048; ++i) {
            dst[i * 2] = static_cast<s16>(std::clamp(left[i] * 32768.f, -32768.f, 32767.f));
            dst[i * 2 + 1] = static_cast<s16>(std::clamp(right[i] * 32768.f, -32768.f, 32767.f));
        }
    }
    ++c.audio_frames;
    return true;
}

// --- au helpers --------------------------------------------------------------------

void write_au_ts(u32 au, s64 pts, s64 dts) {
    auto &m = mem();
    // SceMpegAu stores timestamps as two big-endian-ordered 32-bit halves: hi word first.
    m.store32(au + 0u, static_cast<u32>(static_cast<u64>(pts) >> 32));
    m.store32(au + 4u, static_cast<u32>(pts));
    m.store32(au + 8u, static_cast<u32>(static_cast<u64>(dts) >> 32));
    m.store32(au + 12u, static_cast<u32>(dts));
}

// --- imports -----------------------------------------------------------------------

void MpegInit(AllegrexContext &c) { ret(c, 0u); }
void MpegFinish(AllegrexContext &c) { ret(c, 0u); }
void MpegQueryMemSize(AllegrexContext &c) { ret(c, 0x10000u); }
void MpegRingbufferQueryMemSize(AllegrexContext &c) { ret(c, arg(c, 0) * (104u + 2048u)); }

void MpegRingbufferConstruct(AllegrexContext &c) {
    const u32 ring = arg(c, 0), packets = arg(c, 1), data = arg(c, 2);
    auto &m = mem();
    m.store32(ring + RB_PACKETS, packets);
    m.store32(ring + RB_READ, 0u);
    m.store32(ring + RB_WRITEPOS, 0u);
    m.store32(ring + RB_AVAIL, 0u);
    m.store32(ring + RB_PACKETSIZE, 2048u);
    m.store32(ring + RB_DATA, data);
    m.store32(ring + RB_CALLBACK, arg(c, 4));
    m.store32(ring + RB_CBARG, arg(c, 5));
    m.store32(ring + RB_UPPER, data + packets * 2048u);
    m.store32(ring + RB_MPEG, 0u);
    m.store32(ring + 44u, K.module_gp());
    ret(c, 0u);
}

void MpegRingbufferDestruct(AllegrexContext &c) { ret(c, 0u); }

void MpegCreate(AllegrexContext &c) {
    const u32 mpeg = arg(c, 0), data = arg(c, 1), ring = arg(c, 3);
    auto &m = mem();
    const u32 handle = data + 0x30u;
    m.store32(mpeg, handle);
    static constexpr u8 magic[12] = {'L', 'I', 'B', 'M', 'P', 'E', 'G', 0, '0', '0', '1', 0};
    for (u32 i = 0; i < 12u; ++i) m.store8(handle + i, magic[i]);
    m.store32(handle + 12u, 0xFFFFFFFFu);
    m.store32(handle + 16u, ring);
    if (ring != 0u) {
        m.store32(handle + 20u, m.load32(ring + RB_UPPER));
        m.store32(ring + RB_MPEG, mpeg);
        m.store32(ring + RB_AVAIL, 0u);
    }
    auto ctx = std::make_unique<Context>();
    ctx->handle = handle;
    ctx->ring = ring;
    ctx->frame_width = arg(c, 4);
    g_contexts[handle] = std::move(ctx);
    LOG_I("mpeg", "create mpeg=%08x ring=%08x width=%u", mpeg, ring, arg(c, 4));
    ret(c, 0u);
}

void MpegDelete(AllegrexContext &c) {
    if (valid_range(arg(c, 0), 4)) g_contexts.erase(mem().load32(arg(c, 0)));
    ret(c, 0u);
}

bool analyze(Context &ctx, u32 buffer) {
    const u8 *p = mem().raw_pointer(buffer, 2048u);
    if (p == nullptr || (p[0] | (p[1] << 8) | (p[2] << 16) | (u32(p[3]) << 24)) != PSMF_MAGIC) return false;
    ctx.stream_offset = be32(p + 8);
    ctx.stream_size = be32(p + 12);
    ctx.first_ts = (s64((p[0x54] << 8) | p[0x55]) << 32) | be32(p + 0x56); // 48-bit big-endian
    if (ctx.first_ts <= 0) ctx.first_ts = 90000;
    ctx.analyzed = true;
    LOG_I("mpeg", "psmf offset=%u size=%u first_ts=%lld", ctx.stream_offset, ctx.stream_size, static_cast<long long>(ctx.first_ts));
    return true;
}

void MpegQueryStreamOffset(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    if (!analyze(*ctx, arg(c, 1))) { mem().store32(arg(c, 2), 0u); ret(c, ERROR_INVALID_VALUE); return; }
    mem().store32(arg(c, 2), ctx->stream_offset);
    ret(c, 0u);
}

void MpegQueryStreamSize(AllegrexContext &c) {
    const u8 *p = mem().raw_pointer(arg(c, 0), 16u);
    if (p == nullptr || p[0] != 'P' || p[1] != 'S' || p[2] != 'M' || p[3] != 'F') { mem().store32(arg(c, 1), 0u); ret(c, ERROR_INVALID_VALUE); return; }
    mem().store32(arg(c, 1), be32(p + 12));
    ret(c, 0u);
}

void MpegRegistStream(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 sid = ctx->next_stream++;
    ctx->streams[sid] = arg(c, 1);
    LOG_I("mpeg", "regist stream type=%u num=%u -> %u", arg(c, 1), arg(c, 2), sid);
    ret(c, sid);
}

void MpegUnRegistStream(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx != nullptr) ctx->streams.erase(arg(c, 1));
    ret(c, 0u);
}

void MpegMallocAvcEsBuf(AllegrexContext &c) { ret(c, 1u); }
void MpegFreeAvcEsBuf(AllegrexContext &c) { ret(c, 0u); }

void MpegInitAu(AllegrexContext &c) {
    const u32 au = arg(c, 2);
    write_au_ts(au, 0, -1);
    mem().store32(au + 16u, 0u);
    mem().store32(au + 20u, arg(c, 1) == 1u ? 2048u : 2112u);
    ret(c, 0u);
}

void MpegQueryAtracEsSize(AllegrexContext &c) {
    mem().store32(arg(c, 1), 2112u);
    mem().store32(arg(c, 2), 8192u);
    ret(c, 0u);
}

void MpegFlushAllStream(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx != nullptr) {
        ctx->packs.clear();
        ctx->video_es.clear();
        ctx->audio_es.clear();
        update_ring_avail(*ctx);
    }
    ret(c, 0u);
}

void MpegRingbufferAvailableSize(AllegrexContext &c) {
    const u32 ring = arg(c, 0);
    auto &m = mem();
    const u32 packets = m.load32(ring + RB_PACKETS);
    const u32 mpeg = m.load32(ring + RB_MPEG);
    Context *ctx = ctx_for(mpeg);
    if (ctx != nullptr) update_ring_avail(*ctx);
    ret(c, packets - m.load32(ring + RB_AVAIL));
}

// Accepts the packets the read callback just wrote, then asks for more if needed.
void ring_put_round(u32 ring, u32 write_offset, s32 remaining, u32 total, u32 v0) {
    auto &m = mem();
    AllegrexContext &c = rt().cpu();
    const u32 packets = m.load32(ring + RB_PACKETS);
    Context *ctx = ctx_for(m.load32(ring + RB_MPEG));
    const s32 added = static_cast<s32>(v0);
    if (added > 0 && ctx != nullptr) {
        const u32 data = m.load32(ring + RB_DATA);
        for (s32 i = 0; i < added; ++i) {
            const u32 slot = (write_offset + static_cast<u32>(i)) % packets;
            const u8 *src = m.raw_pointer(data + slot * 2048u, 2048u);
            if (src != nullptr) ctx->packs.emplace_back(src, src + 2048);
        }
        ctx->bytes_received += static_cast<u64>(added) * 2048u;
        m.store32(ring + RB_READ, m.load32(ring + RB_READ) + static_cast<u32>(added));
        m.store32(ring + RB_WRITEPOS, m.load32(ring + RB_WRITEPOS) + static_cast<u32>(added));
        update_ring_avail(*ctx);
        total += static_cast<u32>(added);
    }
    if (added > 0 && remaining > 0) {
        const u32 offset = m.load32(ring + RB_WRITEPOS) % packets;
        const u32 want = std::min<u32>(static_cast<u32>(remaining), packets - offset);
        K.call_guest(c, m.load32(ring + RB_CALLBACK), {m.load32(ring + RB_DATA) + offset * 2048u, want, m.load32(ring + RB_CBARG)},
                     [ring, offset, rest = remaining - static_cast<s32>(want), total](u32 r) { ring_put_round(ring, offset, rest, total, r); });
        return;
    }
    c.gpr[2] = (added < 0 && total == 0u) ? static_cast<u32>(added) : total;
}

void MpegRingbufferPut(AllegrexContext &c) {
    const u32 ring = arg(c, 0);
    auto &m = mem();
    const u32 packets = m.load32(ring + RB_PACKETS);
    Context *ctx = ctx_for(m.load32(ring + RB_MPEG));
    if (ctx != nullptr) update_ring_avail(*ctx);
    s32 count = std::min(static_cast<s32>(arg(c, 1)), static_cast<s32>(arg(c, 2)));
    count = std::min(count, static_cast<s32>(packets - m.load32(ring + RB_AVAIL)));
    const u32 callback = m.load32(ring + RB_CALLBACK);
    if (count <= 0 || callback == 0u || ctx == nullptr) { ret(c, 0u); return; }
    const u32 offset = m.load32(ring + RB_WRITEPOS) % packets;
    const u32 want = std::min<u32>(static_cast<u32>(count), packets - offset);
    ret(c, 0u);
    K.call_guest(c, callback, {m.load32(ring + RB_DATA) + offset * 2048u, want, m.load32(ring + RB_CBARG)},
                 [ring, offset, rest = count - static_cast<s32>(want)](u32 r) { ring_put_round(ring, offset, rest, 0u, r); });
}

void MpegGetAvcAu(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 au = arg(c, 2);
    const s64 pts = ctx->first_ts + ctx->video_frames * kVideoStep;
    if (ctx->bytes_received == 0u) { write_au_ts(au, 0, 0); ret(c, ERROR_NO_DATA); return; }
    if (ctx->video_end || (ctx->packs.empty() && ctx->video_es.empty() && stream_complete(*ctx) && !ctx->have_frame && ctx->video_frames > 0)) {
        write_au_ts(au, pts, -1);
        ret(c, ERROR_NO_DATA);
        return;
    }
    if (ctx->packs.empty() && ctx->video_es.size() < 4096u && !stream_complete(*ctx)) {
        write_au_ts(au, pts, pts - kVideoStep);
        ret(c, ERROR_NO_DATA);
        return;
    }
    write_au_ts(au, pts, pts - kVideoStep);
    if (arg(c, 3) != 0u) mem().store32(arg(c, 3), 1u);
    ret(c, 0u);
}

void MpegAvcDecodeYCbCr(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 au = arg(c, 1), init = arg(c, 3);
    ctx->have_frame = false;
    const bool ok = decode_video_frame(*ctx);
    update_ring_avail(*ctx);
    const s64 pts = ctx->first_ts + (ctx->video_frames - 1) * kVideoStep;
    write_au_ts(au, pts, pts - kVideoStep);
    if (init != 0u) mem().store32(init, ok ? 1u : 0u);
    if (!ok && ctx->video_end) { ret(c, ERROR_AVC_DECODE_FATAL); return; }
    ret(c, 0u);
}

void MpegAvcDecodeStopYCbCr(AllegrexContext &c) {
    if (arg(c, 2) != 0u) mem().store32(arg(c, 2), 0u);
    ret(c, 0u);
}

void MpegAvcDecodeFlush(AllegrexContext &c) { ret(c, 0u); }

void MpegAvcQueryYCbCrSize(AllegrexContext &c) {
    const u32 w = arg(c, 2), h = arg(c, 3);
    if ((w & 15u) != 0u || (h & 15u) != 0u || h > 272u || w > 480u) { ret(c, ERROR_INVALID_VALUE); return; }
    mem().store32(arg(c, 4), (w / 2u) * (h / 2u) * 6u + 128u);
    ret(c, 0u);
}

void MpegAvcInitYCbCr(AllegrexContext &c) { ret(c, 0u); }

void MpegAvcCsc(AllegrexContext &c) {
    ge_flush(); // writes VRAM from the CPU
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 range = arg(c, 2);
    u32 width = arg(c, 3);
    const u32 dest = arg(c, 4);
    auto &m = mem();
    const int x = static_cast<int>(m.load32(range)), y = static_cast<int>(m.load32(range + 4u));
    const int w = static_cast<int>(m.load32(range + 8u)), h = static_cast<int>(m.load32(range + 12u));
    if (width == 0u) width = ctx->frame_width != 0u ? ctx->frame_width : 512u;
    write_rgba(*ctx, dest, width, x, y, w, h);
    ret(c, 0u);
}

void MpegGetAtracAu(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 au = arg(c, 2);
    const s64 pts = ctx->first_ts + ctx->audio_frames * kAudioStep;
    // Make sure a frame is available without consuming it: demux until a full frame is buffered.
    while (ctx->audio_es.size() < 0x2000u && demux_more(*ctx)) {}
    update_ring_avail(*ctx);
    if (ctx->audio_es.size() < 16u) { write_au_ts(au, pts, -1); ret(c, ERROR_NO_DATA); return; }
    write_au_ts(au, pts, pts);
    if (arg(c, 3) != 0u) mem().store32(arg(c, 3), 0u);
    ret(c, 0u);
}

void MpegAtracDecode(AllegrexContext &c) {
    Context *ctx = ctx_for(arg(c, 0));
    if (ctx == nullptr) { ret(c, 0xFFFFFFFFu); return; }
    const u32 out = arg(c, 2);
    if (u8 *p = mem().raw_pointer(out, 8192u)) std::memset(p, 0, 8192u);
    decode_audio_frame(*ctx, out);
    update_ring_avail(*ctx);
    const s64 pts = ctx->first_ts + ctx->audio_frames * kAudioStep;
    write_au_ts(arg(c, 1), pts, pts);
    ret(c, 0u);
}

} // namespace

void register_mpeg_imports() {
    const char *M = "sceMpeg";
    hle(M, "sceMpegInit", MpegInit);
    hle(M, "sceMpegFinish", MpegFinish);
    hle(M, "sceMpegQueryMemSize", MpegQueryMemSize);
    hle(M, "sceMpegRingbufferQueryMemSize", MpegRingbufferQueryMemSize);
    hle(M, "sceMpegRingbufferConstruct", MpegRingbufferConstruct);
    hle(M, "sceMpegRingbufferDestruct", MpegRingbufferDestruct);
    hle(M, "sceMpegRingbufferPut", MpegRingbufferPut);
    hle(M, "sceMpegRingbufferAvailableSize", MpegRingbufferAvailableSize);
    hle(M, "sceMpegCreate", MpegCreate);
    hle(M, "sceMpegDelete", MpegDelete);
    hle(M, "sceMpegQueryStreamOffset", MpegQueryStreamOffset);
    hle(M, "sceMpegQueryStreamSize", MpegQueryStreamSize);
    hle(M, "sceMpegRegistStream", MpegRegistStream);
    hle(M, "sceMpegUnRegistStream", MpegUnRegistStream);
    hle(M, "sceMpegFlushAllStream", MpegFlushAllStream);
    hle(M, "sceMpegMallocAvcEsBuf", MpegMallocAvcEsBuf);
    hle(M, "sceMpegFreeAvcEsBuf", MpegFreeAvcEsBuf);
    hle(M, "sceMpegInitAu", MpegInitAu);
    hle(M, "sceMpegQueryAtracEsSize", MpegQueryAtracEsSize);
    hle(M, "sceMpegGetAvcAu", MpegGetAvcAu);
    hle(M, "sceMpegGetAtracAu", MpegGetAtracAu);
    hle(M, "sceMpegAtracDecode", MpegAtracDecode);
    hle(M, "sceMpegAvcDecodeYCbCr", MpegAvcDecodeYCbCr);
    hle(M, "sceMpegAvcDecodeStopYCbCr", MpegAvcDecodeStopYCbCr);
    hle(M, "sceMpegAvcDecodeFlush", MpegAvcDecodeFlush);
    hle(M, "sceMpegAvcQueryYCbCrSize", MpegAvcQueryYCbCrSize);
    hle(M, "sceMpegAvcInitYCbCr", MpegAvcInitYCbCr);
    hle(M, "sceMpegAvcCsc", MpegAvcCsc);
}

// Decoders are not serialized: they are recreated lazily, so a state taken mid-movie
// resumes with a few damaged pictures until the next keyframe. Idle contexts (the
// title screen keeps one) round-trip exactly.
void mpeg_save(StateWriter &w) {
    w.section("MPEG");
    w.pod(static_cast<u32>(g_contexts.size()));
    for (const auto &[handle, c] : g_contexts) {
        w.pod(handle);
        w.pod(c->handle); w.pod(c->ring); w.pod(c->frame_width); w.pod(c->analyzed);
        w.pod(c->stream_offset); w.pod(c->stream_size); w.pod(c->first_ts); w.pod(c->bytes_received);
        w.pod(static_cast<u32>(c->packs.size()));
        for (const auto &p : c->packs) w.vec(p);
        w.vec(c->video_es); w.vec(c->audio_es); w.pod(c->packs_demuxed);
        w.pod(c->video_frames); w.pod(c->video_end); w.pod(c->audio_frames); w.vec(c->audio_pcm);
        w.pod(static_cast<u32>(c->streams.size()));
        for (const auto &[id, type] : c->streams) { w.pod(id); w.pod(type); }
        w.pod(c->next_stream);
    }
}

void mpeg_load(StateReader &r) {
    r.section("MPEG");
    g_contexts.clear();
    for (u32 n = r.pod<u32>(); n > 0; --n) {
        const u32 handle = r.pod<u32>();
        auto c = std::make_unique<Context>();
        r.pod(c->handle); r.pod(c->ring); r.pod(c->frame_width); r.pod(c->analyzed);
        r.pod(c->stream_offset); r.pod(c->stream_size); r.pod(c->first_ts); r.pod(c->bytes_received);
        for (u32 k = r.pod<u32>(); k > 0; --k) { std::vector<u8> p; r.vec(p); c->packs.push_back(std::move(p)); }
        r.vec(c->video_es); r.vec(c->audio_es); r.pod(c->packs_demuxed);
        r.pod(c->video_frames); r.pod(c->video_end); r.pod(c->audio_frames); r.vec(c->audio_pcm);
        for (u32 k = r.pod<u32>(); k > 0; --k) { const u32 id = r.pod<u32>(); c->streams[id] = r.pod<u32>(); }
        r.pod(c->next_stream);
        g_contexts[handle] = std::move(c);
    }
}

} // namespace p2
