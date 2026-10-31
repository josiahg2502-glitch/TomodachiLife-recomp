/* The picture's way to the screen through DXGI (2026-10-30; included by nvn_gl.c, Windows only, render thread only).
 * OpenGL's own SwapBuffers on a window that covers the monitor is flipped directly by the driver, past Windows' compositor
 * (nvn_gl.c, 2026-10-25: "flipped directly"). The user saw the whole picture and single objects flash in the Island Builder,
 * and a screen recording of it held ONE frozen picture for 23 s while the game ran: the compositor's copy of the window
 * never changed, so whenever Windows composed (an overlay, the recorder's bar) the screen showed that stale copy.
 * Now the finished picture is drawn into a texture Direct3D 11 shares with OpenGL (WGL_NV_DX_interop2) and shown by a
 * flip-model swap chain, the way Windows expects a game to present: the compositor, overlays and capture see every frame.
 * graphics.ini present_api = opengl (or TOMO_PRESENT=gl): SwapBuffers as before; also the fallback when any step here
 * fails (no interop extension, Direct3D on another GPU, or the first pictures not arriving: dx_check). */
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_2.h>

static const GUID k_iid_dxgi_device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID k_iid_dxgi_factory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID k_iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
typedef HANDLE (WINAPI *dx_open_t)(void *);
typedef BOOL (WINAPI *dx_close_t)(HANDLE);
typedef HANDLE (WINAPI *dx_reg_t)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL (WINAPI *dx_unreg_t)(HANDLE, HANDLE);
typedef BOOL (WINAPI *dx_lock_t)(HANDLE, GLint, HANDLE *);
static struct {
    int state;                                     /* 0 not tried, 1 on, -1 off (OpenGL's SwapBuffers) */
    ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGISwapChain1 *sc;
    ID3D11Texture2D *pic;                          /* the picture: OpenGL draws it, Direct3D copies it into the back buffer */
    HANDLE idev, iobj;
    GLuint tex, fbo;
    int w, h, locked, checks, attached;
    dx_open_t open; dx_close_t close; dx_reg_t reg; dx_unreg_t unreg; dx_lock_t lock, unlock;
    LONG64 presents;
} g_dx;
static void dx_off(const char *why) {              /* back to SwapBuffers for good */
    if (g_dx.locked && g_dx.unlock) g_dx.unlock(g_dx.idev, 1, &g_dx.iobj);
    if (g_dx.iobj && g_dx.unreg) g_dx.unreg(g_dx.idev, g_dx.iobj);
    if (g_dx.idev && g_dx.close) g_dx.close(g_dx.idev);
    if (g_dx.fbo) glDeleteFramebuffers(1, &g_dx.fbo);
    if (g_dx.tex) glDeleteTextures(1, &g_dx.tex);
    if (g_dx.pic) ID3D11Texture2D_Release(g_dx.pic);
    if (g_dx.sc) IDXGISwapChain1_Release(g_dx.sc);
    if (g_dx.ctx) ID3D11DeviceContext_Release(g_dx.ctx);
    if (g_dx.dev) ID3D11Device_Release(g_dx.dev);
    memset(&g_dx, 0, sizeof g_dx);
    g_dx.state = -1;
    gs_invalidate();
    printf("nvn_gl: the picture goes to the screen through OpenGL's SwapBuffers (%s)%c", why, 10);
    fflush(stdout);
}
static int dx_picture(int w, int h) {              /* (re)make the shared picture texture of the window's size */
    if (g_dx.iobj) { if (g_dx.locked) { g_dx.unlock(g_dx.idev, 1, &g_dx.iobj); g_dx.locked = 0; } g_dx.unreg(g_dx.idev, g_dx.iobj); g_dx.iobj = NULL; }
    if (g_dx.pic) { ID3D11Texture2D_Release(g_dx.pic); g_dx.pic = NULL; }
    if (g_dx.tex) { glDeleteTextures(1, &g_dx.tex); g_dx.tex = 0; }
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dx.dev, &td, NULL, &g_dx.pic))) return 0;
    glGenTextures(1, &g_dx.tex);
    g_dx.iobj = g_dx.reg(g_dx.idev, g_dx.pic, g_dx.tex, GL_TEXTURE_2D, 0x0002 /* WGL_ACCESS_WRITE_DISCARD_NV */);
    if (!g_dx.iobj) return 0;
    g_dx.w = w; g_dx.h = h; g_dx.attached = 0;
    return 1;
}
static int dx_init(int w, int h) {
    const char *e = getenv("TOMO_PRESENT");
    int api = e ? (!_stricmp(e, "gl") || !_stricmp(e, "opengl") ? 2 : 1) : gfx_present_api();
    g_dx.state = -1;
    if (api == 2) { printf("nvn_gl: the picture goes to the screen through OpenGL's SwapBuffers (present_api = opengl)%c", 10); return 0; }
    g_dx.open = (dx_open_t)wglGetProcAddress("wglDXOpenDeviceNV"); g_dx.close = (dx_close_t)wglGetProcAddress("wglDXCloseDeviceNV");
    g_dx.reg = (dx_reg_t)wglGetProcAddress("wglDXRegisterObjectNV"); g_dx.unreg = (dx_unreg_t)wglGetProcAddress("wglDXUnregisterObjectNV");
    g_dx.lock = (dx_lock_t)wglGetProcAddress("wglDXLockObjectsNV"); g_dx.unlock = (dx_lock_t)wglGetProcAddress("wglDXUnlockObjectsNV");
    if (!g_dx.open || !g_dx.close || !g_dx.reg || !g_dx.unreg || !g_dx.lock || !g_dx.unlock) { dx_off("the driver has no WGL_NV_DX_interop2"); return 0; }
    HMODULE m = LoadLibraryA("d3d11.dll");
    PFN_D3D11_CREATE_DEVICE create = m ? (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(m, "D3D11CreateDevice") : NULL;
    if (!create || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_dx.dev, NULL, &g_dx.ctx))) { dx_off("no Direct3D 11 device"); return 0; }
    IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *fa = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_dx.dev, &k_iid_dxgi_device, (void **)&dd)) || FAILED(IDXGIDevice_GetAdapter(dd, &ad)) ||
        FAILED(IDXGIAdapter_GetParent(ad, &k_iid_dxgi_factory2, (void **)&fa))) {
        if (ad) IDXGIAdapter_Release(ad);
        if (dd) IDXGIDevice_Release(dd);
        dx_off("no DXGI 1.2 factory"); return 0;
    }
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = (UINT)w; sd.Height = (UINT)h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(fa, (IUnknown *)g_dx.dev, g_hwnd, &sd, NULL, NULL, &g_dx.sc);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(fa, g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGIFactory2_Release(fa); IDXGIAdapter_Release(ad); IDXGIDevice_Release(dd);
    if (FAILED(hr)) { char why[80]; snprintf(why, sizeof why, "no flip-model swap chain for the window: 0x%08lx", (unsigned long)hr); dx_off(why); return 0; }
    if (!(g_dx.idev = g_dx.open(g_dx.dev))) { dx_off("the driver cannot share textures with this Direct3D device (another GPU?)"); return 0; }
    if (!dx_picture(w, h)) { dx_off("the shared picture texture was not made"); return 0; }
    glGenFramebuffers(1, &g_dx.fbo);
    g_dx.state = 1;
    printf("nvn_gl: the picture goes to the screen through a DXGI flip-model swap chain (%dx%d; present_api = opengl: SwapBuffers)%c", w, h, 10);
    return 1;
}
/* job_present, before the picture is drawn: the framebuffer to draw it into (0: the window's own, SwapBuffers). The
 * picture's rows are top first here (Direct3D's order): *flip = 0 for the caller, which flips them for the window's. */
