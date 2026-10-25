// Games module (spec 14): ROM browser over Games/ and the NES game screen.
#pragma once

// OT_GAMES (reimpl/Makefile, docs/OPTIMIZATION_2X.md OP-4): 1 = Games module
// built in (default, also host tests and the simulator), 0 = compiled out:
// no NES core, no Games screen, no File F7 "Games" entry.
#ifndef OT_GAMES
#define OT_GAMES 1
#endif

#include "ui/ui.h"

#if OT_GAMES
#include <cstdint>

#include "games/rom_list.h"

namespace ot {

// File screen F7 "Games" (spec 14 §1.1, spec 05 E1.5): switch to the Games
// module. Resumes a suspended game directly (spec 14 §1.7).
void openGames(App& app);

class GamesScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Games"; }
    void start(int opt) override;
    void stop() override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void onPad(uint8_t pad, uint8_t state) override { (void)pad; (void)state; }   // pads unbound (§1.5, §2.1)
    void onEncoder(int d) override;
    bool fullScreen() const override { return inGame_; }

    // spec 14 §1.2 game state (+0x57).
    enum class Game : uint8_t { None = 0, Running = 1, Suspended = 2 };
    static constexpr int kRows = 14;          // spec 14 §1.5 list rows

    Game game() const { return game_; }
    bool inGame() const { return inGame_; }
    bool ready() const { return ready_; }
    int count() const { return roms_.count(); }
    const games::RomList& roms() const { return roms_; }
    // True while the whole sample memory is lent to the emulator (the
    // project's free sample memory was too small for the ROM).
    bool bankLent() const { return lent_; }
    int cursor = 0;                           // persists between visits (§1.2 +0x4d)

private:
    void showBrowser();
    void refreshBar();
    void launch();
    void enterGame();
    void leaveGame();
    void endGame();
    void returnBank();
    void stopTransport();
    uint32_t micros() const;

    games::RomList roms_;
    bool ready_ = false;
    bool inGame_ = false;
    bool lent_ = false;
    Game game_ = Game::None;
    uint32_t print_ = 0;                      // arena fingerprint at suspend
    uint32_t tailUsed_ = 0;                   // sample memory in use when the game started (tail mode)
    uint8_t keys_ = 0;                        // NES pad bits held
    int top_ = 0;                             // first visible list row
};

}  // namespace ot
#endif  // OT_GAMES
