/* mel - a pretty, customizable X11 screen locker.
 * Layout: one override-redirect window per monitor, Cairo drawing, PAM auth. */
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

#define VERSION "0.1.0"
void config_set(config_t *c, const char *key, const char *val);

typedef struct { int x, y, w, h; Window win; cairo_surface_t *bg, *xs, *back; } mon_t;

static config_t cfg;
static Display *dpy;
static int scr;
static Window root;
static double ui_scale = 1.0;
static mon_t *mons;
static int nmons;
static char pwbuf[256];
static size_t pwlen;
static int failed_attempts, authenticating;

/* ---------- small helpers ---------- */

static double now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void die(const char *m) { fprintf(stderr, "mel: %s\n", m); exit(1); }
static void rgba(cairo_t *cr, color_t c, double mul) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a * mul); }

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

static void text_center(cairo_t *cr, const char *s, double cx, double base, double size,
                        const char *font, int bold, color_t c, double alpha)
{
    cairo_text_extents_t e;
    cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    rgba(cr, c, alpha);
    cairo_move_to(cr, cx - (e.width / 2 + e.x_bearing), base);
    cairo_show_text(cr, s);
}

/* ---------- background generation ---------- */

/* Fast approximate gaussian: 3 box-blur passes. Alpha is forced opaque. */
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
    m->bg = s;
}

/* ---------- drawing ---------- */

static int caps_on(void)
{
    unsigned n = 0;
    XkbGetIndicatorState(dpy, XkbUseCoreKbd, &n);
    return n & 1;
}

static size_t utf8_count(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* fade: 0..1 overall opacity, pop: 0..1 panel scale-in progress, shake: x-offset in ui units */
static void draw_mon(mon_t *m, double fade, double pop, double shake, double shake_amt)
{
    cairo_t *cr = cairo_create(m->back);   /* offscreen: nothing half-drawn is ever visible */
    const double W = cfg.panel_width, H = cfg.panel_height;
    char buf[256];
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);

    cairo_push_group(cr);
    cairo_set_source_surface(cr, m->bg, 0, 0);
    cairo_paint(cr);

    cairo_save(cr);
    double s = 0.92 + 0.08 * pop;
    cairo_translate(cr, m->w / 2.0, m->h / 2.0);
    cairo_scale(cr, s * ui_scale, s * ui_scale);
    cairo_translate(cr, -W / 2, -H / 2);

    /* soft shadow: stacked translucent rounded rects */
    if (cfg.panel_shadow)
        for (int i = 0; i < 14; i++) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.032 * (1.0 - i / 14.0));
            rrect(cr, -i * 2, 8 - i * 2, W + i * 4, H + i * 4, cfg.panel_radius + i * 2);
            cairo_fill(cr);
        }
    rrect(cr, 0, 0, W, H, cfg.panel_radius);
    rgba(cr, cfg.panel_color, cfg.panel_opacity);
    cairo_fill_preserve(cr);
    if (cfg.panel_border) { rgba(cr, cfg.panel_border_color, 1); cairo_set_line_width(cr, 1); cairo_stroke(cr); }
    else cairo_new_path(cr);

    /* clock + date */
    double y = 24 + cfg.clock_size * 0.8;
    if (cfg.show_clock) {
        strftime(buf, sizeof buf, cfg.clock_format, &tm);
        text_center(cr, buf, W / 2, y, cfg.clock_size, cfg.clock_font, 1, cfg.clock_color, 1);
        strftime(buf, sizeof buf, cfg.date_format, &tm);
        text_center(cr, buf, W / 2, y + cfg.date_size + 14, cfg.date_size, cfg.font, 0, cfg.date_color, 1);
    }

    /* username */
    struct passwd *pw = getpwuid(getuid());
    double iy = H - 78, ih = 42, ix = 40, iw = W - 80;
    text_center(cr, pw ? pw->pw_name : "", W / 2, iy - 12, cfg.font_size * 0.95, cfg.font, 0,
                cfg.input_text_color, 0.7);

    /* input field (shakes horizontally on failure) */
    cairo_save(cr);
    cairo_translate(cr, shake, 0);
    rrect(cr, ix, iy, iw, ih, cfg.input_radius);
    rgba(cr, cfg.input_color, 1);
    cairo_fill_preserve(cr);
    if (shake_amt > 0.02) {
        rgba(cr, cfg.caps_lock_color, shake_amt); cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
    } else cairo_new_path(cr);

    size_t n = utf8_count(pwbuf, pwlen);
    if (n == 0) {
        text_center(cr, cfg.placeholder, W / 2, iy + ih / 2 + cfg.font_size * 0.35, cfg.font_size,
                    cfg.font, 0, cfg.input_text_color, 0.45);
    } else {
        char dots[512] = ""; size_t cl = strlen(cfg.password_char);
        size_t shown = n > 28 ? 28 : n;
        for (size_t i = 0; i < shown && strlen(dots) + cl < sizeof dots; i++) strcat(dots, cfg.password_char);
        text_center(cr, dots, W / 2, iy + ih / 2 + cfg.font_size * 0.45, cfg.font_size * 1.1,
                    cfg.font, 0, cfg.input_text_color, 1);
    }
    cairo_restore(cr);

    /* status line */
    const double sy = H - 16, ss = cfg.font_size * 0.85;
    if (authenticating)
        text_center(cr, "Authenticating\xe2\x80\xa6", W / 2, sy, ss, cfg.font, 0, cfg.input_text_color, 0.7);
    else if (cfg.show_caps_lock && caps_on())
        text_center(cr, "Caps Lock is on", W / 2, sy, ss, cfg.font, 1, cfg.caps_lock_color, 1);
    else if (cfg.show_failed_attempts && failed_attempts > 0) {
        snprintf(buf, sizeof buf, "%d failed attempt%s", failed_attempts, failed_attempts == 1 ? "" : "s");
        text_center(cr, buf, W / 2, sy, ss, cfg.font, 0, cfg.caps_lock_color, 0.85);
    } else if (cfg.message[0])
        text_center(cr, cfg.message, W / 2, sy, ss, cfg.font, 0, cfg.input_text_color, 0.6);

    cairo_restore(cr);
    cairo_pattern_t *grp = cairo_pop_group(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);                       /* black base so fade goes to/from black */
    cairo_set_source(cr, grp);
    cairo_paint_with_alpha(cr, fade);
    cairo_pattern_destroy(grp);
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

    /* grab pointer + keyboard (retry: another client may briefly hold them) */
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

