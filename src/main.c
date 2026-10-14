/* mel - a pretty, customizable X11 screen locker.
 * One override-redirect window per monitor, Cairo drawing into an offscreen
 * buffer (no flicker), PAM auth. Layouts: minimal | card | left | pill | avatar | band | terminal | corner. */
#define _GNU_SOURCE
#include <ctype.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include "auth.h"
#include "config.h"

#define VERSION "0.4.0"

enum { L_CARD, L_MINIMAL, L_LEFT, L_PILL, L_AVATAR, L_BAND, L_TERMINAL, L_CORNER, L_RING, L_REACTOR };
enum { DOT_ANIM_MS = 240, GHOST_MS = 280, CAPS_MS = 220, UNLOCK_MS = 280 };

typedef struct { int x, y, w, h; Window win; cairo_surface_t *bg, *bg_sharp, *xs, *back; } mon_t;

/* per-frame animation values */
typedef struct {
    double t, fade, bgmix, e_panel, e_clock, e_date, e_input;
    double shake, shake_amt, unlock, caps_a, ghost;
    int caret;
} frame_t;

/* element positions, in UI units relative to the monitor centre */
typedef struct {
    int panel, align, avatar, band, ring;
    double pw, ph, cx, cy, dy, lx, ly, ix, iy, iw, ih, sx, sy, band_h, ax, ay, ar;
} lay_t;

static config_t cfg;
static Display *dpy;
static int scr, layout_id = L_MINIMAL, anim = 1;
static Window root;
static double ui_scale = 1.0;
static mon_t *mons;
static int nmons;
static char user[128];

/* password + typing state */
static char pwbuf[256];
static size_t pwlen, pwcount;
static double dot_birth[256];
static int ghost_n, ghost_fail;
static double ghost_t = -1e9;
static int failed_attempts, authenticating, unlock_ok;
static int caps_state;
#define NKEYS 16
static double key_time[NKEYS], key_ang[NKEYS];
static int key_kind[NKEYS], key_head;
static double caps_t = -1e9;

/* ---------- helpers ---------- */


static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void key_event(int kind)
{
    key_head = (key_head + 1) % NKEYS;
    key_time[key_head] = now_ms();
    key_ang[key_head] = drand48() * 2 * M_PI;
    key_kind[key_head] = kind;
}
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static double ease_out(double x) { x = clamp01(x); return 1 - pow(1 - x, 3); }
static double ease_back(double x)       /* overshoots slightly: gives the dots a "pop" */
{
    x = clamp01(x);
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * pow(x - 1, 3) + c1 * pow(x - 1, 2);
}
static void die(const char *m) { fprintf(stderr, "mel: %s\n", m); exit(1); }
static void rgba(cairo_t *cr, color_t c, double mul) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * mul); }
static color_t lerpc(color_t a, color_t b, double t)
{
    color_t c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
    return c;
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

/* align: 0 = centred on x, 1 = left edge at x */
static void text_at(cairo_t *cr, const char *s, double x, double base, double size, const char *font,
                    int bold, int align, color_t c, double alpha)
{
    cairo_text_extents_t e;
    cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    rgba(cr, c, alpha);
    cairo_move_to(cr, align ? x - e.x_bearing : x - (e.width / 2 + e.x_bearing), base);
    cairo_show_text(cr, s);
}

/* Clock text where ':' can pulse independently (alpha colon_a). */
static void clock_text(cairo_t *cr, const char *s, double x, double base, double size, const char *font,
                       int bold, int align, color_t c, double a, double colon_a)
{
    cairo_text_extents_t e;
    cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    cairo_move_to(cr, align ? x - e.x_bearing : x - (e.width / 2 + e.x_bearing), base);
    const char *p = s;
    while (*p) {
        size_t n = strcspn(p, ":");
        if (n) {
            char tmp[64]; if (n > 63) n = 63;
            memcpy(tmp, p, n); tmp[n] = 0;
            rgba(cr, c, a); cairo_show_text(cr, tmp); p += n;
        }
        if (*p == ':') { rgba(cr, c, a * colon_a); cairo_show_text(cr, ":"); p++; }
    }
}

/* ---------- background generation ---------- */

static void box_pass(uint8_t *img, int w, int h, int stride, int r, int horiz)
{
    int n = horiz ? w : h, lines = horiz ? h : w;
    int es = horiz ? 4 : stride, ls = horiz ? stride : 4;
    uint8_t *tmp = malloc((size_t)n * 3);
    if (!tmp) return;
    for (int l = 0; l < lines; l++) {
        uint8_t *base = img + (size_t)l * ls;
        for (int c = 0; c < 3; c++) {
            long sum = (long)(r + 1) * base[c];
            for (int i = 1; i <= r; i++) sum += base[(i < n ? i : n - 1) * es + c];
            for (int x = 0; x < n; x++) {
                tmp[x * 3 + c] = sum / (2 * r + 1);
                int a = x + r + 1; if (a > n - 1) a = n - 1;
                int b = x - r;     if (b < 0) b = 0;
                sum += base[a * es + c] - base[b * es + c];
            }
        }
        for (int x = 0; x < n; x++) {
            memcpy(base + (size_t)x * es, tmp + x * 3, 3);
            base[(size_t)x * es + 3] = 255;
        }
    }
    free(tmp);
}

static cairo_surface_t *make_blur(cairo_surface_t *src, int w, int h, int strength)
{
    int d = 4, sw = w / d > 0 ? w / d : 1, sh = h / d > 0 ? h / d : 1;
    cairo_surface_t *small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
    cairo_t *cr = cairo_create(small);
    cairo_scale(cr, (double)sw / w, (double)sh / h);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(small);
    uint8_t *data = cairo_image_surface_get_data(small);
    int stride = cairo_image_surface_get_stride(small);
    int r = strength / 2 < 1 ? 1 : strength / 2;
    for (int i = 0; i < 3; i++) { box_pass(data, sw, sh, stride, r, 1); box_pass(data, sw, sh, stride, r, 0); }
    cairo_surface_mark_dirty(small);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cr = cairo_create(out);
    cairo_scale(cr, (double)w / sw, (double)h / sh);
    cairo_set_source_surface(cr, small, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(small);
    return out;
}

static cairo_surface_t *grab_region(int x, int y, int w, int h)
{
    XImage *im = XGetImage(dpy, root, x, y, w, h, AllPlanes, ZPixmap);
    if (!im) return NULL;
    if (im->bits_per_pixel != 32) { XDestroyImage(im); return NULL; }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_surface_flush(s);
    uint8_t *d = cairo_image_surface_get_data(s);
    int st = cairo_image_surface_get_stride(s);
    for (int row = 0; row < h; row++)
        memcpy(d + (size_t)row * st, im->data + (size_t)row * im->bytes_per_line, (size_t)w * 4);
    cairo_surface_mark_dirty(s);
    XDestroyImage(im);
    return s;
}

/* darkens the edges: focuses the eye on the centre and hides blur edge artifacts */
static void vignette(cairo_surface_t *s, int w, int h, double strength)
{
    if (strength <= 0) return;
    cairo_t *cr = cairo_create(s);
    cairo_pattern_t *p = cairo_pattern_create_radial(w / 2.0, h / 2.0, 0, w / 2.0, h / 2.0, hypot(w, h) / 2);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(p, 0.45, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0, 0, 0, strength);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_destroy(cr);
}

static void build_bg(mon_t *m)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, m->w, m->h);
    cairo_t *cr = cairo_create(s);
    int done = 0;

    if (!strcmp(cfg.background, "blur") || !strcmp(cfg.background, "dim")) {
        cairo_surface_t *snap = grab_region(m->x, m->y, m->w, m->h);
        if (snap) {
            int blur = !strcmp(cfg.background, "blur");
            cairo_surface_t *pic = blur ? make_blur(snap, m->w, m->h, cfg.blur_strength) : snap;
            cairo_set_source_surface(cr, pic, 0, 0);
            cairo_paint(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, blur ? 0.25 : cfg.dim_strength);
            cairo_paint(cr);
            if (blur && cfg.blur_in && anim) {
                /* sharp, dimmed copy: the lock "blurs in" from this */
                m->bg_sharp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, m->w, m->h);
                cairo_t *c2 = cairo_create(m->bg_sharp);
                cairo_set_source_surface(c2, snap, 0, 0);
                cairo_paint(c2);
                cairo_set_source_rgba(c2, 0, 0, 0, 0.25);
                cairo_paint(c2);
                cairo_destroy(c2);
                vignette(m->bg_sharp, m->w, m->h, cfg.vignette);
            }
            if (blur) cairo_surface_destroy(pic);
            cairo_surface_destroy(snap);
            done = 1;
        }
    } else if (!strcmp(cfg.background, "image") && cfg.background_image[0]) {
        cairo_surface_t *img = cairo_image_surface_create_from_png(cfg.background_image);
        if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
            double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
            double sc = fmax(m->w / iw, m->h / ih);
            cairo_translate(cr, (m->w - iw * sc) / 2, (m->h - ih * sc) / 2);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, img, 0, 0);
            cairo_paint(cr);
            done = 1;
        } else fprintf(stderr, "mel: cannot load PNG '%s'\n", cfg.background_image);
        cairo_surface_destroy(img);
    } else if (!strcmp(cfg.background, "gradient")) {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, m->w, m->h);
        cairo_pattern_add_color_stop_rgb(p, 0, cfg.gradient_start.r, cfg.gradient_start.g, cfg.gradient_start.b);
        cairo_pattern_add_color_stop_rgb(p, 1, cfg.gradient_end.r, cfg.gradient_end.g, cfg.gradient_end.b);
        cairo_set_source(cr, p); cairo_paint(cr); cairo_pattern_destroy(p);
        done = 1;
    }
    if (!done) {
        cairo_set_source_rgb(cr, cfg.background_color.r, cfg.background_color.g, cfg.background_color.b);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    vignette(s, m->w, m->h, cfg.vignette);
    m->bg = s;
}

