// MSR-nx: the port menu. Hold minus (half a second) to open it; a short tap of minus still reaches
// the game as Select. Up/down picks a row, left/right changes it, A confirms, B or minus closes.
// Button mapping: pick a 3DS button, press A, then the Switch button for it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "port_menu.h"
#include "msr_log.h"

void dkr_set_sync_mode(int m);   // gpu/dk/dk_pica.c

PortSettings g_set;
int g_menu_open;
int g_map_view;
static char g_path[256];
static int g_row, g_wait_bind = -1, g_dirty = 1;
static u32 g_panel[PANEL_W * PANEL_H];

extern volatile int sched_pause_request;   // os/sched.c holds the game at its next switch while set

static const char* k_layout_names[LAYOUT_COUNT] = {"Top large, bottom small", "Side by side", "Stacked", "Single screen"};
static const char* k_clock_names[CLOCK_COUNT] = {"Stock (1020 MHz)", "Boost (1785 MHz)"};
static const char* k_bind_names[NBIND] = {"3DS A", "3DS B", "3DS X", "3DS Y", "3DS L", "3DS R", "3DS Start", "3DS Select"};
static const int k_bind_bits[NBIND] = {B3_A, B3_B, B3_X, B3_Y, B3_L, B3_R, B3_START, B3_SELECT};
static const u64 k_bind_default[NBIND] = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
                                          HidNpadButton_L, HidNpadButton_R, HidNpadButton_Plus, HidNpadButton_Minus};
static const struct { u64 b; const char* name; } k_sw[] = {
    {HidNpadButton_A, "A"}, {HidNpadButton_B, "B"}, {HidNpadButton_X, "X"}, {HidNpadButton_Y, "Y"},
    {HidNpadButton_L, "L"}, {HidNpadButton_R, "R"}, {HidNpadButton_ZL, "ZL"}, {HidNpadButton_ZR, "ZR"},
    {HidNpadButton_Plus, "Plus"}, {HidNpadButton_Minus, "Minus (tap)"}, {HidNpadButton_StickL, "Left stick click"},
    {HidNpadButton_StickR, "Right stick click"},
};
#define NSW (sizeof k_sw / sizeof k_sw[0])

static const char* sw_name(u64 b) {
    for (unsigned k = 0; k < NSW; k++) if (k_sw[k].b == b) return k_sw[k].name;
    return "(none)";
}

// rows: 0 layout, 1 clock, 2..9 bindings, 10 reset bindings, 11 close
enum { ROW_LAYOUT, ROW_CLOCK, ROW_FPS, ROW_SYNC, ROW_BIND0, ROW_RESET = ROW_BIND0 + NBIND, ROW_CLOSE, NROWS };

// ---- clocks -----------------------------------------------------------------------------------------
static int g_clk_ok = -1;
static ClkrstSession g_cpu;
static void clock_apply(void) {
    if (g_clk_ok < 0) {
        g_clk_ok = hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize()) &&
                   R_SUCCEEDED(clkrstOpenSession(&g_cpu, PcvModuleId_CpuBus, 3));
    }
    if (!g_clk_ok) return;
    u32 hz = g_set.clock == CLOCK_BOOST ? 1785000000u : 1020000000u;
    clkrstSetClockRate(&g_cpu, hz);
    u32 now = 0;
    clkrstGetClockRate(&g_cpu, &now);
    msr_receipt("menu: CPU clock set to %u MHz (reads back %u MHz)", hz / 1000000, now / 1000000);
}

// ---- settings file ----------------------------------------------------------------------------------
static void save(void) {
    // written in place: the SD card cannot rename over an existing file (learned on MPH-nx)
    FILE* f = fopen(g_path, "w");
    if (!f) return;
    fprintf(f, "layout=%d\nclock=%d\nfps=%d\nsync=%d\n", g_set.layout, g_set.clock, g_set.show_fps, g_set.sync);
    for (int k = 0; k < NBIND; k++) fprintf(f, "bind%d=%llu\n", k, (unsigned long long)g_set.bind[k]);
    fclose(f);
}

void port_menu_init(const char* settings_path) {
    snprintf(g_path, sizeof g_path, "%s", settings_path);
    g_set.layout = LAYOUT_BIG_SMALL;
    g_set.clock = CLOCK_STOCK;
    memcpy(g_set.bind, k_bind_default, sizeof g_set.bind);
    FILE* f = fopen(g_path, "r");
    int found = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            int v, k;
            unsigned long long b;
            if (sscanf(line, "layout=%d", &v) == 1 && v >= 0 && v < LAYOUT_COUNT) g_set.layout = v;
            else if (sscanf(line, "clock=%d", &v) == 1 && v >= 0 && v < CLOCK_COUNT) g_set.clock = v;
            else if (sscanf(line, "fps=%d", &v) == 1) g_set.show_fps = v != 0;
            else if (sscanf(line, "sync=%d", &v) == 1 && v >= 0 && v < 3) g_set.sync = v;
            else if (sscanf(line, "bind%d=%llu", &k, &b) == 2 && k >= 0 && k < NBIND) g_set.bind[k] = b;
        }
        fclose(f);
        found = 1;
    }
    dkr_set_sync_mode(g_set.sync);
    msr_receipt("menu: settings %s: layout %s, CPU clock %s", found ? "loaded" : "default", k_layout_names[g_set.layout],
                k_clock_names[g_set.clock]);
    if (g_set.clock != CLOCK_STOCK) clock_apply();
}