static GLuint dx_target(int w, int h, int *flip) {
    *flip = 1;
    if (w <= 0 || h <= 0 || g_dx.state < 0) return 0;
    if (!g_dx.state && !dx_init(w, h)) return 0;
    if (w != g_dx.w || h != g_dx.h) {             /* the window changed size: the swap chain and the picture follow */
        if (g_dx.locked) { g_dx.unlock(g_dx.idev, 1, &g_dx.iobj); g_dx.locked = 0; }
        if (FAILED(IDXGISwapChain1_ResizeBuffers(g_dx.sc, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0)) || !dx_picture(w, h)) { dx_off("resizing the swap chain failed"); return 0; }
    }
    if (!g_dx.locked) { if (!g_dx.lock(g_dx.idev, 1, &g_dx.iobj)) { dx_off("the shared picture could not be locked for OpenGL"); return 0; } g_dx.locked = 1; }
    if (!g_dx.attached) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_dx.fbo);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, g_dx.tex, 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER_) != 0x8CD5 /* COMPLETE */) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0); dx_off("the shared picture is not a complete framebuffer"); return 0; }
        g_dx.attached = 1;
    }
    *flip = 0;
    return g_dx.fbo;
}
/* The first pictures with something in them: the pixel in the middle as OpenGL wrote it and as the swap chain's back buffer
 * holds it after the copy. A difference = the sharing does not work on this PC: SwapBuffers from then on. */
