#ifndef XYSCOPE_BLOOM_H
#define XYSCOPE_BLOOM_H

#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glext.h>
#else
    #include <GL/gl.h>
    #include <GL/glext.h>
#endif

/* APIENTRYP is defined by GL/glext.h on Linux and Windows, but macOS's
 * glext.h does not define it. Fall back to an empty expansion so the
 * typedefs below compile cleanly on every platform. */
#ifndef APIENTRYP
    #ifdef APIENTRY
        #define APIENTRYP APIENTRY *
    #else
        #define APIENTRYP *
    #endif
#endif

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
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
    GLuint gamma_prog;
    GLint  gamma_loc_tex;
    GLint  gamma_loc_gamma;
} bloom_state_t;

/* GL 2.x/3.x function pointers — loaded via SDL_GL_GetProcAddress in bloom_init. */
typedef GLuint (APIENTRYP GLCREATESHADERPROC_)(GLenum);
typedef void   (APIENTRYP GLSHADERSOURCEPROC_)(GLuint, GLsizei, const GLchar * const *, const GLint *);
typedef void   (APIENTRYP GLCOMPILESHADERPROC_)(GLuint);
typedef void   (APIENTRYP GLGETSHADERIVPROC_)(GLuint, GLenum, GLint *);
typedef void   (APIENTRYP GLGETSHADERINFOLOGPROC_)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (APIENTRYP GLCREATEPROGRAMPROC_)(void);
typedef void   (APIENTRYP GLATTACHSHADERPROC_)(GLuint, GLuint);
typedef void   (APIENTRYP GLLINKPROGRAMPROC_)(GLuint);
typedef void   (APIENTRYP GLGETPROGRAMIVPROC_)(GLuint, GLenum, GLint *);
typedef void   (APIENTRYP GLGETPROGRAMINFOLOGPROC_)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (APIENTRYP GLUSEPROGRAMPROC_)(GLuint);
typedef void   (APIENTRYP GLDELETESHADERPROC_)(GLuint);
typedef void   (APIENTRYP GLDELETEPROGRAMPROC_)(GLuint);
typedef GLint  (APIENTRYP GLGETUNIFORMLOCATIONPROC_)(GLuint, const GLchar *);
typedef void   (APIENTRYP GLUNIFORM1IPROC_)(GLint, GLint);
typedef void   (APIENTRYP GLUNIFORM1FPROC_)(GLint, GLfloat);
typedef void   (APIENTRYP GLUNIFORM2FPROC_)(GLint, GLfloat, GLfloat);
typedef void   (APIENTRYP GLACTIVETEXTUREPROC_)(GLenum);

typedef void   (APIENTRYP GLGENFRAMEBUFFERSPROC_)(GLsizei, GLuint *);
typedef void   (APIENTRYP GLDELETEFRAMEBUFFERSPROC_)(GLsizei, const GLuint *);
typedef void   (APIENTRYP GLBINDFRAMEBUFFERPROC_)(GLenum, GLuint);
typedef void   (APIENTRYP GLFRAMEBUFFERTEXTURE2DPROC_)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRYP GLCHECKFRAMEBUFFERSTATUSPROC_)(GLenum);
typedef void   (APIENTRYP GLBLITFRAMEBUFFERPROC_)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

static GLCREATESHADERPROC_           p_glCreateShader;
static GLSHADERSOURCEPROC_           p_glShaderSource;
static GLCOMPILESHADERPROC_          p_glCompileShader;
static GLGETSHADERIVPROC_            p_glGetShaderiv;
static GLGETSHADERINFOLOGPROC_       p_glGetShaderInfoLog;
static GLCREATEPROGRAMPROC_          p_glCreateProgram;
static GLATTACHSHADERPROC_           p_glAttachShader;
static GLLINKPROGRAMPROC_            p_glLinkProgram;
static GLGETPROGRAMIVPROC_           p_glGetProgramiv;
static GLGETPROGRAMINFOLOGPROC_      p_glGetProgramInfoLog;
static GLUSEPROGRAMPROC_             p_glUseProgram;
static GLDELETESHADERPROC_           p_glDeleteShader;
static GLDELETEPROGRAMPROC_          p_glDeleteProgram;
static GLGETUNIFORMLOCATIONPROC_     p_glGetUniformLocation;
static GLUNIFORM1IPROC_              p_glUniform1i;
static GLUNIFORM1FPROC_              p_glUniform1f;
static GLUNIFORM2FPROC_              p_glUniform2f;
static GLACTIVETEXTUREPROC_          p_glActiveTexture;

