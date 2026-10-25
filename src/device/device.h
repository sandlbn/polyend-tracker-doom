// polyend-doom -- shared declarations of the Tracker platform (src/device).
#pragma once

#include <stdint.h>

#include "hal/hal.h"

constexpr const char* kDoomDir = "/DOOM";   // WADs, doom.cfg, savegames, return.txt

// syscalls.cpp
void pt_syscalls_init(ot::IStorage* fs);
void pt_heap_enable();
uint32_t pt_heap_used();
uint32_t pt_heap_size();

// display.cpp
// Frame timing since the last pt_display_take_stats(), summed over `frames`.
struct PtFrameStats {
    uint32_t frames = 0;          // frames shown
    uint64_t gameUs = 0;          // Doom between presents: game logic, rendering, sound, input
    uint64_t waitUploadUs = 0;    // waiting for the previous frame's DMA upload
    uint64_t waitDisplayUs = 0;   // waiting for the previous swap (display refresh)
    uint64_t presentUs = 0;       // all of pt_present (the waits, the copy, the palette)
    bool asyncUpload = false;     // the SPI DMA is on (else uploads block)
};
void pt_display_init(ot::IDisplay* d);
void pt_display_take_stats(PtFrameStats* out);
// Stats line in the strip under the picture (toggled with Aux).
void pt_display_set_overlay(bool on, const char* text);

// app_main.cpp
ot::hal::HalSet& pt_hal();
void pt_pump();   // HAL service + input -> Doom; call often from the main loop
extern "C" [[noreturn]] void pt_device_exit(int status);
