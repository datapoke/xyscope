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
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#else
#include <GL/gl.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <fftw3.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


/* Preferences file */
#define DEFAULT_PREF_FILE ".xyscope.pref"

/* Default line width setting */
#define DEFAULT_LINE_WIDTH 1

/* Maximum line width setting */
#define MAX_LINE_WIDTH 8

/* Default full screen mode setting */
#define DEFAULT_FULL_SCREEN true

/* Default auto-scale setting */
#define DEFAULT_AUTO_SCALE true

/* Default spline steps setting */
#define DEFAULT_SPLINE_STEPS 32

/* Default color mode setting */
#define DEFAULT_COLOR_MODE ColorDeltaMode

/* Default color range setting */
#define DEFAULT_COLOR_RANGE 10.0

/* Default color rate setting */
#define DEFAULT_COLOR_RATE 0.0

/* Default display mode setting */
#define DEFAULT_DISPLAY_MODE DisplayFrequencyMode

/* Set this to your sample rate */
#define SAMPLE_RATE 96000

/* Set this to your desired Frames Per Second */
#define FRAME_RATE 60

/* ringbuffer size in seconds; expect memory usage to exceed:
 *
 * (SAMPLE_RATE * BUFFER_SECONDS + SAMPLE_RATE / FRAME_RATE) * sizeof(frame_t)
 *
 * e.g. (44100 * 60 + 44100 / 60) * 8 = 21173880 bytes or 20.2MB
 *
 * That being said, the custom ringbuffer will round up to the next
 * power of two, in the above case giving us a 32.0MB ringbuffer.
 */
#define BUFFER_SECONDS 60.0

/* How many times to draw each frame */
#define DRAW_EACH_FRAME 2

/* whether to limit frame rate */
#define RESPONSIBLE_FOR_FRAME_RATE true


/* End of easily configurable settings */


/* This must be at least SAMPLE_RATE / FRAME_RATE to draw every sample */
#define FRAMES_PER_BUF (SAMPLE_RATE / FRAME_RATE) * DRAW_EACH_FRAME

/* Connect the end-points with a line */
#define DRAW_FRAMES (FRAMES_PER_BUF + 1)

/* ringbuffer size in frames */
#define DEFAULT_RB_SIZE (SAMPLE_RATE * BUFFER_SECONDS + FRAMES_PER_BUF)


/* Audio types */
typedef float sample_t;

/* Custom ring buffer (used on both macOS and Linux) */
typedef struct {
    char *buf;
    size_t size;
    size_t write_ptr;
    size_t read_ptr;
} ringbuffer_t;

typedef struct _thread_data {
    pthread_t thread_id;
#ifdef __APPLE__
    AudioComponentInstance audio_unit;
#else
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
#endif
    sample_t **input_buffer;
    size_t frame_size;
    ringbuffer_t *ringbuffer;
    size_t rb_size;
    pthread_mutex_t ringbuffer_lock;
    pthread_cond_t data_ready;
    unsigned int channels;
    volatile bool can_process;
    volatile bool pause_scope;
    timeval last_write;
} thread_data_t;

typedef struct _frame_t {
    sample_t left_channel;
    sample_t right_channel;
} frame_t;

thread_data_t Thread_Data;

#define SQRT_TWO 1.41421356237309504880

#define LEFT_PORT  0
#define RIGHT_PORT 1

#define TIMED true
#define NOT_TIMED false

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))
#define sign(A) ((A) < 0.0 ? -1.0 : 1.0)



/* useful functions */

double timeDiff(timeval a, timeval b)
{
    return ((double) (b.tv_sec - a.tv_sec) +
            ((double) (b.tv_usec - a.tv_usec) * .000001));
}


/* Custom ring buffer implementation (used on both macOS and Linux) */
ringbuffer_t* ringbuffer_create(size_t size) {
    ringbuffer_t *rb = (ringbuffer_t*)malloc(sizeof(ringbuffer_t));
    size_t power_of_two = 1;
    while (power_of_two < size) power_of_two <<= 1;
    rb->size = power_of_two;
    rb->buf = (char*)malloc(rb->size);
    rb->write_ptr = 0;
    rb->read_ptr = 0;
    return rb;
}

void ringbuffer_free(ringbuffer_t *rb) {
    if (rb) {
        free(rb->buf);
        free(rb);
    }
}

size_t ringbuffer_write_space(ringbuffer_t *rb) {
    size_t w = rb->write_ptr;
    size_t r = rb->read_ptr;
    if (w > r) {
        return ((r - w + rb->size) & (rb->size - 1)) - 1;
    } else if (w < r) {
        return (r - w) - 1;
    } else {
        return rb->size - 1;
    }
}

size_t ringbuffer_read_space(ringbuffer_t *rb) {
    size_t w = rb->write_ptr;
    size_t r = rb->read_ptr;
    if (w > r) {
        return w - r;
    } else {
        return (w - r + rb->size) & (rb->size - 1);
    }
}

size_t ringbuffer_write(ringbuffer_t *rb, const char *src, size_t cnt) {
    size_t free_cnt;
    size_t cnt2;
    size_t to_write;
    size_t n1, n2;

    free_cnt = ringbuffer_write_space(rb);
    if (free_cnt == 0) return 0;

    to_write = cnt > free_cnt ? free_cnt : cnt;
    cnt2 = rb->write_ptr + to_write;

    if (cnt2 > rb->size) {
        n1 = rb->size - rb->write_ptr;
        n2 = cnt2 & (rb->size - 1);
    } else {
        n1 = to_write;
        n2 = 0;
    }

    memcpy(rb->buf + rb->write_ptr, src, n1);
    rb->write_ptr = (rb->write_ptr + n1) & (rb->size - 1);

    if (n2) {
        memcpy(rb->buf + rb->write_ptr, src + n1, n2);
        rb->write_ptr = (rb->write_ptr + n2) & (rb->size - 1);
    }

    return to_write;
}

