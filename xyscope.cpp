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
#include <GL/glut.h>
#include <GL/gl.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
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
#define DEFAULT_LINE_WIDTH 1

/* Maximum line width setting */
#define MAX_LINE_WIDTH 8

/* Default full screen mode setting */
#define DEFAULT_FULL_SCREEN true

/* Default auto-scale setting */
#define DEFAULT_AUTO_SCALE true

/* Default color mode setting */
#define DEFAULT_COLOR_MODE ColorDeltaMode

/* Default color range setting used by DisplayLengthMode */
#define DEFAULT_COLOR_RANGE 1.0

/* Default color rate setting */
#define DEFAULT_COLOR_RATE 10.0

/* Default display mode setting */
#define DEFAULT_DISPLAY_MODE DisplayLengthMode

/* Set this to your sample rate */
#define SAMPLE_RATE 44100

/* Set this to your desired Frames Per Second */
#define FRAME_RATE 60

/* ringbuffer size in seconds; expect memory usage to exceed:
 *
 * (SAMPLE_RATE * BUFFER_SECONDS + SAMPLE_RATE / FRAME_RATE) * sizeof (frame_t)
 *
 * e.g. (44100 * 60 + 44100 / 60) * 8 = 21173880 bytes or 20.2MB
 *
 * That being said, the jack ringbuffer will round up to the next
 * power of two, in the above case giving us a 32.0MB ringbuffer.
 */
#define BUFFER_SECONDS 1.0

/* How many times to draw each frame */
#define DRAW_EACH_FRAME 2

/* whether to limit frame rate */
#define RESPONSIBLE_FOR_FRAME_RATE true


/* End of easily configurable settings */


/* This must be at least SAMPLE_RATE / FRAME_RATE to draw every sample */
#define FRAMES_PER_BUF (SAMPLE_RATE / FRAME_RATE) * DRAW_EACH_FRAME

/* Connect the end-points with a line */
#define DRAW_FRAMES (FRAMES_PER_BUF + 1)


/* ringbuffer size in frames */
#define DEFAULT_RB_SIZE (SAMPLE_RATE * BUFFER_SECONDS + FRAMES_PER_BUF)


/* Jack Audio types */
typedef jack_default_audio_sample_t sample_t;

typedef struct _frame_t {
    sample_t left_channel;
    sample_t right_channel;
} frame_t;

typedef struct _thread_data {
    pthread_t thread_id;
    jack_client_t *client;
    jack_port_t **ports;
    sample_t **input_buffer;
    size_t frame_size;
    jack_ringbuffer_t *ringbuffer;
    jack_nframes_t rb_size;
    pthread_mutex_t ringbuffer_lock;
    pthread_cond_t data_ready;
    unsigned int channels;
    volatile bool can_process;
    volatile bool pause_scope;
    volatile bool new_port_available;
    timeval last_new_port;
    timeval last_write;
} jack_thread_data_t;

jack_thread_data_t Thread_Data;


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



/* Jack Audio callback functions */

/* The callback function to tell us new ports are available */
void newPort (jack_port_id_t port_id, int something, void *arg)
{
    jack_thread_data_t *t_data = (jack_thread_data_t *) arg;
    gettimeofday (&t_data->last_new_port, NULL);
    t_data->new_port_available = true;
}

/* The callback function for Jack Audio to provide us with audio data */
int process (jack_nframes_t nframes, void *arg)
{
    jack_thread_data_t *t_data = (jack_thread_data_t *) arg;
    sample_t **input_buffer = t_data->input_buffer;
    frame_t frame;
    unsigned int p;
    size_t i;

    /* Do nothing if the scope is paused or we are not ready. */
    if (t_data->pause_scope || ! t_data->can_process)
        return 0;

    /* start the latency timer */
    gettimeofday (&t_data->last_write, NULL);

    /* get the input buffers from jack */
    for (p = 0; p < t_data->channels; p++)
        t_data->input_buffer[p]
            = (sample_t *) jack_port_get_buffer (t_data->ports[p], nframes);

    /* scene::drawPlot() requires interleaved data.  It is simpler here
     * to just queue interleaved samples to a single ringbuffer. */
    for (i = 0; i < nframes; i++) {
        frame.left_channel  = (sample_t) * (input_buffer[LEFT_PORT]  + i);
        frame.right_channel = (sample_t) * (input_buffer[RIGHT_PORT] + i);
        jack_ringbuffer_write (t_data->ringbuffer,
                               (const char *) &frame,
                               t_data->frame_size);
    }

    if (pthread_mutex_trylock (&t_data->ringbuffer_lock) == 0) {
        /* drawPlot() will wait for this signal before reading from
         * the ringbuffer and displaying the most recent framebuf */
        pthread_cond_signal (&t_data->data_ready);
        pthread_mutex_unlock (&t_data->ringbuffer_lock);
    }

    return 0;
}

void jackShutdown (void *arg)
{
    fprintf (stderr, "JACK shutdown\n");
    abort ();
}



