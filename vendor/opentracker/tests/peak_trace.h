// Song 5 peak trace: format (written by tests/peak_trace.cpp) and replay
// (tests/audio_bench.cpp "peak", host and bench-m4).
//
// File: "OTPK", u32 version, u32 sizes[5] (MixerSettings, Instrument,
// SynthPatch, VoiceNoteOn, SynthNoteOn), MixerSettings, Instrument x 48,
// u32 n + n x {u32 index, u32 frames, u8 stereo, int16 left[frames]
// [, int16 right[frames]]}, 3 x {SynthPatch, u8 record[0x40]}, i32 budget,
// then the event stream: {u8 kind, u8 len, len bytes}; kind kBlockEnd closes
// a block (payload: song position, master step lo / hi).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "audio/engine.h"
#include "model/project.h"

namespace ot {
namespace peak {

constexpr char kMagic[4] = {'O', 'T', 'P', 'K'};
constexpr uint32_t kVersion = 1;
enum Kind : uint8_t {
    kVoiceOn = 1, kVoiceOff, kVoiceFx, kResetFx, kTempo, kClick, kSynthOn, kSynthOff, kSynthFx, kSynthMacro,
    kSynthStop, kBlockEnd = 0xFF,
};

// Replays a trace into an engine. The caller owns every buffer (the bench
// keeps them in PSRAM on the M4): `file` the whole trace, `instruments` 48
// entries, `patches` 3. load() initialises the engine state; then each
// applyBlock() (called from the engine's pre-block hook, where the device's
// sequencer runs) applies the next block's events.
class Replay {
public:
    bool load(const uint8_t* file, size_t size, audio::AudioEngine& e, Instrument* instruments, SynthPatch* patches,
              char* why, size_t whyCap) {
        p_ = file;
        end_ = file + size;
        e_ = &e;
        auto fail = [&](const char* m) {
            std::snprintf(why, whyCap, "%s", m);
            return false;
        };
        if (size < 8 || std::memcmp(file, kMagic, 4) != 0) return fail("not a peak trace");
        p_ += 4;
        uint32_t ver = 0, sizes[5] = {};
        if (!get(&ver, 4) || ver != kVersion) return fail("trace version");
        if (!get(sizes, sizeof sizes)) return fail("short header");
        const uint32_t want[5] = {uint32_t(sizeof(audio::MixerSettings)), uint32_t(sizeof(Instrument)),
                                  uint32_t(sizeof(SynthPatch)), uint32_t(sizeof(VoiceNoteOn)),
                                  uint32_t(sizeof(SynthNoteOn))};
        if (std::memcmp(sizes, want, sizeof want) != 0) return fail("struct sizes differ from the writer's");
        audio::MixerSettings ms;
        if (!get(&ms, sizeof ms)) return fail("short settings");
        for (int i = 0; i < kSampleInstrumentCount; ++i)
            if (!get(&instruments[i], sizeof(Instrument))) return fail("short instruments");
        e.bank().clear();
        e.stopAll();
        e.clearEffects();
        uint32_t n = 0;
        if (!get(&n, 4)) return fail("short bank");
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t idx = 0, frames = 0;
            uint8_t st = 0;
            if (!get(&idx, 4) || !get(&frames, 4) || !get(&st, 1) || idx >= uint32_t(kSampleInstrumentCount))
                return fail("bad bank entry");
            const size_t bytes = size_t(frames) * 2u * (st ? 2u : 1u);
            if (size_t(end_ - p_) < bytes) return fail("short sample data");
            int16_t* dst = e.bank().reserve(int(idx), frames, st != 0);
            if (!dst) return fail("sample bank full");
            std::memcpy(dst, p_, bytes);
            p_ += bytes;
        }
        audio::SynthBank& b = e.synth();
        b.reset();
        b.setEngineOverride(nullptr);
        for (int s = 0; s < 3; ++s) {
            uint8_t rec[mt::kSynthSlotSize];
            if (!get(&patches[s], sizeof(SynthPatch)) || !get(rec, sizeof rec)) return fail("short synth slots");
            SynthSlotRecord r;
            r.decode(rec);
            b.bindPatch(s, &patches[s]);
            b.setSlotRecord(s, r);
        }
        int32_t budget = 0;
        if (!get(&budget, 4)) return fail("short budget");
        e.settings() = ms;
        e.governor().setBudget(budget);
        events_ = p_;
        blocks_ = 0;
        for (const uint8_t* q = p_; q + 2 <= end_; q += 2 + q[1])
            if (q[0] == kBlockEnd) ++blocks_;
        return true;
    }
    uint32_t blocks() const { return blocks_; }
    // Back to the first block (the engine state is not restored).
    void rewind() { p_ = events_; }
    // Applies the next block's events; false at the end. pos / step: the
    // song position and master step at the end of that block.
    bool applyBlock(int* pos = nullptr, int* step = nullptr) {
        while (p_ + 2 <= end_) {
            const uint8_t kind = p_[0], len = p_[1];
            const uint8_t* d = p_ + 2;
            p_ += 2 + len;
            switch (kind) {
                case kVoiceOn: {
                    VoiceNoteOn n;
                    std::memcpy(&n, d + 1, sizeof n);
                    e_->voiceNoteOn(d[0], n);
                    break;
                }
                case kVoiceOff: e_->voiceNoteOff(d[0], NoteOffMode(int8_t(d[1]))); break;
                case kVoiceFx: e_->voiceApplyFx(d[0], d[1], d[2], d[3]); break;
                case kResetFx: e_->resetAllVoiceFx(); break;
                case kTempo: {
                    float t;
                    std::memcpy(&t, d, 4);
                    e_->engineBlockUpdate(t);
                    break;
                }
                case kClick: e_->metronomeClick(d[0] != 0, d[1]); break;
                case kSynthOn: {
                    SynthNoteOn n;
                    std::memcpy(&n, d + 1, sizeof n);
                    e_->synthNoteOn(d[0], n);
                    break;
                }
                case kSynthOff: e_->synthNoteOff(d[0], NoteOffMode(int8_t(d[1]))); break;
                case kSynthFx: e_->synthApplyFx(d[0], d[1], d[2]); break;
                case kSynthMacro: e_->synthMacroFx(d[0], d[1], d[2]); break;
                case kSynthStop: e_->synthTransportStop(); break;
                case kBlockEnd:
                    if (pos) *pos = d[0];
                    if (step) *step = int(d[1]) | (int(d[2]) << 8);
                    return true;
                default: break;
            }
        }
        return false;
    }

private:
    bool get(void* dst, size_t n) {
        if (size_t(end_ - p_) < n) return false;
        std::memcpy(dst, p_, n);
        p_ += n;
        return true;
    }
    const uint8_t* p_ = nullptr;
    const uint8_t* end_ = nullptr;
    const uint8_t* events_ = nullptr;
    uint32_t blocks_ = 0;
    audio::AudioEngine* e_ = nullptr;
};

}  // namespace peak
}  // namespace ot
