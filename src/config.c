/* mel - config parsing: "key = value" lines, '#' comments, [sections] ignored */
#include "config.h"
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { T_STR, T_INT, T_DBL, T_BOOL, T_COL };
typedef struct { const char *key; int type; size_t off, size; } opt_t;
#define O(k, t, f) { k, t, offsetof(config_t, f), sizeof(((config_t *)0)->f) }

static const opt_t opts[] = {
    O("auth_service", T_STR, auth_service), O("grace_period", T_INT, grace_period),
    O("ignore_empty_password", T_BOOL, ignore_empty_password),
    O("show_failed_attempts", T_BOOL, show_failed_attempts),
    O("background", T_STR, background), O("background_image", T_STR, background_image),
    O("background_color", T_COL, background_color), O("blur_strength", T_INT, blur_strength),
    O("dim_strength", T_DBL, dim_strength), O("gradient_start", T_COL, gradient_start),
    O("gradient_end", T_COL, gradient_end),
    O("panel_width", T_INT, panel_width), O("panel_height", T_INT, panel_height),
    O("panel_radius", T_INT, panel_radius), O("panel_opacity", T_DBL, panel_opacity),
    O("panel_color", T_COL, panel_color), O("panel_border", T_BOOL, panel_border),
    O("panel_border_color", T_COL, panel_border_color), O("panel_shadow", T_BOOL, panel_shadow),
    O("show_clock", T_BOOL, show_clock), O("clock_format", T_STR, clock_format),
    O("date_format", T_STR, date_format), O("clock_size", T_INT, clock_size),
    O("date_size", T_INT, date_size), O("clock_color", T_COL, clock_color),
    O("date_color", T_COL, date_color),
    O("password_char", T_STR, password_char), O("input_radius", T_INT, input_radius),
    O("input_color", T_COL, input_color), O("input_text_color", T_COL, input_text_color),
    O("placeholder", T_STR, placeholder), O("show_caps_lock", T_BOOL, show_caps_lock),
    O("caps_lock_color", T_COL, caps_lock_color),
    O("font", T_STR, font), O("font_size", T_INT, font_size), O("clock_font", T_STR, clock_font),
    O("fade_in_duration", T_INT, fade_in_duration), O("shake_intensity", T_INT, shake_intensity),
    O("enable_animations", T_BOOL, enable_animations),
    O("bell_on_fail", T_BOOL, bell_on_fail), O("timeout", T_INT, timeout),
    O("message", T_STR, message), O("on_unlock", T_STR, on_unlock),
    O("theme", T_STR, theme), O("scale", T_DBL, scale),
};

int config_parse_color(const char *s, color_t *c)
{
    unsigned r, g, b, a = 255;
    if (*s == '#') s++;
    size_t n = strlen(s);
    if (n == 6 && sscanf(s, "%2x%2x%2x", &r, &g, &b) == 3) {}
    else if (n == 8 && sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4) {}
    else return -1;
    c->r = r / 255.0; c->g = g / 255.0; c->b = b / 255.0; c->a = a / 255.0;
    return 0;
}

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->auth_service, "login");
    c->show_failed_attempts = 1;
    strcpy(c->background, "blur");
    config_parse_color("#1a1a2e", &c->background_color);
    c->blur_strength = 12; c->dim_strength = 0.55;
    config_parse_color("#0f0c29", &c->gradient_start);
    config_parse_color("#302b63", &c->gradient_end);
    c->panel_width = 420; c->panel_height = 280; c->panel_radius = 18; c->panel_opacity = 0.82;
    config_parse_color("#0f0f13", &c->panel_color);
    c->panel_border = 1; config_parse_color("#ffffff22", &c->panel_border_color);
    c->panel_shadow = 1;
    c->show_clock = 1; strcpy(c->clock_format, "%H:%M"); strcpy(c->date_format, "%A, %d %B");
    c->clock_size = 64; c->date_size = 18;
    config_parse_color("#ffffff", &c->clock_color); config_parse_color("#ffffffbb", &c->date_color);
    strcpy(c->password_char, "\xe2\x80\xa2"); c->input_radius = 10;
    config_parse_color("#ffffff15", &c->input_color);
    config_parse_color("#ffffff", &c->input_text_color);
    strcpy(c->placeholder, "Enter password"); c->show_caps_lock = 1;
    config_parse_color("#ff6b6b", &c->caps_lock_color);
    strcpy(c->font, "Inter"); c->font_size = 15; strcpy(c->clock_font, "Inter");
    c->fade_in_duration = 280; c->shake_intensity = 12; c->enable_animations = 1;
    c->bell_on_fail = 1;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void set_opt(config_t *c, const opt_t *o, char *v)
{
    char *p = (char *)c + o->off;
    switch (o->type) {
    case T_STR:
        if (o->key[0] == 'b' && !strcmp(o->key, "background_image")) config_expand_tilde(v, p, o->size);
        else snprintf(p, o->size, "%s", v);
        break;
    case T_INT: *(int *)p = atoi(v); break;
    case T_DBL: *(double *)p = atof(v); break;
    case T_BOOL: *(int *)p = !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
                             !strcasecmp(v, "on") || !strcmp(v, "1"); break;
    case T_COL: { color_t col; if (!config_parse_color(v, &col)) *(color_t *)p = col; break; }
    }
}

/* Apply one "key = value" assignment (also used for CLI overrides). */
static void apply(config_t *c, char *key, char *val)
{
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++)
        if (!strcmp(opts[i].key, key)) { set_opt(c, &opts[i], val); return; }
    fprintf(stderr, "mel: unknown config key '%s'\n", key);
}

int config_load(config_t *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == '[' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s), *val = eq + 1;
        /* inline comment: '#' preceded by whitespace (so "#rrggbb" values survive) */
        val = trim(val);
        for (char *q = val + 1; *q; q++)
            if (*q == '#' && isspace((unsigned char)q[-1])) { *q = 0; break; }
        val = trim(val);
        size_t n = strlen(val);
        if (n >= 2 && (val[0] == '"' || val[0] == '\'') && val[n - 1] == val[0]) { val[n - 1] = 0; val++; }
        apply(c, key, val);
    }
    fclose(f);
    return 0;
}

void config_expand_tilde(const char *in, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home) snprintf(out, n, "%s%s", home, in + 1);
    else snprintf(out, n, "%s", in);
}

int config_find_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) { snprintf(out, n, "%s/mel/config", xdg); if (!access(out, R_OK)) return 0; }
    if (home) { snprintf(out, n, "%s/.config/mel/config", home); if (!access(out, R_OK)) return 0; }
    snprintf(out, n, "/etc/mel.conf");
    return access(out, R_OK);
}

int config_find_theme(const char *name, char *out, size_t n)
{
    const char *home = getenv("HOME");
    if (home) { snprintf(out, n, "%s/.config/mel/themes/%s", home, name); if (!access(out, R_OK)) return 0; }
    snprintf(out, n, PREFIX "/share/mel/themes/%s", name);
    return access(out, R_OK);
}

/* "key=value" from --set style CLI use */
void config_set(config_t *c, const char *key, const char *val);
void config_set(config_t *c, const char *key, const char *val)
{
    char k[64], v[512];
    snprintf(k, sizeof k, "%s", key); snprintf(v, sizeof v, "%s", val);
    apply(c, k, v);
}