static GLGENFRAMEBUFFERSPROC_        p_glGenFramebuffers;
static GLDELETEFRAMEBUFFERSPROC_     p_glDeleteFramebuffers;
static GLBINDFRAMEBUFFERPROC_        p_glBindFramebuffer;
static GLFRAMEBUFFERTEXTURE2DPROC_   p_glFramebufferTexture2D;
static GLCHECKFRAMEBUFFERSTATUSPROC_ p_glCheckFramebufferStatus;
static GLBLITFRAMEBUFFERPROC_        p_glBlitFramebuffer;

/* On Windows the HDR path creates its GL context via wglCreateContext and
 * wraps the foreign HWND through SDL_CreateWindowFrom. Because SDL never
 * owned that context, SDL_GL_GetProcAddress cannot load any GL 2.0+ procs
 * (its internal gl_data is NULL and the opengl32.dll fallback only exports
 * GL 1.1 core). Use wglGetProcAddress directly — it follows the currently-
 * bound WGL context and returns the right entry points regardless of which
 * code path created it. This mirrors how the existing glClampColorARB
 * loader works in xyscope.mm. */
#ifdef _WIN32
#define BLOOM_GET_PROC(name) wglGetProcAddress(name)
#else
#define BLOOM_GET_PROC(name) SDL_GL_GetProcAddress(name)
#endif

static inline bool bloom_load_procs(void)
{
    #define LOAD(name) do { \
        p_##name = (decltype(p_##name))BLOOM_GET_PROC(#name); \
        if (!p_##name) { fprintf(stderr, "bloom: missing GL proc %s\n", #name); return false; } \
    } while (0)

    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader);
    LOAD(glGetShaderiv); LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram); LOAD(glDeleteShader); LOAD(glDeleteProgram);
    LOAD(glGetUniformLocation); LOAD(glUniform1i); LOAD(glUniform1f); LOAD(glUniform2f);
    LOAD(glActiveTexture);
    LOAD(glGenFramebuffers); LOAD(glDeleteFramebuffers); LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D); LOAD(glCheckFramebufferStatus);
    LOAD(glBlitFramebuffer);
    #undef LOAD
    return true;
}

static const char *BLOOM_VS_SRC =
    "#version 120\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = gl_Vertex;\n"
    "    v_uv = gl_MultiTexCoord0.xy;\n"
    "}\n";

static const char *BLOOM_BLUR_FS_SRC =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_direction;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 s = texture2D(u_tex, v_uv) * 0.227027;\n"
    "    s += texture2D(u_tex, v_uv + u_direction * 1.0) * 0.1945946;\n"
    "    s += texture2D(u_tex, v_uv - u_direction * 1.0) * 0.1945946;\n"
    "    s += texture2D(u_tex, v_uv + u_direction * 2.0) * 0.1216216;\n"
    "    s += texture2D(u_tex, v_uv - u_direction * 2.0) * 0.1216216;\n"
    "    s += texture2D(u_tex, v_uv + u_direction * 3.0) * 0.054054;\n"
    "    s += texture2D(u_tex, v_uv - u_direction * 3.0) * 0.054054;\n"
    "    s += texture2D(u_tex, v_uv + u_direction * 4.0) * 0.016216;\n"
    "    s += texture2D(u_tex, v_uv - u_direction * 4.0) * 0.016216;\n"
    "    gl_FragColor = s;\n"
    "}\n";

static const char *BLOOM_GAMMA_FS_SRC =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_gamma;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    "    gl_FragColor = pow(max(c, vec4(0.0)), vec4(u_gamma));\n"
    "}\n";

static const char *BLOOM_COMP_FS_SRC =
    "#version 120\n"
    "uniform sampler2D u_scene;\n"
    "uniform sampler2D u_bloom;\n"
    "uniform float u_intensity;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 s = texture2D(u_scene, v_uv);\n"
    "    vec4 b = texture2D(u_bloom, v_uv);\n"
    "    gl_FragColor = max(s, b * u_intensity);\n"
    "}\n";

