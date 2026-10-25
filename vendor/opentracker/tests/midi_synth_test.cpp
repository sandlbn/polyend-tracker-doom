// MIDI Synthesizer / MIDI Mapping mode (spec 17 §1): note input, voice
// allocation, chord step entry, CC map incl. CC 16-19, CC A..F value names,
// MIDI Start on the Song screen (STRINGS_DIFF 1.9.3 item 2).
#include <cstring>
#include <string>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "app/midi_cc.h"
#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

static PatternScreen& pe(Harness& h) { return *static_cast<PatternScreen*>(h.app->ui.screen(kModPattern)); }

static void addWav(Harness& h, const char* path, uint32_t frames) {
    std::vector<int16_t> s(frames);
    for (uint32_t i = 0; i < frames; ++i) s[i] = int16_t((i % 100) * 300 - 15000);
    std::vector<uint8_t> out(frames * 2 + 64);
    size_t n = wavEncode(s.data(), frames, 1, out.data(), out.size());
    out.resize(n);
    h.fs.files[path] = out;
}

static void send(Harness& h, std::initializer_list<uint8_t> b) {
    h.midi.in.insert(h.midi.in.end(), b);
    h.run(5);
}
static void noteOn(Harness& h, uint8_t n, uint8_t v = 100) { send(h, {0x90, n, v}); }
static void noteOff(Harness& h, uint8_t n) { send(h, {0x80, n, 0}); }

static int busyTracks(App& a) {
    int c = 0;
    for (int n = 0; n < 128; ++n)
        if (a.auditionTrack(uint16_t(App::kAuditionMidiIn + n)) >= 0) ++c;
    return c;
}

// Velocity of the last MIDI note-on in the sent stream, or -1.
static int lastNoteOnVelocity(Harness& h) {
    int v = -1;
    for (size_t i = 0; i + 2 < h.midi.sent.size(); ++i)
        if ((h.midi.sent[i] & 0xF0) == 0x90 && h.midi.sent[i + 2]) v = h.midi.sent[i + 2];
    return v;
}

static void loadSample(Harness& h, int slot) {
    addWav(h, "tone.wav", 20000);
    CHECK(h.app->importSample("tone.wav", slot));
}

TEST(sample_instrument_polyphony_8_no_stealing) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    loadSample(h, 0);
    a.setCurrentInstrument(0);
    a.editorTrack = 3;
    pe(h).track = 3;
    // Same note twice takes two tracks; 8 voices from the cursor track, wrap.
    for (int i = 0; i < 8; ++i) noteOn(h, uint8_t(60 + (i == 1 ? 0 : i)));
    CHECK_EQ(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60), 3), 3);
    for (int t = 0; t < 8; ++t) CHECK(a.engine.voice(t).active());
    // Ninth note: no free track, dropped (no stealing).
    noteOn(h, 90);
    CHECK_EQ(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 90)), -1);
    // Release one, the next note takes that track.
    noteOff(h, 64);
    noteOn(h, 91);
    CHECK(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 91)) >= 0);
    for (int n = 0; n < 128; ++n) noteOff(h, uint8_t(n));
    CHECK_EQ(busyTracks(a), 1);   // the second 60 needs its own note-off
    noteOff(h, 60);
    CHECK_EQ(busyTracks(a), 0);
    // Muted tracks are skipped.
    a.setTrackMute(3, true);
    noteOn(h, 60);
    CHECK_EQ(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60)), 4);
    noteOff(h, 60);
}

// Stock: 12 (one per track); 2.x: one per track of all 16 (2x-01 §6).
TEST(midi_instrument_polyphony_16_and_velocity) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.cfg.midiOut = 3;   // USB + jack
    a.setCurrentInstrument(kFirstMidiInstrument);
    for (int i = 0; i < kTrackCount + 1; ++i) noteOn(h, uint8_t(40 + i));
    int n = 0;
    for (int i = 0; i < kTrackCount + 1; ++i)
        if (a.auditionTrack(uint16_t(App::kAuditionMidiIn + 40 + i)) >= 0) ++n;
    CHECK_EQ(n, kTrackCount);
    // Velocity is fixed (127 * default velocity / 127) outside a velocity mode.
    CHECK_EQ(lastNoteOnVelocity(h), 127);
    for (int i = 0; i < kTrackCount + 1; ++i) noteOff(h, uint8_t(40 + i));
    MidiInstrument mi = a.project.midiInstrument(0);
    mi.velocity = 100;
    a.project.setMidiInstrument(0, mi);
    h.midi.sent.clear();
    noteOn(h, 60, 30);
    CHECK_EQ(lastNoteOnVelocity(h), 100);
    noteOff(h, 60);
    // Velocity recording mode: vel * IV / 127.
    a.cfg.recordingOptions = 2;
    h.midi.sent.clear();
    noteOn(h, 60, 127);
    CHECK_EQ(lastNoteOnVelocity(h), 100);
    noteOff(h, 60);
    h.midi.sent.clear();
    noteOn(h, 60, 64);
    CHECK_EQ(lastNoteOnVelocity(h), 64 * 100 / 127);
    noteOff(h, 60);
}