/* ---------- layouts ---------- */

static void compute_layout(const mon_t *m, lay_t *L, double cs, double ds)
{
    double mw = m->w / ui_scale, mh = m->h / ui_scale, fs = cfg.font_size;
    memset(L, 0, sizeof *L);
    switch (layout_id) {
    case L_CARD: {
        double W = cfg.panel_width, H = cfg.panel_height;
        L->panel = 1; L->pw = W; L->ph = H;
        L->cx = 0; L->cy = -H / 2 + 24 + cs * 0.8; L->dy = ds + 14;
        L->iw = W - 80; L->ih = 42; L->ix = -L->iw / 2; L->iy = H / 2 - 78;
        L->lx = 0; L->ly = L->iy - 12;
        L->sx = 0; L->sy = H / 2 - 16;
        break; }
    case L_LEFT: {
        double margin = fmax(64, mw * 0.07);
        L->align = 1; L->cx = -mw / 2 + margin; L->cy = -60; L->dy = ds + 20;
        L->lx = L->cx; L->ly = L->cy + L->dy + fs + 18;
        L->iw = 340; L->ih = 52; L->ix = L->cx; L->iy = L->ly + 34;
        L->sx = L->ix + L->iw / 2; L->sy = L->iy + L->ih + 28;
        break; }
    case L_PILL:
        L->cx = 0; L->cy = -mh * 0.12; L->dy = ds + 16;
        L->lx = 0; L->ly = L->cy + L->dy + fs + 14;
        L->iw = 360; L->ih = 56; L->ix = -L->iw / 2; L->iy = mh / 2 - 130; L->avatar = 1;
        L->sx = 0; L->sy = L->iy + L->ih + 28;
        break;
    case L_AVATAR:
        L->cx = 0; L->cy = -mh / 2 + 120; L->dy = ds + 14;
        L->ring = 1; L->ax = 0; L->ay = -10; L->ar = 52;
        L->lx = 0; L->ly = L->ay + L->ar + 38;
        L->iw = 300; L->ih = 48; L->ix = -L->iw / 2; L->iy = L->ly + 22;
        L->sx = 0; L->sy = L->iy + L->ih + 28;
        break;
    case L_BAND: {
        double margin = fmax(64, mw * 0.07);
        L->band = 1; L->band_h = 200; L->align = 1;
        L->cx = -mw / 2 + margin; L->cy = -6; L->dy = ds + 16;
        L->lx = L->cx; L->ly = L->cy + L->dy + fs + 14;
        L->iw = 340; L->ih = 52; L->ix = mw / 2 - margin - L->iw; L->iy = -L->ih / 2 - 8;
        L->sx = L->ix + L->iw / 2; L->sy = L->iy + L->ih + 26;
        break; }
    case L_CORNER: {
        double margin = fmax(64, mw * 0.06);
        L->align = 1; L->cx = -mw / 2 + margin; L->dy = ds + 22; L->cy = mh / 2 - margin - L->dy;
        L->lx = L->cx; L->ly = -mh / 2 + margin + fs;
        L->iw = 320; L->ih = 52; L->ix = mw / 2 - margin - L->iw; L->iy = mh / 2 - margin - L->ih;
        L->sx = L->ix + L->iw / 2; L->sy = L->iy - 14;
        break; }
    case L_TERMINAL:
        L->align = 1; L->cx = -fmin(360, mw / 2 - 40); L->cy = -40; L->dy = ds + 14;
        L->iy = L->cy + L->dy + 70;                 /* prompt baseline */
        break;
    default: /* L_MINIMAL */
        L->cx = 0; L->cy = -44; L->dy = ds + 18;
        L->lx = 0; L->ly = L->cy + L->dy + fs + 14;
        L->iw = 300; L->ih = 48; L->ix = -L->iw / 2; L->iy = L->ly + 28;
        L->sx = 0; L->sy = L->iy + L->ih + 28;
    }
}

