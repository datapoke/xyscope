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
                    /* Use peak luminance in scRGB units (80 nits = 1.0).
                     * MaxLuminance is peak for small highlights; scope
                     * traces are thin lines so this is appropriate. */
                    double m = (double)desc1.MaxLuminance / 80.0;
                    if (m > result)
                        result = m;
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
    /* Try DXGI first — gives peak display luminance for HDR */
    double m = detect_hdr_brightness_dxgi();
    if (m > 1.0)
        return m;

    /* Fall back to DisplayConfig SDR white level */
    return detect_hdr_brightness_displayconfig();
}

/* WGL extension constants */
#define WGL_DRAW_TO_WINDOW_ARB    0x2001
#define WGL_SUPPORT_OPENGL_ARB    0x2010
#define WGL_DOUBLE_BUFFER_ARB     0x2011
#define WGL_PIXEL_TYPE_ARB        0x2013
#define WGL_COLOR_BITS_ARB        0x2014
#define WGL_ALPHA_BITS_ARB        0x201B
#define WGL_TYPE_RGBA_FLOAT_ARB   0x21A0

typedef BOOL (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);

/* State returned from create_hdr_window */
typedef struct {
    HWND hwnd;
    HDC  hdc;
    HGLRC hglrc;
} hdr_window_t;

/*
 * Create a Win32 window with a floating-point pixel format for HDR/scRGB.
 * Uses the WGL bootstrap pattern: create a dummy context to load
 * wglChoosePixelFormatARB, then create the real window with the
 * float pixel format.  Returns true on success.
 */
static inline bool create_hdr_window(hdr_window_t *out,
                                     const char *title,
                                     int x, int y, int w, int h)
{
    /* Register window class */
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance      = GetModuleHandle(NULL);
    wc.lpszClassName  = "XYScopeHDR";
    wc.style          = CS_OWNDC;
    RegisterClassA(&wc);

    /* Dummy window to bootstrap WGL extensions */
    HWND dummy_hwnd = CreateWindowExA(0, wc.lpszClassName, "dummy",
        WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!dummy_hwnd) return false;

    HDC dummy_dc = GetDC(dummy_hwnd);
    PIXELFORMATDESCRIPTOR dummy_pfd = {};
    dummy_pfd.nSize      = sizeof(dummy_pfd);
    dummy_pfd.nVersion   = 1;
    dummy_pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    dummy_pfd.iPixelType = PFD_TYPE_RGBA;
    dummy_pfd.cColorBits = 32;
    int dummy_pf = ChoosePixelFormat(dummy_dc, &dummy_pfd);
    SetPixelFormat(dummy_dc, dummy_pf, &dummy_pfd);
    HGLRC dummy_rc = wglCreateContext(dummy_dc);
    wglMakeCurrent(dummy_dc, dummy_rc);

    /* Load WGL extension */
    PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB =
        (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(dummy_rc);
    ReleaseDC(dummy_hwnd, dummy_dc);
    DestroyWindow(dummy_hwnd);

    if (!wglChoosePixelFormatARB) return false;

    /* Create the real window */
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    RECT rect = { 0, 0, w, h };
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, title, style,
        x, y, rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return false;

    HDC hdc = GetDC(hwnd);

    /* Request float pixel format */
    int attribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, GL_TRUE,
        WGL_SUPPORT_OPENGL_ARB, GL_TRUE,
        WGL_DOUBLE_BUFFER_ARB,  GL_TRUE,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_FLOAT_ARB,
        WGL_COLOR_BITS_ARB,     64,
        WGL_ALPHA_BITS_ARB,     16,
        0
    };
    int pixel_format = 0;
    UINT num_formats = 0;
    if (!wglChoosePixelFormatARB(hdc, attribs, NULL, 1, &pixel_format, &num_formats)
        || num_formats == 0) {
        /* Float format not available, fall back to standard */
        DestroyWindow(hwnd);
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    DescribePixelFormat(hdc, pixel_format, sizeof(pfd), &pfd);
    SetPixelFormat(hdc, pixel_format, &pfd);

    HGLRC hglrc = wglCreateContext(hdc);
    if (!hglrc) {
        DestroyWindow(hwnd);
        return false;
    }
    wglMakeCurrent(hdc, hglrc);

    out->hwnd  = hwnd;
    out->hdc   = hdc;
    out->hglrc = hglrc;
    printf("HDR: created floating-point framebuffer (scRGB)\n");
    return true;
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
