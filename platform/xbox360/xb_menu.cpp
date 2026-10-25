// Harissa64 V2 - front-end menus (see xb_menu.h).
#include "xb_menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../core/cart/h64_rom.h"
#include "../../core/cart/h64_save.h"
#include "../../core/cart/h64_zip.h"
#include "../../core/common/h64_log.h"
#include "../../core/common/h64_version.h"
#include "../../core/common/h64_fenv.h"
#include "../../core/common/h64_png.h"
#include "../../render/xenos/h64_hts.h"
#include "xb_art.h"
#include "xb_audio.h"
#include "xb_flags.h"
#include "xb_lang.h"
#include "xb_ui.h"
#include "xb_update.h"

// Moves an animated value towards its target (exponential, ~70 ms), or
// jumps there when the animation restarts.
struct UiAnim
{
    float value, last;
    int live;
};

// `rate`: how fast it closes in (per second; 16 is ~70 ms, lower glides longer).
// `snap`: close enough to stop (pixels for positions, less for pages and slots).
static float anim_step_rate(UiAnim *a, float target, int restart, float rate, float snap)
{
    float now = UiTime(), dt = now - a->last;
    a->last = now;
    if (restart || !a->live || dt > 0.3f || dt < 0.0f)
    {
        a->value = target;
        a->live = 1;
        return target;
    }
    {
        float k = dt * rate;
        if (k > 1.0f) k = 1.0f;
        a->value += (target - a->value) * k;
        if (fabsf(target - a->value) < snap) a->value = target;
    }
    return a->value;
}

static float anim_step(UiAnim *a, float target, int restart) { return anim_step_rate(a, target, restart, 16.0f, 0.25f); }

// The selection's light blue ring and glow fade in and out together, everywhere
// (list rows, menus, the covers grid, the language cards); never quite gone,
// so the selection stays easy to find.
static float selection_pulse(void) { return 0.25f + 0.75f * (0.5f + 0.5f * sinf(UiTime() * 4.0f)); }
#define SEL_RING(a) D3DCOLOR_ARGB((int)(255 * (a)), 120, 205, 255)
#define SEL_GLOW(a) D3DCOLOR_ARGB((int)(150 * (a)), 90, 190, 255)

// The pulsing outer glow around a selection's ring (the same on every border):
// a wide soft halo and a brighter one close to the edge.
static void selection_glow(float x, float y, float w, float h, float r, float a)
{
    UiShadow(x - 2, y - 2, w + 4, h + 4, r + 2, 34, SEL_GLOW(a));
    UiShadow(x, y, w, h, r, 14, SEL_GLOW(a));
}

// The selection highlight of a list row: a blue-tinted pill, its pulsing ring
// and a bar on its left (on its right in a right-to-left menu).
static void selection_bar_side(float x, float y, float w, float h, int right)
{
    float a = selection_pulse();
    selection_glow(x, y, w, h, 12, a);
    UiRoundRect(x, y, w, h, 12, D3DCOLOR_ARGB(60, 90, 170, 240), D3DCOLOR_ARGB(38, 90, 170, 240));
    UiRoundRing(x, y, w, h, 12, 2.0f, SEL_RING(a));
    UiRoundRect(right ? x + w - 11 : x + 7, y + h * 0.25f, 4, h * 0.5f, 2, D3DCOLOR_ARGB(255, 150, 220, 255), D3DCOLOR_ARGB(255, 90, 180, 250));
}

static void selection_bar(float x, float y, float w, float h) { selection_bar_side(x, y, w, h, 0); }

// A paragraph wrapped to `w` (top at y); returns its height.
static float paragraph(int font, float size, float x, float y, float w, int maxLines, D3DCOLOR color, const char *text)
{
    char buf[1024];
    int lines = UiTextWrap(font, size, w, maxLines, text, buf, sizeof(buf));
    float lineH = size * 1.35f;
    UiText(font, size, x, y + lineH * 0.5f, color, buf, UI_LEFT);
    return lines * lineH;
}

// A small section title in capitals, in the accent colour.
static void heading(float x, float cy, const char *text)
{
    UiText(UI_BOLD, 15, x, cy, UI_COL_ACCENT_HI, text, UI_LEFT);
}

// ---- Generic list panel ----
void UiMenuClear(UiMenu *m, const char *title, const char *footer)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->title, title, sizeof(m->title) - 1);
    strncpy(m->footer, footer, sizeof(m->footer) - 1);
}

int UiMenuAdd(UiMenu *m, const char *label, const char *value)
{
    if (m->count >= UI_MENU_MAX) return -1;
    strncpy(m->label[m->count], label, sizeof(m->label[0]) - 1);
    strncpy(m->value[m->count], value ? value : "", sizeof(m->value[0]) - 1);
    return m->count++;
}

// A value shown "< 250 ms >" (DPAD changes it): drawn as the text between two
// chevrons. Returns the left end.
static float draw_value(float right, float cy, const char *value, int selected)
{
    size_t n = strlen(value);
    D3DCOLOR text = selected ? UI_COL_TEXT : UI_COL_TEXT2;
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        float cw = UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON), x = right;
        D3DCOLOR chev = selected ? UI_COL_ACCENT_HI : UI_COL_TEXT3;
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_RCHEVRON, UI_RIGHT);
        x -= cw + 10;
        x -= UiText(selected ? UI_BOLD : UI_REGULAR, 21, x, cy, text, inner, UI_RIGHT);
        x -= 10;
        UiText(UI_BOLD, 30, x, cy - 1, chev, UI_CH_LCHEVRON, UI_RIGHT);
        return x - cw;
    }
    return right - UiText(UI_REGULAR, 21, right, cy, text, value, UI_RIGHT);
}

// The width draw_value takes.
static float value_width(const char *value, int selected)
{
    size_t n = strlen(value);
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        return 2 * (UiTextWidth(UI_BOLD, 30, UI_CH_RCHEVRON) + 10) + UiTextWidth(selected ? UI_BOLD : UI_REGULAR, 21, inner);
    }
    return UiTextWidth(UI_REGULAR, 21, value);
}

static void controls_draw(const UiMenu *m);
static void calib_draw(const UiMenu *m);
static void volume_draw(const UiMenu *m);

static int s_browser;   // opened from the ROM list: the box art rows are shown (also: UiMenuDraw skips the dim behind it in-game, over the frozen frame)

// A row's small line symbol (UI_ICON_*), about 22 pixels across, centred on (cx, cy).
static void menu_icon(int icon, float cx, float cy, D3DCOLOR c)
{
    int k;
    switch (icon)
    {
    case UI_ICON_SYSTEM:      // a chip: a square, its core and three pins on each side
        UiRoundRing(cx - 8, cy - 8, 16, 16, 3, 2.0f, c);
        UiRoundRect(cx - 3.5f, cy - 3.5f, 7, 7, 1.5f, c, c);
        for (k = -1; k <= 1; k++)
        {
            UiRect(cx - 11, cy + k * 4.5f - 0.75f, 3, 1.5f, c);
            UiRect(cx + 8, cy + k * 4.5f - 0.75f, 3, 1.5f, c);
            UiRect(cx + k * 4.5f - 0.75f, cy - 11, 1.5f, 3, c);
            UiRect(cx + k * 4.5f - 0.75f, cy + 8, 1.5f, 3, c);
        }
        break;
    case UI_ICON_GRAPHICS:    // a picture: a frame, a sun and a hill
        UiRoundRing(cx - 11, cy - 8, 22, 16, 3, 2.0f, c);
        UiCircle(cx + 4, cy - 2.5f, 2.2f, c);
        UiRoundRect(cx - 8, cy + 1, 10, 4.5f, 2, c, c);
        break;
    case UI_ICON_SCREEN:      // a monitor on its stand
        UiRoundRing(cx - 11, cy - 9, 22, 14, 2.5f, 2.0f, c);
        UiRect(cx - 1, cy + 5, 2, 3, c);
        UiRoundRect(cx - 6, cy + 7.5f, 12, 2, 1, c, c);
        break;
    case UI_ICON_SOUND:       // a speaker and three level bars
        UiRoundRect(cx - 11, cy - 3.5f, 5, 7, 1, c, c);
        UiRoundRect(cx - 7, cy - 6.5f, 3.5f, 13, 1.5f, c, c);
        UiRoundRect(cx + 0.5f, cy - 2, 2, 4, 1, c, c);
        UiRoundRect(cx + 4.5f, cy - 4.5f, 2, 9, 1, c, c);
        UiRoundRect(cx + 8.5f, cy - 7, 2, 14, 1, c, c);
        break;
    case UI_ICON_CONTROLS:    // a pad: its outline, a cross and two buttons
        UiRoundRing(cx - 12, cy - 7, 24, 14, 7, 2.0f, c);
        UiRect(cx - 8.5f, cy - 0.75f, 6, 1.5f, c);
        UiRect(cx - 6.25f, cy - 3, 1.5f, 6, c);
        UiCircle(cx + 4, cy - 1.5f, 1.4f, c);
        UiCircle(cx + 7, cy + 1.5f, 1.4f, c);
        break;
    case UI_ICON_INTERFACE:   // a window: a top bar and a side column
        UiRoundRing(cx - 10, cy - 9, 20, 18, 3, 2.0f, c);
        UiRect(cx - 9, cy - 4, 18, 2, c);
        UiRect(cx - 4, cy - 3, 2, 11, c);
        break;
    case UI_ICON_SAVE:        // a floppy disk: its outline, the shutter and the label
        UiRoundRing(cx - 9, cy - 9, 18, 18, 2.5f, 2.0f, c);
        UiRect(cx - 4.5f, cy - 8, 8, 5, c);
        UiRoundRect(cx - 5, cy + 1.5f, 10, 5.5f, 1, c, c);
        break;
    }
}

// A side bar down the left of the screen: the LAIKA 64 logo, the page's
// title, its rows (each with its symbol, when it has one) and, at the
// bottom, the button prompts.
void UiMenuDraw(const UiMenu *m)
{
    static UiAnim sel;
    static char lastTitle[128];
    static int lastCount;
    const float x = 8, y = 12, w = 690, h = UI_HEIGHT - 24, head = 190, foot = 78;
    // Rows shrink a little when a page has many, so they stay above the prompts.
    const float room = h - head - 18 - foot;
    float rowH = m->count > 0 && room / m->count < 48 ? room / m->count : 48, sy, lx, ix;
    int i, restart = strcmp(lastTitle, m->title) != 0 || lastCount != m->count, rtl = LangRtl(), icons = 0;
    if (m->page == UI_PAGE_CONTROLS) { controls_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_CALIB) { calib_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_VOLUME) { volume_draw(m); lastTitle[0] = 0; return; }
    strncpy(lastTitle, m->title, sizeof(lastTitle) - 1);
    lastCount = m->count;
    for (i = 0; i < m->count; i++) icons |= m->icon[i];
    // Dimmed over the ROM list's own background (contrast); not in-game,
    // where this draws over the frozen frame and the dim hid graphics
    // changes (texture packs, filters) from showing clearly while paused.
    if (s_browser) UiDim();
    UiPanel(x, y, w, h, 22);
    UiBrand(x + 66, y + 60, 44);
    // Right to left (Arabic): title and labels on the right, values on the left.
    UiText(UI_BOLD, 30, rtl ? x + w - 66 : x + 66, y + 130, UI_COL_TEXT, m->title, rtl ? UI_RIGHT : UI_LEFT);
    sy = anim_step(&sel, y + head + m->sel * rowH, restart);
    if (m->count) selection_bar_side(x + 42, sy + 3, w - 66, rowH - 6, rtl);
    // With symbols, they take the label's place and the label moves over.
    ix = rtl ? x + w - 78 : x + 78;
    lx = icons ? (rtl ? ix - 24 : ix + 24) : (rtl ? x + w - 66 : x + 66);
    for (i = 0; i < m->count; i++)
    {
        float cy = y + head + i * rowH + rowH * 0.5f;
        int s = i == m->sel;
        D3DCOLOR col = s ? UI_COL_TEXT : UI_COL_TEXT2;
        if (m->icon[i]) menu_icon(m->icon[i], ix, cy, col);
        UiText(s ? UI_BOLD : UI_REGULAR, 22, lx, cy, col, m->label[i], rtl ? UI_RIGHT : UI_LEFT);
        if (!m->value[i][0]) continue;
        if (rtl) draw_value(x + 62 + value_width(m->value[i], s), cy, m->value[i], s);
        else draw_value(x + w - 40, cy, m->value[i], s);
    }
    UiRect(x + 46, y + h - foot, w - 70, 1, UI_COL_LINE);
    UiFooter(rtl ? x + w - 66 - UiFooterWidth(m->footer) : x + 66, y + h - foot * 0.5f, m->footer);
}

// A picture under the side bar's rows (the States page: the slot's save), in a
// 4:3 box as tall as the room left allows, framed like the selected cover
// (pulsing light blue ring and glow). NULL: an empty box with `emptyText`.
void UiMenuThumb(const UiMenu *m, IDirect3DTexture9 *tex, const char *emptyText)
{
    const float x = 8, y = 12, w = 690, h = UI_HEIGHT - 24, head = 190, foot = 78;
    const float room = h - head - 18 - foot;
    float rowH = m->count > 0 && room / m->count < 48 ? room / m->count : 48;
    float top = y + head + m->count * rowH + 28, bottom = y + h - foot - 28, bh = bottom - top, bw, bx, a = selection_pulse();
    if (bh < 60) return;
    bw = bh * 4.0f / 3.0f;
    if (bw > w - 132) { bw = w - 132; bh = bw * 0.75f; }
    bx = x + (w - bw) * 0.5f;
    selection_glow(bx - 4, top - 4, bw + 8, bh + 8, 16, a);
    UiRoundRing(bx - 4, top - 4, bw + 8, bh + 8, 16, 3.0f, SEL_RING(a));
    UiRoundRect(bx, top, bw, bh, 12, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    if (tex) UiImage(tex, bx, top, bw, bh, 12, D3DCOLOR_ARGB(255, 255, 255, 255));
    else if (emptyText) UiText(UI_REGULAR, 18, bx + bw * 0.5f, top + bh * 0.5f, UI_COL_TEXT3, emptyText, UI_CENTER);
}

void UiMenuNavigate(UiMenu *m, WORD down)
{
    if (!m->count) return;
    if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = (m->sel + m->count - 1) % m->count;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) m->sel = (m->sel + 1) % m->count;
}

// ---- Settings ----
// Main page: System, Graphics, Screen, Sound, Controls, Interface, save
// (B goes back). Graphics (preset, texture filter, hi-res textures, smoothing,
// sharpening, blur, FPS) and Screen (resolution, stretch, image calibration,
// brightness, VSync, antiflicker, screen effect) are shown in the same
// UiMenu as the main page; their Back returns to it.
enum { S_SYSTEM = 0, S_GRAPHICS, S_SCREEN, S_SOUND, S_CONTROLS, S_INTERFACE };
enum { V_GAME = 0, V_MUSIC, V_COUNT };   // the Volumen page: two bars
enum { I_DESIGN = 0, I_SKINS, I_LANGUAGE };   // the Interface page; B goes back, no row for it
#define I_ART (I_LANGUAGE + 1)       // "Box art downloads" (ROM list only)
// The ROM browser's layouts (Config::browserDesign; Interface > Design, X in the browser):
// the list and its card, the covers grid, the big covers grid, the carousel.
enum { BD_LIST = 0, BD_COVERS, BD_BIG, BD_CAROUSEL, BD_COUNT };
static const int s_designLabels[BD_COUNT] = { T_DESIGN_DETAILED, T_DESIGN_COVERS, T_DESIGN_BIG, T_DESIGN_CAROUSEL };
enum { SYS_CPU = 0, SYS_RSP, SYS_PADLOSS, SYS_AUTOSLOT, SYS_BATTERY };   // the System page: CPU and RSP (both apply at the next game start), what a controller unplugged in game does, the slot it saves to, the low battery warning; B goes back, no row for it
enum { SND_VOLUME = 0, SND_AUDIO };   // the Sound page: Volumen (opens the bars page), Margen de audio; B goes back, no row for it
enum { G_PRESET = 0, G_FILTER, G_HIRES, G_SMOOTH, G_SHARPEN, G_BLUR, G_FPS };   // the Graphics page; B goes back, no row for it
enum { SCR_RES = 0, SCR_STRETCH, SCR_CAL, SCR_BRIGHT, SCR_VSYNC, SCR_DEFLICKER, SCR_SCREEN };   // the Screen page (SCR_CAL opens the calibration page below); B goes back, no row for it
enum { CAL_SIZE = 0, CAL_LR, CAL_UD };   // the Image calibration page, opened from Screen > SCR_CAL; B goes back, no row for it

// Shown FPS levels (off, FPS, +CPU/RAM, +resolution, +GPU load) - the Graphics page's inline cycle.
static const int s_fpsLabels[5] = { T_OFF, T_FPS_ONLY, T_FPS_CPU, T_FPS_ALL, T_FPS_GPU };

// Graphics presets (the user's table): texture filter, smoothing, sharpening,
// blur, screen effect. The resolution stays as it is.
struct GfxPreset { int name; int texFilter, smooth, sharpen, blur, screen; };
static const GfxPreset s_presets[4] = {
    { T_PRESET_ORIGINAL, 2, 0, 0, 0, 0 },   // nearest, nothing added
    { T_PRESET_ENHANCED, 0, 1, 1, 0, 0 },   // 3-point, light sharpening
    { T_PRESET_SMOOTH, 0, 1, 1, 0, 5 },     // 3-point, light sharpening, light scanlines
    { T_PRESET_CRT, 0, 0, 0, 0, 2 },        // 3-point, CRT: scanlines and glow
};

// The preset the settings match (-1: custom).
static int current_preset(const Config *c)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        const GfxPreset *p = &s_presets[i];
        if (c->texFilter == p->texFilter && c->smooth == p->smooth && c->sharpen == p->sharpen && c->blur == p->blur &&
            c->screen == p->screen)
            return i;
    }
    return -1;
}
static int s_graphicsPage;
static int s_screenPage;
static int s_calPage;
static int s_interfacePage;
static int s_soundPage;
static int s_systemPage;
static int s_perGame;
static char s_langPath[160];   // where a language change is kept (SettingsSetLangFile)
#define S_SAVE_ALL (S_INTERFACE + 1)

static const char *onoff(int v) { return Tr(v ? T_ON : T_OFF); }

void SettingsSetLangFile(const char *path)
{
    strncpy(s_langPath, path ? path : "", sizeof(s_langPath) - 1);
    s_langPath[sizeof(s_langPath) - 1] = 0;
}

// The menus in another language, kept in the settings file at once.
static void lang_change(Config *c, int lang)
{
    LangSet(lang);
    strcpy(c->lang, LangCode(lang));
    if (s_langPath[0] && !ConfigWriteLang(s_langPath, c->lang)) H64_WARN("[lang] cannot write %s", s_langPath);
    H64_INFO("[lang] menus in %s", c->lang);
}

static void settings_values(UiMenu *m, const Config *c)
{
    if (s_interfacePage)
    {
        int sn = SkinCount(), si = 0, k;
        sprintf(m->value[I_DESIGN], "< %s >", Tr(s_designLabels[c->browserDesign >= 0 && c->browserDesign < BD_COUNT ? c->browserDesign : 0]));
        if (!c->skinName[0] || !sn) sprintf(m->value[I_SKINS], "< %s >", Tr(T_SKIN_NONE));
        else
        {
            for (k = 0; k < sn; k++)
                if (!strcmp(SkinName(k), c->skinName)) { si = k; break; }
            if (sn > 1) sprintf(m->value[I_SKINS], "< %s (%d/%d) >", SkinName(si), si + 1, sn);
            else sprintf(m->value[I_SKINS], "< %s >", SkinName(si));
        }
        sprintf(m->value[I_LANGUAGE], "< %s >", Tr(T_LANG_NAME));
        if (s_browser) sprintf(m->value[I_ART], "< %s >", onoff(c->artDownload));
        return;
    }
    if (s_systemPage)
    {
        sprintf(m->value[SYS_CPU], "< %s >", strcmp(c->cpu, "interp") ? Tr(T_RECOMPILER) : Tr(T_INTERPRETER));
        sprintf(m->value[SYS_RSP], "< %s >", c->hle ? "HLE" : "LLE");
        sprintf(m->value[SYS_PADLOSS], "< %s >", Tr(c->padLoss == PADLOSS_SAVE ? T_PAD_LOSS_SAVE : c->padLoss == PADLOSS_PAUSE ? T_PAD_LOSS_PAUSE : T_OFF));
        sprintf(m->value[SYS_AUTOSLOT], "< %d >", c->autoSaveSlot);
        if (c->batteryWarn) sprintf(m->value[SYS_BATTERY], "< %d%% >", c->batteryWarn);
        else sprintf(m->value[SYS_BATTERY], "< %s >", Tr(T_OFF));
        return;
    }
    if (s_soundPage)
    {
        sprintf(m->value[SND_AUDIO], "< %d ms >", c->audioMs);
        return;
    }
    if (s_calPage)
    {
        sprintf(m->value[CAL_SIZE], "< %d%% >", c->zoom);
        sprintf(m->value[CAL_LR], "< %+d >", c->offsetX);
        sprintf(m->value[CAL_UD], "< %+d >", c->offsetY);
        return;
    }
    if (s_screenPage)
    {
        const char *res[4] = { Tr(T_RES_NATIVE), "x2 640x480", "x3 960x720", "HD 1280x720 (16:9)" };
        const char *scr[6] = { Tr(T_SCREEN_NONE), Tr(T_SCANLINES), Tr(T_CRT), Tr(T_CRT_CURVED), Tr(T_LCD_GRID), Tr(T_LIGHT_SCANLINES) };
        const char *br[3] = { Tr(T_NORMAL), Tr(T_LIGHTER), Tr(T_MUCH_LIGHTER) };
        sprintf(m->value[SCR_RES], "< %s >", res[(c->resScale < 1 ? 1 : c->resScale > 4 ? 4 : c->resScale) - 1]);
        sprintf(m->value[SCR_STRETCH], "< %s >", onoff(c->stretch));
        sprintf(m->value[SCR_BRIGHT], "< %s >", br[c->brightness < 0 || c->brightness > 2 ? 0 : c->brightness]);
        sprintf(m->value[SCR_VSYNC], "< %s >", onoff(c->vsync));
        sprintf(m->value[SCR_DEFLICKER], "< %s >", onoff(c->deflicker));
        sprintf(m->value[SCR_SCREEN], "< %s >", scr[c->screen < 0 || c->screen > 5 ? 0 : c->screen]);
        return;
    }
    if (s_graphicsPage)
    {
        const char *filt[3] = { Tr(T_FILTER_3POINT), Tr(T_FILTER_BILINEAR), Tr(T_FILTER_NEAREST) };
        const char *lvl[3] = { Tr(T_OFF), Tr(T_LOW), Tr(T_HIGH) };
        const char *blr[3] = { Tr(T_OFF), Tr(T_SOFT), Tr(T_STRONG) };
        int pr = current_preset(c);
        sprintf(m->value[G_PRESET], "< %s >", pr < 0 ? Tr(T_CUSTOM) : Tr(s_presets[pr].name));
        sprintf(m->value[G_FILTER], "< %s >", filt[c->texFilter < 0 || c->texFilter > 2 ? 1 : c->texFilter]);
        {
            // Off, or the chosen (or first) pack's file name without ".hts", with a
            // (i/N) counter when the game has more than one - hiTexFile picks which
            // (see HiresScanPacks/HiresOpen); the list itself always matches whatever
            // ROM is selected in the browser or running in game.
            int hn = HiresPackCount(), hi = 0, k;
            if (!c->hiTex || !hn) sprintf(m->value[G_HIRES], "< %s >", Tr(T_OFF));
            else
            {
                char name[40];
                const char *p;
                size_t nlen;
                if (c->hiTexFile[0])
                    for (k = 0; k < hn; k++)
                        if (!strcmp(HiresPackName(k), c->hiTexFile)) { hi = k; break; }
                p = HiresPackName(hi);
                nlen = strlen(p);
                if (nlen > 4 && !strcmp(p + nlen - 4, ".hts")) nlen -= 4;
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                memcpy(name, p, nlen);
                name[nlen] = 0;
                if (hn > 1) sprintf(m->value[G_HIRES], "< %s (%d/%d) >", name, hi + 1, hn);
                else sprintf(m->value[G_HIRES], "< %s >", name);
            }
        }
        sprintf(m->value[G_SMOOTH], "< %s >", onoff(c->smooth));
        sprintf(m->value[G_SHARPEN], "< %s >", lvl[c->sharpen < 0 || c->sharpen > 2 ? 0 : c->sharpen]);
        sprintf(m->value[G_BLUR], "< %s >", blr[c->blur < 0 || c->blur > 2 ? 0 : c->blur]);
        sprintf(m->value[G_FPS], "< %s >", Tr(s_fpsLabels[c->showFps < 0 || c->showFps > 4 ? 0 : c->showFps]));
        return;
    }
    {
        int i, custom = 0;
        for (i = 0; i < 4; i++) custom |= !h64_padmap_is_default(&c->padMap[i]);
        strcpy(m->value[S_CONTROLS], custom ? Tr(T_CUSTOM) : Tr(T_DEFAULT));
    }
}