static int caps_on(void)
{
    unsigned n = 0;
    XkbGetIndicatorState(dpy, XkbUseCoreKbd, &n);
    return n & 1;
}

static void draw_input(cairo_t *cr, const lay_t *L, const frame_t *f, double a)
{
    double ix = L->ix, iy = L->iy, iw = L->iw, ih = L->ih, t = f->t;
    double rad = L->panel ? cfg.input_radius : ih / 2;
    double cy = iy + ih / 2;

    cairo_save(cr);
    cairo_translate(cr, f->shake, (1 - f->e_input) * 14);

    /* field + outline (neutral / error / success) */
    rrect(cr, ix, iy, iw, ih, rad);
    rgba(cr, cfg.input_color, a);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.5);
    if (f->unlock > 0 && unlock_ok) rgba(cr, cfg.success_color, a * f->unlock);
    else if (f->shake_amt > 0.02)   rgba(cr, cfg.caps_lock_color, a * f->shake_amt);
    else { cairo_set_line_width(cr, 1); rgba(cr, cfg.input_text_color, a * 0.10); }
    cairo_stroke(cr);

    /* content area: [avatar] ...dots... [submit arrow] */
    double x0 = ix + (L->avatar ? ih : 16), x1 = ix + iw - ih, mid = (x0 + x1) / 2;
    if (L->avatar) {
        double ar = ih / 2 - 7, ax = ix + ih / 2;
        cairo_arc(cr, ax, cy, ar, 0, 2 * M_PI);
        rgba(cr, cfg.input_text_color, a * 0.16);
        cairo_fill(cr);
        char ini[2] = { (char)toupper((unsigned char)user[0]), 0 };
        text_at(cr, ini, ax, cy + ih * 0.13, ih * 0.36, cfg.font, 1, 0, cfg.input_text_color, a * 0.9);
    }

    double ready = pwcount > 0 ? (anim ? ease_out((t - dot_birth[0]) / 180) : 1) : 0;
    if (authenticating || (f->unlock > 0 && unlock_ok)) ready = 1;
    {   /* submit arrow: lights up once something is typed */
        double ax = ix + iw - ih / 2, ar = ih / 2 - 8;
        cairo_new_path(cr);
        cairo_arc(cr, ax, cy, ar, 0, 2 * M_PI);
        rgba(cr, cfg.input_text_color, a * (0.10 + 0.80 * ready));
        cairo_fill(cr);
        color_t dim = cfg.input_text_color; dim.a *= 0.5;
        color_t ink = lerpc(dim, cfg.panel_color, ready);
        rgba(cr, ink, a);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, ax - 5, cy); cairo_line_to(cr, ax + 5, cy);
        cairo_move_to(cr, ax + 1, cy - 4.5); cairo_line_to(cr, ax + 5.5, cy); cairo_line_to(cr, ax + 1, cy + 4.5);
        cairo_stroke(cr);
    }

    const double sp = 16, rd = 4.6;
    int maxv = (int)((x1 - x0 - 12) / sp); if (maxv < 1) maxv = 1;
    if (f->unlock > 0 && unlock_ok) {                     /* success check mark */
        double p = ease_out(f->unlock * 1.8);
        rgba(cr, cfg.success_color, a * p);
        cairo_set_line_width(cr, 2.4);
        cairo_move_to(cr, mid - 9, cy + 1); cairo_line_to(cr, mid - 3, cy + 7); cairo_line_to(cr, mid + 10, cy - 7);
        cairo_stroke(cr);
    } else if (pwcount == 0) {
        if (f->ghost >= 0 && f->ghost < 1 && ghost_n > 0) {   /* dots shrink away after fail / clear */
            int v = ghost_n < maxv ? ghost_n : maxv;
            double g = ease_out(f->ghost);
            for (int i = 0; i < v; i++) {
                cairo_arc(cr, mid - v * sp / 2 + sp / 2 + i * sp, cy, rd * (1 - 0.6 * g), 0, 2 * M_PI);
                rgba(cr, ghost_fail ? cfg.caps_lock_color : cfg.input_text_color, a * (1 - g));
                cairo_fill(cr);
            }
        } else if (!(f->unlock > 0)) {
            text_at(cr, cfg.placeholder, mid, cy + cfg.font_size * 0.35, cfg.font_size, cfg.font, 0, 0,
                    cfg.input_text_color, a * 0.45);
        }
    } else if (!strcmp(cfg.dot_style, "char")) {
        char dots[512] = ""; size_t cl = strlen(cfg.password_char);
        size_t shown = pwcount > 28 ? 28 : pwcount;
        for (size_t i = 0; i < shown && strlen(dots) + cl < sizeof dots; i++) strcat(dots, cfg.password_char);
        text_at(cr, dots, mid, cy + cfg.font_size * 0.45, cfg.font_size * 1.1, cfg.font, 0, 0,
                cfg.input_text_color, a);
    } else {
        int v = (int)pwcount < maxv ? (int)pwcount : maxv, first = (int)pwcount - v;
        for (int i = 0; i < v; i++) {
            double e = anim ? ease_back((t - dot_birth[first + i]) / DOT_ANIM_MS) : 1;
            if (e < 0) e = 0;
            cairo_arc(cr, mid - v * sp / 2 + sp / 2 + i * sp, cy, rd * e, 0, 2 * M_PI);
            rgba(cr, cfg.input_text_color, a);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

/* big profile circle; the ring fills as you type, flashes red on failure, green on success */
static void draw_big_avatar(cairo_t *cr, const lay_t *L, const frame_t *f)
{
    double e = f->e_clock, pop = anim ? ease_back(e) : 1, r = L->ar * (pop < 0 ? 0 : pop);
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - e) * 10);
    cairo_arc(cr, L->ax, L->ay, r, 0, 2 * M_PI);
    rgba(cr, cfg.input_text_color, 0.14 * e);
    cairo_fill(cr);
    char ini[2] = { (char)toupper((unsigned char)user[0]), 0 };
    text_at(cr, ini, L->ax, L->ay + r * 0.32, r * 0.9, cfg.font, 1, 0, cfg.input_text_color, 0.9 * e);
    cairo_set_line_width(cr, 2.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_path(cr);
    cairo_arc(cr, L->ax, L->ay, L->ar + 9, 0, 2 * M_PI);
    if (f->unlock > 0 && unlock_ok) rgba(cr, cfg.success_color, f->unlock);
    else if (f->shake_amt > 0.02)   rgba(cr, cfg.caps_lock_color, f->shake_amt);
    else                            rgba(cr, cfg.input_text_color, 0.16 * e);
    cairo_stroke(cr);
    if (pwcount && !(f->unlock > 0 && unlock_ok)) {
        double sweep = 2 * M_PI * fmin(1.0, pwcount / 16.0);
        cairo_new_path(cr);
        cairo_arc(cr, L->ax, L->ay, L->ar + 9, -M_PI / 2, -M_PI / 2 + sweep);
        rgba(cr, cfg.input_text_color, 0.75 * e);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* shell-prompt layout: monospace, "user@host:~$ ****" with a blinking block cursor */
static void draw_terminal(cairo_t *cr, const lay_t *L, const frame_t *f, const struct tm *tm, double cs, double ds)
{
    const char *mono = cfg.mono_font;
    char host[64], buf[256];
    gethostname(host, sizeof host); host[63] = 0;
    char *dot = strchr(host, '.'); if (dot) *dot = 0;
    double fs = cfg.font_size * 1.35, base = L->iy;

    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_clock) * 14);
    strftime(buf, sizeof buf, cfg.clock_format, tm);
    clock_text(cr, buf, L->cx, L->cy, cs, mono, 1, 1, cfg.clock_color, f->e_clock,
               (cfg.blink_colon && anim && (tm->tm_sec & 1)) ? 0.3 : 1);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_date) * 14);
    strftime(buf, sizeof buf, cfg.date_format, tm);
    char cm[300]; snprintf(cm, sizeof cm, "# %s", buf);
    text_at(cr, cm, L->cx, L->cy + L->dy, ds, mono, 0, 1, cfg.date_color, f->e_date);
    cairo_restore(cr);

    /* prompt line (shakes on failure) */
    cairo_save(cr);
    cairo_translate(cr, f->shake, (1 - f->e_input) * 14);
    cairo_select_font_face(cr, mono, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    snprintf(buf, sizeof buf, "%s@%s", user, host);
    cairo_move_to(cr, L->cx, base);
    rgba(cr, cfg.success_color, f->e_input); cairo_show_text(cr, buf);
    rgba(cr, cfg.clock_color, f->e_input);   cairo_show_text(cr, ":~$ ");
    char stars[128] = ""; size_t n = pwcount > 64 ? 64 : pwcount;
    for (size_t i = 0; i < n; i++) strcat(stars, "*");
    rgba(cr, lerpc(cfg.input_text_color, cfg.caps_lock_color, f->shake_amt), f->e_input);
    cairo_show_text(cr, stars);
    if (f->caret && !(f->unlock > 0)) {
        double px, py; cairo_get_current_point(cr, &px, &py);
        cairo_rectangle(cr, px + 2, base - fs * 0.85, fs * 0.55, fs * 1.1);
        rgba(cr, cfg.input_text_color, 0.85 * f->e_input);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    /* status lines */
    double sy = base + fs * 1.8, ss = fs * 0.85;
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_input) * 14);
    if (authenticating) text_at(cr, "authenticating...", L->cx, sy, ss, mono, 0, 1, cfg.input_text_color, 0.7);
    else if (f->unlock > 0 && unlock_ok) text_at(cr, "access granted", L->cx, sy, ss, mono, 0, 1, cfg.success_color, 1);
    else if (cfg.show_caps_lock && f->caps_a > 0.01)
        text_at(cr, "! caps lock is on", L->cx, sy, ss, mono, 0, 1, cfg.caps_lock_color, f->caps_a);
    else if (cfg.show_failed_attempts && failed_attempts > 0) {
        snprintf(buf, sizeof buf, "authentication failure (%d)", failed_attempts);
        text_at(cr, buf, L->cx, sy, ss, mono, 0, 1, cfg.caps_lock_color, 0.9);
    } else if (cfg.message[0]) {
        snprintf(buf, sizeof buf, "# %s", cfg.message);
        text_at(cr, buf, L->cx, sy, ss, mono, 0, 1, cfg.input_text_color, 0.5);
    }
    cairo_restore(cr);
}

