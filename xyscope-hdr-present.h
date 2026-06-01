/*
 *  xyscope-hdr-present.h
 *  Windows-only: present the OpenGL frame through a DXGI flip-model
 *  swapchain with an explicit scRGB color space and HDR10 metadata, so
 *  the compositor drives the panel to its real peak luminance instead of
 *  tone-mapping our values down.
 *
 *  Why this exists: a raw WGL fp16 window presented with SwapBuffers is
 *  composited by the DWM as scRGB but stays subject to the system HDR
 *  tone-map — bright values get rolled off well below the panel peak (the
 *  trace looks dim and stops brightening even as the value climbs). The
 *  fix is to declare HDR explicitly via IDXGISwapChain3::SetColorSpace1 +
 *  IDXGISwapChain4::SetHDRMetaData, which only exist on a DXGI swapchain.
 *  OpenGL has no native DXGI present, so we bridge with WGL_NV_DX_interop2:
 *  share a D3D fp16 texture into GL, blit the finished GL frame (FBO 0)
 *  into it, copy that into the swapchain backbuffer, and Present.
 *
 *  All rendering stays in OpenGL exactly as before. This module only
 *  replaces the final SwapBuffers. Every entry point degrades gracefully:
 *  if any D3D/interop step fails, init returns false and the caller falls
 *  back to the plain WGL SwapBuffers path — the app never regresses.
 *
 *  Copyright (c) 2006-2026 by Chris Reaume <chris@flatlan.net>
 *  GPL v2 or later.
 */

#ifndef XYSCOPE_HDR_PRESENT_H
#define XYSCOPE_HDR_PRESENT_H

#ifdef _WIN32

#include <windows.h>
#include <d3d11.h>
#include <d3d9.h>
#include <dxgi1_6.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

/* --- GL constants we need (may be absent from mingw's gl.h) --- */
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER   0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER   0x8CA9
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER        0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0  0x8CE0
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER       0x8D41
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT         0x140B
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

/* --- WGL_NV_DX_interop / interop2 --- */
#ifndef WGL_ACCESS_WRITE_DISCARD_NV
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002
#endif

