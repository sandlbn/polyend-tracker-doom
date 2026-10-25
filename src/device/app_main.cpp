// polyend-doom -- the Tracker application: OpenTracker's main.cpp brings the
// board up and calls otAppMain() (weak there, provided here) once the SDRAM
// passed its test. This file runs the launcher (IWAD choice, firmware
// update), then Doom, and owns the main-loop services: HAL service, input,
// pad LEDs, MIDI out and the exit screen.
//
// Getting off this image: Launcher > Firmware update (or the screen after
// Doom quits) installs any image in /Firmware through the update bootloader,
// e.g. OpenTracker or the stock PolyendTracker_1.9.2.ptf. OpenTracker's
// /Firmware/ot_restore.txt boot trigger (main.cpp) works here as well.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Arduino.h>

#include "hal/audio_sgtl5000.h"
#include "hal/diag.h"
#include "hal/fw_update.h"
#include "hal/hal.h"
#include "hal_api.h"

#include "device.h"
#include "pt.h"

extern "C" uint8_t _ssdram_bss[], _esdram_bss[];     // HAL scratch (.sdram_bss)
extern "C" uint8_t _ssdram_doom[], _esdram_doom[];   // Doom's large .bss (doom.ld)
extern "C" uint8_t _ssdram_data[], _esdram_data[], _sisdram_data[];   // Doom's .data, flash image (doom.ld)

namespace {

ot::hal::HalSet* gHal = nullptr;
bool gDoomRunning = false;

// OpenTracker button ids (core/ui/buttons.h).
constexpr uint8_t kBtnF1 = 0x00, kBtnF2 = 0x01, kBtnPlay = 0x17, kBtnUp = 0x1a, kBtnDown = 0x1f,
                  kBtnEncPush = 0x21, kBtnAux = 0x22;

constexpr float kHeadphoneLevel = 0.715f;

constexpr int kMaxNames = 16;
constexpr int kNameLen = 48;
using NameList = char[kMaxNames][kNameLen];

NameList gWads;        // file names in /DOOM with an IWAD header
int gWadCount = 0;
NameList gImages;      // update images in /Firmware, sorted
int gImageCount = 0;

void audioFill(int16_t* out, uint32_t frames, void* ctx) {
    (void)ctx;
    pt_mix(out, frames);
}

bool endsWithNoCase(const char* s, const char* suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i)
        if (tolower(static_cast<unsigned char>(s[n - m + i])) != suffix[i]) return false;
    return true;
}

bool isIwad(const char* name) {
    char path[96];
    snprintf(path, sizeof path, "%s/%.47s", kDoomDir, name);
    ot::IStorage& fs = *gHal->storage;
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return false;
    char magic[4] = {};
    const bool ok = fs.read(h, magic, 4) == 4 && memcmp(magic, "IWAD", 4) == 0;
    fs.close(h);
    return ok;
}

struct ListCtx {
    NameList* names;
    int count;
    bool (*accept)(const char* name);
};

// Names in `dir` that `accept` takes, sorted.
int listNames(const char* dir, NameList& names, bool (*accept)(const char*)) {
    ListCtx ctx = {&names, 0, accept};
    gHal->storage->listDir(
        dir,
        [](const ot::IStorage::DirEntry& e, void* p) {
            ListCtx& c = *static_cast<ListCtx*>(p);
            if (!e.isDir && strlen(e.name) < kNameLen && c.accept(e.name))
                snprintf((*c.names)[c.count++], kNameLen, "%.47s", e.name);
            return c.count < kMaxNames;
        },
        &ctx);
    qsort(names, size_t(ctx.count), kNameLen,
          [](const void* a, const void* b) { return strcmp(static_cast<const char*>(a), static_cast<const char*>(b)); });
    return ctx.count;
}

void scanWads() {
    static NameList candidates;
    const int n = listNames(kDoomDir, candidates, [](const char* name) { return endsWithNoCase(name, ".wad"); });
    gWadCount = 0;
    for (int i = 0; i < n; ++i)
        if (isIwad(candidates[i])) snprintf(gWads[gWadCount++], kNameLen, "%.47s", candidates[i]);
}

