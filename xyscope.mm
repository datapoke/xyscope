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
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <fftw3.h>
#include <mmsystem.h>
#include <process.h>
#include <io.h>
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
#ifndef _WIN32
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "xyscope-shared.h"
#include "xyscope-ringbuffer.h"
#include "xyscope-draw.h"
#include "xyscope-hdr.h"

#ifdef _WIN32
/* ---- Windows compatibility layer ---- */
/* timeval, gettimeofday, timespec, clock_gettime, bzero now in xyscope-shared.h */

typedef unsigned int useconds_t;
#define usleep(us) Sleep(((us) + 999) / 1000)
#pragma comment(lib, "winmm.lib")

#define open _open
#define read(fd, buf, n) _read((fd), (buf), (unsigned int)(n))
#define write(fd, buf, n) _write((fd), (buf), (unsigned int)(n))
#define close _close

/* pthreads compatibility using Win32 primitives */
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;
typedef HANDLE pthread_t;

#define PTHREAD_CANCEL_ASYNCHRONOUS 0
#define pthread_setcanceltype(t, o) ((void)0)

static inline int pthread_mutex_lock(pthread_mutex_t *m) { EnterCriticalSection(m); return 0; }
static inline int pthread_mutex_trylock(pthread_mutex_t *m) { return TryEnterCriticalSection(m) ? 0 : 1; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(m); return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }

static int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long ms = (long)((abstime->tv_sec - now.tv_sec) * 1000 +
                      abstime->tv_nsec / 1000000 - now.tv_usec / 1000);
    if (ms < 0) ms = 0;
    if (ms > 1000) ms = 1000;
    SleepConditionVariableCS(c, m, (DWORD)ms);
    return 0;
}

struct w32_thread_info {
    void *(*func)(void *);
    void *arg;
};

static unsigned __stdcall w32_thread_entry(void *p) {
    struct w32_thread_info *info = (struct w32_thread_info *)p;
    void *(*func)(void *) = info->func;
    void *arg = info->arg;
    free(info);
    func(arg);
    return 0;
}

static int pthread_create(pthread_t *t, const void *attr, void *(*func)(void *), void *arg) {
    (void)attr;
    struct w32_thread_info *info = (struct w32_thread_info *)malloc(sizeof(struct w32_thread_info));
    info->func = func;
    info->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, w32_thread_entry, info, 0, NULL);
    return *t ? 0 : -1;
}

/* WASAPI COM GUIDs (explicit for MSVC and MinGW compatibility) */
static const GUID XYSCOPE_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID XYSCOPE_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID XYSCOPE_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID XYSCOPE_IID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48a0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};

#endif /* _WIN32 */

#ifdef _WIN32
/* Forward declaration — defined after scene class */
extern HDC hdr_hdc;
#endif

/* Constants now in xyscope-shared.h */

/* Audio sample rate and display frame rate — detected at runtime */
static int sample_rate = 96000;
static int frame_rate  = 120;

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
static int frames_per_buf;
static int draw_frames;
static int default_rb_size;

static void compute_derived_rates() {
    frames_per_buf  = (sample_rate / frame_rate) * DRAW_EACH_FRAME;
    draw_frames     = frames_per_buf + 1;
    default_rb_size = (int)(sample_rate * BUFFER_SECONDS + frames_per_buf);
}



/* sample_t, ringbuffer_t now in shared headers */

typedef struct _thread_data {
    pthread_t thread_id;
#ifdef __APPLE__
    AudioComponentInstance audio_unit;
    AudioDeviceID audio_device;
#elif defined(_WIN32)
    void *audio_client;       /* IAudioClient* */
    void *capture_client;     /* IAudioCaptureClient* */
    unsigned int wasapi_channels;
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
    volatile int negotiated_sample_rate;
    timeval last_write;
} thread_data_t;

/* frame_t now in xyscope-shared.h */

thread_data_t Thread_Data;

#define LEFT_PORT  0
#define RIGHT_PORT 1

#define TIMED true
#define NOT_TIMED false

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))
#define sign(A) ((A) < 0.0 ? -1.0 : 1.0)