static void arc_c(cairo_t *cr, double r, double a0, double a1, double lw, color_t c, double a, int cap)
{
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, r, a0, a1);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, cap ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
    rgba(cr, c, a);
    cairo_stroke(cr);
}
static void arc_glow(cairo_t *cr, double r, double a0, double a1, double lw, color_t c, double a, int cap)
{
    arc_c(cr, r, a0, a1, lw * 3.4, c, a * 0.16, cap);     /* fake glow: wide faint pass + crisp pass */
    arc_c(cr, r, a0, a1, lw, c, a, cap);
}
static void tick(cairo_t *cr, double ang, double r0, double r1, double lw, color_t c, double a)
{
    cairo_new_path(cr);
    cairo_move_to(cr, cos(ang) * r0, sin(ang) * r0);
    cairo_line_to(cr, cos(ang) * r1, sin(ang) * r1);
    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, c, a);
    cairo_stroke(cr);
}

/* Radial HUD layouts. ring: i3lock-color style (random arc flash per key, tick progress).
 * reactor: concentric rotating rings, progress arc, shockwave per key. */
static void draw_radial(cairo_t *cr, const frame_t *f, const struct tm *tm, double cs, double ds, int bold,
                        int reactor)
{
    const double TAU = 2 * M_PI, TOP = -M_PI / 2;
    double R = cfg.ring_radius, t = f->t, e = f->e_panel, rot = t * 0.00035;
    int ok = f->unlock > 0 && unlock_ok;
    color_t ac = ok ? cfg.success_color : lerpc(cfg.accent_color, cfg.caps_lock_color, f->shake_amt > 0.02 ? f->shake_amt : 0);
    int lit = ok ? 60 : (int)(pwcount > 60 ? 60 : pwcount);
    static const double dash_a[2] = { 2, 9 }, dash_b[2] = { 1, 10 };
    char buf[256];

    cairo_save(cr);
    cairo_translate(cr, f->shake * 0.6, 0);                  /* ring wobbles on a wrong password */
    cairo_scale(cr, 0.9 + 0.1 * e, 0.9 + 0.1 * e);

    if (!reactor) {
        arc_c(cr, R, TOP, TOP + TAU * e, 3, ac, 0.20, 0);     /* track sweeps in on lock */
        for (int i = 0; i < 60; i++) {                          /* 60 ticks: one lights per typed char */
            double a = TOP + i * TAU / 60;
            if (i < lit) tick(cr, a, R + 14, R + (i % 5 ? 20 : 25), i % 5 ? 1.6 : 2.2, ac, 0.95 * e);
            else         tick(cr, a, R + 14, R + (i % 5 ? 20 : 25), i % 5 ? 1.2 : 1.8, cfg.input_text_color, 0.16 * e);
        }
        cairo_set_dash(cr, dash_a, 2, -t * 0.012);              /* inner dashed ring turns the other way */
        arc_c(cr, R - 16, 0, TAU, 2, ac, 0.28 * e, 0);
        cairo_set_dash(cr, NULL, 0, 0);
        for (int k = 0; k < 3; k++) arc_c(cr, R + 40, rot + k * TAU / 3, rot + k * TAU / 3 + 0.7, 1.6, ac, 0.4 * e, 1);
        for (int i = 0; i < NKEYS; i++) {                       /* random arc flashes per keypress */
            double age = t - key_time[i];
            if (age < 0 || age > 800) continue;
            double al = 1 - age / 800;
            arc_glow(cr, R, key_ang[i] - 0.23, key_ang[i] + 0.23, 5,
                     key_kind[i] ? cfg.caps_lock_color : ac, al * al * 0.5 + al * 0.5, 1);
        }
        if (authenticating) arc_glow(cr, R, t * 0.006, t * 0.006 + 1.2, 5, ac, 0.95, 1);
    } else {
        arc_c(cr, R, 0, TAU, 1.5, ac, 0.22 * e, 0);
        arc_c(cr, R - 26, 0, TAU, 3, ac, 0.12 * e, 0);          /* progress arc: fills as you type */
        double sw = TAU * fmin(1.0, (ok ? 16 : (double)pwcount) / 16.0);
        if (sw > 0) arc_glow(cr, R - 26, TOP, TOP + sw, 3.5, ac, 0.95 * e, 1);
        for (int k = 0; k < 3; k++) arc_c(cr, R + 26, rot * 1.6 + k * TAU / 3, rot * 1.6 + k * TAU / 3 + 1.1, 2, ac, 0.55 * e, 1);
        cairo_set_dash(cr, dash_b, 2, t * 0.01);
        arc_c(cr, R + 52, 0, TAU, 2, ac, 0.30 * e, 0);
        cairo_set_dash(cr, NULL, 0, 0);
        for (int k = 0; k < 4; k++) tick(cr, k * TAU / 4, R + 64, R + 76, 1.8, ac, 0.5 * e);
        for (int i = 0; i < NKEYS; i++) {                       /* shockwave per keypress */
            double age = t - key_time[i];
            if (age < 0 || age > 700) continue;
            double p = ease_out(age / 700);
            arc_c(cr, R - 26 + p * 96, 0, TAU, 2, key_kind[i] ? cfg.caps_lock_color : ac, (1 - p) * 0.6 * e, 0);
        }
        if (authenticating) arc_glow(cr, R + 26, t * 0.008, t * 0.008 + 1.6, 3, ac, 0.95, 1);
    }
    cairo_restore(cr);

    /* centre: clock, date, greeting, hint */
    double cy = 14;
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_clock) * 14);
    strftime(buf, sizeof buf, cfg.clock_format, tm);
    clock_text(cr, buf, 0, cy, cs, cfg.clock_font, bold, 0, cfg.clock_color, f->e_clock,
               (cfg.blink_colon && anim && (tm->tm_sec & 1)) ? 0.3 : 1);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_date) * 14);
    strftime(buf, sizeof buf, cfg.date_format, tm);
    text_at(cr, buf, 0, cy + ds + 12, ds, cfg.font, 0, 0, cfg.date_color, f->e_date);
    char nm[128], lb[200];
    snprintf(nm, sizeof nm, "%s", user); nm[0] = toupper((unsigned char)nm[0]);
    const char *g = tm->tm_hour < 5 ? "Good night" : tm->tm_hour < 12 ? "Good morning" :
                    tm->tm_hour < 18 ? "Good afternoon" : "Good evening";
    if (cfg.greeting) snprintf(lb, sizeof lb, "%s, %s", g, nm); else snprintf(lb, sizeof lb, "%s", nm);
    text_at(cr, lb, 0, cy - cs * 0.78 - 16, cfg.font_size * 0.95, cfg.font, 0, 0, cfg.input_text_color, 0.7 * f->e_date);
    cairo_restore(cr);

    double sy = R + (reactor ? 106 : 84);
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_input) * 14);
    if (authenticating) text_at(cr, "Authenticating\xe2\x80\xa6", 0, sy, cfg.font_size * 0.85, cfg.font, 0, 0, cfg.input_text_color, 0.7);
    else if (cfg.show_caps_lock && f->caps_a > 0.01)
        text_at(cr, "Caps Lock is on", 0, sy, cfg.font_size * 0.85, cfg.font, 1, 0, cfg.caps_lock_color, f->caps_a);
    else if (cfg.show_failed_attempts && failed_attempts > 0) {
        snprintf(buf, sizeof buf, "%d failed attempt%s", failed_attempts, failed_attempts == 1 ? "" : "s");
        text_at(cr, buf, 0, sy, cfg.font_size * 0.85, cfg.font, 0, 0, cfg.caps_lock_color, 0.85);
    } else if (pwcount == 0 && !ok)
        text_at(cr, cfg.placeholder, 0, cy + ds + 44, cfg.font_size * 0.85, cfg.font, 0, 0, cfg.input_text_color, 0.38 * f->e_input);
    else if (cfg.message[0])
        text_at(cr, cfg.message, 0, sy, cfg.font_size * 0.85, cfg.font, 0, 0, cfg.input_text_color, 0.6);
    cairo_restore(cr);
}

