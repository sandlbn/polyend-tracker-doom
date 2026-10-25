// Peak trace generator (make -C tests peak-trace; docs/OPTIMIZATION_2X.md
// "Peak attribution"): plays TESTSONGS song 5 (OT Test 05 - Synths) through
// the whole App on the host and writes everything the audio engine saw into
// one file, so tests/audio_bench.cpp (host bench and bench-m4) can replay
// the song's audio blocks on the engine alone:
//   - the engine state at the start (mixer settings, the 48 sample
//     instruments and their sample data, the three synth patches and slot
//     records, the governor budget);
//   - per audio block, the sequencer sink calls of that block (note on / off,
//     FX, tempo, synth calls; MIDI is not traced) and the song position.
// Usage: peak_trace <out file> [seconds]   (default: the whole song, 46 s)
// It also reports the seq phase of the App's pre-block hook per block (the
// block profile's seqWhy / nPatTracks, audio/block_profile.h): the device
// showed ~140k-cycle seq blocks without a note, one per pattern (a whole
// pattern copied SDRAM -> SDRAM in the audio block). The bench replay does
// not run the sequencer, so this is where that cost is seen on the host.
// The format is little-endian and raw struct images (both sides are 32/64-bit
// little-endian with the same layout of these plain structs; the header
// carries their sizes and the reader refuses a mismatch). peak_trace.h has
// the format.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "peak_trace.h"
#include "ui_fakes.h"

#include "../tools/make_testsongs/stock_check.cpp"
#include "../tools/make_testsongs/testsongs.cpp"

