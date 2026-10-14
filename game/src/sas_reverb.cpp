#include "sas_reverb.hpp"
#include "savestate.hpp"

#include <algorithm>
#include <cstring>

namespace p2 {
namespace {

s16 clamp16(int v) { return static_cast<s16>(std::clamp(v, -32768, 32767)); }

#define S(x) static_cast<s16>(x)
// Field order matches SasReverb::Data. Six presets equal the PS1's; Room and the
// Echo/Delay entries carry the PSP's values (including real diff-side taps).
const s16 kPresets[9][31] = {
    // Room
    {S(0x26C0 & 0), 0x007D, 0x005B, 0x6D80, 0x54B8, S(0xBED0), 0x0000, 0x0000, S(0xBA80), 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227,
     0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C},
    // Studio Small
    {0, 0x0033, 0x0025, 0x70F0, 0x4FA8, S(0xBCE0), 0x4410, S(0xC0F0), S(0x9C00), 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372,
     0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026},
    // Studio Medium
    {0, 0x00B1, 0x007F, 0x70F0, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0xB4C0), 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2,
     0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080},
    // Studio Large
    {0, 0x00E3, 0x00A9, 0x6F60, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0xA680), 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9,
     0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA},
    // Hall
    {0, 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, S(0xB800), S(0xBC00), S(0xC000), 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC,
     0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A},
    // Space Echo
    {0, 0x033D, 0x0231, 0x7E00, 0x5000, S(0xB400), S(0xB000), 0x4C00, S(0xB000), 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2,
     0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232},
    // Echo (almost infinite)
    {0, 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, S(0x8100), 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000,
     0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004},
    // Delay (one-shot echo)
    {0, 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000,
     0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004},
    // Half Echo
    {0, 0x0017, 0x0013, 0x70F0, 0x4FA8, S(0xBCE0), 0x4510, S(0xBEF0), S(0x8500), 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0,
     0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014},
};
#undef S
const s32 kPresetSizes[9] = {0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};
constexpr int kEcho = 6, kDelay = 7;

} // namespace

SasReverb::SasReverb() : work_(kBufSize, 0) {}

void SasReverb::set_preset(int preset) {
    if (preset < -1 || preset > 8) return;
    preset_ = preset;
    pos_ = preset_ >= 0 ? kBufSize - kPresetSizes[preset_] : 0;
    std::fill(work_.begin(), work_.end(), s16{0});
    apply_params();
}

void SasReverb::set_params(int delay, int feedback) {
    delay_ = delay;
    feedback_ = feedback;
    apply_params();
}

void SasReverb::apply_params() {
    if (preset_ < 0) return;
    data_.size = kPresetSizes[preset_];
    std::memcpy(&data_.dAPF1, &kPresets[preset_][1], 30 * sizeof(s16));
    if (preset_ != kEcho && preset_ != kDelay) return;
    // Echo/Delay: the hardware recomputes these taps from sceSasRevParam.
    const int d16 = (delay_ + 1) * 16;
    data_.mLSAME = static_cast<s16>(d16 * 2 - data_.dAPF1);
    data_.mRSAME = static_cast<s16>(d16 - data_.dAPF2);
    data_.mLCOMB1 = static_cast<s16>(data_.mRCOMB1 + d16);
    data_.dLSAME = static_cast<s16>(data_.dRSAME + d16);
    data_.mLAPF1 = static_cast<s16>(data_.mLAPF2 + d16);
    data_.mRAPF1 = static_cast<s16>(d16 + (data_.mLAPF2 >> 1));
    data_.vWALL = static_cast<s16>(-(feedback_ << 8));
}

