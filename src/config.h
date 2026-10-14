#ifndef MEL_CONFIG_H
#define MEL_CONFIG_H
#include <stddef.h>

typedef struct { double r, g, b, a; } color_t;

typedef struct {
    /* general */
    char auth_service[64]; int grace_period; int ignore_empty_password; int show_failed_attempts;
    /* background */
    char background[16]; char background_image[512]; color_t background_color;
    int blur_strength; double dim_strength; color_t gradient_start, gradient_end;
    /* panel */
    int panel_width, panel_height, panel_radius; double panel_opacity; color_t panel_color;
    int panel_border; color_t panel_border_color; int panel_shadow;
    /* clock */
    int show_clock; char clock_format[64], date_format[64]; int clock_size, date_size;
    color_t clock_color, date_color;
    /* input */
    char password_char[8]; int input_radius; color_t input_color, input_text_color;
    char placeholder[64]; int show_caps_lock; color_t caps_lock_color;
    /* typography */
    char font[64]; int font_size; char clock_font[64];
    /* animation */
    int fade_in_duration; int shake_intensity; int enable_animations;
    /* behavior */
    int bell_on_fail; int timeout; char message[128]; char on_unlock[512];
    char theme[64]; double scale;
} config_t;

void config_defaults(config_t *c);
int  config_load(config_t *c, const char *path);          /* 0 = ok */
int  config_find_path(char *out, size_t n);               /* 0 = found */
int  config_find_theme(const char *name, char *out, size_t n);
void config_expand_tilde(const char *in, char *out, size_t n);
int  config_parse_color(const char *s, color_t *c);       /* 0 = ok */
#endif
