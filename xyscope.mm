/*
 *  xyscope.cpp
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  Some code copyright (c) Luke Campagnola <lcampagn@mines.edu>
 *  Some code copyright (c) 2001 Paul Davis
 *  Some code copyright (c) 2003 Jack O'Quin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA
 *
 * $Id: xyscope.cpp,v 1.175 2007/03/26 17:31:28 chris Exp $
 *
 */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define INITGUID
#endif
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#ifndef __APPLE__
#include <SDL2/SDL_syswm.h>
#endif

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <fftw3.h>
#else
#include <GL/gl.h>
#include <fftw3.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "xyscope-shared.h"
#include "xyscope-ringbuffer.h"
#include "xyscope-draw.h"
#include "xyscope-hdr.h"
#include "xyscope-bloom.h"

#include "xyscope-compat.h"
#include "xyscope-audio.h"

#ifdef _WIN32
/* Forward declarations — defined after scene class */
extern HDC hdr_hdc;
extern HWND fs_cover_hwnd;
#elif !defined(__APPLE__)
extern bool wayland_hdr_active;
#endif

/* Audio sample rate and display frame rate — detected at runtime */
int sample_rate = 96000;
int frame_rate  = 120;

/* ringbuffer size in seconds; expect memory usage to exceed:
 *
 * (sample_rate * BUFFER_SECONDS + sample_rate / frame_rate) * sizeof(frame_t)
 *
 * That being said, the custom ringbuffer will round up to the next
 * power of two.
 */
#define BUFFER_SECONDS 60.0

/* How many times to draw each frame */
#define DRAW_EACH_FRAME 2

/* whether to limit frame rate */
#define RESPONSIBLE_FOR_FRAME_RATE true


/* End of easily configurable settings */


/* Derived from sample_rate, frame_rate, DRAW_EACH_FRAME, BUFFER_SECONDS */
int frames_per_buf;
int draw_frames;
int default_rb_size;

static void compute_derived_rates() {
    frames_per_buf  = (sample_rate / frame_rate) * DRAW_EACH_FRAME;
    draw_frames     = frames_per_buf;
    default_rb_size = (int)(sample_rate * BUFFER_SECONDS + frames_per_buf);
}



thread_data_t Thread_Data;

#define LEFT_PORT  0
#define RIGHT_PORT 1

#define TIMED true
#define NOT_TIMED false

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))
#define sign(A) ((A) < 0.0 ? -1.0 : 1.0)


/* The scene object */

// Global SDL variables (declared here so scene class can access them)
extern TTF_Font *font;
extern SDL_Window *window;
extern SDL_GLContext gl_context;

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

/* Spectrum color shader — compiled in main(), used by drawPlot.
 * Declared before scene so drawPlot can reference them. */
static GLuint spectrum_shader_prog = 0;
static GLint  spectrum_brightness_loc = -1;

/* GPU spline shader — compiled in main(), used by drawPlot. */
static GLuint spline_shader_prog = 0;
static GLint  spline_loc_positions = -1;
static GLint  spline_loc_colors = -1;
static GLint  spline_loc_num_samples = -1;
static GLint  spline_loc_spline_steps = -1;
static GLuint spline_pos_tex = 0;
static GLuint spline_col_tex = 0;
static GLuint spline_index_vbo = 0;
static unsigned int spline_index_alloc = 0;

class scene
{
public:
    audioInput* ai;
    size_t frame_size;
    size_t bytes_per_buf;
    frame_t *framebuf;
    int offset;
    int bump;
#ifdef __APPLE__
    FFTSetup fft_setup;
    DSPSplitComplex fft_out;
#else
    fftw_complex* fft_out;
#endif
    size_t frames_read;

    double mouse[4];
    GLuint textures;

    preferences_t prefs;
    presets_t presets;
    app_config_t app;

    double target_side[4];
    double latency;
    double fps;
    double max_sample_value;
    double top_offset;
    double vertical_increment;
    double color_delta;
    double color_threshold;
    unsigned int frame_count;
    unsigned int vertex_count;
    bool window_is_dirty;
    bool mouse_is_dirty;

    bool show_intro;
    bool show_help;
    bool show_mouse;
    bool dj_mode;

    #define NUM_TEXT_TIMERS 20
    #define NUM_AUTO_TEXT_TIMERS 16
    typedef struct _text_timer_t {
        bool show;
        timeval time;
        char string[64];
        bool auto_position;
        double x_position;
        double y_position;
    } text_timer_t;
    enum {
        AutoScaleTimer   = 0,
        SplineTimer      = 1,
        LineWidthTimer   = 2,
        ParticlesTimer   = 3,
        ColorModeTimer   = 4, 
        DisplayModeTimer = 5,
        ColorRangeTimer  = 6,
        ColorRateTimer   = 7,
        DelayTimer       = 8,
        BrightnessTimer  = 9,
        VelocityDimTimer = 10,
        BloomTimer       = 11,
        BloomGammaTimer  = 12,
        BloomRadiusTimer = 13,
        SampleRateTimer  = 14,
        FrameRateTimer   = 15,
        /* End of text timers automatically included in stats display */
        PresetTimer      = 16,
        PausedTimer      = 17,
        ScaleTimer       = 18,
        CounterTimer     = 19
    } text_timer_handles;
    text_timer_t text_timer[NUM_TEXT_TIMERS];
    timeval show_intro_time;
    timeval last_frame_time;
    timeval reset_frame_time;
    timeval mouse_dirty_time;

    #define NUM_COLOR_MODES 2
    #define NUM_DISPLAY_MODES 3
    static const unsigned int DefaultColorMode    = DEFAULT_COLOR_MODE;
    static const unsigned int DefaultDisplayMode  = DEFAULT_DISPLAY_MODE;
    const char *color_mode_names[NUM_COLOR_MODES] = {"Standard", "Delta"};
    const char *display_mode_names[NUM_DISPLAY_MODES] = {
        "Standard", "Radius", "Spectrum"
    };

    scene()
    {
        frame_size         = sizeof(frame_t);
        framebuf           = NULL;
        ai                 = NULL;
        offset             = 0;
        bump               = 0;
        bytes_per_buf      = 0;
        latency            = 0.0;
        fps                = 0.0;
        frame_count        = 0;
        vertex_count       = 0;
        window_is_dirty    = true;
        mouse_is_dirty     = true;
        max_sample_value   = 1.0;
        top_offset         = -60.0;
        vertical_increment = -60.0;
        color_delta        = 0.0;
        color_threshold    = 0.0;
        show_intro         = true;
        show_help          = false;
        show_mouse         = true;
        dj_mode            = false;
        memset(&prefs,   0, sizeof(prefs));
        memset(&presets, 0, sizeof(presets));
        memset(&app,     0, sizeof(app));

        bzero(&text_timer, sizeof(text_timer_t) * NUM_TEXT_TIMERS);
        timeval now;
        gettimeofday(&now, NULL);
        show_intro_time = last_frame_time = reset_frame_time = mouse_dirty_time = now;

        for (int i = 0; i < 4; i += 2) {
			prefs.side[i]   = 1.0;
            prefs.side[i+1] = -1.0;
        }
        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
    }

    void init()
    {
        bytes_per_buf = draw_frames * frame_size;
        framebuf      = (frame_t *) malloc(bytes_per_buf);
        offset        = -frames_per_buf;
        bump          = -draw_frames;
#ifdef __APPLE__
        int log2n     = 0;
        int n         = draw_frames;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup     = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp = (float *) malloc(draw_frames/2 * sizeof(float));
        fft_out.imagp = (float *) malloc(draw_frames/2 * sizeof(float));
#else
        fft_out       = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * draw_frames);
#endif
        ai = new audioInput(app.target);
    }

    void reinit_frame_rate(int new_rate)
    {
        frame_rate = new_rate;
        compute_derived_rates();

        bytes_per_buf = draw_frames * frame_size;
        free(framebuf);
        framebuf = (frame_t *) malloc(bytes_per_buf);

#ifdef __APPLE__
        vDSP_destroy_fftsetup(fft_setup);
        free(fft_out.realp);
        free(fft_out.imagp);
        int log2n = 0;
        int n = draw_frames;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup     = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp = (float *) malloc(draw_frames/2 * sizeof(float));
        fft_out.imagp = (float *) malloc(draw_frames/2 * sizeof(float));
#else
        fftw_free(fft_out);
        fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * draw_frames);
#endif

        offset = -frames_per_buf;
        bump   = -draw_frames;

        printf("Display changed: frame rate now %d fps, frames_per_buf: %d\n",
               frame_rate, frames_per_buf);
        showFrameRate(TIMED);
    }

    void reinit_sample_rate(int new_rate)
    {
        sample_rate = new_rate;
        compute_derived_rates();

        bytes_per_buf = draw_frames * frame_size;
        free(framebuf);
        framebuf = (frame_t *) malloc(bytes_per_buf);

#ifdef __APPLE__
        vDSP_destroy_fftsetup(fft_setup);
        free(fft_out.realp);
        free(fft_out.imagp);
        int log2n = 0;
        int n = draw_frames;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup     = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp = (float *) malloc(draw_frames/2 * sizeof(float));
        fft_out.imagp = (float *) malloc(draw_frames/2 * sizeof(float));
#else
        fftw_free(fft_out);
        fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * draw_frames);