static void settings_main(UiMenu *m, const Config *c)
{
    s_graphicsPage = 0;
    s_screenPage = 0;
    s_calPage = 0;
    s_interfacePage = 0;
    s_soundPage = 0;
    s_systemPage = 0;
    static const unsigned char icons[] = { UI_ICON_SYSTEM, UI_ICON_GRAPHICS, UI_ICON_SCREEN, UI_ICON_SOUND, UI_ICON_CONTROLS,
                                           UI_ICON_INTERFACE, UI_ICON_SAVE, UI_ICON_SAVE };
    int i;
    UiMenuClear(m, Tr(T_SETTINGS), Tr(T_FOOT_SELECT));   // just "Settings", from the ROM list too
    UiMenuAdd(m, Tr(T_SYSTEM), "");
    UiMenuAdd(m, Tr(T_GRAPHICS), "");
    UiMenuAdd(m, Tr(T_SCREEN), "");
    UiMenuAdd(m, Tr(T_SOUND), "");
    UiMenuAdd(m, Tr(T_CONTROLS), "");
    UiMenuAdd(m, Tr(T_INTERFACE), "");
    UiMenuAdd(m, Tr(T_SAVE_ALL), "");
    if (s_perGame) UiMenuAdd(m, Tr(T_SAVE_GAME), "");
    for (i = 0; i < m->count; i++) m->icon[i] = icons[i];
    settings_values(m, c);   // no Back row: B goes back
}

static void settings_interface(UiMenu *m, const Config *c)
{
    s_interfacePage = 1;
    // The menu's own state (title, rows, count) first, as a tight, I/O-free
    // sequence - SkinScan's directory listing (disk I/O, unlike every other
    // field here) runs only after, so nothing can observe *m mid-rebuild
    // while this thread is possibly waiting on the filesystem.
    UiMenuClear(m, Tr(T_INTERFACE), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_DESIGN), "");
    UiMenuAdd(m, Tr(T_SKINS), "");
    UiMenuAdd(m, Tr(T_LANGUAGE), "");
    if (s_browser) UiMenuAdd(m, Tr(T_ART_DOWNLOADS), "");
    SkinScan();   // keeps the inline Skins picker below matching the game:\skins folder
    settings_values(m, c);
}

// Design and Skins both cycle in place (dir or A moves them on) - no sub-page,
// same as every other "< value >" row in this menu.
static int interface_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_INTERFACE;
        return SET_NONE;
    }
    if (m->sel == I_DESIGN && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        c->browserDesign = ((c->browserDesign >= 0 && c->browserDesign < BD_COUNT ? c->browserDesign : 0) + dir + BD_COUNT) % BD_COUNT;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_SKINS && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        int sn = SkinCount(), states = sn + 1, cur = 0, k;
        if (!dir) dir = 1;
        if (c->skinName[0] && sn)
            for (k = 0; k < sn; k++)
                if (!strcmp(SkinName(k), c->skinName)) { cur = k + 1; break; }
        cur = (cur + dir + states) % states;
        if (cur == 0) c->skinName[0] = 0;
        else
        {
            strncpy(c->skinName, SkinName(cur - 1), sizeof(c->skinName) - 1);
            c->skinName[sizeof(c->skinName) - 1] = 0;
        }
        UiSetSkinWant(c->skinName);
        if (c->skinName[0]) xb_audio_music_play(c->skinName);
        else xb_audio_music_stop();
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_LANGUAGE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // From the ROM list, A opens the list with the flags; left/right (and A in game) step through the languages.
        if (s_browser && !dir) return SET_LANGUAGE;
        lang_change(c, (LangGet() + (dir ? dir : 1) + LANG_COUNT) % LANG_COUNT);
        settings_interface(m, c);   // a language change redraws every label, not just the values settings_values() touches
        m->sel = I_LANGUAGE;
        return SET_NONE;
    }
    if (s_browser && m->sel == I_ART && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!c->artDownload) return SET_ART_ASK;   // turning downloads on asks first (settings_loop)
        c->artDownload = 0;
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

// ---- System page: CPU and RSP (recompiler/interpreter, HLE/LLE) - both
// apply at the next game start, not live; what a controller unplugged during
// a game does (off, save and pause, pause: main.cpp) and the slot that save
// goes to (kept for it: manual saves skip it); the low battery warning.
static void settings_system(UiMenu *m, const Config *c)
{
    s_systemPage = 1;
    UiMenuClear(m, Tr(T_SYSTEM), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_CPU_NEXT), "");
    UiMenuAdd(m, Tr(T_RSP_NEXT), "");
    UiMenuAdd(m, Tr(T_PAD_LOSS), "");
    UiMenuAdd(m, Tr(T_AUTOSAVE_SLOT), "");
    UiMenuAdd(m, Tr(T_BATTERY_WARN), "");
    settings_values(m, c);
}

static int system_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_SYSTEM;
        return SET_NONE;
    }
    if (m->sel == SYS_CPU && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        strcpy(c->cpu, strcmp(c->cpu, "interp") ? "interp" : "dynarec");
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SYS_RSP && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->hle = !c->hle;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if ((m->sel == SYS_PADLOSS || m->sel == SYS_AUTOSLOT) && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        if (m->sel == SYS_PADLOSS) c->padLoss = (c->padLoss + dir + PADLOSS_COUNT) % PADLOSS_COUNT;
        else c->autoSaveSlot = (c->autoSaveSlot - 1 + dir + 9) % 9 + 1;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SYS_BATTERY && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        static const int levels[4] = { 0, 10, 25, 50 };
        int k, cur = 0;
        for (k = 0; k < 4; k++)
            if (levels[k] == c->batteryWarn) cur = k;
        c->batteryWarn = levels[(cur + (dir ? dir : 1) + 4) % 4];
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

// ---- Volumen page: two bars (game, interface music) - DPAD up/down picks
// one, left/right moves it by 5. Its own UI_PAGE_VOLUME draw, since a plain
// list row can't show a real bar (see volume_draw, UiMenuDraw's dispatch).
static const Config *s_volumeCfg;
static void settings_sound(UiMenu *m, const Config *c);   // volume_input's B returns here (opened from the Sound page)

static void settings_volume(UiMenu *m, const Config *c)
{
    UiMenuClear(m, Tr(T_VOLUME), Tr(T_FOOT_SELECT));
    m->page = UI_PAGE_VOLUME;
    m->count = V_COUNT;
    m->sel = 0;
    s_volumeCfg = c;
}

static int volume_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_sound(m, c);
        m->sel = SND_VOLUME;
        return SET_NONE;
    }
    if (dir)
    {
        int *v = m->sel == V_MUSIC ? &c->volumeMusic : &c->volumeGame;
        *v += dir * 5;
        if (*v < 0) *v = 0;
        if (*v > 100) *v = 100;
        if (m->sel == V_MUSIC) xb_audio_set_music_volume(c->volumeMusic);
        else xb_audio_set_game_volume(c->volumeGame);
        return SET_CHANGED;
    }
    return SET_NONE;
}

static void volume_bar_draw(float x, float y, float w, const char *label, int percent, int selected)
{
    const float barH = 28, barY = y + 34;
    char pct[16];
    float fillW;
    UiText(UI_BOLD, 20, x, y, selected ? UI_COL_TEXT : UI_COL_TEXT2, label, UI_LEFT);
    _snprintf(pct, sizeof(pct) - 1, "%d%%", percent);
    pct[sizeof(pct) - 1] = 0;
    UiText(UI_BOLD, 20, x + w, y, selected ? UI_COL_TEXT : UI_COL_TEXT2, pct, UI_RIGHT);
    UiRoundRect(x, barY, w, barH, barH * 0.5f, D3DCOLOR_ARGB(255, 34, 36, 47), D3DCOLOR_ARGB(255, 27, 29, 38));
    fillW = w * (percent / 100.0f);
    if (fillW > 0)
    {
        if (fillW < barH) fillW = barH;   // keeps the rounded cap from looking clipped at tiny values
        UiRoundRect(x, barY, fillW, barH, barH * 0.5f, D3DCOLOR_ARGB(255, 150, 220, 255), D3DCOLOR_ARGB(255, 70, 160, 240));
    }
    if (selected)
    {
        // The bar being changed: the selection's pulsing light blue ring and glow.
        float a = selection_pulse();
        selection_glow(x, barY, w, barH, barH * 0.5f, a);
        UiRoundRing(x, barY, w, barH, barH * 0.5f, 2.0f, SEL_RING(a));
    }
    else UiRoundRing(x, barY, w, barH, barH * 0.5f, 1.0f, UI_COL_LINE);
}

static void volume_draw(const UiMenu *m)
{
    const float w = 660, foot = 78, h = 100 + 2 * 90 + 18 + foot;
    const float x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
    const Config *c = s_volumeCfg;
    UiDim();
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, m->title, UI_LEFT);
    volume_bar_draw(x + 44, y + 120, w - 88, Tr(T_VOLUME_GAME), c ? c->volumeGame : 100, m->sel == V_GAME);
    volume_bar_draw(x + 44, y + 210, w - 88, Tr(T_VOLUME_MUSIC), c ? c->volumeMusic : 100, m->sel == V_MUSIC);
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- Sound page: Volumen (opens the bars page above) and Margen de audio
// (an inline "< N ms >" cycle, moved here from the main page).
static void settings_sound(UiMenu *m, const Config *c)
{
    s_soundPage = 1;
    UiMenuClear(m, Tr(T_SOUND), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_VOLUME), "");
    UiMenuAdd(m, Tr(T_AUDIO_MARGIN), "");
    settings_values(m, c);
}

static int sound_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_SOUND;
        return SET_NONE;
    }
    if (m->sel == SND_VOLUME && (down & XINPUT_GAMEPAD_A))
    {
        settings_volume(m, c);
        return SET_NONE;
    }
    if (m->sel == SND_AUDIO && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        c->audioMs += dir * 50;
        if (c->audioMs < 50) c->audioMs = 50;
        if (c->audioMs > 500) c->audioMs = 500;
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

static void settings_graphics(UiMenu *m, const Config *c)
{
    s_graphicsPage = 1;
    UiMenuClear(m, Tr(T_GRAPHICS), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PRESET), "");
    UiMenuAdd(m, Tr(T_TEXFILTER), "");
    UiMenuAdd(m, Tr(T_HIRESTEX), "");
    UiMenuAdd(m, Tr(T_FXAA), "");
    UiMenuAdd(m, Tr(T_SHARPEN), "");
    UiMenuAdd(m, Tr(T_BLUR), "");
    UiMenuAdd(m, Tr(T_SHOW_FPS), "");
    settings_values(m, c);
}

// ---- Screen page: resolution, stretch, the image calibration sub-page,
// brightness, VSync, antiflicker, screen effect.
static void settings_screen(UiMenu *m, const Config *c)
{
    s_screenPage = 1;
    s_calPage = 0;   // rebuilding Screen (fresh, or back from Calibration): Calibration is definitely not showing
    UiMenuClear(m, Tr(T_SCREEN), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_RESOLUTION), "");
    UiMenuAdd(m, Tr(T_STRETCH), "");
    UiMenuAdd(m, Tr(T_PIC_CAL), "");
    UiMenuAdd(m, Tr(T_BRIGHTNESS), "");
    UiMenuAdd(m, Tr(T_VSYNC), "");
    UiMenuAdd(m, Tr(T_ANTIFLICKER), "");
    UiMenuAdd(m, Tr(T_SCREEN_EFFECT), "");
    settings_values(m, c);
}

// ---- Image calibration page: size, left/right, up/down - opened from Screen > SCR_CAL.
static void settings_cal(UiMenu *m, const Config *c)
{
    s_calPage = 1;
    UiMenuClear(m, Tr(T_PIC_CAL), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PIC_SIZE), "");
    UiMenuAdd(m, Tr(T_PIC_LR), "");
    UiMenuAdd(m, Tr(T_PIC_UD), "");
    settings_values(m, c);
}

