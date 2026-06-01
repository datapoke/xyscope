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
#include <SDL2/SDL_syswm.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <fftw3.h>
#else
#include <GL/gl.h>
#include <fftw3.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "xyscope-shared.h"
#include "xyscope-ringbuffer.h"
#include "xyscope-draw.h"
#include "xyscope-hdr.h"
#include "xyscope-hdr-present.h"
#include "xyscope-bloom.h"

#include "xyscope-compat.h"
#include "xyscope-audio.h"

#ifdef _WIN32
/* Forward declarations — defined after scene class */
extern HDC hdr_hdc;
extern HWND fs_cover_hwnd;
extern HWND gl_hidden_hwnd;
extern hdr_present_t g_hdr_present;
#elif !defined(__APPLE__)
extern bool wayland_hdr_active;
#endif

/* Audio sample rate and display frame rate — detected at runtime */
int sample_rate = 96000;
int frame_rate  = 120;

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
int frames_per_buf;
int draw_frames;
int default_rb_size;

static void compute_derived_rates() {
    frames_per_buf  = (sample_rate / frame_rate) * DRAW_EACH_FRAME;
    draw_frames     = frames_per_buf;
    default_rb_size = (int)(sample_rate * BUFFER_SECONDS + frames_per_buf);
}



thread_data_t Thread_Data;

#define LEFT_PORT  0
#define RIGHT_PORT 1

#define TIMED true
#define NOT_TIMED false

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))
#define sign(A) ((A) < 0.0 ? -1.0 : 1.0)


/* The scene object */

// Global SDL variables (declared here so scene class can access them)
extern TTF_Font *font;
extern SDL_Window *window;
extern SDL_GLContext gl_context;

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif

/* Spectrum color shader — compiled in main(), used by drawPlot.
 * Declared before scene so drawPlot can reference them. */
static GLuint spectrum_shader_prog = 0;
static GLint  spectrum_brightness_loc = -1;

/* GPU spline shader — compiled in main(), used by drawPlot. */
static GLuint spline_shader_prog = 0;
static GLint  spline_loc_positions = -1;
static GLint  spline_loc_colors = -1;
static GLint  spline_loc_num_samples = -1;
static GLint  spline_loc_spline_steps = -1;
static GLuint spline_pos_tex[2] = {0, 0};
static GLuint spline_col_tex[2] = {0, 0};
static GLuint spline_index_vbo = 0;
static unsigned int spline_index_alloc = 0;


#include "xyscope-scene.h"
static scene scn;
static bloom_state_t bloom = {0};

static const char *SPECTRUM_VS_SRC =
    "#version 120\n"
    "uniform float u_brightness;\n"
    "varying vec4 v_color;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "    v_color = vec4(gl_Color.rgb * u_brightness, gl_Color.a);\n"
    "}\n";

static const char *SPECTRUM_FS_SRC =
    "#version 120\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";

/* ---- GPU spline shader ---- */


static const char *SPLINE_VS_SRC =
    "#version 120\n"
    "uniform sampler1D u_positions;\n"
    "uniform sampler1D u_colors;\n"
    "uniform float u_num_samples;\n"
    "uniform float u_spline_steps;\n"
    "void main() {\n"
    "    float idx = gl_Vertex.x;\n"
    "    float seg = floor(idx / u_spline_steps);\n"
    "    float t = idx / u_spline_steps - seg;\n"
    "    seg += 1.0;\n"
    "    float inv_n = 1.0 / u_num_samples;\n"
    "    vec4 s0 = texture1DLod(u_positions, (seg - 1.0 + 0.5) * inv_n, 0.0);\n"
    "    vec4 s1 = texture1DLod(u_positions, (seg + 0.5) * inv_n, 0.0);\n"
    "    vec4 s2 = texture1DLod(u_positions, (seg + 1.0 + 0.5) * inv_n, 0.0);\n"
    "    vec4 s3 = texture1DLod(u_positions, (seg + 2.0 + 0.5) * inv_n, 0.0);\n"
    "    float t2 = t * t, t3 = t2 * t;\n"
    "    float x = 0.5 * (2.0*s1.r + (-s0.r+s2.r)*t + (2.0*s0.r-5.0*s1.r+4.0*s2.r-s3.r)*t2 + (-s0.r+3.0*s1.r-3.0*s2.r+s3.r)*t3);\n"
    "    float y = 0.5 * (2.0*s1.g + (-s0.g+s2.g)*t + (2.0*s0.g-5.0*s1.g+4.0*s2.g-s3.g)*t2 + (-s0.g+3.0*s1.g-3.0*s2.g+s3.g)*t3);\n"
    "    gl_Position = gl_ModelViewProjectionMatrix * vec4(x, y, 0.0, 1.0);\n"
    "    gl_FrontColor = texture1DLod(u_colors, (seg + 0.5) * inv_n, 0.0);\n"
    "}\n";

