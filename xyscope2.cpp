/*
 *  xyscope.cpp
 *  Copyright (c) 2006-2007 by datapoke <7674597+datapoke@users.noreply.github.com>
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
 * $Id: xyscope.cpp,v 1.175 2007/03/26 17:31:28 datapoke Exp $
 *
 */
#ifdef __APPLE__
#include <GLUT/glut.h>
#include <OpenGL/gl.h>
#else
#include <GL/glut.h>
#include <GL/gl.h>
#endif
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <pipewire/context.h>
#include <pipewire/core.h>
#include <spa/utils/ringbuffer.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Preferences file */
#define DEFAULT_PREF_FILE ".xyscope.pref"

/* Default line width setting */
#define DEFAULT_LINE_WIDTH 2

/* Maximum line width setting */
#define MAX_LINE_WIDTH 8

/* Default full screen mode setting */
#define DEFAULT_FULL_SCREEN false

/* Default auto-scale setting */
#define DEFAULT_AUTO_SCALE false

/* Default color mode setting */
#define DEFAULT_COLOR_MODE ColorStandardMode

/* Default color range setting used by DisplayLengthMode */
#define DEFAULT_COLOR_RANGE 1.0

/* Default color rate setting */
#define DEFAULT_COLOR_RATE 1.0

/* Default display mode setting */
#define DEFAULT_DISPLAY_MODE DisplayStandardMode

/* Set this to your sample rate */
#define SAMPLE_RATE 192000

/* Set this to your desired Frames Per Second */
#define FRAME_RATE 60

/* ringbuffer size in seconds; expect memory usage to exceed:
 *
 * (SAMPLE_RATE * BUFFER_SECONDS + SAMPLE_RATE / FRAME_RATE) * sizeof (frame_t)
 *
 * e.g. (44100 * 60 + 44100 / 60) * 8 = 21173880 bytes or 20.2MB
 *
 * That being said, the ringbuffer will round up to the next
 * power of two, in the above case giving us a 32.0MB ringbuffer.
 */
#define BUFFER_SECONDS 60

/* How many times to draw each frame */
#define DRAW_EACH_FRAME 2

/* Whether or not we are responsible for the frame rate */
#ifdef __APPLE__
#define RESPONSIBLE_FOR_FRAME_RATE false
#else
#define RESPONSIBLE_FOR_FRAME_RATE true
#endif


/* End of easily configurable settings */


/* This must be at least SAMPLE_RATE / FRAME_RATE to draw every sample */
#define FRAMES_PER_BUF (SAMPLE_RATE / FRAME_RATE) * DRAW_EACH_FRAME

/* Connect the end-points with a line */
#define DRAW_FRAMES (FRAMES_PER_BUF + 1)


/* ringbuffer size in frames */
#define DEFAULT_RB_SIZE (SAMPLE_RATE * BUFFER_SECONDS + FRAMES_PER_BUF)

/* Audio types */
typedef float sample_t;
typedef struct pw_core pw_core_t;
typedef struct pw_port pw_port_t;
typedef uint32_t pw_nframes_t;

typedef struct _frame_t {
    sample_t left_channel;
    sample_t right_channel;
} frame_t;

typedef struct _thread_data {
    pthread_t thread_id;
    pw_core_t *core;
    struct pw_stream **ports;
    struct pw_stream *stream;
    struct spa_buffer *input_buffer;
    size_t frame_size;
    struct spa_ringbuffer *ringbuffer;
    pw_nframes_t rb_size;
    pthread_mutex_t ringbuffer_lock;
    pthread_cond_t data_ready;
    unsigned int channels;
    volatile bool can_process;
    volatile bool pause_scope;
    volatile bool new_port_available;
    timeval last_new_port;
    timeval last_write;
} pipewire_thread_data_t;

pipewire_thread_data_t Thread_Data;

static void registry_events_on_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props);

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_events_on_global,
    // ... other events if needed
};


#define ROOT_TWO 1.41421356237309504880

#define LEFT_PORT  0
#define RIGHT_PORT 1

#define TIMED true
#define NOT_TIMED false

#define max(A, B) ((A) > (B) ? (A) : (B))
#define min(A, B) ((A) < (B) ? (A) : (B))
#define sign(A) ((A) < 0.0 ? -1.0 : 1.0)



/* useful functions */

double timeDiff (timeval a, timeval b)
{
    return ((double) (b.tv_sec - a.tv_sec) +
            ((double) (b.tv_usec - a.tv_usec) * .000001));
}



/* pipewire callback functions */

static void registry_events_on_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props)
{
    pipewire_thread_data_t *t_data = (pipewire_thread_data_t *) data;
    const char *media_class;

    if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

        if (media_class && strstr(media_class, "Audio/Sink")) {
            fprintf(stdout, "Found monitor port with id: %u\n", id);
            gettimeofday(&t_data->last_new_port, NULL);
            t_data->new_port_available = true;

            // Connect the stream to the monitor port
            pw_stream_connect(t_data->ports[0],
                            PW_DIRECTION_INPUT,
                            id,
                            (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), NULL, 0);
        }
    }
}

