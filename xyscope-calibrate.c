/*
 * xyscope-calibrate - Measure audio output latency and update xyscope preferences
 *
 * Plays a short impulse through the default audio output, records from the
 * default microphone, and measures the delay between them. Writes the result
 * to .xyscope.pref so xyscope can compensate for audio-visual sync.
 *
 * Usage: xyscope-calibrate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
#include <windows.h>
int main(int argc, char *argv[]);
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d)
{
    return main(__argc, __argv);
}
#endif

#define PREF_FILE       ".xyscope.pref"
#define PREF_SIZE       136
#define DELAY_OFFSET    128
#define SAMPLE_RATE     48000
#define CHANNELS        1
#define RECORD_MS       500
#define RECORD_FRAMES   (SAMPLE_RATE * RECORD_MS / 1000)
#define IMPULSE_FRAMES  1
#define THRESHOLD       0.05f

/* Playback state */
typedef struct {
    int played;
} playback_state_t;

/* Recording state */
typedef struct {
    float *buffer;
    int    frames_written;
    int    max_frames;
    int    active;
} record_state_t;

static void playback_callback(void *userdata, Uint8 *stream, int len)
{
    playback_state_t *state = (playback_state_t *)userdata;
    int frames = len / (sizeof(float) * CHANNELS);
    float *out = (float *)stream;

    for (int i = 0; i < frames; i++) {
        if (!state->played && i == 0) {
            out[i] = 1.0f;  /* single-sample impulse */
            state->played = 1;
        } else {
            out[i] = 0.0f;
        }
    }
}

static void capture_callback(void *userdata, Uint8 *stream, int len)
{
    record_state_t *state = (record_state_t *)userdata;
    if (!state->active)
        return;

    float *in = (float *)stream;
    int frames = len / (sizeof(float) * CHANNELS);

    for (int i = 0; i < frames && state->frames_written < state->max_frames; i++) {
        state->buffer[state->frames_written++] = in[i];
    }
}

static double find_impulse(float *buffer, int frames)
{
    for (int i = 0; i < frames; i++) {
        if (fabsf(buffer[i]) > THRESHOLD) {
            return (double)i / (double)SAMPLE_RATE * 1000.0;
        }
    }
    return -1.0;
}

static int update_prefs(double delay_ms)
{
    unsigned char buf[PREF_SIZE];
    FILE *f;

    f = fopen(PREF_FILE, "rb");
    if (f) {
        size_t n = fread(buf, 1, PREF_SIZE, f);
        fclose(f);
        if (n != PREF_SIZE) {
            fprintf(stderr, "Warning: pref file size mismatch (%zu != %d), skipping update\n",
                    n, PREF_SIZE);
            return -1;
        }
    } else {
        /* No prefs file — create one with zeros (defaults) */
        memset(buf, 0, PREF_SIZE);
    }

    /* Write the delay value at its known offset */
    memcpy(buf + DELAY_OFFSET, &delay_ms, sizeof(double));

    f = fopen(PREF_FILE, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot write %s\n", PREF_FILE);
        return -1;
    }
    fwrite(buf, 1, PREF_SIZE, f);
    fclose(f);
    return 0;
}

int main(int argc, char *argv[])
{
    playback_state_t play_state = {0};
    record_state_t   rec_state  = {0};
    SDL_AudioDeviceID play_dev, cap_dev;
    SDL_AudioSpec play_want, play_have;
    SDL_AudioSpec cap_want,  cap_have;

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Set up recording buffer */
    rec_state.max_frames = RECORD_FRAMES;
    rec_state.buffer = (float *)calloc(RECORD_FRAMES, sizeof(float));
    rec_state.active = 0;

    /* Open capture device (microphone) */
    memset(&cap_want, 0, sizeof(cap_want));
    cap_want.freq     = SAMPLE_RATE;
    cap_want.format   = AUDIO_F32SYS;
    cap_want.channels = CHANNELS;
    cap_want.samples  = 256;
    cap_want.callback = capture_callback;
    cap_want.userdata = &rec_state;

    cap_dev = SDL_OpenAudioDevice(NULL, 1, &cap_want, &cap_have, 0);
    if (cap_dev == 0) {
        fprintf(stderr, "Cannot open microphone: %s\n", SDL_GetError());
        free(rec_state.buffer);
        SDL_Quit();
        return 1;
    }

    /* Open playback device */
    memset(&play_want, 0, sizeof(play_want));
    play_want.freq     = SAMPLE_RATE;
    play_want.format   = AUDIO_F32SYS;
    play_want.channels = CHANNELS;
    play_want.samples  = 256;
    play_want.callback = playback_callback;
    play_want.userdata = &play_state;

    play_dev = SDL_OpenAudioDevice(NULL, 0, &play_want, &play_have, 0);
    if (play_dev == 0) {
        fprintf(stderr, "Cannot open audio output: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(cap_dev);
        free(rec_state.buffer);
        SDL_Quit();
        return 1;
    }

    printf("Playing impulse...\n");

    /* Start capture first, then playback */
    rec_state.active = 1;
    SDL_PauseAudioDevice(cap_dev, 0);
    SDL_PauseAudioDevice(play_dev, 0);

    /* Wait for recording to complete */
    while (rec_state.frames_written < rec_state.max_frames) {
        SDL_Delay(10);
    }

    SDL_PauseAudioDevice(play_dev, 1);
    SDL_PauseAudioDevice(cap_dev, 1);

    /* Find the impulse in the recording */
    double delay_ms = find_impulse(rec_state.buffer, rec_state.frames_written);

    if (delay_ms < 0.0) {
        printf("No impulse detected. Try increasing speaker volume.\n");
    } else {
        printf("Detected at %.1f ms\n", delay_ms);
        if (update_prefs(delay_ms) == 0)
            printf("Updated %s\n", PREF_FILE);
    }

    /* Cleanup */
    SDL_CloseAudioDevice(play_dev);
    SDL_CloseAudioDevice(cap_dev);
    free(rec_state.buffer);
    SDL_Quit();

    return (delay_ms < 0.0) ? 1 : 0;
}