static const char *SPLINE_FS_SRC =
    "#version 120\n"
    "void main() {\n"
    "    gl_FragColor = gl_Color;\n"
    "}\n";


#include "xyscope-callbacks.h"

// Global SDL variables (definition)
SDL_Window *window = NULL;
SDL_GLContext gl_context = NULL;
#ifdef _WIN32
HDC hdr_hdc = NULL;              /* non-NULL when using WGL float framebuffer */
HGLRC hdr_hglrc = NULL;
HWND gl_hidden_hwnd = NULL;      /* offscreen GL render window behind DXGI present */
hdr_present_t g_hdr_present = {}; /* DXGI HDR present path; enabled when active */
HWND fs_cover_hwnd = NULL;
#elif !defined(__APPLE__)
bool wayland_hdr_active = false;
#endif
TTF_Font *font = NULL;

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SDL_SetMainReady();
    timeBeginPeriod(1);

    /* -mwindows silently discards stdout/stderr. Redirect both to a log file
     * in the config dir so diagnostic prints (bloom init, HDR setup, errors)
     * can be read after the fact. */
    {
        const char *appdata = getenv("APPDATA");
        if (!appdata) appdata = ".";
        char logdir[480];
        char logpath[512];
        snprintf(logdir, sizeof(logdir), "%s\\XYScope", appdata);
        CreateDirectoryA(logdir, NULL);
        snprintf(logpath, sizeof(logpath), "%s\\xyscope.log", logdir);
        freopen(logpath, "w", stderr);
        freopen(logpath, "a", stdout);
        fprintf(stderr, "=== XYScope log ===\n");
        fflush(stderr);
    }
#elif !defined(__APPLE__)
    /* GUI app launchers (COSMIC, and likely others) don't drain child
     * stdio at all — both stdout and stderr can block on full pipes.
     * dup2 of one onto the other doesn't help. When we're not attached
     * to a tty, redirect both streams to a log file in the config dir
     * so diagnostic writes always complete instantly. Matches the
     * Windows -mwindows approach. Line-buffer the log so partial output
     * survives crashes. */
    if (!isatty(STDERR_FILENO)) {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        char logdir[480];
        char logpath[512];
        snprintf(logdir, sizeof(logdir), "%s/.config/xyscope", home);
        mkdir(logdir, 0755);
        snprintf(logpath, sizeof(logpath), "%s/xyscope.log", logdir);
        freopen(logpath, "w", stderr);
        freopen(logpath, "a", stdout);
        setvbuf(stderr, NULL, _IOLBF, 0);
        setvbuf(stdout, NULL, _IOLBF, 0);
    }
#endif
    // Load preferences
    bool config_loaded = load_config(&scn.prefs, &scn.presets, &scn.app);
    if (!config_loaded)
        scn.prefs.is_full_screen = DEFAULT_FULL_SCREEN;

    // Parse CLI arguments
    int start_preset = -1;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--preset")) && i + 1 < argc) {
            start_preset = atoi(argv[++i]);
        }
#if !defined(__APPLE__) && !defined(_WIN32)
        else if ((!strcmp(argv[i], "-t") || !strcmp(argv[i], "--target")) && i + 1 < argc) {
            snprintf(scn.app.target, sizeof(scn.app.target), "%s", argv[++i]);
        }
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--reset-target")) {
            scn.app.target[0] = '\0';
        }