void process(void *data, uint32_t seq, uint64_t time, uint32_t priority, const struct spa_pod *pod)
{
    pipewire_thread_data_t *t_data = (pipewire_thread_data_t *)data;
    struct spa_buffer *buffers;
    struct spa_data *d;
    frame_t frame;
    uint32_t num_buffers, data_size, num_samples, i;
    uint32_t write_index;
    uint32_t write_space;

    /* Do nothing if the scope is paused or we are not ready. */
    if (t_data->pause_scope || !t_data->can_process)
        return;

    /* start the latency timer */
    gettimeofday(&t_data->last_write, NULL);

    /* Get the input buffers from PipeWire */
    buffers = t_data->input_buffer;
    d = &buffers->datas[0];
    num_buffers = 1;
    data_size = d->chunk->size;
    num_samples = data_size / (t_data->channels * sizeof(sample_t));

    /* Write interleaved data to the ringbuffer */
    for (i = 0; i < num_samples; i += t_data->channels) {
        frame.left_channel = *(sample_t *) SPA_MEMBER(d->data, i * sizeof(sample_t), void);
        frame.right_channel = *(sample_t *) SPA_MEMBER(d->data, (i + 1) * sizeof(sample_t), void);

        spa_ringbuffer_get_write_index(t_data->ringbuffer, &write_index);
        write_space = spa_ringbuffer_get_write_index(t_data->ringbuffer, &write_index);
        if (write_space >= t_data->frame_size) {
            spa_ringbuffer_init(t_data->ringbuffer);
            spa_ringbuffer_write_data(t_data->ringbuffer, write_index, t_data->rb_size, (const char *)&frame, t_data->frame_size);
            spa_ringbuffer_write_update(t_data->ringbuffer, t_data->frame_size);
        }
    }

    if (pthread_mutex_trylock(&t_data->ringbuffer_lock) == 0) {
        pthread_cond_signal(&t_data->data_ready);
        pthread_mutex_unlock(&t_data->ringbuffer_lock);
    }
}



class scene
{
public:
    pipewire_thread_data_t *t_data;
    size_t frame_size;
    size_t bytes_per_buf;
    frame_t *framebuf;
    int offset;
    int bump;
    size_t frames_read;

    double mouse[4];
    GLuint textures;

    preferences_t prefs;

    double target_side[4];
    double latency;
    double fps;
    double max_sample_value;
    double top_offset;
    double vertical_increment;
    double color_delta;
    double color_threshold;
    unsigned int frame_count;
    bool window_is_dirty;
    bool mouse_is_dirty;

    bool show_intro;
    bool show_help;
    bool show_mouse;

