/*
 * fps_core.h - Portable core for the Xbox 360 FPS counter.
 *
 * No XDK or platform headers. The same code is compiled into the
 * 360 title DLL and into the PC-side tests and preview tool, so what
 * the tests check is what runs on the console.
 *
 * Written in C89-friendly style so Visual Studio 2010 (the XDK's
 * compiler) accepts it as plain C.
 */
#ifndef FPS_CORE_H
#define FPS_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long fc_u64;

/* Button bits, identical to XINPUT_GAMEPAD wButtons on the 360. */
#define FC_BTN_DPAD_UP     0x0001
#define FC_BTN_DPAD_DOWN   0x0002
#define FC_BTN_DPAD_LEFT   0x0004
#define FC_BTN_DPAD_RIGHT  0x0008
#define FC_BTN_START       0x0010
#define FC_BTN_BACK        0x0020
#define FC_BTN_LTHUMB      0x0040
#define FC_BTN_RTHUMB      0x0080
#define FC_BTN_LB          0x0100
#define FC_BTN_RB          0x0200
#define FC_BTN_A           0x1000
#define FC_BTN_B           0x2000
#define FC_BTN_X           0x4000
#define FC_BTN_Y           0x8000

/* ------------------------------------------------------------------ */
/* Frame counter                                                       */
/* ------------------------------------------------------------------ */

typedef struct FcCounter {
    fc_u64   freq;        /* timer ticks per second                    */
    fc_u64   window;      /* ticks per measurement window              */
    fc_u64   start;       /* tick at which the current window started  */
    unsigned frames;      /* frames presented in the current window    */
    unsigned fps_x10;     /* last measured FPS times 10 (one decimal)  */
    int      started;
} FcCounter;

void     fc_counter_init(FcCounter *c, fc_u64 ticks_per_sec, unsigned window_ms);
/* Call once per presented frame. Returns 1 when a new reading is ready. */
int      fc_counter_frame(FcCounter *c, fc_u64 now);
/* Last reading rounded to a whole number. */
unsigned fc_counter_fps(const FcCounter *c);

/* ------------------------------------------------------------------ */
/* Realtime toggle (held button combo)                                 */
/* ------------------------------------------------------------------ */

typedef struct FcToggle {
    unsigned short combo;      /* all of these buttons must be held    */
    fc_u64         hold_ticks; /* how long they must be held           */
    fc_u64         hold_start;
    int            holding;    /* combo currently held, timer running  */
    int            latched;    /* fired; waiting for a release         */
    int            visible;    /* current on/off state                 */
} FcToggle;

void fc_toggle_init(FcToggle *t, unsigned short combo, unsigned hold_ms,
                    fc_u64 ticks_per_sec, int start_visible);
/* Call once per frame with the pad's button bits. Returns 1 when the
 * visible state flips. */
int  fc_toggle_update(FcToggle *t, unsigned short buttons, fc_u64 now);

/* ------------------------------------------------------------------ */
/* Config (fps.ini)                                                    */
/* ------------------------------------------------------------------ */

typedef struct FcConfig {
    int            enabled;      /* start visible? 0 = off until combo  */
    int            x, y;         /* top-left of the box, 1280x720 space */
    int            scale;        /* pixels per font pixel               */
    int            opacity_pct;  /* backing box opacity, 0..100         */
    unsigned short combo;        /* toggle buttons                      */
    int            hold_ms;      /* toggle hold time                    */
    unsigned       text_rgb;     /* 0xRRGGBB                            */
    /* Extras included in the combo's display cycle (see fc_mode_config). */
    int            show_stats;   /* v2: second line "MIN n AVG n"       */
    int            show_graph;   /* v3: frametime graph                 */
    int            show_temps;   /* CPU/GPU/RAM/board temperatures      */
} FcConfig;

void fc_config_defaults(FcConfig *cfg);
/* Parses ini text (NUL-terminated). Unknown keys and bad values are
 * ignored; numbers are clamped to safe ranges. Returns how many
 * settings were applied. */
int  fc_config_parse(FcConfig *cfg, const char *text);
/* Parses "LB+RB+DOWN" style text. Returns 0 if any name is unknown. */
int  fc_parse_combo(const char *text, unsigned short *out);

/* Display modes, stepped through by the combo: 0 = off, 1 = FPS only, then
 * each extra the config includes (stats, graph, temps, in that order) is
 * added one at a time, then back to off. */
int  fc_mode_count(const FcConfig *cfg);
/* The config to draw for `mode`: a copy of cfg with only that mode's
 * extras switched on. */
void fc_mode_config(const FcConfig *cfg, int mode, FcConfig *out);

/* ------------------------------------------------------------------ */
/* Overlay geometry                                                    */
/* ------------------------------------------------------------------ */