void SasReverb::process(s16 *output, const s16 *input, int frames, int vol_left, int vol_right) {
    if (preset_ < 0) {
        for (int i = 0; i < frames; ++i) {
            output[i * 4 + 0] = output[i * 4 + 2] = clamp16(input[i * 2] * vol_left >> 15);
            output[i * 4 + 1] = output[i * 4 + 3] = clamp16(input[i * 2 + 1] * vol_right >> 15);
        }
        return;
    }
    // The return path is (evol * out) >> 11; the caller passes evol << 3.
    vol_left <<= 1;
    vol_right <<= 1;
    const Data &d = data_;
    const int base = kBufSize - d.size;
    auto at = [&](int index) -> s16 & {
        int addr = pos_ + index;
        if (addr >= kBufSize) addr -= d.size;
        if (addr < base) addr += d.size;
        return work_[static_cast<std::size_t>(addr)];
    };
    for (int i = 0; i < frames; ++i) {
        // The ME feeds the reverb at a quarter of the mix level.
        const int lin = input[i * 2] >> 2, rin = input[i * 2 + 1] >> 2;
        // Same-side and cross-side reflections.
        at(d.mLSAME) = clamp16(lin + (at(d.dLSAME) * d.vWALL >> 15) - (at(d.mLSAME - 1) * d.vIIR >> 15) + at(d.mLSAME - 1));
        at(d.mRSAME) = clamp16(rin + (at(d.dRSAME) * d.vWALL >> 15) - (at(d.mRSAME - 1) * d.vIIR >> 15) + at(d.mRSAME - 1));
        at(d.mLDIFF) = clamp16(lin + (at(d.dRDIFF) * d.vWALL >> 15) - (at(d.mLDIFF - 1) * d.vIIR >> 15) + at(d.mLDIFF - 1));
        at(d.mRDIFF) = clamp16(rin + (at(d.dLDIFF) * d.vWALL >> 15) - (at(d.mRDIFF - 1) * d.vIIR >> 15) + at(d.mRDIFF - 1));
        // Early echo (comb filters).
        int lout = (d.vCOMB1 * at(d.mLCOMB1) + d.vCOMB2 * at(d.mLCOMB2) + d.vCOMB3 * at(d.mLCOMB3) + d.vCOMB4 * at(d.mLCOMB4)) >> 15;
        int rout = (d.vCOMB1 * at(d.mRCOMB1) + d.vCOMB2 * at(d.mRCOMB2) + d.vCOMB3 * at(d.mRCOMB3) + d.vCOMB4 * at(d.mRCOMB4)) >> 15;
        // Late reverb: two all-pass stages.
        at(d.mLAPF1) = clamp16(lout - (d.vAPF1 * at(d.mLAPF1 - d.dAPF1) >> 15));
        lout = at(d.mLAPF1 - d.dAPF1) + (at(d.mLAPF1) * d.vAPF1 >> 15);
        at(d.mRAPF1) = clamp16(rout - (d.vAPF1 * at(d.mRAPF1 - d.dAPF1) >> 15));
        rout = at(d.mRAPF1 - d.dAPF1) + (at(d.mRAPF1) * d.vAPF1 >> 15);
        at(d.mLAPF2) = clamp16(lout - (d.vAPF2 * at(d.mLAPF2 - d.dAPF2) >> 15));
        lout = at(d.mLAPF2 - d.dAPF2) + (at(d.mLAPF2) * d.vAPF2 >> 15);
        at(d.mRAPF2) = clamp16(rout - (d.vAPF2 * at(d.mRAPF2 - d.dAPF2) >> 15));
        rout = at(d.mRAPF2 - d.dAPF2) + (at(d.mRAPF2) * d.vAPF2 >> 15);
        output[i * 4 + 0] = clamp16(lout * vol_left >> 15);
        output[i * 4 + 1] = clamp16(rout * vol_right >> 15);
        output[i * 4 + 2] = 0;
        output[i * 4 + 3] = 0;
        if (++pos_ >= kBufSize) pos_ -= d.size;
    }
}

void SasReverb::save(StateWriter &w) const {
    w.vec(work_);
    w.pod(data_);
    w.pod(preset_);
    w.pod(pos_);
    w.pod(delay_);
    w.pod(feedback_);
}

void SasReverb::load(StateReader &r) {
    r.vec(work_);
    r.pod(data_);
    r.pod(preset_);
    r.pod(pos_);
    r.pod(delay_);
    r.pod(feedback_);
}

} // namespace p2
