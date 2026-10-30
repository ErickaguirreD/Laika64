// Harissa64 V2 - Xenos renderer (Xbox 360, Direct3D 9 of the XDK).
//
// Implements render/api.h with the GPU. The software RDP runs in "state
// only" mode (sys->options.rdpStateOnly) and keeps the RDP state and TMEM;
// this renderer reads them to draw:
//  - HLE triangles (screen-space vertices) and texture/fill rectangles,
//  - with pixel shaders generated from the colour combiner (compiled at run
//    time with D3DXCompileShader, cached per combiner mode),
//  - textures decoded texel by texel from TMEM (h64_rdp_fetch_texel), so
//    every format, palette, mask, mirror and clamp follows the RDP,
//  - blender modes mapped to GPU blending, depth test and alpha compare.
// N64 frames are drawn into an EDRAM render target of their own, resolved
// into one texture per RDRAM colour image, and the one the VI shows is
// drawn to the back buffer and presented at each VI interrupt. Frames the
// CPU wrote itself (no RDP drawing at that address) are shown from RDRAM.
//
// Xbox lessons applied (lessons from Harissa64 V1): never leave a texture unit empty,
// never release a texture the GPU may still read, DrawPrimitiveUP only,
// linear textures for CPU uploads (0xAARRGGBB words on the big-endian CPU).
#ifndef H64_XENOS_H
#define H64_XENOS_H

#include "../api.h"

struct H64System;
struct IDirect3DDevice9;

// Creates the renderer on an existing device (640x480 back buffer created
// without an automatic depth buffer), and puts the software RDP in state
// only mode. Returns NULL on failure.
H64Renderer *h64_xenos_create(H64System *sys, IDirect3DDevice9 *dev);
void h64_xenos_free(H64Renderer *r);

// At each VI interrupt: shows the frame the VI is scanning out and presents.
void h64_xenos_present(H64Renderer *r);

// Optional: called just before each present, with the back buffer bound
// (on-screen messages of the front end). NULL: nothing.
void h64_xenos_set_overlay(H64Renderer *r, void (*fn)(void *user, IDirect3DDevice9 *dev), void *user);

// Saves the frame shown at the last present as a 24-bit BMP (waits for the
// GPU). Returns 0 on success.
int h64_xenos_save_frame(H64Renderer *r, const char *path);
// The same frame as malloc'd RGBA bytes (R, G, B, A), *w x *h; NULL when none.
u8 *h64_xenos_grab_frame(H64Renderer *r, int *w, int *h);

// Captures whatever is bound as render target 0 right now (normally the
// back buffer, with the front end's own UI already drawn on it) to a 24-bit
// BMP. No H64Renderer needed - just the device (xb_ui.cpp's L3+R3 UI
// screenshot, unrelated to the game frame capture above). Returns 0 on success.
int h64_xenos_save_backbuffer(IDirect3DDevice9 *dev, const char *path);

// Debug output of combined primitives: 0 normal, 1 solid red, 2 shade
// colour only, 3 texture 0 only.
void h64_xenos_set_debug(H64Renderer *r, int mode);