static int screen_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_SCREEN;
        return SET_NONE;
    }
    if (m->sel == SCR_CAL && (down & XINPUT_GAMEPAD_A))
    {
        settings_cal(m, c);
        return SET_NONE;
    }
    if (dir || (down & XINPUT_GAMEPAD_A))
    {
        if (!dir) dir = 1;
        switch (m->sel)
        {
        case SCR_RES: c->resScale = (c->resScale - 1 + dir + 4) % 4 + 1; break;
        case SCR_STRETCH: c->stretch = !c->stretch; break;
        case SCR_BRIGHT: c->brightness = (c->brightness + dir + 3) % 3; break;
        case SCR_VSYNC: c->vsync = !c->vsync; break;
        case SCR_DEFLICKER: c->deflicker = !c->deflicker; break;
        case SCR_SCREEN: c->screen = (c->screen + dir + 6) % 6; break;
        default: return SET_NONE;   // SCR_CAL with a left/right press: no value of its own to cycle
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

static int cal_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_screen(m, c);
        m->sel = SCR_CAL;
        return SET_NONE;
    }
    if (dir || (down & XINPUT_GAMEPAD_A))
    {
        if (!dir) dir = 1;
        switch (m->sel)
        {
        case CAL_SIZE: c->zoom += dir * 2; if (c->zoom < 80) c->zoom = 80; if (c->zoom > 140) c->zoom = 140; break;
        case CAL_LR: c->offsetX += dir * 4; if (c->offsetX < -160) c->offsetX = -160; if (c->offsetX > 160) c->offsetX = 160; break;
        case CAL_UD: c->offsetY += dir * 4; if (c->offsetY < -120) c->offsetY = -120; if (c->offsetY > 120) c->offsetY = 120; break;
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

// ---- Controls page ----
// Which Xbox 360 input drives each N64 control, per player (LB/RB), and the
// analog stick's source (a stick or the D-pad), inverted axes, dead zone and sensitivity. A on a button waits for
// an input to set it (Y adds one more, X clears); BACK or 5 s cancel. The
// D-pad moves freely over the three columns; on a value row A or RT steps the
// value on, LT back. Auto setup (after a question) asks for the N64 buttons (all but Start) in turn.
// Inputs are read from controller 1, whichever player is being edited.
// C_CALIB opens the stick calibration page (dead zone, sensitivity, curve,
// inversion, centre: see "Stick calibration" below).
enum { C_STICK = H64_N64_COUNT, C_CALIB, C_SHOTCOMBO, C_RUMBLE, C_RESET, C_AUTO, C_COUNT };
#define CAPTURE_MS 5000

struct ControlsState
{
    Config *c;
    int player;          // 0..3
    int capturing;       // waiting for an input for item `capItem`
    int capItem, capAdd, capReleased;
    int capStage;        // C_SHOTCOMBO only: 0 waiting for the first button, 1 the second
    DWORD capStart;
    int autoMode;        // 0, AUTO_ASK (the question is up) or AUTO_RUN (asking for s_ctlOrder[autoIdx])
    int autoIdx;
    H64PadMap autoSaved; // the player's mapping before auto setup (back when it is cancelled)
    int trigPrev;        // LT (1) and RT (2) held at the last look (ctl_triggers)
    char msg[160];       // a short note under the panel ("D-pad up set")
    DWORD msgAt;
};
static ControlsState s_ctl;
enum { AUTO_ASK = 1, AUTO_RUN };

// The items (the N64 buttons first), and where each one is shown.
static const int s_ctlOrder[C_COUNT] = {
    H64_N64_A, H64_N64_B, H64_N64_Z, H64_N64_START, H64_N64_L, H64_N64_R,
    H64_N64_DUP, H64_N64_DDOWN, H64_N64_DLEFT, H64_N64_DRIGHT, H64_N64_CUP, H64_N64_CDOWN, H64_N64_CLEFT, H64_N64_CRIGHT,
    C_STICK, C_CALIB, C_SHOTCOMBO, C_RUMBLE, C_RESET, C_AUTO,
};
// What auto setup asks for, in order: every N64 button but Start (the user's
// choice: it keeps its mapping).
static const int s_autoOrder[] = {
    H64_N64_A, H64_N64_B, H64_N64_Z, H64_N64_L, H64_N64_R,
    H64_N64_DUP, H64_N64_DDOWN, H64_N64_DLEFT, H64_N64_DRIGHT, H64_N64_CUP, H64_N64_CDOWN, H64_N64_CLEFT, H64_N64_CRIGHT,
};
#define AUTO_COUNT ((int)(sizeof(s_autoOrder) / sizeof(s_autoOrder[0])))

#define CTL_X 70.0f
#define CTL_Y 55.0f
#define CTL_W 1140.0f
#define CTL_H 640.0f   // +30 over the original 610: room for C_SHOTCOMBO at row 9 (see ctl_place, draw_n64_pad's caller)
#define CTL_ROW 40.0f
#define CTL_TOP 150.0f       // first row's top, from the panel's
#define CTL_COLW 352.0f
#define CTL_GAP 20.0f

// Column and row slot (0-based, in rows of CTL_ROW; headings take half a row) of an item.
static void ctl_place(int item, int *col, float *row)
{
    switch (item)
    {
    case H64_N64_A: *col = 0; *row = 0; return;
    case H64_N64_B: *col = 0; *row = 1; return;
    case H64_N64_Z: *col = 0; *row = 2; return;
    case H64_N64_START: *col = 0; *row = 3; return;
    case H64_N64_L: *col = 0; *row = 4; return;
    case H64_N64_R: *col = 0; *row = 5; return;
    case H64_N64_DUP: *col = 1; *row = 0; return;
    case H64_N64_DDOWN: *col = 1; *row = 1; return;
    case H64_N64_DLEFT: *col = 1; *row = 2; return;
    case H64_N64_DRIGHT: *col = 1; *row = 3; return;
    case H64_N64_CUP: *col = 1; *row = 5; return;
    case H64_N64_CDOWN: *col = 1; *row = 6; return;
    case H64_N64_CLEFT: *col = 1; *row = 7; return;
    case H64_N64_CRIGHT: *col = 1; *row = 8; return;
    case C_STICK: *col = 2; *row = 0; return;
    case C_CALIB: *col = 2; *row = 1; return;
    case C_SHOTCOMBO: *col = 2; *row = 9; return;   // below the N64 pad drawing (see draw_n64_pad's caller)
    case C_RUMBLE: *col = 0; *row = 6; return;
    case C_RESET: *col = 0; *row = 7; return;
    default: *col = 0; *row = 8; return;   // C_AUTO
    }
}

// LT (1) and RT (2) newly pressed on controller 1 (UiInputPoll's bits have no triggers).
static int ctl_triggers(void)
{
    XINPUT_STATE st;
    int held = 0, down;
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
        held = (st.Gamepad.bLeftTrigger > 128 ? 1 : 0) | (st.Gamepad.bRightTrigger > 128 ? 2 : 0);
    down = held & ~s_ctl.trigPrev;
    s_ctl.trigPrev = held;
    return down;
}

// D-pad on this page: up/down to the next item of the same column (round at
// its ends), left/right to the next column, on the row nearest the same height.
static void ctl_move(UiMenu *m, WORD down)
{
    int dx = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    int dy = (down & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (down & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
    int col, best = -1, wrap = -1, i;
    float row, bestD = 1e9f, wrapRow = 0;
    if (!dx && !dy) return;
    ctl_place(s_ctlOrder[m->sel], &col, &row);
    if (!dy) col = (col + dx + 3) % 3;
    for (i = 0; i < C_COUNT; i++)
    {
        int c2;
        float r2, d;
        ctl_place(s_ctlOrder[i], &c2, &r2);
        if (c2 != col || i == m->sel) continue;
        if (dy)
        {
            // The nearest one that way; none: the far end the other way.
            d = (r2 - row) * dy;
            if (d > 0 && d < bestD) { bestD = d; best = i; }
            if (wrap < 0 || (r2 - wrapRow) * dy < 0) { wrap = i; wrapRow = r2; }
        }
        else
        {
            d = r2 > row ? r2 - row : row - r2;
            if (d < bestD) { bestD = d; best = i; }
        }
    }
    if (best < 0) best = wrap;
    if (best >= 0) m->sel = best;
}

static float ctl_col_x(int col) { return CTL_X + 24 + col * (CTL_COLW + CTL_GAP); }
static float ctl_row_y(float row) { return CTL_Y + CTL_TOP + row * CTL_ROW; }

static void controls_open(UiMenu *m, Config *c)
{
    UiMenuClear(m, Tr(T_CONTROLS), "");
    m->page = UI_PAGE_CONTROLS;
    m->count = C_COUNT;
    m->sel = 0;
    s_ctl.c = c;
    s_ctl.capturing = 0;
    s_ctl.autoMode = 0;
    s_ctl.msg[0] = 0;
    s_ctl.trigPrev = 3;   // a trigger already held when the page opens does not count
    if (s_ctl.player < 0 || s_ctl.player > 3) s_ctl.player = 0;
}

static void ctl_note(const char *text)
{
    strncpy(s_ctl.msg, text, sizeof(s_ctl.msg) - 1);
    s_ctl.msg[sizeof(s_ctl.msg) - 1] = 0;
    s_ctl.msgAt = GetTickCount();
}

// Controller 1's state as the mapping's input.
static int ctl_read(H64PadInput *in)
{
    XINPUT_STATE st;
    if (XInputGetState(0, &st) != ERROR_SUCCESS) return 0;
    in->buttons = st.Gamepad.wButtons;
    in->lt = st.Gamepad.bLeftTrigger;
    in->rt = st.Gamepad.bRightTrigger;
    in->lx = st.Gamepad.sThumbLX;
    in->ly = st.Gamepad.sThumbLY;
    in->rx = st.Gamepad.sThumbRX;
    in->ry = st.Gamepad.sThumbRY;
    return 1;
}

// While waiting for an input: everything released first (the A that started
// it), then the first input pressed. BACK cancels, as does the time out.
static int controls_capture(void)
{
    H64PadInput in;
    u32 src = 0;
    H64PadMap *map = &s_ctl.c->padMap[s_ctl.player];
    int i;
    char note[160];
    if (ctl_read(&in))
    {
        if (in.buttons & XINPUT_GAMEPAD_BACK)
        {
            int restored = s_ctl.autoMode == AUTO_RUN;
            if (restored) *map = s_ctl.autoSaved;   // auto setup cancelled: the mapping it started from
            s_ctl.capturing = 0;
            s_ctl.autoMode = 0;
            ctl_note(Tr(T_CANCELLED));
            return restored ? SET_CHANGED : SET_NONE;
        }
        src = h64_padmap_sources(&in);
    }
    if (!s_ctl.capReleased)
    {
        if (!src) { s_ctl.capReleased = 1; s_ctl.capStart = GetTickCount(); }
        return SET_NONE;
    }
    if (s_ctl.autoMode != AUTO_RUN && GetTickCount() - s_ctl.capStart >= CAPTURE_MS)   // auto setup waits for as long as it takes
    {
        s_ctl.capturing = 0;
        ctl_note(Tr(T_NO_INPUT));
        return SET_NONE;
    }
    if (!src) return SET_NONE;
    for (i = 0; i < H64_SRC_COUNT; i++)
        if (src & (1u << i)) break;
    if (s_ctl.capItem == C_SHOTCOMBO)
    {
        _snprintf(note, sizeof(note) - 1, Tr(T_INPUT_SET), Tr(T_SCREENSHOT_COMBO), TrSource(i));
        note[sizeof(note) - 1] = 0;
        ctl_note(note);
        if (s_ctl.capStage == 0)
        {
            // The first button: keep capturing (capturing stays 1) for the second one.
            s_ctl.c->shotBtn1 = i;
            s_ctl.capStage = 1;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
            return SET_CHANGED;
        }
        s_ctl.c->shotBtn2 = i;
        s_ctl.capturing = 0;
        return SET_CHANGED;
    }
    s_ctl.capturing = 0;
    if (s_ctl.capAdd) map->src[s_ctl.capItem] |= 1u << i;
    else map->src[s_ctl.capItem] = 1u << i;
    _snprintf(note, sizeof(note) - 1, Tr(s_ctl.capAdd ? T_INPUT_ADDED : T_INPUT_SET), TrN64(s_ctl.capItem), TrSource(i));
    note[sizeof(note) - 1] = 0;
    ctl_note(note);
    if (s_ctl.autoMode == AUTO_RUN)
    {
        // Auto setup: on to the next N64 button, once this input is let go.
        if (++s_ctl.autoIdx < AUTO_COUNT)
        {
            s_ctl.capturing = 1;
            s_ctl.capItem = s_autoOrder[s_ctl.autoIdx];
            s_ctl.capReleased = 0;
        }
        else
        {
            s_ctl.autoMode = 0;
            ctl_note(Tr(T_AUTO_DONE));
        }
    }
    return SET_CHANGED;
}

static void calib_open(UiMenu *m, Config *c);   // the stick calibration page, below

static int controls_input(UiMenu *m, Config *c, WORD down)
{
    H64PadMap *map;
    int item, dir, trig;
    s_ctl.c = c;
    map = &c->padMap[s_ctl.player];
    trig = ctl_triggers();   // every frame, so a trigger used as an input while capturing is not taken as a press later
    if (s_ctl.capturing) return controls_capture();
    if (s_ctl.autoMode == AUTO_ASK)
    {
        // "Start auto setup?": A starts it on the first button, B leaves it.
        if (down & XINPUT_GAMEPAD_A)
        {
            s_ctl.autoSaved = *map;
            s_ctl.autoMode = AUTO_RUN;
            s_ctl.autoIdx = 0;
            s_ctl.capturing = 1;
            s_ctl.capItem = s_autoOrder[0];
            s_ctl.capAdd = 0;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
        }
        else if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) s_ctl.autoMode = 0;
        return SET_NONE;
    }
    item = s_ctlOrder[m->sel];
    // A and RT step a value on, LT back (the D-pad moves between the rows and columns).
    dir = ((down & XINPUT_GAMEPAD_A) || (trig & 2)) ? 1 : (trig & 1) ? -1 : 0;
    ctl_move(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_CONTROLS;
        return SET_NONE;
    }
    if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
    {
        s_ctl.player = (s_ctl.player + ((down & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 3)) % 4;
        return SET_NONE;
    }
    if (item < H64_N64_COUNT)
    {
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_Y))
        {
            s_ctl.capturing = 1;
            s_ctl.capItem = item;
            s_ctl.capAdd = (down & XINPUT_GAMEPAD_A) ? 0 : 1;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
            return SET_NONE;
        }
        if (down & XINPUT_GAMEPAD_X)
        {
            map->src[item] = 0;
            return SET_CHANGED;
        }
    }
    if (item == C_SHOTCOMBO && (down & XINPUT_GAMEPAD_A))
    {
        s_ctl.capturing = 1;
        s_ctl.capItem = item;
        s_ctl.capAdd = 0;   // one source, not a mask: always replaces
        s_ctl.capStage = 0;   // the first of the two buttons
        s_ctl.capReleased = 0;
        s_ctl.capStart = GetTickCount();
        return SET_NONE;
    }
    if (item == C_SHOTCOMBO && (down & XINPUT_GAMEPAD_X))
    {
        c->shotBtn1 = c->shotBtn2 = -1;
        return SET_CHANGED;
    }
    if (item == C_RESET && (down & XINPUT_GAMEPAD_A))
    {
        h64_padmap_default(map);
        ctl_note(Tr(T_DEFAULT_CONTROLS));
        return SET_CHANGED;
    }
    if (item == C_AUTO && (down & XINPUT_GAMEPAD_A))
    {
        s_ctl.autoMode = AUTO_ASK;
        return SET_NONE;
    }
    if (item == C_RUMBLE && dir)
    {
        c->rumble = !c->rumble;   // one setting for every player
        return SET_CHANGED;
    }
    if (item == C_STICK && dir)
    {
        // Round at the ends: A alone reaches every value.
        map->stick = (map->stick + dir + H64_STICK_COUNT) % H64_STICK_COUNT;
        return SET_CHANGED;
    }
    if (item == C_CALIB && (down & XINPUT_GAMEPAD_A))
    {
        calib_open(m, c);
        return SET_NONE;
    }
    return SET_NONE;
}

// The inputs of a mask as button prompts ("X/Y/LT/RT|DPAD:Up"), or "" when none.
static void ctl_hints(u32 mask, char *out, size_t size)
{
    static const char *const glyph[H64_SRC_COUNT] = {
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "DPAD", "DPAD", "DPAD", "DPAD",
        "LS", "RS", "LS", "LS", "LS", "LS", "RS", "RS", "RS", "RS",
    };
    static const int label[H64_SRC_COUNT] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT,
        T_CLICK, T_CLICK, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT, T_DIR_UP, T_DIR_DOWN, T_DIR_LEFT, T_DIR_RIGHT,
    };
    int pass, i;
    out[0] = 0;
    // Plain glyphs first, sharing one prompt ("X/Y/LT"), then the labelled ones.
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < H64_SRC_COUNT; i++)
        {
            char item[96];
            size_t len = strlen(out);
            int labelled = label[i] >= 0;
            if (!(mask & (1u << i)) || labelled != pass) continue;
            if (labelled) _snprintf(item, sizeof(item) - 1, "%s:%s", glyph[i], Tr(label[i]));
            else strcpy(item, glyph[i]);
            item[sizeof(item) - 1] = 0;
            if (len + strlen(item) + 2 > size) return;
            if (len) strcat(out, pass ? "|" : "/");
            strcat(out, item);
        }
}

static void ctl_value_text(int item, const H64PadMap *map, char *out)
{
    if (item == C_RUMBLE) { sprintf(out, "< %s >", onoff(s_ctl.c ? s_ctl.c->rumble : 1)); return; }
    const char *sticks[H64_STICK_COUNT] = { Tr(T_STICK_LEFT), Tr(T_STICK_RIGHT), Tr(T_STICK_DPAD), Tr(T_OFF) };
    if (item == C_STICK) sprintf(out, "< %s >", sticks[map->stick >= 0 && map->stick < H64_STICK_COUNT ? map->stick : 0]);
    else if (item == C_SHOTCOMBO)
    {
        // Both full "glyph:direction" hints together don't fit this column even with
        // just the glyph (checked). As with RUMBLE, the row just says whether the
        // combo is configured; which buttons got picked shows in the note right after
        // capturing them (ctl_note, below), the same way "Z set" does for N64 buttons.
        int b1 = s_ctl.c ? s_ctl.c->shotBtn1 : -1, b2 = s_ctl.c ? s_ctl.c->shotBtn2 : -1;
        if (b1 >= 0 && b2 >= 0) sprintf(out, "< %s >", Tr(T_ON));
        else if (b1 >= 0 || b2 >= 0)
        {
            char h[64], *p;
            ctl_hints((u32)1 << (b1 >= 0 ? b1 : b2), h, sizeof(h));
            if ((p = strchr(h, ':')) != NULL) *p = 0;
            sprintf(out, "< %s >", h);
        }
        else sprintf(out, "< %s >", Tr(T_OFF));
    }
    else out[0] = 0;
}

// ---- The N64 controller drawn on the Controls page ----
// A plain drawing of the controller in units (100 x 56, scaled by `s`): the
// control being edited is outlined, and blinks while it waits for an input.
static D3DCOLOR pad_rgb(int r, int g, int b) { return D3DCOLOR_ARGB(255, r, g, b); }

// Where `item` is on the drawing (a box in units, its corner radius); 0: not on it.
static int pad_shape(int item, float *x, float *y, float *w, float *h, float *r)
{
    static const float circles[][4] = {   // item, centre x, y, radius
        { H64_N64_A, 78.5f, 30.0f, 3.6f }, { H64_N64_B, 73.0f, 23.0f, 3.0f }, { H64_N64_START, 50.0f, 13.5f, 2.6f },
        { H64_N64_CUP, 86.0f, 12.5f, 2.1f }, { H64_N64_CDOWN, 86.0f, 21.5f, 2.1f },
        { H64_N64_CLEFT, 81.5f, 17.0f, 2.1f }, { H64_N64_CRIGHT, 90.5f, 17.0f, 2.1f },
        { C_STICK, 50.0f, 30.0f, 6.0f }, { C_CALIB, 50.0f, 30.0f, 6.0f },
    };
    static const float boxes[][6] = {     // item, x, y, w, h, radius
        { H64_N64_DUP, 13.8f, 10.7f, 4.4f, 4.3f, 1.0f }, { H64_N64_DDOWN, 13.8f, 19.4f, 4.4f, 4.3f, 1.0f },
        { H64_N64_DLEFT, 9.5f, 15.0f, 4.3f, 4.4f, 1.0f }, { H64_N64_DRIGHT, 18.2f, 15.0f, 4.3f, 4.4f, 1.0f },
        { H64_N64_L, 8.0f, 1.0f, 22.0f, 5.5f, 2.5f }, { H64_N64_R, 70.0f, 1.0f, 22.0f, 5.5f, 2.5f },   // the part above the body
        { H64_N64_Z, 45.5f, 45.0f, 9.0f, 7.0f, 2.0f },
    };
    int i;
    for (i = 0; i < (int)(sizeof(circles) / sizeof(circles[0])); i++)
        if ((int)circles[i][0] == item)
        {
            *x = circles[i][1] - circles[i][3]; *y = circles[i][2] - circles[i][3];
            *w = *h = circles[i][3] * 2; *r = circles[i][3];
            return 1;
        }
    for (i = 0; i < (int)(sizeof(boxes) / sizeof(boxes[0])); i++)
        if ((int)boxes[i][0] == item)
        {
            *x = boxes[i][1]; *y = boxes[i][2]; *w = boxes[i][3]; *h = boxes[i][4]; *r = boxes[i][5];
            return 1;
        }
    return 0;
}

static void draw_n64_pad(float ox, float oy, float s, int hiItem, int blink)
{
    const D3DCOLOR dark = pad_rgb(54, 56, 64), shoulder = pad_rgb(78, 80, 90);
    float x, y, w, h, r;
    int i;
#define PU(v) ((v) * s)
    // Shoulder buttons behind the body, then the body: three handles and the top bar.
    UiRoundRect(ox + PU(8), oy + PU(1), PU(22), PU(8), PU(2.5f), shoulder, shoulder);
    UiRoundRect(ox + PU(70), oy + PU(1), PU(22), PU(8), PU(2.5f), shoulder, shoulder);
    UiShadow(ox + PU(4), oy + PU(6), PU(92), PU(50), PU(9), 18, UI_COL_SHADOW);
    UiRoundRect(ox + PU(6), oy + PU(12), PU(20), PU(40), PU(10), pad_rgb(152, 154, 164), pad_rgb(112, 114, 124));
    UiRoundRect(ox + PU(74), oy + PU(12), PU(20), PU(40), PU(10), pad_rgb(152, 154, 164), pad_rgb(112, 114, 124));
    UiRoundRect(ox + PU(41), oy + PU(12), PU(18), PU(44), PU(9), pad_rgb(152, 154, 164), pad_rgb(108, 110, 120));
    UiRoundRect(ox + PU(4), oy + PU(6), PU(92), PU(20), PU(9), pad_rgb(178, 180, 190), pad_rgb(146, 148, 158));
    // D-pad, Start, the stick, Z (under the middle handle on the real one).
    UiRoundRect(ox + PU(9.5f), oy + PU(15), PU(13), PU(4.4f), PU(1), dark, dark);
    UiRoundRect(ox + PU(13.8f), oy + PU(10.7f), PU(4.4f), PU(13), PU(1), dark, dark);
    UiCircle(ox + PU(50), oy + PU(13.5f), PU(2.6f), pad_rgb(206, 44, 44));
    UiCircle(ox + PU(50), oy + PU(30), PU(6), pad_rgb(66, 68, 78));
    UiCircle(ox + PU(50), oy + PU(30), PU(4), pad_rgb(196, 198, 206));
    UiRoundRect(ox + PU(45.5f), oy + PU(45), PU(9), PU(7), PU(2), dark, dark);
    UiText(UI_BOLD, PU(4.5f), ox + PU(50), oy + PU(48.5f), UI_COL_TEXT2, "Z", UI_CENTER);
    // A, B and the four C buttons.
    UiCircle(ox + PU(78.5f), oy + PU(30), PU(3.6f), pad_rgb(52, 92, 214));
    UiText(UI_BOLD, PU(4.2f), ox + PU(78.5f), oy + PU(30), UI_COL_TEXT, "A", UI_CENTER);
    UiCircle(ox + PU(73), oy + PU(23), PU(3), pad_rgb(40, 164, 72));
    UiText(UI_BOLD, PU(3.6f), ox + PU(73), oy + PU(23), UI_COL_TEXT, "B", UI_CENTER);
    for (i = H64_N64_CUP; i <= H64_N64_CRIGHT; i++)
        if (pad_shape(i, &x, &y, &w, &h, &r))
            UiCircle(ox + PU(x + r), oy + PU(y + r), PU(r), pad_rgb(232, 192, 40));
    // The control being edited: outlined; blinking while an input is awaited.
    if (pad_shape(hiItem, &x, &y, &w, &h, &r))
    {
        float on = blink ? 0.5f + 0.5f * sinf(UiTime() * 12.566f) : 1.0f, pad = 3.0f;
        int a = (int)(255 * on);
        x = ox + PU(x); y = oy + PU(y); w = PU(w); h = PU(h); r = PU(r);
        if (blink)
        {
            UiShadow(x - 2, y - 2, w + 4, h + 4, r + 2, 16, D3DCOLOR_ARGB((int)(200 * on), 255, 112, 84));
            UiRoundRect(x, y, w, h, r, D3DCOLOR_ARGB((int)(190 * on), 255, 112, 84), D3DCOLOR_ARGB((int)(190 * on), 234, 76, 58));
        }
        UiRoundRing(x - pad, y - pad, w + pad * 2, h + pad * 2, r + pad, 2.5f, D3DCOLOR_ARGB(a, 255, 112, 84));
    }
#undef PU
}

// Text at `size`, made smaller when it would be wider than `maxW`.
static void ctl_fit_text(int font, float size, float x, float cy, float maxW, D3DCOLOR color, const char *text)
{
    float w = UiTextWidth(font, size, text);
    if (w > maxW) size *= maxW / w;
    UiText(font, size, x, cy, color, text, UI_LEFT);
}

// Auto setup, over the page: the question first, then the N64 button asked
// for, large, next to the controller drawing with that button blinking.
static void ctl_auto_draw(void)
{
    const float w = 760, h = 340, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
    char buf[200];
    UiDim();
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_AUTO_CONFIG), UI_LEFT);
    UiRoundRect(x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    UiRect(x + 24, y + h - 78, w - 48, 1, UI_COL_LINE);
    if (s_ctl.autoMode == AUTO_ASK)
    {
        UiText(UI_BOLD, 24, x + 40, y + 140, UI_COL_TEXT, Tr(T_AUTO_ASK), UI_LEFT);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_AUTO_ASK_NOTE), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 18, x + 40, y + 182, UI_COL_TEXT2, buf, UI_LEFT);
        UiFooter(x + 40, y + h - 39, Tr(T_FOOT_AUTO_ASK));
        return;
    }
    {
        const float s = 2.8f, padX = x + w - 40 - 100 * s, padY = y + 96 + (h - 78 - 96 - 56 * s) * 0.5f;
        const float textW = padX - 24 - (x + 40);
        const char *name = TrN64(s_ctl.capItem);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_AUTO_STEP), s_ctl.autoIdx + 1, AUTO_COUNT);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 18, x + w - 40, y + 50, UI_COL_TEXT3, buf, UI_RIGHT);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PRESS_FOR), name);
        buf[sizeof(buf) - 1] = 0;
        ctl_fit_text(UI_REGULAR, 20, x + 40, y + 140, textW, UI_COL_TEXT2, buf);
        ctl_fit_text(UI_BOLD, 56, x + 40, y + 200, textW, UI_COL_ACCENT_HI, name);
        draw_n64_pad(padX, padY, s, s_ctl.capItem, 1);
        // The input just set for the previous button, then BACK to cancel (the mapping goes back as it was).
        if (s_ctl.msg[0] && GetTickCount() - s_ctl.msgAt < 3000)
            UiText(UI_BOLD, 18, x + 40, y + h - 39, UI_COL_ACCENT_HI, s_ctl.msg, UI_LEFT);
        UiFooter(x + w - 40 - UiFooterWidth(Tr(T_FOOT_CANCEL)), y + h - 39, Tr(T_FOOT_CANCEL));
    }
}

static void controls_draw(const UiMenu *m)
{
    static UiAnim selX, selY;
    static int lastCol = -1;
    const H64PadMap *map;
    H64PadMap def;
    int i, col, selItem = s_ctlOrder[m->sel < 0 || m->sel >= C_COUNT ? 0 : m->sel];
    float row, x = CTL_X, y = CTL_Y, sx, sy;
    char buf[320];
    if (s_ctl.c) map = &s_ctl.c->padMap[s_ctl.player];
    else { h64_padmap_default(&def); map = &def; }

    UiDim();
    UiPanel(x, y, CTL_W, CTL_H, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_CONTROLS), UI_LEFT);
    UiRoundRect(x + 40, y + 74, 30, 4, 2, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    {
        // Player selector: "LB  Player 1  RB" at the top right.
        float r = x + CTL_W - 40, w;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PLAYER), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        r -= UiFooterWidth("RB");
        UiFooter(r, y + 50, "RB");
        w = UiTextWidth(UI_BOLD, 22, buf);
        UiText(UI_BOLD, 22, r - 16, y + 50, UI_COL_TEXT, buf, UI_RIGHT);
        r -= 16 + w + 16 + UiFooterWidth("LB");
        UiFooter(r, y + 50, "LB");
        if (s_ctl.player)
        {
            _snprintf(buf, sizeof(buf) - 1, Tr(T_USED_WHEN), s_ctl.player + 1);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 16, x + CTL_W - 40, y + 82, UI_COL_TEXT3, buf, UI_RIGHT);
        }
    }

    heading(ctl_col_x(0) + 20, y + CTL_TOP - 22, Tr(T_HEAD_BUTTONS));
    heading(ctl_col_x(1) + 20, y + CTL_TOP - 22, Tr(T_HEAD_DPAD));
    heading(ctl_col_x(1) + 20, ctl_row_y(5) - 16, Tr(T_HEAD_CBUTTONS));
    heading(ctl_col_x(2) + 20, y + CTL_TOP - 22, Tr(T_HEAD_STICK));

    ctl_place(selItem, &col, &row);
    sx = anim_step(&selX, ctl_col_x(col), col != lastCol);
    sy = anim_step(&selY, ctl_row_y(row), col != lastCol);
    lastCol = col;
    selection_bar(sx, sy + 3, CTL_COLW, CTL_ROW - 6);

    for (i = 0; i < C_COUNT; i++)
    {
        int item = s_ctlOrder[i], s = i == m->sel;
        float cx, cy;
        ctl_place(item, &col, &row);
        cx = ctl_col_x(col);
        cy = ctl_row_y(row) + CTL_ROW * 0.5f;
        if (item < H64_N64_COUNT)
        {
            UiText(s ? UI_BOLD : UI_REGULAR, 20, cx + 24, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, TrN64(item), UI_LEFT);
            if (s_ctl.capturing && item == s_ctl.capItem)
                UiText(UI_BOLD, 18, cx + CTL_COLW - 18, cy, UI_COL_ACCENT_HI, Tr(T_PRESS_INPUT), UI_RIGHT);
            else
            {
                ctl_hints(map->src[item], buf, sizeof(buf));
                if (!buf[0]) UiText(UI_REGULAR, 18, cx + CTL_COLW - 18, cy, UI_COL_TEXT3, Tr(T_NO_INPUT_SET), UI_RIGHT);
                else
                {
                    float w = UiFooterWidth(buf), maxW = CTL_COLW - 130;
                    if (w <= maxW) UiFooter(cx + CTL_COLW - 18 - w, cy, buf);
                    else
                    {
                        char n[64];
                        int k, cnt = 0;
                        for (k = 0; k < H64_SRC_COUNT; k++) cnt += (map->src[item] >> k) & 1;
                        _snprintf(n, sizeof(n) - 1, Tr(T_N_INPUTS), cnt);
                        n[sizeof(n) - 1] = 0;
                        UiText(UI_REGULAR, 18, cx + CTL_COLW - 18, cy, UI_COL_TEXT2, n, UI_RIGHT);
                    }
                }
            }
        }
        else
        {
            static const int labels[] = { T_ANALOG_STICK, T_CALIBRATION, T_SCREENSHOT_COMBO, T_RUMBLE, T_RESET_DEFAULT, T_AUTO_CONFIG };
            UiText(s ? UI_BOLD : UI_REGULAR, 20, cx + 24, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, Tr(labels[item - C_STICK]), UI_LEFT);
            if (s_ctl.capturing && item == s_ctl.capItem)
                UiText(UI_BOLD, 18, cx + CTL_COLW - 18, cy, UI_COL_ACCENT_HI, Tr(T_PRESS_INPUT), UI_RIGHT);
            else
            {
                ctl_value_text(item, map, buf);
                if (buf[0]) draw_value(cx + CTL_COLW - 18, cy, buf, s);
            }
        }
    }

    // The controller under the stick settings, with the control being edited.
    // bottom is anchored to row 9 (not to CTL_H): C_SHOTCOMBO sits right there, and
    // growing the panel must not push the drawing down over that row.
    {
        float s = CTL_COLW / 100.0f, top = ctl_row_y(2) + 8, bottom = ctl_row_y(9) - 8;
        draw_n64_pad(ctl_col_x(2), top + (bottom - top - 56 * s) * 0.5f, s,
                     s_ctl.capturing ? s_ctl.capItem : selItem, s_ctl.capturing);
    }

    UiRect(x + 24, y + CTL_H - 78, CTL_W - 48, 1, UI_COL_LINE);
    if (s_ctl.capturing)
    {
        DWORD age = s_ctl.capReleased ? GetTickCount() - s_ctl.capStart : 0;
        float left = age >= CAPTURE_MS ? 0.0f : (CAPTURE_MS - age) / (float)CAPTURE_MS, bw = 220;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PRESS_FOR), TrN64(s_ctl.capItem));
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_BOLD, 20, x + 40, y + CTL_H - 39, UI_COL_TEXT, buf, UI_LEFT);
        UiFooter(x + CTL_W - 40 - UiFooterWidth(Tr(T_FOOT_CANCEL)), y + CTL_H - 39, Tr(T_FOOT_CANCEL));
        if (s_ctl.autoMode != AUTO_RUN)   // the time left (auto setup has no time limit)
        {
            float bx = x + CTL_W - 80 - UiFooterWidth(Tr(T_FOOT_CANCEL)) - bw;
            UiRoundRect(bx, y + CTL_H - 42, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
            if (left > 0) UiRoundRect(bx, y + CTL_H - 42, bw * left, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
        }
    }
    else
    {
        // The last change, or a word of help, on the right.
        DWORD age = GetTickCount() - s_ctl.msgAt;
        if (selItem < H64_N64_COUNT) UiFooter(x + 40, y + CTL_H - 39, Tr(T_FOOT_CTL_BUTTON));
        else if (selItem == C_STICK || selItem == C_RUMBLE) UiFooter(x + 40, y + CTL_H - 39, Tr(T_FOOT_CTL_VALUE));
        else UiFooter(x + 40, y + CTL_H - 39, Tr(T_FOOT_CTL_OTHER));
        if (s_ctl.msg[0] && age < 3000) UiText(UI_BOLD, 18, x + CTL_W - 40, y + CTL_H - 39, UI_COL_ACCENT_HI, s_ctl.msg, UI_RIGHT);
        else UiText(UI_REGULAR, 16, x + CTL_W - 40, y + CTL_H - 39, UI_COL_TEXT3, Tr(T_BACK_RESERVED), UI_RIGHT);
    }
    if (s_ctl.autoMode) ctl_auto_draw();
}

