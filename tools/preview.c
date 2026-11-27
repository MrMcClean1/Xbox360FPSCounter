/*
 * preview.c - Renders the overlay panel onto a 1280x720 test frame using
 * fps_core.c's panel renderer, pixel for pixel as the console draws it
 * (no blending), and writes a PPM image.
 *
 * Usage: preview <out.ppm> <scene: bright|dark|busy> <fps> [fps.ini] [key=value ...]
 */
#include "fps_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1280
#define H 720

typedef struct Canvas {
    unsigned char *px; /* RGB */
    const FcConfig *cfg;
} Canvas;

static void put(Canvas *cv, int x, int y, unsigned rgb, int alpha_pct)
{
    unsigned char *p;
    int c;
    if (x < 0 || y < 0 || x >= W || y >= H)
        return;
    p = cv->px + (y * W + x) * 3;
    for (c = 0; c < 3; c++) {
        int src = (rgb >> (16 - 8 * c)) & 0xFF;
        p[c] = (unsigned char)((src * alpha_pct + p[c] * (100 - alpha_pct)) / 100);
    }
}

static void scene(unsigned char *px, const char *kind)
{
    int x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned char *p = px + (y * W + x) * 3;
            int r, g, b;
            if (strcmp(kind, "bright") == 0) {
                /* Midday sky fading to a pale horizon: worst case for white text. */
                r = 150 + y * 100 / H; g = 190 + y * 60 / H; b = 235 + y * 20 / H;
                if (((x / 90) + (y / 70)) % 5 == 0) { r = g = b = 250; } /* clouds */
            } else if (strcmp(kind, "dark") == 0) {
                /* Night interior. */
                r = 18 + (x * 20) / W; g = 20 + (y * 15) / H; b = 30 + (x * 25) / W;
            } else {
                /* Busy: high-contrast stripes and noise, like foliage or HUD clutter. */
                int v = ((x * 7 + y * 13) ^ (x * y)) & 0xFF;
                r = (x / 8 + y / 8) % 2 ? 230 : 40;
                g = (r + v) / 2;
                b = (v * 3) & 0xFF;
            }
            p[0] = (unsigned char)(r > 255 ? 255 : r);
            p[1] = (unsigned char)(g > 255 ? 255 : g);
            p[2] = (unsigned char)(b > 255 ? 255 : b);
        }
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Draws the panel the way the console does: every pixel is either left
 * alone (FC_PX_SKIP) or overwritten with a solid color. No blending. */
static void draw_panel(Canvas *cv, unsigned fps)
{
    static unsigned short ft[400];
    FcPanelData d;
    unsigned char *px;
    int w, h, x, y, i;

    /* Sample data: frame times around 1000/fps ms with a few hitches. */
    for (i = 0; i < 400; i++) {
        int base = fps ? (int)(10000 / fps) : 333;
        ft[i] = (unsigned short)(base + ((i * 37) % 23) - 11 + ((i % 97) == 50 ? 300 : 0));
    }
    memset(&d, 0, sizeof(d));
    d.fps = fps;
    d.fps_min = fps > 4 ? fps - 4 : 0;
    d.fps_avg = fps;
    d.temps_valid = 1;
    d.temp_c[0] = 62; d.temp_c[1] = 58; d.temp_c[2] = 50; d.temp_c[3] = 45;
    d.ft_x10 = ft;
    d.ft_count = 400;

    fc_panel_size(cv->cfg, &w, &h);
    px = (unsigned char *)malloc((size_t)(w * h));
    fc_panel_render(cv->cfg, &d, px, w, h);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            switch (px[y * w + x]) {
            case FC_PX_BG:
            case FC_PX_SHADOW: put(cv, cv->cfg->x + x, cv->cfg->y + y, 0x000000, 100); break;
            case FC_PX_TEXT:
            case FC_PX_GRAPH:  put(cv, cv->cfg->x + x, cv->cfg->y + y, cv->cfg->text_rgb, 100); break;
            case FC_PX_GRID:   put(cv, cv->cfg->x + x, cv->cfg->y + y, 0x808080, 100); break;
            default: break;
            }
    free(px);
}

int main(int argc, char **argv)
{
    FcConfig cfg;
    Canvas cv;
    FILE *out;
    int i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s out.ppm bright|dark|busy fps [fps.ini] [key=value ...]\n", argv[0]);
        return 2;
    }
    fc_config_defaults(&cfg);
    if (argc >= 5) {
        char *ini = read_file(argv[4]);
        if (ini) { fc_config_parse(&cfg, ini); free(ini); }
    }
    for (i = 5; i < argc; i++)      /* extra settings on the command line */
        fc_config_parse(&cfg, argv[i]);

    cv.px  = (unsigned char *)malloc(W * H * 3);
    cv.cfg = &cfg;
    scene(cv.px, argv[2]);
    draw_panel(&cv, (unsigned)atoi(argv[3]));

    out = fopen(argv[1], "wb");
    if (!out) return 1;
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    fwrite(cv.px, 1, W * H * 3, out);
    fclose(out);
    free(cv.px);
    return 0;
}
