/*
 * xyscope-calibrate - Measure audio and display latency, update xyscope preferences
 *
 * Opens an SDL/OpenGL window, plays a short impulse through the default audio
 * output, captures from the microphone, and renders captured audio through the
 * xyscope GL drawing pipeline.  Measures three timestamps:
 *
 *   T_play    - when the first impulse sample is written to the output buffer
 *   T_capture - when the impulse is detected in the capture callback
 *   T_render  - when glFinish()+SwapWindow completes for the impulse frame
 *
 * Computes:
 *   audio_delay   = (T_capture - T_play) in ms
 *   display_delay = (T_render  - T_capture) in ms
 *   delay         = max(0, audio_delay - display_delay) in ms
 *
 * Writes all three values to xyscope.conf.
 *
 * Usage: xyscope-calibrate
 */

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#ifndef _WIN32
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <process.h>
#define bzero(b, len) memset((b), 0, (len))
#endif

#include "xyscope-shared.h"
#include "xyscope-ringbuffer.h"
#include "xyscope-draw.h"

/* ---- Constants ---- */

#define SAMPLE_RATE     48000
#define CHANNELS        1
#define SETTLE_MS       500
#define RECORD_SECONDS  2
#define CLICK_MS        20
#define CLICK_FRAMES    (SAMPLE_RATE * CLICK_MS / 1000)
#define CLICK_FREQ      1000
#define THRESHOLD       0.02f
#define WINDOW_W        640
#define WINDOW_H        480
#define TARGET_FPS      60
#define RB_FRAMES       (SAMPLE_RATE * RECORD_SECONDS)
#define RB_SIZE         (RB_FRAMES * (int)sizeof(frame_t))

/* ---- Shared state ---- */

typedef struct {
    int                played;            /* playback sample counter */
    struct timeval     t_play;            /* T_play timestamp */
    volatile int       play_started;      /* flag: playback has begun */

    ringbuffer_t      *ringbuffer;        /* capture -> render */
    struct timeval     t_capture;         /* T_capture timestamp */
    volatile int       capture_detected;  /* flag: impulse seen in capture */
    volatile int       capture_active;    /* flag: capture is recording */

    struct timeval     t_render;          /* T_render timestamp */
    volatile int       render_detected;   /* flag: impulse rendered */
    volatile int       done;             /* flag: measurement complete */
} calibrate_state_t;

/* ---- Audio callbacks ---- */

static void playback_callback(void *userdata, Uint8 *stream, int len)
{
    calibrate_state_t *state = (calibrate_state_t *)userdata;
    int frames = len / (sizeof(float) * CHANNELS);
    float *out = (float *)stream;

    for (int i = 0; i < frames; i++) {
        if (state->played < CLICK_FRAMES) {
            if (state->played == 0) {
                gettimeofday(&state->t_play, NULL);
                state->play_started = 1;
            }
            out[i] = sinf(2.0f * 3.14159f * CLICK_FREQ * state->played / SAMPLE_RATE);
            state->played++;
        } else {
            out[i] = 0.0f;
        }
    }
}

static void capture_callback(void *userdata, Uint8 *stream, int len)
{
    calibrate_state_t *state = (calibrate_state_t *)userdata;
    if (!state->capture_active) return;

    float *in = (float *)stream;
    int frames = len / (sizeof(float) * CHANNELS);

    for (int i = 0; i < frames; i++) {
        frame_t frame;
        frame.left_channel  = in[i];
        frame.right_channel = in[i];
        ringbuffer_write(state->ringbuffer, (const char *)&frame, sizeof(frame_t));

        if (!state->capture_detected && state->play_started && fabsf(in[i]) > THRESHOLD) {
            gettimeofday(&state->t_capture, NULL);
            state->capture_detected = 1;
        }
    }
}

/* ---- Preferences update ---- */

static int update_prefs(double delay_ms, double audio_ms, double display_ms)
{
    preferences_t prefs;
    presets_t presets;
    app_config_t app;
    memset(&prefs, 0, sizeof(prefs));
    memset(&presets, 0, sizeof(presets));
    memset(&app, 0, sizeof(app));
    load_config(&prefs, &presets, &app);

    prefs.delay         = delay_ms;
    prefs.audio_delay   = audio_ms;
    prefs.display_delay = display_ms;

    if (!save_config(&prefs, &presets, &app)) {
        fprintf(stderr, "Error: cannot write %s\n", get_config_path());
        return -1;
    }
    return 0;
}

/* ---- Windows entry point ---- */