static void draw_mon(mon_t *m, const frame_t *f)
{
    cairo_t *cr = cairo_create(m->back);   /* offscreen: nothing half-drawn is ever visible */
    char buf[256], label[160];
    time_t tt = time(NULL);
    struct tm tm; localtime_r(&tt, &tm);

    static const int auto_cs[] = { 64, 112, 150, 96, 72, 96, 72, 160, 64, 64 },
                     auto_ds[] = { 18, 22, 26, 20, 18, 20, 18, 24, 18, 18 };
    double cs = cfg.clock_size > 0 ? cfg.clock_size : auto_cs[layout_id];
    double ds = cfg.date_size > 0 ? cfg.date_size : auto_ds[layout_id];
    int bold = cfg.clock_bold < 0 ? layout_id == L_CARD : cfg.clock_bold;
    double mw = m->w / ui_scale;
    lay_t L; compute_layout(m, &L, cs, ds);

    /* background (blurs in from the sharp desktop on lock) */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    if (m->bg_sharp && f->bgmix < 1) {
        cairo_set_source_surface(cr, m->bg_sharp, 0, 0); cairo_paint(cr);
        cairo_set_source_surface(cr, m->bg, 0, 0); cairo_paint_with_alpha(cr, f->bgmix);
    } else { cairo_set_source_surface(cr, m->bg, 0, 0); cairo_paint(cr); }

    cairo_save(cr);
    double us = 1 + 0.04 * ease_out(f->unlock);          /* gentle zoom on unlock */
    cairo_translate(cr, m->w / 2.0, m->h / 2.0);
    cairo_scale(cr, ui_scale * us, ui_scale * us);

    if (L.panel) {
        double s = 0.94 + 0.06 * f->e_panel, W = L.pw, H = L.ph, ep = f->e_panel;
        cairo_scale(cr, s, s);
        cairo_translate(cr, -W / 2, -H / 2);
        if (cfg.panel_shadow)
            for (int i = 0; i < 14; i++) {
                cairo_set_source_rgba(cr, 0, 0, 0, 0.032 * (1.0 - i / 14.0) * ep);
                rrect(cr, -i * 2, 8 - i * 2, W + i * 4, H + i * 4, cfg.panel_radius + i * 2);
                cairo_fill(cr);
            }
        rrect(cr, 0, 0, W, H, cfg.panel_radius);
        rgba(cr, cfg.panel_color, cfg.panel_opacity * ep);
        cairo_fill_preserve(cr);
        if (cfg.panel_border) {      /* glassy edge: brighter at the top */
            cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, H);
            cairo_pattern_add_color_stop_rgba(g, 0, cfg.panel_border_color.r, cfg.panel_border_color.g,
                cfg.panel_border_color.b, fmin(1, cfg.panel_border_color.a * 2.2) * ep);
            cairo_pattern_add_color_stop_rgba(g, 1, cfg.panel_border_color.r, cfg.panel_border_color.g,
                cfg.panel_border_color.b, cfg.panel_border_color.a * 0.5 * ep);
            cairo_set_source(cr, g); cairo_set_line_width(cr, 1); cairo_stroke(cr);
            cairo_pattern_destroy(g);
        } else cairo_new_path(cr);
        cairo_translate(cr, W / 2, H / 2);      /* back to centre-origin coordinates */
    }

    if (L.band) {                /* full-width frosted strip, grows open */
        double bh = L.band_h * f->e_panel;
        cairo_rectangle(cr, -mw / 2 - 100, -bh / 2, mw + 200, bh);
        rgba(cr, cfg.panel_color, cfg.panel_opacity * 0.7 * f->e_panel);
        cairo_fill(cr);
        rgba(cr, cfg.input_text_color, 0.10 * f->e_panel);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, -mw / 2 - 100, -bh / 2 + 0.5); cairo_line_to(cr, mw / 2 + 100, -bh / 2 + 0.5);
        cairo_move_to(cr, -mw / 2 - 100, bh / 2 - 0.5);  cairo_line_to(cr, mw / 2 + 100, bh / 2 - 0.5);
        cairo_stroke(cr);
    }
    if (L.ring) draw_big_avatar(cr, &L, f);

    if (layout_id == L_TERMINAL) draw_terminal(cr, &L, f, &tm, cs, ds);
    else if (layout_id == L_RING || layout_id == L_REACTOR) draw_radial(cr, f, &tm, cs, ds, bold, layout_id == L_REACTOR);
    else {
    /* clock + date, staggered entrance */
    if (cfg.show_clock) {
        cairo_save(cr);
        cairo_translate(cr, 0, (1 - f->e_clock) * 14);
        strftime(buf, sizeof buf, cfg.clock_format, &tm);
        double colon = (cfg.blink_colon && anim && (tm.tm_sec & 1)) ? 0.3 : 1;
        clock_text(cr, buf, L.cx, L.cy, cs, cfg.clock_font, bold, L.align, cfg.clock_color, f->e_clock, colon);
        cairo_restore(cr);
        cairo_save(cr);
        cairo_translate(cr, 0, (1 - f->e_date) * 14);
        strftime(buf, sizeof buf, cfg.date_format, &tm);
        text_at(cr, buf, L.cx, L.cy + L.dy, ds, cfg.font, 0, L.align, cfg.date_color, f->e_date);
        cairo_restore(cr);
    }

    /* greeting / username */
    if (cfg.greeting) {
        const char *g = tm.tm_hour < 5 ? "Good night" : tm.tm_hour < 12 ? "Good morning" :
                        tm.tm_hour < 18 ? "Good afternoon" : "Good evening";
        char nm[128]; snprintf(nm, sizeof nm, "%s", user); nm[0] = toupper((unsigned char)nm[0]);
        snprintf(label, sizeof label, "%s, %s", g, nm);
    } else snprintf(label, sizeof label, "%s", user);
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_date) * 14);
    text_at(cr, label, L.lx, L.ly, cfg.font_size * (L.panel ? 0.95 : 1.05), cfg.font, 0, L.align,
            cfg.input_text_color, 0.7 * f->e_date);
    cairo_restore(cr);

    draw_input(cr, &L, f, f->e_input);

    /* status line: caps lock slides/fades in; otherwise attempts or message */
    const double ss = cfg.font_size * 0.85;
    cairo_save(cr);
    cairo_translate(cr, 0, (1 - f->e_input) * 14);
    if (authenticating)
        text_at(cr, "Authenticating\xe2\x80\xa6", L.sx, L.sy, ss, cfg.font, 0, 0, cfg.input_text_color, 0.7);
    else if (cfg.show_caps_lock && f->caps_a > 0.01) {
        cairo_translate(cr, 0, (1 - f->caps_a) * 6);
        text_at(cr, "Caps Lock is on", L.sx, L.sy, ss, cfg.font, 1, 0, cfg.caps_lock_color, f->caps_a);
    } else if (cfg.show_failed_attempts && failed_attempts > 0) {
        snprintf(buf, sizeof buf, "%d failed attempt%s", failed_attempts, failed_attempts == 1 ? "" : "s");
        text_at(cr, buf, L.sx, L.sy, ss, cfg.font, 0, 0, cfg.caps_lock_color, 0.85 * f->e_input);
    } else if (cfg.message[0])
        text_at(cr, cfg.message, L.sx, L.sy, ss, cfg.font, 0, 0, cfg.input_text_color, 0.6 * f->e_input);
    cairo_restore(cr);
    }   /* !terminal */
    cairo_restore(cr);

    /* global fade to/from black */
    if (f->fade < 1) {
        cairo_set_source_rgba(cr, 0, 0, 0, 1 - f->fade);
        cairo_paint(cr);
    }
    cairo_destroy(cr);

    /* single atomic copy of the finished frame to the window */
    cairo_t *wc = cairo_create(m->xs);
    cairo_set_operator(wc, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(wc, m->back, 0, 0);
    cairo_paint(wc);
    cairo_destroy(wc);
}