#endif

        offset = -frames_per_buf;
        bump   = -draw_frames;

        printf("Sample rate changed: %d Hz, frames_per_buf: %d\n",
               sample_rate, frames_per_buf);
        showSampleRate(TIMED);
    }

    ~scene()
    {
        save_config(&prefs, &presets, &app);
        delete ai;
#ifdef __APPLE__
        vDSP_destroy_fftsetup(fft_setup);
        free(fft_out.realp);
        free(fft_out.imagp);
#else
        fftw_free(fft_out);
#endif
        free(framebuf);
    }

    void drawPlot()
    {
        thread_data_t *t_data = ai->getThreadData();
        size_t bytes_ready = 0, bytes_read = 0;
        double dt  = 0.0;
        signed int distance = 0;

        /* FFT stuff */
        unsigned int window_size, overlap_size;
        if (prefs.display_mode == DisplaySpectrumMode) {
            /* Spectrum mode: color_range indexes octaves of window_size
             * so each integer step of color_range doubles the FFT
             * window (and halves the bin width).
             *
             *   color_range  0  1  2  3  4  5
             *   window_size  32 64 128 256 512 1024
             *
             * Default color_range=1 gives window_size=64, bin_width
             * 1500 Hz. Cranks above that give progressively finer
             * frequency resolution at the cost of fewer STFT windows
             * per frame.
             *
             * overlap_size = 0 means windows tile (stride = window_size)
             * rather than 50%-overlapping. That halves the FFT count
             * per frame at small window sizes without meaningfully
             * reducing color variation, and when frames_read isn't a
             * clean multiple of window_size the aggregation adds one
             * "nudged" STFT at frames_read-window_size to cover the
             * trailing samples. */
            /* Base window scales with sample rate so the same
             * color_range gives the same bin width at any rate.
             * At 96 kHz base=32 → color_range 0=32, 1=64, etc.
             * At 192 kHz base=64 → color_range 0=64, 1=128, etc.
             * At 48 kHz base=16 → color_range 0=16, 1=32, etc. */
            unsigned int base = 1;
            while (base * 2 <= (unsigned int)(32 * sample_rate / 96000))
                base *= 2;
            int steps = (int)prefs.color_range;
            if (steps < 0)  steps = 0;
            if (steps > 10) steps = 10;
            window_size = base;
            for (int i = 0; i < steps; i++) {
                unsigned int next = window_size * 2;
                if (next > (unsigned int)draw_frames || next > 2048) break;
                window_size = next;
            }
            overlap_size = 0;
        } else {
            window_size  = draw_frames / 100;
            overlap_size = draw_frames / 200;
            if (window_size < 2) window_size = 2;
            if (overlap_size >= window_size) overlap_size = window_size / 2;
        }
        double* spectrum_colors = NULL;  /* per-window RGB triples for DisplaySpectrumMode */
        double** stft_results;
#ifndef __APPLE__
        fftw_plan fft_plan;
#endif

        /* if the scope is paused or audio not initialized, there are no samples available;
         * therefore we should not wait for the reader thread */
        if (! t_data->pause_scope && t_data->can_process) {
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
            pthread_mutex_lock(&t_data->ringbuffer_lock);

            // Use timed wait to avoid hanging forever if audio fails
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 1000000000 / frame_rate;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec  += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&t_data->data_ready, &t_data->ringbuffer_lock, &ts);
        }


        /* Read data from the ring buffer */
        if (t_data->pause_scope) {
            distance = bump * frame_size;
            bump     = -draw_frames;
        }
        else if (t_data->ringbuffer) {
            int delay_frames = (int)(prefs.delay * 0.001 * sample_rate);
            int delay_bytes  = delay_frames * frame_size;
            bytes_ready = ringbuffer_read_space(t_data->ringbuffer);
            if (bytes_ready != (size_t)(bytes_per_buf + delay_bytes))
                distance = bytes_ready - bytes_per_buf - delay_bytes;
        }
        if (distance != 0 && t_data->ringbuffer)
            ringbuffer_read_advance(t_data->ringbuffer, distance);
        if (t_data->ringbuffer)
            bytes_read = ringbuffer_read(t_data->ringbuffer,
                                          (char *) framebuf,
                                          bytes_per_buf);

        if (! t_data->pause_scope)
            pthread_mutex_unlock(&t_data->ringbuffer_lock);

        frames_read = bytes_read / frame_size;


        /* prescans the framebuf in order to auto-scale */
        if (prefs.auto_scale)
            autoScale();


        /* set up the OpenGL */
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();

        glOrtho(prefs.side[3], prefs.side[2],
                 prefs.side[1], prefs.side[0],
                 -10.0, 10.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        if (prefs.particles) {
            glPointSize((GLfloat) prefs.line_width);
        }
        else {
            glLineWidth((GLfloat) prefs.line_width);
        }

        /* FFT setup for spectrum mode — runs on raw samples before
         * spline interpolation so it sees the original signal. */
        if (prefs.display_mode == DisplaySpectrumMode) {
                unsigned int fft_count = frames_read;
                unsigned int window_size_fft = window_size;
                unsigned int overlap_size_fft = overlap_size;
                unsigned int stride_fft = window_size_fft - overlap_size_fft;

                /* Build FFT input. Complex FFT: L=real, R read from
                 * framebuf in the lambda as imaginary (L+iR). */
                double *fft_input = new double[fft_count];
                for (unsigned int i = 0; i < fft_count; i++) {
                    fft_input[i] = framebuf[i].left_channel;
                }

                /* Allocate n_windows + 1 STFT slots. The extra slot is
                 * either the "nudged" tail window in spectrum mode or
                 * trailing carry-forward in both modes. n_windows in
                 * audio units. */
                unsigned int stride_audio = window_size - overlap_size;
                unsigned int n_windows_audio = frames_read / stride_audio;
                unsigned int n_stft_slots = n_windows_audio + 1;
                stft_results = new double*[n_stft_slots];
                for (unsigned int i = 0; i < n_stft_slots; i++) {
                    stft_results[i] = new double[window_size_fft]();
                }
#ifdef __APPLE__
                // Set up vDSP FFT once outside loop (performance optimization)
                int log2n_win = 0;
                int n_win = window_size_fft;
                while (n_win > 1) { n_win >>= 1; log2n_win++; }
                FFTSetup fft_setup_local = vDSP_create_fftsetup(log2n_win, FFT_RADIX2);
                DSPSplitComplex fft_data;
                /* Full N for complex FFT (spectrum), N/2 for real FFT (frequency) */
                unsigned int fft_alloc = (prefs.display_mode == DisplaySpectrumMode)
                    ? window_size_fft : window_size_fft / 2;
                fft_data.realp = new float[fft_alloc];
                fft_data.imagp = new float[fft_alloc];
#else
                fftw_complex *fft_out_local =
                    (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * window_size_fft);
#endif
                bool spectrum = (prefs.display_mode == DisplaySpectrumMode);

                auto compute_fft_at = [&](unsigned int start_i, unsigned int target_slot) {
#ifdef __APPLE__
                    if (spectrum) {
                        /* Complex FFT: L=real, R=imag */
                        for (unsigned int j = 0; j < window_size_fft; j++) {
                            fft_data.realp[j] = (float)fft_input[start_i + j];
                            fft_data.imagp[j] = (float)framebuf[start_i + j].right_channel;
                        }
                        vDSP_fft_zip(fft_setup_local, &fft_data, 1, log2n_win, FFT_FORWARD);
                    } else {
                        /* Real FFT: mono */
                        float *input_data = new float[window_size_fft];
                        for (unsigned int j = 0; j < window_size_fft; j++) {
                            input_data[j] = (float)fft_input[start_i + j];
                        }
                        vDSP_ctoz((DSPComplex*)input_data, 2, &fft_data, 1, window_size_fft/2);
                        vDSP_fft_zrip(fft_setup_local, &fft_data, 1, log2n_win, FFT_FORWARD);
                        delete[] input_data;
                    }
                    /* For the complex FFT (spectrum mode), combine
                     * positive and negative frequency bins so the
                     * magnitude is rotation-direction-independent.
                     * Clockwise XY motion puts energy in negative bins
                     * (N-k), counterclockwise in positive bins (k).
                     * Summing both makes the spectrum invariant to
                     * rotation direction. For real FFT (frequency
                     * mode), negative bins mirror positive so this
                     * is a harmless 2x scale. */
                    for (unsigned int j = 0; j < window_size_fft/2; j++) {
                        double rp = fft_data.realp[j];
                        double ip = fft_data.imagp[j];
                        double mag = rp*rp + ip*ip;
                        if (spectrum && j > 0 && j < window_size_fft/2) {
                            unsigned int nj = window_size_fft - j;
                            double rn = fft_data.realp[nj];
                            double in_ = fft_data.imagp[nj];
                            mag += rn*rn + in_*in_;
                        }
                        stft_results[target_slot][j] = sqrt(mag);
                    }
#else
                    double (*temp_data)[2] = new double[window_size_fft][2];
                    for (unsigned int j = 0; j < window_size_fft; j++) {
                        temp_data[j][0] = fft_input[start_i + j];
                        temp_data[j][1] = spectrum
                            ? framebuf[start_i + j].right_channel
                            : 0.0;
                    }
                    fft_plan = fftw_plan_dft_1d(window_size_fft, temp_data, fft_out_local, FFTW_FORWARD, FFTW_ESTIMATE);
                    fftw_execute(fft_plan);
                    for (unsigned int j = 0; j < window_size_fft/2; j++) {
                        double rp = fft_out_local[j][0];
                        double ip = fft_out_local[j][1];
                        double mag = rp*rp + ip*ip;
                        if (spectrum && j > 0) {
                            unsigned int nj = window_size_fft - j;
                            double rn = fft_out_local[nj][0];
                            double in_ = fft_out_local[nj][1];
                            mag += rn*rn + in_*in_;
                        }
                        stft_results[target_slot][j] = sqrt(mag);
                    }
                    fftw_destroy_plan(fft_plan);
                    delete[] temp_data;
#endif
                };

                // Regular STFT loop over the (possibly splined) FFT input
                unsigned int w_idx = 0;
                for (unsigned int i = 0; i + window_size_fft <= fft_count; i += stride_fft) {
                    compute_fft_at(i, w_idx);
                    w_idx++;
                }

                /* Nudge: in spectrum mode, if the last regular window
                 * doesn't cover the end of the frame, add one more FFT
                 * positioned to end exactly at fft_count. It lands in
                 * slot n_windows_audio (the last allocated slot), which
                 * is exactly where vertex indexing sends the trailing
                 * vertices via `i / stride`. */
                if (prefs.display_mode == DisplaySpectrumMode && w_idx > 0) {
                    unsigned int last_end = (w_idx - 1) * stride_fft + window_size_fft;
                    if (last_end < fft_count) {
                        compute_fft_at(fft_count - window_size_fft, n_windows_audio);
                    }
                }
                delete[] fft_input;
#ifdef __APPLE__
                // Clean up FFT resources after loop
                vDSP_destroy_fftsetup(fft_setup_local);
                delete[] fft_data.realp;
                delete[] fft_data.imagp;
#else
                fftw_free(fft_out_local);
#endif

                unsigned int n_windows = frames_read / (window_size - overlap_size);

                {
                    /* Spectrum mode:
                     *   R = sum(bin0..r_last)    (~0–1 kHz, sub-bass+kick)
                     *   G = sum(r_last+1..g_last) (~1–5 kHz, fat mid)
                     *   B = sum(g_last+1..b_last) (~5–20 kHz, audible treble)
                     * Boundaries are computed dynamically from bin_width
                     * so they adapt to whatever FFT window color_range
                     * picked — at baseline (window_size=64, bin_width
                     * 1500 Hz) this gives R=bin0, G=bins 1..3, B=bins
                     * 4..13. At larger windows each band gets many more
                     * bins and finer frequency resolution.
                     *
                     * Supersonic bins (>20 kHz) are excluded so pure
                     * tones with no real treble don't get false blue
                     * from accumulated noise or spline overshoot.
                     *
                     * Then divide all three by the per-frame max
                     * CHANNEL value so the strongest band in the
                     * frame is exactly 1.0 and the other two are
                     * proportional ratios less than 1.0. */
                    unsigned int half_w = window_size_fft / 2;
                    /* Bin width = sample_rate / window_size (same as
                     * for the raw-audio FFT because spline upsampling
                     * scales both the effective sample rate AND the
                     * window size proportionally). */
                    double bin_width_hz = (double)sample_rate / (double)window_size;
                    unsigned int r_last = (unsigned int)(1000.0 / bin_width_hz);
                    unsigned int g_last = (unsigned int)(5000.0 / bin_width_hz);
                    unsigned int b_last = (unsigned int)(20000.0 / bin_width_hz);
                    /* Enforce r_last < g_last < b_last < half_w,
                     * leaving at least one bin per band. */
                    if (r_last >= half_w)            r_last = half_w - 3;
                    if (g_last <= r_last)            g_last = r_last + 1;
                    if (b_last <= g_last)            b_last = g_last + 1;
                    if (b_last >= half_w)            b_last = half_w - 1;
                    spectrum_colors = new double[(n_windows + 1) * 3]();
                    /* First pass: compute per-band sums and track
                     * the max CHANNEL value across the whole frame.
                     * Iterates to n_windows INCLUSIVE: the extra slot
                     * holds either the nudged tail FFT (if there was
                     * a tail gap) or zero (which triggers the trailing
                     * carry-forward in the second pass). Either way
                     * the padding slot participates in aggregation so
                     * vertex indexing beyond the last regular window
                     * gets a sensible color. */
                    double max_v = 0.0;
                    for (unsigned int i = 0; i <= n_windows; i++) {
                        double R = 0.0;
                        for (unsigned int j = 0; j <= r_last; j++) {
                            R += stft_results[i][j];
                        }
                        double G = 0.0;
                        for (unsigned int j = r_last + 1; j <= g_last; j++) {
                            G += stft_results[i][j];
                        }
                        double B = 0.0;
                        for (unsigned int j = g_last + 1; j <= b_last; j++) {
                            B += stft_results[i][j];
                        }
                        spectrum_colors[i * 3 + 0] = R;
                        spectrum_colors[i * 3 + 1] = G;
                        spectrum_colors[i * 3 + 2] = B;
                        if (R > max_v) max_v = R;
                        if (G > max_v) max_v = G;
                        if (B > max_v) max_v = B;
                        delete[] stft_results[i];
                    }
                    /* Second pass: normalize and carry the previous
                     * valid color forward through trailing zero rows. */
                    double last_r = 0.0, last_g = 0.0, last_b = 0.0;
                    for (unsigned int i = 0; i <= n_windows; i++) {
                        double R = (max_v > 0.0) ? spectrum_colors[i*3+0] / max_v : 0.0;
                        double G = (max_v > 0.0) ? spectrum_colors[i*3+1] / max_v : 0.0;
                        double B = (max_v > 0.0) ? spectrum_colors[i*3+2] / max_v : 0.0;
                        if (R + G + B > 0.01) {
                            last_r = R; last_g = G; last_b = B;
                        }
                        spectrum_colors[i * 3 + 0] = last_r;
                        spectrum_colors[i * 3 + 1] = last_g;
                        spectrum_colors[i * 3 + 2] = last_b;
                    }
                }
                delete[] stft_results;
        }

        /* Compute color delta accumulator for ColorDeltaMode */
        if (prefs.color_mode == ColorDeltaMode) {
            double olc = 0.0, orc = 0.0;
            for (unsigned int i = 0; i < frames_read; i++) {
                double lc = framebuf[i].left_channel;
                double rc = framebuf[i].right_channel;
                dt += hypot(lc - olc, rc - orc) / SQRT_TWO;
                olc = lc;
                orc = rc;
            }
        }

        if (prefs.velocity_dim > 0.0) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        }

        /* GPU spline path: upload raw samples as textures, vertex
         * shader does Catmull-Rom.  Falls back to CPU if shader
         * didn't compile or spline_steps <= 1. */
        bool use_gpu_spline = (spline_shader_prog != 0
                               && prefs.spline_steps > 1
                               && frames_read > 4
                               && p_glBindBuffer_ && p_glBufferData_);

        if (use_gpu_spline) {
            /* Compute per-sample colors on CPU (~1600 iterations) */
            static float *s_pos = NULL;
            static float *s_col = NULL;
            static unsigned int s_samp_alloc = 0;
            if (frames_read > s_samp_alloc) {
                free(s_pos); free(s_col);
                s_pos = (float *)malloc(frames_read * 4 * sizeof(float));
                s_col = (float *)malloc(frames_read * 4 * sizeof(float));
                s_samp_alloc = frames_read;
            }

            unsigned int spl_stride = (window_size > overlap_size) ? (window_size - overlap_size) : 1;
            double h = -1.0, s = 1.0, v = 1.0, a = 1.0;
            double r = 1.0, g = 1.0, b = 1.0;
            double olc = 0.0, orc = 0.0;
            if (prefs.display_mode == DisplayStandardMode)
                HSVtoRGB(&r, &g, &b, prefs.hue, s, v);

            for (unsigned int i = 0; i < frames_read; i++) {
                double lc = framebuf[i].left_channel;
                double rc = framebuf[i].right_channel;
                double d = hypot(lc - olc, rc - orc) / SQRT_TWO;
                if (prefs.velocity_dim > 0.0)
                    a = 1.0 / (1.0 + d * 10.0 * prefs.velocity_dim * prefs.scale_factor);
                else
                    a = 1.0;

                bool color_set = false;
                switch (prefs.display_mode) {
                    case DisplayStandardMode: break;
                    case DisplayRadiusMode:
                        h = ((hypot(lc, rc) / SQRT_TWO) * 360.0 * prefs.color_range * prefs.scale_factor) + prefs.hue;
                        break;
                    case DisplaySpectrumMode:
                        if (spectrum_colors) {
                            unsigned int w = i / spl_stride;
                            double sr = spectrum_colors[w * 3 + 0];
                            double sg = spectrum_colors[w * 3 + 1];
                            double sb = spectrum_colors[w * 3 + 2];
                            double sh, ss, sv;
                            RGBtoHSV(sr, sg, sb, &sh, &ss, &sv);
                            ss *= 1.5; if (ss > 1.0) ss = 1.0;
                            double v_floor = 0.5 / prefs.brightness;
                            if (v_floor > 0.5) v_floor = 0.5;
                            sv = sv * (1.0 - v_floor) + v_floor;
                            HSVtoRGB(&r, &g, &b, sh, ss, sv);
                            color_set = true;
                        }
                        break;
                }
                if (!color_set) {
                    if (h > -1.0 && prefs.display_mode != DisplayStandardMode)
                        h = normalizeHue(h);
                    if (h > -1.0)
                        HSVtoRGB(&r, &g, &b, h, s, v);
                    else if (prefs.velocity_dim > 0.0)
                        HSVtoRGB(&r, &g, &b, prefs.hue, s, v);
                }

                s_pos[i * 4 + 0] = (float)lc;
                s_pos[i * 4 + 1] = (float)rc;
                s_pos[i * 4 + 2] = 0.0f;
                s_pos[i * 4 + 3] = 0.0f;
                s_col[i * 4 + 0] = (float)(r * prefs.brightness);
                s_col[i * 4 + 1] = (float)(g * prefs.brightness);
                s_col[i * 4 + 2] = (float)(b * prefs.brightness);
                s_col[i * 4 + 3] = (float)a;
                olc = lc; orc = rc;
            }

            /* Upload positions to 1D texture (unit 0).
             * Pre-allocate at max size once, then glTexSubImage1D
             * to avoid GPU reallocation every frame. */
            static unsigned int s_tex_alloc = 0;
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_1D, spline_pos_tex);
            if (frames_read > s_tex_alloc) {
                glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA16F, frames_read, 0, GL_RGBA, GL_FLOAT, s_pos);
                p_glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_1D, spline_col_tex);
                glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA16F, frames_read, 0, GL_RGBA, GL_FLOAT, s_col);
                glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                p_glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_1D, spline_pos_tex);
                glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                s_tex_alloc = frames_read;
            } else {
                glTexSubImage1D(GL_TEXTURE_1D, 0, 0, frames_read, GL_RGBA, GL_FLOAT, s_pos);
                p_glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_1D, spline_col_tex);
                glTexSubImage1D(GL_TEXTURE_1D, 0, 0, frames_read, GL_RGBA, GL_FLOAT, s_col);
            }

            /* Ensure index VBO is large enough.  Standard Catmull-Rom:
             * (frames_read - 3) usable segments, spline_steps verts each,
             * plus 1 for the final endpoint. */
            unsigned int n_spline_verts = (frames_read - 3) * prefs.spline_steps + 1;
            if (n_spline_verts > spline_index_alloc) {
                float *indices = (float *)malloc(n_spline_verts * 2 * sizeof(float));
                for (unsigned int i = 0; i < n_spline_verts; i++) {
                    indices[i * 2]     = (float)i;
                    indices[i * 2 + 1] = 0.0f;
                }
                if (!spline_index_vbo)
                    p_glGenBuffers_(1, &spline_index_vbo);
                p_glBindBuffer_(GL_ARRAY_BUFFER, spline_index_vbo);
                p_glBufferData_(GL_ARRAY_BUFFER, n_spline_verts * 2 * sizeof(float), indices, 0x88E4 /* GL_STATIC_DRAW */);
                p_glBindBuffer_(GL_ARRAY_BUFFER, 0);
                free(indices);
                spline_index_alloc = n_spline_verts;
            }

            /* Draw with spline shader */
            p_glUseProgram(spline_shader_prog);
            p_glUniform1i(spline_loc_positions, 0);
            p_glUniform1i(spline_loc_colors, 1);
            p_glUniform1f(spline_loc_num_samples, (float)frames_read);
            p_glUniform1f(spline_loc_spline_steps, (float)prefs.spline_steps);

            glEnableClientState(GL_VERTEX_ARRAY);
            p_glBindBuffer_(GL_ARRAY_BUFFER, spline_index_vbo);
            glVertexPointer(2, GL_FLOAT, 0, 0);
            glDrawArrays(prefs.particles ? GL_POINTS : GL_LINE_STRIP, 0, n_spline_verts);
            p_glBindBuffer_(GL_ARRAY_BUFFER, 0);
            glDisableClientState(GL_VERTEX_ARRAY);

            p_glUseProgram(0);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_1D, 0);
            p_glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_1D, 0);

            vertex_count = n_spline_verts;
        } else {
            /* CPU fallback */
            bool gpu_color = (prefs.display_mode == DisplaySpectrumMode
                              && spectrum_shader_prog != 0);
            if (gpu_color) {
                p_glUseProgram(spectrum_shader_prog);
                p_glUniform1f(spectrum_brightness_loc, (float)prefs.brightness);
            }

            vertex_count = draw_xy_vertices(
                framebuf, frames_read,
                prefs.display_mode, prefs.color_mode,
                prefs.hue, prefs.color_range, prefs.scale_factor,
                prefs.spline_steps,
                window_size, overlap_size,
                prefs.brightness, prefs.velocity_dim,
                spectrum_colors,
                prefs.particles,
                gpu_color);

            if (gpu_color)
                p_glUseProgram(0);
        }

        if (prefs.velocity_dim > 0.0)
            glDisable(GL_BLEND);
        glPopMatrix();
        if (prefs.display_mode == DisplaySpectrumMode)
            delete[] spectrum_colors;

        switch (prefs.color_mode) {
            case ColorStandardMode:
                prefs.hue -= prefs.color_rate;
                break;
            case ColorDeltaMode:
                if (color_threshold > 0.0 && dt > color_threshold) {
                    color_delta = dt / color_threshold - 1.0;
                    /* smooth(&color_threshold, dt, 0.1); */
                }
                else {
                    color_delta = 0.0;
                }
                color_threshold = dt;
                prefs.hue -= prefs.color_rate * color_delta;
                break;
            default:
                break;
        }

        prefs.hue = normalizeHue(prefs.hue);

        if (! prefs.auto_scale) {
            for (int i = 0; i < 4; i++)
                 smooth(&prefs.side[i], target_side[i], 0.1);
        }
        prefs.scale_factor = 2.0 / min(prefs.side[0] - prefs.side[1],
                                       prefs.side[2] - prefs.side[3]);
    }

    void beginText()
    {
        top_offset = -160.0;
        if (text_timer[ScaleTimer].show)
            top_offset = -220.0;

        glDisable(GL_LIGHTING);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();

        glOrtho(-1.0, 1.0, -1.0, 1.0, -1000.0, 1000.0);
        glColor3d(0.75, 0.75, 0.75);
    }

    double getTextWidth(char *string)
    {
        if (!font || !string || strlen(string) == 0) return 0.0;

        int char_width = 0;
        TTF_SizeText(font, "M", &char_width, NULL);

        return (double)(strlen(string) * char_width);
    }

    void drawString(double x, double y, char *string)
    {
        if (!font || !string || strlen(string) == 0) return;

        // Render text to surface first (need actual width for right-alignment)
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *text_surface = TTF_RenderText_Blended(font, string, white);
        if (!text_surface) {
            fprintf(stderr, "TTF_RenderText_Blended failed: %s\n", TTF_GetError());
            return;
        }

        // Convert surface to RGBA format for OpenGL
        SDL_Surface *rgba_surface = SDL_ConvertSurfaceFormat(text_surface, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(text_surface);

        if (!rgba_surface) {
            fprintf(stderr, "SDL_ConvertSurfaceFormat failed: %s\n", SDL_GetError());
            return;
        }

        // Calculate text width and height in normalized coordinates
        double text_w = (double)rgba_surface->w / (double)prefs.dim[0] * 2.0;
        double text_h = (double)rgba_surface->h / (double)prefs.dim[1] * 2.0;

        /* Position: positive x = offset from left edge.
         * Negative x = right-align with margin from right edge.
         * For right-align, use actual rendered surface width
         * so position is stable regardless of content. */
        if (x >= 0.0)
            x = -1.0 + x / (double) prefs.dim[0];
        else
            x = 1.0 - text_w - (-x) / (double) prefs.dim[0];

        if (y >= 0.0)
            y = -1.0 + y / (double) prefs.dim[1];
        else
            y =  1.0 + y / (double) prefs.dim[1];

        // Create OpenGL texture from surface
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     rgba_surface->w, rgba_surface->h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_surface->pixels);

        // Enable blending for text transparency
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);

        // Draw textured quad (flip Y texture coordinate for SDL surfaces)
        glBindTexture(GL_TEXTURE_2D, texture);
        glColor3f(1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x + text_w, y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x + text_w, y + text_h);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y + text_h);
        glEnd();

        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);

        // Cleanup
        SDL_FreeSurface(rgba_surface);
        glDeleteTextures(1, &texture);
    }

    void endText()
    {
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
    }

    void drawHelp()
    {
        double left_offset   =  80.0;
        double right_offset  = 740.0;
        char help[][2][64] = {
        { "Escape",            "Quit" },
        { "F1 thru F5",        "Quickly resize window" },
        { "Home and Page Up",  "Zoom in" },
        { "End and Page Down", "Zoom out" },
        { "Shift+0 thru 9",    "Set zoom factor" },
        { "`",                 "Load default settings" },
        { "0 thru 9",          "Load preset" },
        { "Ctrl+0 thru 9",     "Save preset" },
        { "Spacebar",          "Pause/Resume" },
        { "< and >",           "Rewind/Fast-Forward when paused" },
        { "[ and ]",           "Adjust color range" },
        { "- and +",           "Adjust color rate" },
        { "a",                 "Auto-scale on/off" },
        { "c and C",           "Color mode" },
        { "d and D",           "Display mode" },
        { "f",                 "Enter/Exit full screen mode" },
        { "h",                 "Show/Hide help" },
        { "/",                 "DJ mode (hide all text)" },
        { "l and L",           "Adjust splines" },
        { "u/i and U/I",       "Adjust brightness" },
        { "b and B",           "Adjust bloom intensity" },
        { "v and V",           "Adjust bloom gamma" },
        { "g and G",           "Adjust bloom radius" },
        { "j/k and J/K",       "Adjust display delay" },
        { "n/m and N/M",       "Adjust velocity dim" },
        { "r",                 "Recenter" },
        { "s and S",           "Show/Hide statistics" },
        { "w and W",           "Adjust line width" },
        { "p",                 "Particles on/off" }
        };
        unsigned int n_items = sizeof(help) / sizeof(help[0]);

        for (unsigned int i = 0; i < n_items; i++) {
            if (-top_offset > 2 * prefs.dim[1] - 120)
                break;
            drawString(left_offset,  top_offset, help[i][0]);
            drawString(right_offset, top_offset, help[i][1]);
            top_offset += vertical_increment;
        }
        top_offset -= 60.0;
    }

    void drawTimedText()
    {
        timeval this_frame_time;
        double elapsed_time;
        double x = 80.0;
        gettimeofday(&this_frame_time, NULL);
        for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
            if (text_timer[i].show) {
                /* get the time so we can calculate how long to display */
                elapsed_time = timeDiff(text_timer[i].time,
                                         this_frame_time);
                if (elapsed_time > 10.0)
                    text_timer[i].show = false;

                if (text_timer[i].auto_position) {
                    if (-top_offset > 2 * prefs.dim[1] - 120)
                        continue;
                    drawString(x, top_offset, text_timer[i].string);
                    top_offset += vertical_increment;
                }
                else {
                    drawString(text_timer[i].x_position,
                                text_timer[i].y_position,
                                text_timer[i].string);
                }
            }
        }
    }

    void drawStats()
    {
        thread_data_t *t_data = ai->getThreadData();
        timeval this_frame_time;
        double elapsed_time;
        /* char color_threshold_string[64]; */
        char fps_string[64];
        char vps_string[64];
        char time_string[64];

        /* Frame counting — always runs, needed by frame rate limiter */
        gettimeofday(&this_frame_time, NULL);
        elapsed_time = timeDiff(reset_frame_time, this_frame_time);
        frame_count++;
        if (elapsed_time >= 1.0) {
            fps = frame_count / elapsed_time;
            reset_frame_time = this_frame_time;
            frame_count = 0;
        }
        last_frame_time = this_frame_time;

        if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3)) {
            snprintf(fps_string, sizeof(fps_string), "%.1f fps", fps);
            drawString(60.0, 60.0, fps_string);
            snprintf(vps_string, sizeof(vps_string), "%d vps", vertex_count * frame_rate);
            drawString(-80.0, -100.0, vps_string);
        }

        /* calculate latency */
        gettimeofday(&this_frame_time, NULL);
        elapsed_time = timeDiff(t_data->last_write, this_frame_time);
        if (elapsed_time > latency)
            latency = elapsed_time;
        else
            smooth(&latency, elapsed_time, 0.01);
        if (latency < 0.0)
            latency = 0.0;
        if (! t_data->pause_scope) {
            snprintf(time_string, sizeof(time_string), "%.0f usec", latency * 100000.0);
            drawString(-80.0, 60.0, time_string);
        }
    }

    void drawText(void)
    {
        thread_data_t *t_data = ai->getThreadData();
        timeval this_frame_time;
        double elapsed_time;
        bool show_timer = false;

        /* get the time so we can calculate how long to display */
        gettimeofday(&this_frame_time, NULL);
        elapsed_time = timeDiff(show_intro_time, this_frame_time);
        if (elapsed_time > 10.0)
            show_intro = false;

        if (show_intro || prefs.show_stats == 1) {
            for (unsigned int i = 0; i < NUM_AUTO_TEXT_TIMERS; i++)
                text_timer[i].show = true;
        }
        if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3))
            text_timer[ScaleTimer].show = true;
        if (text_timer[ScaleTimer].show)
            snprintf(text_timer[ScaleTimer].string, sizeof(text_timer[ScaleTimer].string), "%.5f", prefs.scale_factor);
        if (t_data->pause_scope) {
            if (prefs.show_stats > 0 && prefs.show_stats < 4)
                text_timer[CounterTimer].show = true;
            if (text_timer[CounterTimer].show)
                snprintf(text_timer[CounterTimer].string, sizeof(text_timer[CounterTimer].string), "%7.2f sec",
                         (double) offset / (double) sample_rate
                         + (double) frames_per_buf / (double) sample_rate);
        }

        for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
            if (text_timer[i].show)
                show_timer = true;
        }
        if (show_intro || show_help || show_timer || prefs.show_stats) {
            beginText();
            if (show_intro || show_help)
                drawHelp();
            if (show_timer)
                drawTimedText();
            if (show_intro || prefs.show_stats)
                drawStats();
            endText();
        }
    }

    void showTimedText(int timer_idx, bool auto_pos, bool timed, const char *fmt, ...)
    {
        text_timer_t *timer = &text_timer[timer_idx];
        timer->auto_position = auto_pos;
        va_list args;
        va_start(args, fmt);
        vsnprintf(timer->string, sizeof(timer->string), fmt, args);
        va_end(args);
        if (timed)
            gettimeofday(&timer->time, NULL);
        timer->show = true;
    }

    /* Stats timers */
    void showAutoScale(bool t) { showTimedText(AutoScaleTimer, true, t, "Auto-scale: %s", prefs.auto_scale ? "on" : "off"); }
    void showSplines(bool t) { showTimedText(SplineTimer, true, t, "Splines: %d", prefs.spline_steps); }
    void showLineWidth(bool t) { showTimedText(LineWidthTimer, true, t, "Line width: %d", prefs.line_width); }
    void showParticles(bool t) { showTimedText(ParticlesTimer, true, t, "Particles: %s", prefs.particles ? "on" : "off"); }
    void showBloomIntensity(bool t) { showTimedText(BloomTimer, true, t, "Bloom intensity: %.1f", prefs.bloom_intensity); }
    void showBloomGamma(bool t) { showTimedText(BloomGammaTimer, true, t, "Bloom gamma: %.1f", prefs.bloom_gamma); }
    void showBloomRadius(bool t) { showTimedText(BloomRadiusTimer, true, t, "Bloom radius: %.1f", prefs.bloom_radius); }
    void showColorMode(bool t) { showTimedText(ColorModeTimer, true, t, "Color mode: %s", color_mode_names[prefs.color_mode]); }
    void showDisplayMode(bool t) { showTimedText(DisplayModeTimer, true, t, "Display mode: %s", display_mode_names[prefs.display_mode]); }
    void showColorRange(bool t) { showTimedText(ColorRangeTimer, true, t, "Color range: %.2f", prefs.color_range); }
    void showColorRate(bool t) { showTimedText(ColorRateTimer, true, t, "Color rate: %.2f", prefs.color_rate); }
    void showDelay(bool t) { showTimedText(DelayTimer, true, t, "Delay: %.2f ms", prefs.delay); }
    void showBrightness(bool t) {
#ifdef _WIN32
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f %s",
                      prefs.brightness, hdr_hdc ? "(HDR)" : "(SDR)");
