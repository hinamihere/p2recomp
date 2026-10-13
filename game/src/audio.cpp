// sceAudio: eight output channels mixed on the host audio thread. Blocking
// output copies the samples immediately and suspends the caller until the
// channel queue has drained to one buffer, matching the PSP's double buffering.
#include "audio.hpp"
#include "savestate.hpp"

#include "host.hpp"
#include "kernel.hpp"

#include <algorithm>
#include <deque>
#include <cstdio>
#include <mutex>
#include <string>

namespace p2 {
namespace {

Kernel &K = g_kernel;

constexpr u32 ERROR_CHANNEL_NOT_INIT = 0x80260001u;
constexpr u32 ERROR_CHANNEL_BUSY = 0x80260002u;
constexpr u32 ERROR_NO_CHANNELS = 0x80260008u;
constexpr int kChannels = 8;

struct Channel {
    bool reserved{};
    u32 sample_count{};
    u32 format{}; // 0 = stereo, 0x10 = mono
    s32 left_volume{0x8000};
    s32 right_volume{0x8000};
    std::deque<s16> queue; // interleaved stereo
};

std::mutex g_mutex;
Channel g_channels[kChannels];
std::FILE *g_dump{}; // raw s16 stereo 44.1 kHz capture of the final mix
bool g_device{};     // an output device consumes the queues (else they drain in game time)

void mix(s16 *out, int frames) {
    std::lock_guard lock(g_mutex);
    if (K.turbo()) {
        std::fill(out, out + frames * 2, s16{0});
        return;
    }
    for (int f = 0; f < frames; ++f) {
        s32 l = 0, r = 0;
        for (auto &ch : g_channels) {
            if (ch.queue.size() < 2u) continue;
            l += ch.queue.front(); ch.queue.pop_front();
            r += ch.queue.front(); ch.queue.pop_front();
        }
        out[f * 2] = static_cast<s16>(std::clamp(l, -32768, 32767));
        out[f * 2 + 1] = static_cast<s16>(std::clamp(r, -32768, 32767));
    }
    if (g_dump != nullptr) std::fwrite(out, 4, static_cast<std::size_t>(frames), g_dump);
}

u32 queued_frames(const Channel &ch) { return static_cast<u32>(ch.queue.size() / 2u); }

// Appends one buffer to a channel, applying the channel volumes.
void enqueue(Channel &ch, u32 buffer, s32 lvol, s32 rvol) {
    auto &m = mem();
    const bool mono = ch.format == 0x10u;
    const u32 bytes = ch.sample_count * (mono ? 2u : 4u);
    const u8 *src = buffer != 0u ? m.raw_pointer(buffer, bytes) : nullptr;
    std::lock_guard lock(g_mutex);
    for (u32 i = 0; i < ch.sample_count; ++i) {
        s32 l = 0, r = 0;
        if (src != nullptr) {
            s16 a, b;
            if (mono) {
                std::memcpy(&a, src + i * 2u, 2);
                b = a;
            } else {
                std::memcpy(&a, src + i * 4u, 2);
                std::memcpy(&b, src + i * 4u + 2u, 2);
            }
            l = (a * lvol) >> 15;
            r = (b * rvol) >> 15;
        }
        ch.queue.push_back(static_cast<s16>(std::clamp(l, -32768, 32767)));
        ch.queue.push_back(static_cast<s16>(std::clamp(r, -32768, 32767)));
    }
}

// Threads blocked on output resume once their channel holds at most one buffer.
// While fast-forwarding (or without an output device, e.g. --headless) nothing plays
// the queues at the game's pace, so queued samples are discarded at 44.1 kHz of
// *virtual* time. Blocking output therefore still paces the producing threads exactly
// as playback would.
void drain_virtual(u64 now) {
    static u64 last = 0;
    if (g_device && !K.turbo()) { last = 0; return; }
    if (last == 0u) { last = now; return; }
    const u64 frames = (now - last) * 44100u / 1000000u;
    if (frames == 0u) return;
    last += frames * 1000000u / 44100u;
    std::lock_guard lock(g_mutex);
    for (auto &ch : g_channels) {
        const std::size_t n = std::min<std::size_t>(ch.queue.size(), frames * 2u);
        ch.queue.erase(ch.queue.begin(), ch.queue.begin() + static_cast<std::ptrdiff_t>(n));
    }
}

void check_waiters(u64 now) {
    drain_virtual(now);
    for (Thread *t : K.waiters(WaitType::Audio, 0, false)) {
        const s32 ch = static_cast<s32>(t->wait_value[0]);
        bool ready;
        {
            std::lock_guard lock(g_mutex);
            ready = queued_frames(g_channels[ch]) <= g_channels[ch].sample_count;
        }
        if (ready) K.wake(*t, g_channels[ch].sample_count);
    }
    if (!K.waiters(WaitType::Audio, 0, false).empty())
        K.listener_deadline = K.listener_deadline == 0u ? Kernel::now_us() + 1000u
                                                        : std::min(K.listener_deadline, Kernel::now_us() + 1000u);
}

bool valid_channel(u32 ch) { return ch < static_cast<u32>(kChannels) && g_channels[ch].reserved; }

void output(AllegrexContext &c, u32 ch, s32 lvol, s32 rvol, u32 buffer, bool blocking) {
    if (!valid_channel(ch)) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    Channel &chan = g_channels[ch];
    u32 queued;
    {
        std::lock_guard lock(g_mutex);
        queued = queued_frames(chan);
    }
    if (!blocking && queued > chan.sample_count) { ret(c, ERROR_CHANNEL_BUSY); return; }
    if (lvol >= 0) chan.left_volume = lvol;
    if (rvol >= 0) chan.right_volume = rvol;
    enqueue(chan, buffer, chan.left_volume, chan.right_volume);
    ret(c, chan.sample_count);
    if (!blocking || queued < chan.sample_count) return;
    K.current()->wait_value[0] = ch;
    K.wait_current(c, WaitType::Audio, 0, 0u, 0u, false);
}

void AudioChReserve(AllegrexContext &c) {
    s32 ch = static_cast<s32>(arg(c, 0));
    if (ch < 0) {
        for (s32 i = kChannels - 1; i >= 0; --i) if (!g_channels[i].reserved) { ch = i; break; }
        if (ch < 0) { ret(c, ERROR_NO_CHANNELS); return; }
    }
    if (ch >= kChannels || g_channels[ch].reserved) { ret(c, ERROR_CHANNEL_BUSY); return; }
    Channel &chan = g_channels[ch];
    chan.reserved = true;
    chan.sample_count = (arg(c, 1) + 63u) & ~63u;
    chan.format = arg(c, 2);
    LOG_I("audio", "reserve channel %d samples=%u format=%x", ch, chan.sample_count, chan.format);
    ret(c, static_cast<u32>(ch));
}

void AudioChRelease(AllegrexContext &c) {
    const u32 ch = arg(c, 0);
    if (!valid_channel(ch)) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    std::lock_guard lock(g_mutex);
    g_channels[ch] = Channel{};
    ret(c, 0u);
}

void AudioOutputBlocking(AllegrexContext &c) {
    output(c, arg(c, 0), static_cast<s32>(arg(c, 1)), static_cast<s32>(arg(c, 1)), arg(c, 2), true);
}
void AudioOutputPannedBlocking(AllegrexContext &c) {
    output(c, arg(c, 0), static_cast<s32>(arg(c, 1)), static_cast<s32>(arg(c, 2)), arg(c, 3), true);
}
void AudioOutputPanned(AllegrexContext &c) {
    output(c, arg(c, 0), static_cast<s32>(arg(c, 1)), static_cast<s32>(arg(c, 2)), arg(c, 3), false);
}

void AudioChangeChannelConfig(AllegrexContext &c) {
    if (!valid_channel(arg(c, 0))) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    g_channels[arg(c, 0)].format = arg(c, 1);
    ret(c, 0u);
}

void AudioChangeChannelVolume(AllegrexContext &c) {
    if (!valid_channel(arg(c, 0))) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    g_channels[arg(c, 0)].left_volume = static_cast<s32>(arg(c, 1));
    g_channels[arg(c, 0)].right_volume = static_cast<s32>(arg(c, 2));
    ret(c, 0u);
}

void AudioSetChannelDataLen(AllegrexContext &c) {
    if (!valid_channel(arg(c, 0))) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    g_channels[arg(c, 0)].sample_count = arg(c, 1);
    ret(c, 0u);
}

void AudioGetChannelRestLength(AllegrexContext &c) {
    if (!valid_channel(arg(c, 0))) { ret(c, ERROR_CHANNEL_NOT_INIT); return; }
    std::lock_guard lock(g_mutex);
    ret(c, queued_frames(g_channels[arg(c, 0)]));
}

} // namespace

void audio_dump_to(const std::string &path) {
    g_dump = std::fopen(path.c_str(), "wb");
}

void audio_start() {
    g_device = host::start_audio(mix, 44100);
}

void register_audio_imports() {
    K.event_listeners.push_back(check_waiters);
    const char *A = "sceAudio";
    hle(A, "sceAudioChReserve", AudioChReserve);
    hle(A, "sceAudioChRelease", AudioChRelease);
    hle(A, "sceAudioOutputBlocking", AudioOutputBlocking);
    hle(A, "sceAudioOutputPannedBlocking", AudioOutputPannedBlocking);
    hle(A, "sceAudioOutputPanned", AudioOutputPanned);
    hle(A, "sceAudioChangeChannelConfig", AudioChangeChannelConfig);
    hle(A, "sceAudioChangeChannelVolume", AudioChangeChannelVolume);
    hle(A, "sceAudioSetChannelDataLen", AudioSetChannelDataLen);
    hle(A, "sceAudioGetChannelRestLength", AudioGetChannelRestLength);
}

void audio_save(StateWriter &w) {
    std::lock_guard lock(g_mutex);
    w.section("AUDI");
    for (const auto &ch : g_channels) {
        w.pod(ch.reserved);
        w.pod(ch.sample_count);
        w.pod(ch.format);
        w.pod(ch.left_volume);
        w.pod(ch.right_volume);
    }
}

void audio_load(StateReader &r) {
    std::lock_guard lock(g_mutex);
    r.section("AUDI");
    for (auto &ch : g_channels) {
        r.pod(ch.reserved);
        r.pod(ch.sample_count);
        r.pod(ch.format);
        r.pod(ch.left_volume);
        r.pod(ch.right_volume);
        ch.queue.clear();
    }
}

} // namespace p2