/* ---------- X setup ---------- */

static void create_windows(void)
{
    int n = 0;
    XRRMonitorInfo *mi = XRRGetMonitors(dpy, root, True, &n);
    if (n <= 0 || !mi) { mi = NULL; n = 1; }
    mons = calloc(n, sizeof *mons);
    nmons = n;

    static char blank[1] = { 0 };
    Pixmap pm = XCreateBitmapFromData(dpy, root, blank, 1, 1);
    XColor black = { 0 };
    Cursor cur = XCreatePixmapCursor(dpy, pm, pm, &black, &black, 0, 0);

    for (int i = 0; i < n; i++) {
        mon_t *m = &mons[i];
        m->x = mi ? mi[i].x : 0;           m->y = mi ? mi[i].y : 0;
        m->w = mi ? mi[i].width : DisplayWidth(dpy, scr);
        m->h = mi ? mi[i].height : DisplayHeight(dpy, scr);
        build_bg(m);                       /* screenshot BEFORE our windows cover the desktop */
    }
    for (int i = 0; i < n; i++) {
        mon_t *m = &mons[i];
        XSetWindowAttributes wa;
        wa.override_redirect = True;
        wa.background_pixel = BlackPixel(dpy, scr);
        wa.event_mask = ExposureMask | VisibilityChangeMask;
        m->win = XCreateWindow(dpy, root, m->x, m->y, m->w, m->h, 0, DefaultDepth(dpy, scr),
                               CopyFromParent, DefaultVisual(dpy, scr),
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
        XDefineCursor(dpy, m->win, cur);
        XMapRaised(dpy, m->win);
        m->xs = cairo_xlib_surface_create(dpy, m->win, DefaultVisual(dpy, scr), m->w, m->h);
        m->back = cairo_surface_create_similar(m->xs, CAIRO_CONTENT_COLOR, m->w, m->h);
    }
    if (mi) XRRFreeMonitors(mi);
    XFreePixmap(dpy, pm);
    XSync(dpy, False);

    int ok = 0;
    for (int i = 0; i < 1000 && !ok; i++) {
        ok = XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, cur, CurrentTime) == GrabSuccess;
        if (!ok) usleep(1000);
    }
    if (!ok) die("cannot grab pointer");
    ok = 0;
    for (int i = 0; i < 1000 && !ok; i++) {
        ok = XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
        if (!ok) usleep(1000);
    }
    if (!ok) die("cannot grab keyboard");
}