#elif defined(__APPLE__)
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f", prefs.brightness);
#else
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f %s",
                      prefs.brightness, wayland_hdr_active ? "(HDR)" : "(SDR)");
#endif
    }
    void showVelocityDim(bool t) { showTimedText(VelocityDimTimer, true, t, "Velocity dim: %.1f", prefs.velocity_dim); }
    void showSampleRate(bool t) { showTimedText(SampleRateTimer, true, t, "Sample rate: %d Hz", sample_rate); }
    void showFrameRate(bool t) { showTimedText(FrameRateTimer, true, t, "Frame rate: %d fps", frame_rate); }

    /* Other timers */
    void showPaused(bool t) { showTimedText(PausedTimer, true, t, "Paused"); }

    void showScale(bool timed)
    {
        text_timer_t *timer  = &text_timer[ScaleTimer];
        timer->auto_position = false;
        timer->x_position    =  80.0;
        timer->y_position    = -100.0;
        if (timed)
            gettimeofday(&timer->time, NULL);
        timer->show = true;
    }

    void showCounter(bool timed)
    {
        text_timer_t *timer  = &text_timer[CounterTimer];
        timer->auto_position = false;
        timer->x_position    = -80.0;
        timer->y_position    =   60.0;
        if (timed)
            gettimeofday(&timer->time, NULL);
        timer->show = true;
    }

    void showMouse()
    {
        gettimeofday(&mouse_dirty_time, NULL);
        show_mouse     = true;
        mouse_is_dirty = true;
    }

    void autoScale()
    {
        double lc = 0.0;
        double rc = 0.0;
        double mv = 0.0;
        double mt = 0.0;
        for (unsigned int i = 0; i < frames_read; i++) {
            lc = fabs(framebuf[i].left_channel);
            rc = fabs(framebuf[i].right_channel);
            mt = max(lc, rc);
            mv = max(mv, mt);
        }
        if (mv > max_sample_value)
            max_sample_value = mv;
        else if (mv < max_sample_value * (1.0 / 3.0))
            smooth(&max_sample_value,
                    (max_sample_value * (2.0 / 3.0) + mv),
                    0.2);
        setSides(max_sample_value / 0.95, 1);
    }

    void zoomIn(void)
    {
        scale(1.1);
    }

    void zoomOut(void)
    {
        scale(1 / 1.1);
    }

    void rescale(void)
    {
        /* change the sides so as to keep the same coordinate-to-pixel
         * ratio after a subsequent Viewport operation as before. */
        if (prefs.old_dim[0] < 1)
            prefs.old_dim[0] = 1;
        if (prefs.old_dim[1] < 1)
            prefs.old_dim[1] = 1;
        double wr = (double) prefs.dim[0] / (double) prefs.old_dim[0];
        double hr = (double) prefs.dim[1] / (double) prefs.old_dim[1];
        prefs.old_dim[0] = prefs.dim[0];
        prefs.old_dim[1] = prefs.dim[1];
        prefs.side[0] = target_side[0] = prefs.side[0] * hr;
        prefs.side[1] = target_side[1] = prefs.side[1] * hr;
        prefs.side[2] = target_side[2] = prefs.side[2] * wr;
        prefs.side[3] = target_side[3] = prefs.side[3] * wr;
    }

    void scale(double factor)
    {
        double width        = target_side[0] - target_side[1];
        double height       = target_side[2] - target_side[3];
        double add_distance = min(width, height) * (1.0 - factor);
        double r            = ((double) prefs.dim[0]
                               / (double) prefs.dim[1]);
        double shortest     = 0.0;
        double longest      = 0.0;
        double t_side[4];
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale(TIMED);
        }
        if (r >= 1.0) {
            t_side[0] = target_side[0] + add_distance / 2.0;
            t_side[1] = target_side[1] - add_distance / 2.0;
            t_side[2] = target_side[2] + (add_distance * r) / 2.0;
            t_side[3] = target_side[3] - (add_distance * r) / 2.0;
        }
        else {
            t_side[0] = target_side[0] + (add_distance / r) / 2.0;
            t_side[1] = target_side[1] - (add_distance / r) / 2.0;
            t_side[2] = target_side[2] + add_distance / 2.0;
            t_side[3] = target_side[3] - add_distance / 2.0;
        }
        width    = t_side[0] - t_side[1];
        height   = t_side[2] - t_side[3];
        shortest = min(width, height);
        longest  = max(width, height);
        if (shortest > 0.00001 && longest < 10000.0) {
            for (int i = 0; i < 4; i++)
                target_side[i] = t_side[i];
        }
        showScale(TIMED);
    }

    void move(int ax, double x)
    {
        int s1    = ax * 2;
        int s2    = s1 + 1;
        double w  = target_side[s2] - target_side[s1];
        double dx = x * w;
        prefs.scale_locked = false;
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale(TIMED);
        }
        target_side[s1] += dx;
        target_side[s2] += dx;
    }

    void toggleFullScreen(void)
    {
        if (prefs.is_full_screen)
            setWindowSize(prefs.normal_dim[0], prefs.normal_dim[1]);
        else
            setFullScreen();
    }

    void toggleAutoScale(void)
    {
        max_sample_value = min((prefs.side[0] - prefs.side[1]) / 2.1,
                               (prefs.side[2] - prefs.side[3]) / 2.1);
        prefs.auto_scale = ! prefs.auto_scale;
        showAutoScale(TIMED);
    }

    void moreSplines(void)
    {
        if (prefs.spline_steps < 1024)
            prefs.spline_steps *= 2;
        showSplines(TIMED);
    }

    void lessSplines(void)
    {
        if (prefs.spline_steps > 1)
            prefs.spline_steps /= 2;
        showSplines(TIMED);
    }

    void togglePaused(void)
    {
        thread_data_t *t_data = ai->getThreadData();
        if (t_data->pause_scope) {
            latency = 0.0;
            text_timer[CounterTimer].show = false;
            text_timer[PausedTimer].show  = false;
        }
        else {
            offset = -frames_per_buf;
            bump   = -draw_frames;
            showCounter(TIMED);
            showPaused(TIMED);
        }
        t_data->pause_scope = ! t_data->pause_scope;
        gettimeofday(&t_data->last_write, NULL);
    }

    void recenter(void)
    {
        target_side[0] =  (prefs.side[0] - prefs.side[1]) / 2.0;
        target_side[1] = -(prefs.side[0] - prefs.side[1]) / 2.0;
        target_side[2] =  (prefs.side[2] - prefs.side[3]) / 2.0;
        target_side[3] = -(prefs.side[2] - prefs.side[3]) / 2.0;
        prefs.hue = 0.0;
    }

    void nextColorMode(void)
    {
        prefs.color_mode = (prefs.color_mode + 1) % NUM_COLOR_MODES;
        showColorMode(TIMED);
    }

    void prevColorMode(void)
    {
        if (prefs.color_mode < 1)
            prefs.color_mode = NUM_COLOR_MODES - 1;
        else
            prefs.color_mode = prefs.color_mode - 1;
        showColorMode(TIMED);
    }

    void nextDisplayMode(void)
    {
        prefs.display_mode = (prefs.display_mode + 1) % NUM_DISPLAY_MODES;
        if (prefs.display_mode == DisplaySpectrumMode)
            setColorRange(prefs.color_range);   /* re-clamp for new mode */
        showDisplayMode(TIMED);
    }

    void prevDisplayMode(void)
    {
        if (prefs.display_mode < 1)
            prefs.display_mode = NUM_DISPLAY_MODES - 1;
        else
            prefs.display_mode = prefs.display_mode - 1;
        if (prefs.display_mode == DisplaySpectrumMode)
            setColorRange(prefs.color_range);   /* re-clamp for new mode */
        showDisplayMode(TIMED);
    }

    void nextStatsGroup(void)
    {
        // thread_data_t *t_data = ai->getThreadData();
        // gettimeofday(&t_data->last_write, NULL);
        // latency = 0.0;
        prefs.show_stats++;
        if (prefs.show_stats > 3)
            prefs.show_stats = 0;
    }

    void prevStatsGroup(void)
    {
        if (prefs.show_stats < 1)
            prefs.show_stats = 3;
        else
            prefs.show_stats--;
    }

    void rewind(int nbufs)
    {
        thread_data_t *t_data = ai->getThreadData();
        if (t_data->pause_scope) {
            int step = (frames_per_buf / DRAW_EACH_FRAME) * nbufs;
            if ((offset - step) >= -default_rb_size) {
                offset -= step;
                bump   -= step;
            }
            showCounter(TIMED);
        }
    }

    void fastForward(int nbufs)
    {
        thread_data_t *t_data = ai->getThreadData();
        if (t_data->pause_scope) {
            int step = (frames_per_buf / DRAW_EACH_FRAME) * nbufs;
            if (offset < -step) {
                offset += step;
                bump   += step;
            }
            showCounter(TIMED);
        }
    }

    /* accessor methods */

    void setWindowSize(unsigned int x, unsigned int y)
    {
        if (prefs.is_full_screen) {
#ifdef _WIN32
            if (fs_cover_hwnd) ShowWindow(fs_cover_hwnd, SW_HIDE);
            HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
            if (taskbar) ShowWindow(taskbar, SW_SHOW);

            SDL_SysWMinfo wminfo;
            SDL_VERSION(&wminfo.version);
            if (SDL_GetWindowWMInfo(window, &wminfo)) {
                HWND hwnd = wminfo.info.win.window;
                LONG style = GetWindowLong(hwnd, GWL_STYLE);
                SetWindowLong(hwnd, GWL_STYLE,
                    style | WS_CAPTION | WS_THICKFRAME);
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
            }
#else
            SDL_SetWindowFullscreen(window, 0);
#endif
        } else {
            SDL_GetWindowPosition(window, &prefs.position[0], &prefs.position[1]);
        }
        SDL_SetWindowPosition(window, prefs.position[0], prefs.position[1]);
        SDL_SetWindowSize(window, x, y);
        window_is_dirty      = true;
        prefs.is_full_screen = false;
    }

    void setFullScreen(void)
    {
        if (! prefs.is_full_screen) {
            SDL_GetWindowPosition(window, &prefs.position[0], &prefs.position[1]);
            SDL_GetWindowSize(window, &prefs.normal_dim[0], &prefs.normal_dim[1]);
        }
#ifdef _WIN32
        /* Use borderless window at desktop resolution instead of
         * SDL_WINDOW_FULLSCREEN_DESKTOP.  Windows silently promotes
         * fullscreen-desktop OpenGL windows to exclusive fullscreen,
         * which causes the volume overlay (and any DWM compositor
         * event) to disrupt rendering and audio. Borderless keeps
         * DWM compositing active so overlays work normally.
         *
         * The window must be slightly smaller than the display to
         * prevent Windows from promoting it to exclusive fullscreen
         * (see libsdl-org/SDL#12791). */
        {
            SDL_DisplayMode mode;
            int di = SDL_GetWindowDisplayIndex(window);
            if (SDL_GetDesktopDisplayMode(di, &mode) == 0) {
                /* Borderless window 1 pixel shorter than the desktop
                 * to prevent Windows/GPU driver from promoting to
                 * exclusive fullscreen (which breaks overlays and
                 * audio).  Hide the taskbar so the gap isn't visible. */
                HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
                if (taskbar) ShowWindow(taskbar, SW_HIDE);

                SDL_SysWMinfo wminfo;
                SDL_VERSION(&wminfo.version);
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    HWND hwnd = wminfo.info.win.window;
                    /* Suppress repainting during transition */
                    SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
                    /* Remove border in one shot */
                    LONG style = GetWindowLong(hwnd, GWL_STYLE);
                    SetWindowLong(hwnd, GWL_STYLE,
                        style & ~(WS_CAPTION | WS_THICKFRAME));
                    /* Resize + reposition atomically */
                    SetWindowPos(hwnd, HWND_TOP, 0, 0,
                                 mode.w, mode.h - 1,
                                 SWP_FRAMECHANGED);
                    SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
                }

                /* Black cover window for the 1-pixel gap at the bottom */
                if (!fs_cover_hwnd) {
                    WNDCLASSA wc = {};
                    wc.lpfnWndProc   = DefWindowProcA;
                    wc.hInstance     = GetModuleHandle(NULL);
                    wc.lpszClassName = "XYScopeCover";
                    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    RegisterClassA(&wc);
                    fs_cover_hwnd = CreateWindowExA(
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        wc.lpszClassName, "",
                        WS_POPUP | WS_VISIBLE,
                        0, mode.h - 1, mode.w, 1,
                        NULL, NULL, wc.hInstance, NULL);
                } else {
                    SetWindowPos(fs_cover_hwnd, HWND_TOP,
                                 0, mode.h - 1, mode.w, 1,
                                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
                }

                /* Grab focus after everything is set up */
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    SetForegroundWindow(wminfo.info.win.window);
                    SetFocus(wminfo.info.win.window);
                }
            }
        }