// ---- Stick calibration (Controls > C_CALIB) ----
// For the player being edited (controller s_ctl.player), one stick at a time
// (LB/RB): a live monitor of its raw position (hollow dot) and of what the
// emulator makes of it (filled dot: centre, radial dead zone, curve,
// inversion - H64StickCal), its dead zone shaded and the edge the last test
// traced (circularity). Readings and the last diagnosis in the middle,
// settings on the right. Tests: centre calibration (rest), diagnosis (rest,
// then circles) and the wizard (rest for both sticks, circles for each, then
// results to apply). They change how the emulator reads a stick; they do not
// mend a worn one (CAL_NOTE says so on the page).
enum { CR_DEAD = 0, CR_SENS, CR_CURVE, CR_INVERT, CR_DRIFT, CR_RAW, CR_CENTER, CR_DIAG, CR_WIZARD, CR_RESET, CR_COUNT };
enum { CT_NONE = 0, CT_CENTER, CT_DIAG, CT_WIZARD };   // the test running
enum { CP_REST = 0, CP_CIRCLE_L, CP_CIRCLE_R, CP_RESULT };   // its phase
#define CAL_REST_MS 2500       // sampling at rest
#define CAL_CIRCLE_MS 5000     // circling the edge
#define CAL_BINS 32            // directions of the traced edge
#define CAL_JUMP 14000         // a change this big between two readings (16 ms apart): a jump
#define CAL_EDGE_MIN 8000      // an edge direction counts as reached beyond this

struct CalRest { int n; double sx, sy, sxx, syy; int minX, maxX, minY, maxY; };   // at rest
struct CalMove { int n, minX, maxX, minY, maxY, jumps, lastX, lastY; float bin[CAL_BINS]; };   // circling, from the rest centre

struct CalibState
{
    Config *c;
    int stick;                  // 0 left, 1 right
    int raw[2][2];              // the last reading of each stick (x, y), raw units
    int connected, paused, rawView;
    int task, phase;            // CT_*, CP_*
    DWORD phaseMs, lastTick;    // time spent in the phase (not while paused)
    float restCx[2], restCy[2]; // the centre the circles are measured from
    CalRest rest[2];
    CalMove move[2];
    int haveRest[2], haveMove[2];
    DWORD deadAt;               // when the dead zone last changed (the monitor shows it off for a moment)
    char msg[160];
    DWORD msgAt;
};
static CalibState s_cal;
#define CAL_DEAD_SHOW_MS 2000

static void cal_note(const char *text)
{
    strncpy(s_cal.msg, text, sizeof(s_cal.msg) - 1);
    s_cal.msg[sizeof(s_cal.msg) - 1] = 0;
    s_cal.msgAt = GetTickCount();
}

static void calib_open(UiMenu *m, Config *c)
{
    UiMenuClear(m, Tr(T_CALIBRATION), "");
    m->page = UI_PAGE_CALIB;
    m->count = CR_COUNT;
    m->sel = 0;
    memset(&s_cal, 0, sizeof(s_cal));
    s_cal.c = c;
    s_cal.lastTick = GetTickCount();
}

static void rest_add(CalRest *r, int x, int y)
{
    if (!r->n) { r->minX = r->maxX = x; r->minY = r->maxY = y; }
    r->n++;
    r->sx += x; r->sy += y;
    r->sxx += (double)x * x; r->syy += (double)y * y;
    if (x < r->minX) r->minX = x;
    if (x > r->maxX) r->maxX = x;
    if (y < r->minY) r->minY = y;
    if (y > r->maxY) r->maxY = y;
}

// Mean (the resting position), spread (standard deviation, both axes
// together) and the farthest reading from the mean, all in raw units.
static void rest_stats(const CalRest *r, float *mx, float *my, float *sd, float *maxDev)
{
    double vx, vy, d;
    *mx = *my = *sd = *maxDev = 0;
    if (!r->n) return;
    *mx = (float)(r->sx / r->n);
    *my = (float)(r->sy / r->n);
    vx = r->sxx / r->n - (double)*mx * *mx;
    vy = r->syy / r->n - (double)*my * *my;
    *sd = (float)sqrt((vx > 0 ? vx : 0) + (vy > 0 ? vy : 0));
    d = r->maxX - *mx;
    if (*mx - r->minX > d) d = *mx - r->minX;
    if (r->maxY - *my > d) d = r->maxY - *my;
    if (*my - r->minY > d) d = *my - r->minY;
    *maxDev = (float)d;
}

static void move_add(CalMove *mv, int x, int y, float cx, float cy)
{
    float dx = x - cx, dy = y - cy, r = sqrtf(dx * dx + dy * dy);
    int b = (int)((atan2f(dy, dx) + 3.14159265f) / (2.0f * 3.14159265f) * CAL_BINS);
    if (b < 0) b = 0;
    if (b >= CAL_BINS) b = CAL_BINS - 1;
    if (!mv->n) { mv->minX = mv->maxX = (int)dx; mv->minY = mv->maxY = (int)dy; }
    else
    {
        float jx = (float)(x - mv->lastX), jy = (float)(y - mv->lastY);
        if (jx * jx + jy * jy > (float)CAL_JUMP * CAL_JUMP) mv->jumps++;
    }
    mv->n++;
    mv->lastX = x;
    mv->lastY = y;
    if (dx < mv->minX) mv->minX = (int)dx;
    if (dx > mv->maxX) mv->maxX = (int)dx;
    if (dy < mv->minY) mv->minY = (int)dy;
    if (dy > mv->maxY) mv->maxY = (int)dy;
    if (r > mv->bin[b]) mv->bin[b] = r;
}

// The traced edge: how many directions were reached (percent), the shortest
// reach over the longest (percent: 100 a perfect circle), and the mean reach.
static void move_stats(const CalMove *mv, int *coverage, int *circ, float *reach)
{
    float lo = 1e9f, hi = 0, sum = 0;
    int i, n = 0;
    for (i = 0; i < CAL_BINS; i++)
        if (mv->bin[i] >= CAL_EDGE_MIN)
        {
            n++;
            sum += mv->bin[i];
            if (mv->bin[i] < lo) lo = mv->bin[i];
            if (mv->bin[i] > hi) hi = mv->bin[i];
        }
    *coverage = n * 100 / CAL_BINS;
    *circ = n && hi > 0 ? (int)(lo * 100.0f / hi + 0.5f) : 0;
    *reach = n ? sum / n : 0;
}

// What the wizard suggests for a stick: a dead zone just past its resting
// noise (once centred), and the travel it really reaches.
static int cal_suggest_dead(int s)
{
    float mx, my, sd, maxDev;
    int d;
    rest_stats(&s_cal.rest[s], &mx, &my, &sd, &maxDev);
    d = (int)(maxDev + 3.0f * sd) + 1500;
    d = (d + 50) / 100 * 100;
    return d < 2000 ? 2000 : d > 13100 ? 13100 : d;
}

static int cal_suggest_outer(int s)
{
    int coverage, circ;
    float reach;
    if (!s_cal.haveMove[s]) return H64_PAD_OUTER_DEFAULT;
    move_stats(&s_cal.move[s], &coverage, &circ, &reach);
    if (coverage < 75 || reach >= 31000) return H64_PAD_OUTER_DEFAULT;
    reach *= 0.97f;   // a little short of the average reach, so every direction gets to full deflection
    return reach < 16000 ? 16000 : (int)reach;
}

static void calib_start(int task)
{
    s_cal.task = task;
    s_cal.phase = CP_REST;
    s_cal.phaseMs = 0;
    s_cal.paused = 0;
    memset(s_cal.rest, 0, sizeof(s_cal.rest));
    if (task != CT_CENTER) memset(s_cal.move, 0, sizeof(s_cal.move));
    if (task == CT_WIZARD) s_cal.haveRest[0] = s_cal.haveRest[1] = s_cal.haveMove[0] = s_cal.haveMove[1] = 0;
    else
    {
        s_cal.haveRest[s_cal.stick] = 0;
        if (task == CT_DIAG) s_cal.haveMove[s_cal.stick] = 0;
    }
}

// The next phase of the running test, its time up.
static int calib_phase_done(void)
{
    H64PadMap *map = &s_cal.c->padMap[s_ctl.player];
    int s = s_cal.stick, k;
    if (s_cal.phase == CP_REST)
    {
        for (k = 0; k < 2; k++)
        {
            float sd, maxDev;
            rest_stats(&s_cal.rest[k], &s_cal.restCx[k], &s_cal.restCy[k], &sd, &maxDev);
        }
        if (s_cal.task == CT_CENTER)
        {
            float sd, maxDev;
            rest_stats(&s_cal.rest[s], &s_cal.restCx[s], &s_cal.restCy[s], &sd, &maxDev);
            s_cal.task = CT_NONE;
            s_cal.haveRest[s] = 1;
            if (maxDev > 6000) { cal_note(Tr(T_CAL_MOVED)); return SET_NONE; }   // touched while measuring
            map->cal[s].cx = (int)floorf(s_cal.restCx[s] + 0.5f);
            map->cal[s].cy = (int)floorf(s_cal.restCy[s] + 0.5f);
            cal_note(Tr(T_CAL_CENTER_SET));
            return SET_CHANGED;
        }
        if (s_cal.task == CT_DIAG) s_cal.haveRest[s] = 1;
        else s_cal.haveRest[0] = s_cal.haveRest[1] = 1;
        s_cal.phase = s_cal.task == CT_DIAG ? (s ? CP_CIRCLE_R : CP_CIRCLE_L) : CP_CIRCLE_L;
    }
    else if (s_cal.phase == CP_CIRCLE_L || s_cal.phase == CP_CIRCLE_R)
    {
        s_cal.haveMove[s_cal.phase == CP_CIRCLE_R] = 1;
        if (s_cal.task == CT_WIZARD && s_cal.phase == CP_CIRCLE_L) s_cal.phase = CP_CIRCLE_R;
        else if (s_cal.task == CT_WIZARD) s_cal.phase = CP_RESULT;
        else
        {
            s_cal.task = CT_NONE;
            cal_note(Tr(T_CAL_DIAG_DONE));
        }
    }
    s_cal.phaseMs = 0;
    return SET_NONE;
}

static void calib_apply_wizard(void)
{
    H64PadMap *map = &s_cal.c->padMap[s_ctl.player];
    int k;
    for (k = 0; k < 2; k++)
    {
        map->cal[k].cx = (int)floorf(s_cal.restCx[k] + 0.5f);
        map->cal[k].cy = (int)floorf(s_cal.restCy[k] + 0.5f);
        map->cal[k].deadzone = cal_suggest_dead(k);
        map->cal[k].outer = cal_suggest_outer(k);
    }
}

static int calib_input(UiMenu *m, Config *c, WORD down)
{
    H64PadMap *map = &c->padMap[s_ctl.player];
    H64StickCal *cal;
    XINPUT_STATE st;
    DWORD now = GetTickCount(), dt = now - s_cal.lastTick;
    int dir, r = SET_NONE;
    s_cal.c = c;
    s_cal.lastTick = now;
    {
        // UiInputPoll turns the left stick into D-pad presses; here the sticks
        // are what is being measured, so only the real D-pad moves around.
        const WORD dpad = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
        XINPUT_STATE nav;
        WORD held = XInputGetState(0, &nav) == ERROR_SUCCESS ? nav.Gamepad.wButtons : 0;
        down &= ~(dpad & ~held);
    }
    // The controller being calibrated: the player's own.
    s_cal.connected = XInputGetState(s_ctl.player, &st) == ERROR_SUCCESS;
    if (s_cal.connected && !s_cal.paused)
    {
        s_cal.raw[0][0] = st.Gamepad.sThumbLX; s_cal.raw[0][1] = st.Gamepad.sThumbLY;
        s_cal.raw[1][0] = st.Gamepad.sThumbRX; s_cal.raw[1][1] = st.Gamepad.sThumbRY;
    }
    if (down & XINPUT_GAMEPAD_START) s_cal.paused = !s_cal.paused;
    if (s_cal.task)
    {
        if (s_cal.phase == CP_RESULT)
        {
            if (down & XINPUT_GAMEPAD_A) { calib_apply_wizard(); s_cal.task = CT_NONE; cal_note(Tr(T_CAL_APPLIED)); return SET_CHANGED; }
            if (down & XINPUT_GAMEPAD_X) { calib_start(CT_WIZARD); return SET_NONE; }
            if (down & XINPUT_GAMEPAD_B) { s_cal.task = CT_NONE; cal_note(Tr(T_CANCELLED)); }
            return SET_NONE;
        }
        if (down & XINPUT_GAMEPAD_B) { s_cal.task = CT_NONE; cal_note(Tr(T_CANCELLED)); return SET_NONE; }
        if (s_cal.paused || !s_cal.connected) return SET_NONE;
        if (s_cal.phase == CP_REST)
        {
            rest_add(&s_cal.rest[0], s_cal.raw[0][0], s_cal.raw[0][1]);
            rest_add(&s_cal.rest[1], s_cal.raw[1][0], s_cal.raw[1][1]);
        }
        else
        {
            int k = s_cal.phase == CP_CIRCLE_R;
            move_add(&s_cal.move[k], s_cal.raw[k][0], s_cal.raw[k][1], s_cal.restCx[k], s_cal.restCy[k]);
        }
        s_cal.phaseMs += dt > 100 ? 100 : dt;
        if (s_cal.phaseMs >= (DWORD)(s_cal.phase == CP_REST ? CAL_REST_MS : CAL_CIRCLE_MS)) r = calib_phase_done();
        return r;
    }
    if (down & XINPUT_GAMEPAD_B)
    {
        int i;
        controls_open(m, c);
        for (i = 0; i < C_COUNT; i++)
            if (s_ctlOrder[i] == C_CALIB) m->sel = i;
        return SET_NONE;
    }
    if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) s_cal.stick = !s_cal.stick;
    UiMenuNavigate(m, down);
    cal = &map->cal[s_cal.stick];
    if (down & XINPUT_GAMEPAD_X)
    {
        // The test of the row, or the diagnosis from any other row.
        calib_start(m->sel == CR_CENTER ? CT_CENTER : m->sel == CR_WIZARD ? CT_WIZARD : CT_DIAG);
        return SET_NONE;
    }
    if (down & XINPUT_GAMEPAD_Y)
    {
        // This row's setting back to its default.
        switch (m->sel)
        {
        case CR_DEAD: cal->deadzone = H64_PAD_DEADZONE_DEFAULT; s_cal.deadAt = now; break;
        case CR_SENS: cal->range = H64_PAD_RANGE_DEFAULT; break;
        case CR_CURVE: cal->curve = H64_PAD_CURVE_DEFAULT; break;
        case CR_INVERT: cal->invert = 0; break;
        case CR_DRIFT: map->drift = H64_PAD_DRIFT_DEFAULT; break;
        case CR_RAW: s_cal.rawView = 0; return SET_NONE;
        case CR_CENTER: cal->cx = cal->cy = 0; cal->outer = H64_PAD_OUTER_DEFAULT; break;
        default: return SET_NONE;
        }
        return SET_CHANGED;
    }
    if (down & XINPUT_GAMEPAD_A)
    {
        if (m->sel == CR_CENTER) { calib_start(CT_CENTER); return SET_NONE; }
        if (m->sel == CR_DIAG) { calib_start(CT_DIAG); return SET_NONE; }
        if (m->sel == CR_WIZARD) { calib_start(CT_WIZARD); return SET_NONE; }
        if (m->sel == CR_RESET)
        {
            h64_padmap_cal_default(cal);
            cal_note(Tr(T_DEFAULT_CONTROLS));
            return SET_CHANGED;
        }
    }
    // Values: left/right, or A to step on (round at the ends).
    dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : (down & XINPUT_GAMEPAD_A) ? 1 : 0;
    if (!dir) return SET_NONE;
    switch (m->sel)
    {
    case CR_DEAD:
    {
        // In whole percent of the travel, 0..40 %.
        int pct = (cal->deadzone * 100 + 16383) / 32767 + dir;
        if (pct < 0) pct = 40;
        if (pct > 40) pct = 0;
        cal->deadzone = pct * 32767 / 100;
        s_cal.deadAt = now;
        break;
    }
    case CR_SENS: cal->range += dir * 5; if (cal->range < 50) cal->range = 150; if (cal->range > 150) cal->range = 50; break;
    case CR_CURVE: cal->curve += dir * 10; if (cal->curve < 50) cal->curve = 250; if (cal->curve > 250) cal->curve = 50; break;
    case CR_INVERT: cal->invert = (cal->invert + dir + 4) & 3; break;
    case CR_DRIFT: map->drift += dir; if (map->drift < 1) map->drift = 30; if (map->drift > 30) map->drift = 1; break;
    case CR_RAW: s_cal.rawView = !s_cal.rawView; return SET_NONE;
    default: return SET_NONE;
    }
    return SET_CHANGED;
}

// The stick monitor: a disc with a grid, the ideal centre's crosshair, the
// dead zone around the calibrated centre, the traced edge, the raw reading
// (hollow) and the corrected one (filled).
static void calib_monitor(float cx, float cy, float R, int s, const H64StickCal *cal)
{
    const float k = R / 32767.0f;
    float ccx = cx + cal->cx * k, ccy = cy - cal->cy * k, rx, ry;
    int i;
    UiShadow(cx - R, cy - R + 8, R * 2, R * 2, R, 30, D3DCOLOR_ARGB(130, 0, 0, 0));
    UiCircle(cx, cy, R, D3DCOLOR_ARGB(255, 20, 22, 30));
    for (i = -3; i <= 3; i++)
    {
        float d = i * R / 4, half = sqrtf(R * R - d * d);
        D3DCOLOR line = i ? D3DCOLOR_ARGB(22, 255, 255, 255) : D3DCOLOR_ARGB(70, 140, 170, 220);
        UiRect(cx - half, cy + d - 0.5f, half * 2, 1, line);
        UiRect(cx + d - 0.5f, cy - half, 1, half * 2, line);
    }
    UiRoundRing(cx - R * 0.5f, cy - R * 0.5f, R, R, R * 0.5f, 1.0f, D3DCOLOR_ARGB(28, 255, 255, 255));
    UiRoundRing(cx - R, cy - R, R * 2, R * 2, R, 2.0f, D3DCOLOR_ARGB(110, 255, 255, 255));
    UiText(UI_BOLD, 15, cx + R + 14, cy, UI_COL_TEXT3, "X", UI_LEFT);
    UiText(UI_BOLD, 15, cx, cy - R - 16, UI_COL_TEXT3, "Y", UI_CENTER);
    if (!s_cal.rawView)
    {
        // The dead zone glides to a new size, and stands out (brighter, its
        // percentage over it) for a moment after it changed.
        static UiAnim deadAnim;
        static int deadStick = -1;
        DWORD since = GetTickCount() - s_cal.deadAt;
        float show = s_cal.deadAt && since < CAL_DEAD_SHOW_MS ? 1.0f - since / (float)CAL_DEAD_SHOW_MS : 0.0f;
        float dr = anim_step_rate(&deadAnim, cal->deadzone * k, deadStick != s, 12.0f, 0.2f), orr = cal->outer * k;
        deadStick = s;
        UiCircle(ccx, ccy, dr, D3DCOLOR_ARGB(50 + (int)(60 * show), 90, 170, 240));
        if (show > 0) selection_glow(ccx - dr, ccy - dr, dr * 2, dr * 2, dr, show);
        UiRoundRing(ccx - dr, ccy - dr, dr * 2, dr * 2, dr, 1.5f + show, D3DCOLOR_ARGB(170 + (int)(85 * show), 120, 205, 255));
        if (show > 0)
        {
            char pct[16];
            sprintf(pct, "%d%%", (cal->deadzone * 100 + 16383) / 32767);
            UiTextShadowed(UI_BOLD, 22, ccx, ccy - dr - 16, D3DCOLOR_ARGB((int)(255 * (show < 0.25f ? show * 4 : 1.0f)), 160, 220, 255), pct, UI_CENTER);
        }
        if (cal->outer < H64_PAD_OUTER_DEFAULT) UiRoundRing(ccx - orr, ccy - orr, orr * 2, orr * 2, orr, 1.5f, D3DCOLOR_ARGB(120, 255, 190, 90));
        if (cal->cx || cal->cy)
        {
            // The calibrated resting position.
            UiRect(ccx - 6, ccy - 0.75f, 12, 1.5f, D3DCOLOR_ARGB(230, 255, 190, 90));
            UiRect(ccx - 0.75f, ccy - 6, 1.5f, 12, D3DCOLOR_ARGB(230, 255, 190, 90));
        }
    }
    UiRoundRing(cx - 13, cy - 13, 26, 26, 13, 1.5f, D3DCOLOR_ARGB(170, 255, 255, 255));
    UiRect(cx - 5, cy - 0.5f, 10, 1, D3DCOLOR_ARGB(170, 255, 255, 255));
    UiRect(cx - 0.5f, cy - 5, 1, 10, D3DCOLOR_ARGB(170, 255, 255, 255));
    if (s_cal.haveMove[s] || ((s_cal.phase == CP_CIRCLE_L || s_cal.phase == CP_CIRCLE_R) && s_cal.task && (s_cal.phase == CP_CIRCLE_R) == s))
    {
        // The edge reached in each direction (from the centre measured at rest).
        float ox = cx + s_cal.restCx[s] * k, oy = cy - s_cal.restCy[s] * k;
        for (i = 0; i < CAL_BINS; i++)
        {
            float a = (i + 0.5f) / CAL_BINS * 2.0f * 3.14159265f - 3.14159265f, rr = s_cal.move[s].bin[i] * k;
            if (s_cal.move[s].bin[i] < CAL_EDGE_MIN) continue;
            UiCircle(ox + cosf(a) * rr, oy - sinf(a) * rr, 3.0f, D3DCOLOR_ARGB(220, 255, 200, 90));
        }
    }
    if (!s_cal.connected) return;
    rx = cx + s_cal.raw[s][0] * k;
    ry = cy - s_cal.raw[s][1] * k;
    UiRoundRing(rx - 8, ry - 8, 16, 16, 8, 2.0f, D3DCOLOR_ARGB(230, 255, 255, 255));
    if (!s_cal.rawView)
    {
        float fx, fy, px, py;
        h64_padmap_stick_norm(cal, s_cal.raw[s][0], s_cal.raw[s][1], &fx, &fy);
        px = cx + fx * R;
        py = cy - fy * R;
        UiShadow(px - 9, py - 9, 18, 18, 9, 12, D3DCOLOR_ARGB(170, 90, 190, 255));
        UiCircle(px, py, 7, D3DCOLOR_ARGB(255, 120, 205, 255));
        UiCircle(px, py, 2.5f, D3DCOLOR_ARGB(255, 255, 255, 255));
    }
}

// One "label ... value" line of the readings column.
static void calib_line(float x, float w, float cy, const char *label, const char *value, D3DCOLOR vcol)
{
    UiText(UI_REGULAR, 16, x, cy, UI_COL_TEXT2, label, UI_LEFT);
    UiText(UI_BOLD, 18, x + w, cy, vcol, value, UI_RIGHT);
}

static float cal_norm(float raw) { float v = raw / 32767.0f; return v > -0.005f && v < 0.005f ? 0.0f : v; }