TEST(middle_c_transposes_input) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.setCurrentInstrument(kFirstMidiInstrument);
    a.cfg.middleC = 1;   // C-4: n - 12
    noteOn(h, 60);
    CHECK(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 48)) >= 0);
    noteOff(h, 60);
    CHECK_EQ(busyTracks(a), 0);
    a.cfg.middleC = 3;   // C-6: n + 12, clamped
    noteOn(h, 125);
    CHECK(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 127)) >= 0);
    noteOff(h, 125);
    CHECK_EQ(busyTracks(a), 0);
}

TEST(ignored_messages) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.cfg.ccIn = 1;
    a.instrPresent[0] = true;
    a.setCurrentInstrument(0);
    const Instrument before = a.instruments[0];
    send(h, {0xE0, 0, 127});     // pitch bend
    send(h, {0xD0, 100});        // channel aftertouch
    send(h, {0xA0, 60, 100});    // poly aftertouch
    send(h, {0xC0, 5});          // program change
    send(h, {0xB0, 64, 127});    // sustain
    send(h, {0xB0, 123, 0});     // all notes off
    CHECK_EQ(busyTracks(a), 0);
    CHECK_EQ(a.instruments[0].volume, before.volume);
    CHECK_EQ(a.instruments[0].tune, before.tune);
}

TEST(edit_mode_chord_written_across_selection) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    CHECK(a.editMode);
    a.setCurrentInstrument(kFirstMidiInstrument);
    PatternScreen& p = pe(h);
    p.step = 4;
    p.track = 2;
    p.anchorStep = 4;
    p.anchorTrack = 0;
    p.selState = 2;
    // Three held notes: tracks 0, 1, 2 at step 4; the fourth sounds on
    // track 3 (outside the selection) and is not written.
    noteOn(h, 60);
    noteOn(h, 64);
    noteOn(h, 67);
    noteOn(h, 71);
    CHECK_EQ(int(a.pattern().tracks[0].steps[4].note), 60);
    CHECK_EQ(int(a.pattern().tracks[1].steps[4].note), 64);
    CHECK_EQ(int(a.pattern().tracks[2].steps[4].note), 67);
    CHECK(a.pattern().tracks[3].steps[4].note == kNoteEmpty);
    CHECK_EQ(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 71)), 3);
    CHECK_EQ(p.step, 4);   // no cursor advance
    for (uint8_t n : {60, 64, 67, 71}) noteOff(h, n);
    // Single-cell cursor: only the first note of a chord is written.
    p.selState = 1;
    p.step = 8;
    p.track = 5;
    noteOn(h, 50);
    noteOn(h, 52);
    CHECK_EQ(int(a.pattern().tracks[5].steps[8].note), 50);
    CHECK(a.pattern().tracks[6].steps[8].note == kNoteEmpty);
    noteOff(h, 50);
    noteOff(h, 52);
    // Velocity mode writes FX-B = Volume round(vol * 100 / 127).
    a.cfg.recordingOptions = 3;
    p.step = 9;
    noteOn(h, 55, 64);
    const Step& st = a.pattern().tracks[5].steps[9];
    CHECK_EQ(int(st.note), 55);
    CHECK_EQ(st.fx[0].type, 18);
    CHECK_EQ(st.fx[0].value, (64 * 127 / 127 * 100 + 63) / 127);
    noteOff(h, 55);
}

TEST(no_entry_outside_edit_or_while_playing) {
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.setCurrentInstrument(kFirstMidiInstrument);
    noteOn(h, 60);   // edit mode off: plays only
    CHECK(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60)) >= 0);
    noteOff(h, 60);
    CHECK(a.pattern().tracks[0].steps[0].note == kNoteEmpty);
}