    #define NUM_TEXT_TIMERS 9
    #define NUM_AUTO_TEXT_TIMERS 6
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
        ColorModeTimer   = 1,
        ColorRangeTimer  = 2,
        ColorRateTimer   = 3,
        DisplayModeTimer = 4,
        LineWidthTimer   = 5,
        /* End of text timers automatically included in stats display */
        PausedTimer      = 6,
        ScaleTimer       = 7,
        CounterTimer     = 8
    } text_timer_handles;
    text_timer_t text_timer[NUM_TEXT_TIMERS];
    timeval show_intro_time;
    timeval last_frame_time;
    timeval reset_frame_time;
    timeval mouse_dirty_time;

    #define NUM_COLOR_MODES 2
    char color_mode_names[NUM_COLOR_MODES][64];
    enum {
        ColorStandardMode = 0,
        ColorDeltaMode    = 1
    } color_mode_handles;

    #define NUM_DISPLAY_MODES 4
    char display_mode_names[NUM_DISPLAY_MODES][64];
    enum {
        DisplayStandardMode = 0,
        DisplayRadiusMode   = 1,
        DisplayLengthMode   = 2,
        DisplayTimeMode     = 3
    } display_mode_handles;

    scene ()
    {
        strcpy (color_mode_names[ColorStandardMode], "Standard");
        strcpy (color_mode_names[ColorDeltaMode], "Delta");
        strcpy (display_mode_names[DisplayStandardMode], "Standard");
        strcpy (display_mode_names[DisplayRadiusMode], "Radius");
        strcpy (display_mode_names[DisplayLengthMode], "Length");
        strcpy (display_mode_names[DisplayTimeMode], "Time");

        frame_size           = sizeof (frame_t);
        bytes_per_buf        = DRAW_FRAMES * frame_size;
        framebuf             = (frame_t *) malloc (bytes_per_buf);
        offset               = -FRAMES_PER_BUF;
        bump                 = -DRAW_FRAMES;
        prefs.dim[0]         = 600;
        prefs.dim[1]         = 600;
        prefs.normal_dim[0]  = 600;
        prefs.normal_dim[1]  = 600;
        prefs.old_dim[0]     = 600;
        prefs.old_dim[1]     = 600;
        prefs.position[0]    = 0;
        prefs.position[1]    = 0;
        prefs.side[0]        =  1.0;
        prefs.side[1]        = -1.0;
        prefs.side[2]        =  1.0;
        prefs.side[3]        = -1.0;
        prefs.scale_factor   = 1.0;
        prefs.scale_locked   = true;
        prefs.is_full_screen = DEFAULT_FULL_SCREEN;
        prefs.auto_scale     = DEFAULT_AUTO_SCALE;
        prefs.color_mode     = DEFAULT_COLOR_MODE;
        prefs.color_range    = DEFAULT_COLOR_RANGE;
        prefs.color_rate     = DEFAULT_COLOR_RATE;
        prefs.display_mode   = DEFAULT_DISPLAY_MODE;
        prefs.line_width     = DEFAULT_LINE_WIDTH;
        prefs.show_stats     = 0;
        prefs.hue            = 0.0;
        latency              = 0.0;
        fps                  = 0.0;
        frame_count          = 0;
        window_is_dirty      = true;
        mouse_is_dirty       = true;
        max_sample_value     = 1.0;
        top_offset           = 0.0;
        vertical_increment   = -40.0;
        color_delta          = 0.0;
        color_threshold      = 0.0;
        show_intro           = true;
        show_help            = false;
        show_mouse           = true;

        bzero (&text_timer, sizeof (text_timer_t) * NUM_TEXT_TIMERS);
        gettimeofday (&show_intro_time, NULL);
        gettimeofday (&last_frame_time, NULL);
        gettimeofday (&reset_frame_time, NULL);
        gettimeofday (&mouse_dirty_time, NULL);

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];
    }
    ~scene ()
    {
        int FH;
        if ((FH = open (DEFAULT_PREF_FILE, O_CREAT | O_WRONLY, 00660))) {
            // prefs.position[0] = glutGet (GLUT_WINDOW_X);
            // prefs.position[1] = glutGet (GLUT_WINDOW_Y);
            fprintf (stderr, "saving preferences\n");
            write (FH, (void *) &prefs, sizeof (preferences_t));
            close (FH);
        }
        free (framebuf);
    }

    void drawPlot ()
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        size_t bytes_ready, bytes_read;
        double h   = -1.0;
        double s   = 1.0;
        double v   = 1.0;
        double r   = 1.0;
        double g   = 1.0;
        double b   = 1.0;
        double lc  = 0.0;
        double rc  = 0.0;
        double olc = 0.0;
        double orc = 0.0;
        double d   = 0.0;
        double dt  = 0.0;
        signed int distance = 0;
        /* if the scope is paused, there are no samples available;
         * therefore we should not wait for the reader thread */
        if (! t_data->pause_scope) {
            pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
            pthread_mutex_lock (&t_data->ringbuffer_lock);
            pthread_cond_wait (&t_data->data_ready, &t_data->ringbuffer_lock);
        }

        /* Read data from the ring buffer */
        if (t_data->pause_scope) {
            distance = bump * t_data->frame_size;
            bump     = -DRAW_FRAMES;
        }
        else {
            uint32_t idx;
            spa_ringbuffer_get_read_index(t_data->ringbuffer, &idx);
            bytes_ready = spa_ringbuffer_get_available(t_data->ringbuffer, idx);
            if (bytes_ready != bytes_per_buf)
                distance = bytes_ready - bytes_per_buf;
        }
        if (distance != 0)
            spa_ringbuffer_read_update(t_data->ringbuffer, idx + distance);
        bytes_read = spa_ringbuffer_read_data(t_data->ringbuffer,
                                            idx + distance,
                                            (char *)framebuf,
                                            bytes_per_buf);

        if (! t_data->pause_scope)
            pthread_mutex_unlock (&t_data->ringbuffer_lock);

        frames_read = bytes_read / frame_size;


        /* prescans the framebuf in order to auto-scale */
        if (prefs.auto_scale)
            autoScale ();


        /* set up OpenGL */
        glMatrixMode (GL_PROJECTION);
        glLoadIdentity ();
        glOrtho (prefs.side[3], prefs.side[2],
                 prefs.side[1], prefs.side[0],
                 -10.0, 10.0);
        glMatrixMode (GL_MODELVIEW);
        glPushMatrix ();
        glLoadIdentity ();
        glLineWidth ((GLfloat) prefs.line_width);
        glBegin (GL_LINE_STRIP);

        switch (prefs.display_mode) {
            case DisplayStandardMode:
                HSVtoRGB (&r, &g, &b, prefs.hue, s, v);
                glColor3d (r, g, b);
                break;
            case DisplayRadiusMode:
                break;
            case DisplayLengthMode:
                break;
            case DisplayTimeMode:
                break;
            default:
                break;
        };


        /* display framebuf contents */
        for (unsigned int i = 0; i < frames_read; i++) {
            lc = framebuf[i].left_channel;
            rc = framebuf[i].right_channel;
            d  = hypot (lc - olc, rc - orc) / ROOT_TWO;
            switch (prefs.color_mode) {
                case ColorStandardMode:
                    break;
                case ColorDeltaMode:
                    dt += d;
                    break;
            }
            switch (prefs.display_mode) {
                case DisplayStandardMode:
                    break;
                case DisplayRadiusMode:
                    h = ((hypot (lc, rc) / ROOT_TWO)
                         * 360.0 * prefs.color_range
                         * prefs.scale_factor) + prefs.hue;
                    break;
                case DisplayLengthMode:
                    h = (d * 360.0 * prefs.color_range) + prefs.hue;
                    if (h < prefs.hue) {
                        h = prefs.hue + 360.0 + h;
                        if (h < prefs.hue)
                            h = prefs.hue;
                    }
                    if (h > prefs.hue + 360.0)
                        h = prefs.hue + 360.0;
                    break;
                case DisplayTimeMode:
                    h = (((double) i / (double) frames_read)
                         * 90.0 * prefs.color_range) + prefs.hue;
                    break;
                default:
                    break;
            };
            switch (prefs.display_mode) {
                case DisplayStandardMode:
                    break;
                case DisplayRadiusMode:
                case DisplayLengthMode:
                case DisplayTimeMode:
                    if (h > 360.0)
                        h = (double) (((int) h) % 360);
                    if (h < 0.0)
                        h = 360.0 - (double) (((int) h) % 360);
                    break;
                default:
                    break;
            }
            if (h > -1.0) {
                HSVtoRGB (&r, &g, &b, h, s, v);
                glColor3d (r, g, b);
            }
            glVertex2d (lc, rc);
            olc = lc, orc = rc;
        }
        glEnd ();
        glPopMatrix ();

        switch (prefs.color_mode) {
            case ColorStandardMode:
                prefs.hue -= prefs.color_rate;
                break;
            case ColorDeltaMode:
                if (color_threshold > 0.0 && dt > color_threshold) {
                    color_delta = dt / color_threshold - 1.0;
                    /* smooth (&color_threshold, dt, 0.1); */
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

        if (prefs.hue > 360.0)
            prefs.hue = (double) (((int) prefs.hue) % 360);
        if (prefs.hue < 0.0)
            prefs.hue = 360.0 - (double) (((int) prefs.hue) % 360);

        if (! prefs.auto_scale) {
            for (int i = 0; i < 4; i++)
                 smooth (&prefs.side[i], target_side[i], 0.1);
        }
        prefs.scale_factor = 2.0 / min (prefs.side[0] - prefs.side[1],
                                        prefs.side[2] - prefs.side[3]);
    }

    void beginText ()
    {
        top_offset = -72.0;
        if (text_timer[ScaleTimer].show)
            top_offset = -100.0;

        glDisable (GL_LIGHTING);
        glMatrixMode (GL_MODELVIEW);
        glPushMatrix ();
        glLoadIdentity ();
        glMatrixMode (GL_PROJECTION);
        glPushMatrix ();
        glLoadIdentity ();

        glOrtho (-1.0, 1.0, -1.0, 1.0, -1000.0, 1000.0);
        glColor3d (0.75, 0.75, 0.75);
    }

    void drawString (double x, double y, char *string)
    {
        int len, i;

        if (x >= 0.0)
            x = -1.0 + x / (double) prefs.dim[0];
        else
            x =  1.0 + x / (double) prefs.dim[0];

        if (y >= 0.0)
            y = -1.0 + y / (double) prefs.dim[1];
        else
            y =  1.0 + y / (double) prefs.dim[1];

        glRasterPos2d (x, y);

        len = (int) strlen (string);
        for (i = 0; i < len; i++) {
            glutBitmapCharacter (GLUT_BITMAP_HELVETICA_18, string[i]);
        }
    }

    void endText ()
    {
        glPopMatrix ();
        glMatrixMode (GL_MODELVIEW);
        glPopMatrix ();
    }

    void drawHelp ()
    {
        double left_offset   =  40.0;
        double right_offset  = 400.0;
        unsigned int n_items =  17;

        char help[][2][64] = {
        { "Escape",            "Quit" },
        { "F1 thru F5",        "Quickly resize window" },
        { "Home and Page Up",  "Zoom in" },
        { "End and Page Down", "Zoom out" },
        { "0 thru 9",          "Set zoom factor" },
        { "Spacebar",          "Pause/Resume" },
        { "< and >",           "Rewind/Fast-Forward when paused" },
        { "[ and ]",           "Adjust color range" },
        { "- and +",           "Adjust color rate" },
        { "a",                 "Auto-scale on/off" },
        { "c and C",           "Color mode" },
        { "d and D",           "Display mode" },
        { "f",                 "Enter/Exit full screen mode" },
        { "h",                 "Show/Hide help" },
        { "r",                 "Recenter" },
        { "s and S",           "Show/Hide statistics" },
        { "w and W",           "Adjust line width" }
        };

        for (unsigned int i = 0; i < n_items; i++) {
            drawString (left_offset,  top_offset, help[i][0]);
            drawString (right_offset, top_offset, help[i][1]);
            top_offset += vertical_increment;
        }
        top_offset -= 20.0;
    }

    void drawTimedText ()
    {
        timeval this_frame_time;
        double elapsed_time;
        double x = 40.0;
        gettimeofday (&this_frame_time, NULL);
        for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
            if (text_timer[i].show) {
                /* get the time so we can calculate how long to display */
                elapsed_time = timeDiff (text_timer[i].time,
                                         this_frame_time);
                if (elapsed_time > 10.0)
                    text_timer[i].show = false;

                if (text_timer[i].auto_position) {
                    drawString (x, top_offset, text_timer[i].string);
                    top_offset += vertical_increment;
                }
                else {
                    drawString (text_timer[i].x_position,
                                text_timer[i].y_position,
                                text_timer[i].string);
                }
            }
        }
    }

    void drawStats ()
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        timeval this_frame_time;
        double elapsed_time;
        /* char color_threshold_string[64]; */
        char fps_string[64];
        char time_string[64];

        if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3)) {
            /*
            if (prefs.color_mode == ColorDeltaMode) {
                sprintf (color_threshold_string, "%.5f", color_threshold);
                drawString (-180.0, -40.0, color_threshold_string);
            }
             */

            /* calculate framerate */
            gettimeofday (&this_frame_time, NULL);
            elapsed_time = timeDiff (reset_frame_time, this_frame_time);
            frame_count++;
            if (elapsed_time >= 1.0) {
                fps = frame_count / elapsed_time;
                reset_frame_time = this_frame_time;
                frame_count = 0;
            }
            last_frame_time = this_frame_time;
            sprintf (fps_string, "%.1f fps", fps);
            drawString (20.0, 20.0, fps_string);
        }

        /* calculate latency */
        gettimeofday (&this_frame_time, NULL);
        elapsed_time = timeDiff (t_data->last_write, this_frame_time);
        if (elapsed_time > latency)
            latency = elapsed_time;
        else
            smooth (&latency, elapsed_time, 0.01);
        if (latency < 0.0)
            latency = 0.0;
        if (! t_data->pause_scope) {
            sprintf (time_string, "%7.0f usec", latency * 100000.0);
            drawString (-210.0, 20.0, time_string);
        }
    }

    void drawText (void)
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        timeval this_frame_time;
        double elapsed_time;
        bool show_timer = false;

        /* get the time so we can calculate how long to display */
        gettimeofday (&this_frame_time, NULL);
        elapsed_time = timeDiff (show_intro_time, this_frame_time);
        if (elapsed_time > 10.0)
            show_intro = false;

        if (show_intro || prefs.show_stats == 1) {
            for (unsigned int i = 0; i < NUM_AUTO_TEXT_TIMERS; i++)
                text_timer[i].show = true;
        }
        if (show_intro || (prefs.show_stats > 0 && prefs.show_stats < 3))
            text_timer[ScaleTimer].show = true;
        if (text_timer[ScaleTimer].show)
            sprintf (text_timer[ScaleTimer].string, "%.5f", prefs.scale_factor);
        if (t_data->pause_scope) {
            if (prefs.show_stats > 0 && prefs.show_stats < 4)
                text_timer[CounterTimer].show = true;
            if (text_timer[CounterTimer].show)
                sprintf (text_timer[CounterTimer].string, "%7.2f sec",
                         (double) offset / (double) SAMPLE_RATE
                         + (double) FRAMES_PER_BUF / (double) SAMPLE_RATE);
        }

        for (unsigned int i = 0; i < NUM_TEXT_TIMERS; i++) {
            if (text_timer[i].show)
                show_timer = true;
        }
        if (show_intro || show_help || show_timer || prefs.show_stats) {
            beginText ();
            if (show_intro || show_help)
                drawHelp ();
            if (show_timer)
                drawTimedText ();
            if (show_intro || prefs.show_stats)
                drawStats ();
            endText ();
        }
    }

    void showAutoScale (bool timed)
    {
        text_timer_t *timer  = &text_timer[AutoScaleTimer];
        timer->auto_position = true;
        sprintf (timer->string,
                 "Auto-scale: %s", prefs.auto_scale ? "on" : "off");
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showLineWidth (bool timed)
    {
        text_timer_t *timer  = &text_timer[LineWidthTimer];
        timer->auto_position = true;
        sprintf (timer->string,
                 "Line width: %d", prefs.line_width);
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showColorMode (bool timed)
    {
        text_timer_t *timer  = &text_timer[ColorModeTimer];
        timer->auto_position = true;
        sprintf (timer->string, "Color mode: %s",
                 color_mode_names[prefs.color_mode]);
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showDisplayMode (bool timed)
    {
        text_timer_t *timer  = &text_timer[DisplayModeTimer];
        timer->auto_position = true;
        sprintf (timer->string, "Display mode: %s",
                 display_mode_names[prefs.display_mode]);
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showColorRange (bool timed)
    {
        text_timer_t *timer  = &text_timer[ColorRangeTimer];
        timer->auto_position = true;
        sprintf (timer->string, "Color range: %.2f",
                 prefs.color_range);
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showColorRate (bool timed)
    {
        text_timer_t *timer  = &text_timer[ColorRateTimer];
        timer->auto_position = true;
        sprintf (timer->string, "Color rate: %.2f",
                 prefs.color_rate);
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showPaused (bool timed)
    {
        text_timer_t *timer  = &text_timer[PausedTimer];
        timer->auto_position = true;
        strcpy (timer->string, "Paused");
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showScale (bool timed)
    {
        text_timer_t *timer  = &text_timer[ScaleTimer];
        timer->auto_position = false;
        timer->x_position    =  20.0;
        timer->y_position    = -40.0;
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showCounter (bool timed)
    {
        text_timer_t *timer  = &text_timer[CounterTimer];
        timer->auto_position = false;
        timer->x_position    = -210.0;
        timer->y_position    =   20.0;
        if (timed)
            gettimeofday (&timer->time, NULL);
        timer->show = true;
    }

    void showMouse ()
    {
        gettimeofday (&mouse_dirty_time, NULL);
        show_mouse     = true;
        mouse_is_dirty = true;
    }

    void autoScale ()
    {
        double lc = 0.0;
        double rc = 0.0;
        double mv = 0.0;
        double mt = 0.0;
        for (unsigned int i = 0; i < frames_read; i++) {
            lc = fabs (framebuf[i].left_channel);
            rc = fabs (framebuf[i].right_channel);
            mt = max (lc, rc);
            mv = max (mv, mt);
        }
        if (mv > max_sample_value)
            max_sample_value = mv;
        else if (mv < max_sample_value * (1.0 / 3.0))
            smooth (&max_sample_value,
                    (max_sample_value * (2.0 / 3.0) + mv),
                    0.2);
        setSides (max_sample_value, 1);
    }

    void zoomIn (void)
    {
        scale (1.1);
    }

    void zoomOut (void)
    {
        scale (1 / 1.1);
    }

    void rescale (void)
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

    void scale (double factor)
    {
        double width        = target_side[0] - target_side[1];
        double height       = target_side[2] - target_side[3];
        double add_distance = min (width, height) * (1.0 - factor);
        double r            = ((double) prefs.dim[0]
                               / (double) prefs.dim[1]);
        double shortest     = 0.0;
        double longest      = 0.0;
        double t_side[4];
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale (TIMED);
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
        shortest = min (width, height);
        longest  = max (width, height);
        if (shortest > 0.00001 && longest < 10000.0) {
            for (int i = 0; i < 4; i++)
                target_side[i] = t_side[i];
        }
        showScale (TIMED);
    }

    void move (int ax, double x)
    {
        int s1    = ax * 2;
        int s2    = s1 + 1;
        double w  = target_side[s2] - target_side[s1];
        double dx = x * w;
        prefs.scale_locked = false;
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale (TIMED);
        }
        target_side[s1] += dx;
        target_side[s2] += dx;
    }

    void toggleFullScreen (void)
    {
        if (prefs.is_full_screen)
            setWindowSize (prefs.normal_dim[0], prefs.normal_dim[1]);
        else
            setFullScreen ();
    }

    void toggleAutoScale (void)
    {
        max_sample_value   = min ((prefs.side[0] - prefs.side[1]) / 2.1,
                                  (prefs.side[2] - prefs.side[3]) / 2.1);
        prefs.auto_scale   = ! prefs.auto_scale;
        showAutoScale (TIMED);
    }

    void togglePaused (void)
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        if (t_data->pause_scope) {
            latency = 0.0;
            text_timer[CounterTimer].show = false;
            text_timer[PausedTimer].show  = false;
        }
        else {
            offset = -FRAMES_PER_BUF;
            bump   = -DRAW_FRAMES;
            showCounter (TIMED);
            showPaused (TIMED);
        }
        t_data->pause_scope = ! t_data->pause_scope;
        gettimeofday (&t_data->last_write, NULL);
    }

    void recenter (void)
    {
        target_side[0] =  (prefs.side[0] - prefs.side[1]) / 2.0;
        target_side[1] = -(prefs.side[0] - prefs.side[1]) / 2.0;
        target_side[2] =  (prefs.side[2] - prefs.side[3]) / 2.0;
        target_side[3] = -(prefs.side[2] - prefs.side[3]) / 2.0;
    }

    void nextColorMode (void)
    {
        prefs.color_mode = (prefs.color_mode + 1) % NUM_COLOR_MODES;
        showColorMode (TIMED);
    }

    void prevColorMode (void)
    {
        if (prefs.color_mode < 1)
            prefs.color_mode = NUM_COLOR_MODES - 1;
        else
            prefs.color_mode = prefs.color_mode - 1;
        showColorMode (TIMED);
    }

    void nextDisplayMode (void)
    {
        prefs.display_mode = (prefs.display_mode + 1) % NUM_DISPLAY_MODES;
        showDisplayMode (TIMED);
    }

    void prevDisplayMode (void)
    {
        if (prefs.display_mode < 1)
            prefs.display_mode = NUM_DISPLAY_MODES - 1;
        else
            prefs.display_mode = prefs.display_mode - 1;
        showDisplayMode (TIMED);
    }

    void nextStatsGroup (void)
    {
        // pipewire_thread_data_t *t_data = &scn.t_data;
        // gettimeofday (&t_data->last_write, NULL);
        // latency = 0.0;
        prefs.show_stats++;
        if (prefs.show_stats > 3)
            prefs.show_stats = 0;
    }

    void prevStatsGroup (void)
    {
        if (prefs.show_stats < 1)
            prefs.show_stats = 3;
        else
            prefs.show_stats--;
    }

    void rewind (int nbufs)
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        if (t_data->pause_scope) {
            if ((offset - FRAMES_PER_BUF * nbufs) >= -DEFAULT_RB_SIZE) {
                offset -= FRAMES_PER_BUF * nbufs;
                bump   -= FRAMES_PER_BUF * nbufs;
            }
            showCounter (TIMED);
        }
    }

    void fastForward (int nbufs)
    {
        pipewire_thread_data_t *t_data = &scn.t_data;
        if (t_data->pause_scope) {
            if (offset < -FRAMES_PER_BUF * nbufs) {
                offset += FRAMES_PER_BUF * nbufs;
                bump   += FRAMES_PER_BUF * nbufs;
            }
            showCounter (TIMED);
        }
    }

    /* accessor methods */

    void setWindowSize (unsigned int x, unsigned int y)
    {
        if (! prefs.is_full_screen) {
            prefs.position[0] = glutGet (GLUT_WINDOW_X);
            prefs.position[1] = glutGet (GLUT_WINDOW_Y);
        }
        glutPositionWindow (prefs.position[0], prefs.position[1]);
        glutReshapeWindow (x, y);
        prefs.is_full_screen = false;
    }

    void setFullScreen (void)
    {
        if (! prefs.is_full_screen) {
            prefs.position[0]   = glutGet (GLUT_WINDOW_X);
            prefs.position[1]   = glutGet (GLUT_WINDOW_Y);
            prefs.normal_dim[0] = glutGet (GLUT_WINDOW_WIDTH);
            prefs.normal_dim[1] = glutGet (GLUT_WINDOW_HEIGHT);
        }
        glutPositionWindow (0, 0);
        glutFullScreen ();
        prefs.is_full_screen = true;
        show_mouse           = false;
        mouse_is_dirty       = true;
    }

    void setZoom (double factor)
    {
        if (prefs.auto_scale) {
            prefs.auto_scale = false;
            showAutoScale (TIMED);
        }
        showScale (TIMED);
        setSides (1.0 / factor, 0);
    }

    void setSides (double x, int no_smooth)
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

    double getColorRange (void)
    {
        return prefs.color_range;
    }

    double getColorRate (void)
    {
        return prefs.color_rate;
    }

    void setColorRange (double range)
    {
        prefs.color_range = range;
        if (prefs.color_range >  100.0)
            prefs.color_range -= 200.0;
        if (prefs.color_range <= -100.0)
            prefs.color_range += 200.0;
        showColorRange (TIMED);
    }

    void setColorRate (double rate)
    {
        prefs.color_rate = rate;
        if (prefs.color_rate >  180.0)
            prefs.color_rate -= 360.0;
        if (prefs.color_rate <= -180.0)
            prefs.color_rate += 360.0;
        showColorRate (TIMED);
    }

    int getLineWidth (void)
    {
        return prefs.line_width;
    }

    void setLineWidth (int width)
    {
        prefs.line_width = width;
        if (prefs.line_width < 1)
            prefs.line_width = MAX_LINE_WIDTH;
        else if (prefs.line_width > MAX_LINE_WIDTH)
            prefs.line_width = 1;
        showLineWidth (TIMED);
    }


    /* useful functions */

    void smooth (double *a, double b, double s)
    {
        *a = *a + (b - *a) * s;
    }

    void HSVtoRGB (double *r, double *g, double *b,
                   double h, double s, double v)
    {
        int i;
        double f, p, q, t;

        if (s == 0) {
            // achromatic (grey)
            *r = *g = *b = v;
            return;
        }

        if (h >= 360.0)
            h -= 360.0;

        h /= 60;              // sector 0 to 5
        i = (int) floorf (h);
        f = h - i;            // factorial part of h
        p = v * (1 - s);
        q = v * (1 - s * f);
        t = v * (1 - s * (1 - f));

        switch (i) {
            case 0:
                *r = v;
                *g = t;
                *b = p;
                break;
            case 1:
                *r = q;
                *g = v;
                *b = p;
                break;
            case 2:
                *r = p;
                *g = v;
                *b = t;
                break;
            case 3:
                *r = p;
                *g = q;
                *b = v;
                break;
            case 4:
                *r = t;
                *g = p;
                *b = v;
                break;
            default:          // case 5:
                *r = v;
                *g = p;
                *b = q;
                break;
        }
    }
};
static scene scn;

void display ()
{
    glClear (GL_COLOR_BUFFER_BIT);

    /* plot the samples on the screen */
    scn.drawPlot ();

    /* draw any text that needs drawing */
    scn.drawText ();

    /* wash, rinse, repeat */
    glFinish ();
    glutSwapBuffers ();
}

void idle (void)
{
    timeval this_moment;
    double elapsed_time;

    /* restore our window title after coming out of full screen mode */
    if (scn.window_is_dirty) {
        glutSetWindowTitle ("XY Scope");
        glutSetIconTitle ("XY Scope");
        scn.prefs.dim[0] = glutGet (GLUT_WINDOW_WIDTH);
        scn.prefs.dim[1] = glutGet (GLUT_WINDOW_HEIGHT);
        if (scn.prefs.scale_locked)
            scn.setSides (1.0 / scn.prefs.scale_factor, 1);
        else
            scn.rescale ();
        glViewport (0, 0, scn.prefs.dim[0], scn.prefs.dim[1]);
        scn.window_is_dirty = false;
    }

    if (scn.show_mouse) {
        gettimeofday (&this_moment, NULL);
        elapsed_time = timeDiff (scn.mouse_dirty_time, this_moment);
        if (elapsed_time > 10.0) {
            gettimeofday (&scn.mouse_dirty_time, NULL);
            scn.show_mouse     = false;
            scn.mouse_is_dirty = true;
        }
    }
    if (scn.mouse_is_dirty) {
        if (scn.show_mouse)
            glutSetCursor (GLUT_CURSOR_LEFT_ARROW);
        else
            glutSetCursor (GLUT_CURSOR_NONE);
    }

    if (RESPONSIBLE_FOR_FRAME_RATE) {
        /* limit our framerate to FRAME_RATE (e.g. 60) frames per second */
        elapsed_time = timeDiff (scn.reset_frame_time, scn.last_frame_time);
        if (elapsed_time < (scn.frame_count / (double) FRAME_RATE)) {
            double remainder = (scn.frame_count
                                / (double) FRAME_RATE - elapsed_time);
            usleep ((useconds_t)(1000000.0 * remainder));
        }
    }
    glutPostRedisplay ();
}

void special (int key, int xPos, int yPos)
{
    switch (key) {
        case 101:                  /* up arrow */
            scn.move (0, 0.2);
            break;
        case 103:                  /* down arrow */
            scn.move (0, -0.2);
            break;
        case 100:                  /* left arrow */
            scn.move (1, -0.2);
            break;
        case 102:                  /* right arrow */
            scn.move (1, 0.2);
            break;
        case 104:                  /* page up */
            scn.zoomIn ();
            break;
        case 105:                  /* page down */
            scn.zoomOut ();
            break;
        case 106:                  /* home */
            scn.zoomIn ();
            break;
        case 107:                  /* end */
            scn.zoomOut ();
            break;
        case 1:                    /* F1 */
            scn.setWindowSize (300, 300);
            break;
        case 2:                    /* F2 */
            scn.setWindowSize (600, 600);
            break;
        case 3:                    /* F3 */
            scn.setWindowSize (800, 800);
            break;
        case 4:                    /* F4 */
            scn.setWindowSize (1000, 1000);
            break;
        case 5:                    /* F5 */
            scn.toggleFullScreen ();
            break;
        default:
            fprintf (stderr, "pressed special key %d\n", key);
            break;
    }
}

void keyboard (unsigned char key, int xPos, int yPos)
{
    switch (key) {
        case 27:                         /* escape */
            scn.ai->quitNow ();
            exit (0);
        case '0':
            scn.setZoom (pow (2.0, 9.0));
            break;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            /* atof ((const char *) &key); ? */
            scn.setZoom (pow (2.0, key - '1'));
            break;
        case ',':
            scn.rewind (1);
            break;
        case '.':
            scn.fastForward (1);
            break;
        case '<':
            scn.rewind (FRAME_RATE / DRAW_EACH_FRAME);
            break;
        case '>':
            scn.fastForward (FRAME_RATE / DRAW_EACH_FRAME);
            break;
        case '_':
            scn.setColorRate (scn.getColorRate () - 0.01);
            break;
        case '+':
            scn.setColorRate (scn.getColorRate () + 0.01);
            break;
        case '-':
            scn.setColorRate (scn.getColorRate () - 1.0);
            break;
        case '=':
            scn.setColorRate (scn.getColorRate () + 1.0);
            break;
        case '{':
            scn.setColorRange (scn.getColorRange () - 0.01);
            break;
        case '}':
            scn.setColorRange (scn.getColorRange () + 0.01);
            break;
        case '[':
            scn.setColorRange (scn.getColorRange () - 1.0);
            break;
        case ']':
            scn.setColorRange (scn.getColorRange () + 1.0);
            break;
        case ' ':                        /* spacebar */
            scn.togglePaused ();
            break;
        case 'a':
            scn.toggleAutoScale ();
            break;
        case 'c':
            scn.nextColorMode ();
            break;
        case 'C':
            scn.prevColorMode ();
            break;
        case 'd':
            scn.nextDisplayMode ();
            break;
        case 'D':
            scn.prevDisplayMode ();
            break;
        case 'f':
            scn.toggleFullScreen ();
            break;
        case 'h':
            if (scn.show_intro)
                scn.show_intro = false;
            else
                scn.show_help = ! scn.show_help;
            break;
        case 'r':
            scn.recenter ();
            break;
        case 's':
            scn.nextStatsGroup ();
            break;
        case 'S':
            scn.prevStatsGroup ();
            break;
        case 'w':
            scn.setLineWidth (scn.getLineWidth () + 1);
            break;
        case 'W':
            scn.setLineWidth (scn.getLineWidth () - 1);
            break;
        default:
            fprintf (stderr, "pressed key %d\n", (int) key);
            break;
    }
}

void reshape (int w, int h)
{
    scn.window_is_dirty = true;
}

void mouse (int button, int state, int x, int y)
{
    scn.mouse[0] = x;
    scn.mouse[1] = y;
    scn.mouse[2] = button;
    if (button == 3 && state == 1) {
        scn.zoomIn ();
    }
    else if (button == 4 && state == 1) {
        scn.zoomOut ();
    }
    /*
    else {
        printf ("Mouse event: button: %d  state: %d  pos: %d, %d\n",
                button, state, x, y);
    }
     */
    scn.showMouse ();
}

void motion (int x, int y)
{
    int dx = (int) (x - scn.mouse[0]);
    int dy = (int) (y - scn.mouse[1]);
    if (scn.mouse[2] == 0) {
        scn.move (0, - (double) dy / (double) scn.prefs.dim[1]);
        scn.move (1,   (double) dx / (double) scn.prefs.dim[0]);
    }
    else if (scn.mouse[2] == 2) {
        scn.scale (1.0 - dy / 50.0);
    }
    scn.mouse[0] = x;
    scn.mouse[1] = y;
    scn.showMouse ();
}

void passiveMotion (int x, int y)
{
    scn.showMouse ();
}

int main (int argc, char * const argv[])
{
    int FH;
    glutInit (&argc, (char **) argv);
    glutInitDisplayMode (GLUT_DOUBLE | GLUT_RGB);
    if ((FH = open (DEFAULT_PREF_FILE, O_RDONLY))) {
        read (FH, (void *) &scn.prefs, sizeof (preferences_t));
        close (FH);
    }
    glutInitWindowSize (scn.prefs.dim[0], scn.prefs.dim[1]);
    glutInitWindowPosition (scn.prefs.position[0], scn.prefs.position[1]);
    glutCreateWindow ("XY Scope");
    glGenTextures (1, &scn.textures);
    if (scn.prefs.is_full_screen)
        scn.setFullScreen ();

    glutDisplayFunc (display);
    glutReshapeFunc (reshape);
    glutSpecialFunc (special);
    glutKeyboardFunc (keyboard);
    glutMouseFunc (mouse);
    glutMotionFunc (motion);
    glutPassiveMotionFunc (passiveMotion);
    glutIdleFunc (idle);

    scn.showDisplayMode (NOT_TIMED);
    scn.showLineWidth (NOT_TIMED);
    scn.showColorRange (NOT_TIMED);
    scn.showColorRate (NOT_TIMED);
    scn.showColorMode (NOT_TIMED);
    scn.showAutoScale (NOT_TIMED);
    scn.showScale (NOT_TIMED);

    // Initialize PipeWire
    pw_init(NULL, NULL);

    // Create main loop
    struct pw_thread_loop *main_loop = pw_thread_loop_new("main_loop", NULL);

    // Create PipeWire context
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(main_loop), NULL, 0);

    // create a registry listener
    struct spa_hook registry_listener;

    // Start the main loop
    pw_thread_loop_start(main_loop);

    // Create PipeWire core
    pipewire_thread_data_t *t_data = &scn.t_data;
    t_data->core = pw_context_connect(context, NULL, 0);

    if (t_data->core == NULL) {
        fprintf(stderr, "Failed to connect to PipeWire core\n");
        // Handle error and clean up resources
        return 1;
    }

    // Register core events
    pw_core_t *core = pw_context_connect(context, NULL, 0);
    pw_registry_t *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY);
    pw_registry_add_listener(registry, &registry_listener, &registry_events, t_data);

    // Create and configure the audio stream
    struct pw_stream *stream;
    struct pw_stream_events stream_events = {0};

    // Set up stream event callbacks
    stream_events.process = your_process_function; // Replace with your function for processing audio data
    // ... Add other event callbacks as needed

    // Stream properties
    struct pw_properties *stream_props = pw_properties_new(NULL, NULL);
    pw_properties_set(stream_props, PW_KEY_MEDIA_TYPE, "Audio");
    pw_properties_set(stream_props, PW_KEY_MEDIA_CATEGORY, "Capture");
    pw_properties_set(stream_props, PW_KEY_MEDIA_ROLE, "DSP");

    // Create the stream
    stream = pw_stream_new(t_data->core, "xyscope_stream", stream_props);
    if (!stream) {
        fprintf(stderr, "Failed to create PipeWire stream\n");
        // Handle error and clean up resources
    }

    // Register the stream events
    struct spa_hook stream_listener;
    pw_stream_add_listener(stream, &stream_listener, &stream_events, t_data);

    // Connect the stream
    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, NULL, 0) < 0) {
        fprintf(stderr, "Failed to connect the stream\n");
        // Handle error and clean up resources
    }

    // Main application loop
    // The monitor port will be connected in the core_events_on_info function when it becomes available

    // Don't forget to clean up resources when the application is done or needs to exit
    glutMainLoop ();

    return 0;
}
