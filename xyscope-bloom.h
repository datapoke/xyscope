#ifndef XYSCOPE_BLOOM_H
#define XYSCOPE_BLOOM_H

/* Include this header from a translation unit that has already pulled in
 * SDL2/SDL.h (for SDL_GL_GetProcAddress used by Task 2's init path). */

#if defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glext.h>
#else
    #include <GL/gl.h>
    #include <GL/glext.h>
#endif

typedef struct {
    bool enabled;              /* true if init succeeded */
    int  width;                /* current full-res */
    int  height;

    /* GL objects — filled in by bloom_init, zeroed otherwise */
    GLuint scene_fbo;
    GLuint scene_tex;
    GLuint blur_fbo[2];
    GLuint blur_tex[2];
    GLuint blur_prog;
    GLuint composite_prog;
    GLint  blur_loc_tex;
    GLint  blur_loc_dir;
    GLint  comp_loc_scene;
    GLint  comp_loc_bloom;
    GLint  comp_loc_intensity;
} bloom_state_t;

/* Public API — see xyscope-bloom-design.md for contracts. */
static inline bool bloom_init   (bloom_state_t *b, int w, int h) { (void)b; (void)w; (void)h; return false; }
static inline void bloom_resize (bloom_state_t *b, int w, int h) { (void)b; (void)w; (void)h; }
static inline void bloom_begin  (bloom_state_t *b)               { (void)b; }
static inline void bloom_end    (bloom_state_t *b, float intensity) { (void)b; (void)intensity; }
static inline void bloom_cleanup(bloom_state_t *b)               { (void)b; }

#endif /* XYSCOPE_BLOOM_H */