size_t ringbuffer_read(ringbuffer_t *rb, char *dest, size_t cnt) {
    size_t free_cnt;
    size_t cnt2;
    size_t to_read;
    size_t n1, n2;

    free_cnt = ringbuffer_read_space(rb);
    if (free_cnt == 0) return 0;

    to_read = cnt > free_cnt ? free_cnt : cnt;
    cnt2 = rb->read_ptr + to_read;

    if (cnt2 > rb->size) {
        n1 = rb->size - rb->read_ptr;
        n2 = cnt2 & (rb->size - 1);
    } else {
        n1 = to_read;
        n2 = 0;
    }

    memcpy(dest, rb->buf + rb->read_ptr, n1);
    rb->read_ptr = (rb->read_ptr + n1) & (rb->size - 1);

    if (n2) {
        memcpy(dest + n1, rb->buf + rb->read_ptr, n2);
        rb->read_ptr = (rb->read_ptr + n2) & (rb->size - 1);
    }

    return to_read;
}

void ringbuffer_read_advance(ringbuffer_t *rb, size_t cnt) {
    rb->read_ptr = (rb->read_ptr + cnt) & (rb->size - 1);
}

/* Signal reader thread that data is ready */
static inline void signal_data_ready(thread_data_t *t_data)
{
    if (pthread_mutex_trylock(&t_data->ringbuffer_lock) == 0) {
        pthread_cond_signal(&t_data->data_ready);
        pthread_mutex_unlock(&t_data->ringbuffer_lock);
    }
}

#ifdef __APPLE__
/* CoreAudio input callback */
static OSStatus audioInputCallback(void *inRefCon,
                                   AudioUnitRenderActionFlags *ioActionFlags,
                                   const AudioTimeStamp *inTimeStamp,
                                   UInt32 inBusNumber,
                                   UInt32 inNumberFrames,
                                   AudioBufferList *ioData) {
    thread_data_t *t_data = (thread_data_t *)inRefCon;

    if (t_data->pause_scope || !t_data->can_process) {
        return noErr;
    }

    gettimeofday(&t_data->last_write, NULL);

    // Use pre-allocated buffers from input_buffer
    AudioBufferList bufferList;
    bufferList.mNumberBuffers = 2;
    bufferList.mBuffers[0].mNumberChannels = 1;
    bufferList.mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    bufferList.mBuffers[0].mData = t_data->input_buffer[0];
    bufferList.mBuffers[1].mNumberChannels = 1;
    bufferList.mBuffers[1].mDataByteSize = inNumberFrames * sizeof(float);
    bufferList.mBuffers[1].mData = t_data->input_buffer[1];

    // Render the audio
    OSStatus status = AudioUnitRender(t_data->audio_unit, ioActionFlags, inTimeStamp,
                                     inBusNumber, inNumberFrames, &bufferList);

    if (status != noErr)
        return status;

    float *leftSamples = (float *)t_data->input_buffer[0];
    float *rightSamples = (float *)t_data->input_buffer[1];

    // Write stereo frames to ringbuffer
    for (UInt32 i = 0; i < inNumberFrames; i++) {
        frame_t frame;
        frame.left_channel = leftSamples[i];
        frame.right_channel = rightSamples[i];
        ringbuffer_write(t_data->ringbuffer, (const char *)&frame, t_data->frame_size);
    }

    signal_data_ready(t_data);
    return noErr;
}
#else

