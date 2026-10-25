// Shared by pattern_mtp_test and mini_fixture_test: the C++ twin of the
// pattern content written by tests/fixtures/gen_mini_fixtures.py, and a
// reader for the fixtures' hex dumps.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "model/pattern.h"

namespace minigold {

inline std::string fixturesDir() {
    const char* d = std::getenv("OT_FIXTURES");
    return d ? d : "fixtures";
}

inline std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    int c;
    while ((c = std::fgetc(f)) != EOF) v.push_back(uint8_t(c));
    std::fclose(f);
    return v;
}

// "oooo: xx xx ..." lines; '#' lines are comments.
inline std::vector<uint8_t> loadHex(const std::string& name) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen((fixturesDir() + "/" + name).c_str(), "r");
    if (!f) return v;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        const char* p = std::strchr(line, ':');
        if (!p) continue;
        ++p;
        char* end = nullptr;
        for (;;) {
            unsigned long b = std::strtoul(p, &end, 16);
            if (end == p) break;
            v.push_back(uint8_t(b));
            p = end;
        }
    }
    std::fclose(f);
    return v;
}

// gen_mini_fixtures.py goldenStep() / goldenLastStep().
inline void goldenPattern(ot::Pattern& p, int seed) {
    for (int t = 0; t < ot::kTrackCount; ++t) {
        ot::Track& tr = p.tracks[t];
        tr.lastStep = uint8_t((t * 7 + 15 + seed) % 128);
        for (int s = 0; s < ot::kStepCount; ++s) {
            ot::Step& st = tr.steps[s];
            st = ot::Step{};
            if ((s + t + seed) % 5) continue;
            const int k = t * 131 + s * 7 + seed * 17;
            st.note = int8_t((24 + k) % 128);
            st.instrument = uint8_t((t * 3 + s + seed) % 67);
            st.fx[1] = {uint8_t(k % 43), uint8_t((k * 5) & 0xFF)};
            st.fx[0] = {uint8_t((k / 3) % 43), uint8_t((k * 11) & 0xFF)};
        }
    }
}

inline bool sameTrack(const ot::Track& a, const ot::Track& b) {
    if (a.lastStep != b.lastStep) return false;
    for (int s = 0; s < ot::kStepCount; ++s)
        if (!(a.steps[s] == b.steps[s])) return false;
    return true;
}

}  // namespace minigold
