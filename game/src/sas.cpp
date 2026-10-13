// sceSasCore: the PSP's software voice mixer (32 voices of VAG ADPCM or PCM
// with pitch and ADSR envelopes). Hardware behaviour (envelope curves, key-on
// delay, two-tap resampling) follows what PPSSPP/JPCSP measured on hardware.
#include "sas.hpp"

#include "kernel.hpp"

#include <algorithm>
#include <array>

namespace p2 {
namespace {

constexpr int kVoices = 32;
constexpr s64 kEnvelopeMax = 0x40000000;
constexpr int kPitchBase = 0x1000;
constexpr int kPitchShift = 12;

enum Curve { LinearIncrease = 0, LinearDecrease = 1, LinearBent = 2, ExponentDecrease = 3, ExponentIncrease = 4, Direct = 5 };
enum EnvState { Attack, Decay, Sustain, Release, Off, KeyOnWait, KeyOnStep };

s16 clamp16(int v) { return static_cast<s16>(std::clamp(v, -32768, 32767)); }

struct Envelope {
    int attack_rate{}, decay_rate{}, sustain_rate{}, release_rate{};
    int attack_type{LinearIncrease}, decay_type{LinearDecrease}, sustain_type{LinearDecrease}, release_type{LinearDecrease};
    int sustain_level{};
    EnvState state{Off};
    s64 height{};

    void walk(int type, int rate) {
        s64 delta;
        switch (type) {
        case LinearIncrease: height += rate; break;
        case LinearDecrease: height -= rate; break;
        case LinearBent: height += height <= kEnvelopeMax * 3 / 4 ? rate : rate / 4; break;
        case ExponentDecrease:
            delta = height - kEnvelopeMax;
            delta += (-delta * rate) >> 32;
            height = delta + kEnvelopeMax - (static_cast<s64>(rate) + 3) / 4;
            break;
        case ExponentIncrease:
            delta = height - kEnvelopeMax;
            delta += (-delta * rate) >> 32;
            height = delta + 0x4000 + kEnvelopeMax;
            break;
        case Direct: height = rate; break;
        default: break;
        }
    }
    void set_state(EnvState s) {
        if (height > kEnvelopeMax) height = kEnvelopeMax;
        state = s;
    }
    void step() {
        switch (state) {
        case Attack:
            walk(attack_type, attack_rate);
            if (height >= kEnvelopeMax || height < 0) set_state(Decay);
            break;
        case Decay:
            walk(decay_type, decay_rate);
            if (height < sustain_level) set_state(Sustain);
            break;
        case Sustain:
            walk(sustain_type, sustain_rate);
            if (height <= 0) { height = 0; set_state(Release); }
            break;
        case Release:
            walk(release_type, release_rate);
            if (height <= 0) { height = 0; set_state(Off); }
            break;
        case KeyOnWait: height = 0; set_state(KeyOnStep); break;
        case KeyOnStep:
            if (++height >= 31) { height = 0; set_state(Attack); }
            break;
        case Off: break;
        }
    }
    bool needs_key_on() const { return state == KeyOnWait; }
    bool ended() const { return state == Off; }
};

int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 26) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}
int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    const int rate = ((7 - (n & 3)) << 24) >> (n >> 2);
    return rate == 0 ? 1 : rate;
}

void set_simple_envelope(Envelope &e, u32 env1, u32 env2) {
    e.attack_rate = simple_rate(static_cast<int>(env1 >> 8));
    e.attack_type = (env1 & 0x8000u) == 0u ? LinearIncrease : LinearBent;
    const int dn = static_cast<int>((env1 >> 4) & 0xFu);
    e.decay_rate = dn == 0 ? 0x7FFFFFFF : static_cast<int>(0x80000000u >> dn);
    e.decay_type = ExponentDecrease;
    e.sustain_type = static_cast<int>((env2 >> 14) & 3u);
    e.sustain_rate = e.sustain_type == ExponentDecrease ? exponent_rate(static_cast<int>(env2 >> 6)) : simple_rate(static_cast<int>(env2 >> 6));
    e.release_type = (env2 & 0x20u) == 0u ? LinearDecrease : ExponentDecrease;
    const int rn = static_cast<int>(env2 & 0x1Fu);
    if (rn == 31) e.release_rate = 0;
    else if (e.release_type == LinearDecrease) e.release_rate = rn == 30 ? 0x40000000 : (rn == 29 ? 1 : (0x10000000 >> rn));
    else e.release_rate = rn == 0 ? 0x7FFFFFFF : static_cast<int>(0x80000000u >> rn);
    e.sustain_level = static_cast<int>(((env1 & 0xFu) + 1u) << 26);
}