// The names the stock updater offers (fw_update.h), except this image.
void scanImages() {
    gImageCount = listNames("/Firmware", gImages, [](const char* name) {
        return ot::hal::isUpdateImageName(name) && strcmp(name, PT_IMAGE_NAME) != 0;
    });
}

// ---- screens ------------------------------------------------------------------------

void beginScreen() {
    ot::IDisplay& d = *gHal->display;
    while (!d.frameReady()) gHal->service();
    d.dlStart();
    d.clearColorRGB(16, 0, 0);
    d.clear(true, true, true);
}

void message(const char* text, uint8_t r = 200, uint8_t g = 200, uint8_t b = 200) {
    beginScreen();
    ot::IDisplay& d = *gHal->display;
    d.colorRGB(r, g, b);
    d.text(400, 240, 29, ot::IDisplay::kOptCenter, text);
    d.swap();
}

struct Menu {
    const char* title;
    const char* note[3];          // lines under the title ("" / null = none)
    const char* items[kMaxNames + 2];
    int count;
    bool canGoBack;               // F1 returns -1
};

void drawMenu(const Menu& m, int sel) {
    beginScreen();
    ot::IDisplay& d = *gHal->display;
    d.colorRGB(220, 40, 30);
    d.text(400, 46, 31, ot::IDisplay::kOptCenter, m.title);
    d.colorRGB(150, 150, 150);
    d.text(400, 88, 26, ot::IDisplay::kOptCenter, "polyend-doom " PT_VERSION " for the Polyend Tracker");
    int y = 125;
    for (const char* line : m.note) {
        if (!line || !line[0]) continue;
        d.colorRGB(255, 200, 80);
        d.text(400, int16_t(y), 27, ot::IDisplay::kOptCenter, line);
        y += 28;
    }
    // At most 8 rows, scrolled to keep the selection visible.
    constexpr int kRows = 8;
    const int first = sel < kRows ? 0 : sel - kRows + 1;
    y += 20;
    for (int i = first; i < m.count && i < first + kRows; ++i, y += 34) {
        if (i == sel) {
            d.colorRGB(120, 20, 10);
            d.begin(ot::IDisplay::kRects);
            d.vertex2f(200 * 16, (y - 15) * 16);
            d.vertex2f(600 * 16, (y + 15) * 16);
            d.end();
        }
        const uint8_t c = i == sel ? 255 : 190;
        d.colorRGB(c, c, c);
        d.text(400, int16_t(y), 29, ot::IDisplay::kOptCenter, m.items[i]);
    }
    d.colorRGB(140, 140, 140);
    d.text(400, 450, 26, ot::IDisplay::kOptCenter,
           m.canGoBack ? "Up / Down or encoder: choose    Play or encoder push: select    F1: back"
                       : "Up / Down or encoder: choose    Play or encoder push: select");
    d.swap();
}

// The selected index, or -1 (F1, when canGoBack). `idle` runs about every
// 2 s and returns true if it changed the menu (redrawn from scratch).
int runMenu(const Menu& m, int sel, bool (*idle)() = nullptr) {
    uint32_t lastIdle = millis();
    for (;;) {
        if (sel >= m.count) sel = m.count ? m.count - 1 : 0;
        drawMenu(m, sel);
        gHal->service();
        ot::InputEvent ev;
        while (gHal->input->poll(ev)) {
            int move = 0;
            if (ev.type == ot::InputEvent::kEncoder) move = ev.delta;
            if (ev.type != ot::InputEvent::kButtonDown && move == 0) continue;
            if (ev.type == ot::InputEvent::kButtonDown) {
                if (ev.id == kBtnUp) move = -1;
                if (ev.id == kBtnDown) move = 1;
                if ((ev.id == kBtnPlay || ev.id == kBtnEncPush || ev.id == kBtnF2) && m.count) return sel;
                if (ev.id == kBtnF1 && m.canGoBack) return -1;
            }
            if (m.count) sel = ((sel + move) % m.count + m.count) % m.count;
        }
        if (idle && millis() - lastIdle > 2000) {
            lastIdle = millis();
            if (idle()) return -2;
        }
    }
}

