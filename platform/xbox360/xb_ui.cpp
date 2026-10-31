// Harissa64 V2 - screen drawing for the front end (see xb_ui.h).
#include "xb_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "../../core/common/h64_log.h"
#include "../../core/common/h64_png.h"
#include "../../core/common/h64_types.h"
#include "../../render/xenos/h64_xenos.h"
#include "xb_audio.h"
#include "xb_lang.h"
#include "xb_logo.h"
#include "xb_logo_variants.h"
#include "xb_ui_atlas.h"
#include "xb_ui_fonts.h"

// stb_image (public domain / MIT, third_party/stb/): the wallpapers' JPEG, PNG
// and BMP - D3DX's JPEG reader on the console turned down every photo tried.
// CPU only, so the wallpapers' reading thread decodes with it too.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#pragma warning(push, 1)
#include "../../third_party/stb/stb_image.h"
#pragma warning(pop)

// ---- Device resources ----
// One vertex: position (pixels, turned into clip space here), colour, t and p.
// Shapes: t = (x, y from the shape's centre, 0, half ring width or 0),
//         p = (half width, half height, corner radius, edge width).
// Atlas:  t = (u, v, 1, 0), p = (screen pixels per unit of the field, edge offset, 0, 1).
struct UiVtx
{
    float x, y;
    D3DCOLOR color;
    float t[4];
    float p[4];
};