typedef HANDLE (WINAPI *HP_wglDXOpenDeviceNV)(void *);
typedef BOOL   (WINAPI *HP_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE (WINAPI *HP_wglDXRegisterObjectNV)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL   (WINAPI *HP_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL   (WINAPI *HP_wglDXLockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef BOOL   (WINAPI *HP_wglDXUnlockObjectsNV)(HANDLE, GLint, HANDLE *);
typedef BOOL   (WINAPI *HP_wglDXSetResourceShareHandleNV)(void *, HANDLE);

/* --- GL FBO/blit procs (named distinctly from xyscope-bloom.h) --- */
typedef void (APIENTRY *HP_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *HP_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *HP_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *HP_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRY *HP_glCheckFramebufferStatus)(GLenum);
typedef void (APIENTRY *HP_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY *HP_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void (APIENTRY *HP_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *HP_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

typedef struct {
    bool enabled;
    int  width, height;

    ID3D11Device        *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain3     *swapchain;     /* SetColorSpace1 */
    IDXGISwapChain4     *swapchain4;    /* SetHDRMetaData (may be NULL) */

    ID3D11Texture2D     *shared_tex;    /* fp16 RT shared with GL (D3D11 interop) */
    HANDLE               gl_device;     /* wglDXOpenDeviceNV handle (D3D11 or D3D9) */
    HANDLE               gl_object;     /* registered interop object */
    GLuint               gl_rbo;        /* GL renderbuffer backed by the shared RT */
    GLuint               gl_fbo;        /* FBO wrapping gl_rbo */
    ID3D11Texture2D     *copy_src;      /* D3D11 texture CopyResource'd to backbuffer */
    HWND                 gl_hwnd;       /* focus window for the D3D9 device */

    /* D3D9 interop bridge (AMD: WGL interop supports D3D9, not D3D11). GL
     * renders into a shared D3D9 fp16 surface, opened in D3D11 as shared11. */
    IDirect3D9Ex        *d3d9;
    IDirect3DDevice9Ex  *device9;
    IDirect3DTexture9   *d3d9_tex;
    IDirect3DSurface9   *d3d9_surf;
    ID3D11Texture2D     *shared11;      /* D3D11 view of the shared D3D9 texture */

    /* Readback fallback (used when GL<->D3D interop is unsupported, e.g.
     * AMD's WGL_NV_DX_interop only supports D3D9, not D3D11). Slower:
     * glReadPixels the GL frame, upload into a CPU-writable D3D texture. */
    bool                 use_readback;
    ID3D11Texture2D     *upload_tex;    /* DYNAMIC fp16, CPU-written */
    void                *readback_buf;  /* glReadPixels destination */

    double               peak_nits;

    /* interop + FBO procs */
    HP_wglDXOpenDeviceNV             pDXOpen;
    HP_wglDXCloseDeviceNV            pDXClose;
    HP_wglDXRegisterObjectNV        pDXRegister;
    HP_wglDXUnregisterObjectNV      pDXUnregister;
    HP_wglDXLockObjectsNV           pDXLock;
    HP_wglDXUnlockObjectsNV         pDXUnlock;
    HP_wglDXSetResourceShareHandleNV pDXSetShare;

    HP_glGenFramebuffers        pGenFB;
    HP_glDeleteFramebuffers     pDelFB;
    HP_glBindFramebuffer        pBindFB;
    HP_glCheckFramebufferStatus pCheckFB;
    HP_glBlitFramebuffer        pBlitFB;
    HP_glGenRenderbuffers       pGenRB;
    HP_glDeleteRenderbuffers    pDelRB;
    HP_glFramebufferRenderbuffer pFBRB;
} hdr_present_t;

/* Forward decl so init's failure path can clean up. */
static inline void hdr_present_shutdown(hdr_present_t *hp);

static inline bool hdr_present_load_procs(hdr_present_t *hp)
{
    hp->pDXOpen      = (HP_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
    hp->pDXClose     = (HP_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
    hp->pDXRegister  = (HP_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
    hp->pDXUnregister= (HP_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
    hp->pDXLock      = (HP_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
    hp->pDXUnlock    = (HP_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
    hp->pDXSetShare  = (HP_wglDXSetResourceShareHandleNV)wglGetProcAddress("wglDXSetResourceShareHandleNV");

    hp->pGenFB   = (HP_glGenFramebuffers)wglGetProcAddress("glGenFramebuffers");
    hp->pDelFB   = (HP_glDeleteFramebuffers)wglGetProcAddress("glDeleteFramebuffers");
    hp->pBindFB  = (HP_glBindFramebuffer)wglGetProcAddress("glBindFramebuffer");
    hp->pCheckFB = (HP_glCheckFramebufferStatus)wglGetProcAddress("glCheckFramebufferStatus");
    hp->pBlitFB  = (HP_glBlitFramebuffer)wglGetProcAddress("glBlitFramebuffer");
    hp->pGenRB   = (HP_glGenRenderbuffers)wglGetProcAddress("glGenRenderbuffers");
    hp->pDelRB   = (HP_glDeleteRenderbuffers)wglGetProcAddress("glDeleteRenderbuffers");
    hp->pFBRB    = (HP_glFramebufferRenderbuffer)wglGetProcAddress("glFramebufferRenderbuffer");

    /* pDXSetShare is only needed for the interop1 (shared) fallback. */
    if (!hp->pDXOpen || !hp->pDXClose || !hp->pDXRegister || !hp->pDXUnregister
        || !hp->pDXLock || !hp->pDXUnlock) {
        fprintf(stderr, "HDR present: WGL_NV_DX_interop2 not available\n");
        return false;
    }
    if (!hp->pGenFB || !hp->pDelFB || !hp->pBindFB || !hp->pCheckFB
        || !hp->pBlitFB || !hp->pGenRB || !hp->pDelRB || !hp->pFBRB) {
        fprintf(stderr, "HDR present: GL framebuffer procs unavailable\n");
        return false;
    }
    return true;
}

/* Create an fp16 D3D render target and register it with GL as a
 * renderbuffer. `shared` selects interop1 (MISC_SHARED + share handle) vs
 * interop2 (non-shared) — AMD/NVIDIA drivers differ on which they accept,
 * so the caller tries both. Cleans up its own partial state on failure so
 * the next attempt starts fresh. */
static inline bool hdr_present_register_rt(hdr_present_t *hp, bool shared)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = (UINT)hp->width;
    td.Height           = (UINT)hp->height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = shared ? D3D11_RESOURCE_MISC_SHARED : 0;

    if (FAILED(hp->device->CreateTexture2D(&td, NULL, &hp->shared_tex)) || !hp->shared_tex) {
        fprintf(stderr, "HDR present: CreateTexture2D failed\n");
        return false;
    }

    if (shared && hp->pDXSetShare) {
        IDXGIResource *res = NULL;
        if (SUCCEEDED(hp->shared_tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res)) && res) {
            HANDLE share = NULL;
            if (SUCCEEDED(res->GetSharedHandle(&share)) && share)
                hp->pDXSetShare(hp->shared_tex, share);
            res->Release();
        }
    }

    hp->pGenRB(1, &hp->gl_rbo);
    hp->gl_object = hp->pDXRegister(hp->gl_device, hp->shared_tex, hp->gl_rbo,
                                    GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!hp->gl_object) {
        fprintf(stderr, "HDR present: register (%s) failed (GetLastError 0x%lx)\n",
                shared ? "shared" : "non-shared", (unsigned long)GetLastError());
        hp->pDelRB(1, &hp->gl_rbo); hp->gl_rbo = 0;
        hp->shared_tex->Release(); hp->shared_tex = NULL;
        return false;
    }
    return true;
}

/* Wrap the registered gl_rbo in an FBO we can blit into. Shared by the
 * D3D11 and D3D9 interop paths. */
static inline bool hdr_present_build_fbo(hdr_present_t *hp)
{
    hp->pGenFB(1, &hp->gl_fbo);
    hp->pBindFB(GL_FRAMEBUFFER, hp->gl_fbo);
    hp->pFBRB(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, hp->gl_rbo);
    GLenum status = hp->pCheckFB(GL_FRAMEBUFFER);
    hp->pBindFB(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "HDR present: interop FBO incomplete (0x%x)\n", status);
        return false;
    }
    return true;
}

/* D3D11 interop: zero-copy on NVIDIA/Intel; fails on AMD (D3D9-only). */
static inline bool hdr_present_make_interop11(hdr_present_t *hp)
{
    hp->gl_device = hp->pDXOpen(hp->device);
    if (!hp->gl_device) {
        fprintf(stderr, "HDR present: wglDXOpenDeviceNV(D3D11) failed\n");
        return false;
    }
    if (!hdr_present_register_rt(hp, false) && !hdr_present_register_rt(hp, true)) {
        fprintf(stderr, "HDR present: could not register D3D11 interop RT\n");
        return false;
    }
    if (!hdr_present_build_fbo(hp)) return false;
    hp->copy_src = hp->shared_tex;
    return true;
}

/* D3D9 interop: AMD-friendly zero-copy path. GL renders into a shared D3D9
 * fp16 surface; the same resource is opened in D3D11 (shared11) and copied
 * to the swapchain backbuffer. Needs a focus window (the hidden GL one). */
static inline bool hdr_present_make_interop9(hdr_present_t *hp)
{
    if (!hp->gl_hwnd) return false;

    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &hp->d3d9)) || !hp->d3d9) {
        fprintf(stderr, "HDR present: Direct3DCreate9Ex failed\n");
        return false;
    }
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth      = 1;
    pp.BackBufferHeight     = 1;
    pp.BackBufferFormat     = D3DFMT_UNKNOWN;
    pp.hDeviceWindow        = hp->gl_hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(hp->d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hp->gl_hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED
            | D3DCREATE_FPU_PRESERVE, &pp, NULL, &hp->device9)) || !hp->device9) {
        fprintf(stderr, "HDR present: D3D9 CreateDeviceEx failed\n");
        return false;
    }

    /* Shared fp16 render-target texture; D3D11 opens the same resource. */
    HANDLE share = NULL;
    if (FAILED(hp->device9->CreateTexture(hp->width, hp->height, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
            &hp->d3d9_tex, &share)) || !hp->d3d9_tex || !share) {
        fprintf(stderr, "HDR present: D3D9 shared fp16 RT failed\n");
        return false;
    }
    if (FAILED(hp->d3d9_tex->GetSurfaceLevel(0, &hp->d3d9_surf)) || !hp->d3d9_surf)
        return false;
    if (FAILED(hp->device->OpenSharedResource(share, __uuidof(ID3D11Texture2D),
            (void **)&hp->shared11)) || !hp->shared11) {
        fprintf(stderr, "HDR present: OpenSharedResource D3D9->D3D11 failed\n");
        return false;
    }

    /* Register the D3D9 surface with GL (AMD supports D3D9 interop). */
    hp->gl_device = hp->pDXOpen(hp->device9);
    if (!hp->gl_device) {
        fprintf(stderr, "HDR present: wglDXOpenDeviceNV(D3D9) failed\n");
        return false;
    }
    hp->pGenRB(1, &hp->gl_rbo);
    hp->gl_object = hp->pDXRegister(hp->gl_device, hp->d3d9_surf, hp->gl_rbo,
                                    GL_RENDERBUFFER, WGL_ACCESS_WRITE_DISCARD_NV);
    if (!hp->gl_object) {
        fprintf(stderr, "HDR present: register D3D9 surface failed (GetLastError 0x%lx)\n",
                (unsigned long)GetLastError());
        return false;
    }
    if (!hdr_present_build_fbo(hp)) return false;
    hp->copy_src = hp->shared11;
    return true;
}

/* Readback bridge: a CPU-writable fp16 texture plus a glReadPixels buffer.
 * Used when GL<->D3D interop is unavailable (AMD D3D11). */
static inline void hdr_present_free_readback(hdr_present_t *hp)
{
    if (hp->upload_tex)   { hp->upload_tex->Release(); hp->upload_tex = NULL; }
    if (hp->readback_buf) { free(hp->readback_buf); hp->readback_buf = NULL; }
}

static inline bool hdr_present_make_readback(hdr_present_t *hp)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = (UINT)hp->width;
    td.Height           = (UINT)hp->height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;       /* UpdateSubresource + CopyResource */
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(hp->device->CreateTexture2D(&td, NULL, &hp->upload_tex)) || !hp->upload_tex) {
        fprintf(stderr, "HDR present: readback upload texture failed\n");
        return false;
    }
    hp->readback_buf = malloc((size_t)hp->width * hp->height * 8); /* fp16 RGBA */
    if (!hp->readback_buf) {
        hp->upload_tex->Release(); hp->upload_tex = NULL;
        return false;
    }
    return true;
}

/* Tear down every bridge resource (GL interop, D3D11 RT, D3D9 chain,
 * readback). Fully guarded — safe to call on partial/failed state. */
static inline void hdr_present_free_bridge(hdr_present_t *hp)
{
    if (hp->gl_fbo)    { hp->pDelFB(1, &hp->gl_fbo); hp->gl_fbo = 0; }
    if (hp->gl_object) { hp->pDXUnregister(hp->gl_device, hp->gl_object); hp->gl_object = NULL; }
    if (hp->gl_rbo)    { hp->pDelRB(1, &hp->gl_rbo); hp->gl_rbo = 0; }
    if (hp->gl_device) { hp->pDXClose(hp->gl_device); hp->gl_device = NULL; }
    if (hp->shared_tex){ hp->shared_tex->Release(); hp->shared_tex = NULL; }
    if (hp->shared11)  { hp->shared11->Release(); hp->shared11 = NULL; }
    if (hp->d3d9_surf) { hp->d3d9_surf->Release(); hp->d3d9_surf = NULL; }
    if (hp->d3d9_tex)  { hp->d3d9_tex->Release(); hp->d3d9_tex = NULL; }
    if (hp->device9)   { hp->device9->Release(); hp->device9 = NULL; }
    if (hp->d3d9)      { hp->d3d9->Release(); hp->d3d9 = NULL; }
    hdr_present_free_readback(hp);
    hp->copy_src = NULL;
}

/* Build the GL->swapchain bridge, best path first: zero-copy D3D11 interop
 * (NVIDIA/Intel), then zero-copy D3D9 interop (AMD), then CPU readback. */
static inline bool hdr_present_make_bridge(hdr_present_t *hp)
{
    hp->use_readback = false;
    hp->copy_src     = NULL;
    if (hdr_present_make_interop11(hp)) return true;
    hdr_present_free_bridge(hp);
    if (hdr_present_make_interop9(hp))  return true;
    hdr_present_free_bridge(hp);
    if (hdr_present_make_readback(hp)) { hp->use_readback = true; return true; }
    hdr_present_free_bridge(hp);
    return false;
}

static inline void hdr_present_set_metadata(hdr_present_t *hp)
{
    if (!hp->swapchain) return;
    /* scRGB: linear, Rec.709 primaries, full range. 1.0 = 80 nits. */
    hp->swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);

    if (hp->swapchain4) {
        /* HDR10 mastering metadata. Primaries in units of 0.00002,
         * mastering luminance in units of 0.0001 nit, CLL in nits.
         * Rec.709 primaries / D65 white describe our scRGB content. */
        DXGI_HDR_METADATA_HDR10 md = {};
        md.RedPrimary[0]   = (UINT16)(0.640 * 50000); md.RedPrimary[1]   = (UINT16)(0.330 * 50000);
        md.GreenPrimary[0] = (UINT16)(0.300 * 50000); md.GreenPrimary[1] = (UINT16)(0.600 * 50000);
        md.BluePrimary[0]  = (UINT16)(0.150 * 50000); md.BluePrimary[1]  = (UINT16)(0.060 * 50000);
        md.WhitePoint[0]   = (UINT16)(0.3127 * 50000); md.WhitePoint[1]  = (UINT16)(0.3290 * 50000);
        double peak = hp->peak_nits > 0 ? hp->peak_nits : 1000.0;
        md.MaxMasteringLuminance = (UINT)peak;  /* units of 1 nit */
        md.MinMasteringLuminance = 0;           /* units of 0.0001 nit */
        md.MaxContentLightLevel      = (UINT16)peak;
        md.MaxFrameAverageLightLevel = (UINT16)(peak * 0.5);
        hp->swapchain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10, sizeof(md), &md);
    }
}

