// Harissa64 V2 - screen drawing for the front end (Xbox 360): text, panels,
// button prompts, the logo, toasts.
//
// Everything is drawn as alpha-blended quads in one small batch per UiBegin /
// UiEnd pair, with one pixel shader: rounded rectangles, circles and rings are
// signed distance functions evaluated per pixel (anti-aliased edges, soft
// shadows), text and the pepper logo come from a signed distance field atlas
// (xb_ui_atlas.h, built offline by tools/gen_ui_atlas.py from Inter), so they
// stay smooth at any size. Coordinates are back-buffer pixels (1280x720),
// fractional values allowed.
//
// The renderer calls the overlay between its last draw and the present, so the
// UI may change any state it likes: h64_xenos_present_vi binds the renderer's
// vertex shader, declaration and textures again afterwards.
#ifndef XB_UI_H
#define XB_UI_H

#include <xtl.h>

#define UI_WIDTH 1280
#define UI_HEIGHT 720
// Title-safe area (5 % margins at 720p).
#define UI_SAFE_X 64
#define UI_SAFE_Y 36

// ---- Palette: dark neutrals and one accent (light blue) ----
#define UI_COL_BG_TOP     D3DCOLOR_ARGB(255, 18, 19, 26)
#define UI_COL_BG_BOTTOM  D3DCOLOR_ARGB(255, 10, 11, 15)
#define UI_COL_PANEL      D3DCOLOR_ARGB(179, 27, 29, 38)   // 70% opaque: every menu panel lets the background show through
#define UI_COL_PANEL_TOP  D3DCOLOR_ARGB(179, 33, 35, 46)
#define UI_COL_CARD       D3DCOLOR_ARGB(255, 34, 36, 47)
#define UI_COL_LINE       D3DCOLOR_ARGB(26, 255, 255, 255)
#define UI_COL_TEXT       D3DCOLOR_ARGB(255, 243, 244, 247)
#define UI_COL_TEXT2      D3DCOLOR_ARGB(255, 164, 168, 182)
#define UI_COL_TEXT3      D3DCOLOR_ARGB(255, 112, 116, 132)
#define UI_COL_ACCENT     D3DCOLOR_ARGB(255, 70, 160, 240)    // light blue, like the selection's ring (was harissa red)
#define UI_COL_ACCENT_HI  D3DCOLOR_ARGB(255, 120, 205, 255)
#define UI_COL_SELECT     D3DCOLOR_ARGB(64, 70, 160, 240)
#define UI_COL_SHADOW     D3DCOLOR_ARGB(150, 0, 0, 0)
#define UI_COL_DIM        D3DCOLOR_ARGB(150, 4, 5, 8)

enum { UI_REGULAR = 0, UI_BOLD = 1 };
enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };

// Starts a batch on `dev` (the bound render target, normally the back buffer).
// The first call creates the atlas texture and the shaders.
void UiBegin(IDirect3DDevice9 *dev);
// Draws what is batched; leaves blending off.
void UiEnd(void);
// Seconds since the first UI call (animations).
float UiTime(void);
// The menus' font (Settings > Interface > Font): 0 Inter, then the others of
// xb_ui_fonts.h; it changes at the next UiBegin. Their count and names.
void UiSetFont(int index);
int UiFontCount(void);
const char *UiFontName(int index);
// Frames drawn so far (UiEnd calls): whether something was drawn in the frame
// just before, whatever time a slow frame took.
unsigned UiFrameCount(void);