TEST(cc_map_conversions) {
    Instrument in;
    // CC1 mod wheel = cutoff, only while the filter is on.
    CHECK(!midicc::applyInstrumentCc(in, 1, 0));
    CHECK(midicc::applyInstrumentCc(in, 9, 40));    // 4*40/127 = 1 -> low-pass
    CHECK_EQ(in.filterEnabled, 1);
    CHECK_EQ(in.filterType, 0);
    CHECK(midicc::applyInstrumentCc(in, 9, 64));    // high-pass
    CHECK_EQ(in.filterType, 1);
    CHECK(midicc::applyInstrumentCc(in, 9, 96));    // band-pass
    CHECK_EQ(in.filterType, 2);
    CHECK(!midicc::applyInstrumentCc(in, 9, 127));  // 4 -> 3, unchanged
    CHECK(midicc::applyInstrumentCc(in, 1, 0));
    CHECK(in.cutoff == 0.0f);
    CHECK(midicc::applyInstrumentCc(in, 9, 31));    // disabled
    CHECK_EQ(in.filterEnabled, 0);
    // Tune: exact target (OpenTracker fix: no +-10 -> octave step quirk).
    in.tune = 0;
    CHECK(midicc::applyInstrumentCc(in, 5, 90));    // 90*48/127-24 = 10
    CHECK_EQ(in.tune, 10);
    CHECK(midicc::applyInstrumentCc(in, 5, 64));    // 64*48/127-24 = 0
    CHECK_EQ(in.tune, 0);
    // LFO speed: exact target in one message (no +-5 limit).
    CHECK(midicc::applyInstrumentCc(in, 114, 127));  // finetune slot -> LFO
    CHECK(midicc::applyInstrumentCc(in, 116, 127));
    CHECK_EQ(in.lfo[5].speed, 28);
    // Time-position scale.
    CHECK_EQ(midicc::ccTimePosition(0), 0);
    CHECK_EQ(midicc::ccTimePosition(32), 50);
    CHECK_EQ(midicc::ccTimePosition(64), 100);
    CHECK_EQ(midicc::ccTimePosition(127), 190);
    CHECK(midicc::applyInstrumentCc(in, 24, 32));   // volume env release: 500 ms
    CHECK_EQ(in.env[0].releaseMs, 500);
    // Release is written also while the slot is an LFO.
    CHECK(midicc::applyInstrumentCc(in, 20, 127));  // LFO
    CHECK(midicc::applyInstrumentCc(in, 24, 127));
    CHECK_EQ(in.env[0].releaseMs, 10000);
    CHECK(!midicc::applyInstrumentCc(in, 23, 0));   // sustain: envelope only
}

TEST(cc_16_19_sample_playback) {
    Instrument in;
    // Not wavetable / granular: ignored.
    CHECK(!midicc::applyPlaybackCc(in, 44100, 16, 127));
    in.playMode = uint8_t(PlayMode::Granular);
    CHECK(midicc::applyPlaybackCc(in, 44100, 16, 127));
    CHECK_EQ(in.granularPosition, 65535 / 65 * 65);
    CHECK(midicc::applyPlaybackCc(in, 20000, 17, 127));
    CHECK(in.grainLength <= 20000 && in.grainLength > 19980);
    CHECK_EQ((in.grainLength - 441) % 11, 0);
    CHECK(midicc::applyPlaybackCc(in, 20000, 18, 127));
    CHECK_EQ(in.grainShape, 2);
    CHECK(midicc::applyPlaybackCc(in, 20000, 19, 64));
    CHECK_EQ(in.granularLoop, 1);
    in.playMode = uint8_t(PlayMode::Wavetable);
    in.wavetableWindowSize = 2048;
    in.wavetableWindowCount = 20000 / 2048;
    in.wavetableCurrentWindow = 0;
    CHECK(midicc::applyPlaybackCc(in, 20000, 16, 127));
    CHECK_EQ(in.wavetableCurrentWindow, in.wavetableWindowCount - 1);
    CHECK(midicc::applyPlaybackCc(in, 20000, 17, 0));   // window 32
    CHECK_EQ(in.wavetableWindowSize, 32);
    CHECK_EQ(in.wavetableWindowCount, 20000u / 32u);
    CHECK_EQ(in.wavetableCurrentWindow, 8u * 64u);      // rescaled by 2^(11-5)
    CHECK(!midicc::applyPlaybackCc(in, 20000, 18, 127));
}