// PSX/PSP ADPCM (VAG) block decoder.
struct VagDecoder {
    u32 data{}, read{};
    int blocks{};
    int cur_block{-1};
    int cur_sample{28};
    int loop_start{-1};
    bool loop_enabled{}, loop_next{}, end{};
    int s1{}, s2{};
    s16 samples[28]{};

    void start(u32 addr, u32 size, bool loop) {
        data = read = addr;
        blocks = static_cast<int>(size / 16u);
        cur_block = -1;
        cur_sample = 28;
        loop_start = -1;
        loop_enabled = loop;
        loop_next = false;
        end = false;
        s1 = s2 = 0;
    }

    void decode_block() {
        static constexpr int coef[16][2] = {{0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0}, {52, 0},
                                            {55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151}};
        if (cur_block == blocks - 1) { end = true; return; }
        const u8 *p = mem().raw_pointer(read, 16);
        if (p == nullptr) { end = true; return; }
        const int predict = p[0] >> 4, shift = p[0] & 0xF;
        const int flags = p[1];
        if (flags == 7) { end = true; return; }
        if (flags == 6) loop_start = cur_block;
        else if (flags == 3 && loop_enabled) loop_next = true;
        const int c1 = coef[predict][0], c2 = -coef[predict][1];
        int a = s1, b = s2;
        for (int i = 0; i < 28; i += 2) {
            const u8 d = p[2 + i / 2];
            const int x1 = static_cast<s16>((d & 0xF) << 12) >> shift;
            const int x2 = static_cast<s16>((d & 0xF0) << 8) >> shift;
            b = clamp16(x1 + ((a * c1 + b * c2) >> 6));
            a = clamp16(x2 + ((b * c1 + a * c2) >> 6));
            samples[i] = static_cast<s16>(b);
            samples[i + 1] = static_cast<s16>(a);
        }
        s1 = a;
        s2 = b;
        cur_sample = 0;
        ++cur_block;
        read += 16;
    }

    void get(s16 *out, int n) {
        for (int i = 0; i < n; ++i) {
            if (end) { std::fill(out + i, out + n, 0); return; }
            if (cur_sample == 28) {
                if (loop_next) {
                    read = data + 16u * static_cast<u32>(loop_start) + 16u;
                    cur_block = loop_start;
                    loop_next = false;
                }
                decode_block();
                if (end) { std::fill(out + i, out + n, 0); return; }
            }
            out[i] = samples[cur_sample++];
        }
    }
};

enum class VoiceType { None, Vag, Pcm, Noise };

struct Voice {
    VoiceType type{VoiceType::None};
    bool playing{}, paused{}, on{}, loop{};
    u32 vag_addr{}, vag_size{};
    u32 pcm_addr{};
    int pcm_size{}, pcm_index{}, pcm_loop{};
    int pitch{kPitchBase};
    int volume_left{0x1000}, volume_right{0x1000}, effect_left{}, effect_right{};
    u32 sample_frac{};
    s16 hist[2]{};
    u32 noise_state{1};
    Envelope env;
    VagDecoder vag;

    void read(s16 *out, int n) {
        switch (type) {
        case VoiceType::Vag: vag.get(out, n); break;
        case VoiceType::Pcm: {
            int i = 0;
            const s16 *src = reinterpret_cast<const s16 *>(mem().raw_pointer(pcm_addr, static_cast<u32>(pcm_size) * 2u));
            while (i < n && src != nullptr && on) {
                const int count = std::min(pcm_size - pcm_index, n - i);
                std::memcpy(out + i, src + pcm_index, static_cast<std::size_t>(count) * 2u);
                i += count;
                pcm_index += count;
                if (pcm_index >= pcm_size) {
                    if (!loop) break;
                    pcm_index = pcm_loop;
                }
            }
            std::fill(out + i, out + n, 0);
            break;
        }
        case VoiceType::Noise:
            for (int i = 0; i < n; ++i) {
                noise_state = noise_state * 1103515245u + 12345u;
                out[i] = static_cast<s16>(noise_state >> 16);
            }
            break;
        default: std::fill(out, out + n, 0); break;
        }
    }
    bool samples_ended() const {
        if (type == VoiceType::Vag) return vag.end;
        if (type == VoiceType::Pcm) return pcm_index >= pcm_size;
        return false;
    }
};

struct SasCore {
    int grain{256};
    int max_voices{32};
    int output_mode{};
    int sample_rate{44100};
    std::array<Voice, kVoices> voices;
    std::vector<int> mix;
    std::vector<s16> temp;
};

SasCore g_sas;