/* The audioInput object: readerThread(), setupPorts(),
 *                        connectPorts(), quitNow() */

class audioInput
{
public:
    pthread_t capture_thread;
    bool quit;

    audioInput ()
    {
        bzero (&Thread_Data, sizeof (Thread_Data));
        pthread_mutex_t ringbuffer_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t data_ready       = PTHREAD_COND_INITIALIZER;
        Thread_Data.ringbuffer_lock     = ringbuffer_lock;
        Thread_Data.data_ready          = data_ready;
        quit = false;
        pthread_create (&capture_thread, NULL, readerThread, (void *) this);
    }
    ~audioInput ()
    {
        jack_thread_data_t *t_data = getThreadData ();
        jack_client_close (t_data->client);
        jack_ringbuffer_free (t_data->ringbuffer);
    }

    static void* readerThread (void* arg)
    {
        audioInput* ai             = (audioInput *) arg;
        jack_thread_data_t *t_data = ai->getThreadData ();
        timeval this_moment;

        t_data->thread_id = ai->capture_thread;

        t_data->client = jack_client_new ("xyscope");
        if (t_data->client == NULL) {
            fprintf (stderr, "JACK server not running?\n");
            exit (1);
        }

        t_data->input_buffer       = NULL;
        t_data->frame_size         = sizeof (frame_t);
        t_data->ringbuffer         = NULL;
        t_data->rb_size            = DEFAULT_RB_SIZE;
        t_data->channels           = 2;
        t_data->can_process        = false;
        t_data->pause_scope        = false;
        t_data->new_port_available = false;
        gettimeofday (&t_data->last_new_port, NULL);
        gettimeofday (&t_data->last_write, NULL);

        jack_set_process_callback (t_data->client, process, t_data);
        jack_set_port_registration_callback (t_data->client, newPort, t_data);
        jack_on_shutdown (t_data->client, jackShutdown, t_data);

        if (jack_activate (t_data->client)) {
            fprintf (stderr, "cannot activate client");
        }
        ai->setupPorts ();

        t_data->can_process = true;  /* process() can start, now */

        while (! ai->quit) {
            if (t_data->new_port_available) {
                gettimeofday (&this_moment, NULL);
                if (timeDiff (t_data->last_new_port, this_moment) > 0.5) {
				    fprintf (stderr, "reconnecting\n");
                    t_data->new_port_available = false;
                    ai->connectPorts ();
                }
            }
            usleep (1000);
        }
        return ai;
    }

    void setupPorts ()
    {
        jack_thread_data_t *t_data = getThreadData ();
        size_t input_buffer_size;

        /* Allocate data structures that depend on the number of ports. */
        t_data->ports = (jack_port_t **) malloc (sizeof (jack_port_t *)
                                                 * t_data->channels);
        input_buffer_size = t_data->channels * sizeof (sample_t *);
        t_data->input_buffer = (sample_t **) malloc (input_buffer_size);
        printf ("requesting %.1fMB (%.1f second) ringbuffer\n",
                (1 / (1024.0 * 1024.0)
                 * ((double) t_data->rb_size * (double) t_data->frame_size)),
                (double) BUFFER_SECONDS);
        t_data->ringbuffer = jack_ringbuffer_create (t_data->frame_size
                                                     * t_data->rb_size);

        /* When JACK is running realtime, jack_activate() will have
         * called mlockall() to lock our pages into memory.  But, we
         * still need to touch any newly allocated pages before
         * process() starts using them.  Otherwise, a page fault could
         * create a delay that would force JACK to shut us down. */
        bzero (t_data->input_buffer, input_buffer_size);
        bzero (t_data->ringbuffer->buf, t_data->ringbuffer->size);
        printf ("initialized %.1fMB (%.1f second) ringbuffer\n",
                (1 / (1024.0 * 1024.0)
                 * (double) t_data->ringbuffer->size),
                 ((double) t_data->ringbuffer->size
                  / ((double) SAMPLE_RATE * (double) t_data->frame_size)));

        for (unsigned int i = 0; i < t_data->channels; i++) {
            char name[64];
            sprintf (name, "in%d", i+1);
            if ((t_data->ports[i] = jack_port_register (t_data->client, name,
                                                        JACK_DEFAULT_AUDIO_TYPE,
                                                        JackPortIsInput,
                                                        0)) == 0) {
                fprintf (stderr, "cannot register input port \"%s\"!\n", name);
                jack_client_close (t_data->client);
                exit (1);
            }
        }
    }