#else
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif
        prefs.is_full_screen = true;
        show_mouse           = false;
        mouse_is_dirty       = true;
        window_is_dirty      = true;
    }

    void setZoom(double factor)
    {
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale(TIMED);
        }
        showScale(TIMED);
        setSides(1.0 / factor, 0);
    }

    void setSides(double x, int no_smooth)
    {
        double r = (double) prefs.dim[0] / (double) prefs.dim[1];
        if (x < 0.000005 || x > 5000.0)
            return;
        prefs.scale_locked = true;
        if (r >= 1.0) {
            target_side[0] =  x;
            target_side[1] = -x;
            target_side[2] =  x * r;
            target_side[3] = -x * r;
        }
        else {
            target_side[0] =  x / r;
            target_side[1] = -x / r;
            target_side[2] =  x;
            target_side[3] = -x;
        }
        if (no_smooth) {
            for (unsigned int i = 0; i < 4; i++)
                prefs.side[i] = target_side[i];
        }
    }

    double getColorRange(void)
    {
        return prefs.color_range;
    }

    double getColorRate(void)
    {
        return prefs.color_rate;
    }

    void setColorRange(double range)
    {
        prefs.color_range = range;
        if (prefs.display_mode == DisplaySpectrumMode) {
            /* Spectrum mode uses color_range as an octave index for
             * window_size. Clamp to the usable octave range: each
             * integer step doubles window_size from 32, and we stop
             * once the next doubling would overflow draw_frames or
             * the 2048 vDSP cap. */
            int max_steps = 0;
            unsigned int base = 1;
            while (base * 2 <= (unsigned int)(32 * sample_rate / 96000))
                base *= 2;
            unsigned int ws = base;
            while (ws * 2 <= (unsigned int)draw_frames && ws * 2 <= 2048) {
                ws *= 2;
                max_steps++;
            }
            if (prefs.color_range < 0.0) prefs.color_range = 0.0;
            if (prefs.color_range > (double)max_steps) prefs.color_range = (double)max_steps;
        } else {
            wrapValue(&prefs.color_range, 100.0);
        }
        showColorRange(TIMED);
    }

    void setColorRate(double rate)
    {
        prefs.color_rate = rate;
        wrapValue(&prefs.color_rate, 180.0);
        showColorRate(TIMED);
    }

    double getBrightness(void)
    {
        return prefs.brightness;
    }

    void setBrightness(double b)
    {
        prefs.brightness = b;
        if (prefs.brightness < 0.1) prefs.brightness = 0.1;
        showBrightness(TIMED);
    }

    void setVelocityDim(double d)
    {
        prefs.velocity_dim = d;
        if (prefs.velocity_dim < 0.0) prefs.velocity_dim = 0.0;
        showVelocityDim(TIMED);
    }

    double getDelay(void)
    {
        return prefs.delay;
    }

    void setDelay(double ms)
    {
        prefs.delay = ms;
        if (prefs.delay < 0.0) prefs.delay = 0.0;
        showDelay(TIMED);
    }

    int getLineWidth(void)
    {
        return prefs.line_width;
    }

    void setLineWidth(int width)
    {
        prefs.line_width = width;
        if (prefs.line_width < 1)
            prefs.line_width = MAX_LINE_WIDTH;
        else if (prefs.line_width > MAX_LINE_WIDTH)
            prefs.line_width = 1;
        showLineWidth(TIMED);
    }

    void toggleParticles(void)
    {
        prefs.particles = !prefs.particles;
        showParticles(TIMED);
    }

    void setBloomIntensity(double v)
    {
        prefs.bloom_intensity = v;
        if (prefs.bloom_intensity < 0.0) prefs.bloom_intensity = 0.0;
        showBloomIntensity(TIMED);
    }

    void setBloomGamma(double v)
    {
        prefs.bloom_gamma = v;
        if (prefs.bloom_gamma < 0.1) prefs.bloom_gamma = 0.1;
        showBloomGamma(TIMED);
    }

    void setBloomRadius(double v)
    {
        prefs.bloom_radius = v;
        if (prefs.bloom_radius < 0.5) prefs.bloom_radius = 0.5;
        showBloomRadius(TIMED);
    }

    void savePreset(int n)
    {
        presets.slot[n] = prefs;
        presets.saved[n] = true;
        showTimedText(PresetTimer, true, TIMED, "Preset %d saved", n);
    }

    unsigned int default_spline_steps()
    {
        unsigned int s = 16 * 96000 / sample_rate;
        if (s < 2) s = 2;
        if (s > 128) s = 128;
        return s;
    }

    void validate_prefs()
    {
        if (prefs.normal_dim[0] < 1) prefs.normal_dim[0] = 1000;
        if (prefs.normal_dim[1] < 1) prefs.normal_dim[1] = 1000;
        /* Old config files may have display_mode=3 (was Spectrum when
         * Frequency was mode 2). Map both 2 and 3 to Spectrum. */
        if (prefs.display_mode == 3)
            prefs.display_mode = DisplaySpectrumMode;
        if (prefs.display_mode >= NUM_DISPLAY_MODES)
            prefs.display_mode = DefaultDisplayMode;
        if (prefs.color_mode >= NUM_COLOR_MODES)
            prefs.color_mode = DefaultColorMode;
        if (prefs.spline_steps < 1 || prefs.spline_steps > 1024)
            prefs.spline_steps = default_spline_steps();
        if (prefs.line_width < 1 || prefs.line_width > MAX_LINE_WIDTH)
            prefs.line_width = DEFAULT_LINE_WIDTH;
        if (prefs.bloom_gamma < 0.1)
            prefs.bloom_gamma = DEFAULT_BLOOM_GAMMA;
        if (prefs.bloom_radius < 0.5)
            prefs.bloom_radius = DEFAULT_BLOOM_RADIUS;
    }

    void loadPreset(int n)
    {
        if (!presets.saved[n]) {
            showTimedText(PresetTimer, true, TIMED, "Preset %d empty", n);
            return;
        }
        /* preserve window geometry and fullscreen state */
        int dim[2], normal_dim[2], old_dim[2], position[2];
        bool is_full_screen;
        memcpy(dim, prefs.dim, sizeof(dim));
        memcpy(normal_dim, prefs.normal_dim, sizeof(normal_dim));
        memcpy(old_dim, prefs.old_dim, sizeof(old_dim));
        memcpy(position, prefs.position, sizeof(position));
        is_full_screen = prefs.is_full_screen;

        prefs = presets.slot[n];
        validate_prefs();

        memcpy(prefs.dim, dim, sizeof(dim));
        memcpy(prefs.normal_dim, normal_dim, sizeof(normal_dim));
        memcpy(prefs.old_dim, old_dim, sizeof(old_dim));
        memcpy(prefs.position, position, sizeof(position));
        prefs.is_full_screen = is_full_screen;

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
        refreshStats(TIMED);
        showTimedText(PresetTimer, true, TIMED, "Preset %d loaded", n);
    }

    void loadDefaults()
    {
        prefs.scale_factor  = 1.0;
        prefs.scale_locked  = true;
        prefs.auto_scale    = DEFAULT_AUTO_SCALE;
        prefs.spline_steps  = default_spline_steps();
        prefs.color_mode    = DEFAULT_COLOR_MODE;
        prefs.color_range   = DEFAULT_COLOR_RANGE;
        prefs.color_rate    = DEFAULT_COLOR_RATE;
        prefs.display_mode  = DEFAULT_DISPLAY_MODE;
        prefs.line_width    = DEFAULT_LINE_WIDTH;
        prefs.particles     = DEFAULT_PARTICLES;
        prefs.hue           = 0.0;
        double detected     = detect_hdr_brightness();
#ifdef __APPLE__
        prefs.brightness    = (detected > 2.0) ? 2.0 : detected;
#else
        prefs.brightness    = detected;
#endif
        prefs.velocity_dim  = prefs.brightness / 2.0;
        if (prefs.velocity_dim < 1.0)
            prefs.velocity_dim = 1.0;
        prefs.bloom_intensity = DEFAULT_BLOOM;
        prefs.bloom_gamma     = DEFAULT_BLOOM_GAMMA;
        prefs.bloom_radius    = DEFAULT_BLOOM_RADIUS;
        max_sample_value = min((prefs.side[0] - prefs.side[1]) / 2.1,
                               (prefs.side[2] - prefs.side[3]) / 2.1);
        refreshStats(TIMED);
        showTimedText(PresetTimer, true, TIMED, "Defaults loaded");
    }

    void refreshStats(bool t)
    {
        showAutoScale(t);
        showSplines(t);
        showLineWidth(t);
        showParticles(t);
        showColorMode(t);
        showDisplayMode(t);
        showColorRange(t);
        showColorRate(t);
        showDelay(t);
        showBrightness(t);
        showVelocityDim(t);
        showBloomIntensity(t);
        showBloomGamma(t);
        showBloomRadius(t);
    }
};
static scene scn;
static bloom_state_t bloom = {0};