static inline GLuint bloom_compile_shader(GLenum type, const char *src)
{
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        p_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "bloom: shader compile failed: %s\n", log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static inline GLuint bloom_build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = bloom_compile_shader(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;
    GLuint fs = bloom_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { p_glDeleteShader(vs); return 0; }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    p_glLinkProgram(prog);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        p_glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "bloom: shader link failed: %s\n", log);
        p_glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static inline bool bloom_make_fbo(GLuint *fbo_out, GLuint *tex_out, int w, int h)
{
    glGenTextures(1, tex_out);
    glBindTexture(GL_TEXTURE_2D, *tex_out);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    p_glGenFramebuffers(1, fbo_out);
    p_glBindFramebuffer(GL_FRAMEBUFFER, *fbo_out);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex_out, 0);
    GLenum status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "bloom: FBO incomplete (status 0x%x)\n", status);
        return false;
    }
    return true;
}

/* Forward declaration so bloom_init's `goto fail;` path can call cleanup. */
static inline void bloom_cleanup(bloom_state_t *b);

static inline bool bloom_init(bloom_state_t *b, int w, int h)
{
    memset(b, 0, sizeof(*b));
    if (w < 1 || h < 1) return false;
    if (!bloom_load_procs()) return false;

    b->width = w;
    b->height = h;

    int bw = w / 4; if (bw < 1) bw = 1;
    int bh = h / 4; if (bh < 1) bh = 1;

    if (!bloom_make_fbo(&b->scene_fbo, &b->scene_tex, w, h))        goto fail;
    if (!bloom_make_fbo(&b->blur_fbo[0], &b->blur_tex[0], bw, bh))  goto fail;
    if (!bloom_make_fbo(&b->blur_fbo[1], &b->blur_tex[1], bw, bh))  goto fail;

    b->blur_prog = bloom_build_program(BLOOM_VS_SRC, BLOOM_BLUR_FS_SRC);
    if (!b->blur_prog) goto fail;
    b->blur_loc_tex = p_glGetUniformLocation(b->blur_prog, "u_tex");
    b->blur_loc_dir = p_glGetUniformLocation(b->blur_prog, "u_direction");

    b->composite_prog = bloom_build_program(BLOOM_VS_SRC, BLOOM_COMP_FS_SRC);
    if (!b->composite_prog) goto fail;
    b->comp_loc_scene     = p_glGetUniformLocation(b->composite_prog, "u_scene");
    b->comp_loc_bloom     = p_glGetUniformLocation(b->composite_prog, "u_bloom");
    b->comp_loc_intensity = p_glGetUniformLocation(b->composite_prog, "u_intensity");

    b->gamma_prog = bloom_build_program(BLOOM_VS_SRC, BLOOM_GAMMA_FS_SRC);
    if (!b->gamma_prog) goto fail;
    b->gamma_loc_tex   = p_glGetUniformLocation(b->gamma_prog, "u_tex");
    b->gamma_loc_gamma = p_glGetUniformLocation(b->gamma_prog, "u_gamma");

    b->enabled = true;
    return true;

fail:
    bloom_cleanup(b);
    return false;
}

static inline void bloom_cleanup(bloom_state_t *b)
{
    if (b->scene_fbo)     { p_glDeleteFramebuffers(1, &b->scene_fbo);    b->scene_fbo = 0; }
    if (b->blur_fbo[0])   { p_glDeleteFramebuffers(1, &b->blur_fbo[0]);  b->blur_fbo[0] = 0; }
    if (b->blur_fbo[1])   { p_glDeleteFramebuffers(1, &b->blur_fbo[1]);  b->blur_fbo[1] = 0; }
    if (b->scene_tex)     { glDeleteTextures(1, &b->scene_tex);          b->scene_tex = 0; }
    if (b->blur_tex[0])   { glDeleteTextures(1, &b->blur_tex[0]);        b->blur_tex[0] = 0; }
    if (b->blur_tex[1])   { glDeleteTextures(1, &b->blur_tex[1]);        b->blur_tex[1] = 0; }
    if (b->blur_prog)     { p_glDeleteProgram(b->blur_prog);             b->blur_prog = 0; }
    if (b->composite_prog){ p_glDeleteProgram(b->composite_prog);        b->composite_prog = 0; }
    if (b->gamma_prog)    { p_glDeleteProgram(b->gamma_prog);            b->gamma_prog = 0; }
    b->enabled = false;
}