/* timeDiff() now in xyscope-shared.h */

/* Ringbuffer functions now in xyscope-ringbuffer.h */

/* Signal reader thread that data is ready */
static inline void signal_data_ready(thread_data_t *t_data)
{
    if (pthread_mutex_trylock(&t_data->ringbuffer_lock) == 0) {
        pthread_cond_signal(&t_data->data_ready);
        pthread_mutex_unlock(&t_data->ringbuffer_lock);
    }
}

#ifdef _WIN32
/* Release WASAPI COM interfaces and clear pointers */
static void teardownWasapiLoopback(thread_data_t *t_data)
{
    if (t_data->audio_client) {
        ((IAudioClient *)t_data->audio_client)->Stop();
        ((IAudioClient *)t_data->audio_client)->Release();
        t_data->audio_client = NULL;
    }
    if (t_data->capture_client) {
        ((IAudioCaptureClient *)t_data->capture_client)->Release();
        t_data->capture_client = NULL;
    }
}

/* Initialize (or reinitialize) WASAPI loopback capture.
 * Returns true on success.  On failure, audio_client and
 * capture_client are left NULL. */
static bool initWasapiLoopback(thread_data_t *t_data, bool verbose)
{
    teardownWasapiLoopback(t_data);

    if (verbose)
        printf("Setting up WASAPI loopback capture...\n");

    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT hr = CoCreateInstance(XYSCOPE_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  XYSCOPE_IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr))
        return false;

    IMMDevice *device = NULL;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr))
        return false;

    /* Print the device name */
    if (verbose) {
        IPropertyStore *props = NULL;
        device->OpenPropertyStore(STGM_READ, &props);
        if (props) {
            PROPVARIANT name;
            PropVariantInit(&name);
            props->GetValue(PKEY_Device_FriendlyName, &name);
            if (name.vt == VT_LPWSTR) {
                char narrow[256];
                WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1,
                                    narrow, sizeof(narrow), NULL, NULL);
                printf("Using audio device: %s\n", narrow);
            }
            PropVariantClear(&name);
            props->Release();
        }
    }

    IAudioClient *audio_client = NULL;
    hr = device->Activate(XYSCOPE_IID_IAudioClient, CLSCTX_ALL,
                          NULL, (void **)&audio_client);
    device->Release();
    if (FAILED(hr))
        return false;

    WAVEFORMATEX *mix_format = NULL;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr)) {
        audio_client->Release();
        return false;
    }

    if (verbose)
        printf("WASAPI format: %d Hz, %d channels, %d bits\n",
               (int)mix_format->nSamplesPerSec, (int)mix_format->nChannels,
               (int)mix_format->wBitsPerSample);

    t_data->wasapi_channels = mix_format->nChannels;

    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK,
                                   0, 0, mix_format, NULL);
    CoTaskMemFree(mix_format);
    if (FAILED(hr)) {
        audio_client->Release();
        return false;
    }

    IAudioCaptureClient *capture_client = NULL;
    hr = audio_client->GetService(XYSCOPE_IID_IAudioCaptureClient,
                                   (void **)&capture_client);
    if (FAILED(hr)) {
        audio_client->Release();
        return false;
    }

    t_data->audio_client = audio_client;
    t_data->capture_client = capture_client;

    hr = audio_client->Start();
    if (FAILED(hr)) {
        teardownWasapiLoopback(t_data);
        return false;
    }

    if (verbose)
        printf("WASAPI loopback initialized successfully\n");
    return true;
}
#endif /* _WIN32 */

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

