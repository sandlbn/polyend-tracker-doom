// polyend-doom -- Tracker controls -> Doom keys, and the on-screen help the
// platform draws (pt.h). Shared by the device and the host build, which maps
// the PC keyboard onto the same button ids.
//
// Button ids are OpenTracker's (vendor/opentracker/src/core/ui/buttons.h);
// pads are 0..47, row-major from the top-left, 12 per row.

#include <stddef.h>

#include "doomkeys.h"
#include "doomtype.h"

#include "pt.h"

// Button ids used by name below.
#define BUTTON_PLAY 0x17
#define BUTTON_UP 0x1a
#define BUTTON_DELETE 0x1b
#define BUTTON_LEFT 0x1e
#define BUTTON_DOWN 0x1f
#define BUTTON_RIGHT 0x20
#define BUTTON_ENC_PUSH 0x21
#define BUTTON_HELP 0x2b

// The 5 x 5 key block, ids 0x08 + row * 5 + col. The bottom-right corner is
// laid out like WASD with strafing:   Insert  Up    Delete
//                                     Left    Down  Right
static const unsigned char buttonKey[44] = {
    // F1..F8 (screen buttons): menu, select, yes, no, save, load, quick save/load.
    [0x00] = KEY_ESCAPE, [0x01] = KEY_ENTER, [0x02] = 'y', [0x03] = 'n',
    [0x04] = KEY_F2, [0x05] = KEY_F3, [0x06] = KEY_F6, [0x07] = KEY_F9,
    // Row 0: Instr Params, Performance, File, Config, Master.
    [0x08] = KEY_F5,     // detail
    [0x09] = KEY_F8,     // messages on/off
    [0x0a] = KEY_ESCAPE,
    [0x0b] = KEY_F4,     // sound volume
    [0x0c] = KEY_F11,    // gamma
    // Row 1: Sample Loader, Playback, Editor, Recorder, Song.
    [0x0d] = '-', [0x0e] = '=',   // view size
    [0x11] = KEY_TAB,             // automap
    // Row 2: Note, Instr, Fx1, Fx2, Pattern.
    [0x12] = '[', [0x13] = ']',   // previous / next weapon (bound in pt_controls)
    [0x16] = KEY_TAB,
    // Row 3: Play, Copy, Insert, Up, Delete.
    [0x17] = KEY_FIRE, [0x18] = KEY_RALT, [0x19] = KEY_STRAFE_L, [0x1a] = KEY_UPARROW, [0x1b] = KEY_STRAFE_R,
    // Row 4: Rec, Shift, Left, Down, Right.
    [0x1c] = KEY_USE, [0x1d] = KEY_RSHIFT, [0x1e] = KEY_LEFTARROW, [0x1f] = KEY_DOWNARROW, [0x20] = KEY_RIGHTARROW,
    // Encoder push: Enter in menus; held in a level: jog walking (pt_glue.c).
    // Help opens the controls card instead of a Doom key.
    [0x21] = KEY_ENTER,
};

// Row 0: weapons 1..7, digits, automap, pause. Rows 1..3: letters for save
// names (and cheats), space, backspace.
static const unsigned char padKey[48] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', KEY_TAB, KEY_PAUSE,
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
    'y', 'z', ' ', KEY_BACKSPACE, '-', '=', 0, 0, 0, 0, 0, 0,
};

// ---- on-screen keyboard (Doom asks for a save name) ---------------------------
// Laid out like the pads, so a pad types the key drawn at its place; the
// encoder or the arrows move the highlight, Play or the encoder push types
// it. Characters are those Doom's save prompt takes (its HU font).

static const char* const keyLabel[48] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
    "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "SPACE", "DEL", ".", "!", "?", "'", "(", ")", "OK", "ESC",
};
static const unsigned char keyboardKey[48] = {
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x',
    'y', 'z', ' ', KEY_BACKSPACE, '.', '!', '?', '\'', '(', ')', KEY_ENTER, KEY_ESCAPE,
};

extern int saveStringEnter;   // m_menu.c: the save prompt takes characters

static int keySel = 12;       // 'A'
static int helpShown;

static boolean Typing(void)
{
    return saveStringEnter != 0;
}