static const D3DVERTEXELEMENT9 s_declElems[] = {
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    { 0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 28, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
    D3DDECL_END()
};

static const char s_uiVs[] =
    "struct VIN { float2 pos : POSITION; float4 col : COLOR0; float4 t : TEXCOORD0; float4 p : TEXCOORD1; };\n"
    "struct VOUT { float4 pos : POSITION; float4 col : COLOR0; float4 t : TEXCOORD0; float4 p : TEXCOORD1; };\n"
    "VOUT main(VIN v) { VOUT o; o.pos = float4(v.pos, 0.0, 1.0); o.col = v.col; o.t = v.t; o.p = v.p; return o; }\n";

// Rounded box distance (negative inside), optionally turned into a ring; or
// the atlas's distance field; or a picture (t.z = 2: the image on s1 over the
// box, its corners rounded like a shape's). Kept short: the Xenos compiler
// ran out of registers on long display shaders.
static const char s_uiPs[] =
    "sampler atlas : register(s0);\n"
    "sampler image : register(s1);\n"
    "float4 main(float4 col : COLOR0, float4 t : TEXCOORD0, float4 p : TEXCOORD1) : COLOR\n"
    "{\n"
    "  float2 q = abs(t.xy) - p.xy + p.z;\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - p.z;\n"
    "  d = t.w > 0.0 ? abs(d + t.w) - t.w : d;\n"
    "  float shape = saturate(0.5 - d / p.w);\n"
    "  shape = shape * shape * (3.0 - 2.0 * shape);\n"
    "  float text = saturate((tex2D(atlas, t.xy).x - 0.5) * p.x + p.y);\n"
    "  float4 img = t.z > 1.5 ? tex2D(image, t.xy / (2.0 * p.xy) + 0.5) : float4(1.0, 1.0, 1.0, 1.0);\n"
    "  return float4(col.rgb * img.rgb, col.a * img.a * (t.z > 0.5 && t.z < 1.5 ? text : shape));\n"
    "}\n";

static IDirect3DDevice9 *s_dev;
static IDirect3DTexture9 *s_atlas;
// The font drawn (Settings > Interface > Font): 0 Inter (xb_ui_atlas.h), else
// s_extraAtlas[n - 1] (xb_ui_fonts.h) - its glyphs, metrics, logo and texture.
static int s_fontSet, s_fontWant;
static const UiAtlasGlyph (*s_glyphTab)[UI_ATLAS_COUNT] = s_atlasGlyph;
static const UiAtlasFont *s_fontTab = s_atlasFont;
static const UiAtlasRect *s_logoTab = s_atlasLogo;
static const unsigned char *s_rle = s_atlasRle;
static unsigned s_rleSize = sizeof(s_atlasRle);
static int s_atlasH = UI_ATLAS_H;
static IDirect3DVertexShader9 *s_vs;
static IDirect3DPixelShader9 *s_ps;
static IDirect3DVertexDeclaration9 *s_decl;
static int s_ready;   // 1: resources made, -1: failed (the UI draws nothing)

// ---- UI screenshot (L3+R3) ----
#define UI_SHOT_MS 2000
static int s_shotPending;   // set by UiInputPoll (no device there); taken in UiEnd, which has one
static char s_shotMsg[160];
static DWORD s_shotUntil;

#define UI_MAX_QUADS 256
static UiVtx s_vtx[UI_MAX_QUADS * 6];
static int s_quads;

static LPD3DXBUFFER compile(const char *src, const char *profile)
{
    LPD3DXBUFFER code = NULL, err = NULL;
    if (FAILED(D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", profile, 0, &code, &err, NULL)))
    {
        H64_ERROR("[ui] %s shader compile failed: %s", profile, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return NULL;
    }
    if (err) err->Release();
    return code;
}

static int make_atlas(IDirect3DDevice9 *dev)
{
    D3DLOCKED_RECT lr;
    const unsigned char *src = s_rle, *end = s_rle + s_rleSize;
    int x = 0, y = 0;
    u8 *row;
    if (FAILED(dev->CreateTexture(UI_ATLAS_W, s_atlasH, 1, 0, D3DFMT_LIN_L8, D3DPOOL_DEFAULT, &s_atlas, NULL)) || !s_atlas)
        return 0;
    if (FAILED(s_atlas->LockRect(0, &lr, NULL, 0))) return 0;
    row = (u8 *)lr.pBits;
    // Run-length decoding straight into the texture, row by row (the pitch may exceed the width).
    while (src < end && y < s_atlasH)
    {
        int c = *src++, n, i;
        if (c < 128)
        {
            n = c + 1;
            for (i = 0; i < n && src < end; i++)
            {
                row[x] = *src++;
                if (++x == UI_ATLAS_W) { x = 0; y++; row = (u8 *)lr.pBits + y * lr.Pitch; if (y == s_atlasH) break; }
            }
        }
        else
        {
            u8 v = src < end ? *src++ : 0;
            n = c - 126;
            for (i = 0; i < n; i++)
            {
                row[x] = v;
                if (++x == UI_ATLAS_W) { x = 0; y++; row = (u8 *)lr.pBits + y * lr.Pitch; if (y == s_atlasH) break; }
            }
        }
    }
    s_atlas->UnlockRect(0);
    return y == s_atlasH;
}

static int init_resources(IDirect3DDevice9 *dev)
{
    LPD3DXBUFFER vs = compile(s_uiVs, "vs_3_0"), ps = compile(s_uiPs, "ps_3_0");
    if (vs) { dev->CreateVertexShader((const DWORD *)vs->GetBufferPointer(), &s_vs); vs->Release(); }
    if (ps) { dev->CreatePixelShader((const DWORD *)ps->GetBufferPointer(), &s_ps); ps->Release(); }
    dev->CreateVertexDeclaration(s_declElems, &s_decl);
    if (!make_atlas(dev)) H64_ERROR("[ui] atlas texture failed");
    if (!s_vs || !s_ps || !s_decl || !s_atlas)
    {
        H64_ERROR("[ui] initialisation failed (vs %p ps %p decl %p atlas %p)", (void *)s_vs, (void *)s_ps, (void *)s_decl, (void *)s_atlas);
        return -1;
    }
    H64_INFO("[ui] ready (atlas %dx%d)", UI_ATLAS_W, UI_ATLAS_H);
    return 1;
}

float UiTime(void)
{
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&t);
    return (float)((double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart);
}

static void flush(void)
{
    if (!s_quads || !s_dev) return;
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, s_quads * 2, s_vtx, sizeof(UiVtx));
    s_quads = 0;
}

// The font UiSetFont asked for: its tables, and its atlas made in place of the
// one drawn (a missing texture would be no text at all: Inter's is kept then).
static void font_apply(IDirect3DDevice9 *dev)
{
    const int want = s_fontWant;
    IDirect3DTexture9 *old = s_atlas;
    s_fontSet = want;
    if (want <= 0 || want > UI_EXTRA_FONTS)
    {
        s_glyphTab = s_atlasGlyph; s_fontTab = s_atlasFont; s_logoTab = s_atlasLogo;
        s_rle = s_atlasRle; s_rleSize = sizeof(s_atlasRle); s_atlasH = UI_ATLAS_H;
    }
    else
    {
        const UiExtraAtlas *e = &s_extraAtlas[want - 1];
        s_glyphTab = e->glyph; s_fontTab = e->font; s_logoTab = e->logo;
        s_rle = e->rle; s_rleSize = e->rleSize; s_atlasH = e->height;
    }
    s_atlas = NULL;
    if (old)
    {
        dev->SetTexture(0, NULL);
        dev->SetTexture(1, NULL);
        dev->BlockUntilIdle();
        old->Release();
    }
    if (!make_atlas(dev))
    {
        H64_WARN("[ui] font %d: atlas failed, back to Inter", want);
        if (s_atlas) { s_atlas->Release(); s_atlas = NULL; }
        s_fontWant = 0;
        if (want) font_apply(dev);
        return;
    }
    H64_INFO("[ui] font: %s", UiFontName(want));
}

void UiSetFont(int index) { s_fontWant = index >= 0 && index <= UI_EXTRA_FONTS ? index : 0; }

int UiFontCount(void) { return UI_EXTRA_FONTS + 1; }

const char *UiFontName(int index) { return index > 0 && index <= UI_EXTRA_FONTS ? s_extraAtlas[index - 1].name : "Inter"; }

// Pictures released while the GPU may still read them (a frame drawn with them
// not done yet): kept until a fence after their last use has passed. Waiting
// for the GPU at each release (BlockUntilIdle) stalled the browser at every
// cover it let go - many a page with hundreds of games.
struct UiDeadTex { IDirect3DTexture9 *tex; DWORD fence; };
static std::vector<UiDeadTex> s_deadTex;

static void dead_textures_collect(IDirect3DDevice9 *dev, int all)
{
    size_t i = 0;
    while (i < s_deadTex.size())
        if (all || !dev->IsFencePending(s_deadTex[i].fence))
        {
            s_deadTex[i].tex->Release();
            s_deadTex[i] = s_deadTex.back();
            s_deadTex.pop_back();
        }
        else
            i++;
}

void UiBegin(IDirect3DDevice9 *dev)
{
    D3DVIEWPORT9 vp;
    if (!s_ready) s_ready = init_resources(dev);
    s_dev = s_ready > 0 ? dev : NULL;
    s_quads = 0;
    if (!s_dev) return;
    if (s_fontWant != s_fontSet) font_apply(dev);
    if (!s_deadTex.empty()) dead_textures_collect(dev, 0);   // released pictures the GPU is done with
    UiTime();
    vp.X = 0; vp.Y = 0; vp.Width = UI_WIDTH; vp.Height = UI_HEIGHT; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexDeclaration(s_decl);
    dev->SetVertexShader(s_vs);
    dev->SetPixelShader(s_ps);
    // Unit 1 holds the pictures (UiImage); the atlas when none is drawn, never nothing.
    {
        DWORD u;
        for (u = 0; u < 2; u++)
        {
            dev->SetTexture(u, s_atlas);
            dev->SetSamplerState(u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(u, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(u, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            dev->SetSamplerState(u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        }
    }
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
}

// Saved to game:\UI Screenshots\ (made the first time) - separate from the
// game's own game:\screenshots\, since this one captures the menu, not a
// game frame.
static void take_ui_screenshot(void)
{
    char path[96];
    int failed;
    CreateDirectoryA("game:\\UI Screenshots", NULL);
    _snprintf(path, sizeof(path) - 1, "game:\\UI Screenshots\\shot_%08u.bmp", GetTickCount());
    path[sizeof(path) - 1] = 0;
    failed = h64_xenos_save_backbuffer(s_dev, path);
    H64_INFO("[ui] screenshot (L3+R3) %s: %s", path, failed ? "failed" : "saved");
    strncpy(s_shotMsg, Tr(failed ? T_SCREENSHOT_FAILED : T_SCREENSHOT_SAVED), sizeof(s_shotMsg) - 1);
    s_shotMsg[sizeof(s_shotMsg) - 1] = 0;
    s_shotUntil = GetTickCount() + UI_SHOT_MS;
}

static unsigned s_uiFrames;   // UiEnd calls so far (UiFrameCount)

unsigned UiFrameCount(void) { return s_uiFrames; }

void UiEnd(void)
{
    if (!s_dev) return;
    s_uiFrames++;
    if (s_shotMsg[0])
    {
        s32 left = (s32)(s_shotUntil - GetTickCount());
        if (left >= 0) UiToast(s_shotMsg, (float)(UI_SHOT_MS - left), (float)UI_SHOT_MS);
        else s_shotMsg[0] = 0;
    }
    // Everything this frame's UiText/UiPanel/... calls queued (s_vtx) only
    // reaches the GPU here - UiImage flushes its own quad right away (it
    // has to, to swap the bound texture), which is why backgrounds/covers
    // showed up in a capture taken before this flush() but menu panels and
    // text (never flushed mid-frame) did not.
    flush();
    if (s_shotPending) { s_shotPending = 0; take_ui_screenshot(); }
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev = NULL;
}

// ---- Quads ----
// UiSetScale: what is drawn next is scaled by k around (cx, cy) - the shapes keep
// their look (their edges are worked out in their own units).
static float s_scaleX, s_scaleY, s_scaleK = 1.0f;

void UiSetScale(float cx, float cy, float k)
{
    s_scaleX = cx;
    s_scaleY = cy;
    s_scaleK = k > 0.0f ? k : 1.0f;
}

static void vtx(UiVtx *v, float x, float y, D3DCOLOR c, float t0, float t1, float t2, float t3, const float *p)
{
    if (s_scaleK != 1.0f)
    {
        x = s_scaleX + (x - s_scaleX) * s_scaleK;
        y = s_scaleY + (y - s_scaleY) * s_scaleK;
    }
    v->x = x * (2.0f / UI_WIDTH) - 1.0f;
    v->y = 1.0f - y * (2.0f / UI_HEIGHT);
    v->color = c;
    v->t[0] = t0; v->t[1] = t1; v->t[2] = t2; v->t[3] = t3;
    v->p[0] = p[0]; v->p[1] = p[1]; v->p[2] = p[2]; v->p[3] = p[3];
}

// Corners (x0, y0)-(x1, y1) with t.xy from (u0, v0) to (u1, v1); top colour ct, bottom cb.
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float mode, float ring,
                 const float *p, D3DCOLOR ct, D3DCOLOR cb)
{
    UiVtx *v;
    if (!s_dev) return;
    if (s_quads == UI_MAX_QUADS) flush();
    v = &s_vtx[s_quads * 6];
    vtx(&v[0], x0, y0, ct, u0, v0, mode, ring, p);
    vtx(&v[1], x1, y0, ct, u1, v0, mode, ring, p);
    vtx(&v[2], x0, y1, cb, u0, v1, mode, ring, p);
    v[3] = v[1];
    vtx(&v[4], x1, y1, cb, u1, v1, mode, ring, p);
    v[5] = v[2];
    s_quads++;
}

// A rounded box (x, y, w, h), radius r, edge width soft; ring: half the ring's width (0: filled).
static void shape(float x, float y, float w, float h, float r, float soft, float ring, D3DCOLOR ct, D3DCOLOR cb)
{
    float p[4], pad = soft + 1.0f, hw = w * 0.5f, hh = h * 0.5f;
    if (w <= 0 || h <= 0) return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    p[0] = hw; p[1] = hh; p[2] = r; p[3] = soft;   // a ring lies inside the edge (the shader offsets it)
    quad(x - pad, y - pad, x + w + pad, y + h + pad, -hw - pad, -hh - pad, hw + pad, hh + pad, 0.0f, ring, p, ct, cb);
}

static D3DCOLOR lerp_color(D3DCOLOR a, D3DCOLOR b, float t)
{
    u32 r = 0;
    int s;
    for (s = 0; s < 32; s += 8)
    {
        float ca = (float)((a >> s) & 0xFF), cbv = (float)((b >> s) & 0xFF);
        float v = ca + (cbv - ca) * t + 0.5f;
        r |= (u32)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v) << s;
    }
    return r;
}

void UiRoundRect(float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom)
{
    // The quad is padded for the anti-aliased edge: extend the gradient to the padded corners.
    float k = h > 0 ? 2.0f / h : 0.0f;
    shape(x, y, w, h, r, 1.0f, 0.0f, lerp_color(top, bottom, -k), lerp_color(top, bottom, 1.0f + k));
}

void UiRect(float x, float y, float w, float h, D3DCOLOR color) { shape(x, y, w, h, 0.0f, 1.0f, 0.0f, color, color); }

void UiRoundRing(float x, float y, float w, float h, float r, float thick, D3DCOLOR color)
{
    shape(x, y, w, h, r, 1.0f, thick * 0.5f, color, color);
}

void UiShadow(float x, float y, float w, float h, float r, float blur, D3DCOLOR color)
{
    shape(x, y, w, h, r + blur * 0.5f, blur, 0.0f, color, color);
}

void UiCircle(float cx, float cy, float r, D3DCOLOR color) { shape(cx - r, cy - r, r * 2, r * 2, r, 1.0f, 0.0f, color, color); }

static D3DCOLOR with_alpha(D3DCOLOR c, float a)
{
    float v = (float)(c >> 24) * a;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (c & 0x00FFFFFF) | ((u32)(v + 0.5f) << 24);
}

// ---- Skins (background.png override; the logo is always LAIKA 64's) ----
static char s_skinWant[64];
static char s_skinLoaded[64];
static int s_skinAttempted;
static IDirect3DTexture9 *s_skinBg;

void UiSetSkinWant(const char *name)
{
    strncpy(s_skinWant, name ? name : "", sizeof(s_skinWant) - 1);
    s_skinWant[sizeof(s_skinWant) - 1] = 0;
}

static unsigned char *skin_file_read(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*size <= 0 || *size > 16 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)*size);
    if (!buf || fread(buf, 1, (size_t)*size, f) != (size_t)*size) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

static IDirect3DTexture9 *skin_image_load(const char *skinName, const char *file, u32 maxPixels)
{
    char path[160];
    unsigned char *enc, *rgba;
    long encSize = 0;
    int w = 0, h = 0;
    IDirect3DTexture9 *tex;
    _snprintf(path, sizeof(path) - 1, "game:\\skins\\%s\\%s", skinName, file);
    path[sizeof(path) - 1] = 0;
    enc = skin_file_read(path, &encSize);
    if (!enc) return NULL;
    rgba = h64_png_decode(enc, (u32)encSize, &w, &h, maxPixels);
    free(enc);
    if (!rgba) return NULL;
    tex = UiImageCreate(s_dev, rgba, w, h);
    free(rgba);
    return tex;
}

// Lazily (re)loads the skin UiSetSkinWant last named, the first time
// UiBackdrop runs after it changed. A device must be bound (UiBegin
// already ran this frame) - if not, nothing is marked done, so the next
// frame tries again instead of being stuck unthemed.
static void skin_sync(void)
{
    if (!s_dev) return;
    if (s_skinAttempted && !strcmp(s_skinWant, s_skinLoaded)) return;
    if (s_skinBg) { UiImageRelease(s_dev, s_skinBg); s_skinBg = NULL; }
    if (s_skinWant[0]) s_skinBg = skin_image_load(s_skinWant, "background.png", UI_WIDTH * UI_HEIGHT);
    strncpy(s_skinLoaded, s_skinWant, sizeof(s_skinLoaded) - 1);
    s_skinLoaded[sizeof(s_skinLoaded) - 1] = 0;
    s_skinAttempted = 1;
}

// ---- Smooth lines and slanted strips (the background effects' waves) ----
// A segment of `thick` pixels: the shape's box laid along it, so its edges are
// anti-aliased whatever the angle (flat slices showed steps on slopes). Its
// ends are square and soft, half a pixel over: the segments of a polyline
// meet without a gap or a brighter bead.
void UiLine(float x0, float y0, float x1, float y1, float thick, D3DCOLOR color)
{
    UiVtx *v;
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy), ht = thick * 0.5f, hw, hl, p[4], cx, cy, ux, uy, nx, ny;
    if (!s_dev || thick <= 0) return;
    if (len < 0.001f) { dx = 1; dy = 0; len = 0; }
    else { dx /= len; dy /= len; }
    if (s_quads == UI_MAX_QUADS) flush();
    hw = len * 0.5f + 0.5f;   // the box: the segment
    hl = hw + 1.0f;           // padded by the edge's width
    p[0] = hw; p[1] = ht; p[2] = 0.0f; p[3] = 1.0f;
    cx = (x0 + x1) * 0.5f; cy = (y0 + y1) * 0.5f;
    ux = dx * hl; uy = dy * hl;                    // along, to the padded end
    nx = -dy * (ht + 1.0f); ny = dx * (ht + 1.0f); // across, to the padded side
    v = &s_vtx[s_quads * 6];
    vtx(&v[0], cx - ux - nx, cy - uy - ny, color, -hl, -(ht + 1.0f), 0.0f, 0.0f, p);
    vtx(&v[1], cx + ux - nx, cy + uy - ny, color, hl, -(ht + 1.0f), 0.0f, 0.0f, p);
    vtx(&v[2], cx - ux + nx, cy - uy + ny, color, -hl, ht + 1.0f, 0.0f, 0.0f, p);
    v[3] = v[1];
    vtx(&v[4], cx + ux + nx, cy + uy + ny, color, hl, ht + 1.0f, 0.0f, 0.0f, p);
    v[5] = v[2];
    s_quads++;
}

// A line through n points as one ribbon: each piece's ends share the corners
// of the next (no overlap: lines of UiLine pieces doubled their alpha at each
// joint, bright knots every few pixels along the PS3/PS4 effects' curves, a
// saw-tooth), its edges fading over `soft` pixels. A loop (the last point on
// the first: the topography's contours) closes with no seam.
void UiPolyline(const float *xy, int n, float thick, float soft, D3DCOLOR color)
{
    float ht = thick * 0.5f, e, p[4], nrm[2][2];
    int i, k, closed;
    UiVtx *v;
    if (!s_dev || n < 2 || thick <= 0) return;
    if (soft < 0.5f) soft = 0.5f;
    e = ht + soft;
    p[0] = 1.0e5f; p[1] = ht; p[2] = 0.0f; p[3] = soft;   // no ends: across only
    closed = n > 3 && fabsf(xy[0] - xy[2 * n - 2]) < 0.01f && fabsf(xy[1] - xy[2 * n - 1]) < 0.01f;
    for (i = 0; i < n - 1; i++)
    {
        for (k = 0; k < 2; k++)
        {
            // The normal at point i + k, from its neighbours (at an end: its one neighbour, or round a loop).
            int a = i + k - 1, b = i + k + 1;
            if (a < 0) a = closed ? n - 2 : 0;
            if (b > n - 1) b = closed ? 1 : n - 1;
            float tx = xy[2 * b] - xy[2 * a], ty = xy[2 * b + 1] - xy[2 * a + 1], l = sqrtf(tx * tx + ty * ty);
            if (l < 0.001f) { tx = 1; ty = 0; l = 1; }
            nrm[k][0] = -ty / l;
            nrm[k][1] = tx / l;
        }
        if (s_quads == UI_MAX_QUADS) flush();
        v = &s_vtx[s_quads * 6];
        vtx(&v[0], xy[2 * i] - nrm[0][0] * e, xy[2 * i + 1] - nrm[0][1] * e, color, 0.0f, -e, 0.0f, 0.0f, p);
        vtx(&v[1], xy[2 * i + 2] - nrm[1][0] * e, xy[2 * i + 3] - nrm[1][1] * e, color, 0.0f, -e, 0.0f, 0.0f, p);
        vtx(&v[2], xy[2 * i] + nrm[0][0] * e, xy[2 * i + 1] + nrm[0][1] * e, color, 0.0f, e, 0.0f, 0.0f, p);
        v[3] = v[1];
        vtx(&v[4], xy[2 * i + 2] + nrm[1][0] * e, xy[2 * i + 3] + nrm[1][1] * e, color, 0.0f, e, 0.0f, 0.0f, p);
        v[5] = v[2];
        s_quads++;
    }
}

// A strip between x0 and x1 whose top and bottom follow the two given heights
// at each end (a slanted slice of a wave): top colour ct, bottom cb.
static void strip_quad(float x0, float top0, float bot0, float x1, float top1, float bot1, D3DCOLOR ct, D3DCOLOR cb)
{
    static const float p[4] = { 1.0e5f, 1.0e5f, 0.0f, 1.0f };   // a box far bigger than the strip: solid
    UiVtx *v;
    if (s_quads == UI_MAX_QUADS) flush();
    v = &s_vtx[s_quads * 6];
    vtx(&v[0], x0, top0, ct, 0, 0, 0, 0, p);
    vtx(&v[1], x1, top1, ct, 0, 0, 0, 0, p);
    vtx(&v[2], x0, bot0, cb, 0, 0, 0, 0, p);
    v[3] = v[1];
    vtx(&v[4], x1, bot1, cb, 0, 0, 0, 0, p);
    v[5] = v[2];
    s_quads++;
}

// A slice of a band (the background effects' ribbons, aurora, silk...): its top
// and bottom edges soft, as UiLine's are - the shader's box, half as high as the
// slice, across a quad a pixel taller on each side (drawn solid, the slanted
// edges stepped: jagged). Three quads with faded colours left dark specks.
void UiStrip(float x0, float top0, float bot0, float x1, float top1, float bot1, D3DCOLOR ct, D3DCOLOR cb)
{
    float hh = ((bot0 - top0) + (bot1 - top1)) * 0.25f, p[4];
    UiVtx *v;
    if (!s_dev) return;
    if (hh < 1.0f) { strip_quad(x0, top0, bot0, x1, top1, bot1, ct, cb); return; }
    if (s_quads == UI_MAX_QUADS) flush();
    p[0] = 1.0e5f; p[1] = hh; p[2] = 0.0f; p[3] = 1.0f;   // no side edges; top and bottom at +-hh
    v = &s_vtx[s_quads * 6];
    vtx(&v[0], x0, top0 - 1.0f, ct, 0.0f, -(hh + 1.0f), 0.0f, 0.0f, p);
    vtx(&v[1], x1, top1 - 1.0f, ct, 0.0f, -(hh + 1.0f), 0.0f, 0.0f, p);
    vtx(&v[2], x0, bot0 + 1.0f, cb, 0.0f, hh + 1.0f, 0.0f, 0.0f, p);
    v[3] = v[1];
    vtx(&v[4], x1, bot1 + 1.0f, cb, 0.0f, hh + 1.0f, 0.0f, 0.0f, p);
    v[5] = v[2];
    s_quads++;
}

// ---- Wallpapers (game:\wallpapers\: a slideshow behind the menus) ----
// The .jpg/.jpeg/.png/.bmp in game:\wallpapers\ and its folders (one level
// down), in name order (or at random), one at a
// time, laid out as chosen (fill, fit, stretch, centre, tile), the next one
// coming in with the chosen transition. Decoded with D3DX (JPEG too), PNG
// through h64_png when D3DX cannot. Only two pictures are held: the one
// shown and, during a transition, the one leaving.
#define WALL_MAX 256
#define WALL_TRANS_S 1.2f
#define WALL_TILE_MIN 48.0f   // a smaller picture is tiled at this size (fewer draws)
struct WallPic
{
    IDirect3DTexture9 *tex;
    float w, h;   // the picture's own size in pixels (the texture may be smaller)
};
static char s_wallWant[128], s_wallLoaded[128];   // the wallpapers' folder ("": none)
static int s_wallSecs = 30, s_wallTrans, s_wallDim = 40, s_wallFit, s_wallShuffle, s_wallScanned;   // s_wallSecs 0: never changes
static char s_wallPick[128], s_wallPicked[128];   // the picture chosen to show (its name in s_wallFiles), and the one last applied
static char s_wallFiles[WALL_MAX][128];   // relative to the folder: "pic.jpg", "folder\pic.jpg"
static int s_wallCount, s_wallIdx;
static WallPic s_wallCur, s_wallPrev;
static float s_wallShownAt, s_wallTransAt = -100.0f;
static int s_wallTransKind, s_wallLoadedOnce;

static void wall_scan(void);
// The list of pictures again when the choice changed (or never read): no device needed.
static void wall_sync_scan(void)
{
    if (s_wallScanned && !strcmp(s_wallWant, s_wallLoaded)) return;
    strcpy(s_wallLoaded, s_wallWant);
    s_wallScanned = 1;
    s_wallLoadedOnce = 0;   // the pictures follow at the next draw
    wall_scan();
}

void UiWallpaperRescan(void) { s_wallScanned = 0; }

static int s_wallOn;   // UiSetWallpapers: shown (else the folder is only read, for Settings to choose from)

void UiSetWallpapers(const char *folder, int on, const char *pick, int seconds, int transition, int dim, int fit, int shuffle)
{
    s_wallOn = on;
    strncpy(s_wallWant, folder ? folder : "", sizeof(s_wallWant) - 1);
    s_wallWant[sizeof(s_wallWant) - 1] = 0;
    strncpy(s_wallPick, pick ? pick : "", sizeof(s_wallPick) - 1);
    s_wallPick[sizeof(s_wallPick) - 1] = 0;
    s_wallSecs = seconds <= 0 ? 0 : seconds < 5 ? 5 : seconds;
    s_wallTrans = transition;
    s_wallDim = dim < 0 ? 0 : dim > 90 ? 90 : dim;
    s_wallFit = fit >= 0 && fit < UI_FIT_COUNT ? fit : UI_FIT_FILL;
    s_wallShuffle = shuffle;
}

// <the wallpapers' folder>\<file> (file may hold a folder: "folder\pic.jpg").
static void wall_path(const char *file, char *out, int size)
{
    _snprintf(out, size - 1, "%s\\%s", s_wallWant, file);
    out[size - 1] = 0;
}

int UiWallpaperIsImage(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!_stricmp(dot, ".jpg") || !_stricmp(dot, ".jpeg") || !_stricmp(dot, ".png") || !_stricmp(dot, ".bmp"));
}

static int wall_cmp(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }

// The pictures of one folder (prefix: its path under the wallpapers folder, "" for that folder itself).
static void wall_scan_dir(const char *prefix, int subfolders)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[200];
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\%s*", s_wallWant, prefix);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            char sub[128];
            if (!subfolders || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            _snprintf(sub, sizeof(sub) - 1, "%s%s\\", prefix, fd.cFileName);
            sub[sizeof(sub) - 1] = 0;
            wall_scan_dir(sub, 0);
            continue;
        }
        if (!UiWallpaperIsImage(fd.cFileName) || strlen(prefix) + strlen(fd.cFileName) >= sizeof(s_wallFiles[0])) continue;
        if (s_wallCount >= WALL_MAX) break;
        _snprintf(s_wallFiles[s_wallCount], sizeof(s_wallFiles[0]) - 1, "%s%s", prefix, fd.cFileName);
        s_wallFiles[s_wallCount][sizeof(s_wallFiles[0]) - 1] = 0;
        s_wallCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void wall_scan(void)
{
    s_wallCount = 0;
    wall_scan_dir("", 1);   // also while they are off: Settings chooses among them
    qsort(s_wallFiles, s_wallCount, sizeof(s_wallFiles[0]), wall_cmp);
    H64_INFO("[ui] wallpapers: %d pictures in %s", s_wallCount, s_wallWant);
}

int UiWallpaperCount(void)
{
    wall_sync_scan();
    return s_wallCount;
}

static void wall_free(WallPic *p)
{
    if (p->tex)
    {
        flush();
        UiImageRelease(s_dev, p->tex);
    }
    memset(p, 0, sizeof(*p));
}

// A picture's pixels (RGBA), no bigger than it takes to cover the screen (a
// large photo scaled down by averaging, a small picture kept as it is), and
// its own size for the layouts. CPU only: the reading thread runs it.
struct WallPixels
{
    unsigned char *rgba;   // malloc'ed
    int w, h;
    float ow, oh;
};

static int wall_pixels(const unsigned char *enc, long size, WallPixels *px)
{
    int w = 0, h = 0, n = 0, tw, th, x, y;
    float s;
    unsigned char *img, *dst;
    memset(px, 0, sizeof(*px));
    if (!enc || size <= 0) return 0;
    img = stbi_load_from_memory(enc, (int)size, &w, &h, &n, 4);
    if (!img) return 0;
    px->ow = (float)w;
    px->oh = (float)h;
    s = (float)UI_WIDTH / w;
    if ((float)UI_HEIGHT / h > s) s = (float)UI_HEIGHT / h;
    tw = (int)(w * s + 0.999f);
    th = (int)(h * s + 0.999f);
    if (tw > 4096) tw = 4096;   // a panorama: squeezed a little more
    if (th > 4096) th = 4096;
    if (s >= 1.0f || (tw >= w && th >= h))
    {
        // Small enough already.
        px->rgba = (unsigned char *)malloc((size_t)w * h * 4);
        if (px->rgba) memcpy(px->rgba, img, (size_t)w * h * 4);
        px->w = w;
        px->h = h;
        stbi_image_free(img);
        return px->rgba != NULL;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    dst = (unsigned char *)malloc((size_t)tw * th * 4);
    if (!dst) { stbi_image_free(img); return 0; }
    for (y = 0; y < th; y++)
    {
        // Each pixel: the average of the source pixels it covers.
        int sy0 = (int)((long long)y * h / th), sy1 = (int)((long long)(y + 1) * h / th);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < tw; x++)
        {
            int sx0 = (int)((long long)x * w / tw), sx1 = (int)((long long)(x + 1) * w / tw), sx, sy, c;
            unsigned sum[4] = { 0, 0, 0, 0 }, cnt;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (sy = sy0; sy < sy1; sy++)
            {
                const unsigned char *p = img + ((size_t)sy * w + sx0) * 4;
                for (sx = sx0; sx < sx1; sx++, p += 4)
                {
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; sum[3] += p[3];
                }
            }
            cnt = (unsigned)((sy1 - sy0) * (sx1 - sx0));
            for (c = 0; c < 4; c++) dst[((size_t)y * tw + x) * 4 + c] = (unsigned char)(sum[c] / cnt);
        }
    }
    stbi_image_free(img);
    px->rgba = dst;
    px->w = tw;
    px->h = th;
    return 1;
}

// A texture from decoded pixels (freed).
static int wall_from_pixels(WallPixels *px, WallPic *out)
{
    IDirect3DTexture9 *tex = px->rgba ? UiImageCreate(s_dev, px->rgba, px->w, px->h) : NULL;
    free(px->rgba);
    px->rgba = NULL;
    if (!tex) return 0;
    out->tex = tex;
    out->w = px->ow;
    out->h = px->oh;
    return 1;
}

// One picture from its file's bytes (stb_image, else D3DX, else h64_png). Takes `enc` (freed).
static int wall_decode(unsigned char *enc, long size, const char *path, WallPic *out)
{
    D3DXIMAGE_INFO info;
    IDirect3DTexture9 *tex = NULL;
    WallPixels px;
    memset(out, 0, sizeof(*out));
    if (!enc) return 0;
    if (wall_pixels(enc, size, &px) && wall_from_pixels(&px, out))
    {
        free(enc);
        H64_INFO("[ui] wallpaper %s: %dx%d", path, (int)out->w, (int)out->h);
        return 1;
    }
    if (SUCCEEDED(D3DXGetImageInfoFromFileInMemory(enc, (UINT)size, &info)) && info.Width && info.Height)
    {
        float s = (float)UI_WIDTH / info.Width;
        int w, h;
        if ((float)UI_HEIGHT / info.Height > s) s = (float)UI_HEIGHT / info.Height;
        if (s > 1.0f) s = 1.0f;
        w = (int)(info.Width * s + 0.5f);
        h = (int)(info.Height * s + 0.5f);
        if (w > 4096) w = 4096;   // a panorama: squeezed a little more
        if (h > 4096) h = 4096;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        if (FAILED(D3DXCreateTextureFromFileInMemoryEx(s_dev, enc, (UINT)size, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                       D3DX_FILTER_LINEAR, D3DX_DEFAULT, 0, NULL, NULL, &tex)))
            tex = NULL;
        out->w = (float)info.Width;
        out->h = (float)info.Height;
    }
    if (!tex)
    {
        int pw = 0, ph = 0;
        unsigned char *rgba = h64_png_decode(enc, (u32)size, &pw, &ph, 2048 * 2048);
        if (rgba)
        {
            tex = UiImageCreate(s_dev, rgba, pw, ph);
            free(rgba);
            out->w = (float)pw;
            out->h = (float)ph;
        }
    }
    free(enc);
    if (!tex)
    {
        H64_WARN("[ui] wallpaper %s: cannot decode (%ld bytes)", path, size);
        return 0;
    }
    H64_INFO("[ui] wallpaper %s: %dx%d", path, (int)out->w, (int)out->h);
    out->tex = tex;
    return 1;
}

static int wall_load(int idx, WallPic *out)
{
    char path[200];
    long size = 0;
    unsigned char *enc;
    memset(out, 0, sizeof(*out));
    if (idx < 0 || idx >= s_wallCount) return 0;
    wall_path(s_wallFiles[idx], path, sizeof(path));
    enc = skin_file_read(path, &size);   // first: it sets size (argument order is unspecified)
    return wall_decode(enc, size, path, out);
}

// ---- The next picture, made ready ahead ----
// While one picture shows, the next one's file is read on a thread of its own
// (the USB drive is the slow part), then decoded here a little later, between
// two frames: when its time comes the transition starts at once, with no
// pause in the middle of the menus.
static WallPic s_wallNext;           // made ready (tex NULL: none)
static int s_wallNextIdx = -1;       // its place in the list
static volatile LONG s_wallReadState;   // 0 idle, 1 reading, 2 read (s_wallReadPx: decoded, else s_wallReadBuf: its bytes, or neither)
static int s_wallReadIdx, s_wallGen, s_wallReadGen;   // the generation: the list changed since the read began -> dropped
static char s_wallReadPath[200];
static unsigned char *s_wallReadBuf;
static long s_wallReadSize;
static WallPixels s_wallReadPx;

static DWORD WINAPI wall_read_thread(LPVOID)
{
    long size = 0;
    unsigned char *buf = skin_file_read(s_wallReadPath, &size);
    // Decoded here too (the slow part after the reading): the main thread only makes the texture.
    if (buf && wall_pixels(buf, size, &s_wallReadPx)) { free(buf); buf = NULL; }
    s_wallReadBuf = buf;
    s_wallReadSize = size;
    InterlockedExchange(&s_wallReadState, 2);
    return 0;
}

// The pictures ahead dropped (the list or the choice changed): a read under way finishes and is let go.
static void wall_drop_next(void)
{
    wall_free(&s_wallNext);
    s_wallNextIdx = -1;
    s_wallGen++;
}

static void wall_prefetch(int idx)
{
    HANDLE th;
    if (s_wallReadState != 0 || idx < 0 || idx >= s_wallCount) return;
    wall_path(s_wallFiles[idx], s_wallReadPath, sizeof(s_wallReadPath));
    s_wallReadIdx = idx;
    s_wallReadGen = s_wallGen;
    s_wallReadBuf = NULL;
    memset(&s_wallReadPx, 0, sizeof(s_wallReadPx));
    InterlockedExchange(&s_wallReadState, 1);
    th = CreateThread(NULL, 0, wall_read_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else InterlockedExchange(&s_wallReadState, 0);   // no thread: read when the time comes instead
}

// `pic` (the picture at idx) in, the one shown now out with the transition.
static void wall_show(WallPic *pic, int idx, float now)
{
    wall_free(&s_wallPrev);
    s_wallPrev = s_wallCur;
    s_wallCur = *pic;
    memset(pic, 0, sizeof(*pic));
    s_wallIdx = idx;
    s_wallTransAt = s_wallPrev.tex ? now : -100.0f;
    s_wallTransKind = s_wallTrans < UI_TRANS_RANDOM ? s_wallTrans : (int)(GetTickCount() / 7 % UI_TRANS_RANDOM);
    s_wallShownAt = now;
}

// The next picture that loads (one that cannot is skipped), from idx on.
static void wall_next(int idx, float now)
{
    int tries;
    WallPic pic;
    for (tries = 0; tries < s_wallCount; tries++)
    {
        int i = (idx + tries) % s_wallCount;
        if (i == s_wallIdx && s_wallCur.tex && tries) break;   // round to the one shown: keep it
        if (wall_load(i, &pic))
        {
            wall_show(&pic, i, now);
            return;
        }
    }
    s_wallShownAt = now;
}

// A picture's place in the list by its name (0 when it is not there any more).
static int wall_find(const char *name)
{
    int i;
    for (i = 0; i < s_wallCount; i++)
        if (!_stricmp(s_wallFiles[i], name)) return i;
    return 0;
}

int UiWallpaperFind(const char *name)
{
    int i;
    wall_sync_scan();
    for (i = 0; i < s_wallCount; i++)
        if (!_stricmp(s_wallFiles[i], name)) return i;
    return -1;
}

const char *UiWallpaperName(int i)
{
    wall_sync_scan();
    return i >= 0 && i < s_wallCount ? s_wallFiles[i] : "";
}

// The one after s_wallIdx: the next by name, or any other at random.
static int wall_after(void)
{
    if (!s_wallShuffle || s_wallCount < 3) return (s_wallIdx + 1) % s_wallCount;
    return (s_wallIdx + 1 + (int)((GetTickCount() * 2654435761u) >> 8) % (s_wallCount - 1)) % s_wallCount;
}

static void wall_sync(void)
{
    float now = UiTime();
    if (!s_dev) return;
    if (s_wallReadState == 2)
    {
        // The next picture is in (decoded on the thread; still a while before it is wanted): its texture now, or let go.
        unsigned char *buf = s_wallReadBuf;
        WallPixels px = s_wallReadPx;
        int idx = s_wallReadIdx, fresh = s_wallReadGen == s_wallGen && s_wallWant[0] && s_wallOn;
        s_wallReadBuf = NULL;
        memset(&s_wallReadPx, 0, sizeof(s_wallReadPx));
        InterlockedExchange(&s_wallReadState, 0);
        if (!fresh) { free(buf); free(px.rgba); return; }
        if (px.rgba ? wall_from_pixels(&px, &s_wallNext) : wall_decode(buf, s_wallReadSize, s_wallReadPath, &s_wallNext)) s_wallNextIdx = idx;
        return;   // one slow thing a frame
    }
    if (!s_wallWant[0] || !s_wallOn)
    {
        // Off: no picture held.
        wall_free(&s_wallPrev);
        wall_free(&s_wallCur);
        wall_drop_next();
        wall_sync_scan();
        return;
    }
    if (!s_wallScanned || strcmp(s_wallWant, s_wallLoaded) || !s_wallLoadedOnce)
    {
        wall_free(&s_wallPrev);
        wall_free(&s_wallCur);
        wall_drop_next();
        wall_sync_scan();
        s_wallLoadedOnce = 1;
        s_wallIdx = 0;
        strcpy(s_wallPicked, s_wallPick);
        // The chosen picture first (or, none chosen, the first or any one).
        if (s_wallCount) wall_next(s_wallPick[0] ? wall_find(s_wallPick) : s_wallShuffle ? (int)(GetTickCount() % s_wallCount) : 0, now);
        s_wallTransAt = -100.0f;
        return;
    }
    if (s_wallPrev.tex && now - s_wallTransAt > WALL_TRANS_S) wall_free(&s_wallPrev);   // the transition is over
    if (strcmp(s_wallPick, s_wallPicked))
    {
        // Another picture chosen (Settings > Interface > Wallpapers): it comes in at once, with the transition.
        int i = wall_find(s_wallPick);
        strcpy(s_wallPicked, s_wallPick);
        if (s_wallCount && i != s_wallIdx)
        {
            if (s_wallNext.tex && s_wallNextIdx == i) wall_show(&s_wallNext, i, now);   // the one made ready
            else wall_next(i, now);
        }
        wall_drop_next();   // what was made ready followed the old order
        return;
    }
    if (!s_wallSecs || s_wallCount < 2 || !s_wallCur.tex) return;
    if (now - s_wallShownAt >= s_wallSecs)
    {
        // Its time: the one made ready, else read now.
        if (s_wallNext.tex)
        {
            int i = s_wallNextIdx;
            s_wallNextIdx = -1;
            wall_show(&s_wallNext, i, now);
        }
        else wall_next(wall_after(), now);
        return;
    }
    // Half a second after the transition, the next one is read in the background.
    if (!s_wallPrev.tex && !s_wallNext.tex && s_wallReadState == 0 && now - s_wallShownAt > WALL_TRANS_S + 0.5f) wall_prefetch(wall_after());
}

// Part (u0..u1, v0..v1 of the whole picture, 0..1) of a picture over the box (x, y, w, h).
void UiImagePart(IDirect3DTexture9 *tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, D3DCOLOR color)
{
    float p[4], fw, fh;
    if (!s_dev || !tex || w <= 0 || h <= 0 || u1 <= u0 || v1 <= v0) return;
    // The whole picture's size at this scale: the shader maps its box to the texture.
    fw = w / (u1 - u0);
    fh = h / (v1 - v0);
    flush();
    s_dev->SetTexture(1, tex);
    p[0] = fw * 0.5f; p[1] = fh * 0.5f; p[2] = 0; p[3] = 1.0f;
    quad(x, y, x + w, y + h, (u0 - 0.5f) * fw, (v0 - 0.5f) * fh, (u1 - 0.5f) * fw, (v1 - 0.5f) * fh, 2.0f, 0.0f, p, color, color);
    flush();
    s_dev->SetTexture(1, s_atlas);
}

// Part (u0..u1, v0..v1) of a picture over any four corners - top left, top
// right, bottom left, bottom right, xy[8] - (a box's faces seen at an angle).
// Two triangles: the picture bends a little along their diagonal.
void UiImageQuad(IDirect3DTexture9 *tex, const float *xy, float u0, float v0, float u1, float v1, D3DCOLOR color)
{
    // The shader's box is the unit square around 0 (t = u - 0.5): inside it, solid; its edge soft by ~a pixel.
    const float p[4] = { 0.5f, 0.5f, 0.0f, 0.004f };
    UiVtx *v;
    if (!s_dev || !tex) return;
    flush();
    s_dev->SetTexture(1, tex);
    v = &s_vtx[0];
    vtx(&v[0], xy[0], xy[1], color, u0 - 0.5f, v0 - 0.5f, 2.0f, 0.0f, p);
    vtx(&v[1], xy[2], xy[3], color, u1 - 0.5f, v0 - 0.5f, 2.0f, 0.0f, p);
    vtx(&v[2], xy[4], xy[5], color, u0 - 0.5f, v1 - 0.5f, 2.0f, 0.0f, p);
    v[3] = v[1];
    vtx(&v[4], xy[6], xy[7], color, u1 - 0.5f, v1 - 0.5f, 2.0f, 0.0f, p);
    v[5] = v[2];
    s_quads = 1;
    flush();
    s_dev->SetTexture(1, s_atlas);
}

// The picture drawn as one rectangle (x, y, w, h) of the screen, only where
// it lies inside the clip (cx0, cy0)-(cx1, cy1).
static void wall_rect(IDirect3DTexture9 *tex, float x, float y, float w, float h, float cx0, float cy0, float cx1, float cy1, D3DCOLOR c)
{
    float ix0 = x > cx0 ? x : cx0, iy0 = y > cy0 ? y : cy0, ix1 = x + w < cx1 ? x + w : cx1, iy1 = y + h < cy1 ? y + h : cy1;
    if (ix1 - ix0 < 0.5f || iy1 - iy0 < 0.5f) return;
    UiImagePart(tex, ix0, iy0, ix1 - ix0, iy1 - iy0, (ix0 - x) / w, (iy0 - y) / h, (ix1 - x) / w, (iy1 - y) / h, c);
}

// A wallpaper laid out (fit: UI_FIT_*) over the area (ax, ay, aw, ah) - the
// screen, or a thumbnail of it at the same proportions - zoomed by `zoom`
// around its middle, moved right by dx, drawn only inside the clip.
static void wall_layout(const WallPic *p, int fit, float ax, float ay, float aw, float ah, float cx0, float cy0, float cx1, float cy1,
                        float dx, float zoom, float alpha)
{
    const float k = aw / UI_WIDTH;   // the area's pixels per screen pixel (a picture's own size scales with it)
    const float mx = ax + aw * 0.5f, my = ay + ah * 0.5f;
    D3DCOLOR c = D3DCOLOR_ARGB((int)(255 * (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha)), 255, 255, 255);
    float w, h;
    if (!p->tex || p->w <= 0 || p->h <= 0) return;
    if (fit == UI_FIT_TILE)
    {
        float tw = p->w * k * zoom, th = p->h * k * zoom, x0, y0, x, y, minT = WALL_TILE_MIN * k;
        if (tw < minT) { th *= minT / tw; tw = minT; }
        if (th < minT) { tw *= minT / th; th = minT; }
        // From a tile centred on the area's middle out to its edges.
        x0 = mx - tw * 0.5f - ceilf((mx - tw * 0.5f - ax) / tw) * tw + dx;
        y0 = my - th * 0.5f - ceilf((my - th * 0.5f - ay) / th) * th;
        for (y = y0; y < ay + ah; y += th)
            for (x = x0; x < ax + aw + dx; x += tw) wall_rect(p->tex, x, y, tw, th, cx0, cy0, cx1, cy1, c);
        return;
    }
    if (fit == UI_FIT_STRETCH) { w = aw; h = ah; }
    else if (fit == UI_FIT_CENTER) { w = p->w * k; h = p->h * k; }
    else
    {
        float s = aw / p->w, t = ah / p->h;
        s = fit == UI_FIT_FIT ? (s < t ? s : t) : (s > t ? s : t);
        w = p->w * s;
        h = p->h * s;
    }
    w *= zoom;
    h *= zoom;
    wall_rect(p->tex, mx - w * 0.5f + dx, my - h * 0.5f, w, h, cx0, cy0, cx1, cy1, c);
}

static float wall_ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }

// The pictures (the one leaving, the one coming) over an area, the transition `k` (0..1) between them.
static void wall_draw_area(float ax, float ay, float aw, float ah, int fit, float zoom)
{
    const float now = UiTime(), x1 = ax + aw, y1 = ay + ah;
    float k = s_wallPrev.tex ? wall_ease((now - s_wallTransAt) / WALL_TRANS_S) : 1.0f;
    if (k >= 1.0f)
    {
        wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, ay, x1, y1, 0, zoom, 1.0f);
        return;
    }
    switch (s_wallTransKind)
    {
    case UI_TRANS_SLIDE:   // the new one pushes the old one out to the left
        wall_layout(&s_wallPrev, fit, ax, ay, aw, ah, ax, ay, x1, y1, -k * aw, 1.0f, 1.0f);
        wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, ay, x1, y1, (1 - k) * aw, zoom, 1.0f);
        break;
    case UI_TRANS_ZOOM:    // the new one comes forward from close up, fading in
        wall_layout(&s_wallPrev, fit, ax, ay, aw, ah, ax, ay, x1, y1, 0, 1.0f + 0.1f * k, 1.0f);
        wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, ay, x1, y1, 0, (1.3f - 0.3f * k) * zoom, k);
        break;
    case UI_TRANS_WIPE:    // a bright edge sweeps across, the new one behind it
    {
        float ex = ax + k * aw;
        wall_layout(&s_wallPrev, fit, ax, ay, aw, ah, ex, ay, x1, y1, 0, 1.0f, 1.0f);
        wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, ay, ex, y1, 0, zoom, 1.0f);
        shape(ex - aw / 32, ay, aw / 16, ah, 0, aw / 42, 0, D3DCOLOR_ARGB((int)(120 * (1 - k)), 255, 255, 255),
              D3DCOLOR_ARGB((int)(120 * (1 - k)), 255, 255, 255));
        break;
    }
    case UI_TRANS_BLINDS:  // twelve slats, each opening from its top
    {
        const int slats = 12;
        const float sh = ah / slats;
        int i;
        for (i = 0; i < slats; i++)
        {
            float kk = wall_ease(k * 1.6f - i * 0.05f), sy = ay + i * sh;   // one after the other, top first
            wall_layout(&s_wallPrev, fit, ax, ay, aw, ah, ax, sy + sh * kk, x1, sy + sh, 0, 1.0f, 1.0f);
            if (kk > 0.002f) wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, sy, x1, sy + sh * kk, 0, zoom, 1.0f);
        }
        break;
    }
    default:               // a fade
        wall_layout(&s_wallPrev, fit, ax, ay, aw, ah, ax, ay, x1, y1, 0, 1.0f, 1.0f - k);
        wall_layout(&s_wallCur, fit, ax, ay, aw, ah, ax, ay, x1, y1, 0, zoom, k);
        break;
    }
}

