// polyend-doom -- Doom frames on the FT81x (pt_present).
//
// A frame of 320x200 palette indices and its RGB565 palette go to RAM_G and
// are drawn as a PALETTED565 bitmap, magnified 2x to 640x400 at (80, 40) of
// the 800x480 panel.
//
// Pipeline: pt_present(N) copies frame N into a DMA buffer (SRAM_U, ~0.3 ms),
// writes its palette, and starts the SPI DMA of the frame into a RAM_G slot
// (writeRamGAsync, ~17 ms at 30 MHz), then returns: Doom computes frame N+1
// while the frame goes out. pt_present(N+1) waits for that upload (normally
// long done) and displays frame N, then starts N+1. One frame of latency.
// Three slots rotate, so the slot being written is neither the one on screen
// nor the one a pending swap is about to show.
//
// RAM_G use (Ft81xDisplay's map, src/hal/ft81x.h): the cover-image band
// above 0x0A3930, unused here. The fonts and fragment slots below stay intact.

#include <stdio.h>
#include <string.h>

#include "hal/diag.h"
#include "hal/dma_mem.h"
#include "hal/ft81x.h"
#include "hal_api.h"

#include "device.h"
#include "pt.h"

namespace {

constexpr uint32_t kFrameBytes = PT_SCREEN_W * PT_SCREEN_H;
constexpr int kSlots = 3;
constexpr uint32_t kSlot[kSlots] = {0x0A4000, 0x0B4000, 0x0C4000};
constexpr uint32_t kPalette[kSlots] = {0x0D4000, 0x0D4200, 0x0D4400};
constexpr int16_t kX = 80, kY = 40;
constexpr uint8_t kScale = 2;
static_assert(kSlot[0] >= ot::IDisplay::kRamGCover, "frame slots above the fonts and fragments");
static_assert(kSlot[0] + kFrameBytes <= kSlot[1] && kSlot[1] + kFrameBytes <= kSlot[2] &&
                  kSlot[2] + kFrameBytes <= kPalette[0],
              "frame slots");
static_assert(kPalette[kSlots - 1] + 512 <= ot::IDisplay::kRamGSize, "palettes");

ot::IDisplay* gDisplay = nullptr;
int gNext = 0;          // slot the next frame is uploaded to
int gPending = -1;      // slot uploaded (or uploading), not shown yet
uint32_t gLastExitUs = 0;
PtFrameStats gStats;
bool gOverlay = false;
char gOverlayText[128];

// Doom renders here (internal SRAM). The DMA reads the copy in SRAM_U.
alignas(4) uint8_t gFramebuffer[kFrameBytes];
OT_DMA_MEM uint8_t gDmaFrame[kFrameBytes];

uint32_t nowUs() { return pt_hal().clock->micros(); }

// ---- around and over the frame (pt_ui) --------------------------------------------
// The panel: top strip y 0..39 (hints, stats), the frame y 40..439, bottom
// strip y 440..479 with the labels of F1..F8, which sit under the screen
// (100 px each, as in OpenTracker's UI). FT81x ROM fonts 26 (16 px high) and 28.

constexpr uint16_t kCenter = ot::IDisplay::kOptCenter, kCenterY = ot::IDisplay::kOptCenterY,
                   kRight = ot::IDisplay::kOptRightX;

void rect(int x0, int y0, int x1, int y1) {
    ot::IDisplay& d = *gDisplay;
    d.begin(ot::IDisplay::kRects);
    d.vertex2f(x0 * 16, y0 * 16);
    d.vertex2f(x1 * 16, y1 * 16);
    d.end();
}

void drawStrips(const pt_ui_t& ui) {
    ot::IDisplay& d = *gDisplay;
    d.colorRGB(150, 150, 150);
    d.text(10, 20, 26, kCenterY,
           ui.typing ? "Type: pads, or encoder + push.   Delete: erase   F2: done   F1: cancel"
                     : "Help: controls");
    if (gOverlay && gOverlayText[0]) {
        d.colorRGB(255, 220, 0);
        d.text(790, 20, 26, uint16_t(kRight | kCenterY), gOverlayText);
    }
    for (int f = 0; f < 8; ++f) {
        const char* label = pt_fkey_label(f);
        if (!label[0]) continue;
        d.colorRGB(45, 45, 45);
        rect(f * 100 + 4, 446, f * 100 + 96, 476);
        d.colorRGB(220, 220, 220);
        d.text(int16_t(f * 100 + 50), 461, 26, kCenter, label);
    }
}

void drawKeyboard(const pt_ui_t& ui) {
    // Pad layout, 12 x 4, over the lower part of the frame (the save slots
    // are above it).
    ot::IDisplay& d = *gDisplay;
    constexpr int kW = 50, kH = 30, kX0 = 100, kY0 = 304;
    d.colorRGB(0, 0, 0);
    d.colorA(220);
    rect(kX0 - 10, kY0 - 10, kX0 + 12 * kW + 10, kY0 + 4 * kH + 10);
    d.colorA(255);
    for (int cell = 0; cell < 48; ++cell) {
        const int x = kX0 + (cell % 12) * kW, y = kY0 + (cell / 12) * kH;
        if (cell == ui.keySel) d.colorRGB(170, 30, 20);
        else d.colorRGB(50, 50, 50);
        rect(x + 2, y + 2, x + kW - 2, y + kH - 2);
        d.colorRGB(255, 255, 255);
        d.text(int16_t(x + kW / 2), int16_t(y + kH / 2), 26, kCenter, pt_keyboard_label(cell));
    }
}

void drawHelp() {
    ot::IDisplay& d = *gDisplay;
    d.colorRGB(0, 0, 0);
    d.colorA(230);
    rect(110, 40, 690, 440);
    d.colorA(255);
    d.colorRGB(220, 40, 30);
    d.text(400, 62, 28, kCenter, "Controls");
    int y = 94;
    for (int i = 0; pt_help_rows[i][0]; ++i, y += 19) {
        d.colorRGB(255, 220, 120);
        d.text(130, int16_t(y), 26, kCenterY, pt_help_rows[i][0]);
        d.colorRGB(220, 220, 220);
        d.text(400, int16_t(y), 26, kCenterY, pt_help_rows[i][1]);
    }
    d.colorRGB(140, 140, 140);
    d.text(400, 424, 26, kCenter, "Help: close");
}

void show(int slot) {
    ot::IDisplay& d = *gDisplay;
    pt_ui_t ui;
    pt_ui(&ui);
    d.dlStart();
    d.clearColorRGB(0, 0, 0);
    d.clear(true, true, true);
    d.colorRGB(255, 255, 255);
    d.drawPaletted565(kX, kY, kSlot[slot], kPalette[slot], PT_SCREEN_W, PT_SCREEN_H, kScale);
    drawStrips(ui);
    if (ui.typing) drawKeyboard(ui);
    if (ui.help) drawHelp();
    d.swap();
}

}  // namespace

