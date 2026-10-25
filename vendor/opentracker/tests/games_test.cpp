// Games (spec 14): NES core on tiny synthetic ROMs assembled here (no
// third-party ROMs), ROM listing, session pacing/upload/audio and the Games
// screen flow (browser, launch, suspend/resume, exit, errors, lent memory).
#include <cstdlib>
#include <vector>

#include "test.h"
#include "ui_fakes.h"
#include "formats/wav.h"
#include "games/nes.h"
#include "games/rom_list.h"
#include "games/session.h"
#include "ui/screen_games.h"

using namespace ot;
using namespace ot::games;

namespace {

// ---------------------------------------------------------------------------
// Synthetic ROMs

void put(std::vector<uint8_t>& prg, uint32_t off, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) prg[off++] = b;
}

std::vector<uint8_t> ines(const std::vector<uint8_t>& prg, uint8_t chr8, uint8_t mapper, bool vertical) {
    std::vector<uint8_t> rom = {'N', 'E', 'S', 0x1A, uint8_t(prg.size() / 16384), chr8,
                                uint8_t((mapper << 4) | (vertical ? 1 : 0)), uint8_t(mapper & 0xF0), 0, 0, 0, 0, 0, 0, 0, 0};
    rom.insert(rom.end(), prg.begin(), prg.end());
    rom.resize(rom.size() + size_t(chr8) * 8192, 0);
    return rom;
}

// NROM, 16 KB PRG at $C000 (mirrored at $8000), 8 KB CHR RAM. Sets the
// backdrop to $16 and colour 1 to $2A, draws tile 1 (solid colour 1) at the
// top-left nametable cell, plays a 440 Hz pulse, enables rendering and NMI,
// then reads the pad into $10 forever (each complete read copied to $13);
// the NMI counts frames in $11.
std::vector<uint8_t> nromTestRom() {
    std::vector<uint8_t> prg(16384, 0xEA);
    put(prg, 0x0000, {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,                      // SEI CLD LDX #$FF TXS
        0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x01, 0x20,    // $2000 = $2001 = 0
        0x2C, 0x02, 0x20, 0x10, 0xFB,                      // vblank wait 1
        0x2C, 0x02, 0x20, 0x10, 0xFB,                      // vblank wait 2
        0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,   // $2006 = $3F00
        0xA9, 0x16, 0x8D, 0x07, 0x20, 0xA9, 0x2A, 0x8D, 0x07, 0x20,   // palette 0 = $16, 1 = $2A
        0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x10, 0x8D, 0x06, 0x20,   // $2006 = $0010 (tile 1)
        0xA2, 0x08, 0xA9, 0xFF, 0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xF8,   // plane 0 = $FF x 8
        0xA2, 0x08, 0xA9, 0x00, 0x8D, 0x07, 0x20, 0xCA, 0xD0, 0xF8,   // plane 1 = $00 x 8
        0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20,   // $2006 = $2000
        0xA9, 0x01, 0x8D, 0x07, 0x20,                      // nametable (0,0) = tile 1
        0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20,    // scroll 0, 0
        0xA9, 0x01, 0x8D, 0x15, 0x40,                      // $4015: pulse 1 on
        0xA9, 0xBF, 0x8D, 0x00, 0x40,                      // duty 50 %, halt, constant volume 15
        0xA9, 0x00, 0x8D, 0x01, 0x40,                      // sweep off
        0xA9, 0xFD, 0x8D, 0x02, 0x40,                      // timer $0FD (~440 Hz)
        0xA9, 0x00, 0x8D, 0x03, 0x40,                      // length load
        0xA9, 0x40, 0x8D, 0x17, 0x40,                      // frame IRQ off
        0xA9, 0x0A, 0x8D, 0x01, 0x20,                      // background on (+ left 8 px)
        0xA9, 0x80, 0x8D, 0x00, 0x20,                      // NMI on
        // $C088 loop: strobe and read 8 pad bits into $10.
        0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40,
        0xA2, 0x08, 0xAD, 0x16, 0x40, 0x4A, 0x26, 0x10, 0xCA, 0xD0, 0xF7,
        0xA5, 0x10, 0x85, 0x13,                            // complete reads -> $13
        0x4C, 0x88, 0xC0,
        // $C0A4 NMI: INC $11, RTI.  $C0A7 IRQ: RTI.
        0xE6, 0x11, 0x40, 0x40,
    });
    put(prg, 0x3FFA, {0xA4, 0xC0, 0x00, 0xC0, 0xA7, 0xC0});
    return ines(prg, 0, 0, false);
}