[[noreturn]] void installFirmware(const char* image) {
    char line[96];
    snprintf(line, sizeof line, "Installing %s ...", image);
    message(line);
    gHal->audio->setOutputEnabled(false);
    ot::hal::diagLogf("doom: firmware update to %s", image);
    ot::hal::requestFirmwareUpdate(*gHal->storage, image);   // returns only on failure
    Menu m = {"Update failed", {}, {"Restart"}, 1, false};
    static char what[96];
    snprintf(what, sizeof what, "The update bootloader did not start for %s.", image);
    m.note[0] = what;
    m.note[1] = ot::hal::eepromPartitionUsable() ? "" : "This unit's EEPROM partition is not confirmed; updates are refused.";
    m.note[2] = "Alternative: /Firmware/ot_restore.txt naming the image, then power-cycle.";
    runMenu(m, 0);
    SCB_AIRCR = 0x05FA0004;   // system reset
    for (;;) {}
}

// Firmware update: pick an image from /Firmware, confirm, install. Returns on
// "back" (F1 or Cancel).
void firmwareMenu() {
    for (;;) {
        scanImages();
        Menu m = {"Firmware update", {}, {}, 0, true};
        if (gImageCount == 0) {
            m.note[0] = "No firmware image in /Firmware on the SD card.";
            m.note[1] = "Copy one there (e.g. PolyendTracker_1.9.2.ptf or an OpenTracker image).";
        } else {
            m.note[0] = "Install another firmware. Doom stays in /Firmware for later.";
        }
        for (int i = 0; i < gImageCount; ++i) m.items[m.count++] = gImages[i];
        m.items[m.count++] = "Back";
        const int sel = runMenu(m, 0);
        if (sel < 0 || sel >= gImageCount) return;
        static char question[96];
        snprintf(question, sizeof question, "Install %s now?", gImages[sel]);
        Menu confirm = {"Firmware update", {question, "The Tracker restarts into it. Unsaved Doom progress is lost."},
                        {"Cancel", "Install"}, 2, true};
        if (runMenu(confirm, 0) == 1) installFirmware(gImages[sel]);
    }
}

// The IWAD to start (index into gWads).
int launcher() {
    for (int sel = 0;;) {
        Menu m = {"DOOM", {}, {}, 0, false};
        if (gWadCount == 0) {
            m.note[0] = "No IWAD found in /DOOM on the SD card.";
            m.note[1] = "Copy doom1.wad (shareware), doom.wad, doom2.wad or a Freedoom IWAD there.";
        }
        for (int i = 0; i < gWadCount; ++i) m.items[m.count++] = gWads[i];
        m.items[m.count++] = "Firmware update ...";
        sel = runMenu(m, sel, gWadCount ? nullptr : [] {   // card swapped in
            scanWads();
            return gWadCount > 0;
        });
        if (sel == -2) {
            sel = 0;
            continue;
        }
        if (sel < gWadCount) return sel;
        firmwareMenu();
    }
}

// Extra command-line arguments from /DOOM/args.txt (e.g. "-file mod.wad -warp 1 2").
int readArgs(char* buf, size_t cap, char** argv, int maxArgs) {
    char path[96];
    snprintf(path, sizeof path, "%s/args.txt", kDoomDir);
    ot::IStorage& fs = *gHal->storage;
    const ot::IStorage::Handle h = fs.open(path, ot::IStorage::kRead);
    if (h < 0) return 0;
    const int32_t n = fs.read(h, buf, uint32_t(cap - 1));
    fs.close(h);
    if (n <= 0) return 0;
    buf[n] = 0;
    int argc = 0;
    for (char* p = buf; *p && argc < maxArgs;) {
        while (*p && isspace(static_cast<unsigned char>(*p))) *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && !isspace(static_cast<unsigned char>(*p))) ++p;
    }
    return argc;
}

// ---- pad LEDs: weapons, keys, health and armour bars -------------------------