static OSStatus onSampleRateChanged(AudioObjectID inObjectID,
                                    UInt32 inNumberAddresses,
                                    const AudioObjectPropertyAddress inAddresses[],
                                    void *inClientData) {
    thread_data_t *t_data = (thread_data_t *)inClientData;
    Float64 newRate = 0;
    UInt32 rateSize = sizeof(newRate);
    AudioObjectPropertyAddress rateAddress = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(inObjectID, &rateAddress, 0, NULL,
                                   &rateSize, &newRate) == noErr && newRate > 0) {
        t_data->negotiated_sample_rate = (int)newRate;
    }
    return noErr;
}
#elif !defined(_WIN32)

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

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    thread_data_t *t_data = (thread_data_t *)userdata;
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info_raw info;
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) {
        t_data->negotiated_sample_rate = info.rate;
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

#endif

/* Detect the audio device's sample rate */
#ifdef __APPLE__
static int detect_sample_rate() {
    AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 dataSize = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize);
    if (dataSize == 0) return 96000;

    UInt32 deviceCount = dataSize / sizeof(AudioDeviceID);
    AudioDeviceID *devices = (AudioDeviceID*)malloc(dataSize);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &dataSize, devices);

    for (UInt32 i = 0; i < deviceCount; i++) {
        CFStringRef deviceName = NULL;
        UInt32 nameSize = sizeof(deviceName);
        AudioObjectPropertyAddress nameAddress = {
            kAudioDevicePropertyDeviceNameCFString,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectGetPropertyData(devices[i], &nameAddress, 0, NULL, &nameSize, &deviceName);
        if (deviceName) {
            char name[256];
            CFStringGetCString(deviceName, name, sizeof(name), kCFStringEncodingUTF8);
            if (strstr(name, "BlackHole") != NULL) {
                CFRelease(deviceName);
                Float64 nominalRate = 0;
                UInt32 rateSize = sizeof(nominalRate);
                AudioObjectPropertyAddress rateAddress = {
                    kAudioDevicePropertyNominalSampleRate,
                    kAudioObjectPropertyScopeGlobal,
                    kAudioObjectPropertyElementMain
                };
                if (AudioObjectGetPropertyData(devices[i], &rateAddress, 0, NULL, &rateSize, &nominalRate) == noErr
                    && nominalRate > 0) {
                    printf("BlackHole device sample rate: %.0f Hz\n", nominalRate);
                    free(devices);
                    return (int)nominalRate;
                }
                break;
            }
            CFRelease(deviceName);
        }
    }
    free(devices);
    printf("BlackHole device not found for rate detection, using default\n");
    return 96000;
}
#elif defined(_WIN32)
static int detect_sample_rate() {
    int rate = 48000;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT hr = CoCreateInstance(XYSCOPE_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  XYSCOPE_IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (SUCCEEDED(hr)) {
        IMMDevice *device = NULL;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) {
            IAudioClient *client = NULL;
            hr = device->Activate(XYSCOPE_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
            if (SUCCEEDED(hr)) {
                WAVEFORMATEX *mix_format = NULL;
                hr = client->GetMixFormat(&mix_format);
                if (SUCCEEDED(hr)) {
                    rate = (int)mix_format->nSamplesPerSec;
                    printf("WASAPI device sample rate: %d Hz\n", rate);
                    CoTaskMemFree(mix_format);
                }
                client->Release();
            }
            device->Release();
        }
        enumerator->Release();
    }
    CoUninitialize();
    return rate;
}
#else
static int detect_sample_rate() {
    printf("Using default sample rate (Pipewire will negotiate)\n");
    return 48000;
}
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
#ifdef _WIN32
        InitializeCriticalSection(&Thread_Data.ringbuffer_lock);
        InitializeConditionVariable(&Thread_Data.data_ready);
#else
        pthread_mutex_t ringbuffer_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t data_ready       = PTHREAD_COND_INITIALIZER;
        Thread_Data.ringbuffer_lock     = ringbuffer_lock;
        Thread_Data.data_ready          = data_ready;
#endif
        quit = false;
        pthread_create(&capture_thread, NULL, readerThread, (void *)this);
    }
    ~audioInput()
    {
        thread_data_t *t_data = getThreadData();
#ifdef __APPLE__
        if (t_data->audio_device) {
            AudioObjectPropertyAddress rateAddress = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectRemovePropertyListener(t_data->audio_device, &rateAddress,
                                              onSampleRateChanged, t_data);
        }
        if (t_data->audio_unit) {
            AudioOutputUnitStop(t_data->audio_unit);
            AudioUnitUninitialize(t_data->audio_unit);
            AudioComponentInstanceDispose(t_data->audio_unit);
        }
#elif defined(_WIN32)
        teardownWasapiLoopback(t_data);
        DeleteCriticalSection(&t_data->ringbuffer_lock);
        CoUninitialize();
#else
        if (t_data->stream) {
            pw_thread_loop_lock(t_data->loop);
            pw_stream_destroy(t_data->stream);
            pw_thread_loop_unlock(t_data->loop);
        }

        if (t_data->loop) {
            pw_thread_loop_stop(t_data->loop);
            pw_thread_loop_destroy(t_data->loop);
        }

        pw_deinit();
#endif
        ringbuffer_free(t_data->ringbuffer);
    }

    static void* readerThread(void* arg)
    {
        audioInput* ai = (audioInput *)arg;
        thread_data_t *t_data = ai->getThreadData();

        t_data->thread_id = ai->capture_thread;
        t_data->input_buffer = NULL;
        t_data->frame_size = sizeof(frame_t);
        t_data->rb_size = default_rb_size;
        t_data->channels = 2;
        t_data->can_process = false;
        t_data->pause_scope = false;
        gettimeofday(&t_data->last_write, NULL);

#ifdef __APPLE__
        ai->setupPorts();
#elif defined(_WIN32)
        ai->setupPorts();
        t_data->can_process = true;
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

#ifdef _WIN32
        /* WASAPI capture loop - poll for audio data.
         *
         * Reconnect on fatal HRESULT errors from the capture client,
         * and periodically health-check the audio client itself
         * (GetBufferSize returns AUDCLNT_E_DEVICE_INVALIDATED on a
         * dead session even when GetNextPacketSize still returns S_OK).
         */
        #define WASAPI_FATAL(hr) \
            ((hr) == (HRESULT)0x88890004 /* AUDCLNT_E_DEVICE_INVALIDATED */ || \
             (hr) == (HRESULT)0x88890010 /* AUDCLNT_E_SERVICE_NOT_RUNNING */ || \
             (hr) == (HRESULT)0x8889000F /* AUDCLNT_E_ENDPOINT_CREATE_FAILED */ )
        {
        DWORD backoff_ms = 500;
        DWORD last_health_check = GetTickCount();
        while (!ai->quit) {
            IAudioCaptureClient *capture = (IAudioCaptureClient *)t_data->capture_client;
            if (!capture) {
                Sleep(backoff_ms);
                if (!initWasapiLoopback(t_data, false)) {
                    if (backoff_ms < 5000) backoff_ms *= 2;
                    continue;
                }
                backoff_ms = 500;
                capture = (IAudioCaptureClient *)t_data->capture_client;
                last_health_check = GetTickCount();
            }

            Sleep(1);
            UINT32 packet_length = 0;
            HRESULT hr = capture->GetNextPacketSize(&packet_length);

            if (WASAPI_FATAL(hr)) {
                teardownWasapiLoopback(t_data);
                continue;
            }

            /* Health-check the IAudioClient every ~1s.
             * GetNextPacketSize on the capture client can return S_OK
             * even when the session is dead; GetBufferSize on the
             * audio client catches it. */
            if ((GetTickCount() - last_health_check) > 1000) {
                last_health_check = GetTickCount();
                IAudioClient *ac = (IAudioClient *)t_data->audio_client;
                if (ac) {
                    UINT32 buf_sz = 0;
                    HRESULT hc = ac->GetBufferSize(&buf_sz);
                    if (WASAPI_FATAL(hc)) {
                        teardownWasapiLoopback(t_data);
                        continue;
                    }
                }
            }

            /* No data available -- keep polling */
            if (FAILED(hr) || packet_length == 0) {
                if (packet_length == 0 && !t_data->pause_scope && t_data->can_process) {
                    gettimeofday(&t_data->last_write, NULL);
                    signal_data_ready(t_data);
                }
                continue;
            }

            while (packet_length > 0) {
                BYTE *data = NULL;
                UINT32 num_frames = 0;
                DWORD flags = 0;

                hr = capture->GetBuffer(&data, &num_frames, &flags, NULL, NULL);
                if (WASAPI_FATAL(hr)) {
                    teardownWasapiLoopback(t_data);
                    break;
                }
                if (FAILED(hr)) break;

                if (!t_data->pause_scope && t_data->can_process) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        for (UINT32 i = 0; i < num_frames; i++) {
                            frame_t frame = {0.0f, 0.0f};
                            ringbuffer_write(t_data->ringbuffer,
                                            (const char *)&frame, t_data->frame_size);
                        }
                    } else {
                        float *samples = (float *)data;
                        unsigned int ch = t_data->wasapi_channels;
                        for (UINT32 i = 0; i < num_frames; i++) {
                            frame_t frame;
                            frame.left_channel = samples[i * ch];
                            frame.right_channel = (ch > 1) ? samples[i * ch + 1] : samples[i * ch];
                            ringbuffer_write(t_data->ringbuffer,
                                            (const char *)&frame, t_data->frame_size);
                        }
                    }
                    gettimeofday(&t_data->last_write, NULL);
                    signal_data_ready(t_data);
                }

                capture->ReleaseBuffer(num_frames);
                hr = capture->GetNextPacketSize(&packet_length);
                if (WASAPI_FATAL(hr)) {
                    teardownWasapiLoopback(t_data);
                    break;
                }
                if (FAILED(hr)) break;
            }
        }
        }