// ---- Shapes ----
// Rounded rectangle with a vertical gradient (top colour, bottom colour).
void UiRoundRect(float x, float y, float w, float h, float r, D3DCOLOR top, D3DCOLOR bottom);
void UiRect(float x, float y, float w, float h, D3DCOLOR color);
// The outline of a rounded rectangle, `thick` pixels inside its edge.
void UiRoundRing(float x, float y, float w, float h, float r, float thick, D3DCOLOR color);
// A soft shadow of a rounded rectangle, `blur` pixels wide.
void UiShadow(float x, float y, float w, float h, float r, float blur, D3DCOLOR color);
void UiCircle(float cx, float cy, float r, D3DCOLOR color);
// An anti-aliased segment `thick` pixels wide (square ends: polylines join cleanly).
void UiLine(float x0, float y0, float x1, float y1, float thick, D3DCOLOR color);
// A slanted strip from x0 to x1, its top and bottom at the heights given at
// each end; top colour ct, bottom cb (a slice of a wave).
void UiStrip(float x0, float top0, float bot0, float x1, float top1, float bot1, D3DCOLOR ct, D3DCOLOR cb);
// Full-screen backdrop of the menus (gradient and a warm glow), or a skin's
// background.png over the whole screen when one is set (see UiSetSkinWant).
void UiBackdrop(void);
// The translucent veil over the frozen game frame.
void UiDim(void);
// A panel: shadow, translucent body, a fine light edge (as UiSetPanelStyle says).
void UiPanel(float x, float y, float w, float h, float r);
// The panels' look: the edge's colour (its alpha too) and width (0: none),
// the body's opacity in percent, a factor on every panel's corner radius.
void UiSetPanelStyle(D3DCOLOR edge, float edgeWidth, int bodyPercent, float roundness);
// A panel's corner radius `r` as the panels are drawn now.
float UiPanelRadius(float r);
// What is drawn next scaled by k around (cx, cy); k = 1: as it is again.
void UiSetScale(float cx, float cy, float k);
// The panels' body colour: a dark tint of (r, g, b); r < 0: the interface's own.
void UiSetPanelColor(int r, int g, int b);
// `c` with its alpha multiplied by `a` (0..1): fades.
D3DCOLOR UiAlpha(D3DCOLOR c, float a);

// ---- Pictures (box art) ----
// A texture from 8-bit RGBA pixels (bytes R, G, B, A); NULL on failure. Draw
// it at about its own size: it has no mipmaps.
IDirect3DTexture9 *UiImageCreate(IDirect3DDevice9 *dev, const unsigned char *rgba, int w, int h);
// Released pictures still waiting for the GPU (UiImageRelease keeps them until
// it is done with them) freed now: before a game takes the memory.
void UiImageFlush(IDirect3DDevice9 *dev);
// A picture of the menus' own as the background (the wallpapers' game
// recommendations): `fn` draws it and returns 1, or 0 to leave the usual one;
// darkened as the wallpapers are. NULL: none.
void UiSetBackdropLayer(int (*fn)(void));
// The LAIKA 64 logo in the colour nearest `rgb` (0xRRGGBB): blue, green, red,
// purple or gold (xb_logo_variants.h); -1, or a colour without hue: the original.
void UiSetLogoColor(int rgb);
// The same from DXT1 blocks as the Xbox 360's GPU reads them (w, h multiples of 4): copied as they are.
IDirect3DTexture9 *UiImageCreateDxt1(IDirect3DDevice9 *dev, const unsigned char *blocks, int w, int h);
// The device the interface draws with (NULL before UiInit): for pictures made on first use.
IDirect3DDevice9 *UiDevice(void);
// Waits for the GPU to be done with it, then releases it (outside UiBegin/UiEnd).
void UiImageRelease(IDirect3DDevice9 *dev, IDirect3DTexture9 *tex);
// Draws the whole picture stretched over (x, y, w, h), corners rounded by `r`,
// tinted by `color` (white: as it is; its alpha fades the picture).
void UiImage(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, D3DCOLOR color);
// The reflection of a picture drawn at (x, y - h - gap, w, h): its lower part
// (`frac` of it) upside down from y, tinted from `top` to `bottom` (fade the
// alpha to 0 for a mirror floor).
void UiImageMirror(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, float frac, D3DCOLOR top, D3DCOLOR bottom);