// The card over the right column while a test runs, or over both columns
// with the wizard's results.
static void calib_task_draw(void)
{
    char buf[200];
    if (s_cal.phase == CP_RESULT)
    {
        const float x = 500, y = 150, w = 670, h = 400;
        int k;
        UiRoundRect(x, y, w, h, 16, D3DCOLOR_ARGB(250, 30, 32, 42), D3DCOLOR_ARGB(250, 24, 26, 34));
        UiRoundRing(x, y, w, h, 16, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 24, x + 28, y + 34, UI_COL_TEXT, Tr(T_CAL_RESULTS), UI_LEFT);
        for (k = 0; k < 2; k++)
        {
            float cx = x + 28 + k * (w - 28) * 0.5f, cw = (w - 28) * 0.5f - 28, ly = y + 84;
            float mx, my, sd, maxDev, reach, drift;
            int coverage, circ, warn = 0, dead = cal_suggest_dead(k), outer = cal_suggest_outer(k);
            const H64PadMap *map = &s_cal.c->padMap[s_ctl.player];
            rest_stats(&s_cal.rest[k], &mx, &my, &sd, &maxDev);
            move_stats(&s_cal.move[k], &coverage, &circ, &reach);
            drift = sqrtf(mx * mx + my * my) / 32767.0f;
            heading(cx, ly, Tr(k ? T_CAL_RIGHT : T_CAL_LEFT));
            ly += 32;
            sprintf(buf, "%.2f, %.2f", cal_norm(mx), cal_norm(my));
            calib_line(cx, cw, ly, Tr(T_CAL_REST), buf, UI_COL_TEXT); ly += 28;
            sprintf(buf, "%.3f", drift);
            calib_line(cx, cw, ly, Tr(T_CAL_DRIFT), buf, drift * 100.0f > map->drift ? D3DCOLOR_ARGB(255, 255, 170, 90) : UI_COL_TEXT); ly += 28;
            sprintf(buf, "%.3f", sd / 32767.0f);
            calib_line(cx, cw, ly, Tr(T_CAL_STABILITY), buf, UI_COL_TEXT); ly += 28;
            sprintf(buf, "%d%%", (dead * 100 + 16383) / 32767);
            calib_line(cx, cw, ly, Tr(T_CAL_SUGGEST), buf, D3DCOLOR_ARGB(255, 120, 205, 255)); ly += 28;
            sprintf(buf, "%d%%", (outer * 100 + 16383) / 32767);
            calib_line(cx, cw, ly, Tr(T_CAL_TRAVEL), buf, D3DCOLOR_ARGB(255, 120, 205, 255)); ly += 28;
            if (coverage >= 75) sprintf(buf, "%d%%", circ); else strcpy(buf, "--");
            calib_line(cx, cw, ly, Tr(T_CAL_CIRC), buf, UI_COL_TEXT); ly += 40;
            // What looks wrong: too short a reach, lopsided, off centre.
            if (coverage >= 75 && reach < 26000) { UiText(UI_BOLD, 16, cx, ly, D3DCOLOR_ARGB(255, 255, 170, 90), Tr(T_CAL_WARN_RANGE), UI_LEFT); ly += 24; warn = 1; }
            if (coverage >= 75 && (circ < 85 || abs(s_cal.move[k].maxX + s_cal.move[k].minX) > 4000 || abs(s_cal.move[k].maxY + s_cal.move[k].minY) > 4000))
            { UiText(UI_BOLD, 16, cx, ly, D3DCOLOR_ARGB(255, 255, 170, 90), Tr(T_CAL_WARN_ASYM), UI_LEFT); ly += 24; warn = 1; }
            if (drift * 100.0f > map->drift) { UiText(UI_BOLD, 16, cx, ly, D3DCOLOR_ARGB(255, 255, 170, 90), Tr(T_CAL_WARN_CENTER), UI_LEFT); ly += 24; warn = 1; }
            if (!warn) UiText(UI_BOLD, 16, cx, ly, D3DCOLOR_ARGB(255, 120, 210, 140), Tr(T_CAL_WARN_NONE), UI_LEFT);
        }
        return;
    }
    {
        const float x = 800, y = 150, w = 370, h = 400;
        int step = 0, steps = 0, title = s_cal.task == CT_CENTER ? T_CAL_CENTER : s_cal.task == CT_DIAG ? T_CAL_DIAG : T_CAL_WIZARD;
        int prompt = s_cal.phase == CP_REST ? T_CAL_RELEASE : s_cal.phase == CP_CIRCLE_R ? T_CAL_CIRCLE_R : T_CAL_CIRCLE_L;
        DWORD total = s_cal.phase == CP_REST ? CAL_REST_MS : CAL_CIRCLE_MS;
        float frac = s_cal.phaseMs >= total ? 1.0f : s_cal.phaseMs / (float)total, ty;
        char wrapped[400];
        UiRoundRect(x, y, w, h, 16, D3DCOLOR_ARGB(250, 30, 32, 42), D3DCOLOR_ARGB(250, 24, 26, 34));
        UiRoundRing(x, y, w, h, 16, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 22, x + 24, y + 34, UI_COL_TEXT, Tr(title), UI_LEFT);
        if (s_cal.task == CT_WIZARD) { steps = 3; step = s_cal.phase + 1; }
        else if (s_cal.task == CT_DIAG) { steps = 2; step = s_cal.phase == CP_REST ? 1 : 2; }
        if (steps)
        {
            _snprintf(buf, sizeof(buf) - 1, Tr(T_CAL_STEP), step, steps);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 16, x + 24, y + 64, UI_COL_TEXT3, buf, UI_LEFT);
        }
        UiTextWrap(UI_REGULAR, 20, w - 48, 5, Tr(prompt), wrapped, sizeof(wrapped));
        UiText(UI_REGULAR, 20, x + 24, y + 112, UI_COL_TEXT, wrapped, UI_LEFT);
        ty = y + h - 96;
        if (s_cal.phase != CP_REST)
        {
            int coverage, circ;
            float reach;
            move_stats(&s_cal.move[s_cal.phase == CP_CIRCLE_R], &coverage, &circ, &reach);
            _snprintf(buf, sizeof(buf) - 1, Tr(T_CAL_COVERAGE), coverage);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 16, x + 24, ty - 28, UI_COL_TEXT2, buf, UI_LEFT);
        }
        UiRoundRect(x + 24, ty, w - 48, 8, 4, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
        if (frac > 0) UiRoundRect(x + 24, ty, (w - 48) * frac, 8, 4, D3DCOLOR_ARGB(255, 150, 220, 255), D3DCOLOR_ARGB(255, 70, 160, 240));
        if (s_cal.paused) UiText(UI_BOLD, 18, x + 24, ty + 40, D3DCOLOR_ARGB(255, 255, 190, 90), Tr(T_CAL_PAUSED), UI_LEFT);
        else if (!s_cal.connected)
        {
            _snprintf(buf, sizeof(buf) - 1, Tr(T_CAL_NO_PAD), s_ctl.player + 1);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_BOLD, 18, x + 24, ty + 40, D3DCOLOR_ARGB(255, 255, 170, 90), buf, UI_LEFT);
        }
    }
}

static void calib_draw(const UiMenu *m)
{
    static UiAnim selY;
    const float x = CTL_X, y = CTL_Y;
    const float mcx = 295, mcy = 360, R = 170, midX = 500, midW = 280, setX = 800, setW = 370, rowH = 38, setTop = 168;
    const H64PadMap *map;
    H64PadMap def;
    int s = s_cal.task && s_cal.phase == CP_CIRCLE_L ? 0 : s_cal.task && s_cal.phase == CP_CIRCLE_R ? 1 : s_cal.stick, i;
    const H64StickCal *cal;
    char buf[200];
    float ly;
    if (s_cal.c) map = &s_cal.c->padMap[s_ctl.player];
    else { h64_padmap_default(&def); map = &def; }
    cal = &map->cal[s];

    UiDim();
    UiPanel(x, y, CTL_W, CTL_H, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_CALIBRATION), UI_LEFT);
    {
        // Stick selector at the top right: "LB  Stick izquierdo  RB", the player under it.
        float r = x + CTL_W - 40, w;
        const char *name = Tr(s ? T_CAL_RIGHT : T_CAL_LEFT);
        r -= UiFooterWidth("RB");
        UiFooter(r, y + 50, "RB");
        w = UiTextWidth(UI_BOLD, 22, name);
        UiText(UI_BOLD, 22, r - 16, y + 50, UI_COL_TEXT, name, UI_RIGHT);
        r -= 16 + w + 16 + UiFooterWidth("LB");
        UiFooter(r, y + 50, "LB");
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PLAYER), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 16, x + CTL_W - 40, y + 82, UI_COL_TEXT3, buf, UI_RIGHT);
    }

    calib_monitor(mcx, mcy, R, s, cal);
    if (!s_cal.connected)
    {
        _snprintf(buf, sizeof(buf) - 1, Tr(T_CAL_NO_PAD), s_ctl.player + 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_BOLD, 18, mcx, mcy + R + 30, D3DCOLOR_ARGB(255, 255, 170, 90), buf, UI_CENTER);
    }
    else if (s_cal.paused) UiText(UI_BOLD, 18, mcx, mcy + R + 30, D3DCOLOR_ARGB(255, 255, 190, 90), Tr(T_CAL_PAUSED), UI_CENTER);
    else UiText(UI_BOLD, 16, mcx, mcy + R + 30, UI_COL_TEXT3, Tr(s ? T_CAL_RIGHT : T_CAL_LEFT), UI_CENTER);

    // Readings: where the stick is, and what the emulator makes of it.
    {
        float nx = cal_norm((float)s_cal.raw[s][0]), ny = cal_norm((float)s_cal.raw[s][1]), fx, fy;
        h64_padmap_stick_norm(cal, s_cal.raw[s][0], s_cal.raw[s][1], &fx, &fy);
        heading(midX, 150, Tr(T_CAL_HEAD_LIVE));
        ly = 176;
        sprintf(buf, "%.2f", nx); calib_line(midX, midW, ly, Tr(T_CAL_XPOS), buf, UI_COL_TEXT); ly += 26;
        sprintf(buf, "%.2f", ny); calib_line(midX, midW, ly, Tr(T_CAL_YPOS), buf, UI_COL_TEXT); ly += 26;
        sprintf(buf, "%.3f", sqrtf(nx * nx + ny * ny)); calib_line(midX, midW, ly, Tr(T_CAL_DIST), buf, UI_COL_TEXT); ly += 26;
        sprintf(buf, "%.3f", s_cal.rawView ? sqrtf(nx * nx + ny * ny) : sqrtf(fx * fx + fy * fy));
        calib_line(midX, midW, ly, Tr(T_CAL_OUT), buf, D3DCOLOR_ARGB(255, 120, 205, 255)); ly += 26;
        // The dead zone, and whether the stick is in it right now (what the game is ignoring).
        sprintf(buf, "%d%%", (cal->deadzone * 100 + 16383) / 32767);
        calib_line(midX, midW, ly, Tr(T_DEAD_ZONE), buf, D3DCOLOR_ARGB(255, 120, 205, 255)); ly += 26;
        if (s_cal.connected)
        {
            int ignored = !s_cal.rawView && fx == 0.0f && fy == 0.0f;
            calib_line(midX, midW, ly, Tr(T_CAL_STATE), Tr(ignored ? T_CAL_IGNORED : T_CAL_ACTIVE),
                       ignored ? UI_COL_TEXT3 : D3DCOLOR_ARGB(255, 120, 210, 140));
        }
        else calib_line(midX, midW, ly, Tr(T_CAL_STATE), "--", UI_COL_TEXT3);
    }
    // The last diagnosis of this stick.
    {
        float mx, my, sd, maxDev, reach, drift = 0;
        int coverage = 0, circ = 0, rest = s_cal.haveRest[s], mv = s_cal.haveMove[s];
        rest_stats(&s_cal.rest[s], &mx, &my, &sd, &maxDev);
        move_stats(&s_cal.move[s], &coverage, &circ, &reach);
        if (rest) drift = sqrtf(mx * mx + my * my) / 32767.0f;
        heading(midX, 344, Tr(T_CAL_HEAD_DIAG));
        ly = 370;
        if (rest) sprintf(buf, "%.2f, %.2f", cal_norm(mx), cal_norm(my)); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_REST), buf, UI_COL_TEXT); ly += 26;
        if (rest) sprintf(buf, "%.3f", drift); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_DRIFT), buf, rest && drift * 100.0f > map->drift ? D3DCOLOR_ARGB(255, 255, 170, 90) : UI_COL_TEXT); ly += 26;
        if (rest) sprintf(buf, "%.3f", sd / 32767.0f); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_STABILITY), buf, UI_COL_TEXT); ly += 26;
        if (mv) sprintf(buf, "%.2f / %.2f", cal_norm((float)s_cal.move[s].minX), cal_norm((float)s_cal.move[s].maxX)); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_RANGE_X), buf, UI_COL_TEXT); ly += 26;
        if (mv) sprintf(buf, "%.2f / %.2f", cal_norm((float)s_cal.move[s].minY), cal_norm((float)s_cal.move[s].maxY)); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_RANGE_Y), buf, UI_COL_TEXT); ly += 26;
        if (mv && coverage >= 75) sprintf(buf, "%d%%", circ); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_CIRC), buf, UI_COL_TEXT); ly += 26;
        if (mv) sprintf(buf, "%d", s_cal.move[s].jumps); else strcpy(buf, "--");
        calib_line(midX, midW, ly, Tr(T_CAL_JUMPS), buf, mv && s_cal.move[s].jumps ? D3DCOLOR_ARGB(255, 255, 170, 90) : UI_COL_TEXT); ly += 30;
        if (rest)
        {
            int bad = drift * 100.0f > map->drift;
            UiText(UI_BOLD, 18, midX, ly, bad ? D3DCOLOR_ARGB(255, 255, 170, 90) : D3DCOLOR_ARGB(255, 120, 210, 140),
                   Tr(bad ? T_CAL_STATUS_DRIFT : T_CAL_STATUS_OK), UI_LEFT);
        }
    }

    // Settings.
    heading(setX + 14, 150, Tr(T_CAL_HEAD_SET));
    if (m->count) selection_bar(setX, anim_step(&selY, setTop + m->sel * rowH, 0) + 3, setW, rowH - 6);
    for (i = 0; i < CR_COUNT; i++)
    {
        static const int labels[CR_COUNT] = { T_DEAD_ZONE, T_SENSITIVITY, T_CAL_CURVE, T_INVERT, T_CAL_DRIFT_THR, T_CAL_RAW,
                                              T_CAL_CENTER, T_CAL_DIAG, T_CAL_WIZARD, T_RESET_DEFAULT };
        const char *inverts[4] = { Tr(T_OFF), "X", "Y", Tr(T_X_AND_Y) };
        float cy = setTop + i * rowH + rowH * 0.5f;
        int sel = i == m->sel;
        UiText(sel ? UI_BOLD : UI_REGULAR, 19, setX + 22, cy, sel ? UI_COL_TEXT : UI_COL_TEXT2, Tr(labels[i]), UI_LEFT);
        buf[0] = 0;
        switch (i)
        {
        case CR_DEAD: sprintf(buf, "< %d%% >", (cal->deadzone * 100 + 16383) / 32767); break;
        case CR_SENS: sprintf(buf, "< %d%% >", cal->range); break;
        case CR_CURVE: sprintf(buf, "< %.1f >", cal->curve / 100.0f); break;
        case CR_INVERT: sprintf(buf, "< %s >", inverts[cal->invert & 3]); break;
        case CR_DRIFT: sprintf(buf, "< %d%% >", map->drift); break;
        case CR_RAW: sprintf(buf, "< %s >", onoff(s_cal.rawView)); break;
        case CR_CENTER: sprintf(buf, "%.2f, %.2f", cal_norm((float)cal->cx), cal_norm((float)cal->cy)); break;
        }
        if (buf[0]) draw_value(setX + setW - 14, cy, buf, sel);
    }
    if (s_cal.task) calib_task_draw();

    // Above the prompts: the last result for a few seconds, else what this page can and cannot do.
    if (s_cal.msg[0] && GetTickCount() - s_cal.msgAt < 3000)
        UiText(UI_BOLD, 18, x + CTL_W * 0.5f, y + CTL_H - 98, D3DCOLOR_ARGB(255, 120, 205, 255), s_cal.msg, UI_CENTER);
    else UiText(UI_REGULAR, 15, x + CTL_W * 0.5f, y + CTL_H - 98, UI_COL_TEXT3, Tr(T_CAL_NOTE), UI_CENTER);
    UiRect(x + 24, y + CTL_H - 78, CTL_W - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + CTL_H - 39, Tr(!s_cal.task ? T_FOOT_CAL : s_cal.phase == CP_RESULT ? T_FOOT_CAL_RESULT : T_FOOT_CAL_TASK));
}

void SettingsBuild(UiMenu *m, const Config *c, int perGame)
{
    s_perGame = perGame;
    s_browser = 0;
    s_ctl.capturing = 0;
    s_ctl.autoMode = 0;
    settings_main(m, c);
}

static int graphics_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_GRAPHICS;
        return SET_NONE;
    }
    if (m->sel == G_HIRES && c->hiTex && HiresPackCount() && (down & XINPUT_GAMEPAD_Y)) return SET_PREVIEW;
    if (dir || (down & XINPUT_GAMEPAD_A))
    {
        if (!dir) dir = 1;
        switch (m->sel)
        {
        case G_PRESET:
        {
            int pr = current_preset(c);
            const GfxPreset *p;
            pr = pr < 0 ? (dir > 0 ? 0 : 3) : (pr + dir + 4) % 4;
            p = &s_presets[pr];
            c->texFilter = p->texFilter;
            c->smooth = p->smooth;
            c->sharpen = p->sharpen;
            c->blur = p->blur;
            c->screen = p->screen;
            break;
        }
        case G_FILTER: c->texFilter = (c->texFilter + dir + 3) % 3; break;
        case G_HIRES:
        {
            // Cycles Off, then one state per pack found for the ROM in context (see
            // HiresScanPacks) - same "dir or A moves it on" pattern as the rest of this page.
            int hn = HiresPackCount(), states = hn + 1, cur = 0, k;
            if (c->hiTex && hn)
            {
                cur = 1;
                if (c->hiTexFile[0])
                    for (k = 0; k < hn; k++)
                        if (!strcmp(HiresPackName(k), c->hiTexFile)) { cur = k + 1; break; }
            }
            cur = (cur + dir + states) % states;
            if (cur == 0) c->hiTex = 0;
            else
            {
                c->hiTex = 1;
                strncpy(c->hiTexFile, HiresPackName(cur - 1), sizeof(c->hiTexFile) - 1);
                c->hiTexFile[sizeof(c->hiTexFile) - 1] = 0;
            }
            break;
        }
        case G_SMOOTH: c->smooth = !c->smooth; break;
        case G_SHARPEN: c->sharpen = (c->sharpen + dir + 3) % 3; if (c->sharpen) c->blur = 0; break;
        case G_BLUR: c->blur = (c->blur + dir + 3) % 3; if (c->blur) c->sharpen = 0; break;
        case G_FPS: c->showFps = ((c->showFps < 0 || c->showFps > 4 ? 0 : c->showFps) + dir + 5) % 5; break;   // off, FPS, + CPU/RAM, + resolution, + GPU
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down)
{
    s_perGame = perGame;
    if (s_graphicsPage) return graphics_input(m, c, down);
    if (s_calPage) return cal_input(m, c, down);
    if (s_screenPage) return screen_input(m, c, down);
    if (s_interfacePage) return interface_input(m, c, down);
    if (m->page == UI_PAGE_CONTROLS) return controls_input(m, c, down);
    if (m->page == UI_PAGE_CALIB) return calib_input(m, c, down);
    if (m->page == UI_PAGE_VOLUME) return volume_input(m, c, down);
    if (s_soundPage) return sound_input(m, c, down);
    if (s_systemPage) return system_input(m, c, down);
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B) return SET_BACK;
    if (m->sel == S_SYSTEM && (down & XINPUT_GAMEPAD_A))
    {
        settings_system(m, c);
        return SET_NONE;
    }
    if (m->sel == S_GRAPHICS && (down & XINPUT_GAMEPAD_A))
    {
        settings_graphics(m, c);
        return SET_NONE;
    }
    if (m->sel == S_SCREEN && (down & XINPUT_GAMEPAD_A))
    {
        settings_screen(m, c);
        return SET_NONE;
    }
    if (m->sel == S_SOUND && (down & XINPUT_GAMEPAD_A))
    {
        settings_sound(m, c);
        return SET_NONE;
    }
    if (m->sel == S_CONTROLS && (down & XINPUT_GAMEPAD_A))
    {
        controls_open(m, c);
        return SET_NONE;
    }
    if (m->sel == S_INTERFACE && (down & XINPUT_GAMEPAD_A))
    {
        settings_interface(m, c);
        return SET_NONE;
    }
    settings_values(m, c);
    if (down & XINPUT_GAMEPAD_A)
    {
        if (m->sel == S_SAVE_ALL) return SET_SAVE_ALL;
        if (perGame && m->sel == S_SAVE_ALL + 1) return SET_SAVE_GAME;
    }
    return SET_NONE;
}

// ---- Leaving ----
// A yes/no question ("Quit to the dashboard?", "Download the box art?"): 1 for A, 0 for B or BACK.
// `title` (-1: none) above the question; everything centred, the panel just
// big enough for its text (a little room around, no wide margins).
static int confirm_ask(IDirect3DDevice9 *dev, int title, int question, int footer)
{
    const float pad = 44.0f, titleH = title >= 0 ? 46.0f : 0.0f, qSize = title >= 0 ? 22.0f : 30.0f;
    float tw = title >= 0 ? UiTextWidth(UI_BOLD, 30, Tr(title)) : 0, qw = UiTextWidth(title >= 0 ? UI_REGULAR : UI_BOLD, qSize, Tr(question));
    float fw = UiFooterWidth(Tr(footer)), w = tw, h = 36 + titleH + 44 + 26 + 66;
    UiInput in;
    if (qw > w) w = qw;
    if (fw > w) w = fw;
    w += pad * 2;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        const float x = (UI_WIDTH - w) / 2, y = (UI_HEIGHT - h) / 2, cx = UI_WIDTH * 0.5f;
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiPanel(x, y, w, h, 22);
        if (title >= 0) UiText(UI_BOLD, 30, cx, y + 36 + 18, UI_COL_TEXT, Tr(title), UI_CENTER);
        UiText(title >= 0 ? UI_REGULAR : UI_BOLD, qSize, cx, y + 36 + titleH + 18, title >= 0 ? UI_COL_TEXT2 : UI_COL_TEXT, Tr(question), UI_CENTER);
        UiRect(x + 24, y + h - 66, w - 48, 1, UI_COL_LINE);
        UiFooter(cx - fw * 0.5f, y + h - 33, Tr(footer));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

static int confirm_quit(IDirect3DDevice9 *dev) { return confirm_ask(dev, T_QUIT_TITLE, T_QUIT_ASK, T_FOOT_QUIT); }

// ---- About ----
// The credits panel: from the ROM browser (X) and the in-game menu.
void AboutDraw(void)
{
    const float x = 150, y = 46, w = 980, h = 628, cx = x + 52, col2 = x + 560;
    float ty;
    UiDim();
    UiPanel(x, y, w, h, 24);
    UiBrand(cx, y + 72, 44);
    UiText(UI_REGULAR, 18, x + w - 52, y + 62, UI_COL_TEXT2, "Version " H64_VERSION_STRING, UI_RIGHT);
    UiText(UI_REGULAR, 16, x + w - 52, y + 88, UI_COL_TEXT3, "Built " __DATE__, UI_RIGHT);
    UiText(UI_REGULAR, 24, cx, y + 146, UI_COL_TEXT, "A Nintendo 64 emulator for the Xbox 360", UI_LEFT);
    {
        float a = UiText(UI_REGULAR, 20, cx, y + 184, UI_COL_TEXT2, "Created by ", UI_LEFT);
        UiText(UI_BOLD, 20, cx + a, y + 184, UI_COL_TEXT, "Mohamed Aymen (kernel64)", UI_LEFT);
    }
    UiText(UI_REGULAR, 18, cx, y + 214, UI_COL_TEXT3, "github.com/kernel64/Harissa64", UI_LEFT);
    UiRect(x + 32, y + 246, w - 64, 1, UI_COL_LINE);

    ty = y + 280;
    heading(cx, ty, "ENGINE");
    paragraph(UI_REGULAR, 19, cx, ty + 16, 460, 6, UI_COL_TEXT2,
              "MIPS R4300i to PowerPC dynamic recompiler.\nRSP: high-level graphics and audio, low-level interpreter.\n"
              "RDP on the Xenos GPU, software RDP for reference.");
    heading(col2, ty, "ACKNOWLEDGEMENTS");
    ty += 16 + paragraph(UI_REGULAR, 19, col2, ty + 16, 368, 6, UI_COL_TEXT2,
                         "Thanks to the ares, ParaLLEl-RDP, mupen64plus, GLideN64, libdragon and zlib projects, "
                         "and to Timothy Lottes and AMD for their display filters. Box art and game "
                         "information: libretro, GameDB-N64, Wikipedia.");
    UiText(UI_REGULAR, 17, col2, ty + 18, UI_COL_TEXT3, "Details and licences: THIRD_PARTY.md", UI_LEFT);

    ty = y + 474;
    heading(cx, ty, "LICENCE");
    paragraph(UI_REGULAR, 17, cx, ty + 14, w - 104, 4, UI_COL_TEXT3,
              "Free software under the GNU GPL v2. Interface font: Inter (SIL Open Font License).\n"
              "Nintendo 64 is a trademark of Nintendo; this project is not affiliated with Nintendo. "
              "Play only games dumped from cartridges you own.");
    UiRect(x + 32, y + h - 72, w - 64, 1, UI_COL_LINE);
    UiFooter(cx, y + h - 36, "B:Back");
}

void AboutScreen(IDirect3DDevice9 *dev)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_BACK)) return;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        AboutDraw();
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- Hi-res pack preview ----
// Packs carry no preview image of their own (see xb_menu.h/h64_hts.h): one
// representative texture - the pack's middle entry by checksum order,
// h64_hts_sample_checksum - is decoded and shown scaled to fit, with the
// pack's file name and texture count.
#define HIRES_PREVIEW_MAX 512   // the shown image never exceeds this on either side
static IDirect3DTexture9 *s_hiresPreviewTex;
static int s_hiresPreviewW, s_hiresPreviewH;
static char s_hiresPreviewWant[160];    // full path HiresPreviewDraw should be showing
static char s_hiresPreviewShown[160];   // full path s_hiresPreviewTex is actually for
static int s_hiresPreviewCount;         // the pack's texture count (0: none decoded/failed)