static void Tap(unsigned char key)
{
    pt_key(1, key);
    pt_key(0, key);
}

static void MoveSel(int delta)
{
    keySel = ((keySel + delta) % 48 + 48) % 48;
}

// Typing: true when the button was the keyboard's.
static boolean KeyboardButton(int pressed, int id)
{
    if (!pressed)
        return id == BUTTON_PLAY || id == BUTTON_ENC_PUSH || id == BUTTON_DELETE || id == BUTTON_UP ||
               id == BUTTON_DOWN || id == BUTTON_LEFT || id == BUTTON_RIGHT;
    switch (id)
    {
    case BUTTON_LEFT: MoveSel(-1); return true;
    case BUTTON_RIGHT: MoveSel(1); return true;
    case BUTTON_UP: MoveSel(-12); return true;
    case BUTTON_DOWN: MoveSel(12); return true;
    case BUTTON_PLAY:
    case BUTTON_ENC_PUSH: Tap(keyboardKey[keySel]); return true;
    case BUTTON_DELETE: Tap(KEY_BACKSPACE); return true;
    default: return false;   // F1 (Esc) / F2 (Enter) and the rest as usual
    }
}

// ---- routing -----------------------------------------------------------------------

int pt_jog_button(int pressed);   // pt_glue.c: encoder push held = jog walking
int pt_encoder_typing(int detents)
{
    if (!Typing())
        return 0;
    MoveSel(detents);
    return 1;
}

void pt_button(int pressed, int id)
{
    if (id == BUTTON_HELP)
    {
        if (pressed)
            helpShown = !helpShown;
        return;
    }
    if (Typing() && KeyboardButton(pressed, id))
        return;
    if (id == BUTTON_ENC_PUSH && pt_jog_button(pressed))
        return;
    if (id < 0 || id >= (int)sizeof(buttonKey) || buttonKey[id] == 0)
        return;
    pt_key(pressed, buttonKey[id]);
}

void pt_pad(int pressed, int pad)
{
    if (pad < 0 || pad >= 48)
        return;
    if (Typing())
    {
        if (pressed)
        {
            keySel = pad;
            Tap(keyboardKey[pad]);
        }
        return;
    }
    if (padKey[pad] != 0)
        pt_key(pressed, padKey[pad]);
}

// ---- what the platform draws ------------------------------------------------------

extern boolean menuactive;   // m_menu.c

void pt_ui(pt_ui_t* out)
{
    out->typing = Typing();
    out->keySel = keySel;
    out->help = helpShown;
}

const char* pt_keyboard_label(int cell)
{
    return cell >= 0 && cell < 48 ? keyLabel[cell] : "";
}

const char* pt_fkey_label(int f)
{
    static const char* const normal[8] = {"Menu", "Enter", "Yes", "No", "Save", "Load", "Quick save", "Quick load"};
    static const char* const typing[8] = {"Cancel", "Done", "", "", "", "", "", ""};
    if (f < 0 || f >= 8)
        return "";
    if (Typing())
        return typing[f];
    if (f == 0 && menuactive)
        return "Back";
    return normal[f];
}

const char* const pt_help_rows[][2] = {
    {"Arrows", "move / turn"},
    {"Insert / Delete", "strafe left / right"},
    {"Play", "fire"},
    {"Rec", "use, open doors"},
    {"Shift (hold)", "run"},
    {"Copy (hold)", "strafe with Left / Right"},
    {"Encoder", "turn"},
    {"Encoder push + turn", "walk"},
    {"Note / Instr", "previous / next weapon"},
    {"Pattern or Song", "automap"},
    {"Pads, top row", "weapons 1-7"},
    {"Pads, rows 2-4", "letters (cheats)"},
    {"Sample Loader / Playback", "smaller / bigger view"},
    {"Master", "brightness"},
    {"Aux", "frame rate"},
    {"Help", "this card"},
    {"Power (hold)", "power off"},
    {NULL, NULL},
};

extern int key_prevweapon, key_nextweapon;   // m_controls.c

void pt_controls(void)
{
    // Not bound by default in Chocolate Doom. A doom.cfg entry overrides these.
    key_prevweapon = '[';
    key_nextweapon = ']';
}