#endif
        else if (!strcmp(argv[i], "--splines") && i + 1 < argc) {
            scn.prefs.spline_steps = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-mode") && i + 1 < argc) {
            scn.prefs.color_mode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-range") && i + 1 < argc) {
            scn.prefs.color_range = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--color-rate") && i + 1 < argc) {
            scn.prefs.color_rate = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--display-mode") && i + 1 < argc) {
            scn.prefs.display_mode = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--line-width") && i + 1 < argc) {
            scn.prefs.line_width = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--particles") && i + 1 < argc) {
            scn.prefs.particles = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--hue") && i + 1 < argc) {
            scn.prefs.hue = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--brightness") && i + 1 < argc) {
            scn.prefs.brightness = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--velocity-dim") && i + 1 < argc) {
            scn.prefs.velocity_dim = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom") && i + 1 < argc) {
            scn.prefs.bloom_intensity = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom-gamma") && i + 1 < argc) {
            scn.prefs.bloom_gamma = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--bloom-radius") && i + 1 < argc) {
            scn.prefs.bloom_radius = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
            scn.prefs.delay = atof(argv[++i]);
        }
        else if (!strcmp(argv[i], "--fullscreen")) {
            scn.prefs.is_full_screen = true;
        }
        else if (!strcmp(argv[i], "--windowed")) {
            scn.prefs.is_full_screen = false;
        }
        else if (!strcmp(argv[i], "--dj")) {
            scn.dj_mode = true;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: xyscope [options]\n\n");
            printf("  -p, --preset N       Load preset N (0-9) on startup\n");
#if !defined(__APPLE__) && !defined(_WIN32)
            printf("  -t, --target ID      Pipewire target node name or serial\n");
            printf("  -r, --reset-target   Clear saved Pipewire target\n");
#endif
            printf("  --splines N          Spline interpolation steps (1-1024)\n");
            printf("  --display-mode N     0=standard, 1=radius, 2=spectrum\n");
            printf("  --color-mode N       0=standard, 1=delta\n");
            printf("  --color-range N      Color range multiplier\n");
            printf("  --color-rate N       Color rotation rate\n");
            printf("  --hue N              Starting hue (0-360)\n");
            printf("  --brightness N       Brightness multiplier\n");
            printf("  --velocity-dim N     Velocity dimming amount\n");
            printf("  --bloom N            Bloom intensity (0=off)\n");
            printf("  --bloom-gamma N      Bloom gamma curve\n");
            printf("  --bloom-radius N     Bloom blur radius\n");
            printf("  --line-width N       Line width (1-%d)\n", MAX_LINE_WIDTH);
            printf("  --particles N        Particles mode (0=lines, 1=points)\n");
            printf("  --delay N            Display delay in ms\n");
            printf("  --fullscreen         Start in fullscreen\n");
            printf("  --windowed           Start in windowed mode\n");
            printf("  --dj                 DJ mode (hide all text)\n");
            printf("  -h, --help           Show this help\n");
            return 0;
        }
    }

    // Validate loaded preferences
    scn.validate_prefs();

    // Apply startup preset (CLI overrides config)
    if (start_preset >= 0 && start_preset < NUM_PRESETS)
        scn.loadPreset(start_preset);

#if !defined(__APPLE__) && !defined(_WIN32)
    /* Force SDL onto the Wayland video driver when we're clearly on a
     * Wayland session. SDL's auto-detect falls through to the X11
     * driver depending on parent-process context — XWayland's X11
     * reply path deadlocks inside XGetWindowAttributes from the COSMIC
     * app launcher on Pop! OS, and SDL's X11 GLX fails with "Couldn't
     * find matching GLX visual" when asked for a float framebuffer on
     * drivers without GLX_ARB_fbconfig_float. Overwrite any existing
     * value so a stale SDL_VIDEODRIVER=x11 in the user's shell profile
     * can't defeat the fix. wayland_hdr_setup() also depends on SDL
     * being on the Wayland driver to extract wl_display from
     * SDL_SysWMinfo, so this is consistent with the HDR pipeline. */
    if (getenv("WAYLAND_DISPLAY")) {
        setenv("SDL_VIDEODRIVER", "wayland", 1);
    }
