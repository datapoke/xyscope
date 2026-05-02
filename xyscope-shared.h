/*
 *  xyscope-shared.h
 *  Shared types, constants, and utility functions for xyscope tools.
 *
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef XYSCOPE_SHARED_H
#define XYSCOPE_SHARED_H

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef _WIN32
/* ---- Windows compatibility layer ---- */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

static inline int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL;
    t /= 10;
    tv->tv_sec = (long)(t / 1000000ULL);
    tv->tv_usec = (long)(t % 1000000ULL);
    return 0;
}

#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
static inline int clock_gettime(int clk_id, struct timespec *ts) {
    (void)clk_id;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
    return 0;
}

#define bzero(b, len) memset((b), 0, (len))

#else
/* ---- POSIX ---- */
#include <sys/time.h>
#endif /* _WIN32 */


/* Constants */
#define DEFAULT_LINE_WIDTH    1
#define DEFAULT_PARTICLES     true
#define MAX_LINE_WIDTH        8
#define DEFAULT_FULL_SCREEN   true
#define DEFAULT_AUTO_SCALE    true
#define DEFAULT_SPLINE_STEPS  64
#define DEFAULT_COLOR_RANGE   2.0
#define DEFAULT_COLOR_RATE    0.0
#define DEFAULT_BLOOM_INTENSITY  1.2
#define DEFAULT_BLOOM_GAMMA      1.0
#define DEFAULT_BLOOM_RADIUS     2.0
#define SQRT_TWO              1.41421356237309504880


/* Audio sample type */
typedef float sample_t;

/* Stereo frame */
typedef struct _frame_t {
    sample_t left_channel;
    sample_t right_channel;
} frame_t;


/* Color modes */
typedef enum {
    ColorStandardMode = 0,
    ColorDeltaMode    = 1
} color_mode_e;

/* Display modes */
typedef enum {
    DisplayStandardMode  = 0,
    DisplayRadiusMode    = 1,
    DisplaySpectrumMode  = 2
} display_mode_e;

/* Default mode macros */
#define DEFAULT_COLOR_MODE    ColorDeltaMode
#define DEFAULT_DISPLAY_MODE  DisplaySpectrumMode


/* Preferences struct */
typedef struct _preferences_t {
    int dim[2];
    int normal_dim[2];
    int old_dim[2];
    int position[2];
    double side[4];
    double scale_factor;
    bool scale_locked;
    bool is_full_screen;
    bool auto_scale;
    unsigned int spline_steps;
    unsigned int color_mode;
    double color_range;
    double color_rate;
    unsigned int display_mode;
    unsigned int line_width;
    bool particles;
    unsigned int show_stats;
    double hue;
    double delay;
    double audio_delay;
    double display_delay;
    double brightness;
    double velocity_dim;
    double bloom_intensity;    /* 0.0 = off, default off */
    double bloom_gamma;        /* power curve on bloom: <1 soft glow, >1 punchy */
    double bloom_radius;       /* blur spread multiplier: <1 tight, >1 wide */
} preferences_t;

#define NUM_PRESETS 10
typedef struct _presets_t {
    preferences_t slot[NUM_PRESETS];
    bool          saved[NUM_PRESETS];
} presets_t;

typedef struct _app_config_t {
    char target[256];
} app_config_t;


/* ---- Utility functions ---- */

static inline double timeDiff(struct timeval a, struct timeval b) {
    return ((double)(b.tv_sec - a.tv_sec) +
            ((double)(b.tv_usec - a.tv_usec) * .000001));
}

static inline void wrapValue(double *val, double max) {
    if (*val > max) *val -= max * 2;
    if (*val <= -max) *val += max * 2;
}

static inline double normalizeHue(double h) {
    if (h > 360.0) h = fmod(h, 360.0);
    if (h < 0.0) h = 360.0 + fmod(h, 360.0);
    return h;
}

static inline void smooth(double *a, double b, double s) {
    *a = *a + (b - *a) * s;
}

static inline double map_value(double value, double fromLow, double fromHigh,
                               double toLow, double toHigh) {
    return (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow) + toLow;
}