void pt_display_init(ot::IDisplay* d) { gDisplay = d; }

void pt_display_take_stats(PtFrameStats* out) {
    *out = gStats;
    out->asyncUpload = ot::hal::Ft81xDisplay::dmaActive();
    gStats = PtFrameStats();
}

void pt_display_set_overlay(bool on, const char* text) {
    gOverlay = on;
    snprintf(gOverlayText, sizeof gOverlayText, "%s", text ? text : "");
}

extern "C" uint8_t* pt_framebuffer(void) { return gFramebuffer; }

extern "C" void pt_present(const uint8_t* frame, const uint16_t* pal565, int paletteChanged) {
    (void)paletteChanged;   // each slot carries its own palette, written every frame (512 B)
    ot::IDisplay& d = *gDisplay;
    const uint32_t t0 = nowUs();
    if (gLastExitUs) gStats.gameUs += t0 - gLastExitUs;

    // The previous frame's upload, then show it once the swap before it took
    // effect (dlStart must not wait inside the driver).
    d.waitRamG();
    const uint32_t t1 = nowUs();
    gStats.waitUploadUs += t1 - t0;
    if (gPending >= 0) {
        while (!d.frameReady()) pt_pump();
        const uint32_t t2 = nowUs();
        gStats.waitDisplayUs += t2 - t1;
        show(gPending);
        ++gStats.frames;
    }

    // This frame: palette (blocking, 512 B), then the frame by DMA.
    memcpy(gDmaFrame, frame, kFrameBytes);
    d.writeRamG(kPalette[gNext], pal565, 512);
    d.writeRamGAsync(kSlot[gNext], gDmaFrame, kFrameBytes);
    gPending = gNext;
    gNext = (gNext + 1) % kSlots;

    gLastExitUs = nowUs();
    gStats.presentUs += gLastExitUs - t0;
}