void HiresPreviewOpen(const Config *c)
{
    int i = 0, k;
    s_hiresPreviewWant[0] = 0;
    if (!c->hiTex || !HiresPackCount()) return;
    if (c->hiTexFile[0])
        for (k = 0; k < HiresPackCount(); k++)
            if (!strcmp(HiresPackName(k), c->hiTexFile)) { i = k; break; }
    strncpy(s_hiresPreviewWant, HiresPackPath(i), sizeof(s_hiresPreviewWant) - 1);
    s_hiresPreviewWant[sizeof(s_hiresPreviewWant) - 1] = 0;
}

static void hires_preview_decode(IDirect3DDevice9 *dev)
{
    H64Hts *h;
    const u32 *px;
    u32 w, hgt, i, n;
    u64 crc;
    if (s_hiresPreviewTex) { s_hiresPreviewTex->Release(); s_hiresPreviewTex = NULL; }
    strncpy(s_hiresPreviewShown, s_hiresPreviewWant, sizeof(s_hiresPreviewShown) - 1);
    s_hiresPreviewShown[sizeof(s_hiresPreviewShown) - 1] = 0;
    s_hiresPreviewCount = 0;
    if (!s_hiresPreviewWant[0]) return;
    h = h64_hts_open(s_hiresPreviewWant);
    if (!h) return;
    s_hiresPreviewCount = (int)h64_hts_count(h);
    crc = h64_hts_sample_checksum(h);
    if (crc && h64_hts_find(h, crc, &px, &w, &hgt) && w && hgt)
    {
        u8 *rgba = (u8 *)malloc((size_t)w * hgt * 4);
        if (rgba)
        {
            int outW, outH;
            u8 *scaled;
            n = w * hgt;
            for (i = 0; i < n; i++)
            {
                // h64_hts gives ARGB words (a<<24|r<<16|g<<8|b); UiImageCreate wants R,G,B,A bytes.
                u32 v = px[i];
                rgba[i * 4 + 0] = (u8)(v >> 16);
                rgba[i * 4 + 1] = (u8)(v >> 8);
                rgba[i * 4 + 2] = (u8)v;
                rgba[i * 4 + 3] = (u8)(v >> 24);
            }
            h64_image_fit((int)w, (int)hgt, HIRES_PREVIEW_MAX, HIRES_PREVIEW_MAX, &outW, &outH);
            scaled = (outW == (int)w && outH == (int)hgt) ? rgba : h64_image_scale(rgba, (int)w, (int)hgt, outW, outH);
            if (scaled)
            {
                s_hiresPreviewTex = UiImageCreate(dev, scaled, outW, outH);
                s_hiresPreviewW = outW;
                s_hiresPreviewH = outH;
                if (scaled != rgba) free(scaled);
            }
            free(rgba);
        }
    }
    h64_hts_close(h);
}

void HiresPreviewDraw(IDirect3DDevice9 *dev)
{
    const float w = 700, h = 560, x = (UI_WIDTH - w) / 2, y = (UI_HEIGHT - h) / 2;
    const float boxW = w - 96, boxH = 340, boxX = x + 48, boxY = y + 90;
    char buf[96];
    if (strcmp(s_hiresPreviewShown, s_hiresPreviewWant)) hires_preview_decode(dev);
    UiDim();
    UiPanel(x, y, w, h, 24);
    UiText(UI_BOLD, 26, x + 48, y + 50, UI_COL_TEXT, Tr(T_PACK_PREVIEW), UI_LEFT);
    UiRoundRect(boxX, boxY, boxW, boxH, 12, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    if (s_hiresPreviewTex)
    {
        float iw = (float)s_hiresPreviewW, ih = (float)s_hiresPreviewH;
        float k = boxW / iw < boxH / ih ? boxW / iw : boxH / ih;
        float dw = iw * k, dh = ih * k, dx = boxX + (boxW - dw) * 0.5f, dy = boxY + (boxH - dh) * 0.5f;
        UiImage(s_hiresPreviewTex, dx, dy, dw, dh, 8, D3DCOLOR_ARGB(255, 255, 255, 255));
    }
    else
        UiText(UI_REGULAR, 19, boxX + boxW * 0.5f, boxY + boxH * 0.5f, UI_COL_TEXT3, Tr(T_NO_PREVIEW), UI_CENTER);
    {
        const char *slash = strrchr(s_hiresPreviewShown, '\\');
        UiText(UI_REGULAR, 19, x + 48, boxY + boxH + 36, UI_COL_TEXT2, slash ? slash + 1 : s_hiresPreviewShown, UI_LEFT);
    }
    _snprintf(buf, sizeof(buf) - 1, Tr(T_N_TEXTURES), s_hiresPreviewCount);
    buf[sizeof(buf) - 1] = 0;
    UiText(UI_REGULAR, 17, x + 48, boxY + boxH + 62, UI_COL_TEXT3, buf, UI_LEFT);
    UiFooter(x + 48, y + h - 36, "B:Back");
}

void HiresPreviewScreen(IDirect3DDevice9 *dev, const Config *c)
{
    UiInput in;
    HiresPreviewOpen(c);
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_BACK)) return;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        HiresPreviewDraw(dev);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- Language list ----
// Seven cards (four, then three), each with its flag and the language's own
// name; the title and the button prompt follow the card under the cursor.
#define LANG_CARD_W 220.0f
#define LANG_CARD_H 196.0f
#define LANG_GAP 24.0f

static void lang_card_pos(int i, float *x, float *y)
{
    int row = i < 4 ? 0 : 1, n = row ? LANG_COUNT - 4 : 4, col = row ? i - 4 : i;
    float total = n * LANG_CARD_W + (n - 1) * LANG_GAP;
    *x = (UI_WIDTH - total) * 0.5f + col * (LANG_CARD_W + LANG_GAP);
    *y = row ? 408.0f : 188.0f;
}

// The card in the other row nearest to card `i` (up/down).
static int lang_other_row(int i)
{
    float x, y, bx, by, best = 1e9f;
    int k, pick = i;
    lang_card_pos(i, &x, &y);
    for (k = 0; k < LANG_COUNT; k++)
    {
        float d;
        if ((k < 4) == (i < 4)) continue;
        lang_card_pos(k, &bx, &by);
        d = fabsf(bx - x);
        if (d < best) { best = d; pick = k; }
    }
    return pick;
}

static int s_langAutoMs;
void LanguageScreenAuto(int ms) { s_langAutoMs = ms; }

int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart)
{
    IDirect3DTexture9 *flags[LANG_COUNT];
    UiInput in;
    UiAnim ax, ay;
    int sel = initial >= 0 && initial < LANG_COUNT ? initial : LANG_EN, i, result = -2;
    DWORD openedAt = GetTickCount();
    memset(&ax, 0, sizeof(ax));
    memset(&ay, 0, sizeof(ay));
    for (i = 0; i < LANG_COUNT; i++)
    {
        int w = 0, h = 0;
        u8 *px = h64_png_decode(s_flagPng[i], s_flagPngSize[i], &w, &h, FLAG_W * FLAG_H);
        flags[i] = px ? UiImageCreate(dev, px, w, h) : NULL;
        free(px);
    }
    UiInputInit(&in);
    H64_INFO("[lang] language list (%s)", firstStart ? "first start" : "settings");
    while (result == -2)
    {
        WORD down = UiInputPoll(&in);
        float cx, cy, now = UiTime();
        if (down & XINPUT_GAMEPAD_DPAD_LEFT) sel = (sel + LANG_COUNT - 1) % LANG_COUNT;
        if (down & XINPUT_GAMEPAD_DPAD_RIGHT) sel = (sel + 1) % LANG_COUNT;
        if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) sel = lang_other_row(sel);
        if ((down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) || (s_langAutoMs > 0 && GetTickCount() - openedAt >= (DWORD)s_langAutoMs)) result = sel;
        else if (!firstStart && (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))) result = -1;

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiBrand(UI_SAFE_X, 72, 34);
        UiText(UI_BOLD, 34, UI_WIDTH * 0.5f, 134, UI_COL_TEXT, TrIn(sel, T_LANG_TITLE), UI_CENTER);
        lang_card_pos(sel, &cx, &cy);
        cx = anim_step(&ax, cx, 0);
        cy = anim_step(&ay, cy, 0);
        selection_glow(cx - 4, cy - 4, LANG_CARD_W + 8, LANG_CARD_H + 8, 24, selection_pulse());
        for (i = 0; i < LANG_COUNT; i++)
        {
            float x, y;
            int s = i == sel;
            lang_card_pos(i, &x, &y);
            UiShadow(x, y + 8, LANG_CARD_W, LANG_CARD_H, 20, 24, UI_COL_SHADOW);
            UiRoundRect(x, y, LANG_CARD_W, LANG_CARD_H, 20, s ? D3DCOLOR_ARGB(255, 36, 46, 62) : UI_COL_PANEL_TOP,
                        s ? D3DCOLOR_ARGB(255, 28, 35, 48) : UI_COL_PANEL);
            if (s) UiRoundRing(x - 3, y - 3, LANG_CARD_W + 6, LANG_CARD_H + 6, 23, 3.0f, SEL_RING(selection_pulse()));
            else UiRoundRing(x, y, LANG_CARD_W, LANG_CARD_H, 20, 1.0f, UI_COL_LINE);
            if (flags[i])
            {
                const float fw = 176, fh = 117, fx = x + (LANG_CARD_W - fw) * 0.5f, fy = y + 20;
                UiShadow(fx, fy + 4, fw, fh, 8, 12, D3DCOLOR_ARGB(140, 0, 0, 0));
                UiImage(flags[i], fx, fy, fw, fh, 8, s ? D3DCOLOR_ARGB(255, 255, 255, 255) : D3DCOLOR_ARGB(255, 200, 200, 200));
                UiRoundRing(fx, fy, fw, fh, 8, 1.0f, D3DCOLOR_ARGB(50, 255, 255, 255));
            }
            UiText(s ? UI_BOLD : UI_REGULAR, 22, x + LANG_CARD_W * 0.5f, y + 166, s ? UI_COL_TEXT : UI_COL_TEXT2, TrIn(i, T_LANG_NAME), UI_CENTER);
        }
        {
            // The selected card's outline, pulsing gently.
            float on = 0.75f + 0.25f * sinf(now * 4.0f);
            UiRoundRing(cx - 3, cy - 3, LANG_CARD_W + 6, LANG_CARD_H + 6, 23, 3.0f, D3DCOLOR_ARGB((int)(255 * on), 255, 112, 84));
        }
        UiScreenFooter(TrIn(sel, firstStart ? T_FOOT_LANG_FIRST : T_FOOT_LANG));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    for (i = 0; i < LANG_COUNT; i++)
        if (flags[i]) UiImageRelease(dev, flags[i]);
    H64_INFO("[lang] language list: %s", result >= 0 ? LangCode(result) : "left");
    return result;
}

// ---- Low battery warning (Settings > System) ----
// The battery level of a wireless controller is not in the public XInput of
// the XDK; xam.xex has it (XamInputGetDeviceStats, ordinal 407: the import
// comes from the XDK's own xav.lib) filling an XINPUT_DEVICE_STATS, whose
// fields the XDK's type information names (below). Its parameters are not
// documented: the stats buffer goes as both the second and the third one, so
// it is the one written to whether it takes (user, stats) or (user, flags,
// stats). What it gives is logged, to be checked on the console.
struct XInputDeviceStats
{
    ULONG batteryLevel, batteryType, signalStrength, deviceType, devicePort, deviceVersion;
    BOOL jumped;
    ULONG spare[9];   // room, should the system's structure be longer
};
extern "C" DWORD WINAPI XamInputGetDeviceStats(DWORD userIndex, void *second, void *third);

#define BATTERY_CHECK_MS 10000      // how often the controllers are asked
#define BATTERY_REPEAT_MS 300000    // a controller still low is said again after this

// A controller's battery in percent (the level as the 0..3 of XInput's
// BATTERY_LEVEL_* buckets, or a percentage if it is larger), or -1: none known.
static int battery_percent(int port)
{
    static const int bucket[4] = { 5, 25, 45, 100 };
    static int logged[4] = { -2, -2, -2, -2 };
    XINPUT_CAPABILITIES caps;
    XInputDeviceStats st;
    DWORD r;
    int pct;
    if (XInputGetCapabilities(port, XINPUT_FLAG_GAMEPAD, &caps) != ERROR_SUCCESS || !(caps.Flags & XINPUT_CAPS_WIRELESS)) return -1;
    memset(&st, 0, sizeof(st));
    r = XamInputGetDeviceStats(port, &st, &st);
    pct = r != ERROR_SUCCESS ? -1 : st.batteryLevel <= 3 ? bucket[st.batteryLevel] : st.batteryLevel <= 100 ? (int)st.batteryLevel : -1;
    if (pct != logged[port])
    {
        H64_INFO("[battery] controller %d: result %u, level %u, type %u, signal %u, device %u -> %d%%", port + 1, (unsigned)r,
                 (unsigned)st.batteryLevel, (unsigned)st.batteryType, (unsigned)st.signalStrength, (unsigned)st.deviceType, pct);
        logged[port] = pct;
    }
    return pct;
}

int BatteryCheck(const Config *c, char *msg, int size)
{
    static DWORD lastCheck, lastWarn[4];
    DWORD now = GetTickCount();
    int port;
    if (!c->batteryWarn || size <= 0) return 0;
    if (lastCheck && now - lastCheck < BATTERY_CHECK_MS) return 0;
    lastCheck = now;
    for (port = 0; port < 4; port++)
    {
        int pct = battery_percent(port);
        if (pct < 0 || pct > c->batteryWarn) { lastWarn[port] = 0; continue; }   // fine again (or gone): said at once next time
        if (lastWarn[port] && now - lastWarn[port] < BATTERY_REPEAT_MS) continue;
        lastWarn[port] = now | 1;
        _snprintf(msg, size - 1, Tr(T_BATTERY_LOW), port + 1);
        msg[size - 1] = 0;
        return 1;
    }
    return 0;
}

// ---- ROM browser ----
struct RomEntry { std::string file, shown; };
static bool rom_less(const RomEntry &a, const RomEntry &b) { return _stricmp(a.shown.c_str(), b.shown.c_str()) < 0; }

