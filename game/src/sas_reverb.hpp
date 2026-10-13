// SAS waveform effect (reverb): the PS1 SPU reverb network (nocash psx-spx
// formula) running at 22 kHz with the PSP's own preset tables. Preset values
// and the Echo/Delay parameter recompute follow PPSSPP's hardware findings.
#pragma once

#include "hle.hpp"

#include <vector>

namespace p2 {

class SasReverb {
public:
    SasReverb();
    // -1 = off, 0..8 = Room, Studio S/M/L, Hall, Space Echo, Echo, Delay, Half Echo.
    void set_preset(int preset);
    void set_params(int delay, int feedback);
    // input: 22 kHz stereo pairs; output: 44 kHz stereo (zero-stuffed), 2 frames per input frame.
    void process(s16 *output, const s16 *input, int frames, int vol_left, int vol_right);
    int preset() const { return preset_; }

private:
    struct Data {
        s32 size;
        s16 dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
        s16 vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
        s16 dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
        s16 dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
    };
    void apply_params();

    static constexpr int kBufSize = 0x20000;
    std::vector<s16> work_;
    Data data_{};
    int preset_{-1};
    int pos_{};
    int delay_{}, feedback_{};
};

} // namespace p2
