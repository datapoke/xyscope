/*
 *  xyscope-callbacks.h
 *  GLUT-style display/idle/input callbacks. Extracted from xyscope.mm;
 *  included after the `scn` and `bloom` globals are defined, since
 *  these reference them.
 */
#ifndef XYSCOPE_CALLBACKS_H
#define XYSCOPE_CALLBACKS_H

void display()
{
    glClear(GL_COLOR_BUFFER_BIT);

    /* plot the samples on the screen */
    bool use_bloom = bloom.enabled && scn.prefs.bloom_intensity > 0.0;
    if (use_bloom) bloom_begin(&bloom);
    scn.drawPlot();
    if (use_bloom) bloom_end(&bloom, (float)scn.prefs.bloom_intensity, (float)scn.prefs.bloom_gamma, (float)scn.prefs.bloom_radius);

    /* draw any text that needs drawing */
    if (!scn.dj_mode) scn.drawText();

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
        bloom_resize(&bloom, drawable_w, drawable_h);
#ifdef _WIN32
        /* Resize the offscreen GL window so FBO 0 matches the visible
         * window, then resize the DXGI swapchain to the same size. */
        if (gl_hidden_hwnd) {
            RECT r = { 0, 0, drawable_w, drawable_h };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(gl_hidden_hwnd, NULL, 0, 0,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        hdr_present_resize(&g_hdr_present, drawable_w, drawable_h);
#endif
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
            scn.setWindowSize(800, 800);
            break;
        case 2:                    /* F2 */
            scn.setWindowSize(1000, 1000);
            break;
        case 3:                    /* F3 */
            scn.setWindowSize(1400, 1400);
            break;
        case 4:                    /* F4 */
            scn.setWindowSize(2000, 2000);
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
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            scn.loadPreset(key - '0');
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
        case 'l':
            scn.moreSplines();
            break;
        case 'L':
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
        case '/':
            scn.dj_mode = !scn.dj_mode;
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
        case 'p':
            scn.toggleParticles();
            break;
        case 'b': {
            double v = scn.prefs.bloom_intensity;
            if (v == 0.0) v = 0.0001;
            else {
                double step = (v < 0.0995)
                              ? ((v < 0.00995)
                                 ? ((v < 0.000995) ? 0.0001 : 0.001)
                                 : 0.01)
                              : 0.1;
                v += step;
            }
            scn.setBloomIntensity(v);
            break;
        }
        case 'B': {
            double v = scn.prefs.bloom_intensity;
            double step = (v <= 0.1005)
                          ? ((v <= 0.01005)
                             ? ((v <= 0.001005) ? 0.0001 : 0.001)
                             : 0.01)
                          : 0.1;
            scn.setBloomIntensity(v - step);
            break;
        }
        case 'v':
            scn.setBloomGamma(scn.prefs.bloom_gamma + 0.1);
            break;
        case 'V':
            scn.setBloomGamma(scn.prefs.bloom_gamma - 0.1);
            break;
        case 'g':
            scn.setBloomRadius(scn.prefs.bloom_radius + 0.5);
            break;
        case 'G':
            scn.setBloomRadius(scn.prefs.bloom_radius - 0.5);
            break;
        case 'i':
            scn.setBrightness(scn.getBrightness() + 1.0);
            break;
        case 'I':
            scn.setBrightness(scn.getBrightness() + 0.1);
            break;
        case 'u':
            scn.setBrightness(scn.getBrightness() - 1.0);
            break;
        case 'U':
            scn.setBrightness(scn.getBrightness() - 0.1);
            break;
        case 'n':
            scn.setVelocityDim(scn.prefs.velocity_dim - 1.0);
            break;
        case 'm':
            scn.setVelocityDim(scn.prefs.velocity_dim + 1.0);
            break;
        case 'N':
            scn.setVelocityDim(scn.prefs.velocity_dim - 0.1);
            break;
        case 'M':
            scn.setVelocityDim(scn.prefs.velocity_dim + 0.1);
            break;
        case '`':
            scn.loadDefaults();
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

#endif /* XYSCOPE_CALLBACKS_H */