static void clear_pw(void) { explicit_bzero(pwbuf, sizeof pwbuf); pwlen = 0; }

static void run_on_unlock(void)
{
    if (!cfg.on_unlock[0]) return;
    if (fork() == 0) { setsid(); execl("/bin/sh", "sh", "-c", cfg.on_unlock, (char *)NULL); _exit(127); }
}

static void usage(void)
{
    puts("usage: mel [OPTIONS]\n"
         "  -c, --config <file>       use custom config\n"
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
        { "config", 1, 0, 'c' }, { "image", 1, 0, 'i' }, { "background", 1, 0, 'b' },
        { "blur", 1, 0, 1 }, { "no-fade", 0, 0, 2 }, { "version", 0, 0, 3 }, { "help", 0, 0, 'h' }, { 0 } };
    char *o_image = NULL, *o_bg = NULL, *o_blur = NULL; int o_nofade = 0, ch;
    while ((ch = getopt_long(argc, argv, "c:i:b:h", lo, NULL)) != -1) switch (ch) {
        case 'c': snprintf(path, sizeof path, "%s", optarg); break;
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

    if (!(dpy = XOpenDisplay(NULL))) die("cannot open display");
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);

    /* HiDPI: explicit scale, else Xft.dpi / 96 */
    if (cfg.scale > 0) ui_scale = cfg.scale;
    else { char *dpi = XGetDefault(dpy, "Xft", "dpi"); if (dpi && atof(dpi) > 96) ui_scale = atof(dpi) / 96.0; }

    /* best-effort hardening: don't let the OOM killer pick us, ignore polite kill signals */
    FILE *oom = fopen("/proc/self/oom_score_adj", "w");
    if (oom) { fputs("-1000\n", oom); fclose(oom); }
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGQUIT, SIG_IGN);

    struct passwd *pw = getpwuid(getuid());
    if (!pw) die("cannot determine user");
    char user[128]; snprintf(user, sizeof user, "%s", pw->pw_name);

    create_windows();

    double t0 = now_ms(), shake_t = -1e9, unlock_t = 0;
    time_t last_sec = 0;
    int dirty = 1, unlocking = 0, bg_cleared = 0;
    int anim = cfg.enable_animations;

    for (;;) {
        /* --- events --- */
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);
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
                    for (int i = 0; i < nmons; i++) draw_mon(&mons[i], 1, 1, 0, 0);
                    XFlush(dpy);
                    int ok = auth_check(cfg.auth_service, user, pwbuf);
                    clear_pw();
                    authenticating = 0;
                    XEvent junk;                      /* drop keys typed while PAM was busy */
                    while (XCheckMaskEvent(dpy, KeyPressMask | KeyReleaseMask, &junk));
                    if (ok) { unlocking = 1; unlock_t = now_ms(); }
                    else {
                        failed_attempts++; shake_t = now_ms();
                        if (cfg.bell_on_fail) XkbBell(dpy, None, 0, None);
                    }
                } else if (ks == XK_Escape) clear_pw();
                else if (ks == XK_BackSpace) {
                    if (pwlen) { do pwlen--; while (pwlen && ((unsigned char)pwbuf[pwlen] & 0xC0) == 0x80);
                                 pwbuf[pwlen] = 0; }
                } else if (ev.xkey.state & ControlMask) {
                    if (ks == XK_u) clear_pw();
                } else if (n > 0 && !iscntrl((unsigned char)buf[0]) && pwlen + n < sizeof pwbuf - 1) {
                    memcpy(pwbuf + pwlen, buf, n); pwlen += n; pwbuf[pwlen] = 0;
                }
            }
        }

        /* --- animation state --- */
        double t = now_ms(), fade = 1, pop = 1, shake = 0, shake_amt = 0;
        int animating = 0;
        if (anim && cfg.fade_in_duration > 0) {
            double f = clamp01((t - t0) / cfg.fade_in_duration);
            if (f < 1) animating = 1;
            fade = f; pop = 1 - pow(1 - f, 3);
        }
        double sdt = t - shake_t;
        if (anim && sdt < 500) {
            double decay = exp(-sdt / 170);
            shake = cfg.shake_intensity * sin(sdt * 0.045) * decay;
            shake_amt = decay; animating = 1;
        } else if (!anim && sdt < 500) shake_amt = 1 - sdt / 500;
        if (unlocking) {
            double dur = anim ? 180 : 0, f = dur > 0 ? (t - unlock_t) / dur : 1;
            if (f >= 1) break;
            fade *= 1 - f; animating = 1;
        }
        time_t sec = time(NULL);
        if (sec != last_sec) { last_sec = sec; dirty = 1; }

        if (dirty || animating) {
            for (int i = 0; i < nmons; i++) draw_mon(&mons[i], fade, pop, shake, shake_amt);
            if (!bg_cleared) {   /* stop the server clearing to black on Expose (= stripes) */
                for (int i = 0; i < nmons; i++) XSetWindowBackgroundPixmap(dpy, mons[i].win, None);
                bg_cleared = 1;
            }
            XFlush(dpy);
            dirty = 0;
        }

        struct pollfd pf = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pf, 1, animating ? 16 : 200);
    }

    /* --- unlock: release grabs, tear down windows --- */
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    for (int i = 0; i < nmons; i++) {
        cairo_surface_destroy(mons[i].back);
        cairo_surface_destroy(mons[i].xs);
        cairo_surface_destroy(mons[i].bg);
        XDestroyWindow(dpy, mons[i].win);
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    clear_pw();
    run_on_unlock();
    return 0;
}