/* Pipewire stream callback */
static void on_process(void *userdata)
{
    thread_data_t *t_data = (thread_data_t *)userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *samples;
    uint32_t n_frames;
    frame_t frame;

    /* Do nothing if the scope is paused or we are not ready. */
    if (t_data->pause_scope || !t_data->can_process)
        return;

    b = pw_stream_dequeue_buffer(t_data->stream);
    if (b == NULL)
        return;

    buf = b->buffer;
    if (buf->datas[0].data == NULL)
        return;

    gettimeofday(&t_data->last_write, NULL);

    /* Process interleaved stereo samples */
    samples = (float *)buf->datas[0].data;
    n_frames = buf->datas[0].chunk->size / (sizeof(float) * t_data->channels);

    for (uint32_t i = 0; i < n_frames; i++) {
        frame.left_channel = samples[i * t_data->channels];
        frame.right_channel = samples[i * t_data->channels + 1];
        ringbuffer_write(t_data->ringbuffer,
                        (const char *)&frame,
                        t_data->frame_size);
    }

    signal_data_ready(t_data);
    pw_stream_queue_buffer(t_data->stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

#endif

/* The audioInput object */

class audioInput
{
public:
    pthread_t capture_thread;
    bool quit;

    audioInput()
    {
        bzero(&Thread_Data, sizeof(Thread_Data));
        pthread_mutex_t ringbuffer_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t data_ready       = PTHREAD_COND_INITIALIZER;
        Thread_Data.ringbuffer_lock     = ringbuffer_lock;
        Thread_Data.data_ready          = data_ready;
        quit = false;
        pthread_create(&capture_thread, NULL, readerThread, (void *)this);
    }
    ~audioInput()
    {
#ifdef __APPLE__
        thread_data_t *t_data = getThreadData();
        if (t_data->audio_unit) {
            AudioOutputUnitStop(t_data->audio_unit);
            AudioUnitUninitialize(t_data->audio_unit);
            AudioComponentInstanceDispose(t_data->audio_unit);
        }
        ringbuffer_free(t_data->ringbuffer);
#else
        thread_data_t *t_data = getThreadData();

        if (t_data->stream) {
            pw_thread_loop_lock(t_data->loop);
            pw_stream_destroy(t_data->stream);
            pw_thread_loop_unlock(t_data->loop);
        }

        if (t_data->loop) {
            pw_thread_loop_stop(t_data->loop);
            pw_thread_loop_destroy(t_data->loop);
        }

        ringbuffer_free(t_data->ringbuffer);
        pw_deinit();
#endif
    }

    static void* readerThread(void* arg)
    {
        audioInput* ai = (audioInput *)arg;
        thread_data_t *t_data = ai->getThreadData();

        t_data->thread_id = ai->capture_thread;
        t_data->input_buffer = NULL;
        t_data->frame_size = sizeof(frame_t);
        t_data->rb_size = DEFAULT_RB_SIZE;
        t_data->channels = 2;
        t_data->can_process = false;
        t_data->pause_scope = false;
        gettimeofday(&t_data->last_write, NULL);

#ifdef __APPLE__
        ai->setupPorts();
#else
        t_data->ringbuffer = NULL;
        pw_init(NULL, NULL);

        t_data->loop = pw_thread_loop_new("xyscope", NULL);
        if (t_data->loop == NULL) {
            fprintf(stderr, "Failed to create Pipewire thread loop\n");
            exit(1);
        }

        ai->setupPorts();

        if (pw_thread_loop_start(t_data->loop) < 0) {
            fprintf(stderr, "Failed to start Pipewire thread loop\n");
            exit(1);
        }

        t_data->can_process = true;
#endif

        while (!ai->quit) {
            usleep(1000);
        }
        return ai;
    }

    void setupPorts()
    {
        thread_data_t *t_data = getThreadData();
        size_t input_buffer_size = t_data->channels * sizeof(sample_t *);

        // Common allocation for both platforms
        t_data->input_buffer = (sample_t **)malloc(input_buffer_size);
        t_data->ringbuffer = ringbuffer_create(t_data->frame_size * t_data->rb_size);
        bzero(t_data->ringbuffer->buf, t_data->ringbuffer->size);

#ifdef __APPLE__
        printf("Setting up CoreAudio input...\n");

        // macOS needs per-channel buffers for AudioUnitRender
        for (unsigned int i = 0; i < t_data->channels; i++) {
            t_data->input_buffer[i] = (sample_t *)malloc(FRAMES_PER_BUF * sizeof(sample_t));
        }

        // Find BlackHole device
        AudioDeviceID blackholeDevice = 0;
        AudioObjectPropertyAddress propertyAddress = {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        UInt32 dataSize = 0;
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
        UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
        AudioDeviceID *devices = (AudioDeviceID*)malloc(dataSize);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, devices);

        for (UInt32 i = 0; i < deviceCount; i++) {
            CFStringRef deviceName = NULL;
            dataSize = sizeof(deviceName);
            AudioObjectPropertyAddress nameAddress = {
                kAudioDevicePropertyDeviceNameCFString,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectGetPropertyData(devices[i], &nameAddress, 0, NULL, &dataSize, &deviceName);
            if (deviceName) {
                char name[256];
                CFStringGetCString(deviceName, name, sizeof(name), kCFStringEncodingUTF8);
                printf("Found audio device: %s\n", name);
                if (strstr(name, "BlackHole") != NULL) {
                    blackholeDevice = devices[i];
                    printf("Using BlackHole device for audio input\n");
                    CFRelease(deviceName);
                    break;
                }
                CFRelease(deviceName);
            }
        }
        free(devices);

        if (!blackholeDevice) {
            fprintf(stderr, "Error: BlackHole device not found!\n");
            fprintf(stderr, "Make sure BlackHole is installed (brew install blackhole-2ch) and Multi-Output Device is configured.\n");
            exit(1);
        }

        // Create Audio Unit for HAL Output (configured for input)
        AudioComponentDescription desc;
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        desc.componentFlags = 0;
        desc.componentFlagsMask = 0;

        AudioComponent component = AudioComponentFindNext(NULL, &desc);
        if (!component) {
            fprintf(stderr, "Error: Cannot find HAL output component\n");
            exit(1);
        }

        OSStatus status = AudioComponentInstanceNew(component, &t_data->audio_unit);
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot create audio unit: %d\n", status);
            exit(1);
        }

        // Enable IO for input
        UInt32 enableIO = 1;
        status = AudioUnitSetProperty(t_data->audio_unit,
                                     kAudioOutputUnitProperty_EnableIO,
                                     kAudioUnitScope_Input,
                                     1, // input bus
                                     &enableIO,
                                     sizeof(enableIO));
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot enable input IO: %d\n", status);
            exit(1);
        }

        // Disable IO for output
        enableIO = 0;
        status = AudioUnitSetProperty(t_data->audio_unit,
                                     kAudioOutputUnitProperty_EnableIO,
                                     kAudioUnitScope_Output,
                                     0, // output bus
                                     &enableIO,
                                     sizeof(enableIO));
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot disable output IO: %d\n", status);
            exit(1);
        }

        // Set current device to BlackHole
        status = AudioUnitSetProperty(t_data->audio_unit,
                                     kAudioOutputUnitProperty_CurrentDevice,
                                     kAudioUnitScope_Global,
                                     0,
                                     &blackholeDevice,
                                     sizeof(blackholeDevice));
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot set current device: %d\n", status);
            exit(1);
        }

        // Set format to stereo float
        AudioStreamBasicDescription streamFormat;
        streamFormat.mSampleRate = SAMPLE_RATE;
        streamFormat.mFormatID = kAudioFormatLinearPCM;
        streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        streamFormat.mFramesPerPacket = 1;
        streamFormat.mChannelsPerFrame = 2;
        streamFormat.mBitsPerChannel = 32;
        streamFormat.mBytesPerPacket = sizeof(float);
        streamFormat.mBytesPerFrame = sizeof(float);

        status = AudioUnitSetProperty(t_data->audio_unit,
                                     kAudioUnitProperty_StreamFormat,
                                     kAudioUnitScope_Output,
                                     1, // input bus
                                     &streamFormat,
                                     sizeof(streamFormat));
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot set stream format: %d\n", status);
            exit(1);
        }

        // Set up input callback
        AURenderCallbackStruct callbackStruct;
        callbackStruct.inputProc = audioInputCallback;
        callbackStruct.inputProcRefCon = t_data;

        status = AudioUnitSetProperty(t_data->audio_unit,
                                     kAudioOutputUnitProperty_SetInputCallback,
                                     kAudioUnitScope_Global,
                                     0,
                                     &callbackStruct,
                                     sizeof(callbackStruct));
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot set input callback: %d\n", status);
            exit(1);
        }

        // Initialize and start the audio unit
        status = AudioUnitInitialize(t_data->audio_unit);
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot initialize audio unit: %d\n", status);
            exit(1);
        }

        status = AudioOutputUnitStart(t_data->audio_unit);
        if (status != noErr) {
            fprintf(stderr, "Error: Cannot start audio unit: %d\n", status);
            exit(1);
        }

        printf("CoreAudio initialized successfully - reading from BlackHole device\n");
        t_data->can_process = true;