TEST(app_cc_16_17_and_persistence) {
    Harness h;
    App& a = *h.app;
    a.cfg.ccIn = 1;
    loadSample(h, 0);
    a.setCurrentInstrument(0);
    a.instruments[0].playMode = uint8_t(PlayMode::Granular);
    a.autosave();
    send(h, {0xB0, 16, 64});
    CHECK(a.instruments[0].granularPosition > 30000);
    CHECK(a.unsavedChanges());
    // Edits persist with the instrument (autosave writes the .pti).
    a.autosave();
    const uint16_t pos = a.instruments[0].granularPosition;
    CHECK(a.loadWorkspace());
    CHECK_EQ(a.instruments[0].granularPosition, pos);
    // MIDI instrument current: CC 16-19 have no effect.
    a.setCurrentInstrument(kFirstMidiInstrument);
    send(h, {0xB0, 16, 0});
    CHECK_EQ(a.instruments[0].granularPosition, pos);
}

TEST(cc_value_names) {
    CHECK_EQ(midicc::kCcValueCount, 131);
    char b[16];
    midicc::ccValueName(0, b, sizeof(b));
    CHECK(std::strcmp(b, "0 Bank") == 0);
    midicc::ccValueName(1, b, sizeof(b));
    CHECK(std::strcmp(b, "1 ModWh") == 0);
    midicc::ccValueName(7, b, sizeof(b));
    CHECK(std::strcmp(b, "7 ChVol") == 0);
    midicc::ccValueName(64, b, sizeof(b));
    CHECK(std::strcmp(b, "64 Sust") == 0);
    midicc::ccValueName(120, b, sizeof(b));
    CHECK(std::strcmp(b, "120 SOff") == 0);
    midicc::ccValueName(123, b, sizeof(b));
    CHECK(std::strcmp(b, "123 NOff") == 0);
    midicc::ccValueName(32, b, sizeof(b));
    CHECK(std::strcmp(b, "32 Bank") == 0);
    midicc::ccValueName(66, b, sizeof(b));
    CHECK(std::strcmp(b, "66 Sste") == 0);
    midicc::ccValueName(84, b, sizeof(b));
    CHECK(std::strcmp(b, "84 Porta") == 0);
    midicc::ccValueName(96, b, sizeof(b));
    CHECK(std::strcmp(b, "96 Dat+") == 0);
    midicc::ccValueName(35, b, sizeof(b));
    CHECK(std::strcmp(b, "35") == 0);
    midicc::ccValueName(3, b, sizeof(b));
    CHECK(std::strcmp(b, "3") == 0);
    midicc::ccValueName(128, b, sizeof(b));
    CHECK(std::strcmp(b, "Prg Chg") == 0);
    midicc::ccValueName(129, b, sizeof(b));
    CHECK(std::strcmp(b, "Chan AT") == 0);
    midicc::ccValueName(130, b, sizeof(b));
    CHECK(std::strcmp(b, "Poly AT") == 0);
    midicc::ccValueName(200, b, sizeof(b));
    CHECK(std::strcmp(b, "Poly AT") == 0);
}

TEST(midi_start_on_song_screen_needs_pattern_in_song) {
    Harness h;
    App& a = *h.app;
    a.cfg.transportIn = 1;   // USB
    a.applySequencerSettings();
    a.project.playlist[0] = 2;
    a.project.playlist[1] = 0;
    h.tap(btn::Song);
    CHECK_EQ(a.ui.active(), int(kModSong));
    CHECK_EQ(int(a.patternNumber()), 1);
    send(h, {0xFA});
    CHECK(!a.seq.isPlaying());
    h.run(5);
    CHECK(h.disp.lastHasText("Load pattern from song first"));
    // Pattern in the song: Start plays the song.
    a.project.playlist[1] = 1;
    send(h, {0xFA});
    CHECK(a.seq.isPlaying());
    send(h, {0xFC});
    CHECK(!a.seq.isPlaying());
}