static void scan_roms(std::vector<RomEntry> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("game:\\roms\\*", &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const char *dot = strrchr(fd.cFileName, '.');
        RomEntry e;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64") && _stricmp(dot, ".zip"))) continue;
        e.file = std::string("game:\\roms\\") + fd.cFileName;
        e.shown = std::string(fd.cFileName, dot - fd.cFileName);
        out->push_back(e);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    std::sort(out->begin(), out->end(), rom_less);
}

// ---- ROM files (plain or zipped) ----
static u32 handle_read(void *user, u32 offset, void *buf, u32 len)
{
    DWORD got = 0;
    if (SetFilePointer((HANDLE)user, (LONG)offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return 0;
    if (!ReadFile((HANDLE)user, buf, len, &got, NULL)) return 0;
    return got;
}

// Opens `path`; for a zip, finds the ROM inside (*isZip set). Returns the
// handle (INVALID_HANDLE_VALUE on failure) and the ROM size in *size.
static HANDLE rom_open(const char *path, H64ZipReader *r, H64ZipEntry *e, int *isZip, u32 *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    u8 magic[4];
    *isZip = 0;
    if (h == INVALID_HANDLE_VALUE) return h;
    r->user = h;
    r->size = GetFileSize(h, NULL);
    r->read = handle_read;
    *size = r->size;
    if (handle_read(h, 0, magic, 4) == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4)
    {
        if (h64_zip_find_rom(r, e)) { H64_WARN("[rom] no N64 ROM in %s", path); CloseHandle(h); return INVALID_HANDLE_VALUE; }
        *isZip = 1;
        *size = e->size;
    }
    return h;
}

u8 *RomFileLoad(const char *path, u32 *size)
{
    H64ZipReader r;
    H64ZipEntry e;
    int zip, ok;
    u32 len;
    HANDLE h = rom_open(path, &r, &e, &zip, &len);
    u8 *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    buf = (u8 *)malloc(len ? len : 1);
    if (!buf) { CloseHandle(h); return NULL; }
    ok = zip ? h64_zip_extract(&r, &e, buf, len) == 0 : handle_read(h, 0, buf, len) == len;
    CloseHandle(h);
    if (!ok) { H64_WARN("[rom] cannot read %s%s", path, zip ? " (bad zip entry)" : ""); free(buf); return NULL; }
    if (zip) H64_INFO("[rom] %s: %s (%u bytes)", path, e.name, len);
    *size = len;
    return buf;
}

// Reads the first 4 KB of a ROM (any dump order, plain or zipped) into *rom.
// Returns the ROM size, or 0 when it is not an N64 ROM. Any thread.
static u32 read_header(const char *path, H64Rom *rom)
{
    const u32 headSize = 0x1000;
    u8 *head;
    H64ZipReader r;
    H64ZipEntry e;
    int zip, ok;
    u32 len;
    HANDLE h = rom_open(path, &r, &e, &zip, &len);
    memset(rom, 0, sizeof(*rom));
    if (h == INVALID_HANDLE_VALUE) return 0;
    head = (u8 *)malloc(headSize);
    ok = head && len >= headSize && (zip ? h64_zip_extract(&r, &e, head, headSize) == 0 : handle_read(h, 0, head, headSize) == headSize);
    CloseHandle(h);
    if (!ok || h64_rom_load(rom, head, headSize)) { free(head); return 0; }
    free(head);
    return len;
}

u32 RomReadHeader(const char *path, H64Rom *rom) { return read_header(path, rom); }

int RomFolderName(const char *path, char *out, size_t size)
{
    H64Rom rom;
    if (!read_header(path, &rom)) return 0;
    h64_save_folder_name(&rom, out, (int)size);
    h64_rom_free(&rom);
    return 1;
}

// ---- Hi-res texture packs (see xb_menu.h) ----
static char s_hiresPacks[HIRES_PACKS_MAX][160];   // full path - HiresPackName strips the folder
static int s_hiresPackCount;

void HiresScanPacks(const char *romName)
{
    char dir[160], pattern[176];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    s_hiresPackCount = 0;
    if (!romName || !romName[0]) return;
    _snprintf(dir, sizeof(dir) - 1, "game:\\hires_texture\\%s", romName);
    dir[sizeof(dir) - 1] = 0;
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*.hts", dir);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (s_hiresPackCount >= HIRES_PACKS_MAX) break;
        _snprintf(s_hiresPacks[s_hiresPackCount], sizeof(s_hiresPacks[0]) - 1, "%s\\%s", dir, fd.cFileName);
        s_hiresPacks[s_hiresPackCount][sizeof(s_hiresPacks[0]) - 1] = 0;
        s_hiresPackCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- Skins (see xb_menu.h) ----
static char s_skins[SKINS_MAX][64];   // folder name only - also Config::skinName
static int s_skinCount;

void SkinScan(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    s_skinCount = 0;
    h = FindFirstFileA("game:\\skins\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (s_skinCount >= SKINS_MAX) break;
        strncpy(s_skins[s_skinCount], fd.cFileName, sizeof(s_skins[0]) - 1);
        s_skins[s_skinCount][sizeof(s_skins[0]) - 1] = 0;
        s_skinCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int SkinCount(void) { return s_skinCount; }

const char *SkinName(int i) { return i >= 0 && i < s_skinCount ? s_skins[i] : ""; }

int HiresPackCount(void) { return s_hiresPackCount; }

const char *HiresPackPath(int i) { return i >= 0 && i < s_hiresPackCount ? s_hiresPacks[i] : ""; }

const char *HiresPackName(int i)
{
    const char *p, *slash;
    if (i < 0 || i >= s_hiresPackCount) return "";
    p = s_hiresPacks[i];
    slash = strrchr(p, '\\');
    return slash ? slash + 1 : p;
}

// The header of one ROM, for the details card.
struct RomInfo
{
    int ok;
    char name[24], code[8], size[16], save[128], folder[64], region[32];
};

static void rom_details(const char *path, RomInfo *info)
{
    H64Rom rom;
    u32 len = read_header(path, &rom);
    int pak, type;
    memset(info, 0, sizeof(*info));
    if (!len) return;
    info->ok = 1;
    type = h64_save_type_for_rom(&rom, &pak);
    strncpy(info->name, rom.name, sizeof(info->name) - 1);
    strncpy(info->code, rom.gameCode, sizeof(info->code) - 1);
    if (len >= (1u << 20)) _snprintf(info->size, sizeof(info->size), "%u MB", len >> 20);
    else _snprintf(info->size, sizeof(info->size), "%u KB", len >> 10);
    _snprintf(info->save, sizeof(info->save), "%s%s",
              type == H64_SAVE_NONE ? Tr(T_SAVE_NONE) : type == H64_SAVE_AUTO ? Tr(T_SAVE_UNKNOWN) : h64_save_type_name(type),
              pak ? " + Controller Pak" : "");
    h64_save_folder_name(&rom, info->folder, (int)sizeof(info->folder));
    strncpy(info->region, h64_gameinfo_region_name(rom.gameCode[3]), sizeof(info->region) - 1);
    info->size[sizeof(info->size) - 1] = 0;
    info->save[sizeof(info->save) - 1] = 0;
    HiresScanPacks(rom.name);   // keeps the Graphics settings' pack picker matching this ROM
    h64_rom_free(&rom);
}

// Up to two initials of a game's name ("Super Mario 64 (USA)" -> "SM") for its tile.
static void initials(const char *name, char *out)
{
    int n = 0, start = 1;
    for (; *name && n < 2; name++)
    {
        char c = *name;
        if (c == '(' || c == '[') break;
        if (start && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        start = c == ' ' || c == '-' || c == '_';
    }
    if (!n) out[n++] = '?';
    out[n] = 0;
}

// The settings for all games, from the ROM list.
static void settings_loop(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, int ctlTest)
{
    UiMenu m;
    UiInput in;
    UiInputInit(&in);
    SettingsBuild(&m, c, 0);
    s_browser = 1;
    settings_main(&m, c);
    if (ctlTest >= 300)
    {
        settings_graphics(&m, c);            // controlstest=300+N: the Graphics page, on item N
        m.sel = (ctlTest - 300) % m.count;
    }
    else if (ctlTest >= 200)
        m.sel = (ctlTest - 200) % m.count;   // controlstest=200+N: this page, on item N
    else if (ctlTest >= 0)
    {
        // controlstest=N: the Controls page on item N; 100+N: also waiting
        // for an input, again and again (captures of the blinking control).
        controls_open(&m, c);
        m.sel = (ctlTest % 100) % C_COUNT;
    }
    for (;;)
    {
        int r;
        if (ctlTest >= 100 && ctlTest < 200 && !s_ctl.capturing && s_ctlOrder[m.sel] < H64_N64_COUNT)
        {
            s_ctl.capturing = 1;
            s_ctl.capItem = s_ctlOrder[m.sel];
            s_ctl.capAdd = 0;
            s_ctl.capReleased = 0;
            s_ctl.capStart = GetTickCount();
        }
        r = SettingsInput(&m, c, 0, UiInputPoll(&in));
        if (r == SET_LANGUAGE)
        {
            int lang = LanguageScreen(dev, LangGet(), 0);
            if (lang >= 0) lang_change(c, lang);
            settings_interface(&m, c);   // Language lives on the Interface page now
            m.sel = I_LANGUAGE;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_PREVIEW)
        {
            HiresPreviewScreen(dev, c);
            UiInputInit(&in);
            continue;
        }
        if (r == SET_ART_ASK)
        {
            // Box art downloads turned on only once confirmed (RomBrowser starts them when this page is left).
            if (confirm_ask(dev, -1, T_ART_ASK, T_FOOT_ART_ASK)) c->artDownload = 1;
            settings_values(&m, c);
            UiInputInit(&in);
            continue;
        }
        if (r == SET_BACK) { s_browser = 0; return; }
        if (r == SET_SAVE_ALL)
        {
            H64_INFO("[menu] settings for all games %s", ConfigWriteMenuKeys(c, settingsPath, 1) ? "saved" : "NOT saved");
            s_browser = 0;
            return;
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- Box art in the browser ----
// The browser's copies of what xb_art.cpp sends (textures and text), by ROM
// file; the least recently shown is dropped when the table is full. Room for
// a covers grid's page and the two read ahead around it (3 x 15), so turning
// to a page seen a moment ago shows it at once (400x232 textures: ~24 MB at
// most, freed when a game starts).
#define ART_VIEWS 64
struct ArtView
{
    int used;
    char path[256];
    int hasInfo;
    H64GameInfo info;
    IDirect3DTexture9 *cover, *glow;
    int coverW, coverH;
    float coverAt;      // UiTime() when the cover arrived (its fade-in)
    DWORD lastUse;
};
static ArtView s_views[ART_VIEWS];
static DWORD s_viewClock;

static void view_free(IDirect3DDevice9 *dev, ArtView *v)
{
    if (v->cover) UiImageRelease(dev, v->cover);
    if (v->glow) UiImageRelease(dev, v->glow);
    memset(v, 0, sizeof(*v));
}

static ArtView *view_find(const char *path)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used && !_stricmp(s_views[i].path, path))
        {
            s_views[i].lastUse = ++s_viewClock;
            return &s_views[i];
        }
    return NULL;
}

// Turns the worker's results into textures.
static void views_take(IDirect3DDevice9 *dev)
{
    ArtResult r;
    while (ArtTake(&r))
    {
        ArtView *v = view_find(r.path);
        float coverAt = UiTime();
        if (v)
        {
            // Newer files for a ROM already shown (a download finished): a cover it had does not fade in again.
            if (v->cover && r.cover) coverAt = v->coverAt;
            view_free(dev, v);
        }
        else
        {
            int i, best = 0;
            for (i = 0; i < ART_VIEWS; i++)
            {
                if (!s_views[i].used) { best = i; break; }
                if (s_views[i].lastUse < s_views[best].lastUse) best = i;
            }
            v = &s_views[best];
            if (v->used)
            {
                ArtForget(v->path);   // sent again if the user comes back to it
                view_free(dev, v);
            }
        }
        v->used = 1;
        strncpy(v->path, r.path, sizeof(v->path) - 1);
        v->hasInfo = r.hasInfo;
        v->info = r.info;
        if (r.cover)
        {
            v->cover = UiImageCreate(dev, r.cover, r.coverW, r.coverH);
            v->coverW = r.coverW;
            v->coverH = r.coverH;
        }
        if (r.glow) v->glow = UiImageCreate(dev, r.glow, ART_GLOW_W, ART_GLOW_H);
        v->coverAt = coverAt;
        v->lastUse = ++s_viewClock;
        ArtResultFree(&r);
    }
}

static void views_clear(IDirect3DDevice9 *dev)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used) view_free(dev, &s_views[i]);
    ArtForgetAll();
}

// ---- Browser layout (UI pixels) ----
#define LIST_W 556.0f
#define LIST_Y 128.0f
#define ROW_H 36.0f
#define CARD_X 644.0f
#define CARD_Y 44.0f
#define CARD_W ((float)UI_WIDTH - UI_SAFE_X - CARD_X)
#define CARD_B 620.0f            // bottom of the list and the card
#define STAGE_H 240.0f

static float smooth01(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3.0f - 2.0f * t);
}

// A colour pair picked from a game's name (the placeholder's gradient).
static void name_colors(const char *name, D3DCOLOR *top, D3DCOLOR *bottom)
{
    static const D3DCOLOR tops[6] = { D3DCOLOR_ARGB(255, 240, 92, 66), D3DCOLOR_ARGB(255, 245, 150, 60), D3DCOLOR_ARGB(255, 150, 100, 235),
                                      D3DCOLOR_ARGB(255, 70, 140, 240), D3DCOLOR_ARGB(255, 40, 180, 170), D3DCOLOR_ARGB(255, 110, 190, 80) };
    static const D3DCOLOR bottoms[6] = { D3DCOLOR_ARGB(255, 150, 28, 30), D3DCOLOR_ARGB(255, 180, 70, 20), D3DCOLOR_ARGB(255, 80, 40, 150),
                                         D3DCOLOR_ARGB(255, 30, 70, 160), D3DCOLOR_ARGB(255, 16, 100, 110), D3DCOLOR_ARGB(255, 40, 110, 50) };
    u32 hsh = 2166136261u;
    const char *p;
    for (p = name; *p; p++) hsh = (hsh ^ (u8)*p) * 16777619u;
    *top = tops[hsh % 6];
    *bottom = bottoms[hsh % 6];
}

// The picture area when there is no box art: the name's gradient and its initials.
static void stage_placeholder(float x, float y, float w, float h, const char *name, float a)
{
    D3DCOLOR top, bottom;
    char ini[4];
    if (a <= 0.01f) return;
    name_colors(name, &top, &bottom);
    initials(name, ini);
    UiRoundRect(x, y, w, h, 16, UiAlpha(top, a * 0.85f), UiAlpha(bottom, a * 0.85f));
    UiRoundRect(x + 2, y + 2, w - 4, h * 0.5f, 14, UiAlpha(D3DCOLOR_ARGB(30, 255, 255, 255), a), D3DCOLOR_ARGB(0, 255, 255, 255));
    UiTextShadowed(UI_BOLD, 92, x + w * 0.5f, y + h * 0.5f, UiAlpha(D3DCOLOR_ARGB(235, 255, 255, 255), a), ini, UI_CENTER);
}

// A small rounded label ("Platform", "1-4 players"); returns its width.
static float chip(float x, float cy, const char *text, float a)
{
    float w = UiTextWidth(UI_BOLD, 15, text) + 26, h = 28;
    UiRoundRect(x, cy - h * 0.5f, w, h, h * 0.5f, UiAlpha(D3DCOLOR_ARGB(255, 50, 53, 68), a), UiAlpha(D3DCOLOR_ARGB(255, 42, 45, 58), a));
    UiRoundRing(x, cy - h * 0.5f, w, h, h * 0.5f, 1.0f, UiAlpha(D3DCOLOR_ARGB(34, 255, 255, 255), a));
    UiText(UI_BOLD, 15, x + w * 0.5f, cy, UiAlpha(UI_COL_TEXT2, a), text, UI_CENTER);
    return w;
}

// One label / value pair of the card's last row.
static void detail(float x, float y, float w, const char *label, const char *value, float a)
{
    char fit[96];
    UiText(UI_BOLD, 13, x, y, UiAlpha(UI_COL_TEXT3, a), label, UI_LEFT);
    UiTextFit(UI_REGULAR, 19, w, value, fit, sizeof(fit));
    UiText(UI_REGULAR, 19, x, y + 25, UiAlpha(UI_COL_TEXT, a), fit, UI_LEFT);
}

// The card on the right: box art, title, facts. `selAt`: when the selection
// changed (the card fades and slides in).
static void card_draw(const char *shown, const RomInfo *ri, const ArtView *v, int busy, float selAt)
{
    const float x = CARD_X, y = CARD_Y, w = CARD_W, h = CARD_B - CARD_Y, ix = x + 22, iw = w - 44;
    const float sx = ix, sy = y + 22, sw = iw, sh = STAGE_H;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    float now = UiTime(), a = 0.3f + 0.7f * smooth01((now - selAt) / 0.2f), slide = (1.0f - smooth01((now - selAt) / 0.2f)) * 10.0f;
    float ca = 0, ty, divY = CARD_B - 76;
    char buf[1024];

    UiPanel(x, y, w, h, 22);

    // ---- The picture: the box art over a glow of its own colours, else the placeholder.
    if (v && v->cover)
        ca = smooth01((now - (v->coverAt > selAt ? v->coverAt : selAt)) / 0.25f);
    UiRoundRect(sx, sy, sw, sh, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    {
        // The initials only when no picture is on its way: the worker sends a game
        // with its picture already decoded, so before its answer, wait a moment.
        float pa = v ? 1.0f : smooth01((now - selAt - 0.3f) / 0.2f);
        stage_placeholder(sx, sy, sw, sh, shown, a * pa * (1.0f - ca));
    }
    if (ca > 0)
    {
        float maxW = sw - 56, maxH = sh - 36, cw = (float)v->coverW, chh = (float)v->coverH, k, cx, cy;
        if (v->glow) UiImage(v->glow, sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        // A veil keeps the glow behind the box (and the edges dark).
        UiRoundRect(sx, sy, sw, sh, 16, UiAlpha(D3DCOLOR_ARGB(120, 12, 13, 18), ca * a), UiAlpha(D3DCOLOR_ARGB(185, 12, 13, 18), ca * a));
        k = maxW / cw < maxH / chh ? maxW / cw : maxH / chh;
        if (k > 1.15f) k = 1.15f;
        cw *= k;
        chh *= k;
        cx = sx + (sw - cw) * 0.5f;
        cy = sy + (sh - chh) * 0.5f + (1.0f - ca) * 6.0f;
        UiShadow(cx, cy + 8, cw, chh, 8, 26, UiAlpha(D3DCOLOR_ARGB(170, 0, 0, 0), ca * a));
        UiImage(v->cover, cx, cy, cw, chh, 7, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a));
        UiRoundRing(cx, cy, cw, chh, 7, 1.0f, UiAlpha(D3DCOLOR_ARGB(40, 255, 255, 255), ca * a));
    }
    UiRoundRing(sx, sy, sw, sh, 16, 1.0f, UiAlpha(UI_COL_LINE, a));
    if (busy && ca <= 0)
    {
        // Downloading: a pill with three pulsing dots at the bottom right of the picture.
        float pw = UiTextWidth(UI_REGULAR, 15, Tr(T_DOWNLOADING)) + 64, ph = 30, px = sx + sw - pw - 12, py = sy + sh - ph - 12;
        int d;
        UiRoundRect(px, py, pw, ph, ph * 0.5f, D3DCOLOR_ARGB(200, 16, 17, 23), D3DCOLOR_ARGB(200, 16, 17, 23));
        UiText(UI_REGULAR, 15, px + 14, py + ph * 0.5f, UI_COL_TEXT2, Tr(T_DOWNLOADING), UI_LEFT);
        for (d = 0; d < 3; d++)
        {
            float on = 0.5f + 0.5f * sinf(now * 6.0f - d * 0.9f);
            UiCircle(px + pw - 38 + d * 11, py + ph * 0.5f, 3.0f, UiAlpha(UI_COL_ACCENT_HI, 0.35f + 0.65f * on));
        }
    }

    // ---- Title, then year, developer, publisher.
    ty = sy + sh + 32 + slide;
    {
        int lines = UiTextWrap(UI_BOLD, 26, iw, 2, gi && gi->title[0] ? gi->title : shown, buf, sizeof(buf));
        UiText(UI_BOLD, 26, ix, ty, UiAlpha(UI_COL_TEXT, a), buf, UI_LEFT);
        ty += (lines - 1) * 26 * 1.35f + 32;
    }
    buf[0] = 0;
    if (gi)
    {
        const char *parts[3];
        int k, np = 0;
        parts[np++] = gi->year[0] ? gi->year : NULL;
        parts[np++] = gi->developer[0] ? gi->developer : NULL;
        parts[np++] = gi->publisher[0] && _stricmp(gi->publisher, gi->developer) ? gi->publisher : NULL;
        for (k = 0; k < np; k++)
        {
            if (!parts[k]) continue;
            if (buf[0]) strcat(buf, "  " UI_CH_MIDDOT "  ");
            strncat(buf, parts[k], sizeof(buf) - strlen(buf) - 8);
        }
    }
    if (!buf[0] && ri->ok) strncpy(buf, ri->name, sizeof(buf) - 1);   // the header's own name
    if (buf[0])
    {
        char fit[256];
        UiTextFit(UI_REGULAR, 18, iw, buf, fit, sizeof(fit));
        UiText(UI_REGULAR, 18, ix, ty, UiAlpha(gi ? UI_COL_TEXT2 : UI_COL_TEXT3, a), fit, UI_LEFT);
        ty += 36;
    }

    // ---- Genre, players, region.
    {
        float cxp = ix;
        const char *region = TrRegion(gi && gi->region[0] ? gi->region : ri->region);
        char players[64];
        int any = 0;
        if (gi && gi->genre[0]) { cxp += chip(cxp, ty, gi->genre, a) + 8; any = 1; }
        if (gi && gi->players[0])
        {
            if (!strcmp(gi->players, "1")) strcpy(players, Tr(T_ONE_PLAYER));
            else _snprintf(players, sizeof(players) - 1, Tr(T_N_PLAYERS), gi->players);
            players[sizeof(players) - 1] = 0;
            cxp += chip(cxp, ty, players, a) + 8;
            any = 1;
        }
        if (region[0] && cxp + UiTextWidth(UI_BOLD, 15, region) + 26 <= ix + iw) { cxp += chip(cxp, ty, region, a) + 8; any = 1; }
        if (any) ty += 30;
    }

    // ---- A few lines about the game (Wikipedia), as many as fit above the last row.
    if (gi && gi->description[0])
    {
        float lineH = 17 * 1.35f, room = divY - 20 - (ty + 4);
        int maxLines = (int)(room / lineH);
        if (maxLines > 0)
        {
            int lines = UiTextWrap(UI_REGULAR, 17, iw, maxLines, gi->description, buf, sizeof(buf));
            UiText(UI_REGULAR, 17, ix, ty + 4 + lineH * 0.5f, UiAlpha(UI_COL_TEXT2, a), buf, UI_LEFT);
            UiText(UI_REGULAR, 13, ix, ty + 4 + lines * lineH + 9, UiAlpha(UI_COL_TEXT3, a), Tr(T_WIKI_TEXT), UI_LEFT);
        }
    }

    // ---- What the emulator knows from the file.
    UiRect(ix, divY, iw, 1, UI_COL_LINE);
    if (!ri->ok)
        UiText(UI_REGULAR, 19, ix, divY + 36, UI_COL_ACCENT_HI, Tr(T_NOT_N64), UI_LEFT);
    else
    {
        detail(ix, divY + 22, 90, Tr(T_GAME_CODE), ri->code, 1.0f);
        detail(ix + 100, divY + 22, 70, Tr(T_SIZE), ri->size, 1.0f);
        detail(ix + 180, divY + 22, 130, Tr(T_SAVE), ri->save, 1.0f);
        // The header's raw internal name: what HiresOpen (main.cpp) matches under
        // game:\hires_texture\<name>\ - shown even when the game database found a
        // nicer display title above, so the user can see the folder name to use.
        detail(ix + 320, divY + 22, iw - 320, Tr(T_INTERNAL_NAME), ri->name, 1.0f);
    }
}

static void browser_draw(const std::vector<RomEntry> &roms, int sel, int top, int rows, const RomInfo *info, const ArtView *view,
                         int busy, float selAt, const ArtStatus *st)
{
    static UiAnim selAnim;
    const float listX = UI_SAFE_X, listY = LIST_Y, listW = LIST_W, rowH = ROW_H, pad = 12;
    const float listH = rows * rowH + 2 * pad;
    int n = (int)roms.size(), i;
    char buf[160];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);   // the logo alone (the version is on the About page)
    if (n)
    {
        sprintf(buf, "%d / %d", sel + 1, n);
        UiText(UI_REGULAR, 19, listX + listW, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    }
    UiPanel(listX, listY, listW, listH, 20);
    if (!n)
    {
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 30, listX + listW * 0.5f, cy - 50, UI_COL_TEXT, Tr(T_NO_ROM), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy, UI_COL_TEXT2, Tr(T_COPY_ROMS), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy + 32, UI_COL_TEXT2, Tr(T_INTO_ROMS), UI_CENTER);
    }
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0), textW = listW - 64;
        selection_bar(listX + 10, sy + 1, listW - (n > rows ? 36 : 20), rowH - 2);
        for (i = 0; i < rows && top + i < n; i++)
        {
            float cy = listY + pad + i * rowH + rowH * 0.5f;
            int s = top + i == sel;
            UiTextFit(s ? UI_BOLD : UI_REGULAR, 21, textW, roms[top + i].shown.c_str(), buf, sizeof(buf));
            UiText(s ? UI_BOLD : UI_REGULAR, 21, listX + 30, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
        }
        if (n > rows)
        {
            // Scroll bar: the visible part of the list.
            float tx = listX + listW - 16, ty = listY + pad + 4, th = listH - 2 * pad - 8;
            float hh = th * rows / n, yy;
            if (hh < 28) hh = 28;
            yy = ty + (th - hh) * top / (float)(n - rows);
            UiRoundRect(tx, ty, 4, th, 2, D3DCOLOR_ARGB(20, 255, 255, 255), D3DCOLOR_ARGB(20, 255, 255, 255));
            UiRoundRect(tx, yy, 4, hh, 2, D3DCOLOR_ARGB(150, 255, 255, 255), D3DCOLOR_ARGB(110, 255, 255, 255));
        }
        card_draw(roms[sel].shown.c_str(), info, view, busy, selAt);
    }
    UiScreenFooter(Tr(n ? T_FOOT_BROWSER : T_FOOT_BROWSER_EMPTY));
    if (st->bulk)
    {
        // "Download all" progress at the right end of the footer.
        const float bw = 150, right = UI_WIDTH - UI_SAFE_X, cy = 664;
        float frac = st->total ? (float)st->done / st->total : 0;
        if (st->done) _snprintf(buf, sizeof(buf) - 1, Tr(T_ART_PROGRESS), st->done, st->total);
        else strncpy(buf, Tr(T_ART_CONNECTING), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 17, right - bw - 16, cy, UI_COL_TEXT2, buf, UI_RIGHT);
        UiRoundRect(right - bw, cy - 3, bw, 6, 3, D3DCOLOR_ARGB(30, 255, 255, 255), D3DCOLOR_ARGB(30, 255, 255, 255));
        if (frac > 0) UiRoundRect(right - bw, cy - 3, bw * frac, 6, 3, UI_COL_ACCENT_HI, UI_COL_ACCENT);
    }
}

// ---- Browser layout (covers grid design, Settings > Interface) ----
// 5x5 would just fit the column budget too, but the user asked for 15 visible
// at once: 5 columns x 3 rows. Checked against UiScreenFooter's divider at
// y=636 (xb_ui.cpp) - the last row ends at 615, 21px clear, not flush against
// it like the Controls page mistake earlier today.
// BD_BIG (the user's ask too): 3 x 2 covers 360 wide, ending at 614. Both
// grids share the drawing and the movement below, through a GridLayout.
// BD_CAROUSEL: one row, the selection big in the middle (browser_draw_carousel).
struct GridLayout
{
    int cols, rows;
    float x, y, tileW, imgH;   // imgH: close to ART_COVER_W:H's 400:232 ratio
    float labelSize, colGap, rowGap;
};
static const GridLayout s_gridCovers = { 5, 3, 70, 110, 215, 125, 16, 16, 20 };
static const GridLayout s_gridBig = { 3, 2, 70, 110, 360, 209, 19, 30, 26 };
#define GRID_LABEL_GAP 6.0f
#define GRID_LABEL_H 24.0f
#define GRID_SLIDE 120.0f   // how far a page slides as it changes (fading as it goes)

static const GridLayout *grid_layout(int design) { return design == BD_BIG ? &s_gridBig : &s_gridCovers; }
static int grid_per_page(const GridLayout *g) { return g->cols * g->rows; }
static float grid_col_x(const GridLayout *g, int col) { return g->x + col * (g->tileW + g->colGap); }
static float grid_row_y(const GridLayout *g, int row) { return g->y + row * (g->imgH + GRID_LABEL_GAP + GRID_LABEL_H + g->rowGap); }

// `page`: which group of grid_per_page(g) ROMs is shown (sel is always inside it - the
// caller keeps page in step with sel, same idea as list mode's `top`).
static void browser_draw_covers(const std::vector<RomEntry> &roms, int sel, int page, const GridLayout *g)
{
    const int perPage = grid_per_page(g);
    int n = (int)roms.size(), i;
    char buf[160];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);   // the logo alone (the version is on the About page)
    // n == 0 never reaches here - the caller (RomBrowser) falls back to browser_draw
    // for that (one tested "no ROMs" screen instead of two).
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    if (n > perPage)
    {
        // The slot the old "game:\roms\" text used to sit in - free since that was removed.
        int pages = (n + perPage - 1) / perPage;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PAGE_N), page + 1, pages);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 15, UI_WIDTH - UI_SAFE_X, 88, UI_COL_TEXT3, buf, UI_RIGHT);
    }
    {
        // Smooth movement: the selection's ring glides from tile to tile, and a
        // page change slides the page leaving by GRID_SLIDE and fades it out
        // while the next one comes in from the other side (down: from below).
        static UiAnim pageAnim, ringX, ringY;
        static const GridLayout *lastG;
        int restart = lastG != g, pg, sc = (sel - page * perPage) % g->cols, sr = (sel - page * perPage) / g->cols;
        float pp, a = selection_pulse(), selFade, rx, ry;
        lastG = g;
        pp = anim_step_rate(&pageAnim, (float)page, restart || fabsf(pageAnim.value - page) > 1.0f, 9.0f, 0.002f);
        selFade = 1.0f - fabsf(page - pp);
        rx = anim_step_rate(&ringX, grid_col_x(g, sc), restart, 14.0f, 0.25f);
        ry = anim_step_rate(&ringY, grid_row_y(g, sr) + (page - pp) * GRID_SLIDE, restart, 14.0f, 0.25f);
        if (selFade > 0.01f)
        {
            selection_glow(rx - 4, ry - 4, g->tileW + 8, g->imgH + 8, 14, a * selFade);
            UiRoundRing(rx - 4, ry - 4, g->tileW + 8, g->imgH + 8, 14, 3.0f, SEL_RING(a * selFade));
        }
        for (pg = (int)floorf(pp); pg <= (int)ceilf(pp); pg++)
        {
            float t = pg - pp, fade = 1.0f - fabsf(t), dy = t * GRID_SLIDE;
            int pgBase = pg * perPage;
            if (fade <= 0.01f || pg < 0) continue;
            for (i = 0; i < perPage && pgBase + i < n; i++)
            {
                int idx = pgBase + i, col = i % g->cols, row = i / g->cols, isSel = idx == sel;
                float cx = grid_col_x(g, col), cy = grid_row_y(g, row) + dy;
                const ArtView *v = view_find(roms[idx].file.c_str());
                char fit[96];
                UiRoundRect(cx, cy, g->tileW, g->imgH, 12, UiAlpha(D3DCOLOR_ARGB(255, 22, 23, 31), fade), UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), fade));
                if (v && v->cover) UiImage(v->cover, cx, cy, g->tileW, g->imgH, 12, D3DCOLOR_ARGB((int)(255 * fade), 255, 255, 255));
                else stage_placeholder(cx, cy, g->tileW, g->imgH, roms[idx].shown.c_str(), fade);
                UiTextFit(isSel ? UI_BOLD : UI_REGULAR, g->labelSize, g->tileW, roms[idx].shown.c_str(), fit, sizeof(fit));
                UiText(isSel ? UI_BOLD : UI_REGULAR, g->labelSize, cx + g->tileW * 0.5f, cy + g->imgH + GRID_LABEL_GAP + GRID_LABEL_H * 0.5f,
                       UiAlpha(isSel ? UI_COL_TEXT : UI_COL_TEXT2, fade), fit, UI_CENTER);
            }
        }
    }
    UiScreenFooter(Tr(T_FOOT_BROWSER));
}

