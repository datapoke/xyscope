/*
 *  xyscope-draw.h
 *  Core GL vertex drawing loop extracted from xyscope.mm drawPlot().
 *
 *  Emits OpenGL vertices between a caller-provided glBegin/glEnd pair.
 *  Handles per-vertex color for all display modes, color modes,
 *  and Catmull-Rom spline interpolation.
 *
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef XYSCOPE_DRAW_H
#define XYSCOPE_DRAW_H

#include "xyscope-shared.h"

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

/*
 * draw_xy_vertices -- emit GL_LINE_STRIP vertices with per-vertex color.
 *
 * The caller is responsible for glBegin(GL_LINE_STRIP) before calling
 * this function and glEnd() after it returns.  Projection setup and
 * glLineWidth are also the caller's responsibility.
 *
 * Returns the number of vertices emitted.
 */
static inline unsigned int draw_xy_vertices(
    frame_t *framebuf,
    unsigned int frames_read,
    unsigned int display_mode,
    unsigned int color_mode,
    double hue,
    double color_range,
    double scale_factor,
    unsigned int spline_steps,
    double *avg_magnitudes,   /* NULL unless DisplayFrequencyMode */
    unsigned int window_size,
    unsigned int overlap_size,
    double max_magnitude,
    double brightness,
    double velocity_dim)
{
    unsigned int vertex_count = 0;
    double h   = -1.0;
    double s   = 1.0;
    double v   = 1.0;
    double a   = 1.0;
    double r   = 1.0;
    double g   = 1.0;
    double b   = 1.0;
    double lc  = 0.0;
    double rc  = 0.0;
    double olc = 0.0;
    double orc = 0.0;
    double d   = 0.0;
    double dt  = 0.0;
    (void)dt;  /* accumulated for caller; suppress unused warning */

    /* Set initial color for standard display mode */
    if (display_mode == DisplayStandardMode) {
        HSVtoRGB(&r, &g, &b, hue, s, v);
        glColor4d(r * brightness, g * brightness, b * brightness, a);
    }

    for (unsigned int i = 0; i < frames_read; i++) {
        lc = framebuf[i].left_channel;
        rc = framebuf[i].right_channel;
        d  = hypot(lc - olc, rc - orc) / SQRT_TWO;

        /* Velocity dim: fade fast-moving segments like analog
         * phosphor.  Alpha blending with additive mode lets
         * slow parts glow bright while fast parts go transparent. */
        if (velocity_dim > 0.0)
            a = 1.0 / (1.0 + d * velocity_dim * scale_factor);
        else
            a = 1.0;

        /* Color mode accumulation */
        switch (color_mode) {
            case ColorStandardMode:
                break;
            case ColorDeltaMode:
                dt += d;
                break;
        }

        /* Display mode: compute hue */
        switch (display_mode) {
            case DisplayStandardMode:
                break;
            case DisplayRadiusMode:
                h = ((hypot(lc, rc) / SQRT_TWO)
                     * 360.0 * color_range
                     * scale_factor) + hue;
                break;
            case DisplayFrequencyMode:
                if (avg_magnitudes != NULL && max_magnitude > 0) {
                    h = map_value(
                        avg_magnitudes[i / (window_size - overlap_size)]
                            * color_range * window_size / 2,
                        0, max_magnitude, 0, 360) + hue;
                }
                break;
            default:
                break;
        }

        /* Normalize and apply color */
        if (h > -1.0 && display_mode != DisplayStandardMode) {
            h = normalizeHue(h);
        }
        if (h > -1.0) {
            HSVtoRGB(&r, &g, &b, h, s, v);
            glColor4d(r * brightness, g * brightness, b * brightness, a);
        } else if (velocity_dim > 0.0) {
            /* Standard mode with velocity dim: recompute color
             * each vertex since alpha changes per sample */
            HSVtoRGB(&r, &g, &b, hue, s, v);
            glColor4d(r * brightness, g * brightness, b * brightness, a);
        }

        /* Catmull-Rom spline interpolation */
        if (spline_steps > 1 && i > 2 && i < frames_read - 2) {
            double prev2_lc = framebuf[i-2].left_channel;
            double prev2_rc = framebuf[i-2].right_channel;
            double prev_lc  = framebuf[i-1].left_channel;
            double prev_rc  = framebuf[i-1].right_channel;
            double next_lc  = framebuf[i+1].left_channel;
            double next_rc  = framebuf[i+1].right_channel;
            double next2_lc = framebuf[i+2].left_channel;
            double next2_rc = framebuf[i+2].right_channel;

            for (double t = 0.0; t <= 1.0; t += 1.0 / (double) spline_steps) {
                double t2 = t  * t;
                double t3 = t2 * t;
                double x = 0.5 * ((2*prev_lc) + (-prev2_lc +   next_lc) * t +
                                  (2*prev2_lc - 5*prev_lc  + 4*next_lc - next2_lc) * t2 +
                                  ( -prev2_lc + 3*prev_lc  - 3*next_lc + next2_lc) * t3);
                double y = 0.5 * ((2*prev_rc) + (-prev2_rc +   next_rc) * t +
                                  (2*prev2_rc - 5*prev_rc  + 4*next_rc - next2_rc) * t2 +
                                  ( -prev2_rc + 3*prev_rc  - 3*next_rc + next2_rc) * t3);
                glVertex2d(x, y);
                vertex_count++;
                framebuf[i].left_channel  = x;
                framebuf[i].right_channel = y;
            }
        } else {
            glVertex2d(lc, rc);
            vertex_count++;
        }

        olc = lc;
        orc = rc;
    }

    return vertex_count;
}

#endif /* XYSCOPE_DRAW_H */
