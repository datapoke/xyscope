/*
 *  xyscope-scene.h
 *  The `scene` class — visualization state, audio read, drawPlot/FFT,
 *  text rendering, and all UI/control accessors. Extracted from
 *  xyscope.mm; a header-only module included into that single
 *  translation unit after the globals it depends on are declared.
 */
#ifndef XYSCOPE_SCENE_H
#define XYSCOPE_SCENE_H

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
    presets_t presets;
    app_config_t app;

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
    bool dj_mode;

    #define NUM_TEXT_TIMERS 20
    #define NUM_AUTO_TEXT_TIMERS 16
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
        ParticlesTimer   = 3,
        ColorModeTimer   = 4, 
        DisplayModeTimer = 5,
        ColorRangeTimer  = 6,
        ColorRateTimer   = 7,
        DelayTimer       = 8,
        BrightnessTimer  = 9,
        VelocityDimTimer = 10,
        BloomTimer       = 11,
        BloomGammaTimer  = 12,
        BloomRadiusTimer = 13,
        SampleRateTimer  = 14,
        FrameRateTimer   = 15,
        /* End of text timers automatically included in stats display */
        PresetTimer      = 16,
        PausedTimer      = 17,
        ScaleTimer       = 18,
        CounterTimer     = 19
    } text_timer_handles;
    text_timer_t text_timer[NUM_TEXT_TIMERS];
    timeval show_intro_time;
    timeval last_frame_time;
    timeval reset_frame_time;
    timeval mouse_dirty_time;
    timeval this_frame_time;

    #define NUM_COLOR_MODES 2
    #define NUM_DISPLAY_MODES 3
    static const unsigned int DefaultColorMode    = DEFAULT_COLOR_MODE;
    static const unsigned int DefaultDisplayMode  = DEFAULT_DISPLAY_MODE;
    const char *color_mode_names[NUM_COLOR_MODES] = {"Standard", "Delta"};
    const char *display_mode_names[NUM_DISPLAY_MODES] = {
        "Standard", "Radius", "Spectrum"
    };

    scene()
    {
        frame_size         = sizeof(frame_t);
        framebuf           = NULL;
        ai                 = NULL;
        offset             = 0;
        bump               = 0;
        bytes_per_buf      = 0;
        latency            = 0.0;
        fps                = 0.0;
        frame_count        = 0;
        vertex_count       = 0;
        window_is_dirty    = true;
        mouse_is_dirty     = true;
        max_sample_value   = 1.0;
        top_offset         = -60.0;
        vertical_increment = -60.0;
        color_delta        = 0.0;
        color_threshold    = 0.0;
        show_intro         = true;
        show_help          = false;
        show_mouse         = true;
        dj_mode            = false;
        memset(&prefs,   0, sizeof(prefs));
        memset(&presets, 0, sizeof(presets));
        memset(&app,     0, sizeof(app));

        bzero(&text_timer, sizeof(text_timer_t) * NUM_TEXT_TIMERS);
        timeval now;
        gettimeofday(&now, NULL);
        show_intro_time = last_frame_time = reset_frame_time = mouse_dirty_time = now;

        for (int i = 0; i < 4; i += 2) {
			prefs.side[i]   = 1.0;
            prefs.side[i+1] = -1.0;
        }
        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
    }

    void init()
    {
        bytes_per_buf = draw_frames * frame_size;
        framebuf      = (frame_t *) malloc(bytes_per_buf);
        offset        = -frames_per_buf;
        bump          = -draw_frames;
#ifdef __APPLE__
        int log2n     = 0;
        int n         = draw_frames;
        while (n > 1) { n >>= 1; log2n++; }
        fft_setup     = vDSP_create_fftsetup(log2n, FFT_RADIX2);
        fft_out.realp = (float *) malloc(draw_frames/2 * sizeof(float));
        fft_out.imagp = (float *) malloc(draw_frames/2 * sizeof(float));
#else
        fft_out       = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * draw_frames);
#endif
        ai = new audioInput(app.target);
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
        save_config(&prefs, &presets, &app);
        delete ai;
#ifdef __APPLE__
        vDSP_destroy_fftsetup(fft_setup);
        free(fft_out.realp);
        free(fft_out.imagp);
#else
        fftw_free(fft_out);
