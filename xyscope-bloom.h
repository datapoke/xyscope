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

static inline bool bloom_load_procs(void)
{
    #define LOAD(name) do { \
        p_##name = (decltype(p_##name))SDL_GL_GetProcAddress(#name); \
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

static const char *BLOOM_COMP_FS_SRC =
    "#version 120\n"
    "uniform sampler2D u_scene;\n"
    "uniform sampler2D u_bloom;\n"
    "uniform float u_intensity;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 s = texture2D(u_scene, v_uv);\n"
    "    vec4 b = texture2D(u_bloom, v_uv);\n"
    "    gl_FragColor = s + b * u_intensity;\n"
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

/* bloom_begin / bloom_end — still stubs. Task 3 implements these. */
static inline void bloom_begin  (bloom_state_t *b)               { (void)b; }
static inline void bloom_end    (bloom_state_t *b, float intensity) { (void)b; (void)intensity; }

#endif /* XYSCOPE_BLOOM_H */