static const char *SPECTRUM_VS_SRC =
    "#version 120\n"
    "uniform float u_brightness;\n"
    "varying vec4 v_color;\n"
    "\n"
    "vec3 rgb2hsv(vec3 c) {\n"
    "    float mx = max(c.r, max(c.g, c.b));\n"
    "    float mn = min(c.r, min(c.g, c.b));\n"
    "    float d = mx - mn;\n"
    "    float h = 0.0, s = 0.0, v = mx;\n"
    "    if (mx > 0.0) s = d / mx;\n"
    "    if (d > 0.0) {\n"
    "        if (mx == c.r) h = mod((c.g - c.b) / d + 6.0, 6.0) / 6.0;\n"
    "        else if (mx == c.g) h = ((c.b - c.r) / d + 2.0) / 6.0;\n"
    "        else h = ((c.r - c.g) / d + 4.0) / 6.0;\n"
    "    }\n"
    "    return vec3(h, s, v);\n"
    "}\n"
    "\n"
    "vec3 hsv2rgb(vec3 c) {\n"
    "    float h = c.x * 6.0, s = c.y, v = c.z;\n"
    "    float i = floor(h), f = h - i;\n"
    "    float p = v * (1.0 - s);\n"
    "    float q = v * (1.0 - s * f);\n"
    "    float t = v * (1.0 - s * (1.0 - f));\n"
    "    if (i < 1.0) return vec3(v, t, p);\n"
    "    else if (i < 2.0) return vec3(q, v, p);\n"
    "    else if (i < 3.0) return vec3(p, v, t);\n"
    "    else if (i < 4.0) return vec3(p, q, v);\n"
    "    else if (i < 5.0) return vec3(t, p, v);\n"
    "    else return vec3(v, p, q);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "    vec3 hsv = rgb2hsv(gl_Color.rgb);\n"
    "    hsv.y = min(hsv.y * 1.5, 1.0);\n"
    "    float v_floor = min(0.5 / u_brightness, 0.5);\n"
    "    hsv.z = hsv.z * (1.0 - v_floor) + v_floor;\n"
    "    vec3 rgb = hsv2rgb(hsv);\n"
    "    v_color = vec4(rgb * u_brightness, gl_Color.a);\n"
    "}\n";