#ifdef _WIN32
int main(int argc, char *argv[]);
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    return main(__argc, __argv);
}
#endif

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    SDL_SetMainReady();

    calibrate_state_t state;
    memset(&state, 0, sizeof(state));

    /* Initialize SDL with video and audio */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Create SDL window with OpenGL context */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *window = SDL_CreateWindow(
        "xyscope-calibrate",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* GL state setup */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.0f);

    /* Create ringbuffer */
    state.ringbuffer = ringbuffer_create(RB_SIZE);

    /* Open capture device (microphone) */
    SDL_AudioSpec cap_want, cap_have;
    memset(&cap_want, 0, sizeof(cap_want));
    cap_want.freq     = SAMPLE_RATE;
    cap_want.format   = AUDIO_F32SYS;
    cap_want.channels = CHANNELS;
    cap_want.samples  = 256;
    cap_want.callback = capture_callback;
    cap_want.userdata = &state;

    SDL_AudioDeviceID cap_dev = SDL_OpenAudioDevice(NULL, 1, &cap_want, &cap_have, 0);
    if (cap_dev == 0) {
        fprintf(stderr, "Cannot open microphone: %s\n", SDL_GetError());
        ringbuffer_free(state.ringbuffer);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Open playback device */
    SDL_AudioSpec play_want, play_have;
    memset(&play_want, 0, sizeof(play_want));
    play_want.freq     = SAMPLE_RATE;
    play_want.format   = AUDIO_F32SYS;
    play_want.channels = CHANNELS;
    play_want.samples  = 256;
    play_want.callback = playback_callback;
    play_want.userdata = &state;

    SDL_AudioDeviceID play_dev = SDL_OpenAudioDevice(NULL, 0, &play_want, &play_have, 0);
    if (play_dev == 0) {
        fprintf(stderr, "Cannot open audio output: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(cap_dev);
        ringbuffer_free(state.ringbuffer);
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    printf("Calibrating...\n");

    /* Start capture and render loop, let everything settle before playing */
    SDL_PauseAudioDevice(cap_dev, 0);
    state.capture_active = 1;

    int bytes_per_buf = (SAMPLE_RATE / TARGET_FPS) * (int)sizeof(frame_t);
    int frames_per_buf = SAMPLE_RATE / TARGET_FPS;
    frame_t *framebuf = (frame_t *)malloc(bytes_per_buf);
    int running = 1;
    int playing = 0;
    Uint32 settle_start = SDL_GetTicks();
    Uint32 timeout_ms = SETTLE_MS + RECORD_SECONDS * 1000 + 1000;

    while (running && !state.done) {
        Uint32 elapsed = SDL_GetTicks() - settle_start;

        /* Check timeout */
        if (elapsed > timeout_ms) {
            fprintf(stderr, "Timeout: no impulse detected within %u ms\n", timeout_ms);
            running = 0;
            break;
        }

        /* Start playback after settle period */
        if (!playing && elapsed >= SETTLE_MS) {
            SDL_PauseAudioDevice(play_dev, 0);
            playing = 1;
        }

        /* Poll SDL events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            }
        }
        if (!running) break;

        /* Set up GL projection */
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        size_t avail = ringbuffer_read_space(state.ringbuffer);

        if ((int)avail >= bytes_per_buf) {
            /* Advance past excess data to stay current */
            while ((int)avail > bytes_per_buf * 2) {
                ringbuffer_read_advance(state.ringbuffer, bytes_per_buf);
                avail = ringbuffer_read_space(state.ringbuffer);
            }

            /* Read one frame's worth of data */
            ringbuffer_read(state.ringbuffer, (char *)framebuf, bytes_per_buf);

            /* Render via GL pipeline — draw_xy_vertices handles
             * vertex arrays and glDrawArrays internally. */
            draw_xy_vertices(
                framebuf,
                frames_per_buf,
                DisplayStandardMode,
                ColorStandardMode,
                120.0,              /* hue: green */
                DEFAULT_COLOR_RANGE,
                1.0,                /* scale_factor */
                DEFAULT_SPLINE_STEPS,
                0, 0,
                1.0,                /* brightness */
                0.0,                /* velocity_dim */
                NULL);              /* no spectrum colors */

            glFinish();
            SDL_GL_SwapWindow(window);

            /* If impulse was captured but not yet recorded as rendered, mark it */
            if (state.capture_detected && !state.render_detected) {
                gettimeofday(&state.t_render, NULL);
                state.render_detected = 1;
                state.done = 1;
            }
        } else {
            /* No data available -- render empty frame to keep pipeline warm */
            glBegin(GL_LINE_STRIP);
            glEnd();
            glFinish();
            SDL_GL_SwapWindow(window);
        }

        SDL_Delay(1);
    }

    /* Stop audio */
    SDL_PauseAudioDevice(play_dev, 1);
    SDL_PauseAudioDevice(cap_dev, 1);

    /* Hold the last frame on screen so the user can see the impulse */
    if (state.render_detected) {
        SDL_Delay(1000);
    }

    /* Compute results */
    int result = 1;

    if (state.done && state.capture_detected && state.render_detected) {
        double audio_delay   = timeDiff(state.t_play, state.t_capture) * 1000.0;
        double display_delay = timeDiff(state.t_capture, state.t_render) * 1000.0;
        double delay         = audio_delay - display_delay;
        if (delay < 0.0) delay = 0.0;

        printf("Audio delay:   %.2f ms  (T_capture - T_play)\n", audio_delay);
        printf("Display delay: %.2f ms  (T_render  - T_capture)\n", display_delay);
        printf("Net delay:     %.2f ms  (compensation value)\n", delay);

        if (update_prefs(delay, audio_delay, display_delay) == 0) {
            printf("Updated %s\n", get_config_path());
        }
        result = 0;
    } else {
        printf("No impulse detected. Try increasing speaker volume.\n");
    }

    /* Cleanup */
    free(framebuf);
    SDL_CloseAudioDevice(play_dev);
    SDL_CloseAudioDevice(cap_dev);
    ringbuffer_free(state.ringbuffer);
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return result;
}
