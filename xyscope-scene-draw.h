/*
 *  xyscope-scene-draw.h
 *  scene::drawPlot (sample read + FFT + spline/vertex emission) and
 *  scene::autoScale. Out-of-line scene methods; included into xyscope.mm
 *  after xyscope-scene.h (the class declaration) and the GL globals.
 */
#ifndef XYSCOPE_SCENE_DRAW_H
#define XYSCOPE_SCENE_DRAW_H

void scene::drawPlot()
{
    thread_data_t *t_data = ai->getThreadData();
    size_t bytes_ready = 0, bytes_read = 0;
    double dt  = 0.0;
    signed int distance = 0;

    /* Frame counting — always runs, needed by frame rate limiter */
    double elapsed_time;
    gettimeofday(&this_frame_time, NULL);
    elapsed_time = timeDiff(reset_frame_time, this_frame_time);
    frame_count++;
    if (elapsed_time >= 1.0) {
        fps = frame_count / elapsed_time;
        reset_frame_time = this_frame_time;
        frame_count = 0;
    }
    last_frame_time = this_frame_time;

    /* FFT stuff */
    unsigned int window_size, overlap_size;
    if (prefs.display_mode == DisplaySpectrumMode) {
        /* Spectrum mode: color_range indexes octaves of window_size
         * so each integer step of color_range doubles the FFT
         * window (and halves the bin width). Floor is 1.
         *
         *   color_range  1   2   3   4   5
         *   window_size  128 256 512 1024 2048
         *
         * Default color_range=1 gives window_size=128, bin_width
         * 750 Hz. Cranks above that give progressively finer
         * frequency resolution at the cost of fewer STFT windows
         * per frame.
         *
         * overlap_size = 0 means windows tile (stride = window_size)
         * rather than 50%-overlapping. That halves the FFT count
         * per frame at small window sizes without meaningfully
         * reducing color variation, and when frames_read isn't a
         * clean multiple of window_size the aggregation adds one
         * "nudged" STFT at frames_read-window_size to cover the
         * trailing samples. */
        /* Base window scales with sample rate so the same
         * color_range gives the same bin width at any rate.
         * At 96 kHz base=64 → color_range 1=128, 2=256, etc.
         * At 192 kHz base=128 → color_range 1=256, 2=512, etc.
         * At 48 kHz base=32 → color_range 1=64, 2=128, etc. */
        unsigned int base = 1;
        while (base * 2 <= (unsigned int)(64 * sample_rate / 96000))
            base *= 2;
        int steps = (int)prefs.color_range;
        if (steps < 0)  steps = 0;
        if (steps > 10) steps = 10;
        window_size = base;
        for (int i = 0; i < steps; i++) {
            unsigned int next = window_size * 2;
            if (next > (unsigned int)draw_frames || next > 2048) break;
            window_size = next;
        }
        overlap_size = 0;
    } else {
        window_size  = draw_frames / 100;
        overlap_size = draw_frames / 200;
        if (window_size < 2) window_size = 2;
        if (overlap_size >= window_size) overlap_size = window_size / 2;
    }
    double* spectrum_colors = NULL;  /* per-window RGB triples for DisplaySpectrumMode */
    double** stft_results;
#ifndef __APPLE__
    fftw_plan fft_plan;
#endif

    /* if the scope is paused or audio not initialized, there are no samples available;
     * therefore we should not wait for the reader thread */
    if (! t_data->pause_scope && t_data->can_process) {
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
        pthread_mutex_lock(&t_data->ringbuffer_lock);

        // Use timed wait to avoid hanging forever if audio fails
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 1000000000 / frame_rate;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec  += 1;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&t_data->data_ready, &t_data->ringbuffer_lock, &ts);
    }


    /* Read data from the ring buffer */
    if (t_data->pause_scope) {
        distance = bump * frame_size;
        bump     = -draw_frames;
    }
    else if (t_data->ringbuffer) {
        int delay_frames = (int)(prefs.delay * 0.001 * sample_rate);
        int delay_bytes  = delay_frames * frame_size;
        bytes_ready = ringbuffer_read_space(t_data->ringbuffer);
        if (bytes_ready != (size_t)(bytes_per_buf + delay_bytes))
            distance = bytes_ready - bytes_per_buf - delay_bytes;
    }
    if (distance != 0 && t_data->ringbuffer)
        ringbuffer_read_advance(t_data->ringbuffer, distance);
    if (t_data->ringbuffer)
        bytes_read = ringbuffer_read(t_data->ringbuffer,
                                      (char *) framebuf,
                                      bytes_per_buf);

    if (! t_data->pause_scope)
        pthread_mutex_unlock(&t_data->ringbuffer_lock);

    frames_read = bytes_read / frame_size;


    /* prescans the framebuf in order to auto-scale */
    if (prefs.auto_scale)
        autoScale();


    /* set up the OpenGL */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    glOrtho(prefs.side[3], prefs.side[2],
             prefs.side[1], prefs.side[0],
             -10.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    if (prefs.particles) {
        glPointSize((GLfloat) prefs.line_width);
    }
    else {
        glLineWidth((GLfloat) prefs.line_width);
    }

    /* FFT setup for spectrum mode — runs on raw samples before
     * spline interpolation so it sees the original signal. */
    if (prefs.display_mode == DisplaySpectrumMode) {
            unsigned int fft_count = frames_read;
            unsigned int window_size_fft = window_size;
            unsigned int overlap_size_fft = overlap_size;
            unsigned int stride_fft = window_size_fft - overlap_size_fft;

            /* Build FFT input. Complex FFT: L=real, R read from
             * framebuf in the lambda as imaginary (L+iR). */
            double *fft_input = new double[fft_count];
            for (unsigned int i = 0; i < fft_count; i++) {
                fft_input[i] = framebuf[i].left_channel;
            }

            /* Allocate n_windows + 1 STFT slots. The extra slot is
             * either the "nudged" tail window in spectrum mode or
             * trailing carry-forward in both modes. n_windows in
             * audio units. */
            unsigned int stride_audio = window_size - overlap_size;
            unsigned int n_windows_audio = frames_read / stride_audio;
            unsigned int n_stft_slots = n_windows_audio + 1;
            stft_results = new double*[n_stft_slots];
            for (unsigned int i = 0; i < n_stft_slots; i++) {
                stft_results[i] = new double[window_size_fft]();
            }
#ifdef __APPLE__
            // Set up vDSP FFT once outside loop (performance optimization)
            int log2n_win = 0;
            int n_win = window_size_fft;
            while (n_win > 1) { n_win >>= 1; log2n_win++; }
            FFTSetup fft_setup_local = vDSP_create_fftsetup(log2n_win, FFT_RADIX2);
            DSPSplitComplex fft_data;
            /* Full N for complex FFT (spectrum), N/2 for real FFT (frequency) */
            unsigned int fft_alloc = (prefs.display_mode == DisplaySpectrumMode)
                ? window_size_fft : window_size_fft / 2;
            fft_data.realp = new float[fft_alloc];
            fft_data.imagp = new float[fft_alloc];
#else
            fftw_complex *fft_out_local =
                (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * window_size_fft);
#endif
            bool spectrum = (prefs.display_mode == DisplaySpectrumMode);

            auto compute_fft_at = [&](unsigned int start_i, unsigned int target_slot) {
#ifdef __APPLE__
                if (spectrum) {
                    /* Complex FFT: L=real, R=imag */
                    for (unsigned int j = 0; j < window_size_fft; j++) {
                        fft_data.realp[j] = (float)fft_input[start_i + j];
                        fft_data.imagp[j] = (float)framebuf[start_i + j].right_channel;
                    }
                    vDSP_fft_zip(fft_setup_local, &fft_data, 1, log2n_win, FFT_FORWARD);
                } else {
                    /* Real FFT: mono */
                    float *input_data = new float[window_size_fft];
                    for (unsigned int j = 0; j < window_size_fft; j++) {
                        input_data[j] = (float)fft_input[start_i + j];
                    }
                    vDSP_ctoz((DSPComplex*)input_data, 2, &fft_data, 1, window_size_fft/2);
                    vDSP_fft_zrip(fft_setup_local, &fft_data, 1, log2n_win, FFT_FORWARD);
                    delete[] input_data;
                }
                /* For the complex FFT (spectrum mode), combine
                 * positive and negative frequency bins so the
                 * magnitude is rotation-direction-independent.
                 * Clockwise XY motion puts energy in negative bins
                 * (N-k), counterclockwise in positive bins (k).
                 * Summing both makes the spectrum invariant to
                 * rotation direction. For real FFT (frequency
                 * mode), negative bins mirror positive so this
                 * is a harmless 2x scale. */
                for (unsigned int j = 0; j < window_size_fft/2; j++) {
                    double rp = fft_data.realp[j];
                    double ip = fft_data.imagp[j];
                    double mag = rp*rp + ip*ip;
                    if (spectrum && j > 0 && j < window_size_fft/2) {
                        unsigned int nj = window_size_fft - j;
                        double rn = fft_data.realp[nj];
                        double in_ = fft_data.imagp[nj];
                        mag += rn*rn + in_*in_;
                    }
                    stft_results[target_slot][j] = sqrt(mag);
                }
#else
                double (*temp_data)[2] = new double[window_size_fft][2];
                for (unsigned int j = 0; j < window_size_fft; j++) {
                    temp_data[j][0] = fft_input[start_i + j];
                    temp_data[j][1] = spectrum
                        ? framebuf[start_i + j].right_channel
                        : 0.0;
                }
                fft_plan = fftw_plan_dft_1d(window_size_fft, temp_data, fft_out_local, FFTW_FORWARD, FFTW_ESTIMATE);
                fftw_execute(fft_plan);
                for (unsigned int j = 0; j < window_size_fft/2; j++) {
                    double rp = fft_out_local[j][0];
                    double ip = fft_out_local[j][1];
                    double mag = rp*rp + ip*ip;
                    if (spectrum && j > 0) {
                        unsigned int nj = window_size_fft - j;
                        double rn = fft_out_local[nj][0];
                        double in_ = fft_out_local[nj][1];
                        mag += rn*rn + in_*in_;
                    }
                    stft_results[target_slot][j] = sqrt(mag);
                }
                fftw_destroy_plan(fft_plan);
                delete[] temp_data;
#endif
            };

            // Regular STFT loop over the (possibly splined) FFT input
            unsigned int w_idx = 0;
            for (unsigned int i = 0; i + window_size_fft <= fft_count; i += stride_fft) {
                compute_fft_at(i, w_idx);
                w_idx++;
            }

            /* Nudge: in spectrum mode, if the last regular window
             * doesn't cover the end of the frame, add one more FFT
             * positioned to end exactly at fft_count. It lands in
             * slot n_windows_audio (the last allocated slot), which
             * is exactly where vertex indexing sends the trailing
             * vertices via `i / stride`. */
            if (prefs.display_mode == DisplaySpectrumMode && w_idx > 0) {
                unsigned int last_end = (w_idx - 1) * stride_fft + window_size_fft;
                if (last_end < fft_count) {
                    compute_fft_at(fft_count - window_size_fft, n_windows_audio);
                }
            }
            delete[] fft_input;
#ifdef __APPLE__
            // Clean up FFT resources after loop
            vDSP_destroy_fftsetup(fft_setup_local);
            delete[] fft_data.realp;
            delete[] fft_data.imagp;
#else
            fftw_free(fft_out_local);
#endif

            unsigned int n_windows = frames_read / (window_size - overlap_size);

            {
                /* Spectrum mode:
                 *   R = sum(bin0..r_last)    (~0–1 kHz, sub-bass+kick)
                 *   G = sum(r_last+1..g_last) (~1–5 kHz, fat mid)
                 *   B = sum(g_last+1..b_last) (~5–20 kHz, audible treble)
                 * Boundaries are computed dynamically from bin_width
                 * so they adapt to whatever FFT window color_range
                 * picked — at baseline (window_size=64, bin_width
                 * 1500 Hz) this gives R=bin0, G=bins 1..3, B=bins
                 * 4..13. At larger windows each band gets many more
                 * bins and finer frequency resolution.
                 *
                 * Supersonic bins (>20 kHz) are excluded so pure
                 * tones with no real treble don't get false blue
                 * from accumulated noise or spline overshoot.
                 *
                 * Then divide all three by the per-frame max
                 * CHANNEL value so the strongest band in the
                 * frame is exactly 1.0 and the other two are
                 * proportional ratios less than 1.0. */
                unsigned int half_w = window_size_fft / 2;
                /* Bin width = sample_rate / window_size (same as
                 * for the raw-audio FFT because spline upsampling
                 * scales both the effective sample rate AND the
                 * window size proportionally). */
                double bin_width_hz = (double)sample_rate / (double)window_size;
                /* Log-spaced boundaries (×10 each) so each band
                 * spans one decade — perceptually closer to how
                 * humans hear pitch (octaves), and white noise
                 * with max-per-band aggregation comes out
                 * actually white instead of B-skewed. */
                unsigned int r_last = (unsigned int)(149.0 / bin_width_hz);
                unsigned int g_last = (unsigned int)(1490.0 / bin_width_hz);
                unsigned int b_last = (unsigned int)(14900.0 / bin_width_hz);
                /* Enforce r_last < g_last < b_last < half_w,
                 * leaving at least one bin per band. */
                if (r_last >= half_w)            r_last = half_w - 3;
                if (g_last <= r_last)            g_last = r_last + 1;
                if (b_last <= g_last)            b_last = g_last + 1;
                if (b_last >= half_w)            b_last = half_w - 1;
                spectrum_colors = new double[(n_windows + 1) * 3]();
                /* First pass: take the max bin in each band and
                 * track the max CHANNEL value across the whole
                 * frame. Iterates to n_windows INCLUSIVE: the
                 * extra slot holds either the nudged tail FFT
                 * (if there was a tail gap) or zero (which
                 * triggers the carry-forward in the second
                 * pass). Either way the padding slot
                 * participates in aggregation so vertex indexing
                 * beyond the last regular window gets a sensible
                 * color. */
                double max_v = 0.0;
                for (unsigned int i = 0; i <= n_windows; i++) {
                    double R = 0.0;
                    for (unsigned int j = 0; j <= r_last; j++) {
                        if (stft_results[i][j] > R) R = stft_results[i][j];
                    }
                    double G = 0.0;
                    for (unsigned int j = r_last + 1; j <= g_last; j++) {
                        if (stft_results[i][j] > G) G = stft_results[i][j];
                    }
                    double B = 0.0;
                    for (unsigned int j = g_last + 1; j <= b_last; j++) {
                        if (stft_results[i][j] > B) B = stft_results[i][j];
                    }
                    spectrum_colors[i * 3 + 0] = R;
                    spectrum_colors[i * 3 + 1] = G;
                    spectrum_colors[i * 3 + 2] = B;
                    if (R > max_v) max_v = R;
                    if (G > max_v) max_v = G;
                    if (B > max_v) max_v = B;
                    delete[] stft_results[i];
                }
                /* Second pass: normalize each window by the
                 * frame max, and carry the previous valid color
                 * forward into the unfilled nudge slot when the
                 * frame divided evenly (R=G=B=0 in that slot). */
                double last_r = 0.0, last_g = 0.0, last_b = 0.0;
                for (unsigned int i = 0; i <= n_windows; i++) {
                    double R = (max_v > 0.0) ? spectrum_colors[i*3+0] / max_v : 0.0;
                    double G = (max_v > 0.0) ? spectrum_colors[i*3+1] / max_v : 0.0;
                    double B = (max_v > 0.0) ? spectrum_colors[i*3+2] / max_v : 0.0;
                    if (R + G + B > 0.01) {
                        last_r = R; last_g = G; last_b = B;
                    }
                    spectrum_colors[i * 3 + 0] = last_r;
                    spectrum_colors[i * 3 + 1] = last_g;
                    spectrum_colors[i * 3 + 2] = last_b;
                }
            }
            delete[] stft_results;
    }

    /* Compute color delta accumulator for ColorDeltaMode */
    if (prefs.color_mode == ColorDeltaMode) {
        double olc = 0.0, orc = 0.0;
        for (unsigned int i = 0; i < frames_read; i++) {
            double lc = framebuf[i].left_channel;
            double rc = framebuf[i].right_channel;
            dt += hypot(lc - olc, rc - orc) / SQRT_TWO;
            olc = lc;
            orc = rc;
        }
    }

    /* Particles: depth test rejects overlapping fragments before
     * they reach the ROP — Hi-Z early rejection.  Alpha blend
     * gives soft edges for the one fragment that survives.
     * Lines: additive blending for glowy accumulation. */
    if (prefs.particles) {
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else if (prefs.velocity_dim > 0.0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    }

    /* GPU spline path: upload raw samples as textures, vertex
     * shader does Catmull-Rom.  At spline_steps=1 the shader
     * evaluates at t=0 per vertex, which degenerates to the raw
     * sample positions — unifies the code path so brightness
     * is consistent across all spline counts.  Falls back to CPU
     * only if the shader didn't compile. */
    bool use_gpu_spline = (spline_shader_prog != 0
                           && frames_read > 4
                           && p_glBindBuffer_ && p_glBufferData_);

    if (use_gpu_spline) {
        /* Compute per-sample colors on CPU (~1600 iterations) */
        static float *s_pos = NULL;
        static float *s_col = NULL;
        static unsigned int s_samp_alloc = 0;
        if (frames_read > s_samp_alloc) {
            free(s_pos); free(s_col);
            s_pos = (float *)malloc(frames_read * 4 * sizeof(float));
            s_col = (float *)malloc(frames_read * 4 * sizeof(float));
            s_samp_alloc = frames_read;
        }

        unsigned int spl_stride = (window_size > overlap_size) ? (window_size - overlap_size) : 1;
        double h = -1.0, s = 1.0, v = 1.0, a = 1.0;
        double r = 1.0, g = 1.0, b = 1.0;
        double olc = 0.0, orc = 0.0;
        if (prefs.display_mode == DisplayStandardMode)
            HSVtoRGB(&r, &g, &b, prefs.hue, s, v);

        for (unsigned int i = 0; i < frames_read; i++) {
            double lc = framebuf[i].left_channel;
            double rc = framebuf[i].right_channel;
            double d = hypot(lc - olc, rc - orc) / SQRT_TWO;
            if (prefs.velocity_dim > 0.0)
                a = 1.0 / (1.0 + d * 10.0 * prefs.velocity_dim * prefs.scale_factor);
            else
                a = 1.0;

            bool color_set = false;
            switch (prefs.display_mode) {
                case DisplayStandardMode: break;
                case DisplayRadiusMode:
                    h = ((hypot(lc, rc) / SQRT_TWO) * 360.0 * prefs.color_range * prefs.scale_factor) + prefs.hue;
                    break;
                case DisplaySpectrumMode:
                    if (spectrum_colors) {
                        unsigned int w = i / spl_stride;
                        double sr = spectrum_colors[w * 3 + 0];
                        double sg = spectrum_colors[w * 3 + 1];
                        double sb = spectrum_colors[w * 3 + 2];
                        double sh, ss, sv;
                        RGBtoHSV(sr, sg, sb, &sh, &ss, &sv);
                        ss *= 1.25; if (ss > 1.0) ss = 1.0;
                        double v_floor = 0.5 / prefs.brightness;
                        if (v_floor > 0.5) v_floor = 0.5;
                        sv = sv * (1.0 - v_floor) + v_floor;
                        HSVtoRGB(&r, &g, &b, sh, ss, sv);
                        color_set = true;
                    }
                    break;
            }
            if (!color_set) {
                if (h > -1.0 && prefs.display_mode != DisplayStandardMode)
                    h = normalizeHue(h);
                if (h > -1.0)
                    HSVtoRGB(&r, &g, &b, h, s, v);
                else if (prefs.velocity_dim > 0.0)
                    HSVtoRGB(&r, &g, &b, prefs.hue, s, v);
            }

            s_pos[i * 4 + 0] = (float)lc;
            s_pos[i * 4 + 1] = (float)rc;
            s_pos[i * 4 + 2] = 0.0f;
            s_pos[i * 4 + 3] = 0.0f;
            s_col[i * 4 + 0] = (float)(r * prefs.brightness);
            s_col[i * 4 + 1] = (float)(g * prefs.brightness);
            s_col[i * 4 + 2] = (float)(b * prefs.brightness);
            s_col[i * 4 + 3] = (float)a;
            olc = lc; orc = rc;
        }

        /* Double-buffered texture upload: alternate between
         * two texture pairs each frame so this frame's upload
         * doesn't stall waiting for last frame's draw to finish
         * reading the same texture. */
        static unsigned int s_tex_alloc[2] = {0, 0};
        static unsigned int s_frame = 0;
        unsigned int tex = s_frame & 1;
        s_frame++;

        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, spline_pos_tex[tex]);
        if (frames_read > s_tex_alloc[tex]) {
            glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA16F, frames_read, 0, GL_RGBA, GL_FLOAT, s_pos);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_1D, spline_col_tex[tex]);
            glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA16F, frames_read, 0, GL_RGBA, GL_FLOAT, s_col);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            s_tex_alloc[tex] = frames_read;
        } else {
            glTexSubImage1D(GL_TEXTURE_1D, 0, 0, frames_read, GL_RGBA, GL_FLOAT, s_pos);
            p_glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_1D, spline_col_tex[tex]);
            glTexSubImage1D(GL_TEXTURE_1D, 0, 0, frames_read, GL_RGBA, GL_FLOAT, s_col);
        }

        /* Ensure index VBO is large enough.  Standard Catmull-Rom:
         * (frames_read - 3) usable segments, spline_steps verts each,
         * plus 1 for the final endpoint. */
        unsigned int n_spline_verts = (frames_read - 3) * prefs.spline_steps + 1;
        if (n_spline_verts > spline_index_alloc) {
            float *indices = (float *)malloc(n_spline_verts * 2 * sizeof(float));
            for (unsigned int i = 0; i < n_spline_verts; i++) {
                indices[i * 2]     = (float)i;
                indices[i * 2 + 1] = 0.0f;
            }
            if (!spline_index_vbo)
                p_glGenBuffers_(1, &spline_index_vbo);
            p_glBindBuffer_(GL_ARRAY_BUFFER, spline_index_vbo);
            p_glBufferData_(GL_ARRAY_BUFFER, n_spline_verts * 2 * sizeof(float), indices, 0x88E4 /* GL_STATIC_DRAW */);
            p_glBindBuffer_(GL_ARRAY_BUFFER, 0);
            free(indices);
            spline_index_alloc = n_spline_verts;
        }

        /* Draw with spline shader */
        p_glUseProgram(spline_shader_prog);
        p_glUniform1i(spline_loc_positions, 0);
        p_glUniform1i(spline_loc_colors, 1);
        p_glUniform1f(spline_loc_num_samples, (float)frames_read);
        p_glUniform1f(spline_loc_spline_steps, (float)prefs.spline_steps);

        glEnableClientState(GL_VERTEX_ARRAY);
        p_glBindBuffer_(GL_ARRAY_BUFFER, spline_index_vbo);
        glVertexPointer(2, GL_FLOAT, 0, 0);
        glDrawArrays(prefs.particles ? GL_POINTS : GL_LINE_STRIP, 0, n_spline_verts);
        p_glBindBuffer_(GL_ARRAY_BUFFER, 0);
        glDisableClientState(GL_VERTEX_ARRAY);

        p_glUseProgram(0);
        p_glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_1D, 0);
        p_glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_1D, 0);

        vertex_count = n_spline_verts;
    } else {
        /* CPU fallback */
        bool gpu_color = (prefs.display_mode == DisplaySpectrumMode
                          && spectrum_shader_prog != 0);
        if (gpu_color) {
            p_glUseProgram(spectrum_shader_prog);
            p_glUniform1f(spectrum_brightness_loc, (float)prefs.brightness);
        }

        vertex_count = draw_xy_vertices(
            framebuf, frames_read,
            prefs.display_mode, prefs.color_mode,
            prefs.hue, prefs.color_range, prefs.scale_factor,
            prefs.spline_steps,
            window_size, overlap_size,
            prefs.brightness, prefs.velocity_dim,
            spectrum_colors,
            prefs.particles,
            gpu_color);

        if (gpu_color)
            p_glUseProgram(0);
    }

    if (prefs.particles) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
    }
    else if (prefs.velocity_dim > 0.0)
        glDisable(GL_BLEND);
    glPopMatrix();
    if (prefs.display_mode == DisplaySpectrumMode)
        delete[] spectrum_colors;

    switch (prefs.color_mode) {
        case ColorStandardMode:
            prefs.hue -= prefs.color_rate;
            break;
        case ColorDeltaMode:
            if (color_threshold > 0.0 && dt > color_threshold) {
                color_delta = dt / color_threshold - 1.0;
                /* smooth(&color_threshold, dt, 0.1); */
            }
            else {
                color_delta = 0.0;
            }
            color_threshold = dt;
            prefs.hue -= prefs.color_rate * color_delta;
            break;
        default:
            break;
    }

    prefs.hue = normalizeHue(prefs.hue);

    if (! prefs.auto_scale) {
        for (int i = 0; i < 4; i++)
             smooth(&prefs.side[i], target_side[i], 0.1);
    }
    prefs.scale_factor = 2.0 / min(prefs.side[0] - prefs.side[1],
                                   prefs.side[2] - prefs.side[3]);
}


void scene::autoScale()
{
    double lc = 0.0;
    double rc = 0.0;
    double mv = 0.0;
    double mt = 0.0;
    for (unsigned int i = 0; i < frames_read; i++) {
        lc = fabs(framebuf[i].left_channel);
        rc = fabs(framebuf[i].right_channel);
        mt = max(lc, rc);
        mv = max(mv, mt);
    }
    if (mv > max_sample_value)
        max_sample_value = mv;
    else if (mv < max_sample_value * (1.0 / 3.0))
        smooth(&max_sample_value,
                (max_sample_value * (2.0 / 3.0) + mv),
                0.2);
    setSides(max_sample_value / 0.95, 1);
}


#endif /* XYSCOPE_SCENE_DRAW_H */