constexpr u32 ERROR_INVALID_VOICE = 0x80420010u;
constexpr u32 ERROR_INVALID_ADDRESS = 0x80420005u;
constexpr u32 ERROR_INVALID_SIZE = 0x8042000Au;

bool bad_voice(u32 v) { return v >= static_cast<u32>(kVoices); }

void mix_voice(Voice &v) {
    const int grain = g_sas.grain;
    int delay = 0;
    if (v.env.needs_key_on()) {
        const bool ignore_pitch = v.type == VoiceType::Pcm && v.pitch > kPitchBase;
        delay = ignore_pitch ? 32 : (32 * v.pitch) >> kPitchShift;
        if (v.type == VoiceType::Vag) ++delay;
    }
    auto &temp = g_sas.temp;
    temp[0] = v.hist[0];
    temp[1] = v.hist[1];
    u32 frac = v.sample_frac;
    int to_read = static_cast<int>((frac + static_cast<u32>(v.pitch) * static_cast<u32>(std::max(0, grain - delay))) >> kPitchShift);
    to_read = std::min(to_read, static_cast<int>(temp.size()) - 4);
    int read_pos = 2;
    if (v.env.needs_key_on()) { read_pos = 0; to_read += 2; }
    v.read(&temp[read_pos], to_read);
    const int temp_pos = read_pos + to_read;
    for (int i = 0; i < delay; ++i) v.env.step();
    const bool interp = v.pitch != kPitchBase || (frac & (kPitchBase - 1)) != 0u;
    for (int i = delay; i < grain; ++i) {
        const s16 *s = temp.data() + (frac >> kPitchShift);
        int sample = s[0];
        if (interp) {
            const int f = static_cast<int>(frac & (kPitchBase - 1));
            sample = s[0] - (((s[0] - s[1]) * f) >> kPitchShift);
        }
        frac += static_cast<u32>(v.pitch);
        int env = static_cast<int>(v.env.height);
        v.env.step();
        env = (env + (1 << 14)) >> 15;
        sample = ((sample * env) + (1 << 14)) >> 15;
        g_sas.mix[i * 2] += (sample * v.volume_left) >> 12;
        g_sas.mix[i * 2 + 1] += (sample * v.volume_right) >> 12;
    }
    v.hist[0] = temp[temp_pos - 2];
    v.hist[1] = temp[temp_pos - 1];
    v.sample_frac = frac - static_cast<u32>(temp_pos - 2) * kPitchBase;
    if (v.samples_ended()) { v.env.state = Off; v.env.height = 0; }
    if (v.env.ended()) { v.playing = false; v.on = false; }
}

void core_mix(u32 out, u32 in, int lvol, int rvol) {
    const int grain = g_sas.grain;
    g_sas.mix.assign(static_cast<std::size_t>(grain) * 2u, 0);
    if (g_sas.temp.size() < static_cast<std::size_t>(grain) * 4u + 8u) g_sas.temp.resize(static_cast<std::size_t>(grain) * 4u + 8u);
    for (auto &v : g_sas.voices)
        if (v.playing && !v.paused) mix_voice(v);
    auto &m = mem();
    if (g_sas.output_mode == 0) {
        s16 *o = reinterpret_cast<s16 *>(m.raw_pointer(out, static_cast<u32>(grain) * 4u));
        const s16 *ip = in != 0u ? reinterpret_cast<const s16 *>(m.raw_pointer(in, static_cast<u32>(grain) * 4u)) : nullptr;
        if (o == nullptr) return;
        for (int i = 0; i < grain * 2; ++i) {
            int s = g_sas.mix[static_cast<std::size_t>(i)];
            if (ip != nullptr) s += (ip[i] * ((i & 1) ? rvol : lvol)) >> 12;
            o[i] = clamp16(s);
        }
    } else {
        s16 *o = reinterpret_cast<s16 *>(m.raw_pointer(out, static_cast<u32>(grain) * 8u));
        if (o == nullptr) return;
        for (int i = 0; i < grain; ++i) {
            o[i] = clamp16(g_sas.mix[static_cast<std::size_t>(i) * 2]);
            o[grain + i] = clamp16(g_sas.mix[static_cast<std::size_t>(i) * 2 + 1]);
            o[grain * 2 + i] = 0;
            o[grain * 3 + i] = 0;
        }
    }
}

// --- imports --------------------------------------------------------------------

