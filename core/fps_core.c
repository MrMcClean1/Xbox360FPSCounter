/*
 * fps_core.c - Portable core for the Xbox 360 FPS counter.
 * See fps_core.h. No platform headers beyond the C standard library.
 */
#include "fps_core.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Frame counter                                                       */
/* ------------------------------------------------------------------ */

void fc_counter_init(FcCounter *c, fc_u64 ticks_per_sec, unsigned window_ms)
{
    memset(c, 0, sizeof(*c));
    c->freq   = ticks_per_sec;
    c->window = (ticks_per_sec * (fc_u64)window_ms) / 1000;
    if (c->window == 0)
        c->window = 1;
}

int fc_counter_frame(FcCounter *c, fc_u64 now)
{
    fc_u64 elapsed;

    if (!c->started) {
        /* First frame only starts the clock; counting frames from here. */
        c->started = 1;
        c->start   = now;
        c->frames  = 0;
        return 0;
    }

    c->frames++;
    elapsed = now - c->start;
    if (elapsed < c->window)
        return 0;

    /* fps = frames / seconds, kept to one decimal and rounded. */
    c->fps_x10 = (unsigned)(((fc_u64)c->frames * c->freq * 10 + elapsed / 2) / elapsed);
    c->frames  = 0;
    c->start   = now;
    return 1;
}

unsigned fc_counter_fps(const FcCounter *c)
{
    return (c->fps_x10 + 5) / 10;
}

/* ------------------------------------------------------------------ */
/* Realtime toggle                                                     */
/* ------------------------------------------------------------------ */

void fc_toggle_init(FcToggle *t, unsigned short combo, unsigned hold_ms,
                    fc_u64 ticks_per_sec, int start_visible)
{
    memset(t, 0, sizeof(*t));
    t->combo      = combo;
    t->hold_ticks = (ticks_per_sec * (fc_u64)hold_ms) / 1000;
    t->visible    = start_visible ? 1 : 0;
}