static const char *SPECTRUM_FS_SRC =
    "#version 120\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";

/* ---- GPU spline shader ---- */


static const char *SPLINE_VS_SRC =
    "#version 120\n"
    "uniform sampler1D u_positions;\n"
    "uniform sampler1D u_colors;\n"
    "uniform float u_num_samples;\n"
    "uniform float u_spline_steps;\n"
    "void main() {\n"
    "    float idx = gl_Vertex.x;\n"
    "    float seg = floor(idx / u_spline_steps);\n"
    "    float t = idx / u_spline_steps - seg;\n"
    "    seg += 1.0;\n"  /* +1 for Catmull-Rom margin */
    "    float inv_n = 1.0 / u_num_samples;\n"
    "    vec4 s0 = texture1DLod(u_positions, (seg - 1.0 + 0.5) * inv_n, 0.0);\n"
    "    vec4 s1 = texture1DLod(u_positions, (seg + 0.5) * inv_n, 0.0);\n"
    "    vec4 s2 = texture1DLod(u_positions, (seg + 1.0 + 0.5) * inv_n, 0.0);\n"
    "    vec4 s3 = texture1DLod(u_positions, (seg + 2.0 + 0.5) * inv_n, 0.0);\n"
    "    float t2 = t * t, t3 = t2 * t;\n"
    "    float x = 0.5 * (2.0*s1.r + (-s0.r+s2.r)*t + (2.0*s0.r-5.0*s1.r+4.0*s2.r-s3.r)*t2 + (-s0.r+3.0*s1.r-3.0*s2.r+s3.r)*t3);\n"
    "    float y = 0.5 * (2.0*s1.g + (-s0.g+s2.g)*t + (2.0*s0.g-5.0*s1.g+4.0*s2.g-s3.g)*t2 + (-s0.g+3.0*s1.g-3.0*s2.g+s3.g)*t3);\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * vec4(x, y, 0.0, 1.0);\n"
    "    gl_FrontColor = texture1DLod(u_colors, (seg + 0.5) * inv_n, 0.0);\n"
    "}\n";

static const char *SPLINE_FS_SRC =
    "#version 120\n"
    "void main() {\n"
    "    gl_FragColor = gl_Color;\n"
    "}\n";

void display()
{
    glClear(GL_COLOR_BUFFER_BIT);

    /* plot the samples on the screen */
    bool use_bloom = bloom.enabled && scn.prefs.bloom_intensity > 0.0;
    if (use_bloom) bloom_begin(&bloom);
    scn.drawPlot();
    if (use_bloom) bloom_end(&bloom, (float)scn.prefs.bloom_intensity, (float)scn.prefs.bloom_gamma, (float)scn.prefs.bloom_radius);

    /* draw any text that needs drawing */
    if (!scn.dj_mode) scn.drawText();

    /* wash, rinse, repeat */
    glFinish();
    // SDL_GL_SwapWindow is called in main loop
}

void idle(void)
{
    timeval this_moment;
    double elapsed_time;

    /* restore our window title after coming out of full screen mode */
    if (scn.window_is_dirty) {
        SDL_SetWindowTitle(window, "XY Scope");

        // Use drawable size for viewport (handles Retina/HiDPI displays)
        int drawable_w, drawable_h;
        SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
        scn.prefs.dim[0] = drawable_w;
        scn.prefs.dim[1] = drawable_h;

        // Also save window size in points (for window recreation)
        if (! scn.prefs.is_full_screen) {
            SDL_GetWindowSize(window, &scn.prefs.normal_dim[0], &scn.prefs.normal_dim[1]);
        }

        if (scn.prefs.scale_locked)
            scn.setSides(1.0 / scn.prefs.scale_factor, 1);
        else
            scn.rescale();
        glViewport(0, 0, drawable_w, drawable_h);
        bloom_resize(&bloom, drawable_w, drawable_h);
        scn.window_is_dirty = false;
    }

    if (scn.show_mouse) {
        gettimeofday(&this_moment, NULL);
        elapsed_time = timeDiff(scn.mouse_dirty_time, this_moment);
        if (elapsed_time > 10.0) {
            gettimeofday(&scn.mouse_dirty_time, NULL);
            scn.show_mouse     = false;
            scn.mouse_is_dirty = true;
        }
    }
    if (scn.mouse_is_dirty) {
        SDL_ShowCursor(scn.show_mouse ? SDL_ENABLE : SDL_DISABLE);
        scn.mouse_is_dirty = false;
    }

    if (RESPONSIBLE_FOR_FRAME_RATE) {
        /* limit our framerate to frame_rate (e.g. 60) frames per second */
        elapsed_time = timeDiff(scn.reset_frame_time, scn.last_frame_time);
        if (elapsed_time < (scn.frame_count / (double) frame_rate)) {
            double remainder = (scn.frame_count
                                / (double) frame_rate - elapsed_time);
            usleep((useconds_t)(1000000.0 * remainder));
        }
    }
    // Rendering happens in main loop, no need to request redisplay
}

void special(int key, int xPos, int yPos)
{
    switch (key) {
        case 101:                  /* up arrow */
            scn.move(0, 0.2);
            break;
        case 103:                  /* down arrow */
            scn.move(0, -0.2);
            break;
        case 100:                  /* left arrow */
            scn.move(1, -0.2);
            break;
        case 102:                  /* right arrow */
            scn.move(1, 0.2);
            break;
        case 104:                  /* page up */
            scn.zoomIn();
            break;
        case 105:                  /* page down */
            scn.zoomOut();
            break;
        case 106:                  /* home */
            scn.zoomIn();
            break;
        case 107:                  /* end */
            scn.zoomOut();
            break;
        case 1:                    /* F1 */
            scn.setWindowSize(800, 800);
            break;
        case 2:                    /* F2 */
            scn.setWindowSize(1000, 1000);
            break;
        case 3:                    /* F3 */
            scn.setWindowSize(1400, 1400);
            break;
        case 4:                    /* F4 */
            scn.setWindowSize(2000, 2000);
            break;
        case 5:                    /* F5 */
            scn.toggleFullScreen();
            break;
        default:
            break;
    }
}