// The menus' usual backdrop: the gradient and its two glows (under a
// wallpaper that does not fill the screen too).
static void backdrop_plain(void)
{
    shape(0, 0, UI_WIDTH, UI_HEIGHT, 0.0f, 1.0f, 0.0f, UI_COL_BG_TOP, UI_COL_BG_BOTTOM);
    // Two soft glows in LAIKA 64's own tones: a deep blue at the top right, violet at
    // the bottom left (Harissa64's red-orange glow is gone).
    shape(860, -420, 760, 760, 380, 420, 0.0f, D3DCOLOR_ARGB(40, 50, 110, 230), D3DCOLOR_ARGB(40, 50, 110, 230));
    shape(-380, 420, 720, 720, 360, 420, 0.0f, D3DCOLOR_ARGB(30, 120, 70, 200), D3DCOLOR_ARGB(30, 120, 70, 200));
}

int UiWallpaperDraw(int kenBurns)
{
    float zoom = 1.0f;
    wall_sync();
    if (!s_wallCur.tex) return 0;
    if (kenBurns)
    {
        // A slow push in over the picture's time (a minute when it never changes), then still.
        float k = (UiTime() - s_wallShownAt) / ((s_wallSecs ? s_wallSecs : 60) + WALL_TRANS_S);
        zoom = 1.0f + 0.08f * (k > 1.0f ? 1.0f : k);
    }
    if (s_wallFit != UI_FIT_FILL && s_wallFit != UI_FIT_STRETCH && !kenBurns) backdrop_plain();   // around a picture that leaves room
    wall_draw_area(0, 0, UI_WIDTH, UI_HEIGHT, s_wallFit, zoom);
    return 1;
}