#endif
        free(framebuf);
    }

    void drawPlot();
    void beginText();
    double getTextWidth(char *string);
    void drawString(double x, double y, char *string);
    void endText();
    void drawHelp();
    void drawTimedText();
    void drawStats();
    void drawText(void);
    void showTimedText(int timer_idx, bool auto_pos, bool timed, const char *fmt, ...);
    void showAutoScale(bool t) { showTimedText(AutoScaleTimer, true, t, "Auto-scale: %s", prefs.auto_scale ? "on" : "off"); }
    void showSplines(bool t) { showTimedText(SplineTimer, true, t, "Splines: %d", prefs.spline_steps); }
    void showLineWidth(bool t) { showTimedText(LineWidthTimer, true, t, "Line width: %d", prefs.line_width); }
    void showParticles(bool t) { showTimedText(ParticlesTimer, true, t, "Particles: %s", prefs.particles ? "on" : "off"); }
    void showBloomIntensity(bool t) { showTimedText(BloomTimer, true, t, "Bloom intensity: %.4f", prefs.bloom_intensity); }
    void showBloomGamma(bool t) { showTimedText(BloomGammaTimer, true, t, "Bloom gamma: %.1f", prefs.bloom_gamma); }
    void showBloomRadius(bool t) { showTimedText(BloomRadiusTimer, true, t, "Bloom radius: %.1f", prefs.bloom_radius); }
    void showColorMode(bool t) { showTimedText(ColorModeTimer, true, t, "Color mode: %s", color_mode_names[prefs.color_mode]); }
    void showDisplayMode(bool t) { showTimedText(DisplayModeTimer, true, t, "Display mode: %s", display_mode_names[prefs.display_mode]); }
    void showColorRange(bool t) { showTimedText(ColorRangeTimer, true, t, "Color range: %.2f", prefs.color_range); }
    void showColorRate(bool t) { showTimedText(ColorRateTimer, true, t, "Color rate: %.2f", prefs.color_rate); }
    void showDelay(bool t) { showTimedText(DelayTimer, true, t, "Delay: %.2f ms", prefs.delay); }
    void showBrightness(bool t) {
#ifdef _WIN32
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f %s",
                      prefs.brightness, hdr_hdc ? "(HDR)" : "(SDR)");
#elif defined(__APPLE__)
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f", prefs.brightness);
#else
        showTimedText(BrightnessTimer, true, t, "Brightness: %.1f %s",
                      prefs.brightness, wayland_hdr_active ? "(HDR)" : "(SDR)");