#endif

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    fprintf(stderr, "SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    
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
        if (!font) font = TTF_OpenFont("/usr/share/fonts/noto/NotoSansMono-Regular.ttf", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 28);
        if (!font) font = TTF_OpenFont("/usr/share/fonts/dejavu/DejaVuSansMono.ttf", 28);
#endif
        if (!font) {
            fprintf(stderr, "Warning: Could not load font: %s\n", TTF_GetError());
        }
    }

#ifdef _WIN32
    /* HDR path: render OpenGL on a HIDDEN float window, and present on a
     * separate CLEAN window through a DXGI scRGB swapchain. DXGI refuses
     * (E_ACCESSDENIED) to create a swapchain on a window that already owns
     * a GL pixel format, so the GL-render and DXGI-present roles must live
     * on different windows. Falls back to the plain SDL path if any step
     * fails. */
    {
        hdr_window_t glwin = {};
        if (create_hdr_window(&glwin, "XY Scope GL", 0, 0,
                              scn.prefs.normal_dim[0], scn.prefs.normal_dim[1],
                              /*visible=*/false)) {
            HWND vis = create_plain_window("XY Scope",
                              scn.prefs.position[0], scn.prefs.position[1],
                              scn.prefs.normal_dim[0], scn.prefs.normal_dim[1]);
            if (vis) window = SDL_CreateWindowFrom((void *)vis);
            if (window) {
                hdr_hdc        = glwin.hdc;
                hdr_hglrc      = glwin.hglrc;
                gl_hidden_hwnd = glwin.hwnd;
                /* Re-assert GL current on the hidden window in case SDL
                 * touched the context while wrapping the visible window. */
                wglMakeCurrent(glwin.hdc, glwin.hglrc);
            } else {
                fprintf(stderr, "HDR: visible/SDL window setup failed; using SDL path\n");
                if (vis) DestroyWindow(vis);
                wglMakeCurrent(NULL, NULL);
                wglDeleteContext(glwin.hglrc);
                DestroyWindow(glwin.hwnd);
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
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#ifndef _WIN32
    SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 1);
#endif

    // Create window
    window = SDL_CreateWindow("XY Scope",
                              scn.prefs.position[0],
                              scn.prefs.position[1],
                              scn.prefs.normal_dim[0],
                              scn.prefs.normal_dim[1],
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

#ifndef _WIN32
    if (!window) {
        /* SDL_GL_FLOATBUFFERS can fail on X11 GLX without
         * GLX_ARB_fbconfig_float (e.g. pure-X11 sessions or older
         * drivers). Retry as SDR — users on those setups lose HDR
         * headroom but keep the app. */
        fprintf(stderr, "Window creation failed (%s); retrying as SDR (no float framebuffer)\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_FLOATBUFFERS, 0);
        window = SDL_CreateWindow("XY Scope",
                                  scn.prefs.position[0],
                                  scn.prefs.position[1],
                                  scn.prefs.normal_dim[0],
                                  scn.prefs.normal_dim[1],
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    }
#endif

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
    if (SDL_GL_SetSwapInterval(-1) == -1)  /* try adaptive vsync first */
        SDL_GL_SetSwapInterval(1);

#ifndef _WIN32
    /* Disable color clamping for HDR.
     * SDL_GL_FLOATBUFFERS gives us a float framebuffer;
     * unclamping lets values > 1.0 reach HDR luminance.
     * (Windows handles this in the WGL HDR path below.) */
    {
        #ifndef GL_CLAMP_VERTEX_COLOR_ARB
        #define GL_CLAMP_VERTEX_COLOR_ARB   0x891A
        #define GL_CLAMP_FRAGMENT_COLOR_ARB 0x891B
        #endif
#ifdef __APPLE__
        extern void glClampColorARB(GLenum, GLenum);
#else
        typedef void (*PFNGLCLAMPCOLORARBPROC)(GLenum, GLenum);
        PFNGLCLAMPCOLORARBPROC glClampColorARB =
            (PFNGLCLAMPCOLORARBPROC)SDL_GL_GetProcAddress("glClampColorARB");
        if (!glClampColorARB) goto skip_clamp;
#endif
        glClampColorARB(GL_CLAMP_VERTEX_COLOR_ARB, GL_FALSE);
        glClampColorARB(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FALSE);
#ifndef __APPLE__
        skip_clamp:;
#endif
    }
#endif

#ifdef __APPLE__
    /* Tag the window's surface as extended-linear sRGB so the compositor
     * interprets our float framebuffer as linear light (1.0 = SDR
     * reference white, > 1.0 = EDR headroom), matching the scRGB-linear
     * semantics of the Windows (WGL float) and Linux (ext_linear) HDR
     * paths. Without this the default EDR surface is gamma-sRGB encoded,
     * so identical bloom/brightness settings land on a different transfer
     * curve than the other platforms — the soft midtone glow gets crushed
     * and can't be recovered with a (linear) brightness multiplier. */
    {
        SDL_SysWMinfo wminfo;
        SDL_VERSION(&wminfo.version);
        if (SDL_GetWindowWMInfo(window, &wminfo) &&
            wminfo.subsystem == SDL_SYSWM_COCOA) {
            NSWindow *nswin = wminfo.info.cocoa.window;
            CGColorSpaceRef cs =
                CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
            if (cs) {
                NSColorSpace *ns =
                    [[NSColorSpace alloc] initWithCGColorSpace:cs];
                if (ns) {
                    nswin.colorSpace = ns;
                    printf("HDR: macOS window tagged extended-linear sRGB\n");
                }
                CGColorSpaceRelease(cs);
            }
        }
    }
#endif

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

#if !defined(__APPLE__) && !defined(_WIN32) && defined(HAVE_WP_COLOR_MANAGEMENT)
    wayland_hdr_active = wayland_hdr_setup(window);
#endif

    glGenTextures(1, &scn.textures);

    // Set initial viewport
    int drawable_w, drawable_h;
    SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
    reshape(drawable_w, drawable_h);
    if (!bloom_init(&bloom, drawable_w, drawable_h)) {
        fprintf(stderr, "Bloom init failed or disabled; rendering without bloom.\n");
    }
#ifdef _WIN32
    /* On Windows the log file is the only way to see diagnostics, and
     * _mwindows buffers stderr until process exit. Force a flush and emit
     * an explicit success marker so the log is readable immediately. */
    else {
        fprintf(stderr, "Bloom init succeeded (%dx%d).\n", bloom.width, bloom.height);
    }
    fflush(stderr);
#endif

    /* Compile the spectrum color shader — applies u_brightness on
     * the GPU so draw_xy_vertices can skip the per-vertex CPU
     * multiply. Uses the same GL proc pointers bloom loaded. */
    if (bloom.enabled) {
        spectrum_shader_prog = bloom_build_program(SPECTRUM_VS_SRC, SPECTRUM_FS_SRC);
        if (spectrum_shader_prog) {
            spectrum_brightness_loc = p_glGetUniformLocation(spectrum_shader_prog, "u_brightness");
            fprintf(stderr, "Spectrum shader compiled.\n");
        }
    }

    /* Compile the GPU spline shader — moves Catmull-Rom interpolation
     * to the vertex shader, uploading only raw samples as textures. */
    if (bloom.enabled) {
        spline_shader_prog = bloom_build_program(SPLINE_VS_SRC, SPLINE_FS_SRC);
        if (spline_shader_prog) {
            spline_loc_positions    = p_glGetUniformLocation(spline_shader_prog, "u_positions");
            spline_loc_colors       = p_glGetUniformLocation(spline_shader_prog, "u_colors");
            spline_loc_num_samples  = p_glGetUniformLocation(spline_shader_prog, "u_num_samples");
            spline_loc_spline_steps = p_glGetUniformLocation(spline_shader_prog, "u_spline_steps");
            /* Create 1D textures for sample data */
            glGenTextures(2, spline_pos_tex);
            glGenTextures(2, spline_col_tex);
            fprintf(stderr, "GPU spline shader compiled.\n");
        }
    }

#ifdef _WIN32
    /* Present the HDR frame through a DXGI scRGB swapchain with explicit
     * HDR10 metadata so the compositor drives the panel to its real peak
     * instead of tone-mapping our values down. Only when the WGL float
     * path is active; falls back to SwapBuffers if interop init fails. */
    if (hdr_hdc) {
        SDL_SysWMinfo wi;
        SDL_VERSION(&wi.version);
        if (SDL_GetWindowWMInfo(window, &wi) && wi.subsystem == SDL_SYSWM_WINDOWS) {
            double peak = detect_hdr_brightness(window) * 80.0;  /* MaxLuminance */
            hdr_present_init(&g_hdr_present, wi.info.win.window, gl_hidden_hwnd,
                             drawable_w, drawable_h, peak);
        }
        fflush(stderr);
    }
#endif

    if (scn.prefs.is_full_screen) {
        scn.setFullScreen();
    }

    // Raise window and grab focus (after fullscreen setup so the
    // cover window and taskbar hiding don't steal it back)
    SDL_RaiseWindow(window);
#ifdef _WIN32
    {
        SDL_SysWMinfo wminfo;
        SDL_VERSION(&wminfo.version);
        if (SDL_GetWindowWMInfo(window, &wminfo)) {
            SetForegroundWindow(wminfo.info.win.window);
            SetFocus(wminfo.info.win.window);
        }
    }
#endif

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

    if (!config_loaded)
        scn.loadDefaults();

    scn.refreshStats(NOT_TIMED);
    scn.showScale(NOT_TIMED);
    scn.showSampleRate(NOT_TIMED);
    scn.showFrameRate(NOT_TIMED);

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
                    // Regular ASCII keys - handle modifiers
                    unsigned char ch = (unsigned char)key;

                    // Ctrl+number: save preset
                    if ((mod & KMOD_CTRL) && ch >= '0' && ch <= '9') {
                        scn.savePreset(ch - '0');
                    }
                    // Shift+number: quick zoom (was plain number)
                    else if ((mod & KMOD_SHIFT) && ch >= '0' && ch <= '9') {
                        if (ch == '0')
                            scn.setZoom(pow(2.0, 9.0));
                        else
                            scn.setZoom(pow(2.0, ch - '1'));
                    }
                    else {
                        // Convert lowercase to uppercase if shift is held
                        if ((mod & KMOD_SHIFT) && ch >= 'a' && ch <= 'z') {
                            ch = ch - 'a' + 'A';
                        }
                        // Handle shifted non-number keys for special characters
                        else if (mod & KMOD_SHIFT) {
                            switch(ch) {
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
        if (hdr_hdc) {
            int dw, dh;
            SDL_GL_GetDrawableSize(window, &dw, &dh);
            /* No fallback: present ONLY through the DXGI HDR swapchain so a
             * failure is obvious (black/frozen + log) instead of silently
             * looking like the old SwapBuffers path. */
            hdr_present_swap(&g_hdr_present, 0, dw, dh);
        } else
#endif
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
#ifdef _WIN32
    {
        HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
        if (taskbar) ShowWindow(taskbar, SW_SHOW);
        if (fs_cover_hwnd) ShowWindow(fs_cover_hwnd, SW_HIDE);
    }
    /* Release D3D/interop while the GL context is still current. */
    hdr_present_shutdown(&g_hdr_present);
    if (hdr_hglrc) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hdr_hglrc);
        if (gl_hidden_hwnd) DestroyWindow(gl_hidden_hwnd);
    } else
#endif
#if !defined(__APPLE__) && !defined(_WIN32) && defined(HAVE_WP_COLOR_MANAGEMENT)
    if (wayland_hdr_active) {
        if (wl_hdr.feedback)
            wp_color_management_surface_feedback_v1_destroy(wl_hdr.feedback);
        if (wl_hdr.cm_surface)
            wp_color_management_surface_v1_destroy(wl_hdr.cm_surface);
        if (wl_hdr.image_desc)
            wp_image_description_v1_destroy(wl_hdr.image_desc);
        if (wl_hdr.manager)
            wp_color_manager_v1_destroy(wl_hdr.manager);
    }
#endif
    bloom_cleanup(&bloom);
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return 0;
}