int UiWallpaperPreview(float x, float y, float w, float h)
{
    wall_sync();
    if (!s_wallCur.tex) return 0;
    wall_draw_area(x, y, w, h, s_wallFit, 1.0f);
    if (s_wallDim) UiRect(x, y, w, h, D3DCOLOR_ARGB(s_wallDim * 255 / 100, 0, 0, 0));
    return 1;
}

int UiWallpaperThumb(float x, float y, float w, float h, int fit)
{
    wall_sync();
    if (!s_wallCur.tex) return 0;
    wall_layout(&s_wallCur, fit, x, y, w, h, x, y, x + w, y + h, 0, 1.0f, 1.0f);
    return 1;
}

// ---- Panels (Settings > Interface > Borders): their edge's colour, opacity
// and width, the body's opacity, how round their corners are ----
static D3DCOLOR s_panelEdge = UI_COL_LINE;
static float s_panelEdgeW = 1.0f, s_panelRound = 1.0f;
static int s_panelAlpha = 179;   // UI_COL_PANEL's own 70%
static D3DCOLOR s_panelTop = UI_COL_PANEL_TOP, s_panelBottom = UI_COL_PANEL;   // the body (UiSetPanelColor)

void UiSetPanelColor(int r, int g, int b)
{
    if (r < 0) { s_panelTop = UI_COL_PANEL_TOP; s_panelBottom = UI_COL_PANEL; return; }
    // A dark tint of the colour: the text on it stays easy to read.
    s_panelTop = D3DCOLOR_ARGB(255, r * 34 / 100, g * 34 / 100, b * 34 / 100);
    s_panelBottom = D3DCOLOR_ARGB(255, r * 22 / 100, g * 22 / 100, b * 22 / 100);
}