TEST(live_midi_instrument_follows_midi_out_mask) {
    // Live MIDI-instrument notes use Config "MIDI out" like the sequencer.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    a.setCurrentInstrument(kFirstMidiInstrument);
    a.cfg.midiOut = 0;   // Off: the note still takes a track, nothing is sent
    noteOn(h, 60);
    CHECK(a.auditionTrack(uint16_t(App::kAuditionMidiIn + 60)) >= 0);
    noteOff(h, 60);
    CHECK_EQ(h.midi.sent.size(), size_t(0));
    a.cfg.midiOut = 2;   // MIDI Out jack only
    noteOn(h, 61);
    noteOff(h, 61);
    CHECK_EQ(h.midi.sentDin, size_t(6));
    CHECK_EQ(h.midi.sentUsb, size_t(0));
    a.cfg.midiOut = 1;   // USB only
    h.midi.sentDin = h.midi.sentUsb = 0;
    noteOn(h, 62);
    noteOff(h, 62);
    CHECK_EQ(h.midi.sentUsb, size_t(6));
    CHECK_EQ(h.midi.sentDin, size_t(0));
}

TEST(step_entry_while_armed_not_while_playing) {
    // spec 17 §1.4 step 8: gated on the play state only; an armed (waiting
    // for external Start) sequencer still takes the entry.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    h.tap(btn::Rec);
    CHECK(a.editMode);
    a.setCurrentInstrument(kFirstMidiInstrument);
    a.seq.settings().clockIn = 1;
    a.seq.userPlay(0);
    CHECK(a.seq.isArmed());
    CHECK(!a.seq.isPlaying());
    PatternScreen& p = pe(h);
    p.step = 2;
    p.track = 0;
    noteOn(h, 60);
    CHECK_EQ(int(a.pattern().tracks[0].steps[2].note), 60);
    noteOff(h, 60);
    a.seq.stop();
    a.seq.settings().clockIn = 0;
    a.seq.play(0);
    CHECK(a.seq.isPlaying());
    p.step = 3;
    noteOn(h, 62);
    CHECK(a.pattern().tracks[0].steps[3].note == kNoteEmpty);
    noteOff(h, 62);
    a.seq.stop();
}

TEST(sample_screens_own_midi_notes) {
    // spec 17 §1.3 owners 2..4: the screen's own preview, not the track voices.
    Harness h;
    App& a = *h.app;
    a.cfg.notesIn = 1;
    loadSample(h, 0);
    a.setCurrentInstrument(0);
    // Sample Playback: the b0 preview voice at the note; the last note sounds.
    h.tap(btn::SamplePlayback);
    CHECK(a.ui.active() == kModPlayback);
    noteOn(h, 64);
    CHECK(a.auditionTrack(App::kAuditionSampleEditor) >= 0);
    CHECK_EQ(busyTracks(a), 0);
    noteOn(h, 67);
    noteOff(h, 64);   // not the sounding note
    CHECK(a.auditionTrack(App::kAuditionSampleEditor) >= 0);
    noteOff(h, 67);
    CHECK_EQ(a.auditionTrack(App::kAuditionSampleEditor), -1);
    // Sample Editor: the F7 preview of the selection.
    h.tap(btn::SampleEditor);
    CHECK(a.ui.active() == kModEditor);
    EditorScreen& e = *static_cast<EditorScreen*>(a.ui.screen(kModEditor));
    noteOn(h, 60);
    CHECK(e.previewPlaying);
    CHECK(a.engine.previewBufferActive());
    CHECK_EQ(busyTracks(a), 0);
    noteOff(h, 61);
    CHECK(e.previewPlaying);
    noteOff(h, 60);
    CHECK(!e.previewPlaying);
    // Other screens: the track voices.
    h.tap(btn::Pattern);
    noteOn(h, 60);
    CHECK_EQ(busyTracks(a), 1);
    noteOff(h, 60);
}

// Pattern Instr column, not editing: a pad picks instrument k and auditions
// it at note 60, not at the pad's note (user report 2.0.6: each instrument
// one semitone higher).
TEST(instr_column_pad_auditions_at_note_60) {
    Harness h;
    App& a = *h.app;
    a.cfg.midiOut = 3;
    PatternScreen& p = pe(h);
    p.track = 8;   // MIDI track 9: the pads pick MIDI instruments (note-ons visible)
    a.editorTrack = 8;
    p.col = 1;
    for (uint8_t pad = 0; pad < 4; ++pad) {
        h.midi.sent.clear();
        h.push(InputEvent::kPadDown, pad);
        h.run(5);
        int note = -1;
        for (size_t i = 0; i + 2 < h.midi.sent.size(); ++i)
            if ((h.midi.sent[i] & 0xF0) == 0x90 && h.midi.sent[i + 2]) note = h.midi.sent[i + 1];
        CHECK_EQ(note, 60);
        h.push(InputEvent::kPadUp, pad);
        h.run(5);
    }
}