static int dx_check(void) {
    uint8_t gl[4] = {0, 0, 0, 0};
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_dx.fbo);
    glReadPixels(g_dx.w / 2, g_dx.h / 2, 1, 1, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, gl);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
    return gl[0] + gl[1] + gl[2] > 24 ? (gl[0] | gl[1] << 8 | gl[2] << 16) : -1;      /* -1: too dark to tell */
}
static int dx_check_back(ID3D11Texture2D *bb, int want) {
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE ms;
    int got = -1;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dx.dev, &td, NULL, &st))) return 1;      /* (cannot tell: keep going) */
    ID3D11DeviceContext_CopyResource(g_dx.ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_dx.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms))) {
        const uint8_t *p = (const uint8_t *)ms.pData + (size_t)(g_dx.h / 2) * ms.RowPitch + (size_t)(g_dx.w / 2) * 4;
        got = p[0] | p[1] << 8 | p[2] << 16;
        ID3D11DeviceContext_Unmap(g_dx.ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    int ok = got >= 0 && abs((got & 255) - (want & 255)) <= 2 && abs((got >> 8 & 255) - (want >> 8 & 255)) <= 2 && abs((got >> 16 & 255) - (want >> 16 & 255)) <= 2;
    printf("nvn_gl: DXGI check: the middle pixel %06x as drawn, %06x in the swap chain: %s%c", want, got, ok ? "the same" : "DIFFERENT", 10);
    return ok;
}
/* gl_swap_now: the picture to the screen. 0 = not through DXGI (the caller calls SwapBuffers). */
static int dx_present(int interval) {
    if (g_dx.state != 1) return 0;
    if (!g_dx.locked) return 0;                    /* nothing drawn since the last present (cannot happen: job_present locks) */
    int want = g_dx.checks < 3 && g_dx.presents % 30 == 0 ? dx_check() : -1;
    static int verify = -1; static unsigned gl_box[3];      /* TEST: TOMO_DXGI_VERIFY=1: every present, a 64x16 patch at the top middle as OpenGL drew
                                                             * it and as the back buffer got it (a stale or half-done copy = a mismatch, logged) */
    if (verify < 0) verify = getenv("TOMO_DXGI_VERIFY") ? atoi(getenv("TOMO_DXGI_VERIFY")) : 0;
    if (verify == 1 && g_dx.w >= 64 && g_dx.h >= 32) {      /* (=2: the back buffer only, every present, no OpenGL read that waits for the GPU) */
        uint8_t px[64 * 16 * 4];
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_dx.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(g_dx.w / 2 - 32, 4, 64, 16, 0x1908 /* RGBA */, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        gl_box[0] = gl_box[1] = gl_box[2] = 0;
        for (int i = 0; i < 64 * 16; i++) { gl_box[0] += px[i * 4]; gl_box[1] += px[i * 4 + 1]; gl_box[2] += px[i * 4 + 2]; }
    }
    g_dx.unlock(g_dx.idev, 1, &g_dx.iobj); g_dx.locked = 0;
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_dx.sc, 0, &k_iid_tex2d, (void **)&bb))) { dx_off("the swap chain's back buffer is not there"); return 0; }
    ID3D11DeviceContext_CopyResource(g_dx.ctx, (ID3D11Resource *)bb, (ID3D11Resource *)g_dx.pic);
    if (verify && g_dx.w >= 64 && g_dx.h >= 32) {
        static ID3D11Texture2D *st; D3D11_MAPPED_SUBRESOURCE ms;
        if (!st) { D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(bb, &td); td.Width = 64; td.Height = 16; td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
                   if (FAILED(ID3D11Device_CreateTexture2D(g_dx.dev, &td, NULL, &st))) st = NULL; }
        D3D11_BOX box = {(UINT)(g_dx.w / 2 - 32), 4, 0, (UINT)(g_dx.w / 2 + 32), 20, 1};
        if (st) {
            ID3D11DeviceContext_CopySubresourceRegion(g_dx.ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_dx.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms))) {
                unsigned d3[3] = {0, 0, 0};
                for (int y = 0; y < 16; y++) for (int x = 0; x < 64; x++) { const uint8_t *p = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * 4; d3[0] += p[0]; d3[1] += p[1]; d3[2] += p[2]; }      /* (RGBA) */
                ID3D11DeviceContext_Unmap(g_dx.ctx, (ID3D11Resource *)st, 0);
                static unsigned long long n, bad;
                if (verify == 2) { printf("dxgi sample: %llu %u %u %u%c", n++, d3[0] / 64, d3[1] / 64, d3[2] / 64, 10); goto sampled; }
                int diff = 0; for (int k = 0; k < 3; k++) { int dd = (int)(d3[k] / 1024) - (int)(gl_box[k] / 1024); if (abs(dd) > diff) diff = abs(dd); }
                n++; if (diff > 1) { bad++; if (bad <= 40) printf("dxgi verify: present %llu: drawn %u %u %u, back buffer %u %u %u%c", n, gl_box[0] / 1024, gl_box[1] / 1024, gl_box[2] / 1024, d3[0] / 1024, d3[1] / 1024, d3[2] / 1024, 10); }
                if (n % 600 == 0) printf("dxgi verify: %llu presents, %llu mismatched%c", n, bad, 10);
                sampled:;
            }
        }
    }
    if (want >= 0) {
        if (!dx_check_back(bb, want)) { ID3D11Texture2D_Release(bb); dx_off("the shared picture did not reach the swap chain"); return 0; }
        g_dx.checks++;
    }
    {   /* TOMO_DXGI_SHOT=<n> (TEST): the swap chain's back buffer at the n-th present, as dxgi_<n>.bmp (what the screen gets) */
        static int shot = -2;
        if (shot == -2) shot = getenv("TOMO_DXGI_SHOT") ? atoi(getenv("TOMO_DXGI_SHOT")) : -1;
        if (shot >= 0 && g_dx.presents == shot) {
            D3D11_TEXTURE2D_DESC td; ID3D11Texture2D *st = NULL; D3D11_MAPPED_SUBRESOURCE ms;
            ID3D11Texture2D_GetDesc(bb, &td);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dx.dev, &td, NULL, &st))) {
                ID3D11DeviceContext_CopyResource(g_dx.ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
                char path[64]; snprintf(path, sizeof path, "dxgi_%d.bmp", shot);
                FILE *f = SUCCEEDED(ID3D11DeviceContext_Map(g_dx.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &ms)) ? fopen(path, "wb") : NULL;
                if (f) {
                    uint32_t w = td.Width, hh = td.Height, row = (w * 3 + 3) & ~3u, size = 54 + row * hh;
                    uint8_t head[54] = {'B', 'M'};
                    int32_t neg = -(int32_t)hh;                      /* top-down rows */
                    memcpy(head + 2, &size, 4); head[10] = 54; head[14] = 40; memcpy(head + 18, &w, 4); memcpy(head + 22, &neg, 4); head[26] = 1; head[28] = 24;
                    fwrite(head, 1, 54, f);
                    uint8_t *line = calloc(1, row);
                    for (uint32_t y = 0; y < hh; y++) {
                        const uint8_t *p = (const uint8_t *)ms.pData + (size_t)y * ms.RowPitch;
                        for (uint32_t x = 0; x < w; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
                        fwrite(line, 1, row, f);
                    }
                    free(line); fclose(f);
                    ID3D11DeviceContext_Unmap(g_dx.ctx, (ID3D11Resource *)st, 0);
                    printf("nvn_gl: DXGI: %s written%c", path, 10);
                }
                ID3D11Texture2D_Release(st);
            }
        }
    }
    ID3D11Texture2D_Release(bb);
    HRESULT hr = IDXGISwapChain1_Present(g_dx.sc, (UINT)interval, 0);
    g_dx.presents++;
    if (FAILED(hr)) { char why[80]; snprintf(why, sizeof why, "Present failed: 0x%08lx", (unsigned long)hr); dx_off(why); return 0; }
    return 1;
}