// ---- Browser layout: the carousel (cover flow) ----
// One row of covers on a mirror floor: the selection big in the middle with
// its glow, the others smaller, darker and fainter the farther they are; the
// row slides to the selection. Each cover's lower half shows upside down
// under the floor line, fading out.
#define CAR_W 460.0f                       // the middle cover
#define CAR_H (CAR_W * 232.0f / 400.0f)
#define CAR_FLOOR 418.0f                   // the covers stand on this line
#define CAR_GAP 6.0f                       // between a cover and its reflection
#define CAR_SPREAD 4                       // covers shown on each side

static void browser_draw_carousel(const std::vector<RomEntry> &roms, int sel)
{
    static UiAnim pos;
    int n = (int)roms.size(), order[2 * CAR_SPREAD + 3], cnt = 0, i, j;
    float p, a = selection_pulse();
    char buf[160];
    UiBackdrop();
    UiBrand(UI_SAFE_X, 72, 34);
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    // The row glides; only a long jump (round the end of the list) cuts straight there.
    p = anim_step_rate(&pos, (float)sel, !pos.live || fabsf(pos.value - sel) > 3 * CAR_SPREAD, 9.0f, 0.002f);
    for (i = (int)floorf(p) - CAR_SPREAD; i <= (int)ceilf(p) + CAR_SPREAD && cnt < (int)(sizeof(order) / sizeof(order[0])); i++)
        if (i >= 0 && i < n) order[cnt++] = i;
    // The farthest first, so the middle one is drawn on top.
    for (i = 1; i < cnt; i++)
        for (j = i; j > 0 && fabsf(order[j] - p) > fabsf(order[j - 1] - p); j--)
        {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (i = 0; i < cnt; i++)
    {
        int idx = order[i];
        float d = idx - p, ad = fabsf(d), near1 = ad < 1 ? ad : 1;
        float s = ad < 1 ? 1.0f - 0.4f * ad : 0.6f - 0.08f * (ad - 1);
        float off = ad < 1 ? ad * 330.0f : 330.0f + (ad - 1) * 190.0f;
        float alpha = ad < 1 ? 1.0f - 0.25f * ad : 0.75f - 0.22f * (ad - 1);
        float w = CAR_W * s, h = CAR_H * s, x = UI_WIDTH * 0.5f + (d < 0 ? -off : off) - w * 0.5f, y = CAR_FLOOR - h;
        int tint = (int)(255 * (1.0f - 0.45f * near1));
        const ArtView *v = view_find(roms[idx].file.c_str());
        if (alpha <= 0.02f || x > UI_WIDTH || x + w < 0) continue;
        if (ad < 0.5f)
        {
            // The middle cover: the selection's glow and ring, fading as it slides away.
            float ga = a * (1.0f - 2.0f * ad);
            selection_glow(x - 5, y - 5, w + 10, h + 10, 16, ga);
            UiRoundRing(x - 5, y - 5, w + 10, h + 10, 16, 3.0f, SEL_RING(ga));
        }
        UiRoundRect(x, y, w, h, 12, UiAlpha(D3DCOLOR_ARGB(255, 22, 23, 31), alpha), UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), alpha));
        if (v && v->cover)
        {
            UiImage(v->cover, x, y, w, h, 12, D3DCOLOR_ARGB((int)(255 * alpha), tint, tint, tint));
            UiImageMirror(v->cover, x, CAR_FLOOR + CAR_GAP, w, h, 12, 0.5f,
                          D3DCOLOR_ARGB((int)(110 * alpha), tint, tint, tint), D3DCOLOR_ARGB(0, tint, tint, tint));
        }
        else
        {
            stage_placeholder(x, y, w, h, roms[idx].shown.c_str(), alpha * (0.55f + 0.45f * (1.0f - near1)));
            UiRoundRect(x, CAR_FLOOR + CAR_GAP, w, h * 0.5f, 12, D3DCOLOR_ARGB((int)(40 * alpha), 255, 255, 255), D3DCOLOR_ARGB(0, 255, 255, 255));
        }
    }
    {
        char fit[160];
        UiTextFit(UI_BOLD, 24, UI_WIDTH - 2 * UI_SAFE_X, roms[sel].shown.c_str(), fit, sizeof(fit));
        UiTextShadowed(UI_BOLD, 24, UI_WIDTH * 0.5f, CAR_FLOOR + CAR_GAP + CAR_H * 0.5f + 34, UI_COL_TEXT, fit, UI_CENTER);
    }
    UiScreenFooter(Tr(T_FOOT_BROWSER));
}

// ---- Covers grid: the enlarged detail screen (A on a tile) ----
// Bigger, per what the user asked for - not card_draw() reused as a panel
// (that one sits at a fixed spot made for sharing the screen with the list).
// The cover sits at the left, the game's info fills the gap to its right.
// Sized to its content (not a fixed big panel): a measuring pass first -
// UiTextWrap/UiTextFit lay text out without drawing anything - decides how
// tall the two columns (cover, text) really are, then the panel, divider and
// detail row are placed right after them instead of leaving a fixed gap.
static void cover_detail_draw(const RomEntry *rom, const RomInfo *ri, const ArtView *v)
{
    const float x = 140, w = 1000, pad = 36;
    const float ix = x + pad, iw = w - pad * 2;
    const float boxW = 420, boxH = boxW * 232.0f / 400.0f;   // ART_COVER_W:H's ratio
    const float gap = 36, textX = ix + boxW + gap, textW = (ix + iw) - textX;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    char titleBuf[1024], subtRaw[1024], subtFit[256], descBuf[1024];
    const char *region = TrRegion(gi && gi->region[0] ? gi->region : ri->region);
    char players[64];
    int titleLines, descLines = 0, hasSubt = 0, hasChips = 0;
    float titleH, subtH = 0, chipsH = 0, descH = 0, textColH, contentH;
    float y, boxX, boxY, divY, detailY, footY, h;

    // ---- Measure: title (up to 2 lines). ----
    titleLines = UiTextWrap(UI_BOLD, 28, textW, 2, gi && gi->title[0] ? gi->title : rom->shown.c_str(), titleBuf, sizeof(titleBuf));
    titleH = (titleLines - 1) * 28 * 1.35f + 34;

    // ---- Measure: subtitle (year . developer . publisher, or the header's name). ----
    subtRaw[0] = 0;
    if (gi)
    {
        const char *parts[3];
        int k, np = 0;
        parts[np++] = gi->year[0] ? gi->year : NULL;
        parts[np++] = gi->developer[0] ? gi->developer : NULL;
        parts[np++] = gi->publisher[0] && _stricmp(gi->publisher, gi->developer) ? gi->publisher : NULL;
        for (k = 0; k < np; k++)
        {
            if (!parts[k]) continue;
            if (subtRaw[0]) strcat(subtRaw, "  " UI_CH_MIDDOT "  ");
            strncat(subtRaw, parts[k], sizeof(subtRaw) - strlen(subtRaw) - 8);
        }
    }
    if (!subtRaw[0] && ri->ok) strncpy(subtRaw, ri->name, sizeof(subtRaw) - 1);
    if (subtRaw[0]) { UiTextFit(UI_REGULAR, 19, textW, subtRaw, subtFit, sizeof(subtFit)); hasSubt = 1; subtH = 38; }

    // ---- Measure: chips (genre/players/region) - just whether the row exists. ----
    players[0] = 0;
    if (gi && gi->players[0])
    {
        if (!strcmp(gi->players, "1")) strcpy(players, Tr(T_ONE_PLAYER));
        else _snprintf(players, sizeof(players) - 1, Tr(T_N_PLAYERS), gi->players);
        players[sizeof(players) - 1] = 0;
    }
    hasChips = (gi && gi->genre[0]) || players[0] || region[0];
    if (hasChips) chipsH = 44;

    // ---- Measure: a few lines about the game (Wikipedia), capped so a long
    // entry cannot push the panel past the screen. ----
    descBuf[0] = 0;
    if (gi && gi->description[0])
    {
        float lineH = 17 * 1.35f;
        descLines = UiTextWrap(UI_REGULAR, 17, textW, 7, gi->description, descBuf, sizeof(descBuf));
        descH = descLines * lineH + 29;   // the lines, then the "Wikipedia" credit line just below
    }

    // ---- Lay out: the panel wraps the taller of the cover and the text
    // column, then the divider/detail row/footer follow right after (28 to
    // the divider, 22 to the detail row, ~39 its own height, 24 to the
    // footer, half its ~30 height on each side of the margin below it). ----
    textColH = titleH + subtH + chipsH + descH;
    contentH = boxH > textColH ? boxH : textColH;
    h = 2 * pad + contentH + 28 + 22 + 39 + 24 + 15 + 15;
    y = (UI_HEIGHT - h) * 0.5f;
    if (y < 20) y = 20;
    boxX = ix;
    boxY = y + pad;
    divY = boxY + contentH + 28;
    detailY = divY + 22;
    footY = detailY + 39 + 24 + 15;

    UiDim();
    UiPanel(x, y, w, h, 24);
    UiRoundRect(boxX, boxY, boxW, boxH, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    if (v && v->cover) UiImage(v->cover, boxX, boxY, boxW, boxH, 16, D3DCOLOR_ARGB(255, 255, 255, 255));
    else stage_placeholder(boxX, boxY, boxW, boxH, rom->shown.c_str(), 1.0f);
    UiRoundRing(boxX, boxY, boxW, boxH, 16, 1.0f, UI_COL_LINE);

    UiText(UI_BOLD, 28, textX, boxY, UI_COL_TEXT, titleBuf, UI_LEFT);
    if (hasSubt) UiText(UI_REGULAR, 19, textX, boxY + titleH, UI_COL_TEXT2, subtFit, UI_LEFT);
    if (hasChips)
    {
        float cxp = textX, cy = boxY + titleH + subtH;
        if (gi && gi->genre[0]) cxp += chip(cxp, cy, gi->genre, 1.0f) + 8;
        if (players[0]) cxp += chip(cxp, cy, players, 1.0f) + 8;
        if (region[0] && cxp + UiTextWidth(UI_BOLD, 15, region) + 26 <= textX + textW) cxp += chip(cxp, cy, region, 1.0f) + 8;
    }
    if (descLines > 0)
    {
        float lineH = 17 * 1.35f, descY = boxY + titleH + subtH + chipsH;
        UiText(UI_REGULAR, 17, textX, descY + lineH * 0.5f, UI_COL_TEXT2, descBuf, UI_LEFT);
        UiText(UI_REGULAR, 13, textX, descY + descLines * lineH + 9, UI_COL_TEXT3, Tr(T_WIKI_TEXT), UI_LEFT);
    }

    UiRect(ix, divY, iw, 1, UI_COL_LINE);
    if (!ri->ok)
        UiText(UI_REGULAR, 19, ix, detailY + 17, UI_COL_ACCENT_HI, Tr(T_NOT_N64), UI_LEFT);
    else
    {
        detail(ix, detailY, 90, Tr(T_GAME_CODE), ri->code, 1.0f);
        detail(ix + 100, detailY, 70, Tr(T_SIZE), ri->size, 1.0f);
        detail(ix + 180, detailY, 140, Tr(T_SAVE), ri->save, 1.0f);
        detail(ix + 330, detailY, iw - 330, Tr(T_INTERNAL_NAME), ri->name, 1.0f);
    }
    UiFooter(ix, footY, Tr(T_FOOT_COVER_DETAIL));
}

// A plays the game - the caller (RomBrowser) does the actual launch (same
// side effects as the list view's A press, kept in one place), B goes back
// to the grid.
static int cover_detail_screen(IDirect3DDevice9 *dev, const RomEntry *rom)
{
    UiInput in;
    RomInfo ri;
    UiInputInit(&in);
    rom_details(rom->file.c_str(), &ri);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        cover_detail_draw(rom, &ri, view_find(rom->file.c_str()));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

#define MSG_MS 6000   // how long a message (new version, box art) stays

// Box art may be downloaded here (the settings allow it; not in Xenia, whose
// network is not the console's, unless artdownload=2).
static int art_net_possible(const Config *c) { return (!c->xenia && h64_fenv_reliable()) || c->artDownload == 2; }

int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    std::vector<RomEntry> roms;
    std::vector<const char *> paths;
    UiInput in;
    int sel = 0, top = 0, page = 0, detailsFor = -1, i, lastSel = -1, wasBulk = 0;
    RomInfo details;
    const int rows = 13;
    char newVersion[32], msg[256], drive[16];
    DWORD msgAt = 0, demoAt = GetTickCount();
    float selAt = 0;
    msg[0] = 0;
    scan_roms(&roms);
    for (i = 0; i < (int)roms.size(); i++)
    {
        paths.push_back(roms[i].file.c_str());
        if (!_stricmp(roms[i].file.c_str(), c->lastRom)) sel = i;
    }
    if (c->browserSel[0])
    {
        // browsersel=FILE (console checks): start on that ROM, whatever the
        // last game played was.
        for (i = 0; i < (int)roms.size(); i++)
        {
            const char *f = roms[i].file.c_str(), *base = strrchr(f, '\\');
            if (!_stricmp(base ? base + 1 : f, c->browserSel)) sel = i;
        }
    }
    if (c->autoStart > 0 && !roms.empty())
    {
        // Remote checks: the last selection, then the next ROMs in the list.
        sel = (sel + c->autoIndex) % (int)roms.size();
        strncpy(path, roms[sel].file.c_str(), pathSize - 1);
        path[pathSize - 1] = 0;
        H64_INFO("[menu] autostart %d: %s", c->autoIndex + 1, path);
        return 1;
    }
    if (c->browserDesign < 0 || c->browserDesign >= BD_COUNT) c->browserDesign = BD_LIST;
    page = roms.empty() ? 0 : sel / grid_per_page(grid_layout(c->browserDesign));   // a grid starts on sel's page, not always page 0
    // Box art: in the folder of the settings' drive ("game:\config\settings.ini" -> game).
    SettingsSetLangFile(settingsPath);
    for (i = 0; settingsPath[i] && settingsPath[i] != ':' && i < (int)sizeof(drive) - 1; i++) drive[i] = settingsPath[i];
    drive[i] = 0;
    if (!settingsPath[i] || !i) strcpy(drive, "game");
    ArtStart(drive);
    ArtSetNetwork(c->artDownload && art_net_possible(c));
    ArtPause(0);
    UiSetSkinWant(c->skinName);
    xb_audio_music_play(c->skinName);   // "" just stops whatever was playing

    UiInputInit(&in);
    memset(&details, 0, sizeof(details));
    if (c->controlsTest >= 0) settings_loop(dev, c, settingsPath, c->controlsTest);
    // The box art of every game, in the background: what is missing (artall=1,
    // checks: also retrying what was not found before).
    if (!roms.empty() && art_net_possible(c) && (c->artAll || c->artDownload)) ArtBulkStart(&paths[0], (int)paths.size(), c->artAll);
    // A newer release on GitHub: a message for a few seconds (the check runs
    // in the background and may answer later, or never without a network).
    // updatecheck=2 (testing): as if this were version 0.0.0, so any release is newer.
    if (c->updateCheck && !c->xenia && h64_fenv_reliable()) UpdateCheckStart(c->updateCheck == 2 ? "0.0.0" : H64_VERSION_STRING);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        ArtStatus st;
        int n = (int)roms.size();
        if (UpdateCheckTake(newVersion, sizeof(newVersion)))
        {
            _snprintf(msg, sizeof(msg) - 1, Tr(T_UPDATE_AVAILABLE), newVersion);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
            H64_INFO("[update] %s", msg);
        }
        if (ArtTakeNote(msg, sizeof(msg))) msgAt = GetTickCount();
        if (BatteryCheck(c, msg, sizeof(msg))) msgAt = GetTickCount();
        if ((down & XINPUT_GAMEPAD_BACK) && confirm_quit(dev))
        {
            views_clear(dev);
            ArtPause(1);
            xb_audio_music_stop();
            return 0;
        }
        if (n)
        {
            if (c->browserDemo > 0 && GetTickCount() - demoAt >= (DWORD)c->browserDemo)
            {
                // browserdemo=N: walk the list by itself (captures).
                sel = (sel + 1) % n;
                demoAt = GetTickCount();
            }
            if (c->browserDesign == BD_LIST)
            {
                if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + n - 1) % n;
                if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % n;
                if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel > rows ? sel - rows : 0;
                if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + rows < n ? sel + rows : n - 1;
                if (down & XINPUT_GAMEPAD_A)
                {
                    strncpy(path, roms[sel].file.c_str(), pathSize - 1);
                    path[pathSize - 1] = 0;
                    strncpy(c->lastRom, path, sizeof(c->lastRom) - 1);
                    ConfigWriteMenuKeys(c, settingsPath, 1);
                    // The game needs the memory and the GPU: the pictures go, the worker waits.
                    views_clear(dev);
                    ArtPause(1);
                    xb_audio_music_stop();
                    return 1;
                }
                if (sel < top) top = sel;
                if (sel >= top + rows) top = sel - rows + 1;
                if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), &details); detailsFor = sel; }
                if (sel != lastSel)
                {
                    // The selection first, then its neighbours (read ahead from the files already there).
                    const char *want[5];
                    int k, cnt = 0;
                    static const int order[5] = { 0, 1, -1, 2, -2 };
                    for (k = 0; k < 5 && k < n; k++) want[cnt++] = paths[(sel + order[k] + n * 2) % n];
                    ArtWant(want, cnt);
                    selAt = UiTime();
                    lastSel = sel;
                }
            }
            else
            {
                // Covers grids: 2D movement within the page, LB/RB changes page.
                // Carousel: left/right step (round the ends), LB/RB jump 5. A
                // opens the enlarged screen; only a Play there (not just opening it)
                // launches, same side effects as the list's A press, kept in one place.
                const GridLayout *g = grid_layout(c->browserDesign);
                const int perPage = grid_per_page(g), carousel = c->browserDesign == BD_CAROUSEL;
                if (carousel)
                {
                    if (down & XINPUT_GAMEPAD_DPAD_LEFT) sel = (sel + n - 1) % n;
                    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) sel = (sel + 1) % n;
                    if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) sel = sel > 5 ? sel - 5 : 0;
                    if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) sel = sel + 5 < n ? sel + 5 : n - 1;
                }
                else
                {
                    int col = (sel - page * perPage) % g->cols;
                    int pages = (n + perPage - 1) / perPage;
                    if ((down & XINPUT_GAMEPAD_DPAD_LEFT) && col > 0) sel--;
                    if ((down & XINPUT_GAMEPAD_DPAD_RIGHT) && col < g->cols - 1 && sel + 1 < n) sel++;
                    if ((down & XINPUT_GAMEPAD_DPAD_UP) && sel >= g->cols) sel -= g->cols;   // from the top row: the previous page's bottom row
                    if ((down & XINPUT_GAMEPAD_DPAD_DOWN) && sel + g->cols < n) sel += g->cols;
                    if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) { page = page > 0 ? page - 1 : pages - 1; sel = page * perPage; }
                    if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) { page = page + 1 < pages ? page + 1 : 0; sel = page * perPage; }
                }
                if (sel >= n) sel = n - 1;
                if (sel < 0) sel = 0;
                page = sel / perPage;
                if (down & XINPUT_GAMEPAD_A)
                {
                    if (cover_detail_screen(dev, &roms[sel]))
                    {
                        strncpy(path, roms[sel].file.c_str(), pathSize - 1);
                        path[pathSize - 1] = 0;
                        strncpy(c->lastRom, path, sizeof(c->lastRom) - 1);
                        ConfigWriteMenuKeys(c, settingsPath, 1);
                        views_clear(dev);
                        ArtPause(1);
                        xb_audio_music_stop();
                        return 1;
                    }
                    UiInputInit(&in);
                }
                if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), &details); detailsFor = sel; }
                if (sel != lastSel)
                {
                    // A grid: the whole visible page at once (it shows them all
                    // together, unlike the list's one-row-at-a-time neighbours).
                    // The carousel: the selection, then outwards on both sides.
                    std::vector<const char *> want;
                    int k, base = page * perPage;
                    if (carousel)
                    {
                        want.push_back(paths[sel]);
                        for (k = 1; k <= CAR_SPREAD; k++)
                        {
                            if (sel + k < n) want.push_back(paths[sel + k]);
                            if (sel - k >= 0) want.push_back(paths[sel - k]);
                        }
                    }
                    else
                    {
                        // The page shown first, then the next and the previous ones
                        // read ahead (round the ends), ready before they are turned to.
                        int pages = (n + perPage - 1) / perPage, side, p;
                        for (k = 0; k < perPage && base + k < n; k++) want.push_back(paths[base + k]);
                        for (side = 0; side < 2 && pages > 1; side++)
                        {
                            p = (page + (side ? pages - 1 : 1)) % pages;
                            if (side && p == (page + 1) % pages) break;   // two pages: the next is the previous
                            for (k = 0; k < perPage && p * perPage + k < n; k++) want.push_back(paths[p * perPage + k]);
                        }
                    }
                    if (!want.empty()) ArtWant(&want[0], (int)want.size());
                    selAt = UiTime();
                    lastSel = sel;
                }
            }
        }
        if (down & XINPUT_GAMEPAD_Y)
        {
            int artWas = c->artDownload;
            settings_loop(dev, c, settingsPath, -1);
            detailsFor = -1;   // the card's texts again (the language may have changed)
            ArtSetNetwork(c->artDownload && art_net_possible(c));
            if (!artWas && c->artDownload && n && art_net_possible(c)) ArtBulkStart(&paths[0], n, 0);   // downloads just turned on
            UiInputInit(&in);
        }
        if ((down & XINPUT_GAMEPAD_X) && !n)
        {
            AboutScreen(dev);   // no ROMs: no view to change, X keeps the About page (FOOT_BROWSER_EMPTY)
            UiInputInit(&in);
        }
        else if (down & XINPUT_GAMEPAD_X)
        {
            // "Interface view": the next layout (list, covers, big covers, carousel; Interfaz > Diseño), kept at once.
            c->browserDesign = (c->browserDesign + 1) % BD_COUNT;
            ConfigWriteMenuKeys(c, settingsPath, 1);
            page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
            if (sel < top || sel >= top + rows) top = sel >= rows ? sel - rows + 1 : 0;
            lastSel = -1;   // the new view asks for its own box art
        }
        ArtGetStatus(&st);
        if (wasBulk && !st.bulk)
            for (i = 0; i < n; i++) ArtForget(paths[i]);   // the downloads are over: the covers shown are read again
        wasBulk = st.bulk;
        views_take(dev);

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        if (c->browserDesign == BD_LIST || !n)
            browser_draw(roms, sel, top, rows, &details, n ? view_find(roms[sel].file.c_str()) : NULL,
                         n && !_stricmp(st.busyPath, roms[sel].file.c_str()), selAt, &st);
        else if (c->browserDesign == BD_CAROUSEL)
            browser_draw_carousel(roms, sel);
        else
            browser_draw_covers(roms, sel, page, grid_layout(c->browserDesign));
        if (msg[0])
        {
            DWORD age = GetTickCount() - msgAt;
            if (age < MSG_MS) UiToast(msg, (float)age, (float)MSG_MS);
            else msg[0] = 0;
        }
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}