#else
        bzero(t_data->input_buffer, input_buffer_size);

        /* Create Pipewire stream */
        pw_thread_loop_lock(t_data->loop);

        t_data->stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(t_data->loop),
            "xyscope",
            pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio",
                PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Music",
                PW_KEY_STREAM_CAPTURE_SINK, "true",
                NULL),
            &stream_events,
            t_data);

        if (t_data->stream == NULL) {
            fprintf(stderr, "Failed to create Pipewire stream\n");
            pw_thread_loop_unlock(t_data->loop);
            exit(1);
        }

        /* Set up audio format parameters */
        uint8_t buffer[1024];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1];

        struct spa_audio_info_raw info = {};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = SAMPLE_RATE;
        info.channels = t_data->channels;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;  /* Front Left */
        info.position[1] = SPA_AUDIO_CHANNEL_FR;  /* Front Right */

        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

        /* Connect the stream */
        if (pw_stream_connect(t_data->stream,
                             PW_DIRECTION_INPUT,
                             PW_ID_ANY,
                             (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                   PW_STREAM_FLAG_RT_PROCESS),
                             params, 1) < 0) {
            fprintf(stderr, "Failed to connect Pipewire stream\n");
            pw_thread_loop_unlock(t_data->loop);
            exit(1);
        }

        pw_thread_loop_unlock(t_data->loop);
#endif
    }

    void quitNow()
    {
        quit = true;
    }

    /* accessor methods */
    thread_data_t *getThreadData(void)
    {
        return &Thread_Data;
    }
};



/* The scene object */

typedef struct _preferences_t {
    int dim[2];
    int normal_dim[2];
    int old_dim[2];
    int position[2];
    double side[4]; /* t, b, r, l */
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
} preferences_t;

// Global SDL variables (declared here so scene class can access them)
extern TTF_Font *font;
extern SDL_Window *window;
extern SDL_GLContext gl_context;

/* Helper functions for value wrapping and normalization */
static inline void wrapValue(double *val, double max) {
    if (*val > max) *val -= max * 2;
    if (*val <= -max) *val += max * 2;
}

static inline double normalizeHue(double h) {
    if (h > 360.0) h = fmod(h, 360.0);
    if (h < 0.0) h = 360.0 + fmod(h, 360.0);
    return h;
}

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

    #define NUM_TEXT_TIMERS 10
    #define NUM_AUTO_TEXT_TIMERS 7
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
        ColorModeTimer   = 2, 
        ColorRangeTimer  = 3,
        ColorRateTimer   = 4,
        DisplayModeTimer = 5,
        LineWidthTimer   = 6,
        /* End of text timers automatically included in stats display */
        PausedTimer      = 7,
        ScaleTimer       = 8,
        CounterTimer     = 9
    } text_timer_handles;
    text_timer_t text_timer[NUM_TEXT_TIMERS];
    timeval show_intro_time;
    timeval last_frame_time;
    timeval reset_frame_time;
    timeval mouse_dirty_time;

    #define NUM_COLOR_MODES 2
    enum {
        ColorStandardMode = 0,
        ColorDeltaMode    = 1
    } color_mode_handles;
    const char *color_mode_names[NUM_COLOR_MODES] = {"Standard", "Delta"};

    #define NUM_DISPLAY_MODES 5
    enum {
        DisplayStandardMode  = 0,
        DisplayRadiusMode    = 1,
        DisplayLengthMode    = 2,
        DisplayFrequencyMode = 3,
        DisplayTimeMode      = 4
    } display_mode_handles;
    const char *display_mode_names[NUM_DISPLAY_MODES] = {
        "Standard", "Radius", "Length", "Frequency", "Time"
    };

    scene()
    {
        frame_size           = sizeof(frame_t);
        bytes_per_buf        = DRAW_FRAMES * frame_size;
        framebuf             = (frame_t *) malloc(bytes_per_buf);
        offset               = -FRAMES_PER_BUF;
        bump                 = -DRAW_FRAMES;
#ifdef __APPLE__
        int log2n = 0;
        int n = DRAW_FRAMES;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup            = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp        = (float *) malloc(DRAW_FRAMES/2 * sizeof(float));
        fft_out.imagp        = (float *) malloc(DRAW_FRAMES/2 * sizeof(float));
#else
        fft_out              = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * DRAW_FRAMES);
