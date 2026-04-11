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
        CGFloat edr = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
        if (edr > 1.0)
            return (double)edr;
    }
#endif
    return 1.0;
}

#else
/* Linux: Wayland HDR via wp_color_management_v1 protocol */

#ifdef HAVE_WP_COLOR_MANAGEMENT

#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL2/SDL_syswm.h>
#include "color-management-v1-client-protocol.h"

typedef struct {
    struct wl_display *display;
    struct wp_color_manager_v1 *manager;
    struct wp_color_management_surface_v1 *cm_surface;
    struct wp_color_management_surface_feedback_v1 *feedback;
    struct wp_image_description_v1 *image_desc;
    bool has_parametric;
    bool has_ext_linear;
    bool has_srgb;
    bool ready;
    double headroom;
} wayland_hdr_state_t;

static wayland_hdr_state_t wl_hdr = {};

/* --- wp_color_manager_v1 listener --- */

static void cm_supported_intent(void *data, struct wp_color_manager_v1 *mgr,
                                uint32_t intent)
{ (void)data; (void)mgr; (void)intent; }

static void cm_supported_feature(void *data, struct wp_color_manager_v1 *mgr,
                                 uint32_t feature)
{
    (void)mgr;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)
        st->has_parametric = true;
}

static void cm_supported_tf_named(void *data, struct wp_color_manager_v1 *mgr,
                                  uint32_t tf)
{
    (void)mgr;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR)
        st->has_ext_linear = true;
}

static void cm_supported_primaries_named(void *data, struct wp_color_manager_v1 *mgr,
                                         uint32_t primaries)
{
    (void)mgr;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB)
        st->has_srgb = true;
}

static void cm_done(void *data, struct wp_color_manager_v1 *mgr)
{ (void)data; (void)mgr; }

static const struct wp_color_manager_v1_listener cm_listener = {
    .supported_intent          = cm_supported_intent,
    .supported_feature         = cm_supported_feature,
    .supported_tf_named        = cm_supported_tf_named,
    .supported_primaries_named = cm_supported_primaries_named,
    .done                      = cm_done,
};

/* --- wp_image_description_v1 listener --- */

static void img_desc_failed(void *data, struct wp_image_description_v1 *desc,
                            uint32_t cause, const char *msg)
{
    (void)desc;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    st->ready = false;
    fprintf(stderr, "HDR: image description failed: %s\n", msg);
}

static void img_desc_ready(void *data, struct wp_image_description_v1 *desc,
                           uint32_t identity)
{
    (void)desc; (void)identity;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    st->ready = true;
}

static const struct wp_image_description_v1_listener img_desc_listener = {
    .failed = img_desc_failed,
    .ready  = img_desc_ready,
};

/* --- wp_image_description_info_v1 listener (headroom query) --- */

typedef struct {
    wayland_hdr_state_t *hdr;
    double max_luminance;
    double reference_luminance;
} feedback_query_t;

static void info_done(void *data, struct wp_image_description_info_v1 *info)
{
    (void)info;
    feedback_query_t *q = (feedback_query_t *)data;
    if (q->reference_luminance > 0 && q->max_luminance > 0)
        q->hdr->headroom = q->max_luminance / q->reference_luminance;
}

static void info_icc_file(void *data, struct wp_image_description_info_v1 *info,
                          int32_t icc, uint32_t icc_size)
{ (void)data; (void)info; (void)icc; (void)icc_size; }

