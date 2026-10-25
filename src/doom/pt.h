// polyend-doom -- the boundary between the Doom glue (src/doom, C, shared by
// every target) and a platform (src/device for the Tracker, host/ for the PC).
//
// The platform implements DG_Init, DG_DrawFrame, DG_SleepMs, DG_GetTicksMs and
// DG_SetWindowTitle (doomgeneric.h) plus the pt_* functions marked "platform"
// below; the glue implements the rest (DG_GetKey included). The link wraps
// I_Quit and I_Error (-Wl,--wrap): quitting really exits, and the error text
// stays readable through pt_error_text().
#ifndef PT_H
#define PT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PT_SCREEN_W 320
#define PT_SCREEN_H 200

// ---- glue -> platform ------------------------------------------------------

// platform: show a finished 320x200 frame of palette indices. pal565 is the
// current 256-entry RGB565 palette (gamma applied); paletteChanged says it
// differs from the one passed with the previous frame.
void pt_present(const uint8_t* frame, const uint16_t* pal565, int paletteChanged);

// platform: the 320x200 buffer Doom renders into (fast memory on the device).
uint8_t* pt_framebuffer(void);

// platform: queue one MIDI message (1..3 bytes) on every MIDI output.
void pt_midi_send(const uint8_t* msg, int len);

// ---- platform -> glue ------------------------------------------------------

// Starts Doom with the given command line and returns after its setup
// (D_DoomMain). Then call pt_doom_frame() forever.
void pt_doom_start(int argc, char** argv);
void pt_doom_frame(void);

// Audio: fill `frames` interleaved stereo int16 frames at PT_MIX_RATE. Runs in
// the audio interrupt on the device; never blocks.
#define PT_MIX_RATE 44100
void pt_mix(int16_t* out, uint32_t frames);

// Input, main loop only: a Tracker button (OpenTracker btn:: id) or pad
// (0..47) press or release, an encoder turn (detents, + clockwise), or a Doom
// key (doomkeys.h) directly.
void pt_button(int pressed, int id);
void pt_pad(int pressed, int pad);
void pt_encoder(int detents);
void pt_key(int pressed, unsigned char doomKey);

// The message of the last I_Error ("" if none), for the platform's exit path.
const char* pt_error_text(void);

// On-screen help the platform draws around / over the frame.
typedef struct {
    int typing;           // Doom waits for a save name: show the keyboard
    int keySel;           // highlighted keyboard cell, 0..47 (pad layout)
    int help;             // the controls card is open (Help toggles it)
} pt_ui_t;
void pt_ui(pt_ui_t* out);
// Keyboard cell labels in pad layout (12 per row, 4 rows); "" = unused.
const char* pt_keyboard_label(int cell);
// Label of screen button F1..F8 (f = 0..7) in the current context.
const char* pt_fkey_label(int f);
// The controls card: rows of {control, action}, ended by {NULL, NULL}.
extern const char* const pt_help_rows[][2];

// HUD state for the pad LEDs.
typedef struct {
    int inLevel;          // a level is being played (not menu / intermission)
    int health;           // 0..200
    int armor;            // 0..200
    int weaponsOwned;     // bit n = weapon slot n+1 (1..7) owned
    int readyWeapon;      // slot 1..7, 0 = none
    int keys;             // bit 0..2 = blue, yellow, red (card or skull)
    int x, y;             // player position, map units (tests, diagnostics)
} pt_hud_t;
void pt_hud(pt_hud_t* out);

#ifdef __cplusplus
}
#endif

#endif  // PT_H
