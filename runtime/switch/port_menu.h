// MSR-nx: the port menu (hold minus): screen layout, CPU clock and button mapping, saved to
// sdmc:/switch/msr/settings.ini. While it is open the game is paused.
#pragma once
#include <switch.h>

enum { LAYOUT_BIG_SMALL, LAYOUT_SIDE, LAYOUT_STACKED, LAYOUT_TOP_ONLY, LAYOUT_COUNT };
enum { CLOCK_STOCK, CLOCK_BOOST, CLOCK_COUNT };
// the 3DS buttons a Switch button can be given to (bit order of msr_buttons)
enum { B3_A, B3_B, B3_SELECT, B3_START, B3_R = 8, B3_L, B3_X, B3_Y };
#define NBIND 8

typedef struct {
    int layout;
    int clock;
    u64 bind[NBIND];   // Switch button (HidNpadButton_*) for A, B, X, Y, L, R, Start, Select
} PortSettings;

extern PortSettings g_set;
extern int g_menu_open;

void port_menu_init(const char* settings_path);
// Feeds the Switch buttons (held) each frame. Returns the 3DS button bits for the game (0 while
// the menu is open), handling the hold-minus gesture.
u32 port_menu_input(u64 held);
// The menu panel, PANEL_W x PANEL_H RGBA, when it changed since the last call (else 0).
#define PANEL_W 800
#define PANEL_H 480
const u32* port_menu_panel(void);
// Where the screens go on the 1280x720 output for the current layout (w or h 0: not shown).
typedef struct { int x, y, w, h; } Rect;
void port_menu_layout(Rect* top, Rect* bottom);