// MMC3, 32 KB PRG (8 KB bank k starts with the marker $B0 + k), CHR RAM.
// Maps bank 1 at $8000 and stores its marker in $20, then runs the scanline
// IRQ with latch 20 and rendering on; the IRQ counts in $12.
std::vector<uint8_t> mmc3TestRom() {
    std::vector<uint8_t> prg(32768, 0xEA);
    for (int k = 0; k < 4; ++k) prg[size_t(k) * 8192] = uint8_t(0xB0 + k);
    put(prg, 0x6001, {
        0x78, 0xD8, 0xA2, 0xFF, 0x9A,                      // $E001: SEI CLD LDX #$FF TXS
        0xA9, 0x06, 0x8D, 0x00, 0x80,                      // bank select R6
        0xA9, 0x01, 0x8D, 0x01, 0x80,                      // R6 = 1
        0xAD, 0x00, 0x80, 0x85, 0x20,                      // $20 = marker at $8000
        0xA9, 0x14, 0x8D, 0x00, 0xC0,                      // IRQ latch 20
        0x8D, 0x01, 0xC0, 0x8D, 0x01, 0xE0,                // reload, enable
        0xA9, 0x08, 0x8D, 0x01, 0x20,                      // background on
        0xA9, 0x40, 0x8D, 0x17, 0x40,                      // frame IRQ off
        0x58,                                              // CLI
        0x4C, 0x2B, 0xE0,                                  // $E02B: JMP $E02B
        // $E02E IRQ: acknowledge, re-enable, INC $12, RTI.  $E037 NMI: RTI.
        0x8D, 0x00, 0xE0, 0x8D, 0x01, 0xE0, 0xE6, 0x12, 0x40, 0x40,
    });
    put(prg, 0x7FFA, {0x37, 0xE0, 0x01, 0xE0, 0x2E, 0xE0});
    return ines(prg, 0, 4, false);
}

struct Core {
    std::vector<uint8_t> chr = std::vector<uint8_t>(Nes::kChrRamBytes), prgRam = std::vector<uint8_t>(Nes::kPrgRamBytes),
                         vx = std::vector<uint8_t>(Nes::kVramExtraBytes);
    std::vector<int16_t> audio = std::vector<int16_t>(Nes::kAudioCap);
    std::vector<uint64_t> lines = std::vector<uint64_t>(Nes::kHeight);
    std::vector<uint32_t> fbWords = std::vector<uint32_t>(Nes::kWidth * Nes::kHeight / 4);
    uint8_t* fb() { return reinterpret_cast<uint8_t*>(fbWords.data()); }
    NesMemory mem() { return NesMemory{chr.data(), prgRam.data(), vx.data(), audio.data(), lines.data()}; }
};

Nes g_nes;   // ~6 KB: static like on the device

// ---------------------------------------------------------------------------
// Session / screen helpers

void addRom(Harness& h, const char* name, const std::vector<uint8_t>& rom) {
    h.fs.dirs.insert("Games");
    h.fs.files[std::string("Games/") + name] = rom;
}

GamesScreen& gamesScreen(Harness& h) { return *static_cast<GamesScreen*>(h.app->ui.screen(kModGames)); }

