// Games session (spec 14 §1.6-§4): the loaded ROM and emulator, frame pacing
// with frame skip, frame upload into FT81x RAM_G and the audio ring the
// audio callback pulls from while gaming.
//
// Memory: everything large lives in one caller-provided arena (on the device
// the free tail of the sample pool, or the whole pool lent by the Games
// screen, SDRAM). Layout, 16-byte aligned:
//   ROM image | CHR RAM 8 KB | PRG RAM 8 KB | VRAM 2 KB   (fingerprinted)
//   sprite lines 1.9 KB | frame audio 2 KB | audio ring 8 KB | frame 60 KB
// The emulator's hot state (~6 KB: CPU RAM, nametables, OAM, line buffers) is
// one static object in internal RAM.
#pragma once

#include <cstdint>

#include "games/nes.h"
#include "hal_api.h"

namespace ot {
namespace games {

constexpr uint32_t kMaxRomBytes = 0xE2000;   // spec 14 §1.6: 925,696 bytes
constexpr int kFrameX = 144, kFrameY = 0;    // spec 14 §3.2: 256 x 240 at x2 -> x 144..655
constexpr uint8_t kFrameScale = 2;

enum class LoadStatus : uint8_t { Ok, OpenFailed, TooLarge, ReadFailed, NoMemory, BadRom };

class Session {
public:
    static constexpr uint32_t kRingSamples = 4096;   // audio ring (power of two), ~93 ms
    static constexpr int kMaxFramesPerPass = 4;       // catch-up limit before the game slows down

    // Arena bytes needed for a ROM file of romBytes.
    static uint32_t arenaBytes(uint32_t romBytes);

    // Read `path` into the arena and power on (spec 14 §1.6 step 4).
    LoadStatus load(IStorage& fs, const char* path, uint8_t* arena, uint32_t arenaCap);
    // Human-readable reason for the last failed load ("" after success).
    const char* errorText() const { return err_; }
    void unload();
    bool loaded() const { return loaded_; }
    bool jammed() const { return nes_.jammed(); }
    const uint8_t* arena() const { return arena_; }

    // Pad 1 state (Nes::Pad bits), sampled at the start of each frame (§2.1).
    void setPad(uint8_t bits) { pad_ = bits; }
    uint8_t pad() const { return pad_; }

    // Frame pacing (spec 14 §3.3): call every main-loop pass. Runs the frames
    // that are due at 60.1 Hz (NTSC), all but the last without drawing; when
    // more than kMaxFramesPerPass are due the backlog is dropped (the game
    // slows down). Returns true when a drawn frame is ready for upload().
    bool service(uint32_t nowUs);
    void resync(uint32_t nowUs);         // after entry / resume: no catch-up burst
    // Upload the rows that changed since the last upload into RAM_G at
    // IDisplay::kRamGGameFrame. Returns the bytes written.
    uint32_t upload(IDisplay& d);
    void invalidateDisplay();            // next upload sends the whole frame

    // Audio (spec 14 §4): the IAudioOut callback while gaming. Mono APU
    // output to both channels at unity; silence when inactive or starved.
    // ctx is ignored (the App passes the engine's ctx so that a callback/ctx
    // pair is never mixed up during the swap).
    static void fillAudio(int16_t* out, uint32_t frames, void* ctx);
    void setAudioActive(bool on);
    uint32_t ringFill() const { return wr_ - rd_; }

    // Checksum of the ROM and cartridge RAM in the arena: a suspended game is
    // only resumed when nothing overwrote it (spec 14 §1.7, OpenTracker).
    uint32_t fingerprint() const;

    // Statistics (tests, diagnostics).
    uint32_t framesRun() const { return framesRun_; }
    uint32_t framesDrawn() const { return framesDrawn_; }
    uint32_t framesDropped() const { return framesDropped_; }
    uint32_t uploadBytes() const { return uploadBytes_; }
    const uint8_t* frame() const { return fb_; }
    Nes& nes() { return nes_; }

private:
    void pushAudio();

    Nes nes_;
    bool loaded_ = false;
    uint8_t pad_ = 0;
    uint8_t* arena_ = nullptr;
    uint32_t fingerprintBytes_ = 0;
    uint8_t* fb_ = nullptr;
    int16_t* frameAudio_ = nullptr;
    int16_t* volatile ring_ = nullptr;   // read by the audio ISR
    volatile uint32_t wr_ = 0, rd_ = 0;   // ring indices: main loop writes wr_, audio ISR rd_
    volatile bool audioActive_ = false;
    int16_t last_ = 0;                    // ISR: held sample on underrun
    bool started_ = false;
    uint32_t lastUs_ = 0;
    int32_t accUs_ = 0;
    uint32_t framesRun_ = 0, framesDrawn_ = 0, framesDropped_ = 0, uploadBytes_ = 0;
    char err_[64] = {};
};

// The single session (static storage: the emulator's hot state).
Session& session();

}  // namespace games
}  // namespace ot