void updateLeds() {
    static uint32_t lastMs = 0;
    static uint8_t shown[48];
    static bool first = true;
    const uint32_t now = millis();
    if (now - lastMs < 100) return;
    lastMs = now;
    pt_hud_t h;
    pt_hud(&h);
    uint8_t level[48] = {};
    if (h.inLevel) {
        for (int w = 0; w < 7; ++w)
            level[w] = h.readyWeapon == w + 1 ? 255 : (h.weaponsOwned >> w & 1) ? 40 : 0;
        for (int k = 0; k < 3; ++k) level[7 + k] = (h.keys >> k & 1) ? 255 : 0;
        const int hp = (h.health * 12 + 99) / 100, ap = (h.armor * 12 + 99) / 100;
        for (int i = 0; i < 12; ++i) {
            level[12 + i] = i < hp ? 160 : 0;
            level[24 + i] = i < ap ? 100 : 0;
        }
    }
    if (!first && memcmp(level, shown, sizeof level) == 0) return;
    first = false;
    memcpy(shown, level, sizeof level);
    for (int p = 0; p < 48 && p < gHal->leds->padCount(); ++p) gHal->leds->setPad(uint8_t(p), level[p], level[p], level[p]);
    gHal->leds->show();
}

// Once a second: the stats line for the on-screen overlay (Aux toggles it);
// every 5 s also to the USB serial log. Per frame: Doom's own time (game
// logic, rendering, sound) without its sleeps, the waits in pt_present for
// the previous upload and the display refresh, and the rest of pt_present.
bool gOverlayOn = false;
uint64_t gSleepUs = 0;   // DG_SleepMs time (Doom ahead of its 35 Hz tics)

void statsTick() {
    static uint32_t lastMs = 0, logCount = 0;
    static uint64_t lastSleepUs = 0;
    const uint32_t now = millis();
    if (now - lastMs < 1000) return;
    PtFrameStats st;
    pt_display_take_stats(&st);
    const uint32_t windowMs = now - lastMs;
    lastMs = now;
    const uint64_t sleepUs = gSleepUs - lastSleepUs;
    lastSleepUs = gSleepUs;
    const uint32_t n = st.frames ? st.frames : 1;
    const uint32_t fps10 = st.frames * 10000u / windowMs;
    const uint64_t busyUs = st.gameUs > sleepUs ? st.gameUs - sleepUs : 0;
    auto ms10 = [n](uint64_t us) { return static_cast<unsigned long>(us / n / 100); };   // tenths of a ms per frame
    char line[128];
    snprintf(line, sizeof line, "%lu.%lu fps | ms/frame: doom %lu.%lu idle %lu.%lu upload %lu.%lu refresh %lu.%lu%s",
             static_cast<unsigned long>(fps10 / 10), static_cast<unsigned long>(fps10 % 10), ms10(busyUs) / 10,
             ms10(busyUs) % 10, ms10(sleepUs) / 10, ms10(sleepUs) % 10, ms10(st.waitUploadUs) / 10,
             ms10(st.waitUploadUs) % 10, ms10(st.waitDisplayUs) / 10, ms10(st.waitDisplayUs) % 10,
             st.asyncUpload ? "" : " | no DMA");
    pt_display_set_overlay(gOverlayOn, line);
    if (++logCount % 5 == 0)
        ot::hal::diagLogf("doom: %s, present %lu.%lu ms, heap %lu / %lu KB", line, ms10(st.presentUs) / 10,
                          ms10(st.presentUs) % 10, static_cast<unsigned long>(pt_heap_used() / 1024),
                          static_cast<unsigned long>(pt_heap_size() / 1024));
}

}  // namespace

ot::hal::HalSet& pt_hal() { return *gHal; }

void pt_pump() {
    gHal->service();
    if (!gDoomRunning) return;
    ot::InputEvent ev;
    while (gHal->input->poll(ev)) {
        switch (ev.type) {
        case ot::InputEvent::kButtonDown:
            if (ev.id == kBtnAux) {   // stats overlay on / off (not a Doom key)
                gOverlayOn = !gOverlayOn;
                pt_display_set_overlay(gOverlayOn, gOverlayOn ? "measuring ..." : "");
                break;
            }
            pt_button(1, ev.id);
            break;
        case ot::InputEvent::kButtonUp: pt_button(0, ev.id); break;
        case ot::InputEvent::kPadDown: pt_pad(1, ev.id); break;
        case ot::InputEvent::kPadUp: pt_pad(0, ev.id); break;
        case ot::InputEvent::kEncoder: pt_encoder(ev.delta); break;
        default: break;
        }
    }
}

// ---- doomgeneric hooks ------------------------------------------------------------