void SasInit(AllegrexContext &c) {
    g_sas = SasCore{};
    g_sas.grain = static_cast<int>(arg(c, 1));
    g_sas.max_voices = static_cast<int>(arg(c, 2));
    g_sas.output_mode = static_cast<int>(arg(c, 3));
    g_sas.sample_rate = static_cast<int>(arg(c, 4));
    LOG_I("sas", "init core=%08x grain=%d voices=%d mode=%d rate=%d", arg(c, 0), g_sas.grain, g_sas.max_voices,
          g_sas.output_mode, g_sas.sample_rate);
    ret(c, 0u);
}

void SasCoreFn(AllegrexContext &c) { core_mix(arg(c, 1), 0u, 0, 0); ret(c, 0u); }
void SasCoreWithMix(AllegrexContext &c) {
    core_mix(arg(c, 1), arg(c, 1), static_cast<int>(arg(c, 2)), static_cast<int>(arg(c, 3)));
    ret(c, 0u);
}

void SasSetVoice(AllegrexContext &c) {
    const u32 vi = arg(c, 1), addr = arg(c, 2), size = arg(c, 3), loop = arg(c, 4);
    if (bad_voice(vi)) { ret(c, ERROR_INVALID_VOICE); return; }
    if (size == 0u || (size & 0xFu) != 0u) { ret(c, ERROR_INVALID_SIZE); return; }
    if (!valid_range(addr, size)) { ret(c, ERROR_INVALID_ADDRESS); return; }
    Voice &v = g_sas.voices[vi];
    v.type = VoiceType::Vag;
    v.vag_addr = addr;
    v.vag_size = size;
    v.loop = loop != 0u;
    ret(c, 0u);
}

void SasSetVoicePCM(AllegrexContext &c) {
    const u32 vi = arg(c, 1), addr = arg(c, 2);
    const int size = static_cast<int>(arg(c, 3)), loop_pos = static_cast<int>(arg(c, 4));
    if (bad_voice(vi)) { ret(c, ERROR_INVALID_VOICE); return; }
    if (size <= 0 || size > 0x10000) { ret(c, ERROR_INVALID_SIZE); return; }
    Voice &v = g_sas.voices[vi];
    v.type = VoiceType::Pcm;
    v.pcm_addr = addr;
    v.pcm_size = size;
    v.pcm_index = 0;
    v.pcm_loop = loop_pos >= 0 ? loop_pos : 0;
    v.loop = loop_pos >= 0;
    ret(c, 0u);
}

void SasSetPitch(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    g_sas.voices[arg(c, 1)].pitch = std::clamp(static_cast<int>(arg(c, 2)), 1, 0x4000);
    ret(c, 0u);
}

void SasSetVolume(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    Voice &v = g_sas.voices[arg(c, 1)];
    v.volume_left = static_cast<int>(arg(c, 2));
    v.volume_right = static_cast<int>(arg(c, 3));
    v.effect_left = static_cast<int>(arg(c, 4));
    v.effect_right = static_cast<int>(arg(c, 5));
    ret(c, 0u);
}

void SasSetADSR(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    Envelope &e = g_sas.voices[arg(c, 1)].env;
    const u32 flag = arg(c, 2);
    if (flag & 1u) e.attack_rate = static_cast<int>(arg(c, 3));
    if (flag & 2u) e.decay_rate = static_cast<int>(arg(c, 4));
    if (flag & 4u) e.sustain_rate = static_cast<int>(arg(c, 5));
    if (flag & 8u) e.release_rate = static_cast<int>(arg(c, 6));
    ret(c, 0u);
}

void SasSetADSRmode(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    Envelope &e = g_sas.voices[arg(c, 1)].env;
    const u32 flag = arg(c, 2);
    if (flag & 1u) e.attack_type = static_cast<int>(arg(c, 3));
    if (flag & 2u) e.decay_type = static_cast<int>(arg(c, 4));
    if (flag & 4u) e.sustain_type = static_cast<int>(arg(c, 5));
    if (flag & 8u) e.release_type = static_cast<int>(arg(c, 6));
    ret(c, 0u);
}

void SasSetSL(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    g_sas.voices[arg(c, 1)].env.sustain_level = static_cast<int>(arg(c, 2));
    ret(c, 0u);
}

void SasSetSimpleADSR(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    set_simple_envelope(g_sas.voices[arg(c, 1)].env, arg(c, 2), arg(c, 3));
    ret(c, 0u);
}

void SasSetKeyOn(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    Voice &v = g_sas.voices[arg(c, 1)];
    v.env.state = KeyOnWait;
    if (v.type == VoiceType::Vag) v.vag.start(v.vag_addr, v.vag_size, v.loop);
    if (v.type == VoiceType::Pcm) v.pcm_index = 0;
    v.playing = true;
    v.on = true;
    v.paused = false;
    v.sample_frac = 0u;
    v.hist[0] = v.hist[1] = 0;
    ret(c, 0u);
}