#else
        while (!ai->quit) {
            usleep(1000);
        }
#endif
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
            t_data->input_buffer[i] = (sample_t *)malloc(frames_per_buf * sizeof(sample_t));
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

        // Store device ID and listen for sample rate changes
        t_data->audio_device = blackholeDevice;
        {
            AudioObjectPropertyAddress rateAddress = {
                kAudioDevicePropertyNominalSampleRate,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            AudioObjectAddPropertyListener(blackholeDevice, &rateAddress,
                                           onSampleRateChanged, t_data);
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
        streamFormat.mSampleRate = sample_rate;
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
#elif defined(_WIN32)
        bzero(t_data->input_buffer, input_buffer_size);

        CoInitializeEx(NULL, COINIT_MULTITHREADED);

        if (!initWasapiLoopback(t_data, true)) {
            fprintf(stderr, "Error: WASAPI loopback initialization failed\n");
            exit(1);
        }
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
        info.rate = 0;  /* Let Pipewire negotiate native rate */
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

/* preferences_t now in xyscope-shared.h (with audio_delay/display_delay fields) */

// Global SDL variables (declared here so scene class can access them)
extern TTF_Font *font;
extern SDL_Window *window;
extern SDL_GLContext gl_context;

/* wrapValue(), normalizeHue() now in xyscope-shared.h */

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

    #define NUM_TEXT_TIMERS 14
    #define NUM_AUTO_TEXT_TIMERS 11
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
        ColorModeTimer   = 3, 
        DisplayModeTimer = 4,
        ColorRangeTimer  = 5,
        ColorRateTimer   = 6,
        SampleRateTimer  = 7,
        FrameRateTimer   = 8,
        DelayTimer       = 9,
        BrightnessTimer  = 10,
        /* End of text timers automatically included in stats display */
        PausedTimer      = 11,
        ScaleTimer       = 12,
        CounterTimer     = 13
    } text_timer_handles;
    text_timer_t text_timer[NUM_TEXT_TIMERS];
    timeval show_intro_time;
    timeval last_frame_time;
    timeval reset_frame_time;
    timeval mouse_dirty_time;

    /* Color/display mode enums now in xyscope-shared.h */
    #define NUM_COLOR_MODES 2
    #define NUM_DISPLAY_MODES 5
    static const unsigned int DefaultColorMode   = DEFAULT_COLOR_MODE;
    static const unsigned int DefaultDisplayMode  = DEFAULT_DISPLAY_MODE;
    const char *color_mode_names[NUM_COLOR_MODES] = {"Standard", "Delta"};
    const char *display_mode_names[NUM_DISPLAY_MODES] = {
        "Standard", "Radius", "Length", "Frequency", "Time"
    };

    scene()
    {
        frame_size           = sizeof(frame_t);
        framebuf             = NULL;
        ai                   = NULL;
        offset               = 0;
        bump                 = 0;
        bytes_per_buf        = 0;
        for (int i = 0; i < 2; i++) {
            prefs.dim[i] = prefs.normal_dim[i] = prefs.old_dim[i] = 600;
            prefs.position[i] = 100;
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
        prefs.delay          = 0.0;
        prefs.audio_delay    = 0.0;
        prefs.display_delay  = 0.0;
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
        show_intro           = true;
        show_help            = false;
        show_mouse           = true;

        bzero(&text_timer, sizeof(text_timer_t) * NUM_TEXT_TIMERS);
        timeval now;
        gettimeofday(&now, NULL);
        show_intro_time = last_frame_time = reset_frame_time = mouse_dirty_time = now;

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
    }

    void init()
    {
        bytes_per_buf        = draw_frames * frame_size;
        framebuf             = (frame_t *) malloc(bytes_per_buf);
        offset               = -frames_per_buf;
        bump                 = -draw_frames;
#ifdef __APPLE__
        int log2n = 0;
        int n = draw_frames;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup            = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp        = (float *) malloc(draw_frames/2 * sizeof(float));
        fft_out.imagp        = (float *) malloc(draw_frames/2 * sizeof(float));
#else
        fft_out              = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * draw_frames);
#endif
        ai = new audioInput();
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
        int FH;
        if ((FH = open(DEFAULT_PREF_FILE, O_CREAT | O_WRONLY | O_TRUNC, 00660)) != -1) {
            write(FH, (void *) &prefs, sizeof(preferences_t));
            close(FH);
        }
        free(framebuf);
    }

    void drawPlot()
    {
        thread_data_t *t_data = ai->getThreadData();
        size_t bytes_ready = 0, bytes_read = 0;
        double dt  = 0.0;
        signed int distance = 0;

        /* FFT stuff */
        unsigned int window_size  = draw_frames / 100;
        unsigned int overlap_size = draw_frames / 200;
        double max_magnitude = 0.0;
        double* avg_magnitudes = NULL;
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
                ts.tv_sec += 1;
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
        glLineWidth((GLfloat) prefs.line_width);

        /* FFT setup for frequency mode (must happen before glBegin) */
        if (prefs.display_mode == DisplayFrequencyMode) {
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

        glBegin(GL_LINE_STRIP);

        /* display framebuf contents */
        vertex_count = draw_xy_vertices(
            framebuf, frames_read,
            prefs.display_mode, prefs.color_mode,
            prefs.hue, prefs.color_range, prefs.scale_factor,
            prefs.spline_steps,
            (prefs.display_mode == DisplayFrequencyMode) ? avg_magnitudes : NULL,
            window_size, overlap_size, max_magnitude,
            prefs.brightness);

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
        unsigned int n_items =  18;

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
        { "j/k and J/K",       "Adjust display delay" },
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
            snprintf(vps_string, sizeof(vps_string), "%d vps", vertex_count * frame_rate);
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

    void showAutoScale(bool t) { showTimedText(AutoScaleTimer, true, t, "Auto-scale: %s", prefs.auto_scale ? "on" : "off"); }
    void showSplines(bool t) { showTimedText(SplineTimer, true, t, "Splines: %d", prefs.spline_steps); }
    void showLineWidth(bool t) { showTimedText(LineWidthTimer, true, t, "Line width: %d", prefs.line_width); }
    void showColorMode(bool t) { showTimedText(ColorModeTimer, true, t, "Color mode: %s", color_mode_names[prefs.color_mode]); }
    void showDisplayMode(bool t) { showTimedText(DisplayModeTimer, true, t, "Display mode: %s", display_mode_names[prefs.display_mode]); }
    void showColorRange(bool t) { showTimedText(ColorRangeTimer, true, t, "Color range: %.2f", prefs.color_range); }
    void showColorRate(bool t) { showTimedText(ColorRateTimer, true, t, "Color rate: %.2f", prefs.color_rate); }
    void showSampleRate(bool t) { showTimedText(SampleRateTimer, true, t, "Sample rate: %d Hz", sample_rate); }
    void showFrameRate(bool t) { showTimedText(FrameRateTimer, true, t, "Frame rate: %d fps", frame_rate); }
    void showDelay(bool t) { showTimedText(DelayTimer, true, t, "Delay: %.2f ms", prefs.delay); }
    void showBrightness(bool t) {
#ifdef _WIN32
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f %s",
                      prefs.brightness, hdr_hdc ? "(HDR)" : "(SDR)");
#else
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f", prefs.brightness);
#endif
    }
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
            /* Restore normal window styles */
            {
                SDL_SysWMinfo wminfo;
                SDL_VERSION(&wminfo.version);
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    HWND hwnd = wminfo.info.win.window;
                    SetWindowLongPtr(hwnd, GWL_STYLE,
                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE);
                    SetWindowLongPtr(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
                    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
                }
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
                /* Set window styles that prevent the GPU driver from
                 * detecting this as a fullscreen-eligible window.
                 * Without this, the driver silently promotes to
                 * exclusive fullscreen on SwapBuffers, breaking
                 * overlays and audio.  (SDL#12791, SFML workaround) */
                SDL_SysWMinfo wminfo;
                SDL_VERSION(&wminfo.version);
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    HWND hwnd = wminfo.info.win.window;
                    SetWindowLongPtr(hwnd, GWL_STYLE,
                        WS_OVERLAPPED | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
                    SetWindowLongPtr(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
                    SetWindowPos(hwnd, HWND_TOP, 0, 0, mode.w, mode.h,
                        SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
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
        wrapValue(&prefs.color_range, 100.0);
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


    /* smooth(), HSVtoRGB() now free functions in xyscope-shared.h */
    /* map() renamed to map_value() in xyscope-shared.h */

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
        if (! scn.prefs.is_full_screen) {
            SDL_GetWindowSize(window, &scn.prefs.normal_dim[0], &scn.prefs.normal_dim[1]);
        }

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
        case 'i':
            scn.setBrightness(scn.getBrightness() + 0.1);
            break;
        case 'I':
            scn.setBrightness(scn.getBrightness() - 0.1);
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
#endif
TTF_Font *font = NULL;

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SDL_SetMainReady();
    timeBeginPeriod(1);
#endif
    int FH;

    // Load preferences
    if ((FH = open(DEFAULT_PREF_FILE, O_RDONLY)) != -1) {
        if (read(FH, (void *) &scn.prefs, sizeof(preferences_t))
            != sizeof(preferences_t))
        {
            fprintf(stderr, "Warning: pref file size mismatch, using defaults\n");
            scn = scene();
        }
        close(FH);
    }

    // Validate loaded preferences
    if (scn.prefs.normal_dim[0] < 1) scn.prefs.normal_dim[0] = 600;
    if (scn.prefs.normal_dim[1] < 1) scn.prefs.normal_dim[1] = 600;
    if (scn.prefs.display_mode >= NUM_DISPLAY_MODES)
        scn.prefs.display_mode = scene::DefaultDisplayMode;
    if (scn.prefs.color_mode >= NUM_COLOR_MODES)
        scn.prefs.color_mode = scene::DefaultColorMode;

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    
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
        if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 28);
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
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 1);
#endif

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

    glGenTextures(1, &scn.textures);

    // Set initial viewport
    int drawable_w, drawable_h;
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    reshape(drawable_w, drawable_h);

    if (scn.prefs.is_full_screen) {
        scn.setFullScreen();
    }

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

    if (scn.prefs.brightness <= 0.0) {
        double detected = detect_hdr_brightness();
        scn.prefs.brightness = (detected > 10.0) ? 10.0 : detected;
    }

    scn.showAutoScale(NOT_TIMED);
    scn.showSplines(NOT_TIMED);
    scn.showLineWidth(NOT_TIMED);
    scn.showColorMode(NOT_TIMED);
    scn.showDisplayMode(NOT_TIMED);
    scn.showColorRange(NOT_TIMED);
    scn.showColorRate(NOT_TIMED);
    scn.showSampleRate(NOT_TIMED);
    scn.showFrameRate(NOT_TIMED);
    scn.showDelay(NOT_TIMED);
    scn.showBrightness(NOT_TIMED);
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
    if (hdr_hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hdr_hglrc);
    } else
#endif
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return 0;
}
