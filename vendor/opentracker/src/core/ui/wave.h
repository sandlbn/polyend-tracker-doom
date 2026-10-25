// Waveform view and zoom model shared by the Sample Editor and the Sample
// Recorder (spec 06 §6.3 "Markers and zoom", spec 05 appendix B §1.9).
#pragma once

#include <cstdint>

#include "ui/gfx.h"

namespace ot {

// Min/max waveform of d[0..len) between fractions [v0, v1), in a w x h box.
void drawWaveData(Gfx& g, const Theme& th, const int16_t* d, uint32_t len, float v0, float v1, int x0, int y0, int w,
                  int h, uint32_t c);

// Zoom by samples-per-pixel over a 600 px view (spec 06 §6.3):
// sppMax = len/600, sppMin = len/65535 + 1, step = max(1, (max-min)/100);
// len < 600 -> no zoom. zoom = len / (spp * 600).
struct WaveZoom {
    uint32_t len = 0;
    uint32_t spp = 0;          // 0 = full view
    uint16_t centre = 0;       // marker the view centres on
    void reset(uint32_t frames) { len = frames; spp = 0; }
    float zoom() const;
    void step(int d);          // + zooms in
    // Visible marker window [v0, v1) as fractions, centred on `centre`.
    void window(float* v0, float* v1) const;
    // One pixel of the current view in marker units (>= 1).
    int pixelUnits() const;
};

}  // namespace ot