static void info_primaries(void *data, struct wp_image_description_info_v1 *info,
                           int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
                           int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{ (void)data; (void)info; (void)r_x; (void)r_y; (void)g_x; (void)g_y;
  (void)b_x; (void)b_y; (void)w_x; (void)w_y; }

static void info_primaries_named(void *data, struct wp_image_description_info_v1 *info,
                                 uint32_t primaries)
{ (void)data; (void)info; (void)primaries; }

static void info_tf_power(void *data, struct wp_image_description_info_v1 *info,
                          uint32_t eexp)
{ (void)data; (void)info; (void)eexp; }

static void info_tf_named(void *data, struct wp_image_description_info_v1 *info,
                          uint32_t tf)
{ (void)data; (void)info; (void)tf; }

static void info_luminances(void *data, struct wp_image_description_info_v1 *info,
                            uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum)
{
    (void)info; (void)min_lum;
    feedback_query_t *q = (feedback_query_t *)data;
    q->reference_luminance = (double)reference_lum;
    q->max_luminance       = (double)max_lum;
}

static void info_target_primaries(void *data, struct wp_image_description_info_v1 *info,
                                  int32_t r_x, int32_t r_y, int32_t g_x, int32_t g_y,
                                  int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{ (void)data; (void)info; (void)r_x; (void)r_y; (void)g_x; (void)g_y;
  (void)b_x; (void)b_y; (void)w_x; (void)w_y; }

static void info_target_luminance(void *data, struct wp_image_description_info_v1 *info,
                                  uint32_t min_lum, uint32_t max_lum)
{
    (void)info; (void)min_lum;
    feedback_query_t *q = (feedback_query_t *)data;
    /* Override max luminance with display's actual peak if available */
    if (max_lum > 0)
        q->max_luminance = (double)max_lum;
}

static void info_target_max_cll(void *data, struct wp_image_description_info_v1 *info,
                                uint32_t max_cll)
{ (void)data; (void)info; (void)max_cll; }

static void info_target_max_fall(void *data, struct wp_image_description_info_v1 *info,
                                 uint32_t max_fall)
{ (void)data; (void)info; (void)max_fall; }

static const struct wp_image_description_info_v1_listener info_listener = {
    .done              = info_done,
    .icc_file          = info_icc_file,
    .primaries         = info_primaries,
    .primaries_named   = info_primaries_named,
    .tf_power          = info_tf_power,
    .tf_named          = info_tf_named,
    .luminances        = info_luminances,
    .target_primaries  = info_target_primaries,
    .target_luminance  = info_target_luminance,
    .target_max_cll    = info_target_max_cll,
    .target_max_fall   = info_target_max_fall,
};

/* --- wp_color_management_surface_feedback_v1 listener --- */

static void feedback_preferred_changed(void *data,
    struct wp_color_management_surface_feedback_v1 *fb, uint32_t identity)
{
    (void)identity;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;

    struct wp_image_description_v1 *pref =
        wp_color_management_surface_feedback_v1_get_preferred(fb);

    st->ready = false;
    wp_image_description_v1_add_listener(pref, &img_desc_listener, st);
    wl_display_roundtrip(st->display);

    if (st->ready) {
        feedback_query_t q = { st, 0, 0 };
        struct wp_image_description_info_v1 *info =
            wp_image_description_v1_get_information(pref);
        wp_image_description_info_v1_add_listener(info, &info_listener, &q);
        wl_display_roundtrip(st->display);
        /* info object is auto-destroyed after done event */
    }

    wp_image_description_v1_destroy(pref);
}

static const struct wp_color_management_surface_feedback_v1_listener feedback_listener = {
    .preferred_changed = feedback_preferred_changed,
};

/* --- wl_registry listener --- */

static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)version;
    wayland_hdr_state_t *st = (wayland_hdr_state_t *)data;
    if (!strcmp(interface, wp_color_manager_v1_interface.name)) {
        st->manager = (struct wp_color_manager_v1 *)
            wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{ (void)data; (void)registry; (void)name; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* --- Main setup function --- */

static bool wayland_hdr_setup(SDL_Window *window)
{
    /* Step 1: Get Wayland display and surface from SDL */
    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    if (!SDL_GetWindowWMInfo(window, &wminfo))
        return false;
    if (wminfo.subsystem != SDL_SYSWM_WAYLAND)
        return false;

    wl_hdr.display = wminfo.info.wl.display;
    struct wl_surface *surface = wminfo.info.wl.surface;

    /* Step 2: Verify float framebuffer */
    int red_size = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &red_size);
    if (red_size < 16) {
        fprintf(stderr, "HDR: float framebuffer not available (red_size=%d)\n", red_size);
        return false;
    }

    /* Step 3: Bind color manager from registry */
    struct wl_registry *registry = wl_display_get_registry(wl_hdr.display);
    wl_registry_add_listener(registry, &registry_listener, &wl_hdr);
    wl_display_roundtrip(wl_hdr.display);
    wl_registry_destroy(registry);

    if (!wl_hdr.manager) {
        fprintf(stderr, "HDR: compositor does not support wp_color_management_v1\n");
        return false;
    }

    /* Step 4: Enumerate capabilities */
    wp_color_manager_v1_add_listener(wl_hdr.manager, &cm_listener, &wl_hdr);
    wl_display_roundtrip(wl_hdr.display);

    if (!wl_hdr.has_parametric || !wl_hdr.has_ext_linear || !wl_hdr.has_srgb) {
        fprintf(stderr, "HDR: compositor lacks required capabilities"
                " (parametric=%d ext_linear=%d srgb=%d)\n",
                wl_hdr.has_parametric, wl_hdr.has_ext_linear, wl_hdr.has_srgb);
        wp_color_manager_v1_destroy(wl_hdr.manager);
        wl_hdr.manager = NULL;
        return false;
    }

    /* Step 5: Create parametric image description (ext_linear TF, sRGB primaries) */
    struct wp_image_description_creator_params_v1 *creator =
        wp_color_manager_v1_create_parametric_creator(wl_hdr.manager);
    wp_image_description_creator_params_v1_set_tf_named(
        creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
    wp_image_description_creator_params_v1_set_primaries_named(
        creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    wl_hdr.image_desc =
        wp_image_description_creator_params_v1_create(creator);
    /* creator is destroyed by create */

    wl_hdr.ready = false;
    wp_image_description_v1_add_listener(wl_hdr.image_desc, &img_desc_listener, &wl_hdr);
    wl_display_roundtrip(wl_hdr.display);

    if (!wl_hdr.ready) {
        fprintf(stderr, "HDR: ext_linear/sRGB image description not accepted\n");
        wp_image_description_v1_destroy(wl_hdr.image_desc);
        wp_color_manager_v1_destroy(wl_hdr.manager);
        wl_hdr.image_desc = NULL;
        wl_hdr.manager = NULL;
        return false;
    }

    /* Step 6: Attach image description to surface */
    wl_hdr.cm_surface = wp_color_manager_v1_get_surface(wl_hdr.manager, surface);
    wp_color_management_surface_v1_set_image_description(
        wl_hdr.cm_surface, wl_hdr.image_desc,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

    /* Step 7: Set up feedback listener for headroom updates */
    wl_hdr.feedback =
        wp_color_manager_v1_get_surface_feedback(wl_hdr.manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(
        wl_hdr.feedback, &feedback_listener, &wl_hdr);

    /* Initial headroom query */
    {
        struct wp_image_description_v1 *pref =
            wp_color_management_surface_feedback_v1_get_preferred(wl_hdr.feedback);
        wl_hdr.ready = false;
        wp_image_description_v1_add_listener(pref, &img_desc_listener, &wl_hdr);
        wl_display_roundtrip(wl_hdr.display);
        if (wl_hdr.ready) {
            feedback_query_t q = { &wl_hdr, 0, 0 };
            struct wp_image_description_info_v1 *info =
                wp_image_description_v1_get_information(pref);
            wp_image_description_info_v1_add_listener(info, &info_listener, &q);
            wl_display_roundtrip(wl_hdr.display);
        }
        wp_image_description_v1_destroy(pref);
    }

    printf("HDR: Wayland color management active (headroom=%.2f)\n",
           wl_hdr.headroom > 0 ? wl_hdr.headroom : 1.0);
    return true;
}

static inline double detect_hdr_brightness(void)
{
    return wl_hdr.headroom > 0 ? wl_hdr.headroom : 1.0;
}

#else
/* Linux without wp_color_management_v1: no HDR */
static inline double detect_hdr_brightness(void)
{
    return 1.0;
}
#endif /* HAVE_WP_COLOR_MANAGEMENT */

#endif /* platform */

#endif /* XYSCOPE_HDR_H */