static inline void bloom_resize(bloom_state_t *b, int w, int h)
{
    if (!b->enabled || w < 1 || h < 1) return;
    b->width = w;
    b->height = h;
    int bw = w / 4; if (bw < 1) bw = 1;
    int bh = h / 4; if (bh < 1) bh = 1;

    glBindTexture(GL_TEXTURE_2D, b->scene_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);

    glBindTexture(GL_TEXTURE_2D, b->blur_tex[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, NULL);

    glBindTexture(GL_TEXTURE_2D, b->blur_tex[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_FLOAT, NULL);

    glBindTexture(GL_TEXTURE_2D, 0);
}

static inline void bloom_draw_fullscreen_quad(void)
{
    /* Use glTexCoord2f (GL 1.1 core) instead of glMultiTexCoord2f (GL 1.3),
     * which is not declared in Windows mingw-w64's gl.h. gl_MultiTexCoord0
     * in the vertex shader reads the same value for texture unit 0. */
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();
}

static inline void bloom_begin(bloom_state_t *b)
{
    if (!b->enabled) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, b->scene_fbo);
    glViewport(0, 0, b->width, b->height);
    glClear(GL_COLOR_BUFFER_BIT);
}

static inline void bloom_end(bloom_state_t *b, float intensity, float gamma = 1.0f, float radius = 1.0f)
{
    if (!b->enabled) return;

    int bw = b->width / 4; if (bw < 1) bw = 1;
    int bh = b->height / 4; if (bh < 1) bh = 1;

    /* 1. Downsample scene_fbo -> blur_fbo[0] via linear blit. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, b->scene_fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->blur_fbo[0]);
    p_glBlitFramebuffer(0, 0, b->width, b->height,
                        0, 0, bw, bh,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    /* Save caller's matrices; set identity for clip-space fullscreen quad. */
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    /* 2. Pre-blur gamma: blur_fbo[0] -> blur_fbo[1].
     *    Applied before blur so gamma reshapes the high-contrast
     *    scene (bright vs dark) rather than the already-smooth
     *    blurred result. gamma > 1 = only bright spots bloom,
     *    gamma < 1 = everything glows. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, b->blur_fbo[1]);
    glViewport(0, 0, bw, bh);
    p_glUseProgram(b->gamma_prog);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, b->blur_tex[0]);
    p_glUniform1i(b->gamma_loc_tex, 0);
    p_glUniform1f(b->gamma_loc_gamma, gamma);
    bloom_draw_fullscreen_quad();

    /* 3. Blur passes — radius controls count (1.0 = 2 passes, 2.0 = 4, etc.).
     *    Each H+V pair at 1-texel step widens the effective Gaussian.
     *    Gamma pre-pass left result in blur[1], so first H reads [1]. */
    float hw = 1.0f / (float)bw;
    float hh = 1.0f / (float)bh;
    int passes = (int)(radius * 2.0f);
    if (passes < 1) passes = 1;
    int src = 1;
    p_glUseProgram(b->blur_prog);
    p_glUniform1i(b->blur_loc_tex, 0);
    for (int p = 0; p < passes; p++) {
        int dst = 1 - src;
        /* H blur */
        p_glBindFramebuffer(GL_FRAMEBUFFER, b->blur_fbo[dst]);
        glBindTexture(GL_TEXTURE_2D, b->blur_tex[src]);
        p_glUniform2f(b->blur_loc_dir, hw, 0.0f);
        bloom_draw_fullscreen_quad();
        src = dst;
        dst = 1 - src;
        /* V blur */
        p_glBindFramebuffer(GL_FRAMEBUFFER, b->blur_fbo[dst]);
        glBindTexture(GL_TEXTURE_2D, b->blur_tex[src]);
        p_glUniform2f(b->blur_loc_dir, 0.0f, hh);
        bloom_draw_fullscreen_quad();
        src = dst;
    }
    /* Result is in blur[src]. */

    /* 6. Composite scene + bloom to default framebuffer. */
    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, b->width, b->height);
    glClear(GL_COLOR_BUFFER_BIT);

    p_glUseProgram(b->composite_prog);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, b->scene_tex);
    p_glUniform1i(b->comp_loc_scene, 0);
    p_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b->blur_tex[src]);
    p_glUniform1i(b->comp_loc_bloom, 1);
    p_glUniform1f(b->comp_loc_intensity, intensity);
    bloom_draw_fullscreen_quad();

    /* Restore GL state drawText expects. */
    p_glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    p_glUseProgram(0);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

#endif /* XYSCOPE_BLOOM_H */