static inline void HSVtoRGB(double *r, double *g, double *b,
                             double h, double s, double v) {
    int i;
    double f, p, q, t;

    if (s == 0) {
        *r = *g = *b = v;
        return;
    }

    if (h >= 360.0)
        h -= 360.0;

    h /= 60;
    i = (int)floor(h);
    f = h - i;
    p = v * (1 - s);
    q = v * (1 - s * f);
    t = v * (1 - s * (1 - f));

    switch (i) {
        case 0:
            *r = v; *g = t; *b = p;
            break;
        case 1:
            *r = q; *g = v; *b = p;
            break;
        case 2:
            *r = p; *g = v; *b = t;
            break;
        case 3:
            *r = p; *g = q; *b = v;
            break;
        case 4:
            *r = t; *g = p; *b = v;
            break;
        default:
            *r = v; *g = p; *b = q;
            break;
    }
}

static inline void RGBtoHSV(double r, double g, double b,
                             double *h, double *s, double *v) {
    double max_c = fmax(r, fmax(g, b));
    double min_c = fmin(r, fmin(g, b));
    double delta = max_c - min_c;

    *v = max_c;
    *s = (max_c > 0.0) ? (delta / max_c) : 0.0;

    if (delta <= 0.0) {
        *h = 0.0;
        return;
    }
    if (max_c == r) {
        *h = 60.0 * fmod((g - b) / delta + 6.0, 6.0);
    } else if (max_c == g) {
        *h = 60.0 * (((b - r) / delta) + 2.0);
    } else {
        *h = 60.0 * (((r - g) / delta) + 4.0);
    }
    if (*h < 0.0) *h += 360.0;
}

/* ---- Config file (INI format) ---- */

#define CONFIG_FILENAME "xyscope.conf"

static inline const char *get_config_path(void) {
    static char path[512];
    char confdir[480];
    const char *dir;
#ifdef _WIN32
    dir = getenv("APPDATA");
    if (!dir) dir = ".";
    snprintf(confdir, sizeof(confdir), "%s\\XYScope", dir);
    CreateDirectoryA(confdir, NULL);
#else
    dir = getenv("HOME");
    if (!dir) dir = ".";
    snprintf(confdir, sizeof(confdir), "%s/.config", dir);
    mkdir(confdir, 0755);
    snprintf(confdir, sizeof(confdir), "%s/.config/xyscope", dir);
    mkdir(confdir, 0755);
#endif
    snprintf(path, sizeof(path), "%s%c%s", confdir,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             CONFIG_FILENAME);
    return path;
}

static inline void write_prefs_section(FILE *fp, const char *section,
                                       const preferences_t *p) {
    fprintf(fp, "[%s]\n", section);
    fprintf(fp, "dim=%d,%d\n",             p->dim[0], p->dim[1]);
    fprintf(fp, "normal_dim=%d,%d\n",      p->normal_dim[0], p->normal_dim[1]);
    fprintf(fp, "old_dim=%d,%d\n",         p->old_dim[0], p->old_dim[1]);
    fprintf(fp, "position=%d,%d\n",        p->position[0], p->position[1]);
    fprintf(fp, "side=%.17g,%.17g,%.17g,%.17g\n",
            p->side[0], p->side[1], p->side[2], p->side[3]);
    fprintf(fp, "scale_factor=%.17g\n",    p->scale_factor);
    fprintf(fp, "scale_locked=%d\n",       p->scale_locked);
    fprintf(fp, "is_full_screen=%d\n",     p->is_full_screen);
    fprintf(fp, "auto_scale=%d\n",         p->auto_scale);
    fprintf(fp, "spline_steps=%u\n",       p->spline_steps);
    fprintf(fp, "color_mode=%u\n",         p->color_mode);
    fprintf(fp, "color_range=%.17g\n",     p->color_range);
    fprintf(fp, "color_rate=%.17g\n",      p->color_rate);
    fprintf(fp, "display_mode=%u\n",       p->display_mode);
    fprintf(fp, "line_width=%u\n",         p->line_width);
    fprintf(fp, "particles=%d\n",          p->particles);
    fprintf(fp, "show_stats=%u\n",         p->show_stats);
    fprintf(fp, "hue=%.17g\n",             p->hue);
    fprintf(fp, "delay=%.17g\n",           p->delay);
    fprintf(fp, "audio_delay=%.17g\n",     p->audio_delay);
    fprintf(fp, "display_delay=%.17g\n",   p->display_delay);
    fprintf(fp, "brightness=%.17g\n",      p->brightness);
    fprintf(fp, "velocity_dim=%.17g\n",    p->velocity_dim);
    fprintf(fp, "bloom_intensity=%.17g\n", p->bloom_intensity);
    fprintf(fp, "bloom_gamma=%.17g\n",    p->bloom_gamma);
    fprintf(fp, "bloom_radius=%.17g\n",   p->bloom_radius);
    fprintf(fp, "\n");
}