void UiSetPanelStyle(D3DCOLOR edge, float edgeWidth, int bodyPercent, float roundness)
{
    s_panelEdge = edge;
    s_panelEdgeW = edgeWidth < 0 ? 0 : edgeWidth;
    s_panelAlpha = (bodyPercent < 0 ? 0 : bodyPercent > 100 ? 100 : bodyPercent) * 255 / 100;
    s_panelRound = roundness < 0 ? 0 : roundness;
}

float UiPanelRadius(float r) { return r * s_panelRound; }

static int (*s_backdropLayer)(void);
void UiSetBackdropLayer(int (*fn)(void)) { s_backdropLayer = fn; }

void UiBackdrop(void)
{
    skin_sync();
    if (s_backdropLayer && s_backdropLayer())
    {
        if (s_wallDim) UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(s_wallDim * 255 / 100, 0, 0, 0));
        return;
    }
    if (UiWallpaperDraw(0))
    {
        // Darkened by the chosen amount, so the menus stay easy to read over it.
        if (s_wallDim) UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(s_wallDim * 255 / 100, 0, 0, 0));
        return;
    }
    if (s_skinBg)
    {
        UiImage(s_skinBg, 0, 0, UI_WIDTH, UI_HEIGHT, 0.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
        return;
    }
    backdrop_plain();
}

void UiDim(void) { UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UI_COL_DIM); }

D3DCOLOR UiAlpha(D3DCOLOR c, float a) { return with_alpha(c, a); }

// ---- Pictures ----
IDirect3DTexture9 *UiImageCreate(IDirect3DDevice9 *dev, const unsigned char *rgba, int w, int h)
{
    IDirect3DTexture9 *tex = NULL;
    D3DLOCKED_RECT lr;
    int x, y;
    if (!dev || !rgba || w <= 0 || h <= 0) return NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) return NULL;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release();
        return NULL;
    }
    // Bytes R, G, B, A to the texture's A, R, G, B (memory order of a big-endian ARGB word).
    for (y = 0; y < h; y++)
    {
        const unsigned char *s = rgba + (size_t)y * w * 4;
        unsigned char *d = (unsigned char *)lr.pBits + (size_t)y * lr.Pitch;
        for (x = 0; x < w; x++, s += 4, d += 4)
        {
            d[0] = s[3];
            d[1] = s[0];
            d[2] = s[1];
            d[3] = s[2];
        }
    }
    tex->UnlockRect(0);
    return tex;
}

IDirect3DTexture9 *UiImageCreateDxt1(IDirect3DDevice9 *dev, const unsigned char *blocks, int w, int h)
{
    IDirect3DTexture9 *tex = NULL;
    D3DLOCKED_RECT lr;
    int by, rowBytes = (w / 4) * 8;
    if (!dev || !blocks || w <= 0 || h <= 0 || (w & 3) || (h & 3)) return NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_DXT1, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) return NULL;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release();
        return NULL;
    }
    for (by = 0; by < h / 4; by++)   // a row of blocks per pitch
        memcpy((unsigned char *)lr.pBits + (size_t)by * lr.Pitch, blocks + (size_t)by * rowBytes, rowBytes);
    tex->UnlockRect(0);
    return tex;
}

void UiImageRelease(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex)
{
    UiDeadTex d;
    if (!tex) return;
    if (!dev) { tex->Release(); return; }
    // The device keeps no reference: never release what the GPU may still read.
    dev->SetTexture(0, NULL);
    dev->SetTexture(1, s_atlas ? (IDirect3DBaseTexture9 *)s_atlas : NULL);
    d.tex = tex;
    d.fence = dev->InsertFence();
    s_deadTex.push_back(d);
    if (s_deadTex.size() > 256) { dev->BlockUntilIdle(); dead_textures_collect(dev, 1); }   // a bound on what waits
    else dead_textures_collect(dev, 0);
}

void UiImageFlush(IDirect3DDevice9 *dev)
{
    if (!dev || s_deadTex.empty()) return;
    dev->BlockUntilIdle();
    dead_textures_collect(dev, 1);
}

void UiImage(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, D3DCOLOR color)
{
    float p[4], pad = 2.0f, hw = w * 0.5f, hh = h * 0.5f;
    if (!s_dev || !tex || w <= 0 || h <= 0) return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    flush();
    s_dev->SetTexture(1, tex);
    p[0] = hw; p[1] = hh; p[2] = r; p[3] = 1.0f;
    quad(x - pad, y - pad, x + w + pad, y + h + pad, -hw - pad, -hh - pad, hw + pad, hh + pad, 2.0f, 0.0f, p, color, color);
    flush();
    s_dev->SetTexture(1, s_atlas);
}

void UiImageMirror(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, float frac, D3DCOLOR top, D3DCOLOR bottom)
{
    float p[4], pad = 2.0f, hw = w * 0.5f, hh = h * 0.5f;
    if (!s_dev || !tex || w <= 0 || h <= 0 || frac <= 0) return;
    if (frac > 1) frac = 1;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    flush();
    s_dev->SetTexture(1, tex);
    p[0] = hw; p[1] = hh; p[2] = r; p[3] = 1.0f;
    // The box's local y runs from its bottom edge (+hh) upwards: the picture
    // upside down, its bottom row first, its bottom corners rounded.
    quad(x - pad, y, x + w + pad, y + h * frac, -hw - pad, hh, hw + pad, hh - h * frac, 2.0f, 0.0f, p, top, bottom);
    flush();
    s_dev->SetTexture(1, s_atlas);
}

