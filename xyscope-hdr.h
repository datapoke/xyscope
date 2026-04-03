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

/* DISPLAYCONFIG_SDR_WHITE_LEVEL is not in mingw headers yet */
#ifndef DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL
#define DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL ((int)0x0B)
typedef struct {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    ULONG SDRWhiteLevel;  /* thousandths: 1000 = 80 nits (1.0x) */
} DISPLAYCONFIG_SDR_WHITE_LEVEL;
#endif

static inline double detect_hdr_brightness_displayconfig(void)
{
    UINT32 num_paths = 0, num_modes = 0;
    LONG rc;
    double max_multiplier = 1.0;

    rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes);
    if (rc != ERROR_SUCCESS || num_paths == 0)
        return 1.0;

    DISPLAYCONFIG_PATH_INFO *paths = (DISPLAYCONFIG_PATH_INFO *)
        calloc(num_paths, sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO *modes = (DISPLAYCONFIG_MODE_INFO *)
        calloc(num_modes, sizeof(DISPLAYCONFIG_MODE_INFO));
    if (!paths || !modes) {
        free(paths);
        free(modes);
        return 1.0;
    }

    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, NULL);
    if (rc != ERROR_SUCCESS) {
        free(paths);
        free(modes);
        return 1.0;
    }

    for (UINT32 i = 0; i < num_paths; i++) {
        DISPLAYCONFIG_SDR_WHITE_LEVEL sdr = {};
        sdr.header.type      = (DISPLAYCONFIG_DEVICE_INFO_TYPE)
                                DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sdr.header.size      = sizeof(sdr);
        sdr.header.adapterId = paths[i].targetInfo.adapterId;
        sdr.header.id        = paths[i].targetInfo.id;

        rc = DisplayConfigGetDeviceInfo(&sdr.header);
        if (rc == ERROR_SUCCESS && sdr.SDRWhiteLevel > 0) {
            double m = (double)sdr.SDRWhiteLevel / 1000.0;
            if (m > max_multiplier)
                max_multiplier = m;
        }
    }

    free(paths);
    free(modes);
    return max_multiplier;
}

static inline double detect_hdr_brightness_dxgi(void)
{
    IDXGIFactory1 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || !factory)
        return 1.0;

    double result = 1.0;
    IDXGIAdapter1 *adapter = NULL;

    for (UINT ai = 0; factory->lpVtbl->EnumAdapters1(factory, ai, &adapter) == S_OK; ai++) {
        IDXGIOutput *output = NULL;
        for (UINT oi = 0; adapter->lpVtbl->EnumOutputs(adapter, oi, &output) == S_OK; oi++) {
            IDXGIOutput6 *output6 = NULL;
            hr = output->lpVtbl->QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6);
            if (SUCCEEDED(hr) && output6) {
                DXGI_OUTPUT_DESC1 desc1;
                hr = output6->lpVtbl->GetDesc1(output6, &desc1);
                if (SUCCEEDED(hr) &&
                    desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    /* HDR is active but we couldn't get SDR white level */
                    result = 2.5;
                }
                output6->lpVtbl->Release(output6);
            }
            output->lpVtbl->Release(output);
        }
        adapter->lpVtbl->Release(adapter);
    }
    factory->lpVtbl->Release(factory);
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