void keyboard(unsigned char key, int xPos, int yPos)
{
    switch (key) {
        case 27:                         /* escape */
            scn.ai->quitNow();
            exit(0);
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            scn.loadPreset(key - '0');
            break;
        case ',':
            scn.rewind(1);
            break;
        case '.':
            scn.fastForward(1);
            break;
        case '<':
            scn.rewind(frame_rate);
            break;
        case '>':
            scn.fastForward(frame_rate);
            break;
        case '_':
            scn.setColorRate(scn.getColorRate() - 0.01);
            break;
        case '+':
            scn.setColorRate(scn.getColorRate() + 0.01);
            break;
        case '-':
            scn.setColorRate(scn.getColorRate() - 1.0);
            break;
        case '=':
            scn.setColorRate(scn.getColorRate() + 1.0);
            break;
        case '{':
            scn.setColorRange(scn.getColorRange() - 0.01);
            break;
        case '}':
            scn.setColorRange(scn.getColorRange() + 0.01);
            break;
        case '[':
            scn.setColorRange(scn.getColorRange() - 1.0);
            break;
        case ']':
            scn.setColorRange(scn.getColorRange() + 1.0);
            break;
        case ' ':                        /* spacebar */
            scn.togglePaused();
            break;
        case 'a':
            scn.toggleAutoScale();
            break;
        case 'l':
            scn.moreSplines();
            break;
        case 'L':
            scn.lessSplines();
            break;
        case 'c':
            scn.nextColorMode();
            break;
        case 'C':
            scn.prevColorMode();
            break;
        case 'd':
            scn.nextDisplayMode();
            break;
        case 'D':
            scn.prevDisplayMode();
            break;
        case 'f':
            scn.toggleFullScreen();
            break;
        case 'h':
            if (scn.show_intro)
                scn.show_intro = false;
            else
                scn.show_help = ! scn.show_help;
            break;
        case '/':
            scn.dj_mode = !scn.dj_mode;
            break;
        case 'j':
            scn.setDelay(scn.getDelay() - 1.0);
            break;
        case 'J':
            scn.setDelay(scn.getDelay() - 0.01);
            break;
        case 'k':
            scn.setDelay(scn.getDelay() + 1.0);
            break;
        case 'K':
            scn.setDelay(scn.getDelay() + 0.01);
            break;
        case 'r':
            scn.recenter();
            break;
        case 's':
            scn.nextStatsGroup();
            break;
        case 'S':
            scn.prevStatsGroup();
            break;
        case 'w':
            scn.setLineWidth(scn.getLineWidth() + 1);
            break;
        case 'W':
            scn.setLineWidth(scn.getLineWidth() - 1);
            break;
        case 'p':
            scn.toggleParticles();
            break;
        case 'b':
            scn.setBloomIntensity(scn.prefs.bloom_intensity + 0.1);
            break;
        case 'B':
            scn.setBloomIntensity(scn.prefs.bloom_intensity - 0.1);
            break;
        case 'v':
            scn.setBloomGamma(scn.prefs.bloom_gamma + 0.1);
            break;
        case 'V':
            scn.setBloomGamma(scn.prefs.bloom_gamma - 0.1);
            break;
        case 'g':
            scn.setBloomRadius(scn.prefs.bloom_radius + 0.5);
            break;
        case 'G':
            scn.setBloomRadius(scn.prefs.bloom_radius - 0.5);
            break;
        case 'i':
            scn.setBrightness(scn.getBrightness() + 1.0);
            break;
        case 'I':
            scn.setBrightness(scn.getBrightness() + 0.1);
            break;
        case 'u':
            scn.setBrightness(scn.getBrightness() - 1.0);
            break;
        case 'U':
            scn.setBrightness(scn.getBrightness() - 0.1);
            break;
        case 'n':
            scn.setVelocityDim(scn.prefs.velocity_dim - 1.0);
            break;
        case 'm':
            scn.setVelocityDim(scn.prefs.velocity_dim + 1.0);
            break;
        case 'N':
            scn.setVelocityDim(scn.prefs.velocity_dim - 0.1);
            break;
        case 'M':
            scn.setVelocityDim(scn.prefs.velocity_dim + 0.1);
            break;
        case '`':
            scn.loadDefaults();
            break;
        default:
            break;
    }
}

void reshape(int w, int h)
{
    scn.window_is_dirty = true;
}

void mouse(int button, int state, int x, int y)
{
    scn.mouse[0] = x;
    scn.mouse[1] = y;
    scn.mouse[2] = button;
    if (button == 3 && state == 1) {
        scn.zoomIn();
    }
    else if (button == 4 && state == 1) {
        scn.zoomOut();
    }
    scn.showMouse();
}

void motion(int x, int y)
{
    int dx = (int) (x - scn.mouse[0]);
    int dy = (int) (y - scn.mouse[1]);
    if (scn.mouse[2] == 0) {
        scn.move(0, - (double) dy / (double) scn.prefs.dim[1]);
        scn.move(1,   (double) dx / (double) scn.prefs.dim[0]);
    }
    else if (scn.mouse[2] == 2) {
        scn.scale(1.0 - dy / 50.0);
    }
    scn.mouse[0] = x;
    scn.mouse[1] = y;
    scn.showMouse();
}

void passiveMotion(int x, int y)
{
    scn.showMouse();
}

// Global SDL variables (definition)
SDL_Window *window = NULL;
SDL_GLContext gl_context = NULL;
#ifdef _WIN32
HDC hdr_hdc = NULL;              /* non-NULL when using WGL float framebuffer */
HGLRC hdr_hglrc = NULL;
HWND fs_cover_hwnd = NULL;
#elif !defined(__APPLE__)
bool wayland_hdr_active = false;
#endif
TTF_Font *font = NULL;

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SDL_SetMainReady();
    timeBeginPeriod(1);

    /* -mwindows silently discards stdout/stderr. Redirect both to a log file
     * in the config dir so diagnostic prints (bloom init, HDR setup, errors)
     * can be read after the fact. */
    {
        const char *appdata = getenv("APPDATA");
        if (!appdata) appdata = ".";
        char logdir[480];
        char logpath[512];
        snprintf(logdir, sizeof(logdir), "%s\\XYScope", appdata);
        CreateDirectoryA(logdir, NULL);
        snprintf(logpath, sizeof(logpath), "%s\\xyscope.log", logdir);
        freopen(logpath, "w", stderr);
        freopen(logpath, "a", stdout);
        fprintf(stderr, "=== XYScope log ===\n");
        fflush(stderr);
    }
#elif !defined(__APPLE__)
    /* GUI app launchers (COSMIC, and likely others) don't drain child
     * stdio at all — both stdout and stderr can block on full pipes.
     * dup2 of one onto the other doesn't help. When we're not attached
     * to a tty, redirect both streams to a log file in the config dir
     * so diagnostic writes always complete instantly. Matches the
     * Windows -mwindows approach. Line-buffer the log so partial output
     * survives crashes. */
    if (!isatty(STDERR_FILENO)) {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        char logdir[480];
        char logpath[512];
        snprintf(logdir, sizeof(logdir), "%s/.config/xyscope", home);
        mkdir(logdir, 0755);
        snprintf(logpath, sizeof(logpath), "%s/xyscope.log", logdir);
        freopen(logpath, "w", stderr);
        freopen(logpath, "a", stdout);
        setvbuf(stderr, NULL, _IOLBF, 0);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
#endif
    // Load preferences
    bool config_loaded = load_config(&scn.prefs, &scn.presets, &scn.app);
    if (!config_loaded)
        scn.prefs.is_full_screen = DEFAULT_FULL_SCREEN;

    // Parse CLI arguments
    int start_preset = -1;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--preset")) && i + 1 < argc) {
            start_preset = atoi(argv[++i]);
        }
#if !defined(__APPLE__) && !defined(_WIN32)
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--target")) && i + 1 < argc) {
            snprintf(scn.app.target, sizeof(scn.app.target), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--reset-target")) {
            scn.app.target[0] = '\0';
        }
#endif
        else if (!strcmp(argv[i], "--splines") && i + 1 < argc) {
            scn.prefs.spline_steps = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-mode") && i + 1 < argc) {
            scn.prefs.color_mode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-range") && i + 1 < argc) {
            scn.prefs.color_range = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-rate") && i + 1 < argc) {
            scn.prefs.color_rate = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--display-mode") && i + 1 < argc) {
            scn.prefs.display_mode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--line-width") && i + 1 < argc) {
            scn.prefs.line_width = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--particles") && i + 1 < argc) {
            scn.prefs.particles = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--hue") && i + 1 < argc) {
            scn.prefs.hue = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--brightness") && i + 1 < argc) {
            scn.prefs.brightness = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--velocity-dim") && i + 1 < argc) {
            scn.prefs.velocity_dim = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom") && i + 1 < argc) {
            scn.prefs.bloom_intensity = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom-gamma") && i + 1 < argc) {
            scn.prefs.bloom_gamma = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom-radius") && i + 1 < argc) {
            scn.prefs.bloom_radius = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            scn.prefs.delay = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--fullscreen")) {
            scn.prefs.is_full_screen = true;
        }
        else if (!strcmp(argv[i], "--windowed")) {
            scn.prefs.is_full_screen = false;
        }
        else if (!strcmp(argv[i], "--dj")) {
            scn.dj_mode = true;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: xyscope [options]\n\n");
            printf("  -p, --preset N       Load preset N (0-9) on startup\n");
#if !defined(__APPLE__) && !defined(_WIN32)
            printf("  -t, --target ID      Pipewire target node name or serial\n");
            printf("  -r, --reset-target   Clear saved Pipewire target\n");
#endif
            printf("  --splines N          Spline interpolation steps (1-1024)\n");
            printf("  --display-mode N     0=standard, 1=radius, 2=spectrum\n");
            printf("  --color-mode N       0=standard, 1=delta\n");
            printf("  --color-range N      Color range multiplier\n");
            printf("  --color-rate N       Color rotation rate\n");
            printf("  --hue N              Starting hue (0-360)\n");
            printf("  --brightness N       Brightness multiplier\n");
            printf("  --velocity-dim N     Velocity dimming amount\n");
            printf("  --bloom N            Bloom intensity (0=off)\n");
            printf("  --bloom-gamma N      Bloom gamma curve\n");
            printf("  --bloom-radius N     Bloom blur radius\n");
            printf("  --line-width N       Line width (1-%d)\n", MAX_LINE_WIDTH);
            printf("  --particles N        Particles mode (0=lines, 1=points)\n");
            printf("  --delay N            Display delay in ms\n");
            printf("  --fullscreen         Start in fullscreen\n");
            printf("  --windowed           Start in windowed mode\n");
            printf("  --dj                 DJ mode (hide all text)\n");
            printf("  -h, --help           Show this help\n");
            return 0;
        }
    }

    // Validate loaded preferences
    scn.validate_prefs();

    // Apply startup preset (CLI overrides config)
    if (start_preset >= 0 && start_preset < NUM_PRESETS)
        scn.loadPreset(start_preset);

#if !defined(__APPLE__) && !defined(_WIN32)
    /* Force SDL onto the Wayland video driver when we're clearly on a
     * Wayland session. SDL's auto-detect falls through to the X11
     * driver depending on parent-process context — XWayland's X11
     * reply path deadlocks inside XGetWindowAttributes from the COSMIC
     * app launcher on Pop! OS, and SDL's X11 GLX fails with "Couldn't
     * find matching GLX visual" when asked for a float framebuffer on
     * drivers without GLX_ARB_fbconfig_float. Overwrite any existing
     * value so a stale SDL_VIDEODRIVER=x11 in the user's shell profile
     * can't defeat the fix. wayland_hdr_setup() also depends on SDL
     * being on the Wayland driver to extract wl_display from
     * SDL_SysWMinfo, so this is consistent with the HDR pipeline. */
    if (getenv("WAYLAND_DISPLAY")) {
        setenv("SDL_VIDEODRIVER", "wayland", 1);
    }
