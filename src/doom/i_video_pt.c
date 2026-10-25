// polyend-doom -- replaces doomgeneric's i_video.c.
//
// Doom renders 8-bit palette indices into pt_framebuffer() (fast memory on
// the device instead of a zone block); a finished frame and the RGB565
// palette go to pt_present() as they are. Scaling and the palette lookup are
// left to the display (FT81x PALETTED565 on the Tracker).

#include <limits.h>
#include <string.h>

#include "doomkeys.h"
#include "doomtype.h"
#include "i_video.h"
#include "tables.h"

#include "pt.h"

byte* I_VideoBuffer = NULL;

boolean screenvisible = true;
boolean screensaver_mode = false;
boolean palette_changed = false;
struct color colors[256];

int usegamma = 0;
int usemouse = 0;
float mouse_acceleration = 2.0f;
int mouse_threshold = 10;
char* video_driver = "";
int screen_width = PT_SCREEN_W;
int screen_height = PT_SCREEN_H;
int screen_bpp = 8;
int fullscreen = 1;
int aspect_ratio_correct = 1;
int show_diskicon = 0;
int diskicon_readbytes = 0;

static uint16_t pal565[256];
static boolean pal565Changed = true;

void I_GetEvent(void);   // i_input.c

void I_InitGraphics(void)
{
    I_VideoBuffer = pt_framebuffer();
    memset(I_VideoBuffer, 0, PT_SCREEN_W * PT_SCREEN_H);
    screenvisible = true;
}

void I_ShutdownGraphics(void) {}

void I_StartFrame(void) {}

void I_StartTic(void)
{
    I_GetEvent();
}

void I_UpdateNoBlit(void) {}

void I_FinishUpdate(void)
{
    pt_present(I_VideoBuffer, pal565, pal565Changed);
    pal565Changed = false;
}

void I_ReadScreen(byte* scr)
{
    memcpy(scr, I_VideoBuffer, PT_SCREEN_W * PT_SCREEN_H);
}

void I_SetPalette(byte* palette)
{
    for (int i = 0; i < 256; ++i)
    {
        const byte r = gammatable[usegamma][*palette++];
        const byte g = gammatable[usegamma][*palette++];
        const byte b = gammatable[usegamma][*palette++];
        colors[i].a = 0;
        colors[i].r = r;
        colors[i].g = g;
        colors[i].b = b;
        pal565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    palette_changed = true;
    pal565Changed = true;
}

int I_GetPaletteIndex(int r, int g, int b)
{
    int best = 0, bestDiff = INT_MAX;

    for (int i = 0; i < 256; ++i)
    {
        const int dr = r - (int)colors[i].r, dg = g - (int)colors[i].g, db = b - (int)colors[i].b;
        const int diff = dr * dr + dg * dg + db * db;
        if (diff < bestDiff)
        {
            best = i;
            bestDiff = diff;
        }
        if (diff == 0)
            break;
    }
    return best;
}

void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char* title) { (void)title; }
void I_InitWindowTitle(void) {}
void I_InitWindowIcon(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void)func; }
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean dots_on) { (void)dots_on; }
void I_CheckIsScreensaver(void) {}