int fc_toggle_update(FcToggle *t, unsigned short buttons, fc_u64 now)
{
    int full = t->combo != 0 && (buttons & t->combo) == t->combo;

    if (!full) {
        /* Any button in the combo let go: reset, ready for the next hold. */
        t->holding = 0;
        t->latched = 0;
        return 0;
    }

    if (t->latched)
        return 0; /* already fired for this hold */

    if (!t->holding) {
        t->holding    = 1;
        t->hold_start = now;
    }

    if (now - t->hold_start >= t->hold_ticks) {
        t->visible = !t->visible;
        t->latched = 1;
        t->holding = 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

void fc_config_defaults(FcConfig *cfg)
{
    cfg->enabled     = 0;     /* off until the combo is pressed        */
    cfg->x           = 64;    /* inside the 5% TV-safe margin at 720p  */
    cfg->y           = 36;
    cfg->scale       = 3;     /* 5x7 font -> 15x21 px characters       */
    cfg->opacity_pct = 50;
    cfg->combo       = FC_BTN_LB | FC_BTN_RB | FC_BTN_DPAD_DOWN;
    cfg->hold_ms     = 500;
    cfg->text_rgb    = 0xFFFFFF;
    cfg->show_stats  = 1;     /* all extras are in the combo cycle     */
    cfg->show_graph  = 1;
    cfg->show_temps  = 1;
}

int fc_mode_count(const FcConfig *cfg)
{
    return 2 + (cfg->show_stats != 0) + (cfg->show_graph != 0) + (cfg->show_temps != 0);
}

void fc_mode_config(const FcConfig *cfg, int mode, FcConfig *out)
{
    int extras = mode - 1;   /* how many extras this mode adds */

    *out = *cfg;
    out->show_stats = out->show_graph = out->show_temps = 0;
    if (cfg->show_stats && extras > 0) { out->show_stats = 1; extras--; }
    if (cfg->show_graph && extras > 0) { out->show_graph = 1; extras--; }
    if (cfg->show_temps && extras > 0) { out->show_temps = 1; extras--; }
}

static int fc_lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

static int fc_is_space(int ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/* Case-insensitive compare of [s, s+n) with a NUL-terminated word. */
static int fc_word_eq(const char *s, int n, const char *word)
{
    int i;
    for (i = 0; i < n; i++) {
        if (word[i] == '\0' || fc_lower((unsigned char)s[i]) != fc_lower((unsigned char)word[i]))
            return 0;
    }
    return word[n] == '\0';
}

/* Parses a decimal (or 0x hex) integer from [s, s+n). Returns 0 on failure. */
static int fc_parse_uint(const char *s, int n, unsigned *out)
{
    unsigned v = 0;
    int i = 0, base = 10, digits = 0;

    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (n >= 1 && s[0] == '#') {
        base = 16;
        i = 1;
    }
    for (; i < n; i++) {
        int ch = fc_lower((unsigned char)s[i]), d;
        if (ch >= '0' && ch <= '9')
            d = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f')
            d = ch - 'a' + 10;
        else
            return 0;
        if (v > 0x0FFFFFFFu)
            return 0; /* overflow guard */
        v = v * base + d;
        digits++;
    }
    if (digits == 0)
        return 0;
    *out = v;
    return 1;
}

static int fc_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

typedef struct FcButtonName {
    const char    *name;
    unsigned short bit;
} FcButtonName;

static const FcButtonName fc_button_names[] = {
    { "a", FC_BTN_A },       { "b", FC_BTN_B },
    { "x", FC_BTN_X },       { "y", FC_BTN_Y },
    { "lb", FC_BTN_LB },     { "rb", FC_BTN_RB },
    { "ls", FC_BTN_LTHUMB }, { "rs", FC_BTN_RTHUMB },
    { "start", FC_BTN_START }, { "back", FC_BTN_BACK },
    { "up", FC_BTN_DPAD_UP },       { "down", FC_BTN_DPAD_DOWN },
    { "left", FC_BTN_DPAD_LEFT },   { "right", FC_BTN_DPAD_RIGHT },
    { "dpad_up", FC_BTN_DPAD_UP },  { "dpad_down", FC_BTN_DPAD_DOWN },
    { "dpad_left", FC_BTN_DPAD_LEFT }, { "dpad_right", FC_BTN_DPAD_RIGHT },
    { NULL, 0 }
};

static int fc_combo_range(const char *s, int n, unsigned short *out)
{
    unsigned short combo = 0;
    int i = 0;

    while (i < n) {
        int start, end, k, found = 0;

        while (i < n && (fc_is_space((unsigned char)s[i]) || s[i] == '+'))
            i++;
        if (i >= n)
            break;
        start = i;
        while (i < n && s[i] != '+' && !fc_is_space((unsigned char)s[i]))
            i++;
        end = i;

        for (k = 0; fc_button_names[k].name != NULL; k++) {
            if (fc_word_eq(s + start, end - start, fc_button_names[k].name)) {
                combo |= fc_button_names[k].bit;
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    if (combo == 0)
        return 0;
    *out = combo;
    return 1;
}

int fc_parse_combo(const char *text, unsigned short *out)
{
    return fc_combo_range(text, (int)strlen(text), out);
}

/* Parses 1/true/on/yes and 0/false/off/no. Returns 0 if neither. */
static int fc_parse_bool(const char *v, int vn, int *out)
{
    if (fc_word_eq(v, vn, "1") || fc_word_eq(v, vn, "true") ||
        fc_word_eq(v, vn, "on") || fc_word_eq(v, vn, "yes")) {
        *out = 1;
        return 1;
    }
    if (fc_word_eq(v, vn, "0") || fc_word_eq(v, vn, "false") ||
        fc_word_eq(v, vn, "off") || fc_word_eq(v, vn, "no")) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* Applies one key/value pair. Returns 1 if a setting changed. */
static int fc_apply(FcConfig *cfg, const char *k, int kn, const char *v, int vn)
{
    unsigned u;

    if (fc_word_eq(k, kn, "enabled"))    return fc_parse_bool(v, vn, &cfg->enabled);
    if (fc_word_eq(k, kn, "show_stats")) return fc_parse_bool(v, vn, &cfg->show_stats);
    if (fc_word_eq(k, kn, "show_graph")) return fc_parse_bool(v, vn, &cfg->show_graph);
    if (fc_word_eq(k, kn, "show_temps")) return fc_parse_bool(v, vn, &cfg->show_temps);
    if (fc_word_eq(k, kn, "combo")) {
        unsigned short combo;
        if (!fc_combo_range(v, vn, &combo))
            return 0;
        cfg->combo = combo;
        return 1;
    }
    if (fc_word_eq(k, kn, "text_color")) {
        if (!fc_parse_uint(v, vn, &u) || u > 0xFFFFFF)
            return 0;
        cfg->text_rgb = u;
        return 1;
    }
    if (!fc_parse_uint(v, vn, &u))
        return 0;
    if (fc_word_eq(k, kn, "x"))            { cfg->x = fc_clamp((int)u, 0, 1200); return 1; }
    if (fc_word_eq(k, kn, "y"))            { cfg->y = fc_clamp((int)u, 0, 680);  return 1; }
    if (fc_word_eq(k, kn, "scale"))        { cfg->scale = fc_clamp((int)u, 1, 8); return 1; }
    if (fc_word_eq(k, kn, "opacity"))      { cfg->opacity_pct = fc_clamp((int)u, 0, 100); return 1; }
    if (fc_word_eq(k, kn, "hold_ms"))      { cfg->hold_ms = fc_clamp((int)u, 0, 5000); return 1; }
    return 0;
}

int fc_config_parse(FcConfig *cfg, const char *text)
{
    const char *p = text;
    int applied = 0;

    while (*p) {
        const char *line = p, *eol, *eq, *ks, *ke, *vs, *ve, *c;

        while (*p && *p != '\n')
            p++;
        eol = p;
        if (*p == '\n')
            p++;

        /* Strip comments (; or #) unless the # starts a color value. */
        for (c = line; c < eol; c++) {
            if (*c == ';')
                break;
            if (*c == '#') {
                const char *q = c - 1;
                while (q >= line && fc_is_space((unsigned char)*q))
                    q--;
                if (q < line || *q != '=')
                    break;
            }
        }
        eol = c;

        eq = (const char *)memchr(line, '=', (size_t)(eol - line));
        if (eq == NULL)
            continue; /* blank, [section] or junk */

        ks = line;
        ke = eq;
        while (ks < ke && fc_is_space((unsigned char)*ks)) ks++;
        while (ke > ks && fc_is_space((unsigned char)ke[-1])) ke--;
        vs = eq + 1;
        ve = eol;
        while (vs < ve && fc_is_space((unsigned char)*vs)) vs++;
        while (ve > vs && fc_is_space((unsigned char)ve[-1])) ve--;

        if (ke > ks && ve > vs)
            applied += fc_apply(cfg, ks, (int)(ke - ks), vs, (int)(ve - vs));
    }
    return applied;
}

/* ------------------------------------------------------------------ */
/* Font and overlay geometry                                           */
/* ------------------------------------------------------------------ */

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column. */
static const unsigned char fc_font_digits[10][FC_GLYPH_H] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E }, /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }  /* 9 */
};
typedef struct FcGlyph {
    char          ch;
    unsigned char rows[FC_GLYPH_H];
} FcGlyph;

/* Letters and marks used by the panel lines (FPS, MIN, AVG, CPU, GPU, RAM, MB, C). */
static const FcGlyph fc_font_letters[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { 0,   { 0 } }
};

static const unsigned char *fc_glyph(char ch)
{
    int i;
    if (ch >= '0' && ch <= '9')
        return fc_font_digits[ch - '0'];
    for (i = 0; fc_font_letters[i].ch != 0; i++)
        if (fc_font_letters[i].ch == ch)
            return fc_font_letters[i].rows;
    return NULL; /* space or unsupported: blank cell */
}

void fc_format(unsigned fps, char *buf)
{
    if (fps > 999)
        fps = 999;
    sprintf(buf, "%u FPS", fps);
}

static int fc_pad(const FcConfig *cfg)
{
    return cfg->scale * 2;
}

void fc_overlay_size(const FcConfig *cfg, const char *text, int *w, int *h)
{
    int n = (int)strlen(text);
    int s = cfg->scale, pad = fc_pad(cfg);
    int text_w = n > 0 ? n * (FC_GLYPH_W + 1) * s - s : 0;

    *w = text_w + 2 * pad;
    *h = FC_GLYPH_H * s + 2 * pad;
}

/* Emits the text pixel runs, shifted by (dx, dy), as `kind`. */
static int fc_emit_text(const FcConfig *cfg, const char *text, int dx, int dy,
                        int kind, FcRectFn fn, void *user)
{
    int s = cfg->scale, pad = fc_pad(cfg);
    int ox = cfg->x + pad + dx, oy = cfg->y + pad + dy;
    int count = 0, i;

    for (i = 0; text[i] != '\0'; i++) {
        const unsigned char *g = fc_glyph(text[i]);
        int gx = ox + i * (FC_GLYPH_W + 1) * s, row;

        if (g == NULL)
            continue;
        for (row = 0; row < FC_GLYPH_H; row++) {
            int col = 0;
            while (col < FC_GLYPH_W) {
                int run;
                if (!(g[row] & (0x10 >> col))) {
                    col++;
                    continue;
                }
                run = col;
                while (run < FC_GLYPH_W && (g[row] & (0x10 >> run)))
                    run++;
                fn(gx + col * s, oy + row * s, (run - col) * s, s, kind, user);
                count++;
                col = run;
            }
        }
    }
    return count;
}

int fc_emit_overlay(const FcConfig *cfg, const char *text, FcRectFn fn, void *user)
{
    int bw, bh, count = 0;
    int shadow = (cfg->scale + 1) / 2; /* 2 px at the default scale of 3 */

    fc_overlay_size(cfg, text, &bw, &bh);
    fn(cfg->x, cfg->y, bw, bh, FC_RECT_BOX, user);
    count++;
    count += fc_emit_text(cfg, text, shadow, shadow, FC_RECT_SHADOW, fn, user);
    count += fc_emit_text(cfg, text, 0, 0, FC_RECT_TEXT, fn, user);
    return count;
}

/* ------------------------------------------------------------------ */
/* Rolling min/avg                                                     */
/* ------------------------------------------------------------------ */

void fc_stats_init(FcStats *s)
{
    memset(s, 0, sizeof(*s));
}

void fc_stats_add(FcStats *s, unsigned fps)
{
    if (fps > 999)
        fps = 999;
    s->v[s->head] = (unsigned short)fps;
    s->head = (s->head + 1) % FC_STATS_N;
    if (s->count < FC_STATS_N)
        s->count++;
}

unsigned fc_stats_min(const FcStats *s)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < s->count; i++)
        if (i == 0 || s->v[i] < m)
            m = s->v[i];
    return m;
}

unsigned fc_stats_avg(const FcStats *s)
{
    unsigned sum = 0;
    int i;
    if (s->count == 0)
        return 0;
    for (i = 0; i < s->count; i++)
        sum += s->v[i];
    return (sum + (unsigned)s->count / 2) / (unsigned)s->count;
}

/* ------------------------------------------------------------------ */
/* Panel                                                               */
/* ------------------------------------------------------------------ */

/* Widest text each line can show; the panel is sized for these. */
#define FC_L1_MAX     "999 FPS"
#define FC_STATS_MAX  "MIN 999 AVG 999"
#define FC_TEMP1_MAX  "CPU 999C GPU 999C"
#define FC_TEMP2_MAX  "RAM 999C MB 999C"

typedef struct FcLayout {
    int s, s2;          /* main and secondary text scale */
    int pad, gap;
    int inner_w;
    int y_l1, y_stats, y_graph, graph_h, y_t1, y_t2;
    int w, h;
} FcLayout;

static int fc_text_w(const char *text, int scale)
{
    int n = (int)strlen(text);
    return n > 0 ? n * (FC_GLYPH_W + 1) * scale - scale : 0;
}

static void fc_layout(const FcConfig *cfg, FcLayout *L)
{
    int y, w;

    L->s   = cfg->scale;
    L->s2  = cfg->scale > 1 ? cfg->scale - 1 : 1;
    L->pad = fc_pad(cfg);
    L->gap = 2 * L->s2;

    L->inner_w = fc_text_w(FC_L1_MAX, L->s);
    if (cfg->show_stats && (w = fc_text_w(FC_STATS_MAX, L->s2)) > L->inner_w) L->inner_w = w;
    if (cfg->show_temps && (w = fc_text_w(FC_TEMP1_MAX, L->s2)) > L->inner_w) L->inner_w = w;
    if (cfg->show_temps && (w = fc_text_w(FC_TEMP2_MAX, L->s2)) > L->inner_w) L->inner_w = w;

    y = L->pad;
    L->y_l1 = y;
    y += FC_GLYPH_H * L->s;
    L->y_stats = L->y_graph = L->y_t1 = L->y_t2 = -1;
    L->graph_h = 0;
    if (cfg->show_stats) { y += L->gap; L->y_stats = y; y += FC_GLYPH_H * L->s2; }
    if (cfg->show_graph) { y += L->gap; L->y_graph = y; L->graph_h = 16 * L->s2; y += L->graph_h; }
    if (cfg->show_temps) {
        y += L->gap; L->y_t1 = y; y += FC_GLYPH_H * L->s2;
        y += L->gap; L->y_t2 = y; y += FC_GLYPH_H * L->s2;
    }
    L->w = L->inner_w + 2 * L->pad;
    L->h = y + L->pad;
}

void fc_panel_size(const FcConfig *cfg, int *w, int *h)
{
    FcLayout L;
    fc_layout(cfg, &L);
    *w = L.w;
    *h = L.h;
}

static void fc_px_set(unsigned char *px, int w, int h, int x, int y, int kind)
{
    if (x >= 0 && y >= 0 && x < w && y < h)
        px[y * w + x] = (unsigned char)kind;
}

/* Draws text at (x, y) with the given scale, as `kind`. */
static void fc_px_text(unsigned char *px, int w, int h, int x, int y,
                       const char *text, int scale, int kind)
{
    int i, row, col, a, b;
    for (i = 0; text[i] != '\0'; i++) {
        const unsigned char *g = fc_glyph(text[i]);
        int gx = x + i * (FC_GLYPH_W + 1) * scale;
        if (g == NULL)
            continue;
        for (row = 0; row < FC_GLYPH_H; row++)
            for (col = 0; col < FC_GLYPH_W; col++)
                if (g[row] & (0x10 >> col))
                    for (b = 0; b < scale; b++)
                        for (a = 0; a < scale; a++)
                            fc_px_set(px, w, h, gx + col * scale + a, y + row * scale + b, kind);
    }
}

/* Text with its drop shadow (shadow first, so text wins where they overlap). */
static void fc_px_label(unsigned char *px, int w, int h, int x, int y,
                        const char *text, int scale)
{
    int sh = (scale + 1) / 2;
    fc_px_text(px, w, h, x + sh, y + sh, text, scale, FC_PX_SHADOW);
    fc_px_text(px, w, h, x, y, text, scale, FC_PX_TEXT);
}

static void fc_temp_text(char *buf, const char *a, int ta, const char *b, int tb, int valid)
{
    if (!valid) {
        sprintf(buf, "%s --C %s --C", a, b);
        return;
    }
    if (ta < 0) ta = 0;
    if (ta > 999) ta = 999;
    if (tb < 0) tb = 0;
    if (tb > 999) tb = 999;
    sprintf(buf, "%s %dC %s %dC", a, ta, b, tb);
}

void fc_panel_render(const FcConfig *cfg, const FcPanelData *d,
                     unsigned char *px, int w, int h)
{
    /* 2x2 ordered dither thresholds: a pixel is backing box when its
     * threshold is below the opacity. 0% none, 50% checkerboard, 100% solid. */
    static const int bayer[4] = { 0, 50, 75, 25 };
    FcLayout L;
    char buf[32];
    int x, y;

    fc_layout(cfg, &L);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            px[y * w + x] = (unsigned char)
                (bayer[(y & 1) * 2 + (x & 1)] < cfg->opacity_pct ? FC_PX_BG : FC_PX_SKIP);

    fc_format(d->fps, buf);
    fc_px_label(px, w, h, L.pad, L.y_l1, buf, L.s);

    if (L.y_stats >= 0) {
        sprintf(buf, "MIN %u AVG %u", d->fps_min > 999 ? 999 : d->fps_min,
                d->fps_avg > 999 ? 999 : d->fps_avg);
        fc_px_label(px, w, h, L.pad, L.y_stats, buf, L.s2);
    }

    if (L.y_graph >= 0) {
        int gx0 = L.pad, gw = L.inner_w, gh = L.graph_h, gy1 = L.y_graph + gh - 1, i;
        /* Dotted guides at 16.7 ms (60 FPS) and 33.3 ms (30 FPS). */
        int guide60 = gy1 - (167 * gh) / (FC_GRAPH_MS * 10);
        int guide30 = gy1 - (333 * gh) / (FC_GRAPH_MS * 10);
        for (x = 0; x < gw; x += 2) {
            fc_px_set(px, w, h, gx0 + x, guide60, FC_PX_GRID);
            fc_px_set(px, w, h, gx0 + x, guide30, FC_PX_GRID);
        }
        /* Line graph, one column per frame, newest on the right. Each column
         * joins its level to the previous column's so spikes stay visible. */
        {
            int prev = -1;
            for (i = 0; i < gw; i++) {
                int idx = d->ft_count - gw + i, lvl, a, b;
                if (d->ft_x10 == NULL || idx < 0)
                    continue;
                lvl = ((int)d->ft_x10[idx] * gh + FC_GRAPH_MS * 5) / (FC_GRAPH_MS * 10);
                if (lvl < 1) lvl = 1;
                if (lvl > gh) lvl = gh;
                a = lvl;
                b = prev < 0 ? lvl : prev;
                if (a > b) { int t = a; a = b; b = t; }
                for (y = a; y <= b; y++)
                    fc_px_set(px, w, h, gx0 + i, gy1 - (y - 1), FC_PX_GRAPH);
                prev = lvl;
            }
        }
    }

    if (L.y_t1 >= 0) {
        fc_temp_text(buf, "CPU", d->temp_c[0], "GPU", d->temp_c[1], d->temps_valid);
        fc_px_label(px, w, h, L.pad, L.y_t1, buf, L.s2);
        fc_temp_text(buf, "RAM", d->temp_c[2], "MB", d->temp_c[3], d->temps_valid);
        fc_px_label(px, w, h, L.pad, L.y_t2, buf, L.s2);
    }
}

void fc_decode_temps(const unsigned char *reply, int out_c[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        unsigned v = (unsigned)reply[1 + 2 * i] | ((unsigned)reply[2 + 2 * i] << 8);
        out_c[i] = (int)((v + 128) >> 8);   /* 8.8 fixed point, rounded */
    }
}