void UiPanel(float x, float y, float w, float h, float r)
{
    r *= s_panelRound;
    UiShadow(x, y + 12, w, h, r, 48, D3DCOLOR_ARGB(150 * s_panelAlpha / 179 > 150 ? 150 : 150 * s_panelAlpha / 179, 0, 0, 0));
    UiRoundRect(x, y, w, h, r, (s_panelTop & 0x00FFFFFF) | ((u32)s_panelAlpha << 24), (s_panelBottom & 0x00FFFFFF) | ((u32)s_panelAlpha << 24));
    if (s_panelEdgeW > 0) UiRoundRing(x, y, w, h, r, s_panelEdgeW, s_panelEdge);
}

// ---- Text ----
// Text is UTF-8. A malformed byte shows as '?', as do characters the atlas
// does not have. Lines holding Arabic are shown right to left (ui_bidi).
#define UI_BIDI_MAX 512

// The next code point of [*pp, end); moves *pp past it.
static u32 utf8_next(const char **pp, const char *end)
{
    const u8 *p = (const u8 *)*pp;
    u32 c = p[0];
    int n, i;
    if (c < 0x80) { *pp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; c &= 0x07; }
    else { *pp += 1; return '?'; }
    if ((const char *)p + 1 + n > end) { *pp += 1; return '?'; }
    for (i = 1; i <= n; i++)
    {
        if ((p[i] & 0xC0) != 0x80) { *pp += 1; return '?'; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return c;
}

// Bytes of the UTF-8 character starting at `s` (1 for a malformed byte).
static int utf8_len(const char *s)
{
    const char *p = s;
    utf8_next(&p, s + strlen(s));
    return (int)(p - s);
}

static const UiAtlasGlyph *glyph(int font, u32 c)
{
    int lo = 95, hi = UI_ATLAS_COUNT - 1;
    const UiAtlasGlyph *g = s_glyphTab[font ? 1 : 0];
    if (c >= 32 && c < 127) return &g[c - 32];   // ASCII: the first 95, in order
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        if (s_atlasCode[mid] == c) return &g[mid];
        if (s_atlasCode[mid] < c) lo = mid + 1;
        else hi = mid - 1;
    }
    return &g['?' - 32];
}

static int has_glyph(int font, u32 c) { return c == '?' || glyph(font, c) != glyph(font, '?'); }

static int utf8_put(u32 c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

void UiUpper(int font, const char *in, char *out, int size)
{
    const char *end = in + strlen(in);
    int len = 0;
    while (in < end && len < size - 4)
    {
        u32 c = utf8_next(&in, end), u = c;
        if (c >= 'a' && c <= 'z') u = c - 32;
        else if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) u = c - 32;   // Latin-1: à..þ
        else if (c >= 0x430 && c <= 0x44F) u = c - 32;               // Cyrillic а..я
        else if (c >= 0x450 && c <= 0x45F) u = c - 80;               // ѐ..џ (ё)
        if (u != c && !has_glyph(font, u)) u = c;                    // no capital in the atlas: as it is
        len += utf8_put(u, out + len);
    }
    out[len] = 0;
}

static float line_width(int font, float size, const char *s, int n)
{
    float w = 0, k = size / UI_ATLAS_BASE;
    const char *end = s + n;
    while (s < end) w += glyph(font, utf8_next(&s, end))->advance * k;
    return w;
}

// ---- Right to left ----
static int is_rtl(u32 c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }
// Letters and digits of the left-to-right scripts.
static int is_ltr(u32 c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7);
}
static u32 mirror(u32 c)
{
    switch (c)
    {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '<': return '>';
    case '>': return '<';
    case 0x2039: return 0x203A;
    case 0x203A: return 0x2039;
    case 0xAB: return 0xBB;
    case 0xBB: return 0xAB;
    }
    return c;
}

// Puts a line holding right-to-left letters in display order (a simple form
// of the Unicode bidirectional algorithm, right-to-left paragraph): runs of
// Latin or Cyrillic words and numbers keep their order inside, everything else
// reads from the right; signs between two left-to-right runs belong to them.
static void ui_bidi(u32 *c, int n)
{
    u8 type[UI_BIDI_MAX];   // 1: left to right
    int i, j;
    for (i = 0; i < n; i++) type[i] = is_ltr(c[i]) ? 1 : 0;
    for (i = 0; i < n; i++)
    {
        if (type[i] || is_rtl(c[i])) continue;
        // Neutral: left to right when the strong characters on both sides are.
        for (j = i; j < n && !type[j] && !is_rtl(c[j]); j++) {}
        if (i > 0 && type[i - 1] && j < n && type[j])
            while (i < j) type[i++] = 1;
        i = j - 1;
    }
    for (i = 0, j = n - 1; i < j; i++, j--)
    {
        u32 t = c[i]; c[i] = c[j]; c[j] = t;
        u8 y = type[i]; type[i] = type[j]; type[j] = y;
    }
    for (i = 0; i < n; i++)
    {
        if (!type[i]) { c[i] = mirror(c[i]); continue; }
        for (j = i; j < n && type[j]; j++) {}
        {
            int a = i, b = j - 1;
            while (a < b) { u32 t = c[a]; c[a] = c[b]; c[b] = t; a++; b--; }
        }
        i = j - 1;
    }
}

static void draw_glyph(const UiAtlasGlyph *g, float *x, float baseline, float k, const float *p, D3DCOLOR color)
{
    if (g->w)
    {
        float gx = *x + g->left * k, gy = baseline + g->top * k;
        quad(gx, gy, gx + g->w * k, gy + g->h * k, (float)g->x / UI_ATLAS_W, (float)g->y / s_atlasH,
             (float)(g->x + g->w) / UI_ATLAS_W, (float)(g->y + g->h) / s_atlasH, 1.0f, 0.0f, p, color, color);
    }
    *x += g->advance * k;
}

// One line; `range` scales the edge sharpness (1: crisp), `bias` the edge offset.
static void draw_line(int font, float size, float x, float baseline, D3DCOLOR color, const char *s, int n, float range, float bias)
{
    float k = size / UI_ATLAS_BASE, p[4];
    const char *end = s + n;
    u32 cps[UI_BIDI_MAX];
    int count = 0, rtl = 0, i;
    p[0] = 2.0f * UI_ATLAS_SPREAD * k * range;
    // Small text: a slightly heavier edge keeps it from looking thin on a TV.
    p[1] = bias + (size < 24.0f ? (24.0f - size) * 0.03f : 0.0f);
    p[2] = 0.0f;
    p[3] = 1.0f;
    while (s < end)
    {
        u32 c = utf8_next(&s, end);
        rtl |= is_rtl(c);
        cps[count++] = c;
        if (count == UI_BIDI_MAX)
        {
            // A very long line: shown in pieces of UI_BIDI_MAX characters.
            if (rtl) ui_bidi(cps, count);
            for (i = 0; i < count; i++) draw_glyph(glyph(font, cps[i]), &x, baseline, k, p, color);
            count = rtl = 0;
        }
    }
    if (rtl) ui_bidi(cps, count);
    for (i = 0; i < count; i++) draw_glyph(glyph(font, cps[i]), &x, baseline, k, p, color);
}

static float text_impl(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align, float range, float bias)
{
    float best = 0, lineH = size * 1.35f, baseline = cy + s_fontTab[font ? 1 : 0].cap * size / UI_ATLAS_BASE * 0.5f;
    for (;;)
    {
        const char *nl = strchr(text, '\n');
        int n = nl ? (int)(nl - text) : (int)strlen(text);
        float w = line_width(font, size, text, n), lx = align == UI_CENTER ? x - w * 0.5f : align == UI_RIGHT ? x - w : x;
        draw_line(font, size, lx, baseline, color, text, n, range, bias);
        if (w > best) best = w;
        if (!nl) break;
        text = nl + 1;
        baseline += lineH;
    }
    return best;
}

float UiText(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align)
{
    return text_impl(font, size, x, cy, color, text, align, 1.0f, 0.5f);
}

float UiTextShadowed(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align)
{
    text_impl(font, size, x, cy + size * 0.06f + 1.0f, with_alpha(D3DCOLOR_ARGB(170, 0, 0, 0), (float)(color >> 24) / 255.0f), text, align,
              0.3f, 0.55f);
    return text_impl(font, size, x, cy, color, text, align, 1.0f, 0.5f);
}

float UiTextWidth(int font, float size, const char *text)
{
    float best = 0;
    for (;;)
    {
        const char *nl = strchr(text, '\n');
        int n = nl ? (int)(nl - text) : (int)strlen(text);
        float w = line_width(font, size, text, n);
        if (w > best) best = w;
        if (!nl) break;
        text = nl + 1;
    }
    return best;
}

void UiTextFit(int font, float size, float maxW, const char *text, char *out, int outSize)
{
    const int ellBytes = (int)sizeof(UI_CH_ELLIPSIS) - 1;
    int n = (int)strlen(text), cut = 0;
    float ell = line_width(font, size, UI_CH_ELLIPSIS, ellBytes), w = 0, k = size / UI_ATLAS_BASE;
    const char *p = text, *end = text + n;
    if (outSize <= 0) return;
    if (line_width(font, size, text, n) <= maxW || n == 0)
    {
        strncpy(out, text, outSize - 1);
        out[outSize - 1] = 0;
        return;
    }
    // The longest start of whole characters that fits with the ellipsis, without a space at its end.
    while (p < end)
    {
        const char *q = p;
        w += glyph(font, utf8_next(&q, end))->advance * k;
        if (w + ell > maxW || (int)(q - text) > outSize - 1 - ellBytes) break;
        p = q;
        if (p[-1] != ' ') cut = (int)(p - text);
    }
    memcpy(out, text, cut);
    memcpy(out + cut, UI_CH_ELLIPSIS, ellBytes + 1);
}

int UiTextWrap(int font, float size, float maxW, int maxLines, const char *text, char *out, int outSize)
{
    int lines = 0, used = 0;
    const char *p = text;
    out[0] = 0;
    while (*p && lines < maxLines)
    {
        // The longest run of whole words that fits (a single long word is cut).
        const char *end = p, *lastFit = NULL;
        int n;
        while (*p == ' ') p++;
        end = p;
        for (;;)
        {
            const char *q = end;
            while (*q && *q != ' ' && *q != '\n') q++;
            if (line_width(font, size, p, (int)(q - p)) > maxW) break;
            lastFit = q;
            if (!*q || *q == '\n') break;
            end = q + 1;
        }
        if (!lastFit)
        {
            // Not even one word: cut it to the width.
            lastFit = p;
            while (*lastFit && *lastFit != ' ' && *lastFit != '\n' &&
                   line_width(font, size, p, (int)(lastFit - p) + utf8_len(lastFit)) <= maxW)
                lastFit += utf8_len(lastFit);
            if (lastFit == p) lastFit += utf8_len(lastFit);
        }
        n = (int)(lastFit - p);
        if (lines == maxLines - 1 && *lastFit && *lastFit != '\n' && lastFit[1])
        {
            // Last allowed line and text remains: cut what is left with an ellipsis.
            char tmp[256];
            int rest = (int)strlen(p);
            if (rest > (int)sizeof(tmp) - 1) rest = (int)sizeof(tmp) - 1;
            memcpy(tmp, p, rest);
            tmp[rest] = 0;
            {
                char *nl = strchr(tmp, '\n');
                if (nl) *nl = 0;
            }
            if (used + 2 >= outSize) break;
            if (lines) out[used++] = '\n';
            UiTextFit(font, size, maxW, tmp, out + used, outSize - used);
            used += (int)strlen(out + used);
            lines++;
            break;
        }
        if (used + n + 2 >= outSize) break;
        if (lines) out[used++] = '\n';
        memcpy(out + used, p, n);
        used += n;
        out[used] = 0;
        lines++;
        p = lastFit;
        if (*p == '\n') p++;
    }
    return lines;
}