// ---- drawing ----------------------------------------------------------------------------------------
static void fill(int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h; j++)
        if (j >= 0 && j < PANEL_H)
            for (int i = x; i < x + w; i++)
                if (i >= 0 && i < PANEL_W) g_panel[j * PANEL_W + i] = c;
}

// libnx's console font: 256 glyphs of 16x16, one little-endian u16 per row, bit 15 the left pixel
// (test 11 read bit 0 as the left one and drew every letter mirrored).
static void text_to(u32* buf, int bw, int bh, int x, int y, const char* s, u32 c) {
    const u16* font = (const u16*)consoleGetDefault()->font.gfx;
    for (; *s; s++, x += 16) {
        const u16* g = font + 16 * (u8)*s;
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 16; b++)
                if ((g[r] >> (15 - b)) & 1) {
                    int px = x + b, py = y + r;
                    if (px >= 0 && px < bw && py >= 0 && py < bh) buf[py * bw + px] = c;
                }
    }
}
static void text(int x, int y, const char* s, u32 c) { text_to(g_panel, PANEL_W, PANEL_H, x, y, s, c); }

#define RGBA(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
static void draw(void) {
    fill(0, 0, PANEL_W, PANEL_H, RGBA(16, 20, 32));
    fill(0, 0, PANEL_W, 4, RGBA(230, 120, 30));
    fill(0, PANEL_H - 4, PANEL_W, 4, RGBA(230, 120, 30));
    text(24, 16, "MSR-nx port menu", RGBA(255, 200, 120));
    int y = 56;
    for (int r = 0; r < NROWS; r++, y += 26) {
        if (r == ROW_BIND0 || r == ROW_RESET) y += 8;
        int sel = r == g_row;
        if (sel) fill(12, y - 4, PANEL_W - 24, 24, RGBA(50, 70, 110));
        u32 c = sel ? RGBA(255, 255, 255) : RGBA(190, 200, 215);
        char v[64] = "";
        const char* label = "";
        if (r == ROW_LAYOUT) { label = "Screen layout"; snprintf(v, sizeof v, "< %s >", k_layout_names[g_set.layout]); }
        else if (r == ROW_CLOCK) { label = "CPU clock"; snprintf(v, sizeof v, "< %s >", k_clock_names[g_set.clock]); }
        else if (r == ROW_SYNC) { static const char* sn[3] = {"Safe", "Fewer waits", "Lean"}; label = "GPU sync (test)"; snprintf(v, sizeof v, "< %s >", sn[g_set.sync]); }
        else if (r == ROW_FPS) { label = "Frame counter"; snprintf(v, sizeof v, "< %s >", g_set.show_fps ? "On" : "Off"); }
        else if (r < ROW_RESET) {
            int k = r - ROW_BIND0;
            label = k_bind_names[k];
            if (g_wait_bind == k) snprintf(v, sizeof v, "press a Switch button...");
            else snprintf(v, sizeof v, "%s", sw_name(g_set.bind[k]));
        } else if (r == ROW_RESET) label = "Reset button mapping";
        else label = "Back to the game";
        text(24, y, label, c);
        text(PANEL_W - 24 - 16 * (int)strlen(v), y, v, sel ? RGBA(255, 210, 120) : c);
    }
    text(24, PANEL_H - 30, "Up/Down: choose  Left/Right: change  A: set  B: back", RGBA(140, 150, 170));
}

const u32* port_menu_panel(void) {
    if (!g_dirty) return 0;
    g_dirty = 0;
    draw();
    return g_panel;
}

// ---- input --------------------------------------------------------------------------------------------
static void open_menu(int open) {
    g_menu_open = open;
    sched_pause_request = open;
    g_wait_bind = -1;
    g_dirty = 1;
    msr_receipt("menu: %s", open ? "opened (game paused)" : "closed");
    if (!open) save();
}

