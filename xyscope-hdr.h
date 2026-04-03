/*
 *  xyscope-hdr.h
 *  Platform-specific HDR detection for brightness auto-scaling.
 *
 *  Copyright (c) 2006-2007 by Chris Reaume <chris@flatlan.net>
 *    All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef XYSCOPE_HDR_H
#define XYSCOPE_HDR_H

#ifdef _WIN32

#include <dxgi.h>
#include <dxgi1_6.h>
#include <wingdi.h>

/*
 * DisplayConfig API - not available in mingw-w64 headers.
 * Define types and load functions at runtime from user32.dll.
 */
#ifndef DISPLAYCONFIG_PATH_ACTIVE
#define DISPLAYCONFIG_PATH_ACTIVE 0x00000001
#endif
#ifndef QDC_ONLY_ACTIVE_PATHS
#define QDC_ONLY_ACTIVE_PATHS 0x00000002
#endif

typedef struct {
    UINT32 type;
    UINT32 size;
    LUID   adapterId;
    UINT32 id;
} XY_DISPLAYCONFIG_DEVICE_INFO_HEADER;

typedef struct {
    LUID   adapterId;
    UINT32 id;
    UINT32 modeInfoIdx;
    UINT32 outputTechnology;
    UINT32 rotation;
    UINT32 scaling;
    struct { UINT32 numerator; UINT32 denominator; } refreshRate;
    UINT32 scanLineOrdering;
    BOOL   targetAvailable;
    UINT32 statusFlags;
} XY_DISPLAYCONFIG_TARGET_INFO;

typedef struct {
    LUID   adapterId;
    UINT32 id;
    UINT32 modeInfoIdx;
    UINT32 statusFlags;
} XY_DISPLAYCONFIG_SOURCE_INFO;

typedef struct {
    XY_DISPLAYCONFIG_SOURCE_INFO sourceInfo;
    XY_DISPLAYCONFIG_TARGET_INFO targetInfo;
    UINT32 flags;
} XY_DISPLAYCONFIG_PATH_INFO;

/* Not used directly, but QueryDisplayConfig requires a modes buffer */
typedef struct {
    UINT32 infoType;
    UINT32 id;
    LUID   adapterId;
    BYTE   data[48]; /* union of target/source mode info */
} XY_DISPLAYCONFIG_MODE_INFO;

#define XY_DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL 0x0B

typedef struct {
    XY_DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    ULONG SDRWhiteLevel;  /* thousandths: 1000 = 80 nits (1.0x) */
} XY_DISPLAYCONFIG_SDR_WHITE_LEVEL;

typedef LONG (WINAPI *PFN_GetDisplayConfigBufferSizes)(UINT32, UINT32 *, UINT32 *);
typedef LONG (WINAPI *PFN_QueryDisplayConfig)(UINT32, UINT32 *,
    XY_DISPLAYCONFIG_PATH_INFO *, UINT32 *, XY_DISPLAYCONFIG_MODE_INFO *, void *);
typedef LONG (WINAPI *PFN_DisplayConfigGetDeviceInfo)(XY_DISPLAYCONFIG_DEVICE_INFO_HEADER *);

static inline double detect_hdr_brightness_displayconfig(void)
{
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (!user32)
        return 1.0;

    PFN_GetDisplayConfigBufferSizes pGetBufferSizes =
        (PFN_GetDisplayConfigBufferSizes)GetProcAddress(user32, "GetDisplayConfigBufferSizes");
    PFN_QueryDisplayConfig pQueryConfig =
        (PFN_QueryDisplayConfig)GetProcAddress(user32, "QueryDisplayConfig");
    PFN_DisplayConfigGetDeviceInfo pGetDeviceInfo =
        (PFN_DisplayConfigGetDeviceInfo)GetProcAddress(user32, "DisplayConfigGetDeviceInfo");

    if (!pGetBufferSizes || !pQueryConfig || !pGetDeviceInfo) {
        FreeLibrary(user32);
        return 1.0;
    }

    UINT32 num_paths = 0, num_modes = 0;
    LONG rc;
    double max_multiplier = 1.0;

    rc = pGetBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes);
    if (rc != ERROR_SUCCESS || num_paths == 0) {
        FreeLibrary(user32);
        return 1.0;
    }

    XY_DISPLAYCONFIG_PATH_INFO *paths = (XY_DISPLAYCONFIG_PATH_INFO *)
        calloc(num_paths, sizeof(XY_DISPLAYCONFIG_PATH_INFO));
    XY_DISPLAYCONFIG_MODE_INFO *modes = (XY_DISPLAYCONFIG_MODE_INFO *)
        calloc(num_modes, sizeof(XY_DISPLAYCONFIG_MODE_INFO));
    if (!paths || !modes) {
        free(paths);
        free(modes);
        FreeLibrary(user32);
        return 1.0;
    }

    rc = pQueryConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, NULL);
    if (rc != ERROR_SUCCESS) {
        free(paths);
        free(modes);
        FreeLibrary(user32);
        return 1.0;
    }

    for (UINT32 i = 0; i < num_paths; i++) {
        XY_DISPLAYCONFIG_SDR_WHITE_LEVEL sdr = {};
        sdr.header.type      = XY_DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sdr.header.size      = sizeof(sdr);
        sdr.header.adapterId = paths[i].targetInfo.adapterId;
        sdr.header.id        = paths[i].targetInfo.id;

        rc = pGetDeviceInfo(&sdr.header);
        if (rc == ERROR_SUCCESS && sdr.SDRWhiteLevel > 0) {
            double m = (double)sdr.SDRWhiteLevel / 1000.0;
            if (m > max_multiplier)
                max_multiplier = m;
        }
    }

    free(paths);
    free(modes);
    FreeLibrary(user32);
    return max_multiplier;
}

static inline double detect_hdr_brightness_dxgi(void)
{
    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    if (FAILED(hr) || !factory)
        return 1.0;

    double result = 1.0;
    IDXGIAdapter1 *adapter = NULL;

    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) == S_OK; ai++) {
        IDXGIOutput *output = NULL;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) == S_OK; oi++) {
            IDXGIOutput6 *output6 = NULL;
            hr = output->QueryInterface(__uuidof(IDXGIOutput6), (void **)&output6);
            if (SUCCEEDED(hr) && output6) {
                DXGI_OUTPUT_DESC1 desc1;
                hr = output6->GetDesc1(&desc1);
                if (SUCCEEDED(hr) &&
                    desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    /* HDR is active but we couldn't get SDR white level */
                    result = 2.5;
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

static inline double detect_hdr_brightness(void)
{
    /* Try DisplayConfig first (gives exact SDR white level) */
    double m = detect_hdr_brightness_displayconfig();
    if (m > 1.0)
        return m;

    /* Fall back to DXGI (can detect HDR but not exact white level) */
    return detect_hdr_brightness_dxgi();
}

#elif defined(__APPLE__)

#ifdef __OBJC__
#import <AppKit/NSScreen.h>
#endif

static inline double detect_hdr_brightness(void)
{
#ifdef __OBJC__
    @autoreleasepool {
        NSScreen *screen = [NSScreen mainScreen];
        CGFloat edr = screen.maximumExtendedDynamicRangeColorComponentValue;
        if (edr > 1.0)
            return (double)edr;
    }
#endif
    return 1.0;
}

#else
/* Linux: no standard HDR detection */
static inline double detect_hdr_brightness(void)
{
    return 1.0;
}

#endif /* platform */

#endif /* XYSCOPE_HDR_H */