// ---- Logo ----
float UiLogo(float x, float y, float size)
{
    float k = size / 128.0f, p[4], u0, v0, u1, v1;
    int i;
    static const D3DCOLOR tops[3] = { D3DCOLOR_ARGB(255, 255, 98, 72), D3DCOLOR_ARGB(110, 255, 236, 228), D3DCOLOR_ARGB(255, 120, 206, 92) };
    static const D3DCOLOR bottoms[3] = { D3DCOLOR_ARGB(255, 190, 30, 26), D3DCOLOR_ARGB(70, 255, 236, 228), D3DCOLOR_ARGB(255, 46, 142, 64) };
    p[2] = 0.0f;
    p[3] = 1.0f;
    for (i = -1; i < 3; i++)
    {
        const UiAtlasRect *r = &s_logoTab[i < 0 ? 0 : i];
        float dy = 0.0f;
        D3DCOLOR ct, cb;
        u0 = (float)r->x / UI_ATLAS_W; v0 = (float)r->y / s_atlasH;
        u1 = (float)(r->x + r->w) / UI_ATLAS_W; v1 = (float)(r->y + r->h) / s_atlasH;
        if (i < 0)
        {
            // A soft shadow of the body, a little lower.
            p[0] = 2.0f * UI_ATLAS_LOGO_SPREAD * k * 0.18f;
            p[1] = 0.6f;
            dy = size * 0.04f;
            ct = cb = D3DCOLOR_ARGB(120, 0, 0, 0);
        }
        else
        {
            p[0] = 2.0f * UI_ATLAS_LOGO_SPREAD * k;
            p[1] = 0.5f;
            ct = tops[i];
            cb = bottoms[i];
        }
        quad(x, y + dy, x + r->w * k, y + dy + r->h * k, u0, v0, u1, v1, 1.0f, 0.0f, p, ct, cb);
    }
    return size;
}


IDirect3DDevice9 *UiDevice(void) { return s_dev; }

// The LAIKA 64 logo picture (xb_logo.h), uploaded the first time it is drawn;
// in the edges' colour when one is chosen (UiSetLogoColor, xb_logo_variants.h).
static IDirect3DTexture9 *s_laikaLogo, *s_logoVar[UI_LOGO_VAR_COUNT];
static int s_laikaLogoTried, s_logoVarTried[UI_LOGO_VAR_COUNT];
static int s_logoVarShown = -1;

void UiSetLogoColor(int rgb)
{
    float r, g, b, mx, mn, h;
    if (rgb < 0) { s_logoVarShown = -1; return; }
    r = ((rgb >> 16) & 255) / 255.0f; g = ((rgb >> 8) & 255) / 255.0f; b = (rgb & 255) / 255.0f;
    mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx <= 0.0f || (mx - mn) / mx < 0.3f) { s_logoVarShown = -1; return; }   // white, greys: the original
    if (mx == r) h = 60.0f * ((g - b) / (mx - mn));
    else if (mx == g) h = 60.0f * ((b - r) / (mx - mn)) + 120.0f;
    else h = 60.0f * ((r - g) / (mx - mn)) + 240.0f;
    if (h < 0) h += 360.0f;
    s_logoVarShown = h < 18.0f || h >= 340.0f ? UI_LOGO_VAR_RED : h < 70.0f ? UI_LOGO_VAR_GOLD : h < 165.0f ? UI_LOGO_VAR_GREEN :
                     h < 255.0f ? UI_LOGO_VAR_BLUE : UI_LOGO_VAR_PURPLE;
}

// The picture shown now (made on first use) and its size; NULL while there is none.
static IDirect3DTexture9 *brand_tex(float *w, float *h)
{
    int v = s_logoVarShown;
    if (v >= 0 && v < UI_LOGO_VAR_COUNT)
    {
        if (!s_logoVarTried[v] && s_dev)
        {
            int pw = 0, ph = 0;
            unsigned char *rgba = h64_png_decode(s_logoVarPng[v], s_logoVarSize[v], &pw, &ph, 1024 * 1024);
            s_logoVarTried[v] = 1;
            if (rgba) { s_logoVar[v] = UiImageCreate(s_dev, rgba, pw, ph); free(rgba); }
        }
        *w = (float)s_logoVarW[v];
        *h = 192.0f;
        if (s_logoVar[v]) return s_logoVar[v];
    }
    if (!s_laikaLogoTried && s_dev)
    {
        int pw = 0, ph = 0;
        unsigned char *rgba = h64_png_decode(s_laikaLogoPng, sizeof(s_laikaLogoPng), &pw, &ph, 1024 * 1024);
        s_laikaLogoTried = 1;
        if (rgba) { s_laikaLogo = UiImageCreate(s_dev, rgba, pw, ph); free(rgba); }
    }
    *w = (float)UI_LAIKA_LOGO_W;
    *h = (float)UI_LAIKA_LOGO_H;
    return s_laikaLogo;
}