// ---- Text ----
// Draws `text` (UTF-8) with the vertical middle of its capitals at `cy` (later
// lines of a "\n"-separated text 1.35 sizes lower), aligned on x by `align`.
// A line holding Arabic letters (already in their joined forms, see
// xb_lang.h) is shown right to left. Returns the widest line's width.
float UiText(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
// The same with a soft drop shadow under it.
float UiTextShadowed(int font, float size, float x, float cy, D3DCOLOR color, const char *text, int align);
float UiTextWidth(int font, float size, const char *text);
// `in` in capitals (ASCII, Latin-1 accented letters, Cyrillic); a letter whose
// capital the atlas lacks stays as it is (no '?'). Arabic has no case.
void UiUpper(int font, const char *in, char *out, int size);
// Copies `text` into `out`, cut with an ellipsis to fit `maxW` pixels.
void UiTextFit(int font, float size, float maxW, const char *text, char *out, int outSize);
// Word-wraps `text` into `out` ("\n" between lines) to fit `maxW`; at most
// `maxLines` lines, the last one cut with an ellipsis. Returns the line count.
int UiTextWrap(int font, float size, float maxW, int maxLines, const char *text, char *out, int outSize);
#define UI_CH_LCHEVRON "\xE2\x80\xB9"   // U+2039
#define UI_CH_RCHEVRON "\xE2\x80\xBA"   // U+203A
#define UI_CH_ELLIPSIS "\xE2\x80\xA6"   // U+2026
#define UI_CH_BULLET   "\xE2\x80\xA2"   // U+2022
#define UI_CH_MIDDOT   "\xC2\xB7"       // U+00B7

// The Harissa pepper, `size` pixels square, top-left at (x, y). Returns `size`.
float UiLogo(float x, float y, float size);
// The LAIKA 64 logo picture (xb_logo.h), whatever the skin; (x, cy) left end
// and vertical middle, `size` the old title's size (the logo is 1.9x as
// high). Returns the width.
float UiBrand(float x, float cy, float size);
// Its width at `size` (UiBrand's), and the logo as large as fits the box
// (x, y, w, h), centred across it, its bottom on the box's bottom.
float UiBrandWidth(float size);
void UiBrandFit(float x, float y, float w, float h);

// ---- Wallpapers (game:\wallpapers\<folder>\, over a skin's background) ----
// The transitions between two wallpapers (Config::wallTransition).
enum { UI_TRANS_FADE = 0, UI_TRANS_SLIDE, UI_TRANS_ZOOM, UI_TRANS_WIPE, UI_TRANS_BLINDS, UI_TRANS_RANDOM, UI_TRANS_COUNT };
// How a wallpaper is laid out (Config::wallFit): filling the screen (cut to
// fit), all of it inside the screen, stretched to it, at its own size in the
// middle, repeated as tiles.
enum { UI_FIT_FILL = 0, UI_FIT_FIT, UI_FIT_STRETCH, UI_FIT_CENTER, UI_FIT_TILE, UI_FIT_COUNT };
// The pictures UiBackdrop shows: those of `folder` (Settings > Paths) and its
// folders, when `on` (else only listed, for Settings to choose from); `pick` the one to show (a name
// from UiWallpaperName; "": the first), then the others in turn, each for
// `seconds` (0: never changes), darkened by `dim` percent, laid out by `fit`,
// in name order or at random (shuffle). Cheap to call every frame: the folder
// is read again only when the choice changes (or after UiWallpaperRescan).
void UiSetWallpapers(const char *folder, int on, const char *pick, int seconds, int transition, int dim, int fit, int shuffle);
// The i-th picture's name ("pic.jpg", "folder\pic.jpg"; "" past the end), and a name's place (-1: not there).
const char *UiWallpaperName(int i);
int UiWallpaperFind(const char *name);
// The wallpaper shown now over the whole screen (kenBurns: slowly zooming in,
// the screen saver's); 0 when there is none.
int UiWallpaperDraw(int kenBurns);
// How many pictures the wallpapers have to show (0: none, or they are off).
int UiWallpaperCount(void);
// Read the wallpapers folder again (the settings page opening: pictures may have been copied in).
void UiWallpaperRescan(void);
// The backdrop's wallpaper as it is now (layout, transition, darkening) in a
// box at the screen's proportions; 0 when there is none.
int UiWallpaperPreview(float x, float y, float w, float h);
// The wallpaper shown now in a small box at the screen's proportions, laid
// out by `fit` (the settings' previews); 0 when there is none.
int UiWallpaperThumb(float x, float y, float w, float h, int fit);
// 1 when `name` is a picture the wallpapers can show (.jpg, .jpeg, .png, .bmp).
int UiWallpaperIsImage(const char *name);
// Part (u0..u1, v0..v1) of a picture over four corners (top left, top right,
// bottom left, bottom right as xy[8]): a box's faces seen at an angle.
void UiImageQuad(IDirect3DTexture9 *tex, const float *xy, float u0, float v0, float u1, float v1, D3DCOLOR color);
// Part (u0..u1, v0..v1, 0..1) of a picture stretched over (x, y, w, h).
void UiImagePart(IDirect3DTexture9 *tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, D3DCOLOR color);

// ---- Skins (background.png override - see xb_menu.h SkinScan) ----
// Remembers which skin's background UiBackdrop should show ("": none,
// the usual drawing). Needs no IDirect3DDevice9*: the texture is only
// decoded and uploaded lazily, on the next UiBackdrop call, using
// whatever device UiBegin was last given - so this can be called from
// anywhere a skin is chosen (the ROM browser or the in-game settings menu
// alike), never only where a device happens to be at hand.
void UiSetSkinWant(const char *name);

// ---- Button prompts ----
// "A:Play|B:Back|LB/RB:Page|DPAD:Change": each button's glyph (coloured disc
// for A/B/X/Y, a pill for LB, RB, BACK, START, a cross for DPAD) and its label,
// from (x, cy) (cy is the vertical centre). Returns the width.
float UiFooter(float x, float cy, const char *hints);
float UiFooterWidth(const char *hints);
// The footer of a full screen: a fine line and the prompts in the safe area.
void UiScreenFooter(const char *hints);

// ---- Notifications ----
// A toast at the bottom centre; `age` and `life` in ms (it slides in and fades out).
void UiToast(const char *text, float age, float life);
// The frames-per-second badge, where UiSetPerfStyle puts it (top right at first).
void UiFpsBadge(const char *text);
// The same place with several lines ("\n"-separated): FPS, CPU, RAM, resolution, temperatures.
void UiPerfPanel(const char *text);
// Where the performance box goes, its text's size (pixels) and colour, its body's opacity in percent.
enum { UI_PERF_TOP_LEFT = 0, UI_PERF_TOP_CENTER, UI_PERF_TOP_RIGHT, UI_PERF_BOTTOM_LEFT, UI_PERF_BOTTOM_RIGHT, UI_PERF_POS_COUNT };
void UiSetPerfStyle(int pos, float textSize, D3DCOLOR color, int boxPercent);
// The icons a line of the box may start with (that byte first): FPS, CPU, RAM, resolution, GPU, a
// temperature normal (under 70 C), high (70-75 C), hot (over 75 C). UiPerfIcon draws one, `s` high.
enum { UI_PERF_ICON_FPS = 1, UI_PERF_ICON_CPU, UI_PERF_ICON_RAM, UI_PERF_ICON_RES, UI_PERF_ICON_GPU,
       UI_PERF_ICON_TEMP_OK, UI_PERF_ICON_TEMP_WARN, UI_PERF_ICON_TEMP_HOT, UI_PERF_ICON_COUNT };
void UiPerfIcon(int kind, float x, float cy, float s, D3DCOLOR col);
// The box as it would be on screen, scaled into (x, y, w, h) (the settings' preview).
void UiPerfPreview(const char *text, float x, float y, float w, float h);
// The CPU, GPU and memory (EDRAM) temperatures in degrees C, read at most once
// a second; 0 when not `allowed` (Xenia) or they cannot be read.
int UiTemperatures(int allowed, float *cpu, float *gpu, float *mem);

// Controller 1 for menus: buttons pressed since the last call, with
// auto-repeat for the D-pad and the left stick (as D-pad directions).
struct UiInput
{
    WORD held, prev;
    DWORD repeatAt, holdStart;
};
void UiInputInit(UiInput *in);
WORD UiInputPoll(UiInput *in);   // XINPUT_GAMEPAD_* bits newly pressed (or repeating)

#endif