#endif
        for (int i = 0; i < 2; i++) {
            prefs.dim[i] = prefs.normal_dim[i] = prefs.old_dim[i] = 600;
            prefs.position[i] = 0;
        }
        for (int i = 0; i < 4; i += 2) {
            prefs.side[i] = 1.0;
            prefs.side[i+1] = -1.0;
        }
        prefs.scale_factor   = 1.0;
        prefs.scale_locked   = true;
        prefs.is_full_screen = DEFAULT_FULL_SCREEN;
        prefs.auto_scale     = DEFAULT_AUTO_SCALE;
        prefs.spline_steps   = DEFAULT_SPLINE_STEPS;
        prefs.color_mode     = DEFAULT_COLOR_MODE;
        prefs.color_range    = DEFAULT_COLOR_RANGE;
        prefs.color_rate     = DEFAULT_COLOR_RATE;
        prefs.display_mode   = DEFAULT_DISPLAY_MODE;
        prefs.line_width     = DEFAULT_LINE_WIDTH;
        prefs.show_stats     = 0;
        prefs.hue            = 0.0;
        latency              = 0.0;
        fps                  = 0.0;
        frame_count          = 0;
        vertex_count         = 0;
        window_is_dirty      = true;
        mouse_is_dirty       = true;
        max_sample_value     = 1.0;
        top_offset           = -60.0;
        vertical_increment   = -60.0;
        color_delta          = 0.0;
        color_threshold      = 0.0;
        show_intro           = false;
        show_help            = false;
        show_mouse           = true;

        bzero(&text_timer, sizeof(text_timer_t) * NUM_TEXT_TIMERS);
        timeval now;
        gettimeofday(&now, NULL);
        show_intro_time = last_frame_time = reset_frame_time = mouse_dirty_time = now;

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];

        ai = new audioInput();
    }
    ~scene()
    {
        int FH;
        if ((FH = open(DEFAULT_PREF_FILE, O_CREAT | O_WRONLY, 00660))) {
            write(FH, (void *) &prefs, sizeof(preferences_t));
            close(FH);
        }
        free(framebuf);
    }

    void drawPlot()
    {
        thread_data_t *t_data = ai->getThreadData();
        size_t bytes_ready, bytes_read;
        double h   = -1.0;
        double s   = 1.0;
        double v   = 1.0;
        double r   = 1.0;
        double g   = 1.0;
        double b   = 1.0;
        double lc  = 0.0;
        double rc  = 0.0;
        double olc = 0.0;
        double orc = 0.0;
        double d   = 0.0;
        double dt  = 0.0;
        signed int distance = 0;

        /* FFT stuff */
        unsigned int window_size  = DRAW_FRAMES / 100;
        unsigned int overlap_size = DRAW_FRAMES / 200;
        double max_magnitude = 0.0;
        double* avg_magnitudes;
        double** stft_results;
#ifndef __APPLE__
        fftw_plan fft_plan;
#endif
        vertex_count = 0;

        /* if the scope is paused or audio not initialized, there are no samples available;
         * therefore we should not wait for the reader thread */
        if (! t_data->pause_scope && t_data->can_process) {
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
            pthread_mutex_lock(&t_data->ringbuffer_lock);

            // Use timed wait to avoid hanging forever if audio fails
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 16666666; // 16.6ms timeout (one frame at 60fps)
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&t_data->data_ready, &t_data->ringbuffer_lock, &ts);
        }


        /* Read data from the ring buffer */
        if (t_data->pause_scope) {
            distance = bump * frame_size;
            bump     = -DRAW_FRAMES;
        }
        else {
            bytes_ready = ringbuffer_read_space(t_data->ringbuffer);
            if (bytes_ready != bytes_per_buf)
                distance = bytes_ready - bytes_per_buf;
        }
        if (distance != 0)
            ringbuffer_read_advance(t_data->ringbuffer, distance);
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
        glLineWidth((GLfloat) prefs.line_width);
        glBegin(GL_LINE_STRIP);

        switch (prefs.display_mode) {
            case DisplayStandardMode:
                HSVtoRGB(&r, &g, &b, prefs.hue, s, v);
                glColor3d(r, g, b);
                break;
            case DisplayRadiusMode:
            case DisplayLengthMode:
                break;
            case DisplayFrequencyMode:
            {
                // Create the STFT array
                stft_results = new double*[frames_read / (window_size - overlap_size)];
                for (unsigned int i = 0; i < frames_read / (window_size - overlap_size); i++) {
                    stft_results[i] = new double[window_size];
                }
#ifdef __APPLE__
                // Set up vDSP FFT once outside loop (performance optimization)
                int log2n_win = 0;
                int n_win = window_size;
                while (n_win > 1) { n_win >>= 1; log2n_win++; }
                FFTSetup fft_setup_local = vDSP_create_fftsetup(log2n_win, FFT_RADIX2);
                DSPSplitComplex fft_data;
                fft_data.realp = new float[window_size/2];
                fft_data.imagp = new float[window_size/2];
#endif
                // Loop over the frame buffer
                for (unsigned int i = 0; i + window_size <= frames_read; i += window_size - overlap_size) {
#ifdef __APPLE__
                    // Copy the next window_size samples to a temporary array
                    float *input_data = new float[window_size];
                    for (unsigned int j = 0; j < window_size; j++) {
                        input_data[j] = (framebuf[i + j].left_channel + framebuf[i + j].right_channel) / 2.0;
                    }

                    // Convert real input to split complex (required by vDSP)
                    vDSP_ctoz((DSPComplex*)input_data, 2, &fft_data, 1, window_size/2);

                    // Perform FFT
                    vDSP_fft_zrip(fft_setup_local, &fft_data, 1, log2n_win, FFT_FORWARD);

                    // Store the FFT results in the STFT array
                    for (unsigned int j = 0; j < window_size/2; j++) {
                        double real = fft_data.realp[j];
                        double imag = fft_data.imagp[j];
                        stft_results[i / (window_size - overlap_size)][j] = sqrt(real * real + imag * imag);
                    }
                    // Mirror for second half (DC and Nyquist)
                    for (unsigned int j = window_size/2; j < window_size; j++) {
                        stft_results[i / (window_size - overlap_size)][j] = stft_results[i / (window_size - overlap_size)][window_size - j];
                    }

                    delete[] input_data;
#else
                    double (*temp_data)[2] = new double[window_size][2];
                    for (unsigned int j = 0; j < window_size; j++) {
                        // average both channels
                        temp_data[j][0] = (framebuf[i + j].left_channel + framebuf[i + j].right_channel) / 2.0; // Real part
                        temp_data[j][1] = 0.0; // Imaginary part
                    }

                    // Perform an FFT on the temporary array
                    fft_plan = fftw_plan_dft_1d(window_size, temp_data, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
                    fftw_execute(fft_plan);

                    // Store the FFT results in the STFT array
                    for (unsigned int j = 0; j < window_size; j++) {
                        double real = fft_out[j][0];
                        double imag = fft_out[j][1];
                        stft_results[i / (window_size - overlap_size)][j] = sqrt(real * real + imag * imag);
                    }

                    // Clean up the temporary array
                    fftw_destroy_plan(fft_plan);
                    delete[] temp_data;
#endif
                }
#ifdef __APPLE__
                // Clean up FFT resources after loop
                vDSP_destroy_fftsetup(fft_setup_local);
                delete[] fft_data.realp;
                delete[] fft_data.imagp;
#endif

                // Calculate the average magnitude of the STFT array
                avg_magnitudes = new double[frames_read / (window_size - overlap_size)];
                for (unsigned int i = 0; i < frames_read / (window_size - overlap_size); i++) {
                    double sum = 0.0;
                    for (unsigned int j = 0; j < window_size; j++) {
                        double magnitude = stft_results[i][j];
                        if (magnitude > max_magnitude) {
                            max_magnitude = magnitude;
                        }
                        sum += stft_results[i][j];
                    }
                    avg_magnitudes[i] = sum / window_size;
                    delete[] stft_results[i];
                }
                delete[] stft_results;
            }
                break;
            case DisplayTimeMode:
                break;
            default:
                break;
        };


        /* display framebuf contents */
        for (unsigned int i = 0; i < frames_read; i++) {
            lc = framebuf[i].left_channel;
            rc = framebuf[i].right_channel;
            d  = hypot(lc - olc, rc - orc) / SQRT_TWO;
            switch (prefs.color_mode) {
                case ColorStandardMode:
                    break;
                case ColorDeltaMode:
                    dt += d;
                    break;
            }
            switch (prefs.display_mode) {
                case DisplayStandardMode:
                    break;
                case DisplayRadiusMode:
                    h = ((hypot(lc, rc) / SQRT_TWO)
                         * 360.0 * prefs.color_range
                         * prefs.scale_factor) + prefs.hue;
                    break;
                case DisplayLengthMode:
                    h = (d * 360.0 * prefs.color_range) + prefs.hue;
                    if (h < prefs.hue) {
                        h = prefs.hue + 360.0 + h;
                        if (h < prefs.hue)
                            h = prefs.hue;
                    }
                    if (h > prefs.hue + 360.0)
                        h = prefs.hue + 360.0;
                    break;
                case DisplayFrequencyMode:
                    h = map(avg_magnitudes[i / (window_size - overlap_size)] * prefs.color_range,
                            0, max_magnitude, 0, 360) + prefs.hue;
                    break;
                case DisplayTimeMode:
                    h = (((double) i / (double) frames_read)
                         * 90.0 * prefs.color_range) + prefs.hue;
                    break;
                default:
                    break;
            };
            if (h > -1.0 && prefs.display_mode != DisplayStandardMode) {
                h = normalizeHue(h);
            }
            if (h > -1.0) {
                HSVtoRGB(&r, &g, &b, h, s, v);
                glColor3d(r, g, b);
            }
            if (prefs.spline_steps > 1 && i > 2 && i < frames_read - 2) {
                // Calculate Catmull-Rom spline segment
                double prev2_lc = framebuf[i-2].left_channel;
                double prev2_rc = framebuf[i-2].right_channel;
                double prev_lc  = framebuf[i-1].left_channel;
                double prev_rc  = framebuf[i-1].right_channel;
                double next_lc  = framebuf[i+1].left_channel;
                double next_rc  = framebuf[i+1].right_channel;
                double next2_lc = framebuf[i+2].left_channel;
                double next2_rc = framebuf[i+2].right_channel;
                for (double t = 0.0; t <= 1.0; t += 1.0 / (double) prefs.spline_steps) {
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
            olc = lc, orc = rc;
        }
        glEnd();
        glPopMatrix();
        if (prefs.display_mode == DisplayFrequencyMode)
            delete[] avg_magnitudes;

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

    double map(double value, double fromLow, double fromHigh, double toLow, double toHigh) {
        return (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow) + toLow;
    }

    void beginText()
    {
        top_offset = -120.0;
        if (text_timer[ScaleTimer].show)
            top_offset = -180.0;

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

        int text_width = 0;
        TTF_SizeText(font, string, &text_width, NULL);

        // Return pixel width
        return (double)text_width;
    }

    void drawString(double x, double y, char *string)
    {
        if (!font || !string || strlen(string) == 0) return;

        if (x >= 0.0)
            x = -1.0 + x / (double) prefs.dim[0];
        else
            x =  1.0 + x / (double) prefs.dim[0];

        if (y >= 0.0)
            y = -1.0 + y / (double) prefs.dim[1];
        else
            y =  1.0 + y / (double) prefs.dim[1];

        // Render text to surface
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

        // Create OpenGL texture from surface
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     rgba_surface->w, rgba_surface->h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_surface->pixels);

        // Calculate text width and height in normalized coordinates
        double text_w = (double)rgba_surface->w / (double)prefs.dim[0] * 2.0;
        double text_h = (double)rgba_surface->h / (double)prefs.dim[1] * 2.0;

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
        double left_offset   =  40.0;
        double right_offset  = 700.0;
        unsigned int n_items =  17;

        char help[][2][64] = {
        { "Escape",            "Quit" },
        { "F1 thru F5",        "Quickly resize window" },
        { "Home and Page Up",  "Zoom in" },
        { "End and Page Down", "Zoom out" },
        { "0 thru 9",          "Set zoom factor" },
        { "Spacebar",          "Pause/Resume" },
        { "< and >",           "Rewind/Fast-Forward when paused" },
        { "[ and ]",           "Adjust color range" },
        { "- and +",           "Adjust color rate" },
        { "a",                 "Auto-scale on/off" },
        { "b and B",           "Adjust splines" },
        { "c and C",           "Color mode" },
        { "d and D",           "Display mode" },
        { "f",                 "Enter/Exit full screen mode" },
        { "h",                 "Show/Hide help" },
        { "r",                 "Recenter" },
        { "s and S",           "Show/Hide statistics" },
        { "w and W",           "Adjust line width" }
        };

        for (unsigned int i = 0; i < n_items; i++) {
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
        double x = 40.0;
        gettimeofday(&this_frame_time, NULL);
        for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
            if (text_timer[i].show) {
                /* get the time so we can calculate how long to display */
                elapsed_time = timeDiff(text_timer[i].time,
                                         this_frame_time);
                if (elapsed_time > 10.0)
                    text_timer[i].show = false;

                if (text_timer[i].auto_position) {
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

        if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3)) {
            /* calculate framerate */
            gettimeofday(&this_frame_time, NULL);
            elapsed_time = timeDiff(reset_frame_time, this_frame_time);
            frame_count++;
            if (elapsed_time >= 1.0) {
                fps = frame_count / elapsed_time;
                reset_frame_time = this_frame_time;
                frame_count = 0;
            }
            last_frame_time = this_frame_time;
            snprintf(fps_string, sizeof(fps_string), "%.1f fps", fps);
            drawString(20.0, 20.0, fps_string);
            snprintf(vps_string, sizeof(vps_string), "%d vps", vertex_count * FRAME_RATE);
            double vps_width = getTextWidth(vps_string);
            drawString(-(vps_width + 240), -60.0, vps_string);
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
            double time_width = getTextWidth(time_string);
            drawString(-(time_width + 240), 20.0, time_string);
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
                         (double) offset / (double) SAMPLE_RATE
                         + (double) FRAMES_PER_BUF / (double) SAMPLE_RATE);
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

    void showAutoScale(bool t) { showTimedText(AutoScaleTimer, true, t, "Auto-scale: %s", prefs.auto_scale ? "on" : "off"); }
    void showSplines(bool t) { showTimedText(SplineTimer, true, t, "Splines: %d", prefs.spline_steps); }
    void showLineWidth(bool t) { showTimedText(LineWidthTimer, true, t, "Line width: %d", prefs.line_width); }
    void showColorMode(bool t) { showTimedText(ColorModeTimer, true, t, "Color mode: %s", color_mode_names[prefs.color_mode]); }
    void showDisplayMode(bool t) { showTimedText(DisplayModeTimer, true, t, "Display mode: %s", display_mode_names[prefs.display_mode]); }
    void showColorRange(bool t) { showTimedText(ColorRangeTimer, true, t, "Color range: %.2f", prefs.color_range); }
    void showColorRate(bool t) { showTimedText(ColorRateTimer, true, t, "Color rate: %.2f", prefs.color_rate); }
    void showPaused(bool t) { showTimedText(PausedTimer, true, t, "Paused"); }

    void showScale(bool timed)
    {
        text_timer_t *timer  = &text_timer[ScaleTimer];
        timer->auto_position = false;
        timer->x_position    =  40.0;
        timer->y_position    = -60.0;
        if (timed)
            gettimeofday(&timer->time, NULL);
        timer->show = true;
    }

    void showCounter(bool timed)
    {
        text_timer_t *timer  = &text_timer[CounterTimer];
        timer->auto_position = false;
        timer->x_position    = -400.0;
        timer->y_position    =   20.0;
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
        setSides(max_sample_value, 1);
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
        max_sample_value   = min((prefs.side[0] - prefs.side[1]) / 2.1,
                                  (prefs.side[2] - prefs.side[3]) / 2.1);
        prefs.auto_scale   = ! prefs.auto_scale;
        showAutoScale(TIMED);
    }

    void moreSplines(void)
    {
        if (prefs.spline_steps < 128)
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
            offset = -FRAMES_PER_BUF;
            bump   = -DRAW_FRAMES;
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
        showDisplayMode(TIMED);
    }

    void prevDisplayMode(void)
    {
        if (prefs.display_mode < 1)
            prefs.display_mode = NUM_DISPLAY_MODES - 1;
        else
            prefs.display_mode = prefs.display_mode - 1;
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
            if ((offset - FRAMES_PER_BUF * nbufs) >= -DEFAULT_RB_SIZE) {
                offset -= FRAMES_PER_BUF * nbufs;
                bump   -= FRAMES_PER_BUF * nbufs;
            }
            showCounter(TIMED);
        }
    }

    void fastForward(int nbufs)
    {
        thread_data_t *t_data = ai->getThreadData();
        if (t_data->pause_scope) {
            if (offset < -FRAMES_PER_BUF * nbufs) {
                offset += FRAMES_PER_BUF * nbufs;
                bump   += FRAMES_PER_BUF * nbufs;
            }
            showCounter(TIMED);
        }
    }

    /* accessor methods */

    void setWindowSize(unsigned int x, unsigned int y)
    {
        if (prefs.is_full_screen) {
            // Exit fullscreen mode first
            SDL_SetWindowFullscreen(window, 0);
        } else {
            SDL_GetWindowPosition(window, &prefs.position[0], &prefs.position[1]);
        }
        SDL_SetWindowPosition(window, prefs.position[0], prefs.position[1]);
        SDL_SetWindowSize(window, x, y);
        prefs.is_full_screen = false;
    }

    void setFullScreen(void)
    {
        if (! prefs.is_full_screen) {
            SDL_GetWindowPosition(window, &prefs.position[0], &prefs.position[1]);
            SDL_GetWindowSize(window, &prefs.normal_dim[0], &prefs.normal_dim[1]);
        }
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
        prefs.is_full_screen = true;
        show_mouse           = false;
        mouse_is_dirty       = true;
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
        wrapValue(&prefs.color_range, 100.0);
        showColorRange(TIMED);
    }

    void setColorRate(double rate)
    {
        prefs.color_rate = rate;
        wrapValue(&prefs.color_rate, 180.0);
        showColorRate(TIMED);
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


    /* useful functions */

    void smooth(double *a, double b, double s)
    {
        *a = *a + (b - *a) * s;
    }

    void HSVtoRGB(double *r, double *g, double *b,
                   double h, double s, double v)
    {
        int i;
        double f, p, q, t;

        if (s == 0) {
            // achromatic (grey)
            *r = *g = *b = v;
            return;
        }

        if (h >= 360.0)
            h -= 360.0;

        h /= 60;              // sector 0 to 5
        i = (int) floorf(h);
        f = h - i;            // factorial part of h
        p = v * (1 - s);
        q = v * (1 - s * f);
        t = v * (1 - s * (1 - f));

        switch (i) {
            case 0:
                *r = v;
                *g = t;
                *b = p;
                break;
            case 1:
                *r = q;
                *g = v;
                *b = p;
                break;
            case 2:
                *r = p;
                *g = v;
                *b = t;
                break;
            case 3:
                *r = p;
                *g = q;
                *b = v;
                break;
            case 4:
                *r = t;
                *g = p;
                *b = v;
                break;
            default:          // case 5:
                *r = v;
                *g = p;
                *b = q;
                break;
        }
    }
};
static scene scn;

void display()
{
    glClear(GL_COLOR_BUFFER_BIT);

    /* plot the samples on the screen */
    scn.drawPlot();

    /* draw any text that needs drawing */
    scn.drawText();

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
        int window_w, window_h;
        SDL_GetWindowSize(window, &window_w, &window_h);
        scn.prefs.normal_dim[0] = window_w;
        scn.prefs.normal_dim[1] = window_h;

        if (scn.prefs.scale_locked)
            scn.setSides(1.0 / scn.prefs.scale_factor, 1);
        else
            scn.rescale();
        glViewport(0, 0, drawable_w, drawable_h);
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
        /* limit our framerate to FRAME_RATE (e.g. 60) frames per second */
        elapsed_time = timeDiff(scn.reset_frame_time, scn.last_frame_time);
        if (elapsed_time < (scn.frame_count / (double) FRAME_RATE)) {
            double remainder = (scn.frame_count
                                / (double) FRAME_RATE - elapsed_time);
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
            scn.setWindowSize(300, 300);
            break;
        case 2:                    /* F2 */
            scn.setWindowSize(600, 600);
            break;
        case 3:                    /* F3 */
            scn.setWindowSize(800, 800);
            break;
        case 4:                    /* F4 */
            scn.setWindowSize(1000, 1000);
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
        case '0':
            scn.setZoom(pow(2.0, 9.0));
            break;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            /* atof ((const char *) &key); ? */
            scn.setZoom(pow(2.0, key - '1'));
            break;
        case ',':
            scn.rewind(1);
            break;
        case '.':
            scn.fastForward(1);
            break;
        case '<':
            scn.rewind(FRAME_RATE / DRAW_EACH_FRAME);
            break;
        case '>':
            scn.fastForward(FRAME_RATE / DRAW_EACH_FRAME);
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
        case 'b':
            scn.moreSplines();
            break;
        case 'B':
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
TTF_Font *font = NULL;

int main(int argc, char * const argv[])
{
    int FH;

    // Load preferences
    if ((FH = open(DEFAULT_PREF_FILE, O_RDONLY))) {
        read(FH, (void *) &scn.prefs, sizeof(preferences_t));
        close(FH);
    }

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    
    // Initialize SDL_ttf
    if (TTF_Init() < 0) {
        fprintf(stderr, "TTF_Init failed: %s\n", TTF_GetError());
    } else {
        font = TTF_OpenFont("/System/Library/Fonts/Monaco.ttf", 28);
        if (!font) font = TTF_OpenFont("/System/Library/Fonts/Courier.ttc", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", 28);
        if (!font) {
            fprintf(stderr, "Warning: Could not load font: %s\n", TTF_GetError());
        }
    }

    // Set OpenGL attributes
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Create window
    window = SDL_CreateWindow("XY Scope",
                              scn.prefs.position[0],
                              scn.prefs.position[1],
                              scn.prefs.normal_dim[0],
                              scn.prefs.normal_dim[1],
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Raise window and give it focus (important when launched from Terminal)
    SDL_RaiseWindow(window);

    // Create OpenGL context
    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        fprintf(stderr, "OpenGL context could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Enable VSync
    SDL_GL_SetSwapInterval(1);

    glGenTextures(1, &scn.textures);

    // Set initial viewport
    int drawable_w, drawable_h;
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    reshape(drawable_w, drawable_h);

    if (scn.prefs.is_full_screen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    scn.showDisplayMode(NOT_TIMED);
    scn.showLineWidth(NOT_TIMED);
    scn.showColorRange(NOT_TIMED);
    scn.showColorRate(NOT_TIMED);
    scn.showColorMode(NOT_TIMED);
    scn.showAutoScale(NOT_TIMED);
    scn.showSplines(NOT_TIMED);
    scn.showScale(NOT_TIMED);

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
                    // Regular ASCII keys - handle shift modifier
                    unsigned char ch = (unsigned char)key;

                    // Convert lowercase to uppercase if shift is held
                    if ((mod & KMOD_SHIFT) && ch >= 'a' && ch <= 'z') {
                        ch = ch - 'a' + 'A';
                    }
                    // Handle shifted number keys for special characters
                    else if (mod & KMOD_SHIFT) {
                        switch(ch) {
                            case '1': ch = '!'; break;
                            case '2': ch = '@'; break;
                            case '3': ch = '#'; break;
                            case '4': ch = '$'; break;
                            case '5': ch = '%'; break;
                            case '6': ch = '^'; break;
                            case '7': ch = '&'; break;
                            case '8': ch = '*'; break;
                            case '9': ch = '('; break;
                            case '0': ch = ')'; break;
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
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    reshape(event.window.data1, event.window.data2);
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

        // Idle processing
        idle();

        // Display
        display();

        // Swap buffers
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