float UiBrand(float x, float cy, float size)
{
    // The LAIKA 64 logo (mark and name in one picture), whatever the skin.
    float pw, ph, lh = size * 1.9f, lw;
    IDirect3DTexture9 *tex = brand_tex(&pw, &ph);
    lw = lh * pw / ph;
    if (tex) UiImage(tex, x, cy - lh * 0.5f, lw, lh, 0.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
    else UiText(UI_BOLD, size, x, cy, UI_COL_TEXT, "LAIKA 64", UI_LEFT);
    return lw;
}

float UiBrandWidth(float size)
{
    int v = s_logoVarShown;
    return size * 1.9f * (v >= 0 && v < UI_LOGO_VAR_COUNT ? s_logoVarW[v] / 192.0f : (float)UI_LAIKA_LOGO_W / UI_LAIKA_LOGO_H);
}

void UiBrandFit(float x, float y, float w, float h)
{
    // As big as the box allows at the logo's own proportions, centred, its bottom on the box's.
    float lh = h, lw = UiBrandWidth(h / 1.9f);
    if (lw > w) { lh = h * w / lw; lw = w; }
    UiBrand(x + (w - lw) * 0.5f, y + h - lh * 0.5f, lh / 1.9f);
}

// ---- Button prompts ----
#define BTN_R 15.0f
#define PILL_H 26.0f
#define HINT_SIZE 19.0f

// Draws (or only measures when draw is 0) one button glyph; returns its width.
static float button_glyph(float x, float cy, const char *name, int draw)
{
    D3DCOLOR fill = 0, fill2 = 0, letter = UI_COL_TEXT;
    if (!strcmp(name, "A"))      { fill = D3DCOLOR_ARGB(255, 116, 196, 72); fill2 = D3DCOLOR_ARGB(255, 70, 150, 40); }
    else if (!strcmp(name, "B")) { fill = D3DCOLOR_ARGB(255, 246, 92, 74);  fill2 = D3DCOLOR_ARGB(255, 200, 40, 32); }
    else if (!strcmp(name, "X")) { fill = D3DCOLOR_ARGB(255, 70, 146, 240); fill2 = D3DCOLOR_ARGB(255, 30, 98, 200); }
    else if (!strcmp(name, "Y")) { fill = D3DCOLOR_ARGB(255, 252, 206, 66); fill2 = D3DCOLOR_ARGB(255, 222, 160, 16); letter = D3DCOLOR_ARGB(255, 40, 32, 10); }
    if (fill)
    {
        if (draw)
        {
            UiShadow(x, cy - BTN_R + 2, BTN_R * 2, BTN_R * 2, BTN_R, 6, D3DCOLOR_ARGB(110, 0, 0, 0));
            UiRoundRect(x, cy - BTN_R, BTN_R * 2, BTN_R * 2, BTN_R, fill, fill2);
            UiText(UI_BOLD, 17, x + BTN_R, cy, letter, name, UI_CENTER);
        }
        return BTN_R * 2;
    }
    if (!strcmp(name, "DPAD"))
    {
        if (draw)
        {
            float cx = x + BTN_R;
            D3DCOLOR c = D3DCOLOR_ARGB(255, 214, 217, 226);
            UiRoundRect(cx - 14, cy - 5, 28, 10, 3, c, c);
            UiRoundRect(cx - 5, cy - 14, 10, 28, 3, c, c);
            UiCircle(cx, cy, 2.5f, D3DCOLOR_ARGB(255, 90, 94, 108));
        }
        return BTN_R * 2;
    }
    {
        // Bumpers, BACK, START: an outlined pill with the name.
        float w = UiTextWidth(UI_BOLD, 14, name) + 22;
        if (w < PILL_H + 8) w = PILL_H + 8;
        if (draw)
        {
            UiRoundRect(x, cy - PILL_H * 0.5f, w, PILL_H, PILL_H * 0.5f, D3DCOLOR_ARGB(255, 58, 62, 76), D3DCOLOR_ARGB(255, 44, 47, 58));
            UiRoundRing(x, cy - PILL_H * 0.5f, w, PILL_H, PILL_H * 0.5f, 1.0f, D3DCOLOR_ARGB(60, 255, 255, 255));
            UiText(UI_BOLD, 14, x + w * 0.5f, cy, UI_COL_TEXT, name, UI_CENTER);
        }
        return w;
    }
}

static float footer_impl(float x, float cy, const char *hints, int draw)
{
    char buf[256], *item, *nextItem;
    float x0 = x;
    int first = 1;
    strncpy(buf, hints, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (item = buf; item && *item; item = nextItem)
    {
        char *label, *btn, *nextBtn;
        nextItem = strchr(item, '|');
        if (nextItem) *nextItem++ = 0;
        label = strchr(item, ':');
        if (label) *label++ = 0;
        if (!first) x += 30;
        first = 0;
        for (btn = item; btn && *btn; btn = nextBtn)   // "LB/RB": glyphs sharing one label
        {
            nextBtn = strchr(btn, '/');
            if (nextBtn) *nextBtn++ = 0;
            x += button_glyph(x, cy, btn, draw);
            if (nextBtn && *nextBtn) x += 6;
        }
        if (label)
        {
            x += 10;
            if (draw) UiText(UI_REGULAR, HINT_SIZE, x, cy, UI_COL_TEXT2, label, UI_LEFT);
            x += UiTextWidth(UI_REGULAR, HINT_SIZE, label);
        }
    }
    return x - x0;
}

float UiFooter(float x, float cy, const char *hints) { return footer_impl(x, cy, hints, 1); }
float UiFooterWidth(const char *hints) { return footer_impl(0, 0, hints, 0); }

void UiScreenFooter(const char *hints)
{
    UiRect(UI_SAFE_X, 636, UI_WIDTH - 2 * UI_SAFE_X, 1, UI_COL_LINE);
    UiFooter(UI_SAFE_X, 664, hints);
}

// ---- Notifications ----
void UiToast(const char *text, float age, float life)
{
    float in = age / 200.0f, out = (life - age) / 350.0f, a, slide, w, h = 50, x, y;
    if (age < 0 || age > life) return;
    if (in > 1) in = 1;
    if (out > 1) out = 1;
    if (out < 0) out = 0;
    a = in < out ? in : out;
    slide = (1.0f - in) * (1.0f - in) * 18.0f;
    w = UiTextWidth(UI_REGULAR, 21, text) + 72;
    x = (UI_WIDTH - w) * 0.5f;
    y = UI_HEIGHT - UI_SAFE_Y - 40 - h + slide;
    UiShadow(x, y + 6, w, h, h * 0.5f, 22, with_alpha(D3DCOLOR_ARGB(150, 0, 0, 0), a));
    UiRoundRect(x, y, w, h, h * 0.5f, with_alpha(D3DCOLOR_ARGB(240, 40, 42, 54), a), with_alpha(D3DCOLOR_ARGB(240, 30, 32, 42), a));
    UiRoundRing(x, y, w, h, h * 0.5f, 1.0f, with_alpha(UI_COL_LINE, a));
    UiCircle(x + 28, y + h * 0.5f, 6, with_alpha(UI_COL_ACCENT, a));
    UiText(UI_REGULAR, 21, x + 46, y + h * 0.5f, with_alpha(UI_COL_TEXT, a), text, UI_LEFT);
}

// ---- The performance box (Settings > System > On-screen performance) ----
// Where it goes (UI_PERF_*), its text's size and colour, its body's opacity.
static int s_perfPos = UI_PERF_TOP_RIGHT, s_perfAlpha = 190;
static float s_perfSize = 17;
static D3DCOLOR s_perfColor = UI_COL_TEXT;

void UiSetPerfStyle(int pos, float textSize, D3DCOLOR color, int boxPercent)
{
    s_perfPos = pos >= 0 && pos < UI_PERF_POS_COUNT ? pos : UI_PERF_TOP_RIGHT;
    s_perfSize = textSize < 10 ? 10 : textSize;
    s_perfColor = color;
    s_perfAlpha = (boxPercent < 0 ? 0 : boxPercent > 100 ? 100 : boxPercent) * 255 / 100;
}

// A small line drawing for one line of the box (UI_PERF_ICON_*), `s` high, centred on (x + s/2, cy).
void UiPerfIcon(int kind, float x, float cy, float s, D3DCOLOR col)
{
    const float h = s, w = s, t = s / 12.0f + 0.6f, cx = x + w * 0.5f;
    switch (kind)
    {
    case UI_PERF_ICON_FPS:   // a gauge: a ring open at the bottom, a needle
        UiRoundRing(cx - h * 0.45f, cy - h * 0.45f, h * 0.9f, h * 0.9f, h * 0.45f, t, col);
        UiRect(cx - h * 0.2f, cy + h * 0.28f, h * 0.4f, h * 0.2f, D3DCOLOR_ARGB(0, 0, 0, 0));
        UiLine(cx, cy, cx + h * 0.26f, cy - h * 0.2f, t * 1.2f, col);
        UiCircle(cx, cy, t * 1.2f, col);
        break;
    case UI_PERF_ICON_CPU:   // a chip: a square and its pins
    {
        int i;
        UiRoundRing(cx - h * 0.3f, cy - h * 0.3f, h * 0.6f, h * 0.6f, h * 0.08f, t, col);
        UiRect(cx - h * 0.12f, cy - h * 0.12f, h * 0.24f, h * 0.24f, col);
        for (i = -1; i <= 1; i++)
        {
            float o = i * h * 0.17f;
            UiRect(cx + o - t * 0.5f, cy - h * 0.45f, t, h * 0.13f, col);
            UiRect(cx + o - t * 0.5f, cy + h * 0.32f, t, h * 0.13f, col);
            UiRect(cx - h * 0.45f, cy + o - t * 0.5f, h * 0.13f, t, col);
            UiRect(cx + h * 0.32f, cy + o - t * 0.5f, h * 0.13f, t, col);
        }
        break;
    }
    case UI_PERF_ICON_RAM:   // a memory stick: a long board, its chips, its contacts
    {
        int i;
        UiRoundRing(cx - w * 0.5f, cy - h * 0.25f, w, h * 0.42f, h * 0.05f, t, col);
        for (i = 0; i < 3; i++) UiRect(cx - w * 0.36f + i * w * 0.27f, cy - h * 0.13f, w * 0.18f, h * 0.18f, col);
        for (i = 0; i < 5; i++) UiRect(cx - w * 0.42f + i * w * 0.2f, cy + h * 0.2f, t, h * 0.12f, col);
        break;
    }
    case UI_PERF_ICON_RES:   // a screen on its stand
        UiRoundRing(cx - w * 0.48f, cy - h * 0.36f, w * 0.96f, h * 0.6f, h * 0.06f, t, col);
        UiRect(cx - t * 0.5f, cy + h * 0.24f, t, h * 0.14f, col);
        UiRect(cx - w * 0.22f, cy + h * 0.38f, w * 0.44f, t, col);
        break;
    case UI_PERF_ICON_GPU:   // a graphics card: a board with a fan
        UiRoundRing(cx - w * 0.5f, cy - h * 0.28f, w, h * 0.5f, h * 0.06f, t, col);
        UiRoundRing(cx - h * 0.16f, cy - h * 0.19f, h * 0.32f, h * 0.32f, h * 0.16f, t, col);
        UiCircle(cx, cy - h * 0.03f, t, col);
        UiRect(cx - w * 0.4f, cy + h * 0.22f, w * 0.5f, h * 0.12f, col);
        break;
    default:                 // a thermometer, filled as far as it is warm: green, amber, red
    {
        const D3DCOLOR fill = kind == UI_PERF_ICON_TEMP_HOT ? D3DCOLOR_ARGB(255, 255, 80, 70) :
                              kind == UI_PERF_ICON_TEMP_WARN ? D3DCOLOR_ARGB(255, 255, 185, 60) : D3DCOLOR_ARGB(255, 110, 220, 120);
        const float level = kind == UI_PERF_ICON_TEMP_HOT ? 0.85f : kind == UI_PERF_ICON_TEMP_WARN ? 0.6f : 0.35f;
        const float tw = h * 0.22f, top = cy - h * 0.48f, stem = h * 0.62f, bulb = h * 0.2f;
        UiRoundRing(cx - tw * 0.5f, top, tw, stem + bulb, tw * 0.5f, t, col);
        UiRoundRing(cx - bulb, cy + h * 0.14f - bulb * 0.2f, bulb * 2, bulb * 2, bulb, t, col);
        UiCircle(cx, cy + h * 0.14f + bulb * 0.8f, bulb * 0.62f, fill);
        UiRoundRect(cx - tw * 0.2f, top + stem * (1 - level), tw * 0.4f, stem * level + bulb * 0.4f, tw * 0.2f, fill, fill);
        break;
    }
    }
}

// The box over the area (ax, ay, aw, ah) - the screen, or a preview of it at
// `k` of its size: one line as a pill, several as a panel, in its corner. A
// line may start with an icon (one byte, UI_PERF_ICON_*), drawn before it.
static void perf_box(const char *text, float ax, float ay, float aw, float ah, float k)
{
    const float size = s_perfSize * k, mx = UI_SAFE_X * k, my = UI_SAFE_Y * k, lineH = size * 1.35f, iconW = size * 1.35f;
    const char *lines[16];
    int icons[16], n = 0, i, anyIcon = 0;
    char buf[512], *p;
    float w = 0, h, x, y, r, pad;
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (p = buf; *p && n < 16;)
    {
        char *e = strchr(p, '\n');
        icons[n] = (unsigned char)*p >= 1 && (unsigned char)*p < UI_PERF_ICON_COUNT ? (unsigned char)*p++ : 0;
        anyIcon |= icons[n] != 0;
        lines[n++] = p;
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    for (i = 0; i < n; i++)
    {
        float lw = UiTextWidth(UI_BOLD, size, lines[i]);
        if (lw > w) w = lw;
    }
    pad = (n > 1 ? 15 : 13) * k * s_perfSize / 17;
    w += 2 * pad + (anyIcon ? iconW : 0);
    h = n > 1 ? 14 * k + n * lineH : size * 1.9f;
    r = n > 1 ? 14 * k : h * 0.5f;
    switch (s_perfPos)
    {
    case UI_PERF_TOP_LEFT: case UI_PERF_BOTTOM_LEFT: x = ax + mx; break;
    case UI_PERF_TOP_CENTER: x = ax + (aw - w) * 0.5f; break;
    default: x = ax + aw - mx - w; break;
    }
    y = s_perfPos == UI_PERF_BOTTOM_LEFT || s_perfPos == UI_PERF_BOTTOM_RIGHT ? ay + ah - my - h : ay + my;
    if (s_perfAlpha)
    {
        UiRoundRect(x, y, w, h, r, (D3DCOLOR_ARGB(255, 24, 26, 34) & 0x00FFFFFF) | ((u32)s_perfAlpha << 24),
                    (D3DCOLOR_ARGB(255, 16, 17, 23) & 0x00FFFFFF) | ((u32)s_perfAlpha << 24));
        UiRoundRing(x, y, w, h, r, 1.0f, D3DCOLOR_ARGB(26 * s_perfAlpha / 190 > 255 ? 255 : 26 * s_perfAlpha / 190, 255, 255, 255));
    }
    for (i = 0; i < n; i++)
    {
        float cy = n > 1 ? y + 7 * k + lineH * (i + 0.5f) : y + h * 0.5f, tx = x + pad;
        if (icons[i])
        {
            // Discreet: the text's colour, fainter (a temperature's own colour shows in its bulb).
            UiPerfIcon(icons[i], tx, cy, size * 0.95f, UiAlpha(s_perfColor, 0.7f));
            tx += iconW;
        }
        else if (anyIcon) tx += iconW;
        UiTextShadowed(UI_BOLD, size, tx, cy, s_perfColor, lines[i], UI_LEFT);
    }
}

void UiFpsBadge(const char *text) { perf_box(text, 0, 0, UI_WIDTH, UI_HEIGHT, 1.0f); }

void UiPerfPanel(const char *text) { perf_box(text, 0, 0, UI_WIDTH, UI_HEIGHT, 1.0f); }

void UiPerfPreview(const char *text, float x, float y, float w, float h) { perf_box(text, x, y, w, h, w / UI_WIDTH); }

// The console's temperatures from its system controller (SMC command 0x07:
// CPU, GPU, EDRAM - the memory - and the board, each in 1/256 degree C),
// asked at most once a second. 0 when they cannot be read (Xenia).
extern "C" VOID HalSendSMCMessage(LPVOID pCommandBuffer, LPVOID pRecvBuffer);
int UiTemperatures(int allowed, float *cpu, float *gpu, float *mem)
{
    static DWORD at;
    static float t[3];
    static int ok;
    DWORD now = GetTickCount();
    if (!allowed) return 0;
    if (!at || now - at >= 1000)
    {
        unsigned char in[16], out[16];
        int i;
        memset(in, 0, sizeof(in));
        memset(out, 0, sizeof(out));
        in[0] = 0x07;
        HalSendSMCMessage(in, out);
        // Each reading that makes sense replaces the last one; one that does not keeps
        // it (and the lines shown): a bad reading used to drop the temperatures for a
        // second, the box shrinking and growing again - it flickered.
        for (i = 0; i < 3; i++)
        {
            float v = (out[1 + i * 2] | (out[2 + i * 2] << 8)) / 256.0f;
            if (v > 1.0f && v < 150.0f) { t[i] = v; if (!i) ok = 1; }
        }
        at = now;
    }
    if (cpu) *cpu = t[0];
    if (gpu) *gpu = t[1];
    if (mem) *mem = t[2];
    return ok;
}

// ---- Menu input ----
// A held direction or shoulder button repeats (LB/RB page through the ROM
// list as if pressed again and again), faster after REPEAT_FAST_AFTER_MS.
#define REPEAT_FIRST_MS 350
#define REPEAT_NEXT_MS 90
#define REPEAT_FAST_MS 45
#define REPEAT_FAST_AFTER_MS 1000
static const WORD REPEATS = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
                            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;

// L3+R3 (both thumbsticks clicked): a screenshot of the menu on screen right
// now (xb_menu.cpp's own ROM browser/settings/pause menu loops all funnel
// their input through UiInputPoll, so this fires everywhere at once, with no
// change needed in any of them). Unrelated to the game's own screenshot
// combo (Config::shotBtn1/2, main.cpp).
#define UI_SHOT_COMBO (XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB)

void UiInputInit(UiInput *in)
{
    XINPUT_STATE st;
    in->held = in->prev = 0;
    in->repeatAt = in->holdStart = 0;
    // Buttons already down (the press that opened the menu) do not count.
    if (XInputGetState(0, &st) == ERROR_SUCCESS) in->prev = st.Gamepad.wButtons;
}

WORD UiInputPoll(UiInput *in)
{
    XINPUT_STATE st;
    WORD b = 0, down;
    DWORD now = GetTickCount();
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
    {
        b = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLY > 20000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -20000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX < -20000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (st.Gamepad.sThumbLX > 20000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }
    down = b & ~in->prev;
    if (down & REPEATS)
    {
        in->repeatAt = now + REPEAT_FIRST_MS;
        in->holdStart = now;
    }
    else if ((b & REPEATS) && (s32)(now - in->repeatAt) >= 0)
    {
        down |= b & REPEATS;
        in->repeatAt = now + (now - in->holdStart >= REPEAT_FAST_AFTER_MS ? REPEAT_FAST_MS : REPEAT_NEXT_MS);
    }
    if ((b & UI_SHOT_COMBO) == UI_SHOT_COMBO && (in->prev & UI_SHOT_COMBO) != UI_SHOT_COMBO) s_shotPending = 1;
    // Settings > Sound > Interface sounds: one sound per press (and per repeat
    // of a held direction), by button - every menu reads its input here, the
    // game never does.
    if (down & XINPUT_GAMEPAD_START) xb_audio_ui_sound(XB_SFX_OPEN);
    else if (down & XINPUT_GAMEPAD_A) xb_audio_ui_sound(XB_SFX_SELECT);
    else if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) xb_audio_ui_sound(XB_SFX_BACK);
    else if (down & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) xb_audio_ui_sound(XB_SFX_ACTION);
    else if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) xb_audio_ui_sound(XB_SFX_PAGE);
    else if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))
        xb_audio_ui_sound(XB_SFX_MOVE);
    xb_audio_music_tick();   // the menus' music moves on to its next track (every menu loop polls here)
    in->prev = b;
    in->held = b;
    return down;
}