void SasSetKeyOff(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    Voice &v = g_sas.voices[arg(c, 1)];
    if (v.playing && !v.paused) {
        v.on = false;
        v.env.set_state(Release);
    }
    ret(c, 0u);
}

void SasSetPause(AllegrexContext &c) {
    const u32 bits = arg(c, 1);
    const bool pause = arg(c, 2) != 0u;
    for (int i = 0; i < kVoices; ++i)
        if (bits & (1u << i)) g_sas.voices[static_cast<std::size_t>(i)].paused = pause;
    ret(c, 0u);
}

void SasGetPauseFlag(AllegrexContext &c) {
    u32 bits = 0u;
    for (int i = 0; i < kVoices; ++i)
        if (g_sas.voices[static_cast<std::size_t>(i)].paused) bits |= 1u << i;
    ret(c, bits);
}

void SasGetEndFlag(AllegrexContext &c) {
    u32 bits = 0u;
    for (int i = 0; i < kVoices; ++i)
        if (!g_sas.voices[static_cast<std::size_t>(i)].playing) bits |= 1u << i;
    ret(c, bits);
}

void SasGetEnvelopeHeight(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    ret(c, static_cast<u32>(g_sas.voices[arg(c, 1)].env.height));
}

void SasGetAllEnvelopeHeights(AllegrexContext &c) {
    const u32 out = arg(c, 1);
    for (u32 i = 0; i < static_cast<u32>(kVoices); ++i) mem().store32(out + i * 4u, static_cast<u32>(g_sas.voices[i].env.height));
    ret(c, 0u);
}

void SasSetNoise(AllegrexContext &c) {
    if (bad_voice(arg(c, 1))) { ret(c, ERROR_INVALID_VOICE); return; }
    g_sas.voices[arg(c, 1)].type = VoiceType::Noise;
    ret(c, 0u);
}

void SasSetGrain(AllegrexContext &c) { g_sas.grain = static_cast<int>(arg(c, 1)); ret(c, 0u); }
void SasGetGrain(AllegrexContext &c) { ret(c, static_cast<u32>(g_sas.grain)); }
void SasSetOutputmode(AllegrexContext &c) { g_sas.output_mode = static_cast<int>(arg(c, 1)); ret(c, 0u); }
void SasGetOutputmode(AllegrexContext &c) { ret(c, static_cast<u32>(g_sas.output_mode)); }
void SasReverbIgnored(AllegrexContext &c) { ret(c, 0u); }

} // namespace

void register_sas_imports() {
    const char *S = "sceSasCore";
    hle(S, "__sceSasInit", SasInit);
    hle(S, "__sceSasCore", SasCoreFn);
    hle(S, "__sceSasCoreWithMix", SasCoreWithMix);
    hle(S, "__sceSasSetVoice", SasSetVoice);
    hle(S, "__sceSasSetVoicePCM", SasSetVoicePCM);
    hle(S, "__sceSasSetPitch", SasSetPitch);
    hle(S, "__sceSasSetVolume", SasSetVolume);
    hle(S, "__sceSasSetADSR", SasSetADSR);
    hle(S, "__sceSasSetADSRmode", SasSetADSRmode);
    hle(S, "__sceSasSetSL", SasSetSL);
    hle(S, "__sceSasSetSimpleADSR", SasSetSimpleADSR);
    hle(S, "__sceSasSetKeyOn", SasSetKeyOn);
    hle(S, "__sceSasSetKeyOff", SasSetKeyOff);
    hle(S, "__sceSasSetPause", SasSetPause);
    hle(S, "__sceSasGetPauseFlag", SasGetPauseFlag);
    hle(S, "__sceSasGetEndFlag", SasGetEndFlag);
    hle(S, "__sceSasGetEnvelopeHeight", SasGetEnvelopeHeight);
    hle(S, "__sceSasGetAllEnvelopeHeights", SasGetAllEnvelopeHeights);
    hle(S, "__sceSasSetNoise", SasSetNoise);
    hle(S, "__sceSasSetGrain", SasSetGrain);
    hle(S, "__sceSasGetGrain", SasGetGrain);
    hle(S, "__sceSasSetOutputmode", SasSetOutputmode);
    hle(S, "__sceSasGetOutputmode", SasGetOutputmode);
    hle(S, "__sceSasRevType", SasReverbIgnored);
    hle(S, "__sceSasRevParam", SasReverbIgnored);
    hle(S, "__sceSasRevEVOL", SasReverbIgnored);
    hle(S, "__sceSasRevVON", SasReverbIgnored);
}

} // namespace p2
