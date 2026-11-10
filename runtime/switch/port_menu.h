// MSR-nx: the port menu (hold minus): screen layout, CPU clock and button mapping, saved to
// sdmc:/switch/msr/settings.ini. While it is open the game is paused.
#pragma once
#include <switch.h>

enum { LAYOUT_BIG_SMALL, LAYOUT_SIDE, LAYOUT_STACKED, LAYOUT_TOP_ONLY, LAYOUT_COUNT };   // TOP_ONLY: single-screen mode
enum { CLOCK_STOCK, CLOCK_BOOST, CLOCK_COUNT };
#define GPU_COUNT 4
// the 3DS buttons a Switch button can be given to (bit order of msr_buttons)
enum { B3_A, B3_B, B3_SELECT, B3_START, B3_R = 8, B3_L, B3_X, B3_Y };
#define NBIND 9   // the 3DS buttons, then Next beam

typedef struct {
    int layout;
    int clock;
    int gpu;           // GPU clock choice (port_menu.c k_gpu_hz), 0 the system's own
    int show_fps;      // the frame counter
    int sync;          // GPU sync: 0 Safe, 1 Fewer waits, 2 Lean (dk_pica.c gpu_barrier)
    int scale;         // resolution scaler for the 3D top screen: 0 1x, 1 1.5x, 2 2x (dk_pica.c g_scale)
    int gxthread;      // the graphics thread: 0 on (os/hle_hid_gsp.c msr_gx_async), 1 off
    int fps60;         // frame rate: 0 the 3DS's 30, 1 60 (os/sched.c msr_frame_limit_ms)
    int copies;        // GX copies and transfers: 0 from render target images, 1 through VRAM (dk_pica.c img_source)
    u64 bind[NBIND];   // Switch button (HidNpadButton_*) for A, B, X, Y, L, R, Start, Select, Next beam
} PortSettings;

extern PortSettings g_set;
extern int g_menu_open;
extern int g_map_view;      // single-screen mode: the game's pause menu frame is on the bottom screen (main_nx.c), so that is shown
extern int g_auto_bottom;   // single-screen mode: the bottom screen shown by itself (the file select screen is up)   // single-screen mode (top screen only): the bottom screen shown instead, a minus tap toggles it

void port_menu_init(const char* settings_path);
// Feeds the Switch buttons (held) each frame. Returns the 3DS button bits for the game (0 while
// the menu is open), handling the hold-minus gesture.
u32 port_menu_input(u64 held);
// The menu panel, PANEL_W x PANEL_H RGBA, when it changed since the last call (else 0).
#define PANEL_W 800
#define PANEL_H 620
const u32* port_menu_panel(void);
// Where the screens go on the 1280x720 output for the current layout (w or h 0: not shown).
typedef struct { int x, y, w, h; } Rect;
void port_menu_layout(Rect* top, Rect* bottom);
// The frame counter, FPS_W x FPS_H RGBA, redrawn when the value changes (else 0). fps10 is tenths.
#define FPS_W 128
#define FPS_H 24
const u32* port_menu_fps(int fps10);
int port_menu_beam_request(void);       // Next beam was pressed since the last call (main_nx.c taps the touch screen)
void port_menu_clocks_refresh(void);   // every few seconds: the chosen clocks again if the system changed them
void port_menu_clocks_restore(void);   // at exit: the system's clocks