static inline bool hdr_present_init(hdr_present_t *hp, HWND hwnd, HWND gl_hwnd,
                                    int w, int h, double peak_nits)
{
    memset(hp, 0, sizeof(*hp));
    if (w < 1 || h < 1) return false;
    hp->width = w; hp->height = h; hp->peak_nits = peak_nits;
    hp->gl_hwnd = gl_hwnd;

    if (!hdr_present_load_procs(hp)) return false;

    /* 1. D3D11 device on the default hardware adapter. */
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                   NULL, 0, D3D11_SDK_VERSION,
                                   &hp->device, NULL, &hp->context);
    if (FAILED(hr) || !hp->device) {
        fprintf(stderr, "HDR present: D3D11CreateDevice failed (0x%lx)\n", hr);
        goto fail;
    }

    /* 2. Reach the DXGI factory that owns this device, make a flip swapchain. */
    {
        IDXGIDevice *dxgiDev = NULL;
        IDXGIAdapter *adapter = NULL;
        IDXGIFactory2 *factory = NULL;
        if (FAILED(hp->device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev)) || !dxgiDev)
            goto fail;
        if (FAILED(dxgiDev->GetAdapter(&adapter)) || !adapter) { dxgiDev->Release(); goto fail; }
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory)) || !factory) {
            adapter->Release(); dxgiDev->Release(); goto fail;
        }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width       = (UINT)w;
        sd.Height      = (UINT)h;
        sd.Format      = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Stereo      = FALSE;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling     = DXGI_SCALING_STRETCH;
        sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

        IDXGISwapChain1 *sc1 = NULL;
        hr = factory->CreateSwapChainForHwnd(hp->device, hwnd, &sd, NULL, NULL, &sc1);
        /* Stop DXGI's Alt+Enter handling on our borderless window. */
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release(); adapter->Release(); dxgiDev->Release();
        if (FAILED(hr) || !sc1) {
            fprintf(stderr, "HDR present: CreateSwapChainForHwnd failed (0x%lx)\n", hr);
            goto fail;
        }
        if (FAILED(sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&hp->swapchain))) {
            sc1->Release(); goto fail;
        }
        sc1->QueryInterface(__uuidof(IDXGISwapChain4), (void **)&hp->swapchain4); /* optional */
        sc1->Release();
    }

    /* 3. Declare scRGB + HDR10 metadata so the DWM drives full panel peak. */
    hdr_present_set_metadata(hp);

    /* 4. Bridge GL -> the swapchain (D3D11 interop / D3D9 interop / readback). */
    if (!hdr_present_make_bridge(hp)) goto fail;

    hp->enabled = true;
    printf("HDR present: DXGI scRGB swapchain active (peak %.0f nits) [%s]\n",
           peak_nits, hp->use_readback ? "readback"
                      : (hp->d3d9 ? "D3D9 interop" : "D3D11 interop"));
    return true;