#endif
    }
    void showVelocityDim(bool t) { showTimedText(VelocityDimTimer, true, t, "Velocity dim: %.1f", prefs.velocity_dim); }
    void showSampleRate(bool t) { showTimedText(SampleRateTimer, true, t, "Sample rate: %d Hz", sample_rate); }
    void showFrameRate(bool t) { showTimedText(FrameRateTimer, true, t, "Frame rate: %d fps", frame_rate); }

    /* Other timers */
    void showPaused(bool t) { showTimedText(PausedTimer, true, t, "Paused"); }

    void showScale(bool timed)
    {
        text_timer_t *timer  = &text_timer[ScaleTimer];
        timer->auto_position = false;
        timer->x_position    =  80.0;
        timer->y_position    = -100.0;
        if (timed)
            gettimeofday(&timer->time, NULL);
        timer->show = true;
    }

    void showCounter(bool timed)
    {
        text_timer_t *timer  = &text_timer[CounterTimer];
        timer->auto_position = false;
        timer->x_position    = -80.0;
        timer->y_position    =   60.0;
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

    void autoScale();
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
        max_sample_value = min((prefs.side[0] - prefs.side[1]) / 2.1,
                               (prefs.side[2] - prefs.side[3]) / 2.1);
        prefs.auto_scale = ! prefs.auto_scale;
        showAutoScale(TIMED);
    }

    void moreSplines(void)
    {
        if (prefs.spline_steps < 1024)
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
        if (prefs.display_mode == DisplaySpectrumMode)
            setColorRange(prefs.color_range);   /* re-clamp for new mode */
        showDisplayMode(TIMED);
    }

    void prevDisplayMode(void)
    {
        if (prefs.display_mode < 1)
            prefs.display_mode = NUM_DISPLAY_MODES - 1;
        else
            prefs.display_mode = prefs.display_mode - 1;
        if (prefs.display_mode == DisplaySpectrumMode)
            setColorRange(prefs.color_range);   /* re-clamp for new mode */
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
            if (fs_cover_hwnd) ShowWindow(fs_cover_hwnd, SW_HIDE);
            HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
            if (taskbar) ShowWindow(taskbar, SW_SHOW);

            SDL_SysWMinfo wminfo;
            SDL_VERSION(&wminfo.version);
            if (SDL_GetWindowWMInfo(window, &wminfo)) {
                HWND hwnd = wminfo.info.win.window;
                LONG style = GetWindowLong(hwnd, GWL_STYLE);
                SetWindowLong(hwnd, GWL_STYLE,
                    style | WS_CAPTION | WS_THICKFRAME);
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                    SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
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
                /* Borderless window 1 pixel shorter than the desktop
                 * to prevent Windows/GPU driver from promoting to
                 * exclusive fullscreen (which breaks overlays and
                 * audio).  Hide the taskbar so the gap isn't visible. */
                HWND taskbar = FindWindow("Shell_TrayWnd", NULL);
                if (taskbar) ShowWindow(taskbar, SW_HIDE);

                SDL_SysWMinfo wminfo;
                SDL_VERSION(&wminfo.version);
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    HWND hwnd = wminfo.info.win.window;
                    /* Suppress repainting during transition */
                    SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
                    /* Remove border in one shot */
                    LONG style = GetWindowLong(hwnd, GWL_STYLE);
                    SetWindowLong(hwnd, GWL_STYLE,
                        style & ~(WS_CAPTION | WS_THICKFRAME));
                    /* Resize + reposition atomically */
                    SetWindowPos(hwnd, HWND_TOP, 0, 0,
                                 mode.w, mode.h - 1,
                                 SWP_FRAMECHANGED);
                    SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
                }

                /* Black cover window for the 1-pixel gap at the bottom */
                if (!fs_cover_hwnd) {
                    WNDCLASSA wc = {};
                    wc.lpfnWndProc   = DefWindowProcA;
                    wc.hInstance     = GetModuleHandle(NULL);
                    wc.lpszClassName = "XYScopeCover";
                    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    RegisterClassA(&wc);
                    fs_cover_hwnd = CreateWindowExA(
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        wc.lpszClassName, "",
                        WS_POPUP | WS_VISIBLE,
                        0, mode.h - 1, mode.w, 1,
                        NULL, NULL, wc.hInstance, NULL);
                } else {
                    SetWindowPos(fs_cover_hwnd, HWND_TOP,
                                 0, mode.h - 1, mode.w, 1,
                                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
                }

                /* Grab focus after everything is set up */
                if (SDL_GetWindowWMInfo(window, &wminfo)) {
                    SetForegroundWindow(wminfo.info.win.window);
                    SetFocus(wminfo.info.win.window);
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
        if (prefs.display_mode == DisplaySpectrumMode) {
            /* Spectrum mode uses color_range as an octave index for
             * window_size. Floor is 1; each integer step doubles
             * window_size from base=64, and we stop once the next
             * doubling would overflow draw_frames or the 2048 vDSP
             * cap. */
            int max_steps = 0;
            unsigned int base = 1;
            while (base * 2 <= (unsigned int)(64 * sample_rate / 96000))
                base *= 2;
            unsigned int ws = base;
            while (ws * 2 <= (unsigned int)draw_frames && ws * 2 <= 2048) {
                ws *= 2;
                max_steps++;
            }
            if (prefs.color_range < 1.0) prefs.color_range = 1.0;
            if (prefs.color_range > (double)max_steps) prefs.color_range = (double)max_steps;
        } else {
            wrapValue(&prefs.color_range, 100.0);
        }
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

    void setVelocityDim(double d)
    {
        prefs.velocity_dim = d;
        if (prefs.velocity_dim < 0.0) prefs.velocity_dim = 0.0;
        showVelocityDim(TIMED);
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

    void toggleParticles(void)
    {
        prefs.particles = !prefs.particles;
        showParticles(TIMED);
    }

    void setBloomIntensity(double v)
    {
        if (v < 0.0001) v = 0.0;
        prefs.bloom_intensity = v;
        showBloomIntensity(TIMED);
    }

    void setBloomGamma(double v)
    {
        prefs.bloom_gamma = v;
        if (prefs.bloom_gamma < 0.1) prefs.bloom_gamma = 0.1;
        showBloomGamma(TIMED);
    }

    void setBloomRadius(double v)
    {
        prefs.bloom_radius = v;
        if (prefs.bloom_radius < 0.5) prefs.bloom_radius = 0.5;
        showBloomRadius(TIMED);
    }

    void savePreset(int n)
    {
        presets.slot[n] = prefs;
        presets.saved[n] = true;
        showTimedText(PresetTimer, true, TIMED, "Preset %d saved", n);
    }

    unsigned int default_spline_steps()
    {
        unsigned int s = DEFAULT_SPLINE_STEPS * 96000 / sample_rate;
        if (s < 2) s = 2;
        if (s > 128) s = 128;
        return s;
    }

    void validate_prefs()
    {
        if (prefs.normal_dim[0] < 1) prefs.normal_dim[0] = 1000;
        if (prefs.normal_dim[1] < 1) prefs.normal_dim[1] = 1000;
        /* Old config files may have display_mode=3 (was Spectrum when
         * Frequency was mode 2). Map both 2 and 3 to Spectrum. */
        if (prefs.display_mode == 3)
            prefs.display_mode = DisplaySpectrumMode;
        if (prefs.display_mode >= NUM_DISPLAY_MODES)
            prefs.display_mode = DefaultDisplayMode;
        if (prefs.color_mode >= NUM_COLOR_MODES)
            prefs.color_mode = DefaultColorMode;
        if (prefs.spline_steps < 1 || prefs.spline_steps > 1024)
            prefs.spline_steps = default_spline_steps();
        if (prefs.line_width < 1 || prefs.line_width > MAX_LINE_WIDTH)
            prefs.line_width = DEFAULT_LINE_WIDTH;
        if (prefs.bloom_gamma < 0.1)
            prefs.bloom_gamma = DEFAULT_BLOOM_GAMMA;
        if (prefs.bloom_radius < 0.5)
            prefs.bloom_radius = DEFAULT_BLOOM_RADIUS;
    }

    void loadPreset(int n)
    {
        if (!presets.saved[n]) {
            showTimedText(PresetTimer, true, TIMED, "Preset %d empty", n);
            return;
        }
        /* preserve window geometry and fullscreen state */
        int dim[2], normal_dim[2], old_dim[2], position[2];
        bool is_full_screen;
        memcpy(dim, prefs.dim, sizeof(dim));
        memcpy(normal_dim, prefs.normal_dim, sizeof(normal_dim));
        memcpy(old_dim, prefs.old_dim, sizeof(old_dim));
        memcpy(position, prefs.position, sizeof(position));
        is_full_screen = prefs.is_full_screen;

        prefs = presets.slot[n];
        validate_prefs();

        memcpy(prefs.dim, dim, sizeof(dim));
        memcpy(prefs.normal_dim, normal_dim, sizeof(normal_dim));
        memcpy(prefs.old_dim, old_dim, sizeof(old_dim));
        memcpy(prefs.position, position, sizeof(position));
        prefs.is_full_screen = is_full_screen;

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
        refreshStats(TIMED);
        showTimedText(PresetTimer, true, TIMED, "Preset %d loaded", n);
    }

    void loadDefaults()
    {
        prefs.scale_factor  = 1.0;
        prefs.scale_locked  = true;
        prefs.auto_scale    = DEFAULT_AUTO_SCALE;
        prefs.spline_steps  = default_spline_steps();
        prefs.color_mode    = DEFAULT_COLOR_MODE;
        prefs.color_range   = DEFAULT_COLOR_RANGE;
        prefs.color_rate    = DEFAULT_COLOR_RATE;
        prefs.display_mode  = DEFAULT_DISPLAY_MODE;
        prefs.line_width    = DEFAULT_LINE_WIDTH;
        prefs.particles     = DEFAULT_PARTICLES;
        prefs.hue           = 0.0;
#ifdef _WIN32
        if (g_hdr_present.enabled && g_hdr_present.peak_nits > 0.0) {
            /* HDR active: reuse the panel peak already detected for the
             * scRGB swapchain (no second detection). Scope traces are thin
             * anti-aliased lines covering ~1/5 of a pixel, so default to
             * peak/15 — the trace uses the headroom without per-pixel
             * full-coverage clipping. ~100 on a 1500-nit panel, scaling
             * down for dimmer HDR displays. */
            prefs.brightness = g_hdr_present.peak_nits / 15.0;
        } else
#endif
        {
            double detected = detect_hdr_brightness(window);
            if (detected < 1.0) detected = 1.0;
            prefs.brightness = detected;
        }
        if (prefs.brightness < 2.0)
            prefs.brightness = 2.0;
        prefs.velocity_dim  = prefs.brightness;
        if (prefs.velocity_dim < 4.0)
            prefs.velocity_dim = 4.0;
        prefs.bloom_intensity = DEFAULT_BLOOM_INTENSITY;
        prefs.bloom_gamma     = DEFAULT_BLOOM_GAMMA;
        prefs.bloom_radius    = DEFAULT_BLOOM_RADIUS;
        max_sample_value = min((prefs.side[0] - prefs.side[1]) / 2.1,
                               (prefs.side[2] - prefs.side[3]) / 2.1);
        refreshStats(TIMED);
        showTimedText(PresetTimer, true, TIMED, "Defaults loaded");
    }

    void refreshStats(bool t)
    {
        showAutoScale(t);
        showSplines(t);
        showLineWidth(t);
        showParticles(t);
        showColorMode(t);
        showDisplayMode(t);
        showColorRange(t);
        showColorRate(t);
        showDelay(t);
        showBrightness(t);
        showVelocityDim(t);
        showBloomIntensity(t);
        showBloomGamma(t);
        showBloomRadius(t);
    }
};

#endif /* XYSCOPE_SCENE_H */
