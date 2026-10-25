// User report: a note with Gate Length 0 on step 1 was sometimes not heard.
// The sequencer runs all of a block's micro-steps before the block renders,
// so note-on and its note-off could land in one block and the voice was
// released before it produced a frame. A same-block note-off now waits one
// block (AudioEngine::voiceNoteOff), so every note sounds.
#include <cmath>
#include <cstdlib>
#include <vector>

#include "test.h"
#include "ui_fakes.h"

#include "formats/wav.h"
#include "ui/screens.h"

using namespace ot;

namespace {
// Plays a 16-step pattern with C6 + Gate Length `gate` on step 1 of track 1
// and returns how many of `loops` loops had no sound in their first steps.
int silentLoops(int gate, int loops) {
    Harness h;
    App& a = *h.app;
    std::vector<int16_t> s(6000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(12000.0 * std::sin(double(i) * 0.05));
    std::vector<uint8_t> out(s.size() * 2 + 64);
    out.resize(wavEncode(s.data(), uint32_t(s.size()), 1, out.data(), out.size()));
    h.fs.files["t.wav"] = out;
    CHECK(a.importSample("t.wav", 0));
    Pattern& p = a.pattern();
    for (auto& t : p.tracks) t.lastStep = 15;
    Step& st = p.tracks[0].steps[0];
    st.note = 72;
    st.instrument = 0;
    st.fx[0].type = 20;   // Gate Length
    st.fx[0].value = uint8_t(gate);
    a.patternEdited();
    h.tap(btn::Pattern);
    h.tap(btn::Play);
    int prev = -1, loop = 0, silent = 0, peak = 0, measuring = 0;
    int16_t buf[256];
    for (int b = 0; b < 60000 && loop <= loops; ++b) {
        h.audio.fn(buf, 128, h.audio.ctx);
        if (b % 3 == 0) h.run(1);
        const int stp = a.seq.step(0);
        if (stp == 0 && prev != 0) {
            if (loop > 0 && peak < 200) ++silent;
            ++loop;
            peak = 0;
            measuring = 30;
        }
        prev = stp;
        if (measuring > 0) {
            --measuring;
            for (int i = 0; i < 256; ++i) peak = std::max(peak, std::abs(int(buf[i])));
        }
    }
    return silent;
}
}  // namespace

TEST(gate_length_zero_note_always_sounds) {
    CHECK_EQ(silentLoops(0, 24), 0);
    CHECK_EQ(silentLoops(1, 24), 0);
}