    void connectPorts ()
    {
        jack_thread_data_t *t_data = getThreadData ();
        const char **out_ports     = jack_get_ports (t_data->client,
                                                     NULL,
                                                     NULL,
                                                     0);
        for (int i = 0; out_ports[i]; i++) {
            const char *port_name = out_ports[i];
            jack_port_t *port     = jack_port_by_name (t_data->client,
                                                       port_name);
            int port_flags        = jack_port_flags (port);

            printf ("noticed port: %s\n", port_name);

            int left_connected = jack_port_connected_to(t_data->ports[0],
			                                            port_name);
			if (!left_connected) {
                int left = strstr(port_name, "output_FL") != NULL;
				if (left == 1) {
            		printf ("connecting port %s to input 0\n", port_name);
                	if (jack_connect (t_data->client,
                                  	port_name,
                                  	jack_port_name (t_data->ports[0]))) {
                    	fprintf (stderr, "cannot connect to %s\n", port_name);
                    	jack_client_close (t_data->client);
                	}
				}
			}
            int right_connected = jack_port_connected_to(t_data->ports[1],
			                                             port_name);
			if (!right_connected) {
                int right = strstr(port_name, "output_FR") != NULL;
				if (right == 1) {
            		printf ("connecting port %s to input 1\n", port_name);
                	if (jack_connect (t_data->client,
                                  	port_name,
                                  	jack_port_name (t_data->ports[1]))) {
                    	fprintf (stderr, "cannot connect to %s\n", port_name);
                    	jack_client_close (t_data->client);
                	}
				}
			}
        }
    }

    void quitNow ()
    {
        jack_thread_data_t *t_data = getThreadData ();
        jack_client_close (t_data->client);
        jack_ringbuffer_free (t_data->ringbuffer);
        quit = true;
    }

    /* accessor methods */
    jack_thread_data_t *getThreadData (void)
    {
        return &Thread_Data;
    }
};



/* The scene object */

typedef struct _preferences_t {
    int dim[2];
    int normal_dim[2];
    int old_dim[2];
    int position[2];
    double side[4]; /* t, b, r, l */
    double scale_factor;
    bool scale_locked;
    bool is_full_screen;
    bool auto_scale;
    unsigned int color_mode;
    double color_range;
    double color_rate;
    unsigned int display_mode;
    unsigned int line_width;
    unsigned int show_stats;
    double hue;
} preferences_t;

class scene
{
public:
    audioInput* ai;
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
        show_intro           = false;
        show_help            = false;
        show_mouse           = true;

        bzero (&text_timer, sizeof (text_timer_t) * NUM_TEXT_TIMERS);
        gettimeofday (&show_intro_time, NULL);
        gettimeofday (&last_frame_time, NULL);
        gettimeofday (&reset_frame_time, NULL);
        gettimeofday (&mouse_dirty_time, NULL);

        for (int i = 0; i < 4; i++)
            target_side[i] = prefs.side[i];

        ai = new audioInput ();
    }
    ~scene ()
    {
        int FH;
        if ((FH = open (DEFAULT_PREF_FILE, O_CREAT | O_WRONLY, 00660))) {
            /*
            prefs.position[0] = glutGet (GLUT_WINDOW_X);
            prefs.position[1] = glutGet (GLUT_WINDOW_Y);
            */
            fprintf (stderr, "saving preferences\n");
            write (FH, (void *) &prefs, sizeof (preferences_t));
            close (FH);
        }
        free (framebuf);
    }

    void drawPlot ()
    {
        jack_thread_data_t *t_data = ai->getThreadData ();
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
            distance = bump * frame_size;
            bump     = -DRAW_FRAMES;
        }
        else {
            bytes_ready = jack_ringbuffer_read_space (t_data->ringbuffer);
            if (bytes_ready != bytes_per_buf)
                distance = bytes_ready - bytes_per_buf;
        }
        if (distance != 0)
            jack_ringbuffer_read_advance (t_data->ringbuffer, distance);
        bytes_read = jack_ringbuffer_read (t_data->ringbuffer,
                                           (char *) framebuf,
                                           bytes_per_buf);

        if (! t_data->pause_scope)
            pthread_mutex_unlock (&t_data->ringbuffer_lock);

        frames_read = bytes_read / frame_size;


        /* prescans the framebuf in order to auto-scale */
        if (prefs.auto_scale)
            autoScale ();


        /* set up the OpenGL */
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
            d  = hypot (lc, rc) / ROOT_TWO;
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
                    h = (d * 360.0 * prefs.scale_factor
                         * prefs.color_range) + prefs.hue;
                    break;
                case DisplayLengthMode:
                    h = ((hypot (lc - olc, rc - orc) / ROOT_TWO)
                         * 360.0 * prefs.color_range) + prefs.hue;
                    if (h < prefs.hue) {
                        h = prefs.hue + 360.0 + h;
                        if (h < prefs.hue)
                            h = prefs.hue;
                    }
                    if (h > prefs.hue + 360.0)
                        h = prefs.hue + 360.0;
                    olc = lc, orc = rc;
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
        jack_thread_data_t *t_data = ai->getThreadData ();
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
        jack_thread_data_t *t_data = ai->getThreadData ();
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
        jack_thread_data_t *t_data = ai->getThreadData ();
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
        // jack_thread_data_t *t_data = ai->getThreadData ();
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
        jack_thread_data_t *t_data = ai->getThreadData ();
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
        jack_thread_data_t *t_data = ai->getThreadData ();
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

    glutMainLoop ();

    return 0;
}
