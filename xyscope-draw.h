/*
 *  xyscope-draw.h
 *  Core GL vertex drawing loop extracted from xyscope.mm drawPlot().
 *
 *  Pre-computes all vertex positions and colors into flat arrays, then
 *  draws with a single glDrawArrays call — eliminates the per-vertex
 *  glVertex2d/glColor4d overhead of legacy immediate mode. The inner
 *  spline loop is pure polynomial evaluation with no GL calls, which
 *  lets the compiler auto-vectorize with -O3.
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
 * draw_xy_vertices -- fill vertex+color arrays and draw with glDrawArrays.
 *
 * Replaces the legacy glBegin/glVertex2d/glColor4d/glEnd path with a
 * single batched draw call. The caller should NOT wrap this in
 * glBegin/glEnd — the function manages its own GL state.
 *
 * Returns the number of vertices drawn.
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
    double velocity_dim,
    double *spectrum_colors,  /* NULL unless DisplaySpectrumMode */
    bool particles = false,
    bool gpu_color = false)   /* true = shader handles HSV; pass raw RGB */
{
    /* Reusable vertex/color buffers — grown as needed, never shrunk.
     * Avoids per-frame malloc/free churn at high spline counts. */
    static float *s_verts  = NULL;
    static float *s_colors = NULL;
    static unsigned int s_alloc = 0;

    unsigned int max_verts = frames_read * (spline_steps > 1 ? spline_steps + 1 : 1);
    if (max_verts > s_alloc) {
        free(s_verts);
        free(s_colors);
        s_verts  = (float *)malloc(max_verts * 2 * sizeof(float));
        s_colors = (float *)malloc(max_verts * 4 * sizeof(float));
        s_alloc  = max_verts;
    }
    float *verts  = s_verts;
    float *colors = s_colors;
    unsigned int n = 0;

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
    (void)dt;

    unsigned int stride = (window_size > overlap_size) ? (window_size - overlap_size) : 1;

    /* Pre-compute initial color for standard display mode */
    if (display_mode == DisplayStandardMode) {
        HSVtoRGB(&r, &g, &b, hue, s, v);
    }

    for (unsigned int i = 0; i < frames_read; i++) {
        lc = framebuf[i].left_channel;
        rc = framebuf[i].right_channel;
        d  = hypot(lc - olc, rc - orc) / SQRT_TWO;

        /* Velocity dim */
        if (velocity_dim > 0.0)
            a = 1.0 / (1.0 + d * 10.0 * velocity_dim * scale_factor);
        else
            a = 1.0;

        /* Color mode accumulation */
        switch (color_mode) {
            case ColorStandardMode: break;
            case ColorDeltaMode:    dt += d; break;
        }

        /* Display mode: compute per-vertex color */
        bool color_set = false;
        switch (display_mode) {
            case DisplayStandardMode:
                break;
            case DisplayRadiusMode:
                h = ((hypot(lc, rc) / SQRT_TWO)
                     * 360.0 * color_range * scale_factor) + hue;
                break;
            case DisplayFrequencyMode:
                if (avg_magnitudes != NULL && max_magnitude > 0) {
                    h = map_value(
                        avg_magnitudes[i / stride]
                            * color_range * window_size / 2,
                        0, max_magnitude, 0, 360) + hue;
                }
                break;
            case DisplaySpectrumMode:
                if (spectrum_colors != NULL) {
                    unsigned int w = i / stride;
                    if (gpu_color) {
                        /* Shader does HSV boost/lift + brightness;
                         * just pass raw spectrum RGB through. */
                        r = spectrum_colors[w * 3 + 0];
                        g = spectrum_colors[w * 3 + 1];
                        b = spectrum_colors[w * 3 + 2];
                    } else {
                        /* CPU fallback */
                        double sr = spectrum_colors[w * 3 + 0];
                        double sg = spectrum_colors[w * 3 + 1];
                        double sb = spectrum_colors[w * 3 + 2];
                        double sh, ss, sv;
                        RGBtoHSV(sr, sg, sb, &sh, &ss, &sv);
                        ss *= 1.5;
                        if (ss > 1.0) ss = 1.0;
                        sv = sv * 0.5 + 0.5;
                        HSVtoRGB(&r, &g, &b, sh, ss, sv);
                    }
                    color_set = true;
                }
                break;
            default:
                break;
        }

        if (!color_set) {
            if (h > -1.0 && display_mode != DisplayStandardMode) {
                h = normalizeHue(h);
            }
            if (h > -1.0) {
                HSVtoRGB(&r, &g, &b, h, s, v);
            } else if (velocity_dim > 0.0) {
                HSVtoRGB(&r, &g, &b, hue, s, v);
            }
        }

        /* Final color for every vertex emitted from this audio sample.
         * When gpu_color is active the shader multiplies by brightness,
         * so we pass raw RGB here to avoid double-applying it. */
        float cr, cg, cb;
        if (gpu_color && display_mode == DisplaySpectrumMode) {
            cr = (float)r;  cg = (float)g;  cb = (float)b;
        } else {
            cr = (float)(r * brightness);
            cg = (float)(g * brightness);
            cb = (float)(b * brightness);
        }
        float ca = (float)a;

        /* Catmull-Rom spline interpolation into the vertex array.
         * Coefficients are precomputed per audio sample so the inner
         * loop is pure polynomial evaluation — no GL calls, no data
         * dependencies between iterations, auto-vectorizes with -O3. */
        if (spline_steps > 1 && i > 2 && i < frames_read - 2) {
            double P0x = framebuf[i-2].left_channel;
            double P0y = framebuf[i-2].right_channel;
            double P1x = framebuf[i-1].left_channel;
            double P1y = framebuf[i-1].right_channel;
            double P2x = framebuf[i+1].left_channel;
            double P2y = framebuf[i+1].right_channel;
            double P3x = framebuf[i+2].left_channel;
            double P3y = framebuf[i+2].right_channel;

            /* Horner-form coefficients for x(t) and y(t) */
            double ax0 = 2.0*P1x;
            double ax1 = -P0x + P2x;
            double ax2 = 2.0*P0x - 5.0*P1x + 4.0*P2x - P3x;
            double ax3 = -P0x + 3.0*P1x - 3.0*P2x + P3x;
            double ay0 = 2.0*P1y;
            double ay1 = -P0y + P2y;
            double ay2 = 2.0*P0y - 5.0*P1y + 4.0*P2y - P3y;
            double ay3 = -P0y + 3.0*P1y - 3.0*P2y + P3y;

            double inv_steps = 1.0 / (double)spline_steps;
            for (unsigned int step = 0; step <= spline_steps; step++) {
                double t  = (double)step * inv_steps;
                double t2 = t * t;
                double t3 = t2 * t;
                double x = 0.5 * (ax0 + ax1*t + ax2*t2 + ax3*t3);
                double y = 0.5 * (ay0 + ay1*t + ay2*t2 + ay3*t3);
                verts[n*2]     = (float)x;
                verts[n*2 + 1] = (float)y;
                colors[n*4]     = cr;
                colors[n*4 + 1] = cg;
                colors[n*4 + 2] = cb;
                colors[n*4 + 3] = ca;
                n++;
                /* Write back so the next iteration's P1 (framebuf[i-1])
                 * picks up where this segment ended — essential for
                 * continuity between adjacent spline segments. */
                framebuf[i].left_channel  = x;
                framebuf[i].right_channel = y;
            }
        } else {
            verts[n*2]     = (float)lc;
            verts[n*2 + 1] = (float)rc;
            colors[n*4]     = cr;
            colors[n*4 + 1] = cg;
            colors[n*4 + 2] = cb;
            colors[n*4 + 3] = ca;
            n++;
        }

        olc = lc;
        orc = rc;
    }

    /* Batch draw — one GL call replaces ~N individual glVertex2d calls */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_FLOAT, 0, colors);
    glDrawArrays(particles ? GL_POINTS : GL_LINE_STRIP, 0, n);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    return n;
}

#endif /* XYSCOPE_DRAW_H */