bool lastHas(const Harness& h, const char* prefix) {
    for (auto& l : h.disp.last)
        if (l.compare(0, std::strlen(prefix), prefix) == 0) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Core

TEST(core_rejects_bad_images) {
    Core c;
    std::vector<uint8_t> junk(64, 0);
    CHECK(g_nes.load(junk.data(), uint32_t(junk.size()), c.mem()) == NesError::BadHeader);
    auto rom = nromTestRom();
    CHECK(g_nes.load(rom.data(), uint32_t(rom.size() - 100), c.mem()) == NesError::Truncated);
    auto mmc5 = rom;
    mmc5[6] = 0x50;   // mapper 5
    CHECK(g_nes.load(mmc5.data(), uint32_t(mmc5.size()), c.mem()) == NesError::Mapper);
    CHECK_EQ(g_nes.mapper(), 5);
    auto empty = rom;
    empty[4] = 0;   // no PRG
    CHECK(g_nes.load(empty.data(), uint32_t(empty.size()), c.mem()) == NesError::BankSize);
    CHECK(!g_nes.loaded());
}

TEST(core_draws_nmis_and_reads_the_pad) {
    Core c;
    auto rom = nromTestRom();
    CHECK(g_nes.load(rom.data(), uint32_t(rom.size()), c.mem()) == NesError::Ok);
    CHECK_EQ(g_nes.mapper(), 0);
    uint8_t lut[64];
    for (int i = 0; i < 64; ++i) lut[i] = uint8_t(i);   // identity: the frame holds NES colours
    g_nes.setPalette(lut);
    g_nes.setPad(Nes::kA | Nes::kStart);
    for (int f = 0; f < 6; ++f) CHECK(g_nes.runFrame(c.fb()));
    const uint8_t* fb = c.fb();
    CHECK_EQ(fb[0], 0x2A);                 // tile 1, colour 1
    CHECK_EQ(fb[7 * 256 + 7], 0x2A);
    CHECK_EQ(fb[8], 0x16);                 // backdrop
    CHECK_EQ(fb[100 * 256 + 100], 0x16);
    const uint8_t nmis = g_nes.peek(0x11);
    CHECK(nmis >= 3 && nmis <= 5);         // NMI on from frame ~2
    CHECK_EQ(g_nes.peek(0x13), 0x90);      // A (bit 7) + Start (bit 4), serial order
    g_nes.setPad(Nes::kLeft);
    CHECK(g_nes.runFrame(nullptr));
    CHECK_EQ(g_nes.peek(0x13), 0x02);
    CHECK_EQ(int(g_nes.peek(0x11)), nmis + 1);
}

TEST(core_dirty_rows_and_frame_skip) {
    Core c;
    auto rom = nromTestRom();
    CHECK(g_nes.load(rom.data(), uint32_t(rom.size()), c.mem()) == NesError::Ok);
    uint8_t lut[64];
    Nes::rgb332Lut(lut);
    g_nes.setPalette(lut);
    for (int f = 0; f < 4; ++f) g_nes.runFrame(c.fb());
    g_nes.clearDirty();
    CHECK(g_nes.runFrame(c.fb()));
    int dirty = 0;
    for (int y = 0; y < Nes::kHeight; ++y) dirty += g_nes.rowDirty(y);
    CHECK_EQ(dirty, 0);                    // a static picture uploads nothing
    std::memset(c.fb(), 0x55, 256);        // row 0 differs from what the PPU draws
    CHECK(g_nes.runFrame(nullptr));        // skipped frame: fb untouched
    CHECK_EQ(c.fb()[0], 0x55);
    CHECK(g_nes.runFrame(c.fb()));
    CHECK(g_nes.rowDirty(0));
    CHECK(!g_nes.rowDirty(1));
    CHECK_EQ(c.fb()[0], lut[0x2A]);
}

TEST(core_audio_pulse) {
    Core c;
    auto rom = nromTestRom();
    CHECK(g_nes.load(rom.data(), uint32_t(rom.size()), c.mem()) == NesError::Ok);
    g_nes.setSampleRate(44100);
    uint32_t total = 0;
    int peak = 0, crossings = 0;
    int16_t prev = 0;
    for (int f = 0; f < 60; ++f) {
        g_nes.runFrame(nullptr);
        total += g_nes.audioCount();
        if (f < 10) continue;   // let the DC blocker settle
        for (uint32_t i = 0; i < g_nes.audioCount(); ++i) {
            const int16_t v = c.audio[i];
            peak = std::abs(v) > peak ? std::abs(v) : peak;
            if ((prev < 0) != (v < 0)) ++crossings;
            prev = v;
        }
    }
    CHECK(total > 60 * 733 && total < 60 * 735);   // 44100 Hz at 60.1 frames/s
    CHECK(peak > 2000);
    // 50 frames ~ 0.83 s of 440 Hz: ~730 zero crossings.
    CHECK(crossings > 650 && crossings < 800);
}

TEST(core_mmc3_banks_and_irq) {
    Core c;
    auto rom = mmc3TestRom();
    CHECK(g_nes.load(rom.data(), uint32_t(rom.size()), c.mem()) == NesError::Ok);
    CHECK_EQ(g_nes.mapper(), 4);
    CHECK_EQ(g_nes.peek(0xC000), 0xB2);   // second-last bank fixed at $C000 (mode 0)
    CHECK_EQ(g_nes.peek(0xE000), 0xB3);
    for (int f = 0; f < 3; ++f) CHECK(g_nes.runFrame(c.fb()));
    CHECK_EQ(g_nes.peek(0x20), 0xB1);     // R6 = 1 mapped bank 1 at $8000
    const int irqs = g_nes.peek(0x12);
    CHECK(irqs >= 20 && irqs <= 40);       // ~11 per frame (241 lines / 21)
}

TEST(core_palette_and_rgb332) {
    // NTSC model sanity: $0F/$1D black, $30 white, $01 blue, $06 red, $0A green.
    auto ch = [](uint32_t c, int s) { return int((c >> s) & 0xFF); };
    CHECK_EQ(Nes::nesColor(0x0F), 0u);
    CHECK_EQ(Nes::nesColor(0x1D), 0u);
    CHECK(ch(Nes::nesColor(0x30), 16) > 240 && ch(Nes::nesColor(0x30), 0) > 240);
    const uint32_t blue = Nes::nesColor(0x01), red = Nes::nesColor(0x06), green = Nes::nesColor(0x0A);
    CHECK(ch(blue, 0) > ch(blue, 16) && ch(blue, 0) > ch(blue, 8));
    CHECK(ch(red, 16) > ch(red, 8) && ch(red, 16) > ch(red, 0));
    CHECK(ch(green, 8) > ch(green, 16) && ch(green, 8) > ch(green, 0));
    uint8_t lut[64];
    Nes::rgb332Lut(lut);
    CHECK_EQ(lut[0x0F], 0x00);
    CHECK_EQ(lut[0x30], 0xFF);
}

// ---------------------------------------------------------------------------
// ROM list

TEST(rom_list_filter_sort_names) {
    CHECK(romNameQualifies("Game.nes", false));
    CHECK(romNameQualifies("GAME.NES", false));
    CHECK(romNameQualifies("x.nes.bak", false));        // substring, not suffix (§1.4)
    CHECK(!romNameQualifies("._Game.nes", false));
    CHECK(!romNameQualifies("dir.nes", true));
    CHECK(!romNameQualifies("notes.txt", false));
    CHECK(naturalCompare("a9", "a10") < 0);
    CHECK(naturalCompare("A10", "a9") > 0);
    CHECK(naturalCompare("abc", "ABD") < 0);
    CHECK(naturalCompare("1x", "x") < 0);               // a digit before a non-digit
    CHECK(naturalCompare("ab", "abc") < 0);             // shorter first
    CHECK_EQ(naturalCompare("a007", "a7"), 0);
    CHECK_EQ(romDisplayLength("Chase.NES"), 5);
    CHECK_EQ(romDisplayLength("x.nes.bak"), 1);

    FakeStorage fs;
    RomList l;
    CHECK_EQ(l.scan(fs), 0);
    CHECK(fs.dirs.count("Games"));                       // created when missing
    for (const char* n : {"b.nes", "a10.nes", "a9.NES", "readme.txt", "._a1.nes"}) fs.files[std::string("Games/") + n] = {1};
    fs.dirs.insert("Games/sub.nes");
    CHECK_EQ(l.scan(fs), 3);
    char dn[32];
    l.displayName(0, dn, sizeof(dn));
    CHECK(std::strcmp(dn, "a9") == 0);
    CHECK(std::strcmp(l.fileName(0), "a9.NES") == 0);
    CHECK(std::strcmp(l.fileName(1), "a10.nes") == 0);
    CHECK(std::strcmp(l.fileName(2), "b.nes") == 0);
    // At most 101 names (§1.4).
    for (int i = 0; i < 150; ++i) fs.files["Games/g" + std::to_string(i) + ".nes"] = {1};
    CHECK_EQ(l.scan(fs), kMaxRoms);
}

// ---------------------------------------------------------------------------
// Session

TEST(session_load_pacing_upload_audio) {
    FakeStorage fs;
    fs.dirs.insert("Games");
    fs.files["Games/t.nes"] = nromTestRom();
    Session& s = session();
    std::vector<uint32_t> arenaWords(Session::arenaBytes(uint32_t(fs.files["Games/t.nes"].size())) / 4 + 4);
    uint8_t* arena = reinterpret_cast<uint8_t*>(arenaWords.data());
    CHECK(s.load(fs, "Games/missing.nes", arena, uint32_t(arenaWords.size() * 4)) == LoadStatus::OpenFailed);
    CHECK(s.load(fs, "Games/t.nes", arena, 1000) == LoadStatus::NoMemory);
    fs.files["Games/big.nes"] = std::vector<uint8_t>(kMaxRomBytes + 1, 0);
    CHECK(s.load(fs, "Games/big.nes", arena, uint32_t(arenaWords.size() * 4)) == LoadStatus::TooLarge);
    fs.files["Games/bad.nes"] = std::vector<uint8_t>(64, 0);
    CHECK(s.load(fs, "Games/bad.nes", arena, uint32_t(arenaWords.size() * 4)) == LoadStatus::BadRom);
    CHECK(std::strlen(s.errorText()) > 0);
    CHECK(s.load(fs, "Games/t.nes", arena, uint32_t(arenaWords.size() * 4)) == LoadStatus::Ok);
    CHECK(s.loaded());

    // Pacing: nothing before a frame is due; then one drawn frame per due frame.
    uint32_t t = 5000000;
    s.resync(t);
    CHECK(!s.service(t + 1000));
    CHECK(s.service(t + Nes::kFrameUs + 10));
    CHECK_EQ(s.framesRun(), 1u);
    t += Nes::kFrameUs + 10;
    // Two frames due: the first runs without drawing.
    CHECK(s.service(t + 2 * Nes::kFrameUs));
    CHECK_EQ(s.framesRun(), 3u);
    CHECK_EQ(s.framesDrawn(), 2u);
    // A long stall drops the backlog instead of bursting.
    t += 2 * Nes::kFrameUs;
    CHECK(s.service(t + 20 * Nes::kFrameUs));
    CHECK_EQ(s.framesRun(), 4u);
    CHECK(s.framesDropped() > 0);
    t += 20 * Nes::kFrameUs;

    // Upload: the whole frame first, then only changed rows (none here).
    RecordingDisplay d;
    s.invalidateDisplay();
    CHECK_EQ(s.upload(d), IDisplay::kGameFrameBytes);
    CHECK(std::memcmp(d.ramg.data() + IDisplay::kRamGGameFrame, s.frame(), IDisplay::kGameFrameBytes) == 0);
    for (int i = 0; i < 5; ++i) {
        t += Nes::kFrameUs;
        s.service(t);
    }
    CHECK_EQ(s.upload(d), 0u);

    // Audio: silent until active, then the pulse from the ring on both channels.
    int16_t block[256];
    Session::fillAudio(block, 128, nullptr);
    CHECK_EQ(block[0], 0);
    s.setAudioActive(true);
    int peak = 0;
    for (int i = 0; i < 30; ++i) {
        t += Nes::kFrameUs;
        s.service(t);
        for (int b = 0; b < 6; ++b) {
            Session::fillAudio(block, 128, nullptr);
            for (int k = 0; k < 256; k += 2) {
                CHECK(block[k] == block[k + 1]);
                peak = std::abs(block[k]) > peak ? std::abs(block[k]) : peak;
            }
        }
    }
    CHECK(peak > 2000);
    CHECK(s.ringFill() < Session::kRingSamples);
    // Fingerprint follows the cartridge memory.
    const uint32_t fp = s.fingerprint();
    CHECK_EQ(fp, s.fingerprint());
    arena[100] ^= 0xFF;
    CHECK(fp != s.fingerprint());
    s.unload();
    CHECK(!s.loaded());
    Session::fillAudio(block, 128, nullptr);
    CHECK_EQ(block[0], 0);
}

// ---------------------------------------------------------------------------
// Games screen

TEST(screen_browser_launch_play_suspend_exit) {
    Harness h;
    addRom(h, "Zed.nes", nromTestRom());
    addRom(h, "Alpha.nes", nromTestRom());
    addRom(h, "notes.txt", {1, 2, 3});
    App& a = *h.app;
    openGames(a);
    h.run(20);
    GamesScreen& g = gamesScreen(h);
    CHECK_EQ(a.ui.active(), int(kModGames));
    CHECK(g.ready());
    CHECK_EQ(g.count(), 2);
    CHECK(h.disp.lastHasText("Alpha"));
    CHECK(!h.disp.lastHasText("1. Alpha"));   // names without a number [S snap 028]
    h.tap(btn::Down);
    CHECK_EQ(g.cursor, 1);
    h.enc(-9);                               // clamped to -5, then to the list
    CHECK_EQ(g.cursor, 0);

    // Start: the sequencer stops and the emulator takes over the audio.
    a.togglePlay(false);
    CHECK(a.seq.isPlaying());
    h.tap(btn::F7);
    CHECK(g.inGame());
    CHECK(g.game() == GamesScreen::Game::Running);
    CHECK(!g.bankLent());
    CHECK(!a.seq.isPlaying());
    CHECK(h.audio.fn == &Session::fillAudio);
    CHECK(h.audio.ctx == &a.engine);
    h.run(300);
    CHECK(session().framesRun() > 10);
    CHECK(lastHas(h, "RGB332 144 0 384000 256 240 2"));
    CHECK(!h.disp.lastHasText("Start"));     // no bottom bar over the game
    CHECK(h.disp.ramgBytes >= IDisplay::kGameFrameBytes);
    CHECK(h.audio.pump(20) > 2000);          // the pulse, music muted

    // Pad: Up held while the game reads it (serial bit 4 -> bit 3 of $13).
    h.down(btn::Up);
    h.run(60);
    CHECK_EQ(session().nes().peek(0x13), 0x08);
    h.up(btn::Up);
    h.down(btn::Play);                       // Start, not transport
    h.run(60);
    CHECK_EQ(session().nes().peek(0x13), 0x10);
    CHECK(!a.seq.isPlaying());
    h.up(btn::Play);

    // A screen key suspends; Games resumes the same game.
    const uint32_t frames = session().framesRun();
    h.tap(btn::Pattern);
    CHECK_EQ(a.ui.active(), int(kModPattern));
    CHECK(g.game() == GamesScreen::Game::Suspended);
    CHECK(h.audio.fn == &audio::AudioEngine::fillCallback);
    h.run(200);
    CHECK_EQ(session().framesRun(), frames);   // not emulated while suspended
    openGames(a);
    h.run(100);
    CHECK(g.inGame());
    CHECK(session().framesRun() > frames);

    // F8 discards the game and shows the browser.
    h.tap(btn::F8);
    CHECK(!g.inGame());
    CHECK(g.game() == GamesScreen::Game::None);
    CHECK(!session().loaded());
    CHECK(h.audio.fn == &audio::AudioEngine::fillCallback);
    CHECK(h.disp.lastHasText("Alpha"));
    // F8 in the browser: back to File.
    h.tap(btn::F8);
    CHECK_EQ(a.ui.active(), int(kModFile));
}

TEST(screen_bad_rom_reports_and_stays) {
    Harness h;
    std::vector<uint8_t> bad = nromTestRom();
    bad[6] = 0x50;   // mapper 5
    addRom(h, "bad.nes", bad);
    openGames(*h.app);
    h.run(20);
    h.tap(btn::F7);
    GamesScreen& g = gamesScreen(h);
    CHECK(!g.inGame());
    CHECK(g.game() == GamesScreen::Game::None);
    h.run(20);
    CHECK(h.disp.lastHasText("Mapper 5 not supported"));
    CHECK(h.audio.fn == &audio::AudioEngine::fillCallback);
}

TEST(screen_empty_folder) {
    Harness h;
    openGames(*h.app);
    h.run(20);
    GamesScreen& g = gamesScreen(h);
    CHECK(h.fs.dirs.count("Games"));
    CHECK_EQ(g.count(), 0);
    h.tap(btn::F7);
    CHECK(!g.inGame());
    CHECK(h.disp.lastHasText("No games"));
}

TEST(screen_lends_sample_memory_when_full) {
    // A 200 KB pool with a 120 KB sample: the free tail cannot hold the game,
    // so the pool is lent and the project's samples come back afterwards.
    Harness h(true, 100000);
    App& a = *h.app;
    std::vector<int16_t> pcm(60000);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((i * 37) & 0x3FFF);
    std::vector<uint8_t> wavFile(wav::kHeaderSize + pcm.size() * 2);
    CHECK(wavEncode(pcm.data(), uint32_t(pcm.size()), 1, wavFile.data(), wavFile.size()) == wavFile.size());
    h.fs.files["tone.wav"] = wavFile;
    CHECK(a.importSample("tone.wav", 0));
    CHECK(a.instrPresent[0]);
    const int16_t first = a.bank.data(0)[100];
    addRom(h, "t.nes", nromTestRom());
    openGames(a);
    h.run(20);
    h.tap(btn::F7);
    GamesScreen& g = gamesScreen(h);
    CHECK(g.inGame());
    CHECK(g.bankLent());
    CHECK(a.sampleBankLent);
    h.run(200);
    CHECK(session().framesRun() > 5);
    // Leaving by a screen key ends a lent game and reloads the samples.
    h.tap(btn::Pattern);
    CHECK(g.game() == GamesScreen::Game::None);
    CHECK(!a.sampleBankLent);
    for (int i = 0; i < 200 && a.jobBusy(); ++i) h.run(20);
    CHECK(!a.jobBusy());
    CHECK(a.instrPresent[0]);
    CHECK_EQ(a.bank.data(0)[100], first);
}

TEST(screen_suspended_game_dropped_when_memory_reused) {
    Harness h;
    App& a = *h.app;
    addRom(h, "t.nes", nromTestRom());
    openGames(a);
    h.run(20);
    h.tap(btn::F7);
    GamesScreen& g = gamesScreen(h);
    CHECK(g.inGame());
    h.run(100);
    h.tap(btn::Pattern);
    CHECK(g.game() == GamesScreen::Game::Suspended);
    // A sample import overwrites the pool's free tail where the game lives.
    std::vector<int16_t> pcm(20000, 1234);
    std::vector<uint8_t> wavFile(wav::kHeaderSize + pcm.size() * 2);
    CHECK(wavEncode(pcm.data(), uint32_t(pcm.size()), 1, wavFile.data(), wavFile.size()) == wavFile.size());
    h.fs.files["s.wav"] = wavFile;
    CHECK(a.importSample("s.wav", 3));
    openGames(a);
    h.run(20);
    CHECK(!g.inGame());
    CHECK(g.game() == GamesScreen::Game::None);
    CHECK(h.disp.lastHasText("Game ended"));
}