static void clear_pw(int fail)
{
    if (pwcount) { ghost_n = (int)pwcount; ghost_t = now_ms(); ghost_fail = fail; key_event(1); }
    explicit_bzero(pwbuf, sizeof pwbuf);
    pwlen = pwcount = 0;
}

static void run_on_unlock(void)
{
    if (!cfg.on_unlock[0]) return;
    if (fork() == 0) { setsid(); execl("/bin/sh", "sh", "-c", cfg.on_unlock, (char *)NULL); _exit(127); }
}

static void usage(void)
{
    puts("usage: mel [OPTIONS]\n"
         "  -c, --config <file>       use custom config\n"
         "  -l, --layout <name>       minimal | card | left | pill | avatar | band | terminal | corner | ring | reactor\n"
         "  -i, --image <path>        background image (PNG), implies image mode\n"
         "  -b, --background <mode>   blur | dim | image | solid | gradient\n"
         "      --blur <strength>     blur strength\n"
         "      --no-fade             disable animations\n"
         "      --version | --help");
}

int main(int argc, char **argv)
{
    char path[512] = "", theme[512];
    static const struct option lo[] = {
        { "config", 1, 0, 'c' }, { "layout", 1, 0, 'l' }, { "image", 1, 0, 'i' }, { "background", 1, 0, 'b' },
        { "blur", 1, 0, 1 }, { "no-fade", 0, 0, 2 }, { "version", 0, 0, 3 }, { "help", 0, 0, 'h' }, { 0 } };
    char *o_image = NULL, *o_bg = NULL, *o_blur = NULL, *o_layout = NULL; int o_nofade = 0, ch;
    while ((ch = getopt_long(argc, argv, "c:l:i:b:h", lo, NULL)) != -1) switch (ch) {
        case 'c': snprintf(path, sizeof path, "%s", optarg); break;
        case 'l': o_layout = optarg; break;
        case 'i': o_image = optarg; break;
        case 'b': o_bg = optarg; break;
        case 1: o_blur = optarg; break;
        case 2: o_nofade = 1; break;
        case 3: puts("mel " VERSION); return 0;
        default: usage(); return ch == 'h' ? 0 : 1;
    }

    /* config: defaults -> (theme) -> user config -> CLI */
    config_defaults(&cfg);
    if (!path[0] && config_find_path(path, sizeof path)) path[0] = 0;
    if (path[0]) config_load(&cfg, path);
    if (cfg.theme[0] && !config_find_theme(cfg.theme, theme, sizeof theme)) {
        char keep[64]; snprintf(keep, sizeof keep, "%s", cfg.theme);
        config_defaults(&cfg); config_load(&cfg, theme);
        if (path[0]) config_load(&cfg, path);
        snprintf(cfg.theme, sizeof cfg.theme, "%s", keep);
    }
    if (o_image) { config_expand_tilde(o_image, cfg.background_image, sizeof cfg.background_image);
                   strcpy(cfg.background, "image"); }
    if (o_bg) snprintf(cfg.background, sizeof cfg.background, "%s", o_bg);
    if (o_blur) cfg.blur_strength = atoi(o_blur);
    if (o_nofade) cfg.enable_animations = 0;
    if (o_layout) snprintf(cfg.layout, sizeof cfg.layout, "%s", o_layout);
    anim = cfg.enable_animations;
    {
        static const char *names[] = { "card", "minimal", "left", "pill", "avatar", "band", "terminal", "corner", "ring", "reactor" };
        layout_id = L_MINIMAL;
        for (int i = 0; i < 10; i++) if (!strcmp(cfg.layout, names[i])) layout_id = i;
    }

    if (!(dpy = XOpenDisplay(NULL))) die("cannot open display");
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);

    if (cfg.scale > 0) ui_scale = cfg.scale;
    else { char *dpi = XGetDefault(dpy, "Xft", "dpi"); if (dpi && atof(dpi) > 96) ui_scale = atof(dpi) / 96.0; }

    FILE *oom = fopen("/proc/self/oom_score_adj", "w");
    if (oom) { fputs("-1000\n", oom); fclose(oom); }
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGQUIT, SIG_IGN);

    struct passwd *pw = getpwuid(getuid());
    if (!pw) die("cannot determine user");
    snprintf(user, sizeof user, "%s", pw->pw_name);

    srand48(time(NULL) ^ getpid());
    for (int i = 0; i < NKEYS; i++) key_time[i] = -1e9;
    create_windows();
    caps_state = caps_on();

    double t0 = now_ms(), shake_t = -1e9, unlock_t = 0;
    time_t last_sec = 0;
    int dirty = 1, unlocking = 0, bg_cleared = 0, last_caret = -1;

    for (;;) {
        /* --- events --- */
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
            if (ev.type == KeyPress || ev.type == KeyRelease) {
                int c = caps_on();
                if (c != caps_state) { caps_state = c; caps_t = now_ms(); dirty = 1; }
            }
            if (ev.type == Expose) dirty = 1;
            else if (ev.type == VisibilityNotify && ev.xvisibility.state != VisibilityUnobscured) {
                for (int i = 0; i < nmons; i++) XRaiseWindow(dpy, mons[i].win);
            } else if (unlocking) continue;
            else if ((ev.type == KeyPress || ev.type == ButtonPress || ev.type == MotionNotify) &&
                     cfg.grace_period > 0 && now_ms() - t0 < cfg.grace_period * 1000.0) {
                unlocking = 1; unlock_t = now_ms();
            } else if (ev.type == KeyPress) {
                char buf[32]; KeySym ks;
                int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
                if (ks == XK_KP_Enter) ks = XK_Return;
                dirty = 1;
                if (ks == XK_Return) {
                    if (pwlen == 0 && cfg.ignore_empty_password) continue;
                    authenticating = 1;
                    frame_t fr = { .t = now_ms(), .fade = 1, .bgmix = 1, .e_panel = 1, .e_clock = 1, .e_date = 1,
                                   .e_input = 1, .caps_a = caps_state, .ghost = -1, .caret = 1 };
                    for (int i = 0; i < nmons; i++) draw_mon(&mons[i], &fr);
                    XFlush(dpy);
                    int ok = auth_check(cfg.auth_service, user, pwbuf);
                    authenticating = 0;
                    XEvent junk;                      /* drop keys typed while PAM was busy */
                    while (XCheckMaskEvent(dpy, KeyPressMask | KeyReleaseMask, &junk));
                    if (ok) {
                        clear_pw(0); ghost_n = 0;
                        unlock_ok = 1; unlocking = 1; unlock_t = now_ms();
                    } else {
                        clear_pw(1);
                        failed_attempts++; shake_t = now_ms();
                        if (cfg.bell_on_fail) XkbBell(dpy, None, 0, None);
                    }
                } else if (ks == XK_Escape) clear_pw(0);
                else if (ks == XK_BackSpace) {
                    if (pwlen) {
                        do pwlen--; while (pwlen && ((unsigned char)pwbuf[pwlen] & 0xC0) == 0x80);
                        pwbuf[pwlen] = 0;
                        if (pwcount) pwcount--;
                        key_event(1);
                    }
                } else if (ev.xkey.state & ControlMask) {
                    if (ks == XK_u) clear_pw(0);
                } else if (n > 0 && !iscntrl((unsigned char)buf[0]) && pwlen + n < sizeof pwbuf - 1 && pwcount < 255) {
                    memcpy(pwbuf + pwlen, buf, n); pwlen += n; pwbuf[pwlen] = 0;
                    dot_birth[pwcount++] = now_ms();
                    key_event(0);
                }
            }
        }

        /* --- animation state --- */
        double t = now_ms(), tt = t - t0;
        frame_t f = { .t = t, .fade = 1, .bgmix = 1, .e_panel = 1, .e_clock = 1, .e_date = 1,
                      .e_input = 1, .caps_a = caps_state, .ghost = -1, .caret = 1 };
        int animating = 0;
        if (anim) {
            double D = cfg.fade_in_duration > 0 ? cfg.fade_in_duration : 1;
            f.fade = clamp01(tt / D);
            f.bgmix = ease_out(tt / (D * 1.8));
            f.e_panel = ease_out(tt / D);
            f.e_clock = ease_out(tt / (D * 1.4));
            f.e_date = ease_out((tt - D * 0.12) / (D * 1.4));
            f.e_input = ease_out((tt - D * 0.24) / (D * 1.4));
            if (tt < D * 1.9) animating = 1;

            double sdt = t - shake_t;
            if (sdt < 500) {
                f.shake_amt = exp(-sdt / 170);
                f.shake = cfg.shake_intensity * sin(sdt * 0.045) * f.shake_amt;
                animating = 1;
            }
            double cdt = t - caps_t;
            double ce = ease_out(cdt / CAPS_MS);
            f.caps_a = caps_state ? ce : 1 - ce;
            if (cdt < CAPS_MS) animating = 1;
            if (t - ghost_t < GHOST_MS) { f.ghost = (t - ghost_t) / GHOST_MS; animating = 1; }
            if (pwcount && t - dot_birth[pwcount - 1] < DOT_ANIM_MS + 20) animating = 1;
        } else {
            double sdt = t - shake_t;
            if (sdt < 500) f.shake_amt = 1 - sdt / 500;
            f.caps_a = caps_state;
        }
        if (unlocking) {
            double dur = anim ? UNLOCK_MS : 0, u = dur > 0 ? (t - unlock_t) / dur : 1;
            if (u >= 1) break;
            f.unlock = clamp01(u); f.fade *= 1 - u; animating = 1;
        }
        int radial = layout_id == L_RING || layout_id == L_REACTOR;
        if (radial && anim) {
            for (int i = 0; i < NKEYS; i++) if (t - key_time[i] < 850) animating = 1;
            if (cfg.idle_fps > 0) dirty = 1;               /* rings keep turning while idle */
        }
        if (layout_id == L_TERMINAL) {
            int blink = (int)((long)(t / 530) & 1);
            f.caret = blink || (pwcount && t - dot_birth[pwcount - 1] < 500);
            if (f.caret != last_caret) { last_caret = f.caret; dirty = 1; }
        }
        time_t sec = time(NULL);
        if (sec != last_sec) { last_sec = sec; dirty = 1; }

        if (dirty || animating) {
            for (int i = 0; i < nmons; i++) draw_mon(&mons[i], &f);
            if (!bg_cleared) {   /* stop the server clearing to black on Expose (= stripes) */
                for (int i = 0; i < nmons; i++) XSetWindowBackgroundPixmap(dpy, mons[i].win, None);
                bg_cleared = 1;
            }
            XFlush(dpy);
            dirty = 0;
        }

        struct pollfd pf = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pf, 1, animating ? 16 : (radial && anim && cfg.idle_fps > 0) ? 1000 / cfg.idle_fps : 200);
    }

    /* --- unlock: release grabs, tear down windows --- */
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    for (int i = 0; i < nmons; i++) {
        cairo_surface_destroy(mons[i].back);
        cairo_surface_destroy(mons[i].xs);
        cairo_surface_destroy(mons[i].bg);
        if (mons[i].bg_sharp) cairo_surface_destroy(mons[i].bg_sharp);
        XDestroyWindow(dpy, mons[i].win);
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    clear_pw(0);
    run_on_unlock();
    return 0;
}
