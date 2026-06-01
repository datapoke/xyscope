/*
 *  xyscope-scene-text.h
 *  scene text rendering (begin/draw/end, help, stats, timed text).
 *  Out-of-line scene methods; included into xyscope.mm after
 *  xyscope-scene.h.
 */
#ifndef XYSCOPE_SCENE_TEXT_H
#define XYSCOPE_SCENE_TEXT_H

/* <unordered_map>/<string> are included at the top of xyscope.mm, before
 * the min/max function-macros (which otherwise break libstdc++'s
 * std::min/std::max template declarations). */

/* Cache of rendered text textures keyed by exact string content. Static
 * strings (e.g. the 56 help lines) become permanent cache hits instead of
 * being re-rasterized every frame; dynamic strings (fps/latency, unique
 * each frame) miss and re-render but are LRU-bounded so the cache can't
 * grow without limit. This kills the per-frame TTF + texture-alloc churn
 * that made heavy text (help) glitch under load. */
struct text_cache_entry_t {
    GLuint        texture;
    int           w, h;
    unsigned long tick;
};
static std::unordered_map<std::string, text_cache_entry_t> g_text_cache;
static unsigned long g_text_tick = 0;
static const size_t  TEXT_CACHE_MAX = 256;

void scene::beginText()
{
    top_offset = -160.0;
    if (text_timer[ScaleTimer].show)
        top_offset = -220.0;

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

double scene::getTextWidth(char *string)
{
    if (!font || !string || strlen(string) == 0) return 0.0;

    int char_width = 0;
    TTF_SizeText(font, "M", &char_width, NULL);

    return (double)(strlen(string) * char_width);
}

void scene::drawString(double x, double y, char *string)
{
    if (!font || !string || strlen(string) == 0) return;

    /* Fetch the cached texture for this exact string, or rasterize +
     * cache it on a miss. Static strings (help) hit; dynamic ones miss
     * and re-render but are LRU-bounded below. */
    GLuint texture;
    int    tex_w, tex_h;
    std::string key(string);
    auto it = g_text_cache.find(key);
    if (it != g_text_cache.end()) {
        texture = it->second.texture;
        tex_w   = it->second.w;
        tex_h   = it->second.h;
        it->second.tick = ++g_text_tick;
    } else {
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface *text_surface = TTF_RenderText_Blended(font, string, white);
        if (!text_surface) {
            fprintf(stderr, "TTF_RenderText_Blended failed: %s\n", TTF_GetError());
            return;
        }
        SDL_Surface *rgba_surface = SDL_ConvertSurfaceFormat(text_surface, SDL_PIXELFORMAT_ABGR8888, 0);
        SDL_FreeSurface(text_surface);
        if (!rgba_surface) {
            fprintf(stderr, "SDL_ConvertSurfaceFormat failed: %s\n", SDL_GetError());
            return;
        }
        tex_w = rgba_surface->w;
        tex_h = rgba_surface->h;

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_surface->pixels);
        SDL_FreeSurface(rgba_surface);

        /* Bound the cache: evict the least-recently-used entry. */
        if (g_text_cache.size() >= TEXT_CACHE_MAX) {
            auto oldest = g_text_cache.begin();
            for (auto i2 = g_text_cache.begin(); i2 != g_text_cache.end(); ++i2)
                if (i2->second.tick < oldest->second.tick) oldest = i2;
            glDeleteTextures(1, &oldest->second.texture);
            g_text_cache.erase(oldest);
        }
        text_cache_entry_t e;
        e.texture = texture; e.w = tex_w; e.h = tex_h; e.tick = ++g_text_tick;
        g_text_cache[key] = e;
    }

    // Calculate text width and height in normalized coordinates
    double text_w = (double)tex_w / (double)prefs.dim[0] * 2.0;
    double text_h = (double)tex_h / (double)prefs.dim[1] * 2.0;

    /* Position: positive x = offset from left edge.
     * Negative x = right-align with margin from right edge.
     * For right-align, use actual rendered surface width
     * so position is stable regardless of content. */
    if (x >= 0.0)
        x = -1.0 + x / (double) prefs.dim[0];
    else
        x = 1.0 - text_w - (-x) / (double) prefs.dim[0];

    if (y >= 0.0)
        y = -1.0 + y / (double) prefs.dim[1];
    else
        y =  1.0 + y / (double) prefs.dim[1];

    // Draw textured quad (flip Y texture coordinate for SDL surfaces)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
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
}

