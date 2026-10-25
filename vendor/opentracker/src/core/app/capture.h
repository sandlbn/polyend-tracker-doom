// Input capture tap for the Sample Recorder (spec 05 §2.11 and appendix C
// §2). Runs in the audio-block context (App::preBlock): it receives the
// same line-input block the engine gets (HalRefs::lineIn), applies the
// recorder gain, meters it and, while recording, appends a mono mix
// (spec 04 §5.7 "recording sums to mono") to a caller-provided buffer
// (the free end of the sample bank). 2.x stereo recording (2x-04 §5,
// revived as an OpenTracker extra): with start(..., stereo) the buffer
// holds two planar lanes, L at buf and R at buf + capacity(). Count-in
// clicks go through the engine metronome. No allocation, no locking: the UI polls the volatile
// state fields.
#pragma once

#include <cstdint>

namespace ot {

namespace audio {
class AudioEngine;
}

class Capture {
public:
    enum State : uint8_t { kIdle = 0, kCountIn = 1, kRecording = 2 };

    // Recording buffer (int16 samples). Not real-time: call while idle.
    void setBuffer(int16_t* buf, uint32_t capacitySamples);
    int16_t* buffer() const { return buf_; }                 // left (or mono) lane
    // Right lane of a stereo recording, nullptr in mono.
    int16_t* bufferR() const { return stereo_ && buf_ ? buf_ + cap_ / 2 : nullptr; }
    // Frames per channel in the latched mode; capacityFor() for a mode
    // before start(); bufferSamples() = the setBuffer() size.
    uint32_t capacity() const { return capacityFor(stereo_); }
    uint32_t capacityFor(bool stereo) const { return stereo ? cap_ / 2 : cap_; }
    uint32_t bufferSamples() const { return cap_; }
    bool stereo() const { return stereo_; }
    // Stereo metering before a recording (setup page); start() latches the
    // recording mode itself. Idle only.
    void setStereo(bool on) {
        if (state_ == kIdle) stereo_ = on;
    }

    // The recorder gain is analog, in the codec (app/input_gain.h, spec 22
    // §3); the captured input is not scaled again here.
    // Line-in channel config (0 stereo -> (L+R)/2, 1 left, 2 right).
    void setChannelMode(uint8_t m) { chMode_ = m > 2 ? 0 : m; }

    // Metering on/off (recorder screen open).
    void setMetering(bool on) { metering_ = on; }
    bool metering() const { return metering_; }
    // Peak |x| (0..1) since the previous call (UI meter, appendix C §2.6):
    // side 0 left, 1 right (in mono both are the recorded mix); takePeak()
    // = the larger of the two, clearing both.
    float takePeak(int side);
    float takePeak();

    // Start: optional count-in of `steps` sequencer steps of `stepSeconds`,
    // then record. steps = 0 records immediately. Metronome (spec 07 Q10
    // case 3): on every step boundary s (counted from the start) during the
    // count-in, and also while recording when `clickWhileRecording` (the
    // Metronome is on and the source is not the radio): accent if
    // s % barSteps == 0, else normal if s % beatSteps == 0.
    // `stereo` (2.x): record L and R into two lanes instead of the mix;
    // latched until the next start().
    void start(uint32_t steps, float stepSeconds, uint32_t beatSteps, uint32_t barSteps = 0,
               bool clickWhileRecording = false, bool stereo = false);
    void stop();
    State state() const { return State(state_); }
    bool recording() const { return state_ == kRecording; }
    bool active() const { return state_ != kIdle; }
    bool full() const { return full_; }
    uint32_t length() const { return len_; }
    uint32_t countInLeft() const { return countLeft_; }   // frames

    // Audio context: one 128-frame interleaved stereo block (may be null =
    // silence). eng may be null (no clicks).
    void process(const int16_t* interleaved, uint32_t frames, audio::AudioEngine* eng);

private:
    int16_t* buf_ = nullptr;
    uint32_t cap_ = 0;
    volatile uint32_t len_ = 0;
    volatile uint8_t state_ = kIdle;
    volatile bool full_ = false;
    volatile bool metering_ = false;
    volatile float peakL_ = 0.0f, peakR_ = 0.0f;
    uint8_t chMode_ = 0;
    bool stereo_ = false;
    // count-in
    volatile uint32_t countLeft_ = 0;
    uint32_t stepFrames_ = 0, beatSteps_ = 1, barSteps_ = 1;
    uint32_t countPos_ = 0;
    bool clickRec_ = false;
};

// Auto-trim after a recording (spec 05 appendix C §2.10): start = first
// frame whose smoothed |x| envelope exceeds 32767*10^-3.5 minus 44 frames,
// end = last such frame + 44 frames (exclusive end, clamped to len). With
// no frame above the threshold the full range is kept.
void autoTrim(const int16_t* d, uint32_t len, uint32_t* start, uint32_t* end);
// 2.x stereo: both lanes trimmed, start = the earlier, end = the later
// A lane that never crosses the threshold is ignored (else a silent R
// would keep the whole take); both silent = the full range.
// r nullptr = autoTrim(l).
void autoTrimLR(const int16_t* l, const int16_t* r, uint32_t len, uint32_t* start, uint32_t* end);

// Level meter units 0..100 from a peak 0..1 (appendix C §2.6).
// SPEC GAP: the 256-entry table 0xa97c4 is not recovered; OpenTracker maps
// -60..-3 dBFS linearly to 0..85 and -3..0 dBFS to 85..100 (>= 85 = clip
// colour, spec thresholds 61/71/85).
int meterUnits(float peak);

}  // namespace ot
