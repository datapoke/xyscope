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
#include <stdbool.h>

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
#define DEFAULT_PREF_FILE     ".xyscope.pref"
#define DEFAULT_LINE_WIDTH    1
#define MAX_LINE_WIDTH        8
#define DEFAULT_FULL_SCREEN   true
#define DEFAULT_AUTO_SCALE    true
#define DEFAULT_SPLINE_STEPS  128
#define DEFAULT_COLOR_RANGE   1.0
#define DEFAULT_COLOR_RATE    0.0
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
    DisplayFrequencyMode = 2
} display_mode_e;

/* Default mode macros */
#define DEFAULT_COLOR_MODE    ColorDeltaMode
#define DEFAULT_DISPLAY_MODE  DisplayFrequencyMode


/* Preferences struct -- binary compatible with .xyscope.pref file.
 * Field order and types matter for file I/O. */
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
    unsigned int show_stats;
    double hue;
    double delay;
    double audio_delay;
    double display_delay;
    double brightness;
    double velocity_dim;
} preferences_t;


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
    i = (int)floorf(h);
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

#endif /* XYSCOPE_SHARED_H */
