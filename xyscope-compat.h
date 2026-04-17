/*
 *  xyscope-compat.h
 *  Windows compatibility shims: pthreads, usleep, POSIX I/O wrappers.
 *
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef XYSCOPE_COMPAT_H
#define XYSCOPE_COMPAT_H

#ifdef _WIN32

#include "xyscope-shared.h"  /* for gettimeofday, timespec */
#include <process.h>
#include <io.h>

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

#endif /* XYSCOPE_COMPAT_H */