struct H64XenosStats
{
    u32 triangles, rects, fills, draws;
    u32 textureUploads, textureCreates, shaderCompiles, presents, copyBacks, fbSwitches;
    u32 texelsDecoded;
    u32 fbTexRects;   // rectangles textured straight from a frame the GPU drew
    u64 tCopyBack;    // copy-backs (GPU wait, readback, conversion)
    u64 tCopyBackWait;   // inside tCopyBack: waiting for the GPU
    u64 tLoads;       // texture loads in the software RDP (inside tState)
    u32 loadResolves; // copy backs put off, then needed (resolve_loads)
    u32 resolveCopyBacks;   // copy backs those made
    u32 cpuWrittenLoads;    // loads from GPU frames the CPU wrote over since (no copy back)
    u32 cpuSprites;         // presents with pixels the CPU drew over the frame (profile gfxCpuSprites)
    // The largest texture decoded: its size, tile (fmt, size, stride, masks, flags) and source
    // (0 triangle, 1 rectangle).
    u32 bigW, bigH, bigFmt, bigSize, bigStride, bigMaskS, bigMaskT, bigFlags, bigRect, bigCount;
    // Time (sys->profClock ticks) in the software RDP state, texture lookups and
    // decoding, draw calls, and the RDP command handler as a whole.
    u64 tState, tTexture, tDraw, tRdp;
    u64 tHash, tDecode;   // inside tTexture: cache keys (TMEM hashing), decoding and upload
    u64 tCreate, tLock, tFill, tUnlock;   // inside tDecode: creation (or arena header), locking, decoding
    u32 arenaTextures, arenaEvictions;    // textures placed in the arena; arena chunks reused
    u32 retires;          // texture cache emptied (full)
};
// Presents with these VI registers (a copy taken at the VI, for the graphics worker).
void h64_xenos_present_vi(H64Renderer *r, const u32 *viRegs);
// The renderer is used by the graphics worker (on) or back on the CPU thread
// (off: RDRAM written meanwhile is reported to the recompiler now).
void h64_xenos_set_worker(H64Renderer *r, int on);
// Loads a hi-res texture pack (GlideHQ/GlideN64 .hts); NULL or "" unloads.
// Returns the number of textures in the pack (0: none loaded).
int h64_xenos_load_hts(H64Renderer *r, const char *path);
// Edge smoothing (FXAA) when the frame is shown, in place of the VI's anti-aliasing (on by default).
void h64_xenos_set_smooth(H64Renderer *r, int on);
// Graphics settings (the menu's Graphics page).
struct H64XenosOptions
{
    unsigned resW, resH;   // internal resolution (a 320x240 image drawn at that size), at most RT_MAX_W x RT_MAX_H and the EDRAM's room (h64_xenos.cpp)
    int texFilter;   // 0 the N64's 3-point filter, 1 bilinear (default), 2 nearest
    int smooth;      // edge smoothing (FXAA) at display
    int sharpen;     // contrast-adaptive sharpening at display: 0 off, 1 low, 2 high
    int blur;        // blur at display: 0 off, 1 soft, 2 strong (instead of sharpening)
    int screen;      // screen effect: 0 none, 1 scanlines, 2 CRT, 3 curved CRT, 4 LCD grid, 5 light scanlines
    int aspect;      // 0 4:3, 1 16:9 widescreen (3D drawn wider, no distortion), 2 16:9 stretched
    int brightness;  // 0 as the console, 1 lighter, 2 much lighter (dark tones lifted, white kept), 3 darker, 4 much darker
    float sharpAmount, blurAmount;   // 0..1: how strong the sharpening / blur (on when `sharpen` / `blur` is)
    float brightPower;   // the brightness as a power on the picture (under 1 lighter, over 1 darker); 0: `brightness`'s step
    int colorStyle;  // the picture's colours: 0 as they are, 1 saturated, 2 vivid, 3 black and white, 4 sepia, 5 warm, 6 cool, 7 green (an old handheld)
    int vsync;       // 1: frames on time wait for the TV's refresh, a late one goes at once; 2 (aggressive): every frame waits (no tearing)
    int deflicker;   // 1: each picture shown is mixed half and half with the previous one
    int hiTex;       // 1: replace textures from the loaded .hts pack (h64_xenos_load_hts)
    int zoom;        // picture size in percent (100: as the aspect gives; more crops the border)
    int offsetX, offsetY;   // picture position on the 1280x720 output, in pixels
    int gfxLog;      // 1: log each new drawing mode, and many more pack misses and replacement details (developer mode)
};
void h64_xenos_set_options(H64Renderer *r, const H64XenosOptions *o);
void h64_xenos_stats(H64Renderer *r, H64XenosStats *out, int reset);
// The internal resolution the games are drawn at (rtW x rtH).
void h64_xenos_internal_size(H64Renderer *r, u32 *w, u32 *h);

#endif