static inline bool save_config(const preferences_t *prefs,
                               const presets_t *presets,
                               const app_config_t *app) {
    const char *path = get_config_path();
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    write_prefs_section(fp, "settings", prefs);
    if (app && app->target[0])
        fprintf(fp, "target=%s\n\n", app->target);
    for (int i = 0; i < NUM_PRESETS; i++) {
        if (presets->saved[i]) {
            char section[16];
            snprintf(section, sizeof(section), "preset.%d", i);
            write_prefs_section(fp, section, &presets->slot[i]);
        }
    }
    fclose(fp);
    return true;
}

static inline void parse_prefs_key(preferences_t *p,
                                   const char *key, const char *val) {
    if      (!strcmp(key, "dim"))
        sscanf(val, "%d,%d", &p->dim[0], &p->dim[1]);
    else if (!strcmp(key, "normal_dim"))
        sscanf(val, "%d,%d", &p->normal_dim[0], &p->normal_dim[1]);
    else if (!strcmp(key, "old_dim"))
        sscanf(val, "%d,%d", &p->old_dim[0], &p->old_dim[1]);
    else if (!strcmp(key, "position"))
        sscanf(val, "%d,%d", &p->position[0], &p->position[1]);
    else if (!strcmp(key, "side"))
        sscanf(val, "%lf,%lf,%lf,%lf",
               &p->side[0], &p->side[1], &p->side[2], &p->side[3]);
    else if (!strcmp(key, "scale_factor"))    p->scale_factor    = atof(val);
    else if (!strcmp(key, "scale_locked"))    p->scale_locked    = atoi(val);
    else if (!strcmp(key, "is_full_screen"))  p->is_full_screen  = atoi(val);
    else if (!strcmp(key, "auto_scale"))      p->auto_scale      = atoi(val);
    else if (!strcmp(key, "spline_steps"))    p->spline_steps    = atoi(val);
    else if (!strcmp(key, "color_mode"))      p->color_mode      = atoi(val);
    else if (!strcmp(key, "color_range"))     p->color_range     = atof(val);
    else if (!strcmp(key, "color_rate"))      p->color_rate      = atof(val);
    else if (!strcmp(key, "display_mode"))    p->display_mode    = atoi(val);
    else if (!strcmp(key, "line_width"))      p->line_width      = atoi(val);
    else if (!strcmp(key, "particles"))       p->particles       = atoi(val);
    else if (!strcmp(key, "show_stats"))      p->show_stats      = atoi(val);
    else if (!strcmp(key, "hue"))             p->hue             = atof(val);
    else if (!strcmp(key, "delay"))           p->delay           = atof(val);
    else if (!strcmp(key, "audio_delay"))     p->audio_delay     = atof(val);
    else if (!strcmp(key, "display_delay"))   p->display_delay   = atof(val);
    else if (!strcmp(key, "brightness"))      p->brightness      = atof(val);
    else if (!strcmp(key, "velocity_dim"))    p->velocity_dim    = atof(val);
    else if (!strcmp(key, "bloom_intensity")) p->bloom_intensity = atof(val);
    else if (!strcmp(key, "bloom_gamma"))     p->bloom_gamma     = atof(val);
    else if (!strcmp(key, "bloom_radius"))   p->bloom_radius    = atof(val);
}

static inline bool load_config(preferences_t *prefs, presets_t *presets,
                               app_config_t *app) {
    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    char line[256];
    preferences_t *current_prefs = NULL;
    bool in_settings = false;
    int preset_idx = -1;

    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        /* skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* section header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            const char *section = line + 1;
            in_settings = false;
            if (!strcmp(section, "settings")) {
                current_prefs = prefs;
                in_settings = true;
                preset_idx = -1;
            }
            else if (!strncmp(section, "preset.", 7)) {
                preset_idx = atoi(section + 7);
                if (preset_idx >= 0 && preset_idx < NUM_PRESETS) {
                    current_prefs = &presets->slot[preset_idx];
                    presets->saved[preset_idx] = true;
                } else {
                    current_prefs = NULL;
                    preset_idx = -1;
                }
            }
            else {
                current_prefs = NULL;
                preset_idx = -1;
            }
            continue;
        }

        /* key=value */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (in_settings && app && !strcmp(key, "target"))
            snprintf(app->target, sizeof(app->target), "%s", val);
        else if (current_prefs)
            parse_prefs_key(current_prefs, key, val);
    }

    fclose(fp);
    return true;
}

#endif /* XYSCOPE_SHARED_H */