namespace {

std::vector<uint8_t> gOut;
void put(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    gOut.insert(gOut.end(), b, b + n);
}
template <class T>
void putv(const T& v) { put(&v, sizeof v); }
void ev(uint8_t kind, const void* p = nullptr, uint8_t n = 0) {
    gOut.push_back(kind);
    gOut.push_back(n);
    if (n) put(p, n);
}

// Forwards every call to the engine and records it.
struct TraceSink : ot::ISequencerSink {
    ot::audio::AudioEngine* e = nullptr;
    void voiceNoteOn(uint8_t track, const ot::VoiceNoteOn& n) override {
        uint8_t b[1 + sizeof n];
        b[0] = track;
        std::memcpy(b + 1, &n, sizeof n);
        ev(ot::peak::kVoiceOn, b, sizeof b);
        e->voiceNoteOn(track, n);
    }
    void voiceNoteOff(uint8_t track, ot::NoteOffMode mode) override {
        const uint8_t b[2] = {track, uint8_t(int8_t(mode))};
        ev(ot::peak::kVoiceOff, b, 2);
        e->voiceNoteOff(track, mode);
    }
    void voiceApplyFx(uint8_t track, uint8_t slot, uint8_t type, uint8_t value) override {
        const uint8_t b[4] = {track, slot, type, value};
        ev(ot::peak::kVoiceFx, b, 4);
        e->voiceApplyFx(track, slot, type, value);
    }
    void resetAllVoiceFx() override {
        ev(ot::peak::kResetFx);
        e->resetAllVoiceFx();
    }
    void engineBlockUpdate(float bpm) override {
        ev(ot::peak::kTempo, &bpm, 4);
        e->engineBlockUpdate(bpm);
    }
    void metronomeClick(bool accent, uint8_t velocity) override {
        const uint8_t b[2] = {uint8_t(accent), velocity};
        ev(ot::peak::kClick, b, 2);
        e->metronomeClick(accent, velocity);
    }
    void midiSend(uint8_t portMask, const uint8_t* bytes, uint8_t len) override { e->midiSend(portMask, bytes, len); }
    void synthNoteOn(uint8_t track, const ot::SynthNoteOn& n) override {
        uint8_t b[1 + sizeof n];
        b[0] = track;
        std::memcpy(b + 1, &n, sizeof n);
        ev(ot::peak::kSynthOn, b, sizeof b);
        e->synthNoteOn(track, n);
    }
    void synthNoteOff(uint8_t track, ot::NoteOffMode mode) override {
        const uint8_t b[2] = {track, uint8_t(int8_t(mode))};
        ev(ot::peak::kSynthOff, b, 2);
        e->synthNoteOff(track, mode);
    }
    void synthApplyFx(uint8_t track, uint8_t type, uint8_t value) override {
        const uint8_t b[3] = {track, type, value};
        ev(ot::peak::kSynthFx, b, 3);
        e->synthApplyFx(track, type, value);
    }
    void synthMacroFx(uint8_t slot, uint8_t macro, uint8_t value) override {
        const uint8_t b[3] = {slot, macro, value};
        ev(ot::peak::kSynthMacro, b, 3);
        e->synthMacroFx(slot, macro, value);
    }
    void synthTransportStop() override {
        ev(ot::peak::kSynthStop);
        e->synthTransportStop();
    }
};

// Deterministic stand-in cycle source: the block profile (seqWhy, pattern
// tracks) is filled only for timed blocks; one count per call keeps the CPU
// meter near zero, so the governor never cuts a voice.
uint32_t gFakeCycles = 0;
uint32_t fakeCycles() { return ++gFakeCycles; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: peak_trace <out> [seconds]\n");
        return 2;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 46.5;
    std::unique_ptr<Harness> hp(new Harness(false, ot::kSampleBankBytes / 2));
    Harness& h = *hp;
    h.disp.customFonts = true;
    h.app->boot();
    ts::GenOptions opt;
    std::string err;
    if (!ts::generateSong(*h.app, h.fs, ts::SongId::Synths, opt, err)) {
        std::fprintf(stderr, "peak_trace: generate failed: %s\n", err.c_str());
        return 1;
    }
    const ts::SongInfo* info = ts::findSong(ts::SongId::Synths);
    if (!h.app->newProject() || !h.app->openProject(ot::kProjectsDir, info->name)) {
        std::fprintf(stderr, "peak_trace: open failed\n");
        return 1;
    }
    h.run(50);
    h.audio.pump(2);   // engine settings applied (App::preBlock)
    ot::App& a = *h.app;
    ot::audio::AudioEngine& eng = a.engine;

    // --- header + engine state ---
    put(ot::peak::kMagic, 4);
    putv(ot::peak::kVersion);
    const uint32_t sizes[5] = {uint32_t(sizeof(ot::audio::MixerSettings)), uint32_t(sizeof(ot::Instrument)),
                               uint32_t(sizeof(ot::SynthPatch)), uint32_t(sizeof(ot::VoiceNoteOn)),
                               uint32_t(sizeof(ot::SynthNoteOn))};
    put(sizes, sizeof sizes);
    putv(eng.settings());
    for (int i = 0; i < ot::kSampleInstrumentCount; ++i) putv(a.instruments[i]);
    uint32_t loaded = 0;
    for (int i = 0; i < ot::kSampleInstrumentCount; ++i) loaded += eng.bank().loaded(i) ? 1u : 0u;
    putv(loaded);
    for (int i = 0; i < ot::kSampleInstrumentCount; ++i) {
        if (!eng.bank().loaded(i)) continue;
        const uint32_t idx = uint32_t(i), frames = eng.bank().frames(i);
        const uint8_t st = eng.bank().stereo(i) ? 1 : 0;
        putv(idx);
        putv(frames);
        putv(st);
        put(eng.bank().left(i), frames * 2u);
        if (st) put(eng.bank().right(i), frames * 2u);
    }
    for (int s = 0; s < 3; ++s) {
        putv(a.synthPatch(s));
        uint8_t rec[ot::mt::kSynthSlotSize];
        a.synthSlot(s).encode(rec);
        put(rec, sizeof rec);
    }
    const int32_t budget = eng.governor().budget();
    putv(budget);

    // --- blocks ---
    TraceSink sink;
    sink.e = &eng;
    a.seq.attach(&sink, &a, &a.project, a.seq.currentPattern(), a.seq.preloadPattern(), a.seq.stagePattern());
    a.seq.playSong(0);
    const uint32_t blocks = uint32_t(seconds * 44117.6 / 128.0);
    uint32_t done = 0, voices = 0;
    // Seq-phase work: blocks with pattern copies, the most tracks in one block.
    uint32_t patBlocks = 0, patTracks = 0, maxTracks = 0, maxAt = 0, ends = 0;
    uint8_t maxWhy = 0;
    ot::audio::CpuMeter::setCycleSource(&fakeCycles);
    for (; done < blocks && a.seq.isPlaying(); ++done) {
        h.audio.pump(1);
        const ot::audio::BlockProfile& bp = eng.lastBlockProfile();
        if (bp.nPatTracks) {
            ++patBlocks;
            patTracks += bp.nPatTracks;
        }
        if (bp.seqWhy & ot::audio::kSeqPatternEnd) ++ends;
        if (bp.nPatTracks > maxTracks) {
            maxTracks = bp.nPatTracks;
            maxAt = done;
            maxWhy = bp.seqWhy;
        }
        const uint8_t tag[3] = {uint8_t(a.seq.songPosition()), uint8_t(a.seq.masterStep() & 0xFF),
                                uint8_t((a.seq.masterStep() >> 8) & 0xFF)};
        ev(ot::peak::kBlockEnd, tag, 3);
        if ((done & 31) == 0) h.run(10);   // main loop (UI, jobs) between blocks, ~ device rate
        const int sv = eng.synth().sounding();
        voices = uint32_t(sv) > voices ? uint32_t(sv) : voices;
    }
    ot::audio::CpuMeter::setCycleSource(nullptr);
    a.seq.stop();
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f || std::fwrite(gOut.data(), 1, gOut.size(), f) != gOut.size()) {
        std::fprintf(stderr, "peak_trace: cannot write %s\n", argv[1]);
        return 1;
    }
    std::fclose(f);
    std::printf("peak_trace: %s: %u blocks of \"%s\", %zu bytes, synth steals %u, max %u synth voices\n", argv[1],
                unsigned(done), info->name, gOut.size(), unsigned(eng.synth().steals()), unsigned(voices));
    // ~8.75k device cycles per 769-byte track (140k for 16 in diag-185237).
    std::printf("peak_trace: seq phase: %u pattern ends, %u blocks copied %u pattern tracks; most in one block: "
                "%u tracks (block %u, seqWhy 0x%02X) = ~%uk device cycles\n",
                unsigned(ends), unsigned(patBlocks), unsigned(patTracks), unsigned(maxTracks), unsigned(maxAt),
                unsigned(maxWhy), unsigned(maxTracks * 8750u / 1000u));
    return 0;
}