void scene::endText()
{
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void scene::drawHelp()
{
    double left_offset   =  80.0;
    double right_offset  = 740.0;
    char help[][2][64] = {
    { "Escape",            "Quit" },
    { "F1 thru F5",        "Quickly resize window" },
    { "Home and Page Up",  "Zoom in" },
    { "End and Page Down", "Zoom out" },
    { "Shift+0 thru 9",    "Set zoom factor" },
    { "`",                 "Load default settings" },
    { "0 thru 9",          "Load preset" },
    { "Ctrl+0 thru 9",     "Save preset" },
    { "Spacebar",          "Pause/Resume" },
    { "< and >",           "Rewind/Fast-Forward when paused" },
    { "[ and ]",           "Adjust color range" },
    { "- and +",           "Adjust color rate" },
    { "a",                 "Auto-scale on/off" },
    { "c and C",           "Color mode" },
    { "d and D",           "Display mode" },
    { "f",                 "Enter/Exit full screen mode" },
    { "h",                 "Show/Hide help" },
    { "/",                 "DJ mode (hide all text)" },
    { "l and L",           "Adjust splines" },
    { "u/i and U/I",       "Adjust brightness" },
    { "b and B",           "Adjust bloom intensity" },
    { "v and V",           "Adjust bloom gamma" },
    { "g and G",           "Adjust bloom radius" },
    { "j/k and J/K",       "Adjust display delay" },
    { "n/m and N/M",       "Adjust velocity dim" },
    { "r",                 "Recenter" },
    { "s and S",           "Show/Hide statistics" },
    { "w and W",           "Adjust line width" },
    { "p",                 "Particles on/off" }
    };
    unsigned int n_items = sizeof(help) / sizeof(help[0]);

    for (unsigned int i = 0; i < n_items; i++) {
        if (-top_offset > 2 * prefs.dim[1] - 120)
            break;
        drawString(left_offset,  top_offset, help[i][0]);
        drawString(right_offset, top_offset, help[i][1]);
        top_offset += vertical_increment;
    }
    top_offset -= 60.0;
}

void scene::drawTimedText()
{
    timeval this_frame_time;
    double elapsed_time;
    double x = 80.0;
    gettimeofday(&this_frame_time, NULL);
    for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
        if (text_timer[i].show) {
            /* get the time so we can calculate how long to display */
            elapsed_time = timeDiff(text_timer[i].time,
                                     this_frame_time);
            if (elapsed_time > 10.0)
                text_timer[i].show = false;

            if (text_timer[i].auto_position) {
                if (-top_offset > 2 * prefs.dim[1] - 120)
                    continue;
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

void scene::drawStats()
{
    thread_data_t *t_data = ai->getThreadData();
    timeval this_frame_time;
    double elapsed_time;
    /* char color_threshold_string[64]; */
    char fps_string[64];
    char vps_string[64];
    char time_string[64];

    /* Frame counting — always runs, needed by frame rate limiter */
    gettimeofday(&this_frame_time, NULL);
    elapsed_time = timeDiff(reset_frame_time, this_frame_time);
    frame_count++;
    if (elapsed_time >= 1.0) {
        fps = frame_count / elapsed_time;
        reset_frame_time = this_frame_time;
        frame_count = 0;
    }
    last_frame_time = this_frame_time;

    if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3)) {
        snprintf(fps_string, sizeof(fps_string), "%.1f fps", fps);
        drawString(60.0, 60.0, fps_string);
        snprintf(vps_string, sizeof(vps_string), "%d vps", vertex_count * frame_rate);
        drawString(-80.0, -100.0, vps_string);
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
        drawString(-80.0, 60.0, time_string);
    }
}

void scene::drawText(void)
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

void scene::showTimedText(int timer_idx, bool auto_pos, bool timed, const char *fmt, ...)
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

/* Stats timers */

#endif /* XYSCOPE_SCENE_TEXT_H */
