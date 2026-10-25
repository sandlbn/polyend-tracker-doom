// Per-block render cost attribution (2.x CPU telemetry, docs/OPTIMIZATION_2X.md
// "Peak attribution") [OT].
//
// While a live block renders (AudioEngine::fillCallback with a cycle source,
// cpu_meter.h) the engine stamps the cycle counter at its phase boundaries
// and counts the events of the block. The block with the highest total in
// each 1 s window (the CpuMeter window, 345 blocks) is kept; the 1 Hz
// "telemetry: cpu" line prints it, so a hardware log says exactly which part
// of the render a spike came from.
//
// Cycle fields (sum <= total; the rest is the stamps themselves and the
// meter / governor bookkeeping):
//   seq      pre-block hook (App::preBlock: line input, recorder tap,
//            sequencer) minus the note work below
//   smpOn    sample voice note-on / off / FX calls from the sequencer
//   synOn    synth note-on / off / FX / macro calls from the sequencer (the
//            framework plus the engines' noteOn)
//   ctrl     per-block control (mixer parameters, sample voice control)
//   smp      sample voice render (8 tracks + metronome)
//   synFw    synth framework in render: sync / reloadParams / catchUp /
//            macros, voice bookkeeping, pending starts, slot mix
//   slot[s]  engine prepare + voice renders of synth slot s
//   dly, rev delay and reverb
//   out      line input / sends, master sum, limiter, output, stems
// Event counts saturate at 255.
// seqWhy / nPatTracks / nMidiOut say what the seq phase did (CPU round 3:
// the device showed 140k-cycle seq blocks with no note at all; a whole
// 16-track pattern copy SDRAM -> SDRAM costs ~140k cycles there).
#pragma once

#include <atomic>
#include <cstdint>

namespace ot {
namespace audio {

// BlockProfile::seqWhy bits (telemetry letters in brackets).
enum SeqWork : uint8_t {
    kSeqPreloadPart = 1u << 0,   // [P] part of a spread preload (a few tracks)
    kSeqWholeLoad = 1u << 1,     // [L] a whole pattern copied in the block
    kSeqPatternEnd = 1u << 2,    // [N] pattern end: swap / song advance
    kSeqEngineApply = 1u << 3,   // [E] engine settings re-applied (project / config edit)
    kSeqRecorder = 1u << 4,      // [R] recorder input tap recording
    kSeqLiveRecord = 1u << 5,    // [W] live record wrote a step into the pattern
};

struct BlockProfile {
    static constexpr int kSlots = 3;
    uint32_t total = 0;
    uint32_t seq = 0, smpOn = 0, synOn = 0, ctrl = 0, smp = 0, synFw = 0;
    uint32_t slot[kSlots] = {};
    uint32_t dly = 0, rev = 0, out = 0;
    uint32_t voiceMax = 0;        // costliest single synth voice render this block
    uint8_t nSmpOn = 0;           // sample note-ons
    uint8_t nSynOn[kSlots] = {};  // synth note-ons per slot (on a PERC slot: drum hits)
    uint8_t nReload = 0;          // reloadParams (patch edit / engine bind)
    uint8_t nFullCopy = 0;        // catchUp copying every parameter
    uint8_t nMacro = 0;           // applyMacros recomputes
    uint8_t nSteal = 0;           // voices put into their steal fade
    uint8_t nPendStart = 0;       // queued notes started after a fade
    uint8_t nDeferred = 0;        // synth note-ons queued to the next block (per-block cap)
    uint8_t nSynVoices = 0;       // synth records rendered (sounding + fading)
    uint8_t nSmpVoices = 0;       // sample voices rendered
    uint8_t seqWhy = 0;           // SeqWork bits of the seq phase
    uint8_t nPatTracks = 0;       // pattern tracks copied in the seq phase (769 B each)
    uint8_t nMidiOut = 0;         // MIDI messages the sequencer sent
    int32_t slotEngine[kSlots] = {};   // Tracker Mini engine id of each slot (0: none)

    static void inc(uint8_t& c) { if (c != 255) ++c; }
    static void add(uint8_t& c, uint32_t n) { c = uint8_t(c + n > 255u ? 255u : c + n); }
};

// Peak block per window, written by the audio context, read by the main loop
// (a sequence count: odd while the audio context writes the snapshot).
class ProfileWindow {
public:
    // Audio context, after every timed block.
    void add(const BlockProfile& b, bool windowEnd) {
        if (b.total >= cur_.total) cur_ = b;
        if (b.voiceMax > curVoice_) curVoice_ = b.voiceMax;
        if (!windowEnd) return;
        seq_ = seq_ + 1;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        last_ = cur_;
        lastVoice_ = curVoice_;
        std::atomic_signal_fence(std::memory_order_seq_cst);
        seq_ = seq_ + 1;
        cur_ = BlockProfile{};
        curVoice_ = 0;
    }
    // Main loop: the peak block of the last complete window and the
    // costliest synth voice render in it. False until a window completed.
    bool read(BlockProfile& out, uint32_t& voiceMax) const {
        for (int tries = 0; tries < 4; ++tries) {
            const uint32_t s0 = seq_;
            if (s0 & 1u) continue;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            out = last_;
            voiceMax = lastVoice_;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            if (seq_ == s0) return s0 != 0;
        }
        return false;
    }
    void reset() { *this = ProfileWindow{}; }

private:
    BlockProfile cur_, last_;
    uint32_t curVoice_ = 0, lastVoice_ = 0;
    volatile uint32_t seq_ = 0;
};

}  // namespace audio
}  // namespace ot