fail:
    hdr_present_shutdown(hp);
    return false;
}

static inline void hdr_present_resize(hdr_present_t *hp, int w, int h)
{
    if (!hp->enabled || w < 1 || h < 1) return;
    if (w == hp->width && h == hp->height) return;

    hdr_present_free_bridge(hp);
    hp->width = w; hp->height = h;

    /* Backbuffers must be released before ResizeBuffers; we never hold one
     * past Present, so just resize and rebuild the GL->swapchain bridge. */
    HRESULT hr = hp->swapchain->ResizeBuffers(0, (UINT)w, (UINT)h,
                                              DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "HDR present: ResizeBuffers failed (0x%lx); disabling\n", hr);
        hp->enabled = false;
        return;
    }
    hdr_present_set_metadata(hp);     /* re-assert after resize */
    if (!hdr_present_make_bridge(hp)) hp->enabled = false;
}

/* Blit the finished GL frame (src_fbo, src_w x src_h) into the shared
 * texture, copy into the swapchain backbuffer, and Present. Returns false
 * if it could not present (caller should fall back to SwapBuffers). */
static inline bool hdr_present_swap(hdr_present_t *hp, GLuint src_fbo,
                                    int src_w, int src_h)
{
    if (!hp->enabled) return false;

    ID3D11Texture2D *source = NULL;   /* what we CopyResource into the backbuffer */

    if (hp->use_readback) {
        /* CPU path: read the GL frame and upload it into a D3D texture.
         * NOTE: presents vertically flipped (GL bottom-left vs D3D
         * top-left); orientation is a trivial fix once the path is proven. */
        (void)src_w; (void)src_h;
        hp->pBindFB(GL_READ_FRAMEBUFFER, src_fbo);
        glReadPixels(0, 0, hp->width, hp->height, GL_RGBA, GL_HALF_FLOAT,
                     hp->readback_buf);
        hp->pBindFB(GL_READ_FRAMEBUFFER, 0);
        hp->context->UpdateSubresource(hp->upload_tex, 0, NULL,
                                       hp->readback_buf, (UINT)(hp->width * 8), 0);
        source = hp->upload_tex;
    } else {
        /* Zero-copy interop path. */
        if (!hp->pDXLock(hp->gl_device, 1, &hp->gl_object))
            return false;
        hp->pBindFB(GL_READ_FRAMEBUFFER, src_fbo);
        hp->pBindFB(GL_DRAW_FRAMEBUFFER, hp->gl_fbo);
        /* Flip Y: GL origin is bottom-left, D3D/DXGI is top-left. */
        hp->pBlitFB(0, 0, src_w, src_h,
                    0, hp->height, hp->width, 0,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
        hp->pBindFB(GL_FRAMEBUFFER, 0);
        hp->pDXUnlock(hp->gl_device, 1, &hp->gl_object);
        source = hp->copy_src;
    }

    ID3D11Texture2D *back = NULL;
    if (FAILED(hp->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back)
        return false;
    hp->context->CopyResource(back, source);
    back->Release();

    HRESULT hr = hp->swapchain->Present(1, 0);
    return SUCCEEDED(hr);
}

static inline void hdr_present_shutdown(hdr_present_t *hp)
{
    /* fully guarded — safe even if no bridge was built. */
    hdr_present_free_bridge(hp);
    if (hp->swapchain4){ hp->swapchain4->Release(); hp->swapchain4 = NULL; }
    if (hp->swapchain) { hp->swapchain->Release(); hp->swapchain = NULL; }
    if (hp->context)   { hp->context->Release(); hp->context = NULL; }
    if (hp->device)    { hp->device->Release(); hp->device = NULL; }
    hp->enabled = false;
}

#endif /* _WIN32 */
#endif /* XYSCOPE_HDR_PRESENT_H */
