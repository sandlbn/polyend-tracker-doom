// polyend-doom -- Doom entry points, input queue and HUD state (pt.h).

#include "d_event.h"
#include "d_main.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "m_argv.h"

#include "pt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void M_FindResponseFile(void);   // m_argv.c
void D_DoomMain(void);           // d_main.c
void pt_controls(void);          // pt_input.c
extern boolean menuactive;       // m_menu.c

void pt_doom_start(int argc, char** argv)
{
    myargc = argc;
    myargv = argv;
    M_FindResponseFile();
    pt_controls();
    D_DoomMain();
}

// ---- exits (-Wl,--wrap=I_Quit,--wrap=I_Error) ---------------------------------

void __real_I_Quit(void);
void __real_I_Error(char* error, ...);

static char errorText[256];

const char* pt_error_text(void)
{
    return errorText;
}

void __wrap_I_Quit(void)
{
    __real_I_Quit();   // the atexit list (saves doom.cfg)
    exit(0);           // doomgeneric's I_Quit returns without ORIGCODE
}

void __wrap_I_Error(char* error, ...)
{
    if (errorText[0] == 0)
    {
        va_list ap;
        va_start(ap, error);
        vsnprintf(errorText, sizeof(errorText), error, ap);
        va_end(ap);
    }
    __real_I_Error("%s", errorText);   // prints, runs the error exit functions, exit(-1)
    exit(-1);
}

static void JogService(void);
int pt_encoder_typing(int detents);   // pt_input.c: moves the keyboard highlight

void pt_doom_frame(void)
{
    JogService();
    doomgeneric_Tick();
}

// ---- keys --------------------------------------------------------------------
// Single producer (pt_key, main loop) and consumer (DG_GetKey, called by
// I_StartTic in the same loop), so no locking.

#define KEYQ_SIZE 64   // power of two
static unsigned short keyq[KEYQ_SIZE];   // pressed << 8 | key
static unsigned keyqRd, keyqWr;

void pt_key(int pressed, unsigned char doomKey)
{
    if (keyqWr - keyqRd >= KEYQ_SIZE)
        return;   // full: drop (the game polls every tic, so this is a burst)
    keyq[keyqWr++ & (KEYQ_SIZE - 1)] = (unsigned short)((pressed ? 0x100 : 0) | doomKey);
}

int DG_GetKey(int* pressed, unsigned char* key)
{
    if (keyqRd == keyqWr)
        return 0;
    const unsigned short e = keyq[keyqRd++ & (KEYQ_SIZE - 1)];
    *pressed = (e >> 8) != 0;
    *key = (unsigned char)e;
    return 1;
}

// ---- encoder -------------------------------------------------------------------
// In menus and screens without a player view, a detent is one Up / Down key
// tap. In a level it turns like a mouse: G_Responder scales data2 by
// (mouseSensitivity + 5) / 10 and G_BuildTiccmd turns by 8x that (65536 =
// full circle). 340 -> about 15 degrees per detent at the default
// sensitivity 5, so one turn of the encoder (24 detents) is a full circle.
//
// Jog walking: with the encoder push held, the encoder walks instead
// (clockwise forward). Each detent keeps the player moving for
// JOG_MS_PER_DETENT more, up to JOG_MAX_AHEAD_MS ahead, so a steady spin
// walks smoothly and stopping the wheel stops soon after. Movement goes
// through the joystick axis (ev_joystick data3 < 0 = forward), which is
// state of its own: held arrow keys keep working, Shift still runs.

#define TURN_PER_DETENT 340
#define JOG_MS_PER_DETENT 120
#define JOG_MAX_AHEAD_MS 400

static int jogHeld;          // encoder push down during a level
static int jogDir;           // joystick y posted last: -1 forward, 1 back, 0 stopped
static uint32_t jogUntil;    // DG_GetTicksMs() when the walk ends

static void PostJoyY(int y)
{
    event_t ev;
    ev.type = ev_joystick;
    ev.data1 = 0;   // no buttons (a button would open the menu on the title screen)
    ev.data2 = 0;
    ev.data3 = y;
    ev.data4 = 0;
    D_PostEvent(&ev);
    jogDir = y;
}

static void JogService(void)
{
    if (jogDir == 0)
        return;
    if (menuactive || gamestate != GS_LEVEL || (int32_t)(DG_GetTicksMs() - jogUntil) >= 0)
        PostJoyY(0);
}

static boolean InLevel(void)
{
    return !menuactive && gamestate == GS_LEVEL;
}

int pt_jog_button(int pressed)
{
    if (pressed && !InLevel())
        return 0;   // menus: the encoder push stays Enter
    if (!pressed && !jogHeld)
        return 0;
    jogHeld = pressed;
    return 1;
}

static void Jog(int detents)
{
    const int dir = detents > 0 ? -1 : 1;
    const uint32_t now = DG_GetTicksMs();
    if (dir != jogDir || (int32_t)(jogUntil - now) < 0)
        jogUntil = now;
    const int n = detents > 0 ? detents : -detents;
    jogUntil += (uint32_t)(n * JOG_MS_PER_DETENT);
    if ((int32_t)(jogUntil - now) > JOG_MAX_AHEAD_MS)
        jogUntil = now + JOG_MAX_AHEAD_MS;
    if (dir != jogDir)
        PostJoyY(dir);
}

void pt_encoder(int detents)
{
    if (detents == 0 || pt_encoder_typing(detents))
        return;
    if (!InLevel())
    {
        const unsigned char k = detents > 0 ? KEY_DOWNARROW : KEY_UPARROW;
        for (int n = detents > 0 ? detents : -detents; n > 0; --n)
        {
            pt_key(1, k);
            pt_key(0, k);
        }
        return;
    }
    if (jogHeld)
    {
        Jog(detents);
        return;
    }
    event_t ev;
    ev.type = ev_mouse;
    ev.data1 = 0;   // no mouse buttons
    ev.data2 = detents * TURN_PER_DETENT;
    ev.data3 = 0;
    ev.data4 = 0;
    D_PostEvent(&ev);
}

// ---- HUD -----------------------------------------------------------------------

void pt_hud(pt_hud_t* out)
{
    const player_t* p = &players[consoleplayer];
    out->inLevel = gamestate == GS_LEVEL && !menuactive && p->mo != NULL;
    out->health = p->health < 0 ? 0 : p->health;
    out->armor = p->armorpoints;
    // Weapon slots as the number keys select them (G_BuildTiccmd).
    static const signed char slotOf[NUMWEAPONS] = {
        1,  // wp_fist
        2,  // wp_pistol
        3,  // wp_shotgun
        4,  // wp_chaingun
        5,  // wp_missile
        6,  // wp_plasma
        7,  // wp_bfg
        1,  // wp_chainsaw
        3,  // wp_supershotgun
    };
    out->weaponsOwned = 0;
    for (int w = 0; w < NUMWEAPONS; ++w)
        if (p->weaponowned[w])
            out->weaponsOwned |= 1 << (slotOf[w] - 1);
    out->readyWeapon = p->readyweapon < NUMWEAPONS ? slotOf[p->readyweapon] : 0;
    out->x = p->mo != NULL ? p->mo->x >> FRACBITS : 0;
    out->y = p->mo != NULL ? p->mo->y >> FRACBITS : 0;
    out->keys = 0;
    if (p->cards[it_bluecard] || p->cards[it_blueskull])
        out->keys |= 1;
    if (p->cards[it_yellowcard] || p->cards[it_yellowskull])
        out->keys |= 2;
    if (p->cards[it_redcard] || p->cards[it_redskull])
        out->keys |= 4;
}