#endif

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    
    // Initialize SDL_ttf
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
    } else {
#ifdef __APPLE__
        font = TTF_OpenFont("/System/Library/Fonts/Monaco.ttf", 28);
        if (!font) font = TTF_OpenFont("/System/Library/Fonts/Courier.ttc", 28);
#elif defined(_WIN32)
        font = TTF_OpenFont("C:\\Windows\\Fonts\\consola.ttf", 28);
        if (!font) font = TTF_OpenFont("C:\\Windows\\Fonts\\cour.ttf", 28);
#else
        font = TTF_OpenFont("/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/noto/NotoSansMono-Regular.ttf", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/dejavu/DejaVuSansMono.ttf", 28);
#endif
        if (!font) {
            fprintf(stderr, "Warning: Could not load font: %s\n", TTF_GetError());
        }
    }

#ifdef _WIN32
    /* Try WGL float framebuffer for HDR; fall back to SDL if unavailable */
    {
        hdr_window_t hdr = {};
        if (create_hdr_window(&hdr, "XY Scope",
                              scn.prefs.position[0], scn.prefs.position[1],
                              scn.prefs.normal_dim[0], scn.prefs.normal_dim[1])) {
            hdr_hdc   = hdr.hdc;
            hdr_hglrc = hdr.hglrc;
            window = SDL_CreateWindowFrom((void *)hdr.hwnd);
            if (!window) {
                fprintf(stderr, "SDL_CreateWindowFrom failed: %s\n", SDL_GetError());
                wglDeleteContext(hdr.hglrc);
                DestroyWindow(hdr.hwnd);
                hdr_hdc = NULL;
                hdr_hglrc = NULL;
            }
        }
    }
    if (!window) {
        /* Standard SDL path (no HDR) */
        hdr_hdc = NULL;
        hdr_hglrc = NULL;
#endif
    // Set OpenGL attributes
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#ifndef _WIN32
    SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 1);
#endif

    // Create window
    window = SDL_CreateWindow("XY Scope",
                              scn.prefs.position[0],
                              scn.prefs.position[1],
                              scn.prefs.normal_dim[0],
                              scn.prefs.normal_dim[1],
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

#ifndef _WIN32
    if (!window) {
        /* SDL_GL_FLOATBUFFERS can fail on X11 GLX without
         * GLX_ARB_fbconfig_float (e.g. pure-X11 sessions or older
         * drivers). Retry as SDR — users on those setups lose HDR
         * headroom but keep the app. */
        fprintf(stderr, "Window creation failed (%s); retrying as SDR (no float framebuffer)\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 0);
        window = SDL_CreateWindow("XY Scope",
                                  scn.prefs.position[0],
                                  scn.prefs.position[1],
                                  scn.prefs.normal_dim[0],
                                  scn.prefs.normal_dim[1],
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    }
#endif

    if (!window) {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Create OpenGL context
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "OpenGL context could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Enable VSync
    if (SDL_GL_SetSwapInterval(-1) == -1)  /* try adaptive vsync first */
        SDL_GL_SetSwapInterval(1);

#ifndef _WIN32
    /* Disable color clamping for HDR.
     * SDL_GL_FLOATBUFFERS gives us a float framebuffer;
     * unclamping lets values > 1.0 reach HDR luminance.
     * (Windows handles this in the WGL HDR path below.) */
    {
        #ifndef GL_CLAMP_VERTEX_COLOR_ARB
        #define GL_CLAMP_VERTEX_COLOR_ARB   0x891A
        #define GL_CLAMP_FRAGMENT_COLOR_ARB 0x891B
        #endif
#ifdef __APPLE__
        extern void glClampColorARB(GLenum, GLenum);
#else
        typedef void (*PFNGLCLAMPCOLORARBPROC)(GLenum, GLenum);
        PFNGLCLAMPCOLORARBPROC glClampColorARB =
            (PFNGLCLAMPCOLORARBPROC)SDL_GL_GetProcAddress("glClampColorARB");
        if (!glClampColorARB) goto skip_clamp;
#endif
        glClampColorARB(GL_CLAMP_VERTEX_COLOR_ARB, GL_FALSE);
        glClampColorARB(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FALSE);
#ifndef __APPLE__
        skip_clamp:;
#endif
    }
#endif

#ifdef _WIN32
    } /* end of SDL fallback block */

    /* Enable VSync for WGL HDR path */
    if (hdr_hdc) {
        typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);
        PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT =
            (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (wglSwapIntervalEXT)
            wglSwapIntervalEXT(1);

        /* Disable color clamping so values > 1.0 reach the float
         * framebuffer.  Without this, the fixed-function pipeline
         * clamps glColor values to [0,1] even with a float FB. */
        #define GL_CLAMP_VERTEX_COLOR_ARB   0x891A
        #define GL_CLAMP_FRAGMENT_COLOR_ARB 0x891B
        #define GL_FALSE_ARB                0
        typedef void (APIENTRY *PFNGLCLAMPCOLORARBPROC)(GLenum, GLenum);
        PFNGLCLAMPCOLORARBPROC glClampColorARB =
            (PFNGLCLAMPCOLORARBPROC)wglGetProcAddress("glClampColorARB");
        if (glClampColorARB) {
            glClampColorARB(GL_CLAMP_VERTEX_COLOR_ARB, GL_FALSE_ARB);
            glClampColorARB(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FALSE_ARB);
        }
    }
#endif

#if !defined(__APPLE__) && !defined(_WIN32) && defined(HAVE_WP_COLOR_MANAGEMENT)
    wayland_hdr_active = wayland_hdr_setup(window);
#endif

    glGenTextures(1, &scn.textures);

    // Set initial viewport
    int drawable_w, drawable_h;
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    reshape(drawable_w, drawable_h);
    if (!bloom_init(&bloom, drawable_w, drawable_h)) {
        fprintf(stderr, "Bloom init failed or disabled; rendering without bloom.\n");
    }
#ifdef _WIN32
    /* On Windows the log file is the only way to see diagnostics, and
     * _mwindows buffers stderr until process exit. Force a flush and emit
     * an explicit success marker so the log is readable immediately. */
    else {
        fprintf(stderr, "Bloom init succeeded (%dx%d).\n", bloom.width, bloom.height);
    }
    fflush(stderr);
#endif

    /* Compile the spectrum color shader — does HSV S-boost + V-lift
     * on the GPU so draw_xy_vertices can skip the per-vertex CPU
     * conversion. Uses the same GL proc pointers bloom loaded. */
    if (bloom.enabled) {
        spectrum_shader_prog = bloom_build_program(SPECTRUM_VS_SRC, SPECTRUM_FS_SRC);
        if (spectrum_shader_prog) {
            spectrum_brightness_loc = p_glGetUniformLocation(spectrum_shader_prog, "u_brightness");
            fprintf(stderr, "Spectrum shader compiled.\n");
        }
    }

    /* Compile the GPU spline shader — moves Catmull-Rom interpolation
     * to the vertex shader, uploading only raw samples as textures. */
    if (bloom.enabled) {
        spline_shader_prog = bloom_build_program(SPLINE_VS_SRC, SPLINE_FS_SRC);
        if (spline_shader_prog) {
            spline_loc_positions    = p_glGetUniformLocation(spline_shader_prog, "u_positions");
            spline_loc_colors       = p_glGetUniformLocation(spline_shader_prog, "u_colors");
            spline_loc_num_samples  = p_glGetUniformLocation(spline_shader_prog, "u_num_samples");
            spline_loc_spline_steps = p_glGetUniformLocation(spline_shader_prog, "u_spline_steps");
            /* Create 1D textures for sample data */
            glGenTextures(1, &spline_pos_tex);
            glGenTextures(1, &spline_col_tex);
            fprintf(stderr, "GPU spline shader compiled.\n");
        }
    }

    if (scn.prefs.is_full_screen) {
        scn.setFullScreen();
    }

    // Raise window and grab focus (after fullscreen setup so the
    // cover window and taskbar hiding don't steal it back)
    SDL_RaiseWindow(window);
#ifdef _WIN32
    {
        SDL_SysWMinfo wminfo;
        SDL_VERSION(&wminfo.version);
        if (SDL_GetWindowWMInfo(window, &wminfo)) {
            SetForegroundWindow(wminfo.info.win.window);
            SetFocus(wminfo.info.win.window);
        }
    }
#endif

    // Detect rates and initialize audio
    {
        SDL_DisplayMode mode;
        int display_index = SDL_GetWindowDisplayIndex(window);
        if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0 && mode.refresh_rate > 0) {
            frame_rate = mode.refresh_rate;
        } else {
            frame_rate = 60;
        }
    }
    sample_rate = detect_sample_rate();
    compute_derived_rates();
    printf("Using sample rate: %d Hz, frame rate: %d fps\n", sample_rate, frame_rate);
    printf("  frames_per_buf: %d, draw_frames: %d, rb_size: %d\n",
           frames_per_buf, draw_frames, default_rb_size);
    scn.init();

    if (!config_loaded)
        scn.loadDefaults();

    scn.refreshStats(NOT_TIMED);
    scn.showScale(NOT_TIMED);
    scn.showSampleRate(NOT_TIMED);
    scn.showFrameRate(NOT_TIMED);

    // Main event loop
    bool running = true;
    SDL_Event event;

    while (running) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                SDL_Keycode key = event.key.keysym.sym;
                SDL_Keymod mod = SDL_GetModState();

                // Handle special keys (F-keys, arrows, etc.)
                if (key >= SDLK_F1 && key <= SDLK_F12) {
                    special(key - SDLK_F1 + 1, 0, 0);  // F1 = 1, F2 = 2, etc.
                } else if (key == SDLK_UP) {
                    special(101, 0, 0);
                } else if (key == SDLK_DOWN) {
                    special(103, 0, 0);
                } else if (key == SDLK_LEFT) {
                    special(100, 0, 0);
                } else if (key == SDLK_RIGHT) {
                    special(102, 0, 0);
                } else if (key == SDLK_PAGEUP) {
                    special(104, 0, 0);
                } else if (key == SDLK_PAGEDOWN) {
                    special(105, 0, 0);
                } else if (key == SDLK_HOME) {
                    special(106, 0, 0);
                } else if (key == SDLK_END) {
                    special(107, 0, 0);
                } else if (key == SDLK_ESCAPE) {
                    running = false;
                } else if (key < 256) {
                    // Regular ASCII keys - handle modifiers
                    unsigned char ch = (unsigned char)key;

                    // Ctrl+number: save preset
                    if ((mod & KMOD_CTRL) && ch >= '0' && ch <= '9') {
                        scn.savePreset(ch - '0');
                    }
                    // Shift+number: quick zoom (was plain number)
                    else if ((mod & KMOD_SHIFT) && ch >= '0' && ch <= '9') {
                        if (ch == '0')
                            scn.setZoom(pow(2.0, 9.0));
                        else
                            scn.setZoom(pow(2.0, ch - '1'));
                    }
                    else {
                        // Convert lowercase to uppercase if shift is held
                        if ((mod & KMOD_SHIFT) && ch >= 'a' && ch <= 'z') {
                            ch = ch - 'a' + 'A';
                        }
                        // Handle shifted non-number keys for special characters
                        else if (mod & KMOD_SHIFT) {
                            switch(ch) {
                                case '-': ch = '_'; break;
                                case '=': ch = '+'; break;
                                case '[': ch = '{'; break;
                                case ']': ch = '}'; break;
                                case ',': ch = '<'; break;
                                case '.': ch = '>'; break;
                            }
                        }

                        keyboard(ch, 0, 0);
                    }
                }
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    reshape(event.window.data1, event.window.data2);
                }
                else if (event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
                    SDL_DisplayMode mode;
                    int di = SDL_GetWindowDisplayIndex(window);
                    if (SDL_GetCurrentDisplayMode(di, &mode) == 0
                        && mode.refresh_rate > 0
                        && mode.refresh_rate != frame_rate) {
                        scn.reinit_frame_rate(mode.refresh_rate);
                    }
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                if (event.motion.state) {
                    motion(event.motion.x, event.motion.y);
                } else {
                    passiveMotion(event.motion.x, event.motion.y);
                }
            } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                int state = (event.type == SDL_MOUSEBUTTONDOWN) ? 0 : 1;
                mouse(event.button.button - 1, state, event.button.x, event.button.y);
            } else if (event.type == SDL_MOUSEWHEEL) {
                if (event.wheel.y > 0) {
                    scn.zoomIn();
                } else if (event.wheel.y < 0) {
                    scn.zoomOut();
                }
            }
        }

        // Check for sample rate change (Pipewire negotiation)
        {
            int negotiated = scn.ai->getThreadData()->negotiated_sample_rate;
            if (negotiated > 0 && negotiated != sample_rate)
                scn.reinit_sample_rate(negotiated);
        }

        // Idle processing
        idle();

        // Display
        display();

        // Swap buffers
#ifdef _WIN32
        if (hdr_hdc)
            SwapBuffers(hdr_hdc);
        else
#endif
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
#ifdef _WIN32
    {
        HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
        if (taskbar) ShowWindow(taskbar, SW_SHOW);
        if (fs_cover_hwnd) ShowWindow(fs_cover_hwnd, SW_HIDE);
    }
    if (hdr_hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hdr_hglrc);
    } else
#endif
#if !defined(__APPLE__) && !defined(_WIN32) && defined(HAVE_WP_COLOR_MANAGEMENT)
    if (wayland_hdr_active) {
        if (wl_hdr.feedback)
            wp_color_management_surface_feedback_v1_destroy(wl_hdr.feedback);
        if (wl_hdr.cm_surface)
            wp_color_management_surface_v1_destroy(wl_hdr.cm_surface);
        if (wl_hdr.image_desc)
            wp_image_description_v1_destroy(wl_hdr.image_desc);
        if (wl_hdr.manager)
            wp_color_manager_v1_destroy(wl_hdr.manager);
    }
#endif
    bloom_cleanup(&bloom);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return 0;
}