#define FC_GLYPH_W 5
#define FC_GLYPH_H 7

/* Writes "60 FPS" style text into buf (at least 16 bytes). */
void fc_format(unsigned fps, char *buf);

/* The overlay is emitted as a list of solid rectangles, in draw order:
 * the backing box, then a dark shadow under every text pixel (keeps the
 * text readable when a bright scene shows through the 50% box), then the
 * text pixels merged into horizontal runs. The 360 renderer turns each
 * into a quad; the PC preview fills them into an image. Same geometry
 * either way. */
enum { FC_RECT_BOX = 0, FC_RECT_SHADOW = 1, FC_RECT_TEXT = 2 };
typedef void (*FcRectFn)(int x, int y, int w, int h, int kind, void *user);

/* Returns the number of rectangles emitted. */
int  fc_emit_overlay(const FcConfig *cfg, const char *text, FcRectFn fn, void *user);
void fc_overlay_size(const FcConfig *cfg, const char *text, int *w, int *h);

/* ------------------------------------------------------------------ */
/* Rolling min/avg (v2)                                                */
/* ------------------------------------------------------------------ */

#define FC_STATS_N 20   /* readings kept: 20 x 0.5 s = the last 10 seconds */

typedef struct FcStats {
    unsigned short v[FC_STATS_N];
    int            count;   /* readings stored, up to FC_STATS_N */
    int            head;    /* next slot to write                */
} FcStats;

void     fc_stats_init(FcStats *s);
void     fc_stats_add(FcStats *s, unsigned fps);
unsigned fc_stats_min(const FcStats *s);   /* 0 if empty */
unsigned fc_stats_avg(const FcStats *s);   /* rounded, 0 if empty */

/* ------------------------------------------------------------------ */
/* Panel: everything the overlay shows, as one pixel image             */
/* ------------------------------------------------------------------ */

/* The 360 draws by having the GPU overwrite single pixels, so it cannot
 * blend. The panel is rendered as one byte per pixel saying what the
 * pixel is; the console turns each kind into a color (or leaves the
 * game's pixel alone for FC_PX_SKIP). The backing box opacity becomes an
 * ordered dither: at 50%, every other pixel is black. */
enum { FC_PX_SKIP = 0, FC_PX_BG, FC_PX_SHADOW, FC_PX_TEXT, FC_PX_GRAPH, FC_PX_GRID };

#define FC_GRAPH_MS 50  /* graph top = 50 ms frame time (20 FPS) */

typedef struct FcPanelData {
    unsigned              fps;
    unsigned              fps_min, fps_avg;  /* show_stats                         */
    int                   temps_valid;       /* show_temps: 0 shows "--"           */
    int                   temp_c[4];         /* CPU, GPU, RAM, board, in degrees C */
    const unsigned short *ft_x10;            /* show_graph: frame times in 0.1 ms, */
    int                   ft_count;          /* oldest first                       */
} FcPanelData;

/* Panel size depends only on the config (fields are sized for their widest
 * value), so the console can size its buffers once. */
void fc_panel_size(const FcConfig *cfg, int *w, int *h);
/* Fills px (w*h bytes, row-major) with FC_PX_* values. */
void fc_panel_render(const FcConfig *cfg, const FcPanelData *d,
                     unsigned char *px, int w, int h);

/* SMC 0x07 reply (16 bytes) -> CPU, GPU, RAM, board in whole degrees C. */
void fc_decode_temps(const unsigned char *reply, int out_c[4]);

/* ------------------------------------------------------------------ */
/* V-Sync probe: PowerPC pattern matching (read-only research)         */
/* ------------------------------------------------------------------ */

/* Instruction words are passed as numbers (as the CPU reads them). */
typedef unsigned int fc_u32;

/* Checks for D3D's SynchronizeToPresentationInterval at w[0]:
 *     lwz  rT, X(rB) ; cmplwi rT,0 ; bc ; cmplwi rT,1 ; bc ; cmplwi rT,2
 * then, within the next words, "lwz rS, Y(rB)" followed by
 * "rlwinm rA, rS, 9, 25, 31" (the 7-bit immediate threshold).
 * On a match returns 1 and sets X (PresentInterval offset in the device)
 * and Y (offset of the word holding PresentImmediateThreshold). */
int fc_match_present_fields(const fc_u32 *w, int avail, fc_u32 *interval_off, fc_u32 *threshold_off);

/* Follows lis/addi/ori/mr through the first n words of a function and
 * finds where it stores its first two arguments (r3, r4) with stw.
 * Returns a bit mask: 1 = r3 store found, 2 = r4 store found. */
int fc_find_arg_stores(const fc_u32 *w, int n, fc_u32 *r3_addr, fc_u32 *r4_addr);

#ifdef __cplusplus
}
#endif

#endif /* FPS_CORE_H */