u32 port_menu_input(u64 held) {
    static u64 prev;
    static unsigned long long minus_since, select_until;
    static int minus_used;
    u64 down = held & ~prev;
    prev = held;
    unsigned long long now = armTicksToNs(armGetSystemTick());

    if (g_menu_open) {
        if (g_wait_bind >= 0) {
            for (unsigned k = 0; k < NSW; k++)
                if (down & k_sw[k].b) {
                    g_set.bind[g_wait_bind] = k_sw[k].b;
                    g_wait_bind = -1;
                    g_dirty = 1;
                    break;
                }
            return 0;
        }
        int dx = !!(down & (HidNpadButton_Right | HidNpadButton_StickLRight)) - !!(down & (HidNpadButton_Left | HidNpadButton_StickLLeft));
        if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) { g_row = (g_row + 1) % NROWS; g_dirty = 1; }
        if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) { g_row = (g_row + NROWS - 1) % NROWS; g_dirty = 1; }
        if (dx && g_row == ROW_LAYOUT) { g_set.layout = (g_set.layout + LAYOUT_COUNT + dx) % LAYOUT_COUNT; g_dirty = 1; }
        if (dx && g_row == ROW_SYNC) { g_set.sync = (g_set.sync + 3 + dx) % 3; dkr_set_sync_mode(g_set.sync); g_dirty = 1; }
        if ((dx || (down & HidNpadButton_A)) && g_row == ROW_FPS) { g_set.show_fps = !g_set.show_fps; g_dirty = 1; }
        if (dx && g_row == ROW_CLOCK) { g_set.clock = (g_set.clock + CLOCK_COUNT + dx) % CLOCK_COUNT; clock_apply(); g_dirty = 1; }
        if (down & HidNpadButton_A) {
            if (g_row >= ROW_BIND0 && g_row < ROW_RESET) { g_wait_bind = g_row - ROW_BIND0; g_dirty = 1; }
            else if (g_row == ROW_RESET) { memcpy(g_set.bind, k_bind_default, sizeof g_set.bind); g_dirty = 1; }
            else if (g_row == ROW_CLOSE) open_menu(0);
        } else if (down & (HidNpadButton_B | HidNpadButton_Minus)) open_menu(0);
        if (!g_menu_open) minus_used = 1;   // the minus that closed it is not a tap
        return 0;
    }

    // hold minus half a second: the menu; a shorter tap reaches the game as Select (when Select is
    // on minus). ZL + ZR + minus is the frame capture, not the menu.
    int minus = (held & HidNpadButton_Minus) != 0;
    int capture_combo = (held & HidNpadButton_ZL) && (held & HidNpadButton_ZR);
    if (minus) {
        if (!minus_since) { minus_since = now; if (!(down & HidNpadButton_Minus)) minus_used = 1; }
        if (!minus_used && !capture_combo && now - minus_since >= 500000000ull) {
            minus_used = 1;
            open_menu(1);
            return 0;
        }
    } else {
        if (minus_since && !minus_used && now - minus_since < 500000000ull) {
            // single-screen mode: the tap shows or hides the bottom screen (map); otherwise Select
            if (g_set.layout == LAYOUT_TOP_ONLY) g_map_view = !g_map_view;
            else select_until = now + 100000000ull;
        }
        minus_since = 0;
        minus_used = 0;
    }
    u32 b = 0;
    for (int k = 0; k < NBIND; k++) {
        u64 sb = g_set.bind[k];
        if (sb == HidNpadButton_Minus) { if (now < select_until) b |= 1u << k_bind_bits[k]; }
        else if (held & sb) b |= 1u << k_bind_bits[k];
    }
    if (held & HidNpadButton_Right) b |= 1 << 4;
    if (held & HidNpadButton_Left) b |= 1 << 5;
    if (held & HidNpadButton_Up) b |= 1 << 6;
    if (held & HidNpadButton_Down) b |= 1 << 7;
    return b;
}

// ---- layouts -------------------------------------------------------------------------------------------
void port_menu_layout(Rect* t, Rect* b) {
    switch (g_set.layout) {
    default:
    case LAYOUT_BIG_SMALL: *t = (Rect){0, 72, 960, 576}; *b = (Rect){960, 240, 320, 240}; break;   // 2.4x and 1x
    case LAYOUT_SIDE: *t = (Rect){0, 147, 711, 427}; *b = (Rect){711, 147, 569, 427}; break;       // both 1.78x
    case LAYOUT_STACKED: *t = (Rect){340, 0, 600, 360}; *b = (Rect){400, 360, 480, 360}; break;    // both 1.5x
    case LAYOUT_TOP_ONLY:   // 3x, the top screen or (map view) the bottom one
        if (g_map_view) { *t = (Rect){0, 0, 0, 0}; *b = (Rect){160, 0, 960, 720}; }
        else { *t = (Rect){40, 0, 1200, 720}; *b = (Rect){0, 0, 0, 0}; }
        break;
    }
}

// ---- frame counter -------------------------------------------------------------------------------------
static u32 g_fps_px[FPS_W * FPS_H];
const u32* port_menu_fps(int fps10) {
    static int last = -1;
    if (fps10 == last) return 0;
    last = fps10;
    for (int i = 0; i < FPS_W * FPS_H; i++) g_fps_px[i] = RGBA(0, 0, 0);
    char t[32];
    snprintf(t, sizeof t, "%2d.%d fps", fps10 / 10, fps10 % 10);
    text_to(g_fps_px, FPS_W, FPS_H, 4, 4, t, fps10 >= 290 ? RGBA(120, 255, 140) : fps10 >= 200 ? RGBA(255, 220, 90) : RGBA(255, 110, 90));
    return g_fps_px;
}