extern "C" {

void DG_Init(void) {}
void DG_DrawFrame(void) {}
void DG_SetWindowTitle(const char* title) { (void)title; }
uint32_t DG_GetTicksMs(void) { return gHal->clock->millis(); }

void DG_SleepMs(uint32_t ms) {
    const uint32_t t0 = gHal->clock->millis(), u0 = gHal->clock->micros();
    do pt_pump();
    while (gHal->clock->millis() - t0 < ms);
    gSleepUs += gHal->clock->micros() - u0;
}

void pt_midi_send(const uint8_t* msg, int len) {
    gHal->midi->send(ot::MidiPortId::kDin, msg, uint32_t(len));
    gHal->midi->send(ot::MidiPortId::kUsb, msg, uint32_t(len));
}

// _exit (syscalls.cpp): Doom quit (status 0) or I_Error. Restart, or install
// another firmware.
[[noreturn]] void pt_device_exit(int status) {
    gDoomRunning = false;
    gHal->audio->setOutputEnabled(false);
    gHal->leds->clear();
    gHal->leds->show();
    const char* err = pt_error_text();
    ot::hal::diagLogf("doom: exit %d %s", status, err);
    static char lines[3][72];
    Menu m = {status == 0 || !err[0] ? "Doom has quit" : "Doom stopped", {}, {"Restart Doom", "Firmware update ..."}, 2,
              false};
    if (status != 0 && err[0]) {
        // The error in up to three lines of 70 characters.
        const size_t len = strlen(err);
        for (int row = 0; row < 3 && size_t(row) * 70 < len; ++row) {
            snprintf(lines[row], sizeof lines[row], "%.70s", err + row * 70);
            m.note[row] = lines[row];
        }
    }
    for (;;) {
        if (runMenu(m, 0) == 0) SCB_AIRCR = 0x05FA0004;   // system reset: boots this image again
        firmwareMenu();
    }
}

}  // extern "C"

void otAppMain(ot::hal::HalSet& hal) {
    gHal = &hal;
    // The SDRAM passed its boot test; nothing has used these regions yet.
    memset(_ssdram_bss, 0, size_t(_esdram_bss - _ssdram_bss));
    memset(_ssdram_doom, 0, size_t(_esdram_doom - _ssdram_doom));
    memcpy(_ssdram_data, _sisdram_data, size_t(_esdram_data - _ssdram_data));
    pt_heap_enable();
    pt_syscalls_init(hal.storage);
    pt_display_init(hal.display);
    ot::hal::diagLogf("doom: polyend-doom %s, heap %lu KB in SDRAM", PT_VERSION,
                      static_cast<unsigned long>(pt_heap_size() / 1024));

    hal.audio->setCallback(audioFill, nullptr);
    hal.audio->start();
    hal.audio->setOutputEnabled(true);
    // The codec init leaves the headphone amp muted (line out is on); open it
    // at OpenTracker's boot level (core/ui/ui.h kHeadphoneBootLevel), as its
    // app_main.cpp does. IAudioOut has no volume control.
    ot::hal::audio().setHeadphoneVolume(kHeadphoneLevel);

    hal.storage->mkdir(kDoomDir);
    scanWads();
    const int wad = launcher();

    static char iwad[96];
    snprintf(iwad, sizeof iwad, "%s/%s", kDoomDir, gWads[wad]);
    static char argsBuf[256];
    static char* argv[32];
    int argc = 0;
    argv[argc++] = const_cast<char*>("doom");
    argv[argc++] = const_cast<char*>("-iwad");
    argv[argc++] = iwad;
    argv[argc++] = const_cast<char*>("-nogui");
    argv[argc++] = const_cast<char*>("-mb");
    argv[argc++] = const_cast<char*>(PT_ZONE_MB);
    argc += readArgs(argsBuf, sizeof argsBuf, argv + argc, 32 - argc - 1);
    argv[argc] = nullptr;

    message("Loading ...");   // D_DoomMain reads the WAD directory and caches the startup lumps
    gDoomRunning = true;
    pt_doom_start(argc, argv);
    for (;;) {
        pt_pump();
        pt_doom_frame();
        updateLeds();
        statsTick();
    }
}
