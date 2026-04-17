/*
 *  xyscope-audio.h
 *  Audio capture: thread_data, platform callbacks, audioInput class.
 *
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef XYSCOPE_AUDIO_H
#define XYSCOPE_AUDIO_H

#include "xyscope-shared.h"
#include "xyscope-ringbuffer.h"

#ifdef __APPLE__
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#elif defined(_WIN32)
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmsystem.h>
#include "xyscope-compat.h"
#else
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#ifndef _WIN32
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#endif

/* Globals defined in xyscope.mm — needed by audioInput */
extern int sample_rate;
extern int frame_rate;
extern int frames_per_buf;
extern int draw_frames;
extern int default_rb_size;

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
    char target[256];
} thread_data_t;

extern thread_data_t Thread_Data;

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
    if (FAILED(hr)) {
        fprintf(stderr, "Error: CoCreateInstance failed (0x%lx)\n", hr);
        return false;
    }

    IMMDevice *device = NULL;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        fprintf(stderr, "Error: GetDefaultAudioEndpoint failed (0x%lx)\n", hr);
        enumerator->Release();
        return false;
    }

    /* Print the device name */
    if (verbose) {
        IPropertyStore *store = NULL;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name))) {
                printf("Audio device: %ls\n", name.pwszVal);
            }
            PropVariantClear(&name);
            store->Release();
        }
    }

    IAudioClient *audioClient = NULL;
    hr = device->Activate(XYSCOPE_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&audioClient);
    device->Release();
    enumerator->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "Error: Activate failed (0x%lx)\n", hr);
        return false;
    }

    WAVEFORMATEX *mix_format = NULL;
    hr = audioClient->GetMixFormat(&mix_format);
    if (FAILED(hr)) {
        fprintf(stderr, "Error: GetMixFormat failed (0x%lx)\n", hr);
        audioClient->Release();
        return false;
    }

    t_data->wasapi_channels = mix_format->nChannels;
    if (verbose)
        printf("WASAPI: %lu Hz, %u channels, %u bits\n",
               mix_format->nSamplesPerSec, mix_format->nChannels, mix_format->wBitsPerSample);

    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        0, 0, mix_format, NULL);
    CoTaskMemFree(mix_format);

    if (FAILED(hr)) {
        fprintf(stderr, "Error: Initialize failed (0x%lx)\n", hr);
        audioClient->Release();
        return false;
    }

    IAudioCaptureClient *captureClient = NULL;
    hr = audioClient->GetService(XYSCOPE_IID_IAudioCaptureClient, (void**)&captureClient);
    if (FAILED(hr)) {
        fprintf(stderr, "Error: GetService failed (0x%lx)\n", hr);
        audioClient->Release();
        return false;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "Error: Start failed (0x%lx)\n", hr);
        captureClient->Release();
        audioClient->Release();
        return false;
    }

    t_data->audio_client = (void *)audioClient;
    t_data->capture_client = (void *)captureClient;

    if (t_data->ringbuffer == NULL) {
        t_data->ringbuffer = ringbuffer_create(t_data->frame_size * t_data->rb_size);
        bzero(t_data->ringbuffer->buf, t_data->ringbuffer->size);
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
    if (buf->datas[0].data == NULL) {
        pw_stream_queue_buffer(t_data->stream, b);
        return;
    }

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
        if (info.channels > 0 && info.channels != t_data->channels) {
            fprintf(stderr, "Pipewire negotiated %u channels (requested %u); using FL/FR from interleaved stream\n",
                    info.channels, t_data->channels);
            t_data->channels = info.channels;
        } else {
            fprintf(stderr, "Pipewire negotiated format: %u Hz, %u channels\n",
                    info.rate, info.channels);
        }
    }
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    (void)userdata;
    fprintf(stderr, "Pipewire stream state: %s -> %s%s%s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state),
            error ? " error=" : "",
            error ? error : "");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
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

    audioInput(const char *target)
    {
        char saved_target[256];
        if (target && target[0])
            snprintf(saved_target, sizeof(saved_target), "%s", target);
        else
            saved_target[0] = '\0';
        bzero(&Thread_Data, sizeof(Thread_Data));
        memcpy(Thread_Data.target, saved_target, sizeof(Thread_Data.target));
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
#ifdef __APPLE__
        if (t_data->input_buffer) {
            for (unsigned int i = 0; i < t_data->channels; i++)
                free(t_data->input_buffer[i]);
        }
#endif
        free(t_data->input_buffer);
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

        struct pw_properties *props = pw_properties_new(
                PW_KEY_MEDIA_TYPE,        "Audio",
                PW_KEY_MEDIA_CATEGORY,    "Capture",
                PW_KEY_MEDIA_ROLE,        "Music",
                PW_KEY_APP_NAME,          "XYScope",
                PW_KEY_APP_ID,            "xyscope",
                PW_KEY_NODE_NAME,         "xyscope",
                PW_KEY_NODE_DESCRIPTION,  "XY Scope visualizer",
                PW_KEY_MEDIA_NAME,        "XY Scope capture",
                NULL);
        if (t_data->target[0])
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, t_data->target);
        else
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

        t_data->stream = pw_stream_new_simple(
            pw_thread_loop_get_loop(t_data->loop),
            "xyscope",
            props,
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

#endif /* XYSCOPE_AUDIO_H */
