/*
 * test_core.c - Unit tests for the portable core.
 * Build and run: make test   (or: gcc -I../core test_core.c ../core/fps_core.c)
 */
#include "fps_core.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failed++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    g_checks++; \
    if (_a != _b) { g_failed++; printf("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", \
        __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

/* The 360's timebase runs at 50 MHz; use the same rate here. */
#define FREQ 50000000ULL

/* Feeds `frames` frames spaced `interval` ticks apart; returns last reading. */
static unsigned feed(FcCounter *c, fc_u64 *now, int frames, fc_u64 interval)
{
    int i;
    for (i = 0; i < frames; i++) {
        *now += interval;
        fc_counter_frame(c, *now);
    }
    return fc_counter_fps(c);
}

static void test_counter(void)
{
    FcCounter c;
    fc_u64 now = 123456789ULL; /* arbitrary non-zero start */

    printf("counter\n");

    fc_counter_init(&c, FREQ, 500);
    CHECK_EQ(fc_counter_fps(&c), 0);          /* nothing shown before first window */
    CHECK_EQ(feed(&c, &now, 121, FREQ / 60), 60);

    fc_counter_init(&c, FREQ, 500);
    CHECK_EQ(feed(&c, &now, 61, FREQ / 30), 30);

    /* A game running at 60 that drops to 20 should read 20 within one window. */
    fc_counter_init(&c, FREQ, 500);
    feed(&c, &now, 121, FREQ / 60);
    CHECK_EQ(feed(&c, &now, 21, FREQ / 20), 20);

    /* Uneven pacing (alternating 16 ms and 50 ms frames) averages out: ~30 FPS. */
    {
        int i;
        fc_counter_init(&c, FREQ, 500);
        for (i = 0; i < 60; i++) {
            now += (i % 2) ? FREQ / 20 : (FREQ * 16) / 1000;
            fc_counter_frame(&c, now);
        }
        CHECK(fc_counter_fps(&c) >= 29 && fc_counter_fps(&c) <= 31);
    }

    /* A long stall (3 s loading screen) reports a low number, not garbage. */
    fc_counter_init(&c, FREQ, 500);
    feed(&c, &now, 31, FREQ / 60);
    now += FREQ * 3;
    fc_counter_frame(&c, now);
    CHECK(fc_counter_fps(&c) <= 11);

    /* The reading only changes when a window closes (twice a second). */
    {
        int updates = 0, i;
        fc_counter_init(&c, FREQ, 500);
        for (i = 0; i < 300; i++) { /* 5 s at 60 FPS */
            now += FREQ / 60;
            updates += fc_counter_frame(&c, now);
        }
        CHECK(updates >= 9 && updates <= 10);
    }
}

static void test_toggle(void)
{
    FcToggle t;
    const unsigned short combo = FC_BTN_LB | FC_BTN_RB | FC_BTN_DPAD_DOWN;
    fc_u64 now = 1000;
    int flips = 0, i;

    printf("toggle\n");
    fc_toggle_init(&t, combo, 500, FREQ, 0);
    CHECK_EQ(t.visible, 0); /* off until enabled */

    /* Two of the three buttons held for 2 s: nothing. */
    for (i = 0; i < 120; i++) {
        now += FREQ / 60;
        flips += fc_toggle_update(&t, FC_BTN_LB | FC_BTN_RB, now);
    }
    CHECK_EQ(flips, 0);

    /* Full combo for 0.3 s, then released: nothing (a quick in-game press). */
    for (i = 0; i < 18; i++) {
        now += FREQ / 60;
        flips += fc_toggle_update(&t, combo, now);
    }
    now += FREQ / 60;
    flips += fc_toggle_update(&t, 0, now);
    CHECK_EQ(flips, 0);
    CHECK_EQ(t.visible, 0);

    /* Full combo held 2 s: flips once, at the 0.5 s mark, and stays on. */
    {
        int flip_frame = -1;
        for (i = 0; i < 120; i++) {
            now += FREQ / 60;
            if (fc_toggle_update(&t, combo, now)) {
                flips++;
                if (flip_frame < 0) flip_frame = i;
            }
        }
        CHECK_EQ(flips, 1);
        CHECK_EQ(t.visible, 1);
        /* Timer starts on the first held frame (i = 0); 0.5 s later is
         * frame 30, or 31 when 1/60 s doesn't divide the tick rate evenly. */
        CHECK(flip_frame >= 30 && flip_frame <= 31);
    }

    /* Extra buttons (A pressed too) still count as the combo. Release, then
     * hold again with A: turns off. */
    now += FREQ / 60;
    fc_toggle_update(&t, 0, now);
    flips = 0;
    for (i = 0; i < 40; i++) {
        now += FREQ / 60;
        flips += fc_toggle_update(&t, combo | FC_BTN_A, now);
    }
    CHECK_EQ(flips, 1);
    CHECK_EQ(t.visible, 0);

    /* Works the same at 20 FPS: timing is by clock, not frame count. */
    now += FREQ / 20;
    fc_toggle_update(&t, 0, now);
    flips = 0;
    for (i = 0; i < 10; i++) { /* first held frame + 9 more = 0.45 s held */
        now += FREQ / 20;
        flips += fc_toggle_update(&t, combo, now);
    }
    CHECK_EQ(flips, 0);
    now += FREQ / 20;
    flips += fc_toggle_update(&t, combo, now); /* 0.5 s held */
    CHECK_EQ(flips, 1);
}

static void test_config(void)
{
    FcConfig cfg;
    unsigned short combo = 0;
    int n;

    printf("config\n");
    fc_config_defaults(&cfg);
    CHECK_EQ(cfg.enabled, 0);
    CHECK_EQ(cfg.opacity_pct, 50);
    CHECK_EQ(cfg.combo, FC_BTN_LB | FC_BTN_RB | FC_BTN_DPAD_DOWN);
    CHECK_EQ(cfg.hold_ms, 500);
    CHECK(cfg.x >= 64 && cfg.y >= 36); /* inside the TV-safe margin */

    CHECK(fc_parse_combo("LB+RB+DOWN", &combo));
    CHECK_EQ(combo, FC_BTN_LB | FC_BTN_RB | FC_BTN_DPAD_DOWN);
    CHECK(fc_parse_combo(" back + rs ", &combo));
    CHECK_EQ(combo, FC_BTN_BACK | FC_BTN_RTHUMB);
    combo = 0;
    CHECK(!fc_parse_combo("LB+TURBO", &combo));
    CHECK_EQ(combo, 0);
    CHECK(!fc_parse_combo("", &combo));

    fc_config_defaults(&cfg);
    n = fc_config_parse(&cfg,
        "; FPS counter settings\r\n"
        "[overlay]\r\n"
        "enabled = true\r\n"
        "x=100\n"
        "  y = 50   ; trailing comment\n"
        "scale=2\n"
        "opacity=75\n"
        "combo = BACK+RS\n"
        "hold_ms=750\n"
        "text_color = #FFFF00\n");
    CHECK_EQ(n, 8);
    CHECK_EQ(cfg.enabled, 1);
    CHECK_EQ(cfg.x, 100);
    CHECK_EQ(cfg.y, 50);
    CHECK_EQ(cfg.scale, 2);
    CHECK_EQ(cfg.opacity_pct, 75);
    CHECK_EQ(cfg.combo, FC_BTN_BACK | FC_BTN_RTHUMB);
    CHECK_EQ(cfg.hold_ms, 750);
    CHECK_EQ(cfg.text_rgb, 0xFFFF00);

    /* Bad values never break the overlay: clamped or ignored. */
    fc_config_defaults(&cfg);
    n = fc_config_parse(&cfg,
        "opacity=250\n"
        "scale=0\n"
        "x=99999\n"
        "hold_ms=abc\n"
        "combo=LB+NOPE\n"
        "enabled=maybe\n"
        "mystery=1\n"
        "no equals sign here\n");
    CHECK_EQ(n, 3);
    CHECK_EQ(cfg.opacity_pct, 100);
    CHECK_EQ(cfg.scale, 1);
    CHECK_EQ(cfg.x, 1200);
    CHECK_EQ(cfg.hold_ms, 500);
    CHECK_EQ(cfg.combo, FC_BTN_LB | FC_BTN_RB | FC_BTN_DPAD_DOWN);
    CHECK_EQ(cfg.enabled, 0);

    CHECK_EQ(fc_config_parse(&cfg, ""), 0);
}

typedef struct Bounds {
    int boxes, runs, shadows, order_ok, last_kind;
    int minx, miny, maxx, maxy; /* of text runs */
    int sminx, sminy, smaxx, smaxy; /* of shadow runs */
    int bx, by, bw, bh;
} Bounds;

static void collect(int x, int y, int w, int h, int kind, void *user)
{
    Bounds *b = (Bounds *)user;
    if (kind < b->last_kind)
        b->order_ok = 0; /* must draw box, then shadow, then text */
    b->last_kind = kind;
    if (kind == FC_RECT_BOX) {
        b->boxes++;
        b->bx = x; b->by = y; b->bw = w; b->bh = h;
        return;
    }
    if (kind == FC_RECT_SHADOW) {
        b->shadows++;
        if (x < b->sminx) b->sminx = x;
        if (y < b->sminy) b->sminy = y;
        if (x + w > b->smaxx) b->smaxx = x + w;
        if (y + h > b->smaxy) b->smaxy = y + h;
        return;
    }
    b->runs++;
    if (x < b->minx) b->minx = x;
    if (y < b->miny) b->miny = y;
    if (x + w > b->maxx) b->maxx = x + w;
    if (y + h > b->maxy) b->maxy = y + h;
}

static void test_overlay(void)
{
    FcConfig cfg;
    Bounds b;
    char text[16];
    int w, h;

    printf("overlay\n");
    fc_format(60, text);
    CHECK(strcmp(text, "60 FPS") == 0);
    fc_format(5, text);
    CHECK(strcmp(text, "5 FPS") == 0);
    fc_format(4000, text);
    CHECK(strcmp(text, "999 FPS") == 0);

    fc_config_defaults(&cfg);
    fc_format(60, text);
    memset(&b, 0, sizeof(b));
    b.minx = b.miny = b.sminx = b.sminy = 1 << 30;
    b.order_ok = 1;
    fc_emit_overlay(&cfg, text, collect, &b);
    fc_overlay_size(&cfg, text, &w, &h);

    CHECK_EQ(b.boxes, 1);
    CHECK(b.runs > 0);
    CHECK_EQ(b.shadows, b.runs);   /* one shadow per text run */
    CHECK(b.order_ok);
    /* Shadow sits down-right of the text and stays inside the box. */
    CHECK(b.sminx > b.minx && b.sminy > b.miny);
    CHECK(b.smaxx <= b.bx + b.bw && b.smaxy <= b.by + b.bh);
    CHECK_EQ(b.bx, cfg.x);
    CHECK_EQ(b.by, cfg.y);
    CHECK_EQ(b.bw, w);
    CHECK_EQ(b.bh, h);
    /* All text sits inside the box with equal padding. */
    CHECK(b.minx >= b.bx && b.miny >= b.by);
    CHECK(b.maxx <= b.bx + b.bw && b.maxy <= b.by + b.bh);
    CHECK_EQ(b.minx - b.bx, (b.bx + b.bw) - b.maxx);
    CHECK_EQ(b.miny - b.by, (b.by + b.bh) - b.maxy);
    /* Widest reading still fits in the top-left quarter of a 720p frame. */
    fc_overlay_size(&cfg, "999 FPS", &w, &h);
    CHECK(cfg.x + w < 640 && cfg.y + h < 360);
}

static void test_config_v2(void)
{
    FcConfig cfg;

    printf("config v2\n");
    fc_config_defaults(&cfg);
    CHECK_EQ(cfg.show_stats, 1);  /* every extra is in the cycle by default */
    CHECK_EQ(cfg.show_graph, 1);
    CHECK_EQ(cfg.show_temps, 1);
    CHECK_EQ(fc_config_parse(&cfg, "show_stats=0\nshow_graph = off\nshow_temps=no\n"), 3);
    CHECK_EQ(cfg.show_stats, 0);
    CHECK_EQ(cfg.show_graph, 0);
    CHECK_EQ(cfg.show_temps, 0);
    CHECK_EQ(fc_config_parse(&cfg, "show_graph=maybe\nshow_temps=1\n"), 1);
    CHECK_EQ(cfg.show_graph, 0);
    CHECK_EQ(cfg.show_temps, 1);
}

static void test_modes(void)
{
    FcConfig cfg, m;

    printf("modes\n");
    fc_config_defaults(&cfg);
    /* Off, FPS, +stats, +graph, +temps */
    CHECK_EQ(fc_mode_count(&cfg), 5);
    fc_mode_config(&cfg, 0, &m);
    CHECK(!m.show_stats && !m.show_graph && !m.show_temps);
    fc_mode_config(&cfg, 1, &m);
    CHECK(!m.show_stats && !m.show_graph && !m.show_temps);
    fc_mode_config(&cfg, 2, &m);
    CHECK(m.show_stats && !m.show_graph && !m.show_temps);
    fc_mode_config(&cfg, 3, &m);
    CHECK(m.show_stats && m.show_graph && !m.show_temps);
    fc_mode_config(&cfg, 4, &m);
    CHECK(m.show_stats && m.show_graph && m.show_temps);
    CHECK_EQ(m.scale, cfg.scale);   /* everything else is copied */
    CHECK_EQ(m.x, cfg.x);

    /* Graph left out of the cycle: Off, FPS, +stats, +temps */
    cfg.show_graph = 0;
    CHECK_EQ(fc_mode_count(&cfg), 4);
    fc_mode_config(&cfg, 3, &m);
    CHECK(m.show_stats && !m.show_graph && m.show_temps);

    /* No extras: just Off and FPS */
    cfg.show_stats = cfg.show_temps = 0;
    CHECK_EQ(fc_mode_count(&cfg), 2);
}

static void test_stats(void)
{
    FcStats s;
    int i;

    printf("stats\n");
    fc_stats_init(&s);
    CHECK_EQ(fc_stats_min(&s), 0);
    CHECK_EQ(fc_stats_avg(&s), 0);
    fc_stats_add(&s, 30);
    fc_stats_add(&s, 20);
    fc_stats_add(&s, 31);
    CHECK_EQ(fc_stats_min(&s), 20);
    CHECK_EQ(fc_stats_avg(&s), 27);
    /* Old readings fall out after FC_STATS_N new ones (the last 10 s). */
    for (i = 0; i < FC_STATS_N; i++)
        fc_stats_add(&s, 60);
    CHECK_EQ(fc_stats_min(&s), 60);
    CHECK_EQ(fc_stats_avg(&s), 60);
    fc_stats_add(&s, 5000);           /* clamped to what the panel can show */
    CHECK_EQ(s.v[(s.head + FC_STATS_N - 1) % FC_STATS_N], 999);
}

static void test_temps(void)
{
    /* Example SMC reply from Free60: 07 ff 24 1b 2f a4 2c fa 26 2c */
    static const unsigned char reply[16] = { 0x07, 0xff, 0x24, 0x1b, 0x2f, 0xa4, 0x2c, 0xfa, 0x26 };
    int t[4];

    printf("temps\n");
    fc_decode_temps(reply, t);
    CHECK_EQ(t[0], 37);   /* CPU   0x24FF / 256 = 36.99 */
    CHECK_EQ(t[1], 47);   /* GPU   0x2F1B / 256 = 47.11 */
    CHECK_EQ(t[2], 45);   /* RAM   0x2CA4 / 256 = 44.64 */
    CHECK_EQ(t[3], 39);   /* board 0x26FA / 256 = 38.98 */
}

static int count_kind(const unsigned char *px, int n, int kind)
{
    int i, c = 0;
    for (i = 0; i < n; i++)
        c += px[i] == kind;
    return c;
}

static void test_panel(void)
{
    static unsigned char px[400 * 300];
    static unsigned short ft[300];
    FcConfig cfg;
    FcPanelData d;
    int w, h, w1, h1, i, x, y, bg, skip, hit;

    printf("panel\n");
    memset(&d, 0, sizeof(d));
    d.fps = 60;

    /* FPS only: fits the old single-line box. */
    fc_config_defaults(&cfg);
    cfg.show_stats = cfg.show_graph = cfg.show_temps = 0;
    fc_panel_size(&cfg, &w1, &h1);
    fc_overlay_size(&cfg, "999 FPS", &w, &h);
    CHECK_EQ(w1, w);
    CHECK_EQ(h1, h);

    /* Default 50% opacity: half the box is dithered black, half see-through. */
    fc_panel_render(&cfg, &d, px, w1, h1);
    bg   = count_kind(px, w1 * h1, FC_PX_BG);
    skip = count_kind(px, w1 * h1, FC_PX_SKIP);
    CHECK(count_kind(px, w1 * h1, FC_PX_TEXT) > 0);
    CHECK(count_kind(px, w1 * h1, FC_PX_SHADOW) > 0);
    CHECK(bg > 0 && skip > 0);
    CHECK(bg - skip < w1 && skip - bg < w1);  /* about equal */

    cfg.opacity_pct = 0;
    fc_panel_render(&cfg, &d, px, w1, h1);
    CHECK_EQ(count_kind(px, w1 * h1, FC_PX_BG), 0);
    cfg.opacity_pct = 100;
    fc_panel_render(&cfg, &d, px, w1, h1);
    CHECK_EQ(count_kind(px, w1 * h1, FC_PX_SKIP), 0);

    /* Everything on: taller, and still inside the top-left quarter at 720p. */
    fc_config_defaults(&cfg);
    cfg.show_stats = cfg.show_graph = cfg.show_temps = 1;
    fc_panel_size(&cfg, &w, &h);
    CHECK(h > h1 && w >= w1);
    CHECK(cfg.x + w < 640 && cfg.y + h < 360);
    CHECK(w <= 400 && h <= 300);

    /* Graph: a steady 33.3 ms (30 FPS) line of bars, newest on the right. */
    for (i = 0; i < 300; i++)
        ft[i] = 333;
    d.ft_x10 = ft;
    d.ft_count = 300;
    d.fps_min = 28;
    d.fps_avg = 30;
    d.temps_valid = 1;
    d.temp_c[0] = 62; d.temp_c[1] = 58; d.temp_c[2] = 50; d.temp_c[3] = 45;
    fc_panel_render(&cfg, &d, px, w, h);
    CHECK(count_kind(px, w * h, FC_PX_GRAPH) > 0);
    CHECK(count_kind(px, w * h, FC_PX_GRID) > 0);
    /* Every graph column has a bar (300 samples > graph width). */
    hit = 0;
    for (x = 0; x < w; x++)
        for (y = 0; y < h; y++)
            if (px[y * w + x] == FC_PX_GRAPH) { hit++; break; }
    CHECK_EQ(hit, w - 2 * cfg.scale * 2);
    /* Nothing is drawn in the outer padding except backing box. */
    for (x = 0; x < w; x++) {
        CHECK(px[x] == FC_PX_BG || px[x] == FC_PX_SKIP);
        CHECK(px[(h - 1) * w + x] == FC_PX_BG || px[(h - 1) * w + x] == FC_PX_SKIP);
    }
    /* No frame data yet: graph area has guides but no bars. */
    d.ft_x10 = NULL;
    d.ft_count = 0;
    fc_panel_render(&cfg, &d, px, w, h);
    CHECK_EQ(count_kind(px, w * h, FC_PX_GRAPH), 0);
}

/* D3D::SynchronizeToPresentationInterval from the Xbox 360 SDK's d3d9.lib (Sep 2013),
 * 120 instruction words as dumpbin shows them. PresentInterval is at +0x36B0 and the
 * immediate threshold word at +0x2FFC in this SDK version. */
static const fc_u32 sdk_sync_words[] = {
    0x7D8802A6u, 0x4BFFFFFDu, 0x9421FF80u, 0x7C9D2378u, 0x38800001u, 0x7C7F1B78u,
    0x4BFFFFE9u, 0x38800000u, 0x7FE3FB78u, 0x4BFFFFDDu, 0x3B80FFFFu, 0x2B1D0000u,
    0x409A008Cu, 0x817F6008u, 0x2B0B0000u, 0x409A0080u, 0x817F0038u, 0x807F0030u,
    0x7F035840u, 0x4099000Cu, 0x7FE3FB78u, 0x4BFFFFADu, 0x39600579u, 0x39400001u,
    0x95630004u, 0x3D60C004u, 0x39200013u, 0x616B3C00u, 0x39000001u, 0x38C00100u,
    0x95430004u, 0x7F87E378u, 0x95630004u, 0x95230004u, 0x817F2B14u, 0x396B0004u,
    0x556A653Eu, 0x556B00FEu, 0x394A0200u, 0x554A04E6u, 0x7D6A5A14u, 0x616B0002u,
    0x95630004u, 0x95030004u, 0x97830004u, 0x94C30004u, 0x907F0030u, 0x817F36B0u,
    0x2B0B0000u, 0x419A0034u, 0x2B0B0001u, 0x419A002Cu, 0x2B0B0002u, 0x419A001Cu,
    0x396BFFFCu, 0x39400003u, 0x316BFFFFu, 0x7D6B5910u, 0x7D6B5038u, 0x48000010u,
    0x39600002u, 0x48000008u, 0x39600001u, 0x815F2FFCu, 0x556B402Eu, 0x809F0030u,
    0x554A4E7Eu, 0x813F0038u, 0x7D4B5B78u, 0x7F044840u, 0x7D7EEB78u, 0x40990010u,
    0x7FE3FB78u, 0x4BFFFEDDu, 0x7C641B78u, 0x3D600000u, 0x7FC7F378u, 0x38CB0000u,
    0x38A00000u, 0x7FE3FB78u, 0x4BFFFEC1u, 0x2B1D0000u, 0x907F0030u, 0x409A0074u,
    0x817F6008u, 0x2B0B0000u, 0x409A0068u, 0x817F0038u, 0x7F035840u, 0x4099000Cu,
    0x7FE3FB78u, 0x4BFFFE95u, 0x3D60C004u, 0x39400013u, 0x616B3C00u, 0x39200000u,
    0x95630004u, 0x39000100u, 0x95430004u, 0x817F2B14u, 0x396B0004u, 0x556A653Eu,
    0x556B00FEu, 0x394A0200u, 0x554A04E6u, 0x7D6A5A14u, 0x616B0002u, 0x95630004u,
    0x95230004u, 0x97830004u, 0x95030004u, 0x907F0030u, 0x38800000u, 0x7FE3FB78u,
    0x4BFFFE39u, 0x897F2B3Du, 0x616B0010u, 0x997F2B3Du, 0x38210080u, 0x4BFFFE24u,
};

static void test_vsync_probe(void)
{
    const int n = (int)(sizeof(sdk_sync_words) / sizeof(sdk_sync_words[0]));
    fc_u32 io = 0, to = 0, a3 = 0, a4 = 0;
    int i, hits = 0, at = -1;

    printf("vsync probe\n");
    CHECK_EQ(n, 120);
    for (i = 0; i < n; i++)
        if (fc_match_present_fields(sdk_sync_words + i, n - i, &io, &to)) { hits++; at = i; }
    CHECK_EQ(hits, 1);
    CHECK_EQ(at, 0xBC / 4);           /* lwz r11,0x36B0(r31) */
    CHECK_EQ(io, 0x36B0);
    CHECK_EQ(to, 0x2FFC);

    /* Nothing matches in unrelated code (the counter's own test data). */
    CHECK(!fc_match_present_fields(sdk_sync_words, 5, &io, &to));

    {
        /* lis r11,0x8018 ; stw r3,0x1234(r11) ; stw r4,0x1238(r11) ; blr */
        static const fc_u32 simple[] = { 0x3D608018u, 0x906B1234u, 0x908B1238u, 0x4E800020u };
        /* mflr r12 ; mr r31,r4 ; lis r10,0x8019 ; addi r10,r10,-0x10 ; stw r31,8(r10) ;
         * stw r3,4(r10) ; blr */
        static const fc_u32 moved[] = { 0x7D8802A6u, 0x7C9F2378u, 0x3D408019u, 0x394AFFF0u,
                                        0x93EA0008u, 0x906A0004u, 0x4E800020u };
        /* lis r11,0x8018 ; bl x ; stw r3,0(r11): stores after a call are ignored */
        static const fc_u32 call[] = { 0x3D608018u, 0x48000005u, 0x906B0000u };
        CHECK_EQ(fc_find_arg_stores(simple, 4, &a3, &a4), 3);
        CHECK_EQ(a3, 0x80181234u);
        CHECK_EQ(a4, 0x80181238u);
        a3 = a4 = 0;
        CHECK_EQ(fc_find_arg_stores(moved, 7, &a3, &a4), 3);
        CHECK_EQ(a4, 0x8018FFF8u);
        CHECK_EQ(a3, 0x8018FFF4u);
        CHECK_EQ(fc_find_arg_stores(call, 3, &a3, &a4), 0);
    }
}

int main(void)
{
    test_counter();
    test_toggle();
    test_config();
    test_overlay();
    test_config_v2();
    test_modes();
    test_stats();
    test_temps();
    test_panel();
    test_vsync_probe();
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
