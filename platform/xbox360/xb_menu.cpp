// Harissa64 V2 - front-end menus (see xb_menu.h).
#include "xb_menu.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
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
#include "xb_lang.h"
#include "xb_flags.h"
#include "xb_pad.h"
#include "xb_profile.h"
#include "xb_wizref.h"
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

// The LAIKA 64 logo's size everywhere (UiBrand's `size`: 1.9x as high).
#define BRAND_SIZE 34.0f

// The selection's light blue ring and glow fade in and out together, everywhere
// (list rows, menus, the covers grid, the language cards); never quite gone,
// so the selection stays easy to find.
static float selection_pulse(void) { return 0.25f + 0.75f * (0.5f + 0.5f * sinf(UiTime() * 4.0f)); }
// Its colour (Settings > Interface > Selection colour, Config::selColor): a
// bright tone for the ring and the arrows, a deeper one for the glow and fill.
#define SEL_COLORS 9
static const unsigned char s_selColors[SEL_COLORS][2][3] = {
    { { 120, 205, 255 }, { 90, 170, 240 } },    // light blue
    { { 255, 120, 120 }, { 235, 70, 80 } },     // red
    { { 255, 175, 95 }, { 240, 125, 45 } },     // orange
    { { 255, 225, 110 }, { 225, 185, 40 } },    // yellow
    { { 135, 240, 140 }, { 60, 200, 95 } },     // green
    { { 110, 240, 220 }, { 40, 200, 180 } },    // teal
    { { 195, 155, 255 }, { 145, 95, 240 } },    // purple
    { { 255, 145, 210 }, { 235, 85, 170 } },    // pink
    { { 245, 245, 250 }, { 185, 190, 205 } },   // white
};
static const int s_selLabels[SEL_COLORS] = { T_COL_BLUE, T_COL_RED, T_COL_ORANGE, T_COL_YELLOW, T_COL_GREEN, T_COL_TEAL, T_COL_PURPLE,
                                             T_COL_PINK, T_COL_WHITE };
static int s_sel;   // the colour in use (ui_from_config)
#define SEL_HI(a) D3DCOLOR_ARGB((int)(a), s_selColors[s_sel][0][0], s_selColors[s_sel][0][1], s_selColors[s_sel][0][2])
#define SEL_LO(a) D3DCOLOR_ARGB((int)(a), s_selColors[s_sel][1][0], s_selColors[s_sel][1][1], s_selColors[s_sel][1][2])
#define SEL_RING(a) SEL_HI(255 * (a))
#define SEL_GLOW(a) SEL_LO(150 * (a))

// The pulsing outer glow around a selection's ring (the same on every border):
// a wide soft halo and a brighter one close to the edge.
static void selection_glow(float x, float y, float w, float h, float r, float a)
{
    UiShadow(x - 2, y - 2, w + 4, h + 4, r + 2, 34, SEL_GLOW(a));
    UiShadow(x, y, w, h, r, 14, SEL_GLOW(a));
}

// A button press in a menu (UiMenuPress): the selection sinks in and lights up
// for a moment, then springs back.
#define PRESS_S 0.20f
static float s_pressAt = -1.0f;

void UiMenuPress(void) { s_pressAt = UiTime(); }

// 0..1..0 over PRESS_S after a press (1 at its deepest), else 0.
static float press_amount(void)
{
    float t = s_pressAt < 0 ? 1.0f : (UiTime() - s_pressAt) / PRESS_S;
    if (t < 0 || t >= 1) return 0;
    return t < 0.3f ? t / 0.3f : 1.0f - (t - 0.3f) / 0.7f;
}

// The selection highlight of a list row: a blue-tinted pill and its pulsing
// ring (`right` kept for the callers' right-to-left menus).
static void selection_bar_side(float x, float y, float w, float h, int right)
{
    float a = selection_pulse(), p = press_amount(), in = p * 3.0f;
    (void)right;
    x += in; y += in * 0.5f; w -= in * 2; h -= in;
    selection_glow(x, y, w, h, 12, a);
    UiRoundRect(x, y, w, h, 12, SEL_LO(60 + (int)(70 * p)), SEL_LO(38 + (int)(70 * p)));
    UiRoundRing(x, y, w, h, 12, 2.0f, SEL_RING(a + (1.0f - a) * p));
}

// The menus' edges (Settings > Interface > Borders and panels): their colour
// (the selection's unless one is chosen), strength and glow (ui_from_config).
static int s_edgeRgb[3] = { 120, 205, 255 }, s_edgeOp = 100, s_edgeGlow;   // s_edgeGlow: Config::borderGlow (see s_glowLabels)
#define GLOW_COUNT 6   // Config::borderGlow's values (s_glowLabels)
#define EDGE_COL(a) D3DCOLOR_ARGB((int)(a), s_edgeRgb[0], s_edgeRgb[1], s_edgeRgb[2])

// The glow around a panel's edge (menus, dialogs): soft rings outside it,
// fading outwards, and a fine ring on the edge - nothing inside, so the
// panel's own colour stays as it is. Pulsing, steady or none, as chosen.
// A point at distance s along a w x h rectangle's edge (clockwise from the top left).
static void edge_point(float x, float y, float w, float h, float s, float *px, float *py)
{
    float L = 2 * (w + h);
    s = fmodf(s, L);
    if (s < 0) s += L;
    if (s < w) { *px = x + s; *py = y; }
    else if (s < w + h) { *px = x + w; *py = y + s - w; }
    else if (s < 2 * w + h) { *px = x + w - (s - w - h); *py = y + h; }
    else { *px = x; *py = y + h - (s - 2 * w - h); }
}

static void edge_glow(float x, float y, float w, float h, float r)
{
    float now = UiTime(), a;
    int i, rgb[3] = { s_edgeRgb[0], s_edgeRgb[1], s_edgeRgb[2] };
    if (s_edgeGlow == 2 || s_edgeOp <= 0) return;
    switch (s_edgeGlow)
    {
    case 0: a = 0.35f + 0.65f * selection_pulse(); break;              // pulsing
    case 4: a = 0.5f - 0.5f * cosf(now * 6.2832f / 3.2f); break;       // fade: all the way out and back, slowly
    case 3: a = 0.30f; break;                                          // snake: a faint edge, the snake below
    default: a = 0.75f; break;                                         // steady, rainbow
    }
    if (s_edgeGlow == 5)
    {
        // Rainbow: the hue goes round every 6 s.
        float hh = fmodf(now / 6.0f, 1.0f) * 6.0f, f = hh - floorf(hh);
        int k = (int)hh, q = (int)(255 * (1 - f)), t = (int)(255 * f);
        int tab[6][3] = { { 255, t, 0 }, { q, 255, 0 }, { 0, 255, t }, { 0, q, 255 }, { t, 0, 255 }, { 255, 0, q } };
        rgb[0] = tab[k % 6][0]; rgb[1] = tab[k % 6][1]; rgb[2] = tab[k % 6][2];
    }
    a *= s_edgeOp / 100.0f;
#define GLOW_COL(al) D3DCOLOR_ARGB((int)(al), rgb[0], rgb[1], rgb[2])
    r = UiPanelRadius(r);
    for (i = 1; i <= 12; i++)
    {
        float f = 1.0f - i / 13.0f;
        UiRoundRing(x - i, y - i, w + i * 2, h + i * 2, r + i, 1.5f, GLOW_COL(70 * a * f * f));
    }
    UiRoundRing(x, y, w, h, r, 1.5f, GLOW_COL(160 * a));
    if (s_edgeGlow == 3)
    {
        // Snake: a bright head running round the edge every 3.5 s, its tail fading behind it.
        const int n = 28;
        float L = 2 * (w + h), head = fmodf(now / 3.5f, 1.0f) * L, px, py;
        for (i = n - 1; i >= 0; i--)
        {
            float k = 1.0f - (float)i / n;
            edge_point(x, y, w, h, head - i * 9.0f, &px, &py);
            UiCircle(px, py, 2.0f + 5.0f * k, GLOW_COL(40 + 200 * k * k * s_edgeOp / 100.0f));
        }
    }
#undef GLOW_COL
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

static void icon_tri(float cx, float cy, float s, int dir, D3DCOLOR c);   // the menu symbols' triangle, below

// A value shown "< 250 ms >" (DPAD changes it): drawn as the text between two
// small solid arrows, light blue on the selected row (grey otherwise), that
// nudge outwards for a moment when the value changes (a press: UiMenuPress).
// Returns the left end.
#define VALUE_ARROW 9.0f    // an arrow's width
#define VALUE_GAP 12.0f     // between an arrow and the text
static float press_amount(void);
#define SWITCH_W 46.0f
#define SWITCH_H 24.0f
// An on/off row's value (toggle()): "\x01" on, "\x02" off, maybe inside "< >".
static int switch_value(const char *value)
{
    if (!strcmp(value, "\x01") || !strcmp(value, "< \x01 >")) return 1;
    if (!strcmp(value, "\x02") || !strcmp(value, "< \x02 >")) return 0;
    return -1;
}

// A switch: a rounded track with a round knob, on the right and in the selection's
// colour when on, on the left and white only when off; on a press (the selected
// row) the knob slides over from the other side.
static void draw_switch(float right, float cy, int on, int selected)
{
    const float x = right - SWITCH_W, y = cy - SWITCH_H * 0.5f, r = SWITCH_H * 0.5f;
    float t = 1.0f, kx;
    if (selected && s_pressAt >= 0)
    {
        t = (UiTime() - s_pressAt) / 0.18f;
        if (t > 1.0f) t = 1.0f;
        t = t * t * (3.0f - 2.0f * t);
    }
    kx = on ? x + r + (SWITCH_W - 2 * r) * t : x + SWITCH_W - r - (SWITCH_W - 2 * r) * t;
    if (on) UiRoundRect(x, y, SWITCH_W, SWITCH_H, r, SEL_HI(255), SEL_LO(255));
    else UiRoundRect(x, y, SWITCH_W, SWITCH_H, r, D3DCOLOR_ARGB(40, 255, 255, 255), D3DCOLOR_ARGB(25, 255, 255, 255));
    UiRoundRing(x, y, SWITCH_W, SWITCH_H, r, 1.5f, on ? SEL_HI(255) : D3DCOLOR_ARGB(selected ? 230 : 150, 255, 255, 255));
    UiCircle(kx, cy, r - 4, D3DCOLOR_ARGB(255, 255, 255, 255));
}

// A bar row's value (bar_value): "\x03" and a level 0..100 - a short bar and the number.
#define VALUE_BAR_W 150.0f
static float draw_value(float right, float cy, const char *value, int selected)
{
    size_t n = strlen(value);
    D3DCOLOR text = selected ? UI_COL_TEXT : UI_COL_TEXT2;
    int sw = switch_value(value);
    if (sw >= 0)
    {
        draw_switch(right, cy, sw, selected);
        return right - SWITCH_W;
    }
    if (value[0] == '\x03')
    {
        int lv = atoi(value + 1);
        char num[16];
        float bx, tw;
        sprintf(num, "%d%%", lv);
        tw = UiText(selected ? UI_BOLD : UI_REGULAR, 21, right, cy, text, num, UI_RIGHT);
        bx = right - tw - 14 - VALUE_BAR_W;
        UiRoundRect(bx, cy - 4, VALUE_BAR_W, 8, 4, D3DCOLOR_ARGB(50, 255, 255, 255), D3DCOLOR_ARGB(35, 255, 255, 255));
        UiRoundRect(bx, cy - 4, VALUE_BAR_W * lv / 100.0f, 8, 4, SEL_HI(255), SEL_LO(255));
        UiCircle(bx + VALUE_BAR_W * lv / 100.0f, cy, selected ? 8.0f : 6.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
        return bx;
    }
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        float x = right, nudge = selected ? press_amount() * 3.0f : 0.0f;
        D3DCOLOR arrow = selected ? SEL_HI(255) : UI_COL_TEXT3;
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        icon_tri(x - VALUE_ARROW * 0.5f + nudge, cy, VALUE_ARROW * 1.3f, 1, arrow);
        x -= VALUE_ARROW + VALUE_GAP;
        x -= UiText(selected ? UI_BOLD : UI_REGULAR, 21, x, cy, text, inner, UI_RIGHT);
        x -= VALUE_GAP;
        icon_tri(x - VALUE_ARROW * 0.5f - nudge, cy, VALUE_ARROW * 1.3f, 3, arrow);
        return x - VALUE_ARROW;
    }
    return right - UiText(UI_REGULAR, 21, right, cy, text, value, UI_RIGHT);
}

// The width draw_value takes.
static float value_width(const char *value, int selected)
{
    size_t n = strlen(value);
    if (switch_value(value) >= 0) return SWITCH_W;
    if (value[0] == '\x03') return VALUE_BAR_W + 14 + UiTextWidth(UI_BOLD, 21, "100%");
    if (n >= 4 && value[0] == '<' && value[1] == ' ' && value[n - 1] == '>' && value[n - 2] == ' ')
    {
        char inner[64];
        size_t k = n - 4 < sizeof(inner) - 1 ? n - 4 : sizeof(inner) - 1;
        memcpy(inner, value + 2, k);
        inner[k] = 0;
        return 2 * (VALUE_ARROW + VALUE_GAP) + UiTextWidth(selected ? UI_BOLD : UI_REGULAR, 21, inner);
    }
    return UiTextWidth(UI_REGULAR, 21, value);
}

static void controls_draw(const UiMenu *m);
static void calib_draw(const UiMenu *m);
static void volume_draw(const UiMenu *m);
static void fxpage_draw(const UiMenu *m);
static void wallpage_draw(const UiMenu *m);
static void borderpage_draw(const UiMenu *m);
static void perfpage_draw(const UiMenu *m);
static void hkpage_draw(const UiMenu *m);

static int s_kodi;      // Interface > Settings style: big icons (kodi_draw)
static int s_kodiSim;   // a dropdown's dry run is going on: handlers leave out their side effects (kodi_dd_open)
static int s_ddApply;   // a value picked in a list is being applied (right pressed for real, dd_input)
#define KODI_COLS 4
// The settings' main page shown as tiles (big icons style).
static int kodi_grid(const UiMenu *m)
{
    return s_kodi && m->page == UI_PAGE_LIST && !strcmp(m->title, Tr(T_SETTINGS)) && !strcmp(m->footer, Tr(T_FOOT_SELECT));
}
static int s_browser;   // opened from the ROM list: the box art rows are shown (also: UiMenuDraw skips the dim behind it in-game, over the frozen frame)

// A small solid triangle pointing `dir` (0 up, 1 right, 2 down, 3 left), `s`
// across, its centre at (cx, cy): slices, the primitives having no triangle.
static void icon_tri(float cx, float cy, float s, int dir, D3DCOLOR c)
{
    const int n = 7;
    int i;
    for (i = 0; i < n; i++)
    {
        float t = (i + 0.5f) / n, len = s * t, step = s / n, along = -s * 0.5f + i * step;
        if (dir == 0) UiRect(cx - len * 0.5f, cy + along, len, step + 0.3f, c);
        else if (dir == 2) UiRect(cx - len * 0.5f, cy - along - step, len, step + 0.3f, c);
        else if (dir == 3) UiRect(cx + along, cy - len * 0.5f, step + 0.3f, len, c);
        else UiRect(cx - along - step, cy - len * 0.5f, step + 0.3f, len, c);
    }
}

void UiMenuIcons(UiMenu *m, const unsigned char *icons, int n)
{
    int i;
    for (i = 0; i < n && i < m->count; i++) m->icon[i] = icons[i];
}

// A row's small line symbol (UI_ICON_*), about 22 pixels across, centred on (cx, cy).
static void menu_icon(int icon, float cx, float cy, D3DCOLOR c)
{
    const D3DCOLOR soft = UiAlpha(c, 0.45f);
    int k;
    switch (icon)
    {
    case UI_ICON_PLAY:        // resume: a ring around a triangle
        UiRoundRing(cx - 11, cy - 11, 22, 22, 11, 2.0f, c);
        icon_tri(cx + 1, cy, 9, 1, c);
        break;
    case UI_ICON_STATES:      // saved states: two cards, one behind the other
        UiRoundRing(cx - 6, cy - 10, 16, 13, 2.5f, 2.0f, soft);
        UiRoundRect(cx - 10, cy - 4, 16, 13, 2.5f, c, c);
        break;
    case UI_ICON_GEAR:        // settings: a ring with eight teeth
        for (k = 0; k < 8; k++)
        {
            float a = k * 0.785398f, tx = cx + cosf(a) * 8.5f, ty = cy + sinf(a) * 8.5f;
            UiRoundRect(tx - 2.2f, ty - 2.2f, 4.4f, 4.4f, 1.0f, c, c);
        }
        UiCircle(cx, cy, 7.5f, c);
        UiCircle(cx, cy, 3.0f, D3DCOLOR_ARGB(255, 27, 29, 38));
        break;
    case UI_ICON_RESET:       // reset: a ring and its arrow head
        UiRoundRing(cx - 9, cy - 9, 18, 18, 9, 2.0f, c);
        icon_tri(cx + 6, cy - 9, 7, 1, c);
        break;
    case UI_ICON_LIST:        // the ROM list: three rows
        for (k = -1; k <= 1; k++)
        {
            UiCircle(cx - 8, cy + k * 6, 1.8f, c);
            UiRoundRect(cx - 4, cy + k * 6 - 1, 14, 2, 1, c, c);
        }
        break;
    case UI_ICON_POWER:       // the dashboard: a power button
        UiRoundRing(cx - 9, cy - 8, 18, 18, 9, 2.0f, c);
        UiRoundRect(cx - 1.5f, cy - 12, 3, 10, 1.5f, c, c);
        break;
    case UI_ICON_SLOT:        // a slot: a grid of dots
        for (k = 0; k < 9; k++) UiCircle(cx + (k % 3 - 1) * 7.0f, cy + (k / 3 - 1) * 7.0f, 2.0f, k == 4 ? c : soft);
        break;
    case UI_ICON_LOAD:        // load / download: an arrow into a tray
        UiRect(cx - 10, cy + 7, 20, 2, c);
        UiRect(cx - 10, cy + 2, 2, 7, c);
        UiRect(cx + 8, cy + 2, 2, 7, c);
        UiRect(cx - 1, cy - 11, 2, 9, c);
        icon_tri(cx, cy + 1, 9, 2, c);
        break;
    case UI_ICON_BATTERY:     // battery: its case, its tip and two bars
        UiRoundRing(cx - 11, cy - 6, 19, 12, 2.5f, 2.0f, c);
        UiRect(cx + 8, cy - 2.5f, 3, 5, c);
        UiRect(cx - 8, cy - 3, 4, 6, c);
        UiRect(cx - 3, cy - 3, 4, 6, c);
        break;
    case UI_ICON_SLIDERS:     // preset: three sliders
        for (k = -1; k <= 1; k++)
        {
            UiRect(cx - 10, cy + k * 7 - 0.75f, 20, 1.5f, soft);
            UiCircle(cx + (k == 0 ? 4.0f : k < 0 ? -4.0f : 1.0f), cy + k * 7, 2.8f, c);
        }
        break;
    case UI_ICON_TEXTURE:     // texture filter: a checkerboard
        UiRoundRect(cx - 9, cy - 9, 9, 9, 1.5f, c, c);
        UiRoundRect(cx, cy, 9, 9, 1.5f, c, c);
        UiRoundRing(cx, cy - 9, 9, 9, 1.5f, 1.5f, soft);
        UiRoundRing(cx - 9, cy, 9, 9, 1.5f, 1.5f, soft);
        break;
    case UI_ICON_HD:          // HD textures
        UiRoundRing(cx - 12, cy - 8, 24, 16, 3, 2.0f, c);
        UiText(UI_BOLD, 11, cx, cy, c, "HD", UI_CENTER);
        break;
    case UI_ICON_SMOOTH:      // edge smoothing: a soft disc
        UiCircle(cx, cy, 10, UiAlpha(c, 0.25f));
        UiCircle(cx, cy, 7, UiAlpha(c, 0.5f));
        UiCircle(cx, cy, 4, c);
        break;
    case UI_ICON_SHARP:       // sharpening: a sharp peak
        icon_tri(cx, cy - 1, 18, 0, c);
        UiRect(cx - 10, cy + 8, 20, 2, c);
        break;
    case UI_ICON_BLUR:        // blur: rings fading out
        UiCircle(cx, cy, 3, c);
        UiRoundRing(cx - 7, cy - 7, 14, 14, 7, 1.5f, UiAlpha(c, 0.6f));
        UiRoundRing(cx - 11, cy - 11, 22, 22, 11, 1.5f, UiAlpha(c, 0.3f));
        break;
    case UI_ICON_CHART:       // FPS: rising bars
        UiRoundRect(cx - 10, cy + 2, 5, 8, 1, c, c);
        UiRoundRect(cx - 2.5f, cy - 3, 5, 13, 1, c, c);
        UiRoundRect(cx + 5, cy - 9, 5, 19, 1, c, c);
        break;
    case UI_ICON_STRETCH:     // stretch: arrows to both sides
    case UI_ICON_ARROWS_H:
        UiRect(cx - 7, cy - 1, 14, 2, c);
        icon_tri(cx - 8, cy, 9, 3, c);
        icon_tri(cx + 8, cy, 9, 1, c);
        if (icon == UI_ICON_STRETCH) { UiRect(cx - 12, cy - 7, 1.5f, 14, soft); UiRect(cx + 10.5f, cy - 7, 1.5f, 14, soft); }
        break;
    case UI_ICON_ARROWS_V:    // up and down
        UiRect(cx - 1, cy - 7, 2, 14, c);
        icon_tri(cx, cy - 8, 9, 0, c);
        icon_tri(cx, cy + 8, 9, 2, c);
        break;
    case UI_ICON_CROSSHAIR:   // calibration: a ring and its ticks
        UiRoundRing(cx - 8, cy - 8, 16, 16, 8, 2.0f, c);
        UiRect(cx - 12, cy - 1, 6, 2, c);
        UiRect(cx + 6, cy - 1, 6, 2, c);
        UiRect(cx - 1, cy - 12, 2, 6, c);
        UiRect(cx - 1, cy + 6, 2, 6, c);
        UiCircle(cx, cy, 1.8f, c);
        break;
    case UI_ICON_SUN:         // brightness: a sun
        UiCircle(cx, cy, 5, c);
        for (k = 0; k < 8; k++)
        {
            float a = k * 0.785398f;
            UiCircle(cx + cosf(a) * 9.5f, cy + sinf(a) * 9.5f, 1.6f, c);
        }
        break;
    case UI_ICON_VSYNC:       // V-Sync: two frames kept in step
        UiRoundRing(cx - 11, cy - 8, 13, 10, 2, 1.5f, soft);
        UiRoundRing(cx - 2, cy - 2, 13, 10, 2, 2.0f, c);
        break;
    case UI_ICON_EYE:         // antiflicker: an eye
        UiRoundRing(cx - 12, cy - 6.5f, 24, 13, 6.5f, 2.0f, c);
        UiCircle(cx, cy, 3.5f, c);
        break;
    case UI_ICON_SCANLINES:   // screen effect: a screen with lines
        UiRoundRing(cx - 11, cy - 9, 22, 18, 3, 2.0f, c);
        for (k = -1; k <= 1; k++) UiRect(cx - 7, cy + k * 4 - 0.75f, 14, 1.5f, soft);
        break;
    case UI_ICON_FRAME:       // picture size: a frame in a frame
        UiRoundRing(cx - 11, cy - 9, 22, 18, 3, 1.5f, soft);
        UiRoundRing(cx - 6, cy - 4.5f, 12, 9, 2, 2.0f, c);
        break;
    case UI_ICON_CLOCK:       // audio margin: a clock
        UiRoundRing(cx - 10, cy - 10, 20, 20, 10, 2.0f, c);
        UiRect(cx - 1, cy - 6, 2, 7, c);
        UiRect(cx - 1, cy - 1, 6, 2, c);
        break;
    case UI_ICON_PALETTE:     // skins: a palette
        UiRoundRing(cx - 11, cy - 10, 22, 20, 10, 2.0f, c);
        UiCircle(cx - 4, cy - 3, 2.2f, c);
        UiCircle(cx + 3, cy - 4, 2.2f, c);
        UiCircle(cx + 5, cy + 3, 2.2f, c);
        break;
    case UI_ICON_GLOBE:       // language: a globe
        UiRoundRing(cx - 10, cy - 10, 20, 20, 10, 2.0f, c);
        UiRoundRing(cx - 4.5f, cy - 10, 9, 20, 4.5f, 1.5f, soft);
        UiRect(cx - 10, cy - 0.75f, 20, 1.5f, soft);
        break;
    case UI_ICON_MUSIC:       // music: two joined notes
        UiCircle(cx - 6, cy + 6, 3.6f, c);
        UiCircle(cx + 6, cy + 4, 3.6f, c);
        UiRect(cx - 3.4f, cy - 9, 2, 15, c);
        UiRect(cx + 8.6f, cy - 11, 2, 15, c);
        UiRoundRect(cx - 3.4f, cy - 11.5f, 14, 3.5f, 1, c, c);
        break;
    case UI_ICON_SPEAKERS:    // mono / stereo: two speakers
        for (k = 0; k < 2; k++)
        {
            float sx = cx + (k ? 2.0f : -12.0f);
            UiRoundRing(sx, cy - 9, 10, 18, 2, 1.8f, c);
            UiCircle(sx + 5, cy + 3, 2.6f, c);
            UiCircle(sx + 5, cy - 4, 1.4f, c);
        }
        break;
    case UI_ICON_PLUS:        // make something new: a plus in a ring
        UiRoundRing(cx - 11, cy - 11, 22, 22, 11, 2.0f, c);
        UiRoundRect(cx - 6, cy - 1.2f, 12, 2.4f, 1, c, c);
        UiRoundRect(cx - 1.2f, cy - 6, 2.4f, 12, 1, c, c);
        break;
    case UI_ICON_INFO:        // about: an "i" in a ring
        UiRoundRing(cx - 10, cy - 10, 20, 20, 10, 2.0f, c);
        UiCircle(cx, cy - 5, 1.6f, c);
        UiRoundRect(cx - 1.25f, cy - 2, 2.5f, 9, 1, c, c);
        break;
    case UI_ICON_TRASH:       // delete: a bin and its lid
        UiRoundRect(cx - 9, cy - 9, 18, 2.5f, 1, c, c);
        UiRect(cx - 3, cy - 11, 6, 2, c);
        UiRoundRing(cx - 7, cy - 5, 14, 16, 2, 2.0f, c);
        UiRect(cx - 2.75f, cy - 2, 1.5f, 10, soft);
        UiRect(cx + 1.25f, cy - 2, 1.5f, 10, soft);
        break;
    case UI_ICON_LAYOUT:      // design: four tiles
        for (k = 0; k < 4; k++) UiRoundRect(cx + (k % 2 ? 1.5f : -10.5f), cy + (k / 2 ? 1.5f : -10.5f), 9, 9, 2, k ? soft : c, k ? soft : c);
        break;
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
// How far the side bar is still sliding in (0 once in place; UiMenuThumb follows it).
static float s_menuSlide;

static float ease_out(float t) { if (t <= 0) return 0; if (t >= 1) return 1; t = 1 - t; return 1 - t * t * t; }

// ---- Settings, big icons style (Interface > Settings style; inspired by Kodi) ----
// The main page as tiles with big icons; each other list page as a side bar (the
// logo, the page's name, the main page's sections with the one open lit) and its
// rows on the right, the time above them and, under them, what the selected row
// does. The panels, edges and selection take Interface > Borders and panels as
// everything else does.
static const struct { int label, desc; } s_rowDesc[] = {
    { T_ANTIFLICKER, T_D_ANTIFLICKER },
    { T_ART_DOWNLOADS, T_D_ART_DOWNLOADS },
    { T_ART_EXTRAS, T_D_ART_EXTRAS },
    { T_ART_BACKGROUND, T_D_ART_BACKGROUND },
    { T_ABOUT, T_D_ABOUT },
    { T_AUDIO_MARGIN, T_D_AUDIO_MARGIN },
    { T_AUDIO_OUTPUT, T_D_AUDIO_OUTPUT },
    { T_AUTOSAVE_SLOT, T_D_AUTOSAVE_SLOT },
    { T_BATTERY_WARN, T_D_BATTERY_WARN },
    { T_BG_EFFECT, T_D_BG_EFFECT },
    { T_BLUR, T_D_BLUR },
    { T_BORDERS, T_D_BORDERS },
    { T_BRIGHTNESS, T_D_BRIGHTNESS },
    { T_CATEGORIES, T_D_CATEGORIES },
    { T_CAT_DELETE, T_D_CAT_DELETE },
    { T_CAT_FIELD, T_D_CAT_FIELD },
    { T_CAT_NEW, T_D_CAT_NEW },
    { T_CAT_SAVE, T_D_CAT_SAVE },
    { T_CAT_VALUE, T_D_CAT_VALUE },
    { T_COLOR_STYLE, T_D_COLOR_STYLE },
    { T_CONTROLS, T_D_CONTROLS },
    { T_CPU_NEXT, T_D_CPU_NEXT },
    { T_DESIGN, T_D_DESIGN },
    { T_DEV_MODE, T_D_DEV_MODE },
    { T_EXPLORER, T_D_EXPLORER },
    { T_FONT, T_D_FONT },
    { T_FXAA, T_D_FXAA },
    { T_FX_RESET, T_D_FX_RESET },
    { T_GAME_IMAGES, T_D_GAME_IMAGES },
    { T_GAME_BG, T_D_GAME_BG },
    { T_GFX_LOG, T_D_GFX_LOG },
    { T_GRAPHICS, T_D_GRAPHICS },
    { T_HIRESTEX, T_D_HIRESTEX },
    { T_IMG_KIND, T_D_IMG_KIND },
    { T_INTERFACE, T_D_INTERFACE },
    { T_INTRO_WELCOME, T_D_INTRO_WELCOME },
    { T_LANGUAGE, T_D_LANGUAGE },
    { T_MUSIC, T_D_MUSIC },
    { T_PAD_LOSS, T_D_PAD_LOSS },
    { T_PATHS, T_D_PATHS },
    { T_PERF_OVERLAY, T_D_PERF_OVERLAY },
    { T_PIC_CAL, T_D_PIC_CAL },
    { T_PIC_LR, T_D_PIC_LR },
    { T_PIC_SIZE, T_D_PIC_SIZE },
    { T_PIC_UD, T_D_PIC_UD },
    { T_PRESET, T_D_PRESET },
    { T_RESOLUTION, T_D_RESOLUTION },
    { T_RES_169, T_D_RES_169 },
    { T_RSP_NEXT, T_D_RSP_NEXT },
    { T_SAVER, T_D_SAVER },
    { T_SAVER_MODE, T_D_SAVER_MODE },
    { T_SAVER_PREVIEW, T_D_SAVER_PREVIEW },
    { T_SAVER_TIME, T_D_SAVER_TIME },
    { T_SAVE_ALL, T_D_SAVE_ALL },
    { T_SAVE_GAME, T_D_SAVE_GAME },
    { T_SCREEN, T_D_SCREEN },
    { T_SCREEN_EFFECT, T_D_SCREEN_EFFECT },
    { T_SEL_COLOR, T_D_SEL_COLOR },
    { T_SETVIEW, T_D_SETVIEW },
    { T_SFX_STYLE, T_D_SFX_STYLE },
    { T_SHARPEN, T_D_SHARPEN },
    { T_SOUND, T_D_SOUND },
    { T_START_ROM, T_D_START_ROM },
    { T_STRETCH, T_D_STRETCH },
    { T_SYSTEM, T_D_SYSTEM },
    { T_TEXFILTER, T_D_TEXFILTER },
    { T_TRANSLATE_DESC, T_D_TRANSLATE_DESC },
    { T_CAL_EDGES, T_D_CAL_EDGES },
    { T_TEX_R3, T_D_TEX_R3 },
    { T_UI_SOUNDS, T_D_UI_SOUNDS },
    { T_VOLUME, T_D_VOLUME },
    { T_VSYNC, T_D_VSYNC },
    { T_WALLPAPERS, T_D_WALLPAPERS },
    { T_WIZARD, T_D_WIZARD }
};

// What a row does (by its label as shown), NULL when there is no text for it.
static const char *row_desc(const char *label)
{
    size_t i;
    for (i = 0; i < sizeof(s_rowDesc) / sizeof(s_rowDesc[0]); i++)
        if (!strcmp(label, Tr(s_rowDesc[i].label))) return Tr(s_rowDesc[i].desc);
    return NULL;
}

// The parts of each page (the side bar): by the page's title, each part its rows'
// labels. A row in none goes with the first part; a part with no row shown (rows
// of the ROM list only, in game) is left out. A page not here has no parts.
struct KodiPart { int name; int rows[6]; };
struct KodiPage { int title; KodiPart parts[4]; };
static const KodiPage s_kodiPages[] = {
    { T_SYSTEM, { { T_KC_PAD_SAVE, { T_PAD_LOSS, T_AUTOSAVE_SLOT, T_BATTERY_WARN } },
                  { T_KC_PERF, { T_PERF_OVERLAY, T_CPU_NEXT, T_RSP_NEXT } },
                  { T_KC_TOOLS, { T_DEV_MODE, T_WIZARD, T_EXPLORER } } } },
    // The picture's filters and effects all here (the preset sets them together);
    // the Screen page keeps the output: size, colour, sync.
    { T_GRAPHICS, { { T_KC_QUALITY, { T_PRESET, T_TEXFILTER, T_HIRESTEX } },
                    { T_KC_FILTERS, { T_FXAA, T_SHARPEN, T_BLUR, T_ANTIFLICKER } },
                    { T_KC_EFFECTS, { T_SCREEN_EFFECT } } } },
    { T_SCREEN, { { T_KC_IMAGE, { T_RESOLUTION, T_RES_169, T_STRETCH, T_PIC_CAL } },
                  { T_KC_COLOR, { T_BRIGHTNESS, T_COLOR_STYLE } },
                  { T_KC_DISPLAY, { T_VSYNC } } } },
    { T_SOUND, { { T_VOLUME, { T_VOLUME } },
                 { T_KC_OUTPUT, { T_AUDIO_MARGIN, T_AUDIO_OUTPUT } },
                 { T_KC_MENUS, { T_UI_SOUNDS, T_SFX_STYLE, T_MUSIC } } } },
    { T_INTERFACE, { { T_KC_LOOK, { T_LANGUAGE, T_SETVIEW, T_FONT, T_INTRO_WELCOME } },
                     { T_KC_COLORS, { T_SEL_COLOR, T_BORDERS } },
                     { T_KC_BACKGROUNDS, { T_WALLPAPERS, T_BG_EFFECT, T_SAVER } },
                     { T_KC_GAMELIST, { T_DESIGN, T_GAME_IMAGES, T_TRANSLATE_DESC, T_CATEGORIES, T_START_ROM, T_GAME_BG } } } },
};

// The page's parts as shown: names (T_*), and each row's part (index into names); 0 parts: none.
struct KodiParts { int count, name[4], rowPart[UI_MENU_MAX]; };
static void kodi_parts(const UiMenu *m, KodiParts *kp)
{
    const KodiPage *pg = NULL;
    int used[4] = { 0, 0, 0, 0 }, map[4], i, k, r;
    size_t p;
    kp->count = 0;
    for (p = 0; p < sizeof(s_kodiPages) / sizeof(s_kodiPages[0]); p++)
        if (!strcmp(m->title, Tr(s_kodiPages[p].title))) pg = &s_kodiPages[p];
    if (!pg) return;
    for (i = 0; i < m->count; i++)
    {
        int part = 0;
        for (k = 0; k < 4 && pg->parts[k].name; k++)
            for (r = 0; r < 6 && pg->parts[k].rows[r]; r++)
                if (!strcmp(m->label[i], Tr(pg->parts[k].rows[r]))) part = k;
        kp->rowPart[i] = part;
        used[part] = 1;
    }
    for (k = 0; k < 4; k++)
    {
        map[k] = -1;
        if (pg->parts[k].name && used[k]) { map[k] = kp->count; kp->name[kp->count++] = pg->parts[k].name; }
    }
    for (i = 0; i < m->count; i++) kp->rowPart[i] = map[kp->rowPart[i]];
}

static int s_kodiSide;   // the side bar has the selection (its part s_kodiPart), not the rows
static int s_kodiPart;
static int s_kodiTile;   // the main page's tile last selected (back up from its save rows)

// Tiles a row: all of them up to 5, else two rows (the first the longer).
static int kodi_cols(int nt) { return nt <= 5 ? nt : (nt + 1) / 2; }

// The rows of the main page that are tiles: up to the first save row (those go in a list under the tiles).
static int kodi_tiles(const UiMenu *m)
{
    int i;
    for (i = 0; i < m->count; i++)
        if (!strcmp(m->label[i], Tr(T_SAVE_GAME)) || !strcmp(m->label[i], Tr(T_SAVE_ALL))) return i;
    return m->count;
}

// A settings page (not the main one) in the big icons style.
// A settings page of rows (the usual footer, or the Paths page's with Y to reset one).
static int footer_rows(const UiMenu *m)
{
    return !strcmp(m->footer, Tr(T_FOOT_SELECT)) || !strcmp(m->footer, Tr(T_FOOT_PATHS));
}

static int kodi_sub(const UiMenu *m)
{
    return s_kodi && m->page == UI_PAGE_LIST && footer_rows(m) && !kodi_grid(m) && strcmp(m->title, Tr(T_STATES));
}

// The rows in the order the page shows them: part after part (each part's rows in
// their own order); without parts, as they are.
static int kodi_order(const UiMenu *m, const KodiParts *kp, int *order)
{
    int n = 0, p, i;
    if (kp->count < 2)
    {
        for (i = 0; i < m->count; i++) order[n++] = i;
        return n;
    }
    for (p = 0; p < kp->count; p++)
        for (i = 0; i < m->count; i++)
            if (kp->rowPart[i] == p) order[n++] = i;
    return n;
}

// ---- Dropdowns: a row with "< >" values opens the list of its values under it ----
// The values are found by a dry run on copies of the menu and the settings (right
// pressed again and again until the first value comes back; s_kodiSim keeps the
// handlers' side effects out). Picking one presses right that many times for real.
#define KODI_DD_MAX 40
static int s_kodiDd = -1;           // the row whose list is open (-1: none)
static int s_kodiDdN, s_kodiDdSel, s_kodiDdTop;
static int s_kodiDdRot;              // the list as shown starts this many values after the row's own (right presses)
static char s_kodiDdItem[KODI_DD_MAX][64];

static int kodi_arrows(const char *v)
{
    size_t n = strlen(v);
    return n >= 4 && v[0] == '<' && v[1] == ' ' && v[n - 1] == '>' && v[n - 2] == ' ' && switch_value(v) < 0;
}

// The open list of values (both settings styles): under its row (yBelow), its right edge at
// `right`; above the row when there is no room under it.
static void dd_draw(float right, float yBelow, float rowH)
{
    const float ih = 36, w = 340, x = right - w;
    int vis = s_kodiDdN < 7 ? s_kodiDdN : 7, k;
    float h = vis * ih + 16, yy = yBelow + 2;
    if (s_kodiDdSel < s_kodiDdTop) s_kodiDdTop = s_kodiDdSel;
    if (s_kodiDdSel >= s_kodiDdTop + vis) s_kodiDdTop = s_kodiDdSel - vis + 1;
    if (yy + h > UI_HEIGHT - 20) yy = yBelow - rowH - h - 2;
    UiShadow(x, yy + 8, w, h, 12, 30, D3DCOLOR_ARGB(150, 0, 0, 0));
    UiRoundRect(x, yy, w, h, 12, D3DCOLOR_ARGB(245, 33, 35, 46), D3DCOLOR_ARGB(245, 27, 29, 38));
    UiRoundRing(x, yy, w, h, 12, 1.5f, SEL_HI(200));
    for (k = 0; k < vis; k++)
    {
        int it = s_kodiDdTop + k;
        char txt[64], fit[64];
        size_t tn = strlen(s_kodiDdItem[it]), cut = tn >= 4 ? tn - 4 : tn;
        float iy = yy + 8 + k * ih;
        memcpy(txt, s_kodiDdItem[it] + (tn >= 4 ? 2 : 0), cut);
        txt[cut] = 0;
        UiTextFit(UI_REGULAR, 20, w - 40, txt, fit, sizeof(fit));
        if (it == s_kodiDdSel) selection_bar(x + 6, iy, w - 12, ih);
        UiText(it == s_kodiDdSel ? UI_BOLD : UI_REGULAR, 20, x + 22, iy + ih * 0.5f, it == s_kodiDdSel ? UI_COL_TEXT : UI_COL_TEXT2, fit, UI_LEFT);
    }
    if (s_kodiDdTop > 0) icon_tri(x + w * 0.5f, yy - 2, 8, 0, UI_COL_TEXT3);
    if (s_kodiDdTop + vis < s_kodiDdN) icon_tri(x + w * 0.5f, yy + h + 4, 8, 2, UI_COL_TEXT3);
}

static void kodi_clock(void)
{
    SYSTEMTIME t;
    char buf[16];
    GetLocalTime(&t);
    sprintf(buf, "%d:%02d", t.wHour, t.wMinute);
    UiText(UI_REGULAR, 34, UI_WIDTH - 34, 38, UiAlpha(UI_COL_TEXT, 0.85f), buf, UI_RIGHT);
}

// The main page's opening (Settings from the ROM list, Y): the logo comes down,
// then the sections rise and grow in one after another. 0..1 for tile `i`
// (-1: the logo), from when the page started being drawn.
static float s_gridOpenAt = -10.0f, s_gridLastDraw = -10.0f;
static float kodi_open_ease(int i)
{
    float t = (UiTime() - s_gridOpenAt - (i < 0 ? 0.0f : 0.08f + 0.055f * i)) / 0.38f;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   // eases out
}

static void kodi_draw(const UiMenu *m)
{
    int i;
    if (kodi_grid(m))
    {
        if (UiTime() - s_gridLastDraw > 0.25f) s_gridOpenAt = UiTime();   // shown again after a while: it opens
        s_gridLastDraw = UiTime();
    }
    if (kodi_grid(m))
    {
        // The sections as one row of tiles, centred; the save rows as a list under them.
        const int nt = kodi_tiles(m), cols = kodi_cols(nt), rows = nt > cols ? 2 : 1;
        const float gap = 20, th = rows > 1 ? 150 : 170, y0 = rows > 1 ? 130 : 170;
        float tw = cols ? (UI_WIDTH - 120 - gap * (cols - 1)) / cols : 228, ly;
        const char *d;
        if (tw > 228) tw = 228;
        {
            const float e = kodi_open_ease(-1);
            UiBrandFit((UI_WIDTH - 320) * 0.5f, 30 - (1.0f - e) * 24, 320, 80);
            UiText(UI_BOLD, 16, UI_WIDTH * 0.5f, 118, UiAlpha(UI_COL_TEXT3, e), "LAIKA 64 V" H64_VERSION_STRING, UI_CENTER);
        }
        kodi_clock();
        {
            // The selection: slides from tile to tile and breathes, as the lists' bar
            // does (it jumped before), drawn under the tiles' symbols and names.
            static UiAnim ax, ay;
            static int wasTile;
            int isTile = m->sel >= 0 && m->sel < nt;
            if (isTile)
            {
                int row = m->sel / cols, inRow = row ? nt - cols : (nt < cols ? nt : cols);
                float tx = (UI_WIDTH - (inRow * tw + (inRow - 1) * gap)) * 0.5f + (m->sel % cols) * (tw + gap) - 6, ty = y0 + row * (th + gap) - 6;
                anim_step(&ax, tx, !wasTile);   // ax.value, ay.value: where it is now
                anim_step(&ay, ty, !wasTile);
            }
            wasTile = isTile;
            for (i = 0; i < nt; i++)
            {
                // Each row centred on its own (the second may hold fewer).
                int row = i / cols, inRow = row ? nt - cols : (nt < cols ? nt : cols);
                float x = (UI_WIDTH - (inRow * tw + (inRow - 1) * gap)) * 0.5f + (i % cols) * (tw + gap), y = y0 + row * (th + gap);
                const float e = kodi_open_ease(i);
                if (e <= 0.0f) continue;
                y += (1.0f - e) * 40;   // rising and growing into place
                UiSetScale(x + tw * 0.5f, y + th * 0.5f, 0.82f + 0.18f * e);
                UiPanel(x, y, tw, th, 12);
                UiSetScale(0, 0, 1.0f);
            }
            if (isTile && kodi_open_ease(m->sel) > 0.0f)
            {
                const float e = kodi_open_ease(m->sel);
                float x = ax.value, y = ay.value + (1.0f - e) * 40, w = tw + 12, h = th + 12, p = selection_pulse();
                UiSetScale(x + w * 0.5f, y + h * 0.5f, 0.82f + 0.18f * e);
                selection_glow(x - 4, y - 4, w + 8, h + 8, 16, p * e);
                edge_glow(x, y, w, h, 12);
                UiRoundRect(x, y, w, h, UiPanelRadius(12), SEL_HI((200 + 40 * p) * e), SEL_LO((200 + 40 * p) * e));
                UiSetScale(0, 0, 1.0f);
            }
        }
        for (i = 0; i < nt; i++)
        {
            // Each row centred on its own (the second may hold fewer).
            int row = i / cols, inRow = row ? nt - cols : (nt < cols ? nt : cols);
            float x = (UI_WIDTH - (inRow * tw + (inRow - 1) * gap)) * 0.5f + (i % cols) * (tw + gap), y = y0 + row * (th + gap), w = tw, h = th, cx;
            int sel = i == m->sel;
            const float e = kodi_open_ease(i);
            D3DCOLOR col = UiAlpha(sel ? UI_COL_TEXT : UiAlpha(UI_COL_TEXT, 0.8f), e);
            if (e <= 0.0f) continue;
            if (sel) { x -= 6; y -= 6; w += 12; h += 12; }   // the selected tile a little bigger (its colour is the sliding selection's)
            y += (1.0f - e) * 40;
            cx = x + w * 0.5f;
            UiSetScale(cx, y + h * 0.42f, 2.6f * (0.82f + 0.18f * e));
            menu_icon(m->icon[i] ? m->icon[i] : UI_ICON_GEAR, cx, y + h * 0.42f, col);
            UiSetScale(0, 0, 1.0f);
            UiText(sel ? UI_BOLD : UI_REGULAR, 20, cx, y + h - 28, col, m->label[i], UI_CENTER);
        }
        ly = y0 + rows * th + (rows - 1) * gap + 40;
        for (i = nt; i < m->count; i++)
        {
            const float lw = 520, lx = (UI_WIDTH - lw) * 0.5f, cy = ly + 24;
            int sel = i == m->sel;
            if (sel) selection_bar(lx, ly, lw, 48);
            else UiPanel(lx, ly, lw, 48, 12);
            menu_icon(m->icon[i] ? m->icon[i] : UI_ICON_SAVE, lx + 40, cy, sel ? UI_COL_TEXT : UI_COL_TEXT2);
            UiText(sel ? UI_BOLD : UI_REGULAR, 21, lx + 72, cy, sel ? UI_COL_TEXT : UI_COL_TEXT2, m->label[i], UI_LEFT);
            ly += 58;
        }
        d = m->sel >= 0 && m->sel < m->count ? row_desc(m->label[m->sel]) : NULL;
        if (d) paragraph(UI_REGULAR, 19, 160, ly + 8, UI_WIDTH - 320, 2, SEL_HI(255), d);
        UiScreenFooter(m->footer);
        return;
    }
    {
        // A page: the side bar with its parts on the left, the rows of the part shown on the right.
        const float sw = 320, rx = sw + 20, rw = UI_WIDTH - rx - 20, ry0 = 76, rowH = 44, descY = 560;
        const int visible = 10;
        KodiParts kp;
        int part, shown = 0, before = 0, top;
        const char *d;
        kodi_parts(m, &kp);
        if (kp.count < 2) s_kodiSide = 0;
        part = kp.count >= 2 ? (s_kodiSide ? s_kodiPart : kp.rowPart[m->sel >= 0 && m->sel < m->count ? m->sel : 0]) : -1;
        UiPanel(-30, -30, sw + 30, UI_HEIGHT + 60, 0);
        UiBrandFit(30, 18, sw - 60, 64);
        UiText(UI_REGULAR, 26, sw * 0.5f + 10, 106, UI_COL_TEXT, m->title, UI_CENTER);
        for (i = 0; i < kp.count; i++)
        {
            float y = 146 + i * 52, cy = y + 23;
            int here = i == part;
            if (here && s_kodiSide) selection_bar(14, y, sw - 28, 46);
            else if (here) UiRoundRect(14, y, sw - 28, 46, 12, SEL_LO(170), SEL_LO(120));
            UiText(here ? UI_BOLD : UI_REGULAR, 22, 54, cy, here ? UI_COL_TEXT : UI_COL_TEXT2, Tr(kp.name[i]), UI_LEFT);
        }
        kodi_clock();
        UiPanel(rx, ry0 - 16, rw, descY - ry0 + 104, 16);
        // Every part under its header (the menu never looks empty), scrolled to the selection.
        {
            int order[UI_MENU_MAX], n = kodi_order(m, &kp, order), line[UI_MENU_MAX * 2], lines = 0, selLine = 0, first, k, ddRow = -1;
            float y, ddY = 0, avail = descY - ry0 - 6;
            for (i = 0; i < n; i++)
            {
                if (kp.count >= 2 && (i == 0 || kp.rowPart[order[i]] != kp.rowPart[order[i - 1]])) line[lines++] = -1 - kp.rowPart[order[i]];   // a header
                if (order[i] == m->sel) selLine = lines;
                line[lines++] = order[i];
            }
            // From the first line that keeps the selection (and its header) in view.
            first = 0;
            {
                float h = 0;
                for (k = selLine; k >= 0; k--)
                {
                    h += line[k] < 0 ? 34.0f : rowH;
                    if (h > avail) { first = k + 1; break; }
                }
                if (first > 0 && first <= selLine && line[first - 1] < 0 && first - 1 >= 0) first--;
            }
            y = ry0;
            for (k = first; k < lines && y < ry0 + avail; k++)
            {
                if (line[k] < 0)
                {
                    UiText(UI_BOLD, 16, rx + rw * 0.5f, y + 18, UI_COL_TEXT3, Tr(kp.name[-1 - line[k]]), UI_CENTER);
                    UiRect(rx + 20, y + 33, rw - 40, 1, UiAlpha(UI_COL_LINE, 0.8f));
                    y += 34;
                    continue;
                }
                {
                    int r = line[k], sel = r == m->sel && !s_kodiSide;
                    float cy = y + rowH * 0.5f;
                    const char *v = m->value[r];
                    if (sel) selection_bar(rx + 12, y + 2, rw - 24, rowH - 4);
                    if (m->icon[r]) menu_icon(m->icon[r], rx + 42, cy, sel ? UI_COL_TEXT : UI_COL_TEXT2);
                    UiText(sel ? UI_BOLD : UI_REGULAR, 21, rx + (m->icon[r] ? 70 : 30), cy, sel ? UI_COL_TEXT : UI_COL_TEXT2, m->label[r], UI_LEFT);
                    if (kodi_arrows(v))
                    {
                        // The value without arrows; a small mark for its list.
                        char inner[64];
                        size_t vn = strlen(v), cut = vn - 4 < sizeof(inner) - 1 ? vn - 4 : sizeof(inner) - 1;
                        memcpy(inner, v + 2, cut);
                        inner[cut] = 0;
                        icon_tri(rx + rw - 38, cy, 9, 2, sel ? SEL_HI(255) : UI_COL_TEXT3);
                        UiText(sel ? UI_BOLD : UI_REGULAR, 21, rx + rw - 56, cy, sel ? UI_COL_TEXT : UI_COL_TEXT2, inner, UI_RIGHT);
                    }
                    else if (v[0]) draw_value(rx + rw - 30, cy, v, sel);
                    if (r == s_kodiDd) { ddRow = r; ddY = y + rowH; }
                    y += rowH;
                }
            }
            top = first;
            shown = lines;
            if (ddRow >= 0) dd_draw(rx + rw - 24, ddY, rowH);
        }
        UiRect(rx + 20, descY, rw - 40, 1, UI_COL_LINE);
        d = s_kodiSide ? NULL : m->sel >= 0 && m->sel < m->count ? row_desc(m->label[m->sel]) : NULL;
        if (d) paragraph(UI_REGULAR, 19, rx + 30, descY + 12, rw - 60, 3, SEL_HI(255), d);
        UiFooter(rx + 30, UI_HEIGHT - 40, m->footer);
    }
}

static void edges_draw(const UiMenu *m);   // Image calibration > Calibrate the edges (below)

void UiMenuDraw(const UiMenu *m)
{
    static UiAnim sel;
    static char lastTitle[128];
    static int lastCount;
    static float lastDraw = -1.0f, openAt = -10.0f, pageAt = -10.0f;
    const float y = 12, w = 690, h = UI_HEIGHT - 24, head = 190, foot = 78;
    // Rows shrink a little when a page has many, so they stay above the prompts.
    const float room = h - head - 18 - foot;
    float rowH = m->count > 0 && room / m->count < 48 ? room / m->count : 48, sy, lx, ix, x, now = UiTime(), shift;
    int i, restart = strcmp(lastTitle, m->title) != 0 || lastCount != m->count, rtl = LangRtl(), icons = 0;
    float ddY = 0;   // under the row whose list of values is open
    char title[160];
    if (m->page == UI_PAGE_CONTROLS) { controls_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_CALIB) { calib_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_VOLUME) { volume_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_FX) { fxpage_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_WALL) { wallpage_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_BORDER) { borderpage_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_PERF) { perfpage_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_HOTKEYS) { hkpage_draw(m); lastTitle[0] = 0; return; }
    if (m->page == UI_PAGE_EDGES) { edges_draw(m); lastTitle[0] = 0; return; }
    if (s_kodi && m->page == UI_PAGE_LIST && footer_rows(m) && strcmp(m->title, Tr(T_STATES)))   // big icons style (the States page: a list)
    {
        kodi_draw(m);
        lastTitle[0] = 0;
        return;
    }
    // A short entrance: the bar slides in from the left as the menu opens (it
    // was not drawn a moment ago: the pause menu, Settings from the ROM list),
    // and its rows slide in when the page changes.
    {
        // Opened just now: not drawn in the frame before (counted in frames, not
        // time - a slow frame, a wallpaper being decoded, must not replay the
        // entrance), or drawn last long ago (the pause menu, between frames of
        // no UI at all).
        static unsigned lastFrame;
        unsigned frame = UiFrameCount();
        if (frame - lastFrame > 1 || now - lastDraw > 3.0f) openAt = now;
        lastFrame = frame;
    }
    lastDraw = now;
    if (restart) pageAt = now;
    s_menuSlide = (1.0f - ease_out((now - openAt) / 0.18f)) * 80.0f;
    shift = (1.0f - ease_out((now - pageAt) / 0.16f)) * 22.0f;
    x = 8 - s_menuSlide;
    strncpy(lastTitle, m->title, sizeof(lastTitle) - 1);
    lastCount = m->count;
    for (i = 0; i < m->count; i++) icons |= m->icon[i];
    // Dimmed over the ROM list's own background (contrast); not in-game,
    // where this draws over the frozen frame and the dim hid graphics
    // changes (texture packs, filters) from showing clearly while paused.
    if (s_browser) UiDim();
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiBrand(x + (w - UiBrandWidth(BRAND_SIZE)) * 0.5f, y + 60, BRAND_SIZE);   // centred, like the page's name
    // The page's name under the logo, centred, in capitals.
    UiUpper(UI_BOLD, m->title, title, sizeof(title));
    UiText(UI_BOLD, 30, x + w * 0.5f, y + 130, UI_COL_TEXT, title, UI_CENTER);
    if (!strcmp(m->title, Tr(T_SETTINGS)))
        UiText(UI_REGULAR, 16, x + w * 0.5f, y + 160, UI_COL_TEXT3, "LAIKA 64 V" H64_VERSION_STRING, UI_CENTER);   // the version, on the main page
    sy = anim_step(&sel, y + head + m->sel * rowH, restart);
    if (m->count) selection_bar_side(x + 42, sy + 3, w - 66, rowH - 6, rtl);
    // With symbols, they take the label's place and the label moves over.
    ix = (rtl ? x + w - 78 : x + 78) + (rtl ? -shift : shift);
    lx = icons ? (rtl ? ix - 24 : ix + 24) : (rtl ? x + w - 66 - shift : x + 66 + shift);
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
        if (i == s_kodiDd) ddY = cy + rowH * 0.5f;
    }
    if (s_kodiDd >= 0 && ddY > 0) dd_draw(x + w - 30, ddY, rowH);   // a value's open list
    UiRect(x + 46, y + h - foot, w - 70, 1, UI_COL_LINE);
    UiFooter(rtl ? x + w - 66 - UiFooterWidth(m->footer) : x + 66, y + h - foot * 0.5f, m->footer);
}

// A picture under the side bar's rows (the States page: the slot's save), in a
// 4:3 box as tall as the room left allows, framed like the selected cover
// (pulsing light blue ring and glow). NULL: an empty box with `emptyText`.
void UiMenuThumb(const UiMenu *m, IDirect3DTexture9 *tex, const char *emptyText)
{
    const float x = 8 - s_menuSlide, y = 12, w = 690, h = UI_HEIGHT - 24, head = 190, foot = 78;
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
    if (kodi_grid(m))
    {
        // Big icons: the tiles in one row (left/right), the save rows under them (up/down).
        int nt = kodi_tiles(m), cols = kodi_cols(nt);
        if (m->sel < nt)
        {
            int row = cols ? m->sel / cols : 0;
            if (down & XINPUT_GAMEPAD_DPAD_LEFT) m->sel = (m->sel + nt - 1) % nt;
            if (down & XINPUT_GAMEPAD_DPAD_RIGHT) m->sel = (m->sel + 1) % nt;
            if ((down & XINPUT_GAMEPAD_DPAD_UP) && row > 0) m->sel -= cols;
            if ((down & XINPUT_GAMEPAD_DPAD_DOWN) && nt > cols && row == 0) m->sel = m->sel + cols < nt ? m->sel + cols : nt - 1;
            else if ((down & XINPUT_GAMEPAD_DPAD_DOWN) && nt < m->count) m->sel = nt;
            if (m->sel < nt) s_kodiTile = m->sel;
        }
        else
        {
            if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = m->sel == nt ? (s_kodiTile < nt ? s_kodiTile : 0) : m->sel - 1;
            if ((down & XINPUT_GAMEPAD_DPAD_DOWN) && m->sel + 1 < m->count) m->sel++;
        }
        return;
    }
    if (down & XINPUT_GAMEPAD_DPAD_UP) m->sel = (m->sel + m->count - 1) % m->count;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) m->sel = (m->sel + 1) % m->count;
}

// ---- Settings ----
// Main page: System, Graphics, Screen, Sound, Controls, Interface, save
// (B goes back). Graphics (preset, texture filter, hi-res textures, smoothing,
// sharpening, blur, FPS) and Screen (resolution, stretch, image calibration,
// brightness, VSync, antiflicker, screen effect) are shown in the same
// UiMenu as the main page; their Back returns to it.
// The most used first: the picture and the controls, then the rest.
enum { S_GRAPHICS = 0, S_SCREEN, S_CONTROLS, S_SOUND, S_SYSTEM, S_INTERFACE, S_PATHS };   // S_INTERFACE, S_PATHS: ROM list only
static int s_rowAbout = -1;   // Settings' About row (the last)
static int s_pathsPage;   // Settings > Paths
static void settings_paths(UiMenu *m, const Config *c);
// The Volumen page: three bars (game, button sounds, menu music - the last from the ROM list only:
// no menu music plays in game).
enum { V_GAME = 0, V_SFX, V_MUSIC, V_COUNT };
// The Interface page (ROM list only); B goes back, no row for it. Borders and
// panels, Wallpapers, Background effect and Screen saver open their own pages with A.
// In the parts' order: look (language first), colours, backgrounds, the game list.
enum { I_LANGUAGE = 0, I_SETVIEW, I_FONT, I_INTRO, I_SELCOLOR, I_BORDERS, I_WALLPAPERS, I_EFFECT, I_SAVER, I_DESIGN };
static int s_saverPage;   // Interface > Screen saver
enum { SAVER_OFF = 0, SAVER_EFFECT, SAVER_WALL, SAVER_LOGO, SAVER_CLOCK, SAVER_COUNT };   // Config::saverMode
#define I_IMAGES (I_DESIGN + 1)    // "Game images": the pictures shown and their downloads, on their own page (ROM list only)
static int s_imagesPage;             // Interface > Game images
static void settings_images(UiMenu *m, const Config *c);
static void images_values(UiMenu *m, const Config *c);
static void paths_values(UiMenu *m, const Config *c);
static int paths_input(UiMenu *m, Config *c, WORD down);
#define I_CATS (I_DESIGN + 3)      // "Categories": the ROM list's tabs, made and changed on their own pages (ROM list only)
#define I_TRANSDESC (I_DESIGN + 2)
#define I_STARTROM (I_DESIGN + 4)   // "Game at start" (ROM list only; was in System) // "Translate descriptions" (ROM list only)
#define I_GAMEBG (I_DESIGN + 5)     // "Background images": a picture from the selected game behind every view (ROM list only)
static int s_gameBg;                 // Config::gameBackdrop (ui_from_config)
// The language the games' descriptions are asked in (Interface > Translate descriptions).
static const char *art_lang(const Config *c) { return c->descTranslate ? LangCode(LangGet()) : "en"; }
static int s_catsPage, s_catEditPage;   // Interface > Categories, and one category being made or changed
static void settings_cats(UiMenu *m, const Config *c, int selCat);   // below, with the game library
static int cats_input(UiMenu *m, Config *c, WORD down);
static int catedit_input(UiMenu *m, Config *c, WORD down);
// The ROM browser's layouts (Config::browserDesign; Interface > Design, X in the browser):
// the list and its card, the covers grid, the big covers grid, the carousel.
// Mosaic: a dense grid (7 x 4). Spotlight: the selection big with its information, the others in a row under it.
// Cartridges: N64 cartridges on a shelf, the box art as labels. Cinema: the picture from the game over the screen, the logo big.
enum { BD_LIST = 0, BD_COVERS, BD_BIG, BD_CAROUSEL, BD_MOSAIC, BD_SPOTLIGHT, BD_CARTRIDGE, BD_CINEMA, BD_COUNT };
static const int s_designLabels[BD_COUNT] = { T_DESIGN_DETAILED, T_DESIGN_COVERS, T_DESIGN_BIG, T_DESIGN_CAROUSEL, T_DESIGN_MOSAIC, T_DESIGN_SPOTLIGHT,
                                              T_DESIGN_CARTRIDGE, T_DESIGN_CINEMA };
// The views that move one game at a time, left and right (the others are grids, or the list).
static int design_row(int d) { return d == BD_CAROUSEL || d == BD_SPOTLIGHT || d == BD_CARTRIDGE || d == BD_CINEMA; }
// The ROM list's animated background (Config::bgEffect; drawn by browser_effect).
enum { BGFX_OFF = 0, BGFX_WAVES, BGFX_PULSE, BGFX_RAIN, BGFX_VORTEX, BGFX_RIPPLES, BGFX_SILK, BGFX_EMBERS, BGFX_GRID,
       BGFX_UNDERWATER, BGFX_ENERGY, BGFX_AURORA, BGFX_LIQUID, BGFX_PARTICLES, BGFX_SMOKE, BGFX_XMB, BGFX_PS4_FLOW,
       BGFX_PS4_BOKEH, BGFX_NEBULA, BGFX_FIRE_DUST, BGFX_LAVA, BGFX_PRISM, BGFX_TOPO, BGFX_HEX, BGFX_SPIRAL, BGFX_INK, BGFX_HOLO,
       BGFX_FRACTAL, BGFX_BURST, BGFX_WARP, BGFX_COUNT };
static const int s_fxLabels[BGFX_COUNT] = { T_OFF, T_FX_WAVES, T_FX_PULSE, T_FX_RAIN, T_FX_VORTEX, T_FX_RIPPLES, T_FX_SILK,
                                            T_FX_EMBERS, T_FX_GRID, T_FX_UNDERWATER, T_FX_ENERGY, T_FX_AURORA, T_FX_LIQUID,
                                            T_FX_PARTICLES, T_FX_SMOKE, T_FX_XMB, T_FX_PS4_FLOW, T_FX_PS4_BOKEH,
                                            T_FX_NEBULA, T_FX_FIRE_DUST, T_FX_LAVA, T_FX_PRISM, T_FX_TOPO, T_FX_HEX, T_FX_SPIRAL,
                                            T_FX_INK, T_FX_HOLO, T_FX_FRACTAL, T_FX_BURST, T_FX_WARP };
static int s_bgEffect = BGFX_WAVES;   // what the ROM list draws (set from Config::bgEffect)
// The colours the effect page offers (Config::fxColor; 0: none - the effect's own colours for colour 1, unused for 2 and 3).
#define FX_PALETTE_COUNT 13
static const int s_fxPalette[FX_PALETTE_COUNT + 1][3] = {
    { 0, 0, 0 },
    { 240, 242, 255 }, { 255, 60, 70 }, { 255, 140, 40 }, { 255, 220, 60 }, { 140, 240, 80 }, { 40, 210, 120 }, { 40, 220, 210 },
    { 60, 200, 255 }, { 60, 120, 255 }, { 110, 80, 255 }, { 170, 80, 240 }, { 240, 70, 200 }, { 255, 130, 180 },
};
static int s_fxColor[3], s_fxOpacity = 100, s_fxSpeed = 100;   // what the ROM list draws with (ui_from_config)
static float s_fxClock = 50.0f, s_fxLast = -1.0f;   // the effects' own time (run at s_fxSpeed), and UiTime() when it last moved
static void browser_effect(void);
static void settings_fx(UiMenu *m, const Config *c);   // Interface > Background effect's own page (below, after Volumen)
static void settings_wall(UiMenu *m, const Config *c);   // Interface > Wallpapers and > Borders (below, after Volumen)
static void settings_saver(UiMenu *m, const Config *c);
static void settings_borders(UiMenu *m, const Config *c);
static void saver_values(UiMenu *m, const Config *c);
static void saver_run(IDirect3DDevice9 *dev, const Config *c);   // after the background effects
static int saver_any_input(void);

// A game's picture as the list shows it: flat, or - its box only (Interface >
// Game images > 3D box; a picture from the game, a title screen, a logo stay
// flat) - as a box seen from its front left corner: the spine (the picture's
// left edge) turning away into shadow, the front receding a little to the
// right, a soft light down the edge between and across the top, the box's
// shadow on the floor under it.
static int s_cover3d;   // ui_from_config

static void cover_draw(IDirect3DTexture9 *tex, float x, float y, float w, float h, float r, D3DCOLOR color, int box)
{
    const float sw = w * 0.085f, fx = x + sw, farH = h * 0.03f;
    const int a = (int)(color >> 24), cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
    float q[8];
    if (!s_cover3d || !box)
    {
        UiImage(tex, x, y, w, h, r, color);
        return;
    }
    // The shadow: a soft one round the box, and a darker one where it stands.
    UiShadow(x + w * 0.04f, y + h * 0.06f, w * 0.98f, h * 0.98f, 8, 26, D3DCOLOR_ARGB(a * 90 / 255, 0, 0, 0));
    UiShadow(x + w * 0.08f, y + h - 6, w * 0.9f, 14, 7, 10, D3DCOLOR_ARGB(a * 150 / 255, 0, 0, 0));
    // The spine: its far (left) edge a little shorter, darker as it turns away.
    q[0] = x; q[1] = y + farH * 1.8f; q[2] = fx; q[3] = y;
    q[4] = x; q[5] = y + h - farH * 1.8f; q[6] = fx; q[7] = y + h;
    UiImageQuad(tex, q, 0.0f, 0.0f, 0.07f, 1.0f, D3DCOLOR_ARGB(a, cr * 150 / 255, cg * 150 / 255, cb * 150 / 255));
    {
        // Shade across it, deepest at the far edge (columns, so it follows the slant).
        int i;
        for (i = 0; i < 6; i++)
        {
            float u0 = i / 6.0f, u1 = (i + 1) / 6.0f, sx0 = x + sw * u0, sx1 = x + sw * u1;
            int al = (int)(a * (0.55f - 0.45f * u0) * 0.9f);
            UiStrip(sx0, y + farH * 1.8f * (1 - u0), y + h - farH * 1.8f * (1 - u0), sx1, y + farH * 1.8f * (1 - u1), y + h - farH * 1.8f * (1 - u1),
                    D3DCOLOR_ARGB(al, 0, 0, 0), D3DCOLOR_ARGB(al, 0, 0, 0));
        }
    }
    // The front: its right edge, further away, a little shorter.
    q[0] = fx; q[1] = y; q[2] = x + w; q[3] = y + farH;
    q[4] = fx; q[5] = y + h; q[6] = x + w; q[7] = y + h - farH;
    UiImageQuad(tex, q, 0.0f, 0.0f, 1.0f, 1.0f, color);
    // A faint shade towards the far right, the light down the near edge and across the top.
    UiStrip(x + w * 0.8f, y + farH * 0.8f, y + h - farH * 0.8f, x + w, y + farH, y + h - farH, D3DCOLOR_ARGB(a * 40 / 255, 0, 0, 0),
            D3DCOLOR_ARGB(a * 40 / 255, 0, 0, 0));
    UiLine(fx, y + 1, fx, y + h - 1, 1.2f, D3DCOLOR_ARGB(a * 70 / 255, 255, 255, 255));
    UiStrip(fx, y, y + h * 0.28f, x + w * 0.55f, y + farH * 0.55f, y + farH * 0.55f, D3DCOLOR_ARGB(a * 22 / 255, 255, 255, 255),
            D3DCOLOR_ARGB(0, 255, 255, 255));
}

// ---- "Now playing" (the pause menu's main page, main.cpp) ----
static std::string clean_name(const char *name, size_t len);   // the list's name of a file (below)
void UiMenuNowPlaying(const UiMenu *m, IDirect3DTexture9 *tex, int texW, int texH, const char *title, int box)
{
    const float x = 8 - s_menuSlide, y = 12, w = 690, h = UI_HEIGHT - 24, head = 190, foot = 78;
    const float room = h - head - 18 - foot;
    float rowH = m->count > 0 && room / m->count < 48 ? room / m->count : 48;
    float top = y + head + m->count * rowH + 22, bottom = y + h - foot - 22, cx = x + w * 0.5f;
    char head2[64], fit[160];
    if (bottom - top < 50) return;
    UiUpper(UI_BOLD, Tr(T_NOW_PLAYING), head2, sizeof(head2));
    UiText(UI_BOLD, 14, cx, top + 8, UI_COL_ACCENT_HI, head2, UI_CENTER);
    UiTextFit(UI_BOLD, 20, w - 132, title, fit, sizeof(fit));
    UiText(UI_BOLD, 20, cx, top + 34, UI_COL_TEXT, fit, UI_CENTER);
    if (tex && texW > 0 && texH > 0)
    {
        // The picture as large as the room under the name allows, its own proportions.
        float ph = bottom - (top + 56), pw = ph * texW / texH, px, py;
        if (pw > w - 132) { pw = w - 132; ph = pw * texH / texW; }
        if (ph < 24) return;
        px = cx - pw * 0.5f;
        py = top + 56 + (bottom - (top + 56) - ph) * 0.5f;
        UiShadow(px, py + 6, pw, ph, 10, 18, D3DCOLOR_ARGB(140, 0, 0, 0));
        cover_draw(tex, px, py, pw, ph, 10, D3DCOLOR_ARGB(255, 255, 255, 255), box);
        if (!s_cover3d || !box) UiRoundRing(px, py, pw, ph, 10, 1.0f, D3DCOLOR_ARGB(46, 255, 255, 255));
    }
}

void NowPlayingLoad(const char *romPath, char *title, int titleSize, u8 **rgba, int *w, int *h, int *box)
{
    H64GameInfo info;
    *rgba = NULL;
    if (ArtReadInfo(romPath, &info) && (info.title[0] || info.name[0]))
        strncpy(title, info.title[0] ? info.title : info.name, titleSize - 1);
    else
    {
        const char *base = strrchr(romPath, '\\'), *dot;
        std::string name;
        base = base ? base + 1 : romPath;
        dot = strrchr(base, '.');
        name = clean_name(base, dot ? (size_t)(dot - base) : strlen(base));
        strncpy(title, name.c_str(), titleSize - 1);
    }
    title[titleSize - 1] = 0;
    {
        int kind = ART_KIND_BOX;
        if (!ArtReadCover(romPath, ArtKindFor(romPath), rgba, w, h, &kind)) *rgba = NULL;
        *box = kind == ART_KIND_BOX;
    }
}

// The folders in use (Settings > Paths; paths_apply): the ROMs, the menus' music.
static char s_romDir[128] = "game:\\roms", s_musicDir[128] = "game:\\music";

static void paths_apply(const Config *c)
{
    strncpy(s_romDir, ConfigPath(c, CONFIG_PATH_ROMS, "game"), sizeof(s_romDir) - 1);
    strncpy(s_musicDir, ConfigPath(c, CONFIG_PATH_MUSIC, "game"), sizeof(s_musicDir) - 1);
    xb_audio_ui_folder(ConfigPath(c, CONFIG_PATH_SOUNDS, "game"));   // the user's own sounds, when there are some
    HiresSetFolder(ConfigPath(c, CONFIG_PATH_TEXTURES, "game"));       // the texture packs' folder
}

// The menus' look from *c (kept in range): the background effect, its
// colours, opacity and speed, the selection's colour, the wallpapers.
// ---- Wallpapers: game recommendations ----
// One of your games at a time behind the menus: its cover blurred over the whole
// screen, the cover itself big on the right, its name, year, genre and a few
// lines about it on the left. Another every wallpaper interval (30 s when it
// never changes), crossfading. Only games with a cover; read on this thread
// (the packed cover: one read).
struct RecShow { IDirect3DTexture9 *tex; int w, h; H64GameInfo info; float at; };
static std::vector<std::string> s_recPaths;   // the ROM list's games (RomBrowser)
static RecShow s_rec[2];                      // shown now, and the one fading out
static int s_recInterval = 30;
static float s_recNextAt;

static void rec_free(RecShow *r)
{
    if (r->tex) UiImageRelease(UiDevice(), r->tex);
    memset(r, 0, sizeof(*r));
}

static void rec_pick(void)
{
    int tries;
    for (tries = 0; tries < 12 && !s_recPaths.empty(); tries++)
    {
        const char *path = s_recPaths[(size_t)(rand() % (int)s_recPaths.size())].c_str();
        RecShow r;
        u8 *rgba = NULL;
        int kind;
        memset(&r, 0, sizeof(r));
        if (!ArtReadInfo(path, &r.info) || !ArtReadCover(path, ART_KIND_BOX, &rgba, &r.w, &r.h, &kind) || !rgba) { free(rgba); continue; }
        r.tex = UiImageCreate(UiDevice(), rgba, r.w, r.h);
        free(rgba);
        if (!r.tex) continue;
        rec_free(&s_rec[1]);
        s_rec[1] = s_rec[0];
        s_rec[0] = r;
        s_rec[0].at = UiTime();
        return;
    }
}

static void rec_draw_one(const RecShow *r, float a)
{
    const float cw = 520, ch = cw * r->h / (r->w ? r->w : 1), cx = UI_WIDTH - 90 - cw, cy = (UI_HEIGHT - ch) * 0.5f, tx = 90, tw = cx - 60 - tx;
    char line[160];
    float ty;
    if (a <= 0.01f || !r->tex) return;
    // The cover over the whole screen, enlarged and faint, then darkened towards the text.
    UiImage(r->tex, -80, -80, UI_WIDTH + 160, UI_HEIGHT + 160, 0.0f, D3DCOLOR_ARGB((int)(90 * a), 255, 255, 255));
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(150 * a), 8, 9, 14));
    UiShadow(cx, cy + 14, cw, ch, 22, 40, UiAlpha(UI_COL_SHADOW, a));
    UiImage(r->tex, cx, cy, cw, ch, 18.0f, D3DCOLOR_ARGB((int)(255 * a), 255, 255, 255));
    UiRoundRing(cx, cy, cw, ch, 18, 1.0f, D3DCOLOR_ARGB((int)(40 * a), 255, 255, 255));
    ty = 200;
    UiText(UI_BOLD, 18, tx, ty, UiAlpha(SEL_HI(255), a), Tr(T_REC_TITLE), UI_LEFT);
    ty += 52;
    ty += paragraph(UI_BOLD, 40, tx, ty - 22, tw, 2, UiAlpha(UI_COL_TEXT, a), r->info.title[0] ? r->info.title : r->info.name) + 6;
    _snprintf(line, sizeof(line) - 1, "%s%s%s", r->info.year, r->info.year[0] && r->info.genre[0] ? "  -  " : "", r->info.genre);
    line[sizeof(line) - 1] = 0;
    UiText(UI_REGULAR, 20, tx, ty + 6, UiAlpha(UI_COL_TEXT2, a), line, UI_LEFT);
    ty += 40;
    if (r->info.description[0]) paragraph(UI_REGULAR, 20, tx, ty, tw, 7, UiAlpha(UI_COL_TEXT2, a), r->info.description);
}

static int rec_layer(void)
{
    float now = UiTime(), f;
    if (s_recPaths.empty() || !UiDevice()) return 0;
    if (!s_rec[0].tex || now >= s_recNextAt)
    {
        rec_pick();
        s_recNextAt = now + (s_recInterval > 0 ? s_recInterval : 30);
    }
    if (!s_rec[0].tex) return 0;
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UI_COL_BG_BOTTOM);
    f = (now - s_rec[0].at) / 1.0f;   // a second's crossfade
    if (f > 1.0f) f = 1.0f;
    if (f < 1.0f) rec_draw_one(&s_rec[1], 1.0f - f);
    else if (s_rec[1].tex) rec_free(&s_rec[1]);
    rec_draw_one(&s_rec[0], f);
    return 1;
}

static void ui_from_config(const Config *c)
{
    int i;
    s_sel = c->selColor >= 0 && c->selColor < SEL_COLORS ? c->selColor : 0;
    s_cover3d = c->artKind == CONFIG_ART_BOX3D;
    s_gameBg = c->gameBackdrop;
    UiSetFont(c->uiFont);
    paths_apply(c);
    UiSetWallpapers(ConfigPath(c, CONFIG_PATH_WALLS, "game"), c->wallCustom, c->wallFile, c->wallInterval, c->wallTransition >= 0 && c->wallTransition < UI_TRANS_COUNT ? c->wallTransition : 0,
                    c->wallDim, c->wallFit, c->wallShuffle);
    s_recInterval = c->wallInterval;
    // The logo in the edges' own colour when one is chosen (Borders and panels > Border colour).
    if (c->borderColor > 0 && c->borderColor <= FX_PALETTE_COUNT)
        UiSetLogoColor(s_fxPalette[c->borderColor][0] << 16 | s_fxPalette[c->borderColor][1] << 8 | s_fxPalette[c->borderColor][2]);
    else
        UiSetLogoColor(-1);
    UiSetBackdropLayer(c->wallRecommend ? rec_layer : NULL);
    {
        // The edges: the selection's colour, or one of the swatches; the panels' body and corners.
        static const float rounds[4] = { 0.2f, 0.55f, 1.0f, 1.6f };
        int width = c->borderWidth < 0 ? 0 : c->borderWidth > 4 ? 4 : c->borderWidth;
        for (i = 0; i < 3; i++)
            s_edgeRgb[i] = c->borderColor > 0 && c->borderColor <= FX_PALETTE_COUNT ? s_fxPalette[c->borderColor][i] : s_selColors[s_sel][0][i];
        s_edgeOp = c->borderOpacity < 0 ? 0 : c->borderOpacity > 100 ? 100 : c->borderOpacity;
        s_edgeGlow = c->borderGlow >= 0 && c->borderGlow < GLOW_COUNT ? c->borderGlow : 0;
        if (c->panelColor > 0 && c->panelColor <= FX_PALETTE_COUNT)
            UiSetPanelColor(s_fxPalette[c->panelColor][0], s_fxPalette[c->panelColor][1], s_fxPalette[c->panelColor][2]);
        else UiSetPanelColor(-1, 0, 0);
        // The panel's own fine edge: as faint as before at 1 px, stronger when wider.
        UiSetPanelStyle(EDGE_COL(s_edgeOp * (width > 1 ? 1.4f : 0.4f)), (float)width, c->panelOpacity,
                        rounds[c->panelRound >= 0 && c->panelRound < 4 ? c->panelRound : 2]);
    }
    s_bgEffect = c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : BGFX_OFF;
    s_kodi = c->settingsView == 1;
    for (i = 0; i < 3; i++) s_fxColor[i] = c->fxColor[i] >= 0 && c->fxColor[i] <= FX_PALETTE_COUNT ? c->fxColor[i] : 0;
    s_fxOpacity = c->fxOpacity < 10 ? 10 : c->fxOpacity > 200 ? 200 : c->fxOpacity;
    s_fxSpeed = c->fxSpeed < 25 ? 25 : c->fxSpeed > 300 ? 300 : c->fxSpeed;
}
// The System page: what a controller unplugged in game does, the slot it saves to, the low battery
// warning, then CPU and RSP (both apply at the next game start, so the ROM list only shows them -
// last, so the rows before keep their place in game). B goes back, no row for it.
enum { SYS_PADLOSS = 0, SYS_AUTOSLOT, SYS_BATTERY, SYS_PERF, SYS_DEV, SYS_CPU, SYS_RSP, SYS_WIZARD, SYS_EXPLORER };   // SYS_CPU..SYS_WIZARD: ROM list only
// The Developer mode page (System > Developer mode, A): the mode on or off, and with it on the
// graphics error log (main.cpp ApplyGraphics: the renderer's gfxLog). B goes back, no row for it.
enum { DEV_GFXLOG = 0 };   // the System page's row turns the mode on and off
static int s_devPage;
static void settings_dev(UiMenu *m, const Config *c);
static void settings_perf(UiMenu *m, const Config *c);   // System > On-screen performance (below, with the other graphical pages)
static std::string s_browserSel;   // the ROM selected in the list when Settings opened (Game at start: that game)
static std::string clean_name(const char *name, size_t len);   // the list's name of a file (below)

// "Game at start"'s value: no, the last game, or the chosen game's name (cut to fit).
static void start_rom_text(const Config *c, char *out, size_t size)
{
    if (c->startRomMode == 1) _snprintf(out, size - 1, "< %s >", Tr(T_START_LAST));
    else if (c->startRomMode == 2 && c->startRom[0])
    {
        const char *base = strrchr(c->startRom, '\\'), *dot;
        std::string name;
        char fit[48];
        base = base ? base + 1 : c->startRom;
        dot = strrchr(base, '.');
        name = clean_name(base, dot ? (size_t)(dot - base) : strlen(base));
        UiTextFit(UI_BOLD, 21, 230, name.c_str(), fit, sizeof(fit));
        _snprintf(out, size - 1, "< %s >", fit);
    }
    else _snprintf(out, size - 1, "< %s >", Tr(T_OFF));
    out[size - 1] = 0;
}
// The Sound page: Volumen (opens the bars page), Margen de audio, mono/stereo, then - from the ROM
// list only, last so the rows before keep their place in game - the menus' button sounds and music.
// B goes back, no row for it.
enum { SND_VOLUME = 0, SND_AUDIO, SND_MONO, SND_UISOUNDS, SND_SFXSTYLE, SND_MUSIC };
static const int s_sfxStyleLabels[XB_SFX_STYLE_COUNT] = { T_STYLE_GLASS, T_STYLE_SOFT, T_STYLE_RETRO, T_STYLE_DIGITAL };
#define MUSIC_DIR s_musicDir   // the menus' music: every .mp3/.wav in it (Settings > Paths)
enum { G_PRESET = 0, G_FILTER, G_HIRES, G_SMOOTH, G_SHARPEN, G_BLUR, G_DEFLICKER, G_SCREEN };   // the Graphics page; B goes back, no row for it
enum { SCR_RES = 0, SCR_RES169, SCR_STRETCH, SCR_CAL, SCR_BRIGHT, SCR_COLOR, SCR_VSYNC };   // anti-flicker and the screen effect: Graphics
// Brightness in the order the row steps through it (Config::brightness: 0 normal, 1-2 lighter, 3-4 darker).
static const int s_brightOrder[5] = { 4, 3, 0, 1, 2 };
static const int s_colorStyleLabels[8] = { T_NORMAL, T_CS_SATURATED, T_CS_VIVID, T_CS_BW, T_CS_SEPIA, T_CS_WARM, T_CS_COOL, T_CS_GREEN };   // the Screen page (SCR_CAL opens the calibration page below); B goes back, no row for it
enum { CAL_SIZE = 0, CAL_EDGES };   // the Image calibration page, opened from Screen > SCR_CAL; B goes back, no row for it


// Graphics presets (the user's table): texture filter, smoothing, sharpening,
// blur, screen effect. The resolution stays as it is.
struct GfxPreset { int name; int texFilter, smooth, sharpen, blur, screen; };
static const GfxPreset s_presets[4] = {
    { T_PRESET_ORIGINAL, 2, 0, 0, 0, 0 },   // nearest, nothing added
    { T_PRESET_ENHANCED, 1, 1, 1, 0, 0 },   // bilinear (the default filter), light sharpening
    { T_PRESET_SMOOTH, 1, 1, 1, 0, 5 },     // bilinear, light sharpening, light scanlines
    { T_PRESET_CRT, 1, 0, 0, 0, 2 },        // bilinear, CRT: scanlines and glow
};

static int s_gfxPick = -1;   // Graphics > Preset picked (0 Recommended, 1.. s_presets), -1: as the settings say

// Recommended (the assistant's Picture page's): 1366x768 16:9, bilinear, brightness 50 %, colours as they are, no stretch.
static int gfx_recommended(const Config *c)
{
    return c->resWide && c->res169 == 4 && c->texFilter == 1 && c->brightLevel == 50 && c->colorStyle == 0 && !c->stretch;
}

static void gfx_apply_recommended(Config *c)
{
    c->resWide = 1; c->res169 = 4;
    c->texFilter = 1;
    c->brightLevel = 50;
    c->colorStyle = 0;
    c->stretch = 0;
}

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
static void settings_edges(UiMenu *m, const Config *c);
static void edges_draw(const UiMenu *m);
static int s_interfacePage;
static int s_soundPage;
static int s_systemPage;
static int s_perGame;
static char s_langPath[160];   // where a language change is kept (SettingsSetLangFile)
// The save rows: in game only, after Controls - "this game only" (when the
// game has a profile), then "all games". From the ROM list there are none:
// what is changed there is kept on leaving (settings_loop). Their places,
// set by settings_main (-1: not shown).
static int s_rowSaveGame = -1, s_rowSaveAll = -1;

static const char *onoff(int v) { return Tr(v ? T_ON : T_OFF); }
// The value of an on/off row in a list: drawn as a switch (draw_value).
static const char *toggle(int v) { return v ? "\x01" : "\x02"; }

// The resolution rows, one per list (4:3, 16:9): its value, or "Off" while the
// other list's is used. Stepping goes through "Off" too: picking a size in one
// list turns the other off, "Off" in one turns the other on.
static void res_value(char *out, const Config *c, int wide)
{
    u32 w, h;
    if ((c->resWide != 0) != wide) { sprintf(out, "< %s >", Tr(T_RES_OFF)); return; }
    ConfigResSize(wide, wide ? c->res169 : c->res43, &w, &h);
    sprintf(out, "< %ux%u >", w, h);
}

static void res_step(Config *c, int wide, int dir)
{
    int n = ConfigResCount(wide), *idx = wide ? &c->res169 : &c->res43;
    int pos = (c->resWide != 0) == wide ? (*idx < 0 || *idx >= n ? 0 : *idx) : n;   // n: Off, at the end of the list
    pos = (pos + dir + n + 1) % (n + 1);
    if (pos == n) c->resWide = !wide;
    else { c->resWide = wide; *idx = pos; }
}

// V-Sync: off, on (a late frame goes at once), aggressive (every frame waits).
// The value of a 0..100 row drawn as a bar (draw_value), into `out`.
// The sharpening and blur steps (presets compare them) from the bars.
static void levels_to_steps(Config *c)
{
    c->sharpen = c->sharpLevel <= 0 ? 0 : c->sharpLevel <= 50 ? 1 : 2;
    c->blur = c->blurLevel <= 0 ? 0 : c->blurLevel <= 50 ? 1 : 2;
}
static void bar_value(char *out, int level) { sprintf(out, "\x03%d", level < 0 ? 0 : level > 100 ? 100 : level); }

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
    if (s_imagesPage)
    {
        images_values(m, c);
        return;
    }
    if (s_pathsPage)
    {
        paths_values(m, c);
        return;
    }
    if (s_interfacePage)
    {
        sprintf(m->value[I_DESIGN], "< %s >", Tr(s_designLabels[c->browserDesign >= 0 && c->browserDesign < BD_COUNT ? c->browserDesign : 0]));
        sprintf(m->value[I_INTRO], "%s", toggle(c->intro));
        sprintf(m->value[I_SETVIEW], "< %s >", Tr(c->settingsView ? T_VIEW_TILES : T_VIEW_LIST));
        // Background effect and Wallpapers show nothing here: both are chosen on their own pages (A), where the
        // choice shows. Other rows that open a page show their value plainly, without the "< >" of one changed in place.
        sprintf(m->value[I_FONT], "< %s >", UiFontName(c->uiFont));
        sprintf(m->value[I_SELCOLOR], "< %s >", Tr(s_selLabels[c->selColor >= 0 && c->selColor < SEL_COLORS ? c->selColor : 0]));
        sprintf(m->value[I_SAVER], "%s", Tr(c->saverMode == SAVER_EFFECT ? T_BG_EFFECT : c->saverMode == SAVER_WALL ? T_WALLPAPERS :
                                                c->saverMode == SAVER_LOGO ? T_SAVER_LOGO : c->saverMode == SAVER_CLOCK ? T_SAVER_CLOCK : T_OFF));
        sprintf(m->value[I_LANGUAGE], "%s", Tr(T_LANG_NAME));   // A: the list of languages
        if (s_browser) sprintf(m->value[I_TRANSDESC], "%s", toggle(c->descTranslate));
        if (s_browser) sprintf(m->value[I_GAMEBG], "%s", toggle(c->gameBackdrop));
        if (s_browser) start_rom_text(c, m->value[I_STARTROM], sizeof(m->value[I_STARTROM]));
        return;
    }
    if (s_devPage)
    {
        sprintf(m->value[DEV_GFXLOG], "%s", toggle(c->gfxLog));
        return;
    }
    if (s_systemPage)
    {
        sprintf(m->value[SYS_DEV], "%s", toggle(c->devMode));   // opens its page: its switch shows the mode
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
        sprintf(m->value[SND_MONO], "< %s >", Tr(c->audioMono ? T_AUDIO_MONO : T_AUDIO_STEREO));
        if (!s_browser) return;   // the menus' sounds and music: ROM list only
        sprintf(m->value[SND_UISOUNDS], "%s", toggle(c->uiSounds));
        sprintf(m->value[SND_SFXSTYLE], "< %s >", Tr(s_sfxStyleLabels[c->uiSoundStyle >= 0 && c->uiSoundStyle < XB_SFX_STYLE_COUNT ? c->uiSoundStyle : 0]));
        if (c->musicMode == XB_MUSIC_OFF) sprintf(m->value[SND_MUSIC], "< %s >", Tr(T_OFF));
        else if (!xb_audio_music_scan(MUSIC_DIR)) sprintf(m->value[SND_MUSIC], "< %s >", Tr(T_MUSIC_NONE));
        else if (c->musicMode == XB_MUSIC_TRACK && c->musicTrack[0])
        {
            // The track's name without its extension, cut to fit the row.
            char name[96], fit[64], *dot;
            strncpy(name, c->musicTrack, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            if ((dot = strrchr(name, '.')) != NULL) *dot = 0;
            UiTextFit(UI_BOLD, 21, 220, name, fit, sizeof(fit) - 6);
            sprintf(m->value[SND_MUSIC], "< %s >", fit);
        }
        else sprintf(m->value[SND_MUSIC], "< %s >", Tr(c->musicMode == XB_MUSIC_RANDOM ? T_MUSIC_RANDOM : T_MUSIC_ORDER));
        return;
    }
    if (s_calPage)
    {
        sprintf(m->value[CAL_SIZE], "< %d%% >", c->zoom);
        m->value[CAL_EDGES][0] = 0;   // opens its screen
        return;
    }
    if (s_screenPage)
    {
        const char *scr[9] = { Tr(T_SCREEN_NONE), Tr(T_SCANLINES), Tr(T_CRT), Tr(T_CRT_CURVED), Tr(T_LCD_GRID), Tr(T_LIGHT_SCANLINES), Tr(T_SCR_VIGNETTE), Tr(T_SCR_APERTURE), Tr(T_SCR_DOTS) };
        const char *br[5] = { Tr(T_NORMAL), Tr(T_LIGHTER), Tr(T_MUCH_LIGHTER), Tr(T_DARKER), Tr(T_MUCH_DARKER) };
        res_value(m->value[SCR_RES], c, 0);
        res_value(m->value[SCR_RES169], c, 1);
        sprintf(m->value[SCR_STRETCH], "%s", toggle(c->stretch));
        bar_value(m->value[SCR_BRIGHT], c->brightLevel);
        (void)br;
        sprintf(m->value[SCR_COLOR], "< %s >", Tr(s_colorStyleLabels[c->colorStyle < 0 || c->colorStyle > 7 ? 0 : c->colorStyle]));
        sprintf(m->value[SCR_VSYNC], "%s", toggle(c->vsync == 2));   // a switch: off, or the TV always waited for (the one-time "aggressive")
        (void)scr;
        return;
    }
    if (s_graphicsPage)
    {
        const char *filt[3] = { Tr(T_FILTER_3POINT), Tr(T_FILTER_BILINEAR), Tr(T_FILTER_NEAREST) };
        const char *lvl[3] = { Tr(T_OFF), Tr(T_LOW), Tr(T_HIGH) };
        const char *blr[3] = { Tr(T_OFF), Tr(T_SOFT), Tr(T_STRONG) };
        int pr = s_gfxPick >= 0 ? s_gfxPick : current_preset(c) >= 0 ? current_preset(c) + 1 : gfx_recommended(c) ? 0 : -1;
        sprintf(m->value[G_PRESET], "< %s >", pr < 0 ? Tr(T_CUSTOM) : pr == 0 ? Tr(T_PRESET_RECOMMENDED) : Tr(s_presets[pr - 1].name));
        sprintf(m->value[G_FILTER], "< %s >", filt[c->texFilter < 0 || c->texFilter > 2 ? 1 : c->texFilter]);
        {
            // Off, or the chosen (or first) pack's file name without ".hts", with a
            // (i/N) counter when the game has more than one - hiTexFile picks which
            // (see HiresScanPacks/HiresOpen); the list itself always matches whatever
            // ROM is selected in the browser or running in game.
            int hn = HiresPackCount(), hi = 0, k;
            if (hn <= 1) sprintf(m->value[G_HIRES], "%s", toggle(c->hiTex && hn));   // on or off: a switch
            else if (!c->hiTex) sprintf(m->value[G_HIRES], "< %s >", Tr(T_OFF));
            else
            {
                // The name cut with an ellipsis to what fits the row beside its label
                // (the counter always shown in full).
                char name[128], fit[48], count[16];
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
                count[0] = 0;
                if (hn > 1) sprintf(count, " (%d/%d)", hi + 1, hn);
                UiTextFit(UI_BOLD, 21, 290 - UiTextWidth(UI_BOLD, 21, count), name, fit, sizeof(fit));
                sprintf(m->value[G_HIRES], "< %s%s >", fit, count);
            }
        }
        sprintf(m->value[G_SMOOTH], "%s", toggle(c->smooth));
        bar_value(m->value[G_SHARPEN], c->sharpLevel);
        bar_value(m->value[G_BLUR], c->blurLevel);
        sprintf(m->value[G_DEFLICKER], "%s", toggle(c->deflicker));
        {
            static const int scrLabels[9] = { T_SCREEN_NONE, T_SCANLINES, T_CRT, T_CRT_CURVED, T_LCD_GRID, T_LIGHT_SCANLINES, T_SCR_VIGNETTE, T_SCR_APERTURE, T_SCR_DOTS };
            sprintf(m->value[G_SCREEN], "< %s >", Tr(scrLabels[c->screen < 0 || c->screen > 8 ? 0 : c->screen]));
        }
        (void)lvl; (void)blr;
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
    s_devPage = 0;
    s_interfacePage = 0;
    s_catsPage = s_catEditPage = 0;
    s_saverPage = s_imagesPage = s_pathsPage = 0;
    s_soundPage = 0;
    s_systemPage = 0;
    static const unsigned char icons[] = { UI_ICON_GRAPHICS, UI_ICON_SCREEN, UI_ICON_CONTROLS, UI_ICON_SOUND, UI_ICON_SYSTEM };
    UiMenuClear(m, Tr(T_SETTINGS), Tr(T_FOOT_SELECT));   // just "Settings", from the ROM list too
    UiMenuAdd(m, Tr(T_GRAPHICS), "");
    UiMenuAdd(m, Tr(T_SCREEN), "");
    UiMenuAdd(m, Tr(T_CONTROLS), "");
    UiMenuAdd(m, Tr(T_SOUND), "");
    UiMenuAdd(m, Tr(T_SYSTEM), "");
    UiMenuIcons(m, icons, sizeof(icons));
    s_rowSaveGame = s_rowSaveAll = s_rowAbout = -1;
    if (s_browser)
    {
        // Not in game: the menus' look cannot be seen there, and the folders are the ROM list's.
        m->icon[UiMenuAdd(m, Tr(T_INTERFACE), "")] = UI_ICON_INTERFACE;
        m->icon[UiMenuAdd(m, Tr(T_PATHS), "")] = UI_ICON_LOAD;
        m->icon[s_rowAbout = UiMenuAdd(m, Tr(T_ABOUT), "")] = UI_ICON_INFO;
    }
    else
    {
        // In game: save for this game, or for all of them (main.cpp asks first).
        if (s_perGame) m->icon[s_rowSaveGame = UiMenuAdd(m, Tr(T_SAVE_GAME), "")] = UI_ICON_SAVE;
        m->icon[s_rowSaveAll = UiMenuAdd(m, Tr(T_SAVE_ALL), "")] = UI_ICON_SAVE;
    }
    settings_values(m, c);   // no Back row: B goes back
}

static void settings_interface(UiMenu *m, const Config *c)
{
    s_interfacePage = 1;
    s_catsPage = s_catEditPage = 0;   // back from Categories: B must reach the main page, not Categories again
    s_saverPage = s_imagesPage = 0;   // the same from Screen saver and Game images
    // The menu's own state (title, rows, count) first, as a tight, I/O-free
    // sequence - SkinScan's directory listing (disk I/O, unlike every other
    // field here) runs only after, so nothing can observe *m mid-rebuild
    // while this thread is possibly waiting on the filesystem.
    UiMenuClear(m, Tr(T_INTERFACE), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_LANGUAGE), "");
    UiMenuAdd(m, Tr(T_SETVIEW), "");
    UiMenuAdd(m, Tr(T_FONT), "");
    UiMenuAdd(m, Tr(T_INTRO_WELCOME), "");
    UiMenuAdd(m, Tr(T_SEL_COLOR), "");
    UiMenuAdd(m, Tr(T_BORDERS), "");
    UiMenuAdd(m, Tr(T_WALLPAPERS), "");
    UiMenuAdd(m, Tr(T_BG_EFFECT), "");
    UiMenuAdd(m, Tr(T_SAVER), "");
    UiMenuAdd(m, Tr(T_DESIGN), "");
    if (s_browser)
    {
        UiMenuAdd(m, Tr(T_GAME_IMAGES), "");   // opens its page: no value shown
        UiMenuAdd(m, Tr(T_TRANSLATE_DESC), "");
        UiMenuAdd(m, Tr(T_CATEGORIES), "");
        UiMenuAdd(m, Tr(T_START_ROM), "");
        UiMenuAdd(m, Tr(T_GAME_BG), "");
    }
    {
        static const unsigned char icons[] = { UI_ICON_GLOBE, UI_ICON_LIST, UI_ICON_SLIDERS, UI_ICON_PALETTE, UI_ICON_EYE, UI_ICON_INTERFACE,
                                               UI_ICON_FRAME, UI_ICON_SUN, UI_ICON_CLOCK, UI_ICON_LAYOUT, UI_ICON_LOAD, UI_ICON_GLOBE, UI_ICON_LIST,
                                               UI_ICON_PLAY, UI_ICON_FRAME };
        UiMenuIcons(m, icons, sizeof(icons));
    }
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
    if (m->sel == I_INTRO && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->intro = !c->intro;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_SETVIEW && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->settingsView = !c->settingsView;
        ui_from_config(c);   // the page itself shows the new style at once
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_FONT && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // In place: the whole menu shows the new letters at once.
        int n = UiFontCount();
        c->uiFont = ((c->uiFont >= 0 && c->uiFont < n ? c->uiFont : 0) + (dir ? dir : 1) + n) % n;
        ui_from_config(c);
        settings_interface(m, c);   // labels cut to fit are cut again in the new letters
        m->sel = I_FONT;
        return SET_CHANGED;
    }
    if (m->sel == I_SELCOLOR && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        if (!dir) dir = 1;
        c->selColor = ((c->selColor >= 0 && c->selColor < SEL_COLORS ? c->selColor : 0) + dir + SEL_COLORS) % SEL_COLORS;
        ui_from_config(c);   // the selection shows it at once
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_BORDERS && (down & XINPUT_GAMEPAD_A))
    {
        settings_borders(m, c);
        return SET_NONE;
    }
    if (m->sel == I_WALLPAPERS && (down & XINPUT_GAMEPAD_A))
    {
        settings_wall(m, c);
        return SET_NONE;
    }
    if (m->sel == I_SAVER && (down & XINPUT_GAMEPAD_A))
    {
        settings_saver(m, c);
        return SET_NONE;
    }
    if (m->sel == I_SAVER && dir)
    {
        c->saverMode = ((c->saverMode >= 0 && c->saverMode < SAVER_COUNT ? c->saverMode : 0) + dir + SAVER_COUNT) % SAVER_COUNT;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == I_EFFECT && (down & XINPUT_GAMEPAD_A))
    {
        settings_fx(m, c);   // its colours, opacity and speed (the page below)
        return SET_NONE;
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
    if (s_browser && m->sel == I_CATS && (down & XINPUT_GAMEPAD_A))
    {
        settings_cats(m, c, -1);
        return SET_NONE;
    }
    if (s_browser && m->sel == I_STARTROM && s_interfacePage && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // No, the last game played, or the game selected in the list (when there is one).
        int states = s_browserSel.empty() ? 2 : 3, cur = c->startRomMode;
        if (!dir) dir = 1;
        if (cur == 2 && states == 2) cur = 0;
        cur = (cur + dir + states) % states;
        c->startRomMode = cur;
        if (cur == 2)
        {
            strncpy(c->startRom, s_browserSel.c_str(), sizeof(c->startRom) - 1);
            c->startRom[sizeof(c->startRom) - 1] = 0;
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (s_browser && m->sel == I_TRANSDESC && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->descTranslate = !c->descTranslate;
        if (!s_kodiSim) ArtSetLang(art_lang(c));   // the descriptions shown again in it
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (s_browser && m->sel == I_GAMEBG && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->gameBackdrop = !c->gameBackdrop;
        s_gameBg = c->gameBackdrop;   // the list behind shows it at once
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (s_browser && m->sel == I_IMAGES && (down & XINPUT_GAMEPAD_A))
    {
        settings_images(m, c);
        return SET_NONE;
    }
    return SET_NONE;
}

// ---- Game images page (Interface > Game images, A): which picture of each
// game the ROM list shows - its box flat or as a box seen at an angle (drawn
// here from the flat one), a picture from the game, its title screen, its
// logo (libretro-thumbnails; the box shows until that one is downloaded, or
// when there is none) - and whether missing pictures are downloaded.
enum { IM_KIND = 0, IM_DOWNLOAD, IM_EXTRAS, IM_BACKGROUND };
static const int s_artKindLabels[CONFIG_ART_COUNT] = { T_KIND_BOX2D, T_KIND_BOX3D, T_KIND_SNAP, T_KIND_TITLE, T_KIND_LOGO };

// The worker's kind of picture for Config::artKind (the 3D box is the box, drawn otherwise).
static int art_kind_of(const Config *c)
{
    static const int kinds[CONFIG_ART_COUNT] = { ART_KIND_BOX, ART_KIND_BOX, ART_KIND_SNAP, ART_KIND_TITLE, ART_KIND_LOGO };
    return kinds[c->artKind >= 0 && c->artKind < CONFIG_ART_COUNT ? c->artKind : 0];
}

static void images_values(UiMenu *m, const Config *c)
{
    sprintf(m->value[IM_KIND], "< %s >", Tr(s_artKindLabels[c->artKind >= 0 && c->artKind < CONFIG_ART_COUNT ? c->artKind : 0]));
    sprintf(m->value[IM_DOWNLOAD], "%s", toggle(c->artDownload));
    sprintf(m->value[IM_EXTRAS], "%s", toggle(c->artDownload && c->artExtras));
    sprintf(m->value[IM_BACKGROUND], "%s", toggle(c->artDownload && c->artBackground));
}

static void settings_images(UiMenu *m, const Config *c)
{
    static const unsigned char icons[] = { UI_ICON_FRAME, UI_ICON_LOAD, UI_ICON_LOAD, UI_ICON_LOAD };
    s_interfacePage = 0;
    s_imagesPage = 1;
    UiMenuClear(m, Tr(T_GAME_IMAGES), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_IMG_KIND), "");
    UiMenuAdd(m, Tr(T_ART_DOWNLOADS), "");
    UiMenuAdd(m, Tr(T_ART_EXTRAS), "");
    UiMenuAdd(m, Tr(T_ART_BACKGROUND), "");
    UiMenuIcons(m, icons, sizeof(icons));
    images_values(m, c);
}

static int images_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_IMAGES;
        return SET_NONE;
    }
    if (!dir && !(down & XINPUT_GAMEPAD_A)) return SET_NONE;
    if (m->sel == IM_KIND)
    {
        c->artKind = ((c->artKind >= 0 && c->artKind < CONFIG_ART_COUNT ? c->artKind : 0) + (dir ? dir : 1) + CONFIG_ART_COUNT) % CONFIG_ART_COUNT;
        ui_from_config(c);   // the 3D box at once; another picture when the list is back
        images_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == IM_BACKGROUND)
    {
        // The downloads of every game going on during a game (with the downloads on).
        if (!c->artDownload) return SET_ART_ASK;
        c->artBackground = !c->artBackground;
        images_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == IM_EXTRAS)
    {
        // The rest of the pictures: with the box art downloads on (turning it on here asks for those first).
        if (!c->artDownload) return SET_ART_ASK;   // asks for these (settings_loop), the box art coming with them
        c->artExtras = !c->artExtras;
        images_values(m, c);
        return SET_CHANGED;
    }
    if (!c->artDownload) return SET_ART_ASK;   // turning downloads on asks first (settings_loop)
    c->artDownload = 0;
    images_values(m, c);
    return SET_CHANGED;
}

// ---- Screen saver page (Interface > Screen saver, A): what it shows, after
// how long without a button in the ROM list, and a preview (saver_run).
enum { SV_MODE = 0, SV_TIME, SV_PREVIEW };
static const int s_saverMins[] = { 1, 2, 3, 5, 10, 15, 30 };
static const int s_saverLabels[SAVER_COUNT] = { T_OFF, T_BG_EFFECT, T_WALLPAPERS, T_SAVER_LOGO, T_SAVER_CLOCK };

static int saver_minutes(const Config *c) { return c->saverTime < 1 ? 1 : c->saverTime > 60 ? 60 : c->saverTime; }

static void saver_values(UiMenu *m, const Config *c)
{
    sprintf(m->value[SV_MODE], "< %s >", Tr(s_saverLabels[c->saverMode >= 0 && c->saverMode < SAVER_COUNT ? c->saverMode : 0]));
    sprintf(m->value[SV_TIME], "< %d min >", saver_minutes(c));
}

static void settings_saver(UiMenu *m, const Config *c)
{
    static const unsigned char icons[] = { UI_ICON_SCREEN, UI_ICON_CLOCK, UI_ICON_PLAY };
    s_interfacePage = 0;
    s_saverPage = 1;
    UiMenuClear(m, Tr(T_SAVER), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_SAVER_MODE), "");
    UiMenuAdd(m, Tr(T_SAVER_TIME), "");
    UiMenuAdd(m, Tr(T_SAVER_PREVIEW), "");
    UiMenuIcons(m, icons, sizeof(icons));
    saver_values(m, c);
}

static int saver_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0, k, cur = 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_SAVER;
        return SET_NONE;
    }
    if (m->sel == SV_PREVIEW) return (down & XINPUT_GAMEPAD_A) ? SET_SAVER : SET_NONE;
    if (!dir && !(down & XINPUT_GAMEPAD_A)) return SET_NONE;
    if (!dir) dir = 1;
    if (m->sel == SV_MODE) c->saverMode = ((c->saverMode >= 0 && c->saverMode < SAVER_COUNT ? c->saverMode : 0) + dir + SAVER_COUNT) % SAVER_COUNT;
    else
    {
        const int n = sizeof(s_saverMins) / sizeof(s_saverMins[0]);
        for (k = 0; k < n; k++)
            if (s_saverMins[k] <= c->saverTime) cur = k;
        c->saverTime = s_saverMins[(cur + dir + n) % n];
    }
    saver_values(m, c);
    return SET_CHANGED;
}

// ---- System page: CPU and RSP (recompiler/interpreter, HLE/LLE) - both
// apply at the next game start, not live; what a controller unplugged during
// a game does (off, save and pause, pause: main.cpp) and the slot that save
// goes to (kept for it: manual saves skip it); the low battery warning.
static void settings_system(UiMenu *m, const Config *c)
{
    s_systemPage = 1;
    UiMenuClear(m, Tr(T_SYSTEM), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PAD_LOSS), "");
    UiMenuAdd(m, Tr(T_AUTOSAVE_SLOT), "");
    UiMenuAdd(m, Tr(T_BATTERY_WARN), "");
    UiMenuAdd(m, Tr(T_PERF_OVERLAY), "");   // opens its page: no value shown
    UiMenuAdd(m, Tr(T_DEV_MODE), "");
    if (s_browser)
    {
        // For the next game only: nothing to change in the one running.
        UiMenuAdd(m, Tr(T_CPU_NEXT), "");
        UiMenuAdd(m, Tr(T_RSP_NEXT), "");
        UiMenuAdd(m, Tr(T_WIZARD), "");   // the first start's set-up again (settings_loop)
        UiMenuAdd(m, Tr(T_EXPLORER), "");   // after a warning (settings_loop)
    }
    {
        static const unsigned char icons[] = { UI_ICON_CONTROLS, UI_ICON_SAVE, UI_ICON_BATTERY, UI_ICON_CHART, UI_ICON_SLIDERS, UI_ICON_SYSTEM,
                                               UI_ICON_SYSTEM, UI_ICON_SLIDERS, UI_ICON_LIST };
        UiMenuIcons(m, icons, sizeof(icons));
    }
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
    if (s_browser && m->sel == SYS_WIZARD && (down & XINPUT_GAMEPAD_A)) return SET_WIZARD;
    if (s_browser && m->sel == SYS_EXPLORER && (down & XINPUT_GAMEPAD_A)) return SET_EXPLORER;
    if (m->sel == SYS_PERF && (down & XINPUT_GAMEPAD_A))
    {
        settings_perf(m, c);
        return SET_NONE;
    }
    if (m->sel == SYS_DEV && dir)
    {
        // Its switch: on or off here; A opens its options (turning it on first when off).
        c->devMode = !c->devMode;
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SYS_DEV && (down & XINPUT_GAMEPAD_A))
    {
        if (!c->devMode)
        {
            c->devMode = 1;
            settings_dev(m, c);
            return SET_CHANGED;
        }
        settings_dev(m, c);
        return SET_NONE;
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
    m->count = s_browser ? V_COUNT : V_MUSIC;
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
        int *v = m->sel == V_MUSIC ? &c->volumeMusic : m->sel == V_SFX ? &c->volumeSfx : &c->volumeGame;
        *v += dir * 5;
        if (*v < 0) *v = 0;
        if (*v > 100) *v = 100;
        if (m->sel == V_MUSIC) xb_audio_set_music_volume(c->volumeMusic);
        else if (m->sel == V_SFX) xb_audio_set_sfx_volume(c->volumeSfx);
        else xb_audio_set_game_volume(c->volumeGame);
        return SET_CHANGED;
    }
    return SET_NONE;
}

// One bar: its label, its percentage, and the fill gliding to the new value (full at maxPercent).
static void volume_bar_draw(float x, float y, float w, const char *label, int percent, int maxPercent, int selected, UiAnim *fill)
{
    const float barH = 28, barY = y + 34;
    char pct[16];
    float fillW, shown = anim_step_rate(fill, (float)percent, 0, 12.0f, 0.05f);
    UiText(UI_BOLD, 20, x, y, selected ? UI_COL_TEXT : UI_COL_TEXT2, label, UI_LEFT);
    _snprintf(pct, sizeof(pct) - 1, "%d%%", (int)(shown + 0.5f));
    pct[sizeof(pct) - 1] = 0;
    UiText(UI_BOLD, 20, x + w, y, selected ? UI_COL_TEXT : UI_COL_TEXT2, pct, UI_RIGHT);
    UiRoundRect(x, barY, w, barH, barH * 0.5f, D3DCOLOR_ARGB(255, 34, 36, 47), D3DCOLOR_ARGB(255, 27, 29, 38));
    fillW = w * (shown / maxPercent);
    if (fillW > 0.5f)
    {
        if (fillW < barH) fillW = barH;   // keeps the rounded cap from looking clipped at tiny values
        UiRoundRect(x, barY, fillW, barH, barH * 0.5f, SEL_HI(255), SEL_LO(255));
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
    static UiAnim fill[V_COUNT];
    const float w = 660, foot = 78, h = 100 + m->count * 90 + 18 + foot;
    const float x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
    const Config *c = s_volumeCfg;
    UiDim();
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, m->title, UI_LEFT);
    volume_bar_draw(x + 44, y + 120, w - 88, Tr(T_VOLUME_GAME), c ? c->volumeGame : 100, 100, m->sel == V_GAME, &fill[V_GAME]);
    volume_bar_draw(x + 44, y + 210, w - 88, Tr(T_UI_SOUNDS), c ? c->volumeSfx : 100, 100, m->sel == V_SFX, &fill[V_SFX]);
    if (m->count > V_MUSIC)
        volume_bar_draw(x + 44, y + 300, w - 88, Tr(T_VOLUME_MUSIC), c ? c->volumeMusic : 100, 100, m->sel == V_MUSIC, &fill[V_MUSIC]);
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- Background effect page (Interface > Background effect, A): the effect,
// up to three colours picked from a row of swatches, its opacity and speed as
// bars, and a reset. Drawn as a panel on the left with no veil, so the effect
// itself shows the changes at once beside it (fxpage_draw).
enum { FXP_EFFECT = 0, FXP_COLOR1, FXP_COLOR2, FXP_COLOR3, FXP_OPACITY, FXP_SPEED, FXP_RESET, FXP_COUNT };
static const Config *s_fxCfg;

static void settings_fx(UiMenu *m, const Config *c)
{
    s_interfacePage = 0;
    UiMenuClear(m, Tr(T_BG_EFFECT), Tr(T_FOOT_SELECT));
    m->page = UI_PAGE_FX;
    m->count = FXP_COUNT;
    m->sel = 0;
    s_fxCfg = c;
}

static int fx_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    int a = (down & XINPUT_GAMEPAD_A) != 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_EFFECT;
        return SET_NONE;
    }
    if (!dir && !a) return SET_NONE;
    switch (m->sel)
    {
    case FXP_EFFECT:
        c->bgEffect = ((c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : 0) + (dir ? dir : 1) + BGFX_COUNT) % BGFX_COUNT;
        break;
    case FXP_COLOR1: case FXP_COLOR2: case FXP_COLOR3:
    {
        int *v = &c->fxColor[m->sel - FXP_COLOR1];
        *v = ((*v >= 0 && *v <= FX_PALETTE_COUNT ? *v : 0) + (dir ? dir : 1) + FX_PALETTE_COUNT + 1) % (FX_PALETTE_COUNT + 1);
        break;
    }
    case FXP_OPACITY:
        if (!dir) return SET_NONE;
        c->fxOpacity += dir * 10;
        if (c->fxOpacity < 10) c->fxOpacity = 10;
        if (c->fxOpacity > 200) c->fxOpacity = 200;
        break;
    case FXP_SPEED:
        if (!dir) return SET_NONE;
        c->fxSpeed += dir * 25;
        if (c->fxSpeed < 25) c->fxSpeed = 25;
        if (c->fxSpeed > 300) c->fxSpeed = 300;
        break;
    case FXP_RESET:
        if (!a) return SET_NONE;
        c->fxColor[0] = c->fxColor[1] = c->fxColor[2] = 0;
        c->fxOpacity = 100;
        c->fxSpeed = 100;
        break;
    }
    ui_from_config(c);   // shows at once beside the page
    return SET_CHANGED;
}

// One colour's row of swatches: "none" (a crossed-out ring) then the palette, the chosen one ringed.
static void fx_swatches(float x, float cy, int chosen, int selected)
{
    const float r = 12, step = 34, rr = r + 4;
    int i;
    for (i = 0; i <= FX_PALETTE_COUNT; i++)
    {
        float sx = x + r + i * step;
        if (!i)
        {
            int k;
            UiCircle(sx, cy, r, D3DCOLOR_ARGB(255, 34, 36, 47));
            UiRoundRing(sx - r, cy - r, r * 2, r * 2, r, 1.5f, UI_COL_TEXT3);
            for (k = -3; k <= 3; k++) UiCircle(sx + k * 2.4f, cy - k * 2.4f, 1.1f, UI_COL_TEXT3);
        }
        else UiCircle(sx, cy, r, D3DCOLOR_ARGB(255, s_fxPalette[i][0], s_fxPalette[i][1], s_fxPalette[i][2]));
        if (i != chosen) continue;
        if (selected)
        {
            float a = selection_pulse();
            selection_glow(sx - rr, cy - rr, rr * 2, rr * 2, rr, a);
            UiRoundRing(sx - rr, cy - rr, rr * 2, rr * 2, rr, 2.5f, SEL_RING(0.5f + 0.5f * a));
        }
        else UiRoundRing(sx - rr, cy - rr, rr * 2, rr * 2, rr, 2.0f, UI_COL_TEXT);
    }
}

static void fxpage_draw(const UiMenu *m)
{
    static UiAnim fill[2];
    const float w = 600, h = 680, x = 24, y = 20, foot = 78, iw = w - 88, ix = x + 44;
    const Config *c = s_fxCfg;
    float ry = y + 96;
    char buf[128], fit[96];
    int i, any;
    if (!c) return;
    any = c->fxColor[0] || c->fxColor[1] || c->fxColor[2];
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 48, UI_COL_TEXT, m->title, UI_LEFT);
    // The effect: "< name >".
    if (m->sel == FXP_EFFECT) selection_bar(x + 28, ry, w - 56, 42);
    UiText(UI_BOLD, 20, ix, ry + 21, m->sel == FXP_EFFECT ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_FX_EFFECT), UI_LEFT);
    UiTextFit(UI_BOLD, 20, iw - 160, Tr(s_fxLabels[c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : 0]), fit, sizeof(fit));
    _snprintf(buf, sizeof(buf) - 1, "< %s >", fit);
    buf[sizeof(buf) - 1] = 0;
    UiText(UI_BOLD, 20, ix + iw, ry + 21, UI_COL_TEXT, buf, UI_RIGHT);
    ry += 60;
    // The three colours: a label (and "Original" / "Off" while none is chosen there), the swatches under it.
    for (i = 0; i < 3; i++)
    {
        int sel = m->sel == FXP_COLOR1 + i, v = c->fxColor[i];
        if (sel) selection_bar(x + 28, ry - 4, w - 56, 66);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_FX_COLOR), i + 1);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_BOLD, 20, ix, ry + 14, sel ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
        if (!v) UiText(UI_REGULAR, 19, ix + iw, ry + 14, UI_COL_TEXT3, Tr(!any && !i ? T_FX_ORIGINAL : T_OFF), UI_RIGHT);
        fx_swatches(ix, ry + 44, v >= 0 && v <= FX_PALETTE_COUNT ? v : 0, sel);
        ry += 70;
    }
    // Opacity and speed: bars, full at 200% and 300%.
    ry += 4;
    volume_bar_draw(ix, ry, iw, Tr(T_FX_OPACITY), c->fxOpacity, 200, m->sel == FXP_OPACITY, &fill[0]);
    ry += 84;
    volume_bar_draw(ix, ry, iw, Tr(T_FX_SPEED), c->fxSpeed, 300, m->sel == FXP_SPEED, &fill[1]);
    ry += 80;
    // Reset: a button.
    {
        float bw = UiTextWidth(UI_BOLD, 20, Tr(T_FX_RESET)) + 60, bx = x + (w - bw) * 0.5f;
        if (m->sel == FXP_RESET) selection_bar(bx, ry, bw, 40);
        else UiRoundRing(bx, ry, bw, 40, 12, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 20, x + w * 0.5f, ry + 20, m->sel == FXP_RESET ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_FX_RESET), UI_CENTER);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- Wallpapers page (Interface > Wallpapers, A): the background - none, or
// one of the pictures of game:\wallpapers\ and its folders, the others after
// it in turn - how they
// are laid out, how long each one stays, the transition between two, their
// order, how much they are darkened. One wide panel: the settings on the left,
// on the right a live preview of the backdrop and a thumbnail of each layout.
enum { WP_PICTURE = 0, WP_RECOMMEND, WP_FIT, WP_INTERVAL, WP_TRANSITION, WP_ORDER, WP_DIM, WP_RESET, WP_COUNT };
static const int s_wallSecsList[] = { 0, 5, 10, 20, 30, 60, 120, 300, 600 };   // 0: never changes

// The background chosen: 0 none, else 1 + its place among the pictures.
static int wall_choice(const Config *c)
{
    int i;
    if (!c->wallCustom || !UiWallpaperCount()) return 0;
    i = UiWallpaperFind(c->wallFile);
    return i < 0 ? 1 : i + 1;   // gone from the folder: the first
}

// "< No >", or "< name (3/12) >" (its name without the extension, cut to fit).
static void wall_choice_text(const Config *c, char *out, size_t size, float maxW)
{
    int k = wall_choice(c), n = UiWallpaperCount();
    if (!k) _snprintf(out, size - 1, "< %s >", Tr(n || !c->wallCustom ? T_OFF : T_WALL_NONE_FOUND));
    else
    {
        char name[128], fit[64], count[16], *dot;
        strncpy(name, UiWallpaperName(k - 1), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        if ((dot = strrchr(name, '.')) != NULL) *dot = 0;
        sprintf(count, " (%d/%d)", k, n);
        UiTextFit(UI_BOLD, 20, maxW - UiTextWidth(UI_BOLD, 20, count), name, fit, sizeof(fit));
        _snprintf(out, size - 1, "< %s%s >", fit, count);
    }
    out[size - 1] = 0;
}
static const int s_transLabels[UI_TRANS_COUNT] = { T_TR_FADE, T_TR_SLIDE, T_TR_ZOOM, T_TR_WIPE, T_TR_BLINDS, T_TR_RANDOM };
static const int s_fitLabels[UI_FIT_COUNT] = { T_FIT_FILL, T_FIT_FIT, T_FIT_STRETCH, T_FIT_CENTER, T_FIT_TILE };
static const Config *s_wallCfg;

static void settings_wall(UiMenu *m, const Config *c)
{
    s_interfacePage = 0;
    UiMenuClear(m, Tr(T_WALLPAPERS), Tr(T_FOOT_SELECT));
    m->page = UI_PAGE_WALL;
    m->count = WP_COUNT;
    m->sel = 0;
    s_wallCfg = c;
    UiWallpaperRescan();   // pictures may have been copied in since
}

static int wall_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0, a = (down & XINPUT_GAMEPAD_A) != 0, k, cur = 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_WALLPAPERS;
        return SET_NONE;
    }
    if (!dir && !a) return SET_NONE;
    switch (m->sel)
    {
    case WP_PICTURE:
    {
        // No, then each picture.
        int n = UiWallpaperCount(), states = n + 1;
        cur = (wall_choice(c) + (dir ? dir : 1) + states) % states;
        c->wallCustom = cur > 0;
        if (cur)
        {
            strncpy(c->wallFile, UiWallpaperName(cur - 1), sizeof(c->wallFile) - 1);
            c->wallFile[sizeof(c->wallFile) - 1] = 0;
        }
        break;
    }
    case WP_RECOMMEND:
        c->wallRecommend = !c->wallRecommend;
        break;
    case WP_FIT:
        c->wallFit = ((c->wallFit >= 0 && c->wallFit < UI_FIT_COUNT ? c->wallFit : 0) + (dir ? dir : 1) + UI_FIT_COUNT) % UI_FIT_COUNT;
        break;
    case WP_INTERVAL:
    {
        const int n = sizeof(s_wallSecsList) / sizeof(s_wallSecsList[0]);
        for (k = 0; k < n; k++)
            if (s_wallSecsList[k] <= c->wallInterval) cur = k;
        c->wallInterval = s_wallSecsList[(cur + (dir ? dir : 1) + n) % n];   // "never" is one of them
        break;
    }
    case WP_TRANSITION:
        c->wallTransition = ((c->wallTransition >= 0 && c->wallTransition < UI_TRANS_COUNT ? c->wallTransition : 0) + (dir ? dir : 1) + UI_TRANS_COUNT) %
                            UI_TRANS_COUNT;
        break;
    case WP_ORDER:
        c->wallShuffle = !c->wallShuffle;
        break;
    case WP_DIM:
        if (!dir) return SET_NONE;
        c->wallDim += dir * 10;
        if (c->wallDim < 0) c->wallDim = 0;
        if (c->wallDim > 80) c->wallDim = 80;
        break;
    case WP_RESET:
        if (!a) return SET_NONE;   // on or off stays: only how it is shown
        c->wallFit = UI_FIT_FILL;
        c->wallInterval = 30;
        c->wallTransition = UI_TRANS_FADE;
        c->wallShuffle = 0;
        c->wallDim = 40;
        break;
    }
    ui_from_config(c);
    return SET_CHANGED;
}

// One row of the graphical pages: the selection behind it, its label at the left, its value at the right.
static void page_row(float x, float w, float y, float h, const char *label, const char *value, int selected)
{
    if (selected) selection_bar(x + 28, y, w - 56, h);
    UiText(UI_BOLD, 20, x + 44, y + h * 0.5f, selected ? UI_COL_TEXT : UI_COL_TEXT2, label, UI_LEFT);
    if (value && value[0]) UiText(UI_BOLD, 20, x + w - 44, y + h * 0.5f, UI_COL_TEXT, value, UI_RIGHT);
}

// A thumbnail of one layout: the wallpaper shown now laid out that way, or a sketch of it when there is none.
static void wall_thumb(float x, float y, float w, float h, int fit, int chosen, int selected)
{
    UiRoundRect(x, y, w, h, 6, D3DCOLOR_ARGB(255, 20, 22, 30), D3DCOLOR_ARGB(255, 14, 15, 21));
    if (!UiWallpaperThumb(x, y, w, h, fit))
    {
        D3DCOLOR c = D3DCOLOR_ARGB(150, 120, 130, 160);
        float pw = h * 1.33f;   // a 4:3 picture
        switch (fit)
        {
        case UI_FIT_FIT: UiRect(x + (w - pw) * 0.5f, y, pw, h, c); break;
        case UI_FIT_CENTER: UiRect(x + w * 0.35f, y + h * 0.3f, w * 0.3f, h * 0.4f, c); break;
        case UI_FIT_TILE:
        {
            int i, j;
            for (i = 0; i < 4; i++)
                for (j = 0; j < 3; j++) UiRect(x + 2 + i * (w - 4) / 4, y + 2 + j * (h - 4) / 3, (w - 4) / 4 - 2, (h - 4) / 3 - 2, c);
            break;
        }
        default: UiRect(x, y, w, h, c); break;
        }
    }
    if (chosen)
    {
        if (selected)
        {
            float a = selection_pulse();
            selection_glow(x - 3, y - 3, w + 6, h + 6, 8, a);
            UiRoundRing(x - 3, y - 3, w + 6, h + 6, 8, 2.5f, SEL_RING(0.5f + 0.5f * a));
        }
        else UiRoundRing(x - 3, y - 3, w + 6, h + 6, 8, 2.0f, UI_COL_TEXT);
    }
    else UiRoundRing(x, y, w, h, 6, 1.0f, UI_COL_LINE);
}

static void wallpage_draw(const UiMenu *m)
{
    static UiAnim fill;
    const float w = 1232, h = 680, x = 24, y = 20, foot = 78, lw = 600, iw = lw - 88, ix = x + 44;
    const float px = x + lw + 10, pw = w - lw - 54, ph = pw * UI_HEIGHT / UI_WIDTH, py = y + 96;
    const Config *c = s_wallCfg;
    float ry = y + 96;
    char buf[96];
    int i, fit;
    if (!c) return;
    fit = c->wallFit >= 0 && c->wallFit < UI_FIT_COUNT ? c->wallFit : 0;
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 48, UI_COL_TEXT, m->title, UI_LEFT);
    // ---- The settings, on the left.
    wall_choice_text(c, buf, sizeof(buf), 300);
    page_row(x, lw, ry, 44, Tr(T_WALL_CUSTOM), buf, m->sel == WP_PICTURE);
    ry += 50;
    page_row(x, lw, ry, 44, Tr(T_WALL_RECOMMEND), "", m->sel == WP_RECOMMEND);
    draw_value(x + lw - 44, ry + 22, toggle(c->wallRecommend), m->sel == WP_RECOMMEND);
    ry += 50;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_fitLabels[fit]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 44, Tr(T_WALL_FIT), buf, m->sel == WP_FIT);
    ry += 50;
    if (c->wallInterval <= 0) _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(T_WALL_NEVER));
    else if (c->wallInterval < 60) _snprintf(buf, sizeof(buf) - 1, "< %d s >", c->wallInterval);
    else _snprintf(buf, sizeof(buf) - 1, "< %d min >", c->wallInterval / 60);
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 44, Tr(T_WALL_INTERVAL), buf, m->sel == WP_INTERVAL);
    ry += 50;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_transLabels[c->wallTransition >= 0 && c->wallTransition < UI_TRANS_COUNT ? c->wallTransition : 0]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 44, Tr(T_WALL_TRANSITION), buf, m->sel == WP_TRANSITION);
    ry += 50;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(c->wallShuffle ? T_ORDER_RANDOM : T_ORDER_NAME));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 44, Tr(T_WALL_ORDER), buf, m->sel == WP_ORDER);
    ry += 58;
    volume_bar_draw(ix, ry, iw, Tr(T_WALL_DIM), c->wallDim, 80, m->sel == WP_DIM, &fill);
    ry += 84;
    {
        float bw = UiTextWidth(UI_BOLD, 20, Tr(T_FX_RESET)) + 60, bx = x + (lw - bw) * 0.5f;
        if (m->sel == WP_RESET) selection_bar(bx, ry, bw, 40);
        else UiRoundRing(bx, ry, bw, 40, 12, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 20, x + lw * 0.5f, ry + 20, m->sel == WP_RESET ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_FX_RESET), UI_CENTER);
    }
    // ---- The preview and the layouts, on the right.
    UiRect(x + lw - 8, y + 90, 1, h - foot - 110, UI_COL_LINE);
    UiText(UI_BOLD, 20, px, py - 22, UI_COL_TEXT2, Tr(T_SAVER_PREVIEW), UI_LEFT);
    UiRoundRect(px, py, pw, ph, 8, D3DCOLOR_ARGB(255, 16, 17, 23), D3DCOLOR_ARGB(255, 10, 11, 15));
    if (c->wallRecommend && s_rec[0].tex)
        UiImage(s_rec[0].tex, px + (pw - ph * s_rec[0].w / s_rec[0].h) * 0.5f, py, ph * s_rec[0].w / s_rec[0].h, ph, 8, D3DCOLOR_ARGB(255, 255, 255, 255));
    else if (!c->wallCustom || !UiWallpaperPreview(px, py, pw, ph))
    {
        // Off, or no picture to show: what to do about it.
        const char *say = Tr(UiWallpaperCount() ? T_WALL_OFF_HINT : T_WALL_HINT);
        float th = paragraph(UI_REGULAR, 19, px + 30, 0, pw - 60, 3, 0, say);   // measured
        paragraph(UI_REGULAR, 19, px + 30, py + (ph - th) * 0.5f, pw - 60, 3, UI_COL_TEXT2, say);
    }
    UiRoundRing(px, py, pw, ph, 8, 1.0f, UI_COL_LINE);
    {
        const float tw = (pw - 4 * 14) / 5, th = tw * UI_HEIGHT / UI_WIDTH, ty = py + ph + 26;
        for (i = 0; i < UI_FIT_COUNT; i++)
        {
            float tx = px + i * (tw + 14);
            wall_thumb(tx, ty, tw, th, i, i == fit, m->sel == WP_FIT);
            UiText(i == fit ? UI_BOLD : UI_REGULAR, 16, tx + tw * 0.5f, ty + th + 18, i == fit ? UI_COL_TEXT : UI_COL_TEXT3, Tr(s_fitLabels[i]), UI_CENTER);
        }
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- Borders page (Interface > Borders and panels, A): the colour of the
// menus' edges (the selection's, or one of the effect page's swatches), how
// strong they are, how wide, whether they glow (pulsing, steady, not at all),
// how see-through the panels are and how round their corners. A panel like
// the others, so every change shows on the page itself.
enum { BP_COLOR = 0, BP_OPACITY, BP_WIDTH, BP_GLOW, BP_PANEL, BP_PANELCOLOR, BP_CORNERS, BP_RESET, BP_COUNT };
// Config::borderGlow (GLOW_COUNT, with the edges above): 0 pulsing, 1 steady, 2 none (the values of
// before), 3 snake, 4 fade, 5 rainbow; the row steps through them in s_glowOrder's order (none last).
static const int s_glowLabels[GLOW_COUNT] = { T_GLOW_PULSE, T_GLOW_STEADY, T_OFF, T_GLOW_SNAKE, T_GLOW_FADE, T_GLOW_RAINBOW };
static const int s_glowOrder[GLOW_COUNT] = { 0, 1, 3, 4, 5, 2 };
static const int s_cornerLabels[4] = { T_CORNER_SQUARE, T_CORNER_SOFT, T_CORNER_ROUND, T_CORNER_ROUNDER };
static const Config *s_borderCfg;

static void settings_borders(UiMenu *m, const Config *c)
{
    s_interfacePage = 0;
    UiMenuClear(m, Tr(T_BORDERS), Tr(T_FOOT_SELECT));
    m->page = UI_PAGE_BORDER;
    m->count = BP_COUNT;
    m->sel = 0;
    s_borderCfg = c;
}

static int border_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0, a = (down & XINPUT_GAMEPAD_A) != 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_BORDERS;
        return SET_NONE;
    }
    if (!dir && !a) return SET_NONE;
    switch (m->sel)
    {
    case BP_COLOR:
        c->borderColor = ((c->borderColor >= 0 && c->borderColor <= FX_PALETTE_COUNT ? c->borderColor : 0) + (dir ? dir : 1) + FX_PALETTE_COUNT + 1) %
                         (FX_PALETTE_COUNT + 1);
        break;
    case BP_OPACITY:
        if (!dir) return SET_NONE;
        c->borderOpacity += dir * 10;
        if (c->borderOpacity < 0) c->borderOpacity = 0;
        if (c->borderOpacity > 100) c->borderOpacity = 100;
        break;
    case BP_WIDTH:
        c->borderWidth = ((c->borderWidth >= 0 && c->borderWidth <= 4 ? c->borderWidth : 1) + (dir ? dir : 1) + 5) % 5;
        break;
    case BP_GLOW:
    {
        int k, at = 0;
        for (k = 0; k < GLOW_COUNT; k++)
            if (s_glowOrder[k] == c->borderGlow) at = k;
        c->borderGlow = s_glowOrder[(at + (dir ? dir : 1) + GLOW_COUNT) % GLOW_COUNT];
        break;
    }
    case BP_PANEL:
        if (!dir) return SET_NONE;
        c->panelOpacity += dir * 10;
        if (c->panelOpacity < 20) c->panelOpacity = 20;
        if (c->panelOpacity > 100) c->panelOpacity = 100;
        break;
    case BP_PANELCOLOR:
        c->panelColor = ((c->panelColor >= 0 && c->panelColor <= FX_PALETTE_COUNT ? c->panelColor : 0) + (dir ? dir : 1) + FX_PALETTE_COUNT + 1) %
                        (FX_PALETTE_COUNT + 1);
        break;
    case BP_CORNERS:
        c->panelRound = ((c->panelRound >= 0 && c->panelRound < 4 ? c->panelRound : 2) + (dir ? dir : 1) + 4) % 4;
        break;
    case BP_RESET:
        if (!a) return SET_NONE;
        c->borderColor = 0;
        c->borderOpacity = 100;
        c->borderWidth = 1;
        c->borderGlow = 0;
        c->panelOpacity = 30;
        c->panelColor = 0;
        c->panelRound = 2;
        break;
    }
    ui_from_config(c);
    return SET_CHANGED;
}

static void borderpage_draw(const UiMenu *m)
{
    static UiAnim fill[2];
    const float w = 600, h = 680, x = 24, y = 20, foot = 78, iw = w - 88, ix = x + 44;
    const Config *c = s_borderCfg;
    float ry = y + 92;
    char buf[96];
    int v;
    if (!c) return;
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 48, UI_COL_TEXT, m->title, UI_LEFT);
    // The colour: "as the selection" first (its own colour, a dot in it), then the swatches.
    v = c->borderColor >= 0 && c->borderColor <= FX_PALETTE_COUNT ? c->borderColor : 0;
    if (m->sel == BP_COLOR) selection_bar(x + 28, ry - 4, w - 56, 66);
    UiText(UI_BOLD, 20, ix, ry + 14, m->sel == BP_COLOR ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_BORDER_COLOR), UI_LEFT);
    if (!v) UiText(UI_REGULAR, 19, ix + iw, ry + 14, UI_COL_TEXT3, Tr(T_COL_SELECTION), UI_RIGHT);
    fx_swatches(ix, ry + 44, v, m->sel == BP_COLOR);
    UiCircle(ix + 12, ry + 44, 12, SEL_HI(255));   // swatch 0 here: the selection's colour, not "none"
    UiCircle(ix + 12, ry + 44, 4, D3DCOLOR_ARGB(255, 20, 22, 30));
    ry += 76;
    volume_bar_draw(ix, ry, iw, Tr(T_BORDER_OPACITY), c->borderOpacity, 100, m->sel == BP_OPACITY, &fill[0]);
    ry += 80;
    if (c->borderWidth <= 0) _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(T_OFF));
    else _snprintf(buf, sizeof(buf) - 1, "< %d px >", c->borderWidth);
    buf[sizeof(buf) - 1] = 0;
    page_row(x, w, ry, 44, Tr(T_BORDER_WIDTH), buf, m->sel == BP_WIDTH);
    ry += 50;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_glowLabels[c->borderGlow >= 0 && c->borderGlow < GLOW_COUNT ? c->borderGlow : 0]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, w, ry, 44, Tr(T_BORDER_GLOW), buf, m->sel == BP_GLOW);
    ry += 56;
    volume_bar_draw(ix, ry, iw, Tr(T_PANEL_OPACITY), c->panelOpacity, 100, m->sel == BP_PANEL, &fill[1]);
    ry += 80;
    // The panels' colour: the interface's own first (a dark dot), then the swatches.
    v = c->panelColor >= 0 && c->panelColor <= FX_PALETTE_COUNT ? c->panelColor : 0;
    if (m->sel == BP_PANELCOLOR) selection_bar(x + 28, ry - 4, w - 56, 66);
    UiText(UI_BOLD, 20, ix, ry + 14, m->sel == BP_PANELCOLOR ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_PANEL_COLOR), UI_LEFT);
    fx_swatches(ix, ry + 44, v, m->sel == BP_PANELCOLOR);
    UiCircle(ix + 12, ry + 44, 12, UI_COL_PANEL_TOP | 0xFF000000);
    ry += 76;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_cornerLabels[c->panelRound >= 0 && c->panelRound < 4 ? c->panelRound : 2]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, w, ry, 44, Tr(T_PANEL_CORNERS), buf, m->sel == BP_CORNERS);
    ry += 58;
    {
        float bw = UiTextWidth(UI_BOLD, 20, Tr(T_FX_RESET)) + 60, bx = x + (w - bw) * 0.5f;
        if (m->sel == BP_RESET) selection_bar(bx, ry, bw, 40);
        else UiRoundRing(bx, ry, bw, 40, 12, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 20, x + w * 0.5f, ry + 20, m->sel == BP_RESET ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_FX_RESET), UI_CENTER);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- On-screen performance page (System > On-screen performance, A): what
// the box shows (each item ticked on or off), and how - where, its text's
// size and colour, its body's opacity, degrees C or F. One wide panel: the
// items on the left, on the right a preview of the box on a screen and the
// rest under it. In game every change shows on the real box too.
// The items (rows) and the Config::perfItems bit of each: the GPU load (bit 4) is gone.
enum { PP_ITEMS = 7, PP_POS = PP_ITEMS, PP_SIZE, PP_UNIT, PP_BOX, PP_COLOR, PP_RESET, PP_COUNT };
static const int s_perfItemLabels[PP_ITEMS] = { T_PERF_FPS, T_PERF_CPU, T_PERF_RAM, T_PERF_RES, T_PERF_TCPU, T_PERF_TGPU, T_PERF_TRAM };
static const int s_perfBit[PP_ITEMS] = { 0, 1, 2, 3, 5, 6, 7 };
static const int s_perfPosLabels[UI_PERF_POS_COUNT] = { T_POS_TL, T_POS_TC, T_POS_TR, T_POS_BL, T_POS_BR };
static const int s_perfSizeLabels[4] = { T_SIZE_S, T_SIZE_M, T_SIZE_L, T_SIZE_XL };
static const float s_perfSizes[4] = { 14, 17, 21, 26 };
static const Config *s_perfCfg;

void PerfStyleApply(const Config *c)
{
    int k = c->perfColor > 0 && c->perfColor <= FX_PALETTE_COUNT ? c->perfColor : 0;
    D3DCOLOR col = k ? D3DCOLOR_ARGB(255, s_fxPalette[k][0], s_fxPalette[k][1], s_fxPalette[k][2]) : UI_COL_TEXT;
    UiSetPerfStyle(c->perfPos, s_perfSizes[c->perfSize >= 0 && c->perfSize < 4 ? c->perfSize : 1], col, c->perfBox);
}

static void settings_perf(UiMenu *m, const Config *c)
{
    s_systemPage = 0;
    UiMenuClear(m, Tr(T_PERF_OVERLAY), Tr(T_FOOT_SELECT));
    m->page = UI_PAGE_PERF;
    m->count = PP_COUNT;
    m->sel = 0;
    s_perfCfg = c;
}

static int perf_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0, a = (down & XINPUT_GAMEPAD_A) != 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_system(m, c);
        m->sel = SYS_PERF;
        return SET_NONE;
    }
    // Left and right go between the two columns (the items; how the box looks), at the
    // same height: the rows on the right were reached only by going down past every
    // item. A changes the row's value (on the right: its next one).
    if (dir > 0 && m->sel < PP_ITEMS)
    {
        m->sel = PP_POS + (m->sel < PP_COUNT - PP_POS ? m->sel : PP_COUNT - PP_POS - 1);
        return SET_NONE;
    }
    if (dir < 0 && m->sel >= PP_ITEMS)
    {
        m->sel = m->sel - PP_POS < PP_ITEMS ? m->sel - PP_POS : PP_ITEMS - 1;
        return SET_NONE;
    }
    if (!a) return SET_NONE;
    if (m->sel < PP_ITEMS) c->perfItems ^= 1 << s_perfBit[m->sel];   // ticked on or off
    else switch (m->sel)
    {
    case PP_POS: c->perfPos = ((c->perfPos >= 0 && c->perfPos < UI_PERF_POS_COUNT ? c->perfPos : 2) + 1) % UI_PERF_POS_COUNT; break;
    case PP_SIZE: c->perfSize = ((c->perfSize >= 0 && c->perfSize < 4 ? c->perfSize : 1) + 1) % 4; break;
    case PP_UNIT: c->perfFahrenheit = !c->perfFahrenheit; break;
    case PP_BOX: c->perfBox = c->perfBox >= 100 ? 0 : (c->perfBox / 10 + 1) * 10; break;   // 0, 10 ... 100, then 0 again
    case PP_COLOR:
        c->perfColor = ((c->perfColor >= 0 && c->perfColor <= FX_PALETTE_COUNT ? c->perfColor : 0) + 1) % (FX_PALETTE_COUNT + 1);
        break;
    case PP_RESET:   // what is shown stays: only how
        c->perfPos = UI_PERF_TOP_RIGHT;
        c->perfSize = 1;
        c->perfColor = 0;
        c->perfBox = 75;
        c->perfFahrenheit = 0;
        break;
    }
    c->showFps = c->perfItems ? 1 : 0;
    PerfStyleApply(c);
    return SET_CHANGED;
}

// The box's text with sample figures (the real temperatures when they can be read).
static void perf_sample(const Config *c, char *out, size_t size)
{
    float t[3] = { 52, 58, 49 };
    int k, n = 0;
    out[0] = 0;
    UiTemperatures(!c->xenia, &t[0], &t[1], &t[2]);
    for (k = 0; k < 8; k++)
    {
        char line[48];
        if (k == 4 || !(c->perfItems & (1 << k))) continue;   // 4: the GPU load, gone
        switch (k)
        {
        case 0: strcpy(line, "\x01" "60.0 FPS"); break;   // each line with its icon (UI_PERF_ICON_*)
        case 1: strcpy(line, "\x02" "CPU 42%"); break;
        case 2: strcpy(line, "\x03" "RAM 236MB"); break;
        case 3: strcpy(line, "\x04" "1280x720"); break;
        default:
        {
            float v = t[k - 5];
            int icon = v > 75.0f ? UI_PERF_ICON_TEMP_HOT : v >= 70.0f ? UI_PERF_ICON_TEMP_WARN : UI_PERF_ICON_TEMP_OK;
            sprintf(line, "%c%s %d\xC2\xB0%c", icon, k == 5 ? "CPU" : k == 6 ? "GPU" : "RAM",
                    (int)(c->perfFahrenheit ? v * 9 / 5 + 32 + 0.5f : v + 0.5f), c->perfFahrenheit ? 'F' : 'C');
            break;
        }
        }
        if (n++) strncat(out, "\n", size - strlen(out) - 1);
        strncat(out, line, size - strlen(out) - 1);
    }
}

static void perfpage_draw(const UiMenu *m)
{
    static UiAnim fill;
    const float w = 1232, h = 680, x = 24, y = 20, foot = 78, lw = 600;
    const float px = x + lw + 10, pw = w - lw - 54, ph = pw * UI_HEIGHT / UI_WIDTH, py = y + 96, rx = px - 44, rw = pw + 88;
    const Config *c = s_perfCfg;
    float ry = y + 92;
    char buf[300];
    int i, v;
    if (!c) return;
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 48, UI_COL_TEXT, m->title, UI_LEFT);
    // ---- What it shows, ticked: on the left.
    for (i = 0; i < PP_ITEMS; i++)
    {
        int on = (c->perfItems >> s_perfBit[i]) & 1, sel = m->sel == i;
        const float bx = x + lw - 80, by = ry + 8;
        static const int icons[PP_ITEMS] = { UI_PERF_ICON_FPS, UI_PERF_ICON_CPU, UI_PERF_ICON_RAM, UI_PERF_ICON_RES,
                                             UI_PERF_ICON_TEMP_OK, UI_PERF_ICON_TEMP_OK, UI_PERF_ICON_TEMP_OK };
        if (sel) selection_bar(x + 28, ry, lw - 56, 34);
        UiPerfIcon(icons[i], x + 44, ry + 17, 20, UiAlpha(sel ? UI_COL_TEXT : UI_COL_TEXT2, 0.75f));   // the box's own, discreet
        UiText(UI_BOLD, 20, x + 74, ry + 17, sel ? UI_COL_TEXT : UI_COL_TEXT2, Tr(s_perfItemLabels[i]), UI_LEFT);
        // The tick box.
        UiRoundRect(bx, by, 18, 18, 5, on ? SEL_HI(255) : D3DCOLOR_ARGB(255, 34, 36, 47), on ? SEL_LO(255) : D3DCOLOR_ARGB(255, 27, 29, 38));
        UiRoundRing(bx, by, 18, 18, 5, 1.5f, on ? SEL_HI(255) : UI_COL_TEXT3);
        if (on)
        {
            UiLine(bx + 4, by + 9.5f, bx + 7.5f, by + 13, 2.2f, D3DCOLOR_ARGB(255, 15, 16, 22));
            UiLine(bx + 7.5f, by + 13, bx + 14, by + 5.5f, 2.2f, D3DCOLOR_ARGB(255, 15, 16, 22));
        }
        ry += 38;
    }
    ry += 6;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_perfPosLabels[c->perfPos >= 0 && c->perfPos < UI_PERF_POS_COUNT ? c->perfPos : 2]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 40, Tr(T_PERF_POS), buf, m->sel == PP_POS);
    ry += 44;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", Tr(s_perfSizeLabels[c->perfSize >= 0 && c->perfSize < 4 ? c->perfSize : 1]));
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 40, Tr(T_PERF_SIZE), buf, m->sel == PP_SIZE);
    ry += 44;
    _snprintf(buf, sizeof(buf) - 1, "< %s >", c->perfFahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
    buf[sizeof(buf) - 1] = 0;
    page_row(x, lw, ry, 40, Tr(T_PERF_UNIT), buf, m->sel == PP_UNIT);
    // ---- The preview, then the box's opacity, its text colour and the reset: on the right.
    UiRect(x + lw - 8, y + 90, 1, h - foot - 110, UI_COL_LINE);
    UiText(UI_BOLD, 20, px, py - 22, UI_COL_TEXT2, Tr(T_SAVER_PREVIEW), UI_LEFT);
    UiRoundRect(px, py, pw, ph, 8, D3DCOLOR_ARGB(255, 46, 70, 110), D3DCOLOR_ARGB(255, 22, 30, 48));   // a "game" behind it
    UiShadow(px + pw * 0.3f, py + ph * 0.45f, pw * 0.4f, ph * 0.35f, 20, 40, D3DCOLOR_ARGB(70, 120, 200, 120));
    perf_sample(c, buf, sizeof(buf));
    if (buf[0]) UiPerfPreview(buf, px, py, pw, ph);
    else UiText(UI_REGULAR, 19, px + pw * 0.5f, py + ph * 0.5f, UI_COL_TEXT2, Tr(T_OFF), UI_CENTER);
    UiRoundRing(px, py, pw, ph, 8, 1.0f, UI_COL_LINE);
    ry = py + ph + 14;
    volume_bar_draw(px, ry, pw, Tr(T_PERF_BOX), c->perfBox, 100, m->sel == PP_BOX, &fill);
    ry += 76;
    v = c->perfColor >= 0 && c->perfColor <= FX_PALETTE_COUNT ? c->perfColor : 0;
    if (m->sel == PP_COLOR) selection_bar(rx + 28, ry - 4, rw - 56, 66);
    UiText(UI_BOLD, 20, px, ry + 14, m->sel == PP_COLOR ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_PERF_COLOR), UI_LEFT);
    if (!v) UiText(UI_REGULAR, 19, px + pw, ry + 14, UI_COL_TEXT3, Tr(T_COL_WHITE), UI_RIGHT);
    fx_swatches(px, ry + 44, v, m->sel == PP_COLOR);
    UiCircle(px + 12, ry + 44, 12, UI_COL_TEXT);   // swatch 0 here: the usual white, not "none"
    {
        // Reset: on the left, under the rows.
        float bw = UiTextWidth(UI_BOLD, 20, Tr(T_FX_RESET)) + 60, bx = x + (lw - bw) * 0.5f, by = y + h - foot - 60;
        if (m->sel == PP_RESET) selection_bar(bx, by, bw, 40);
        else UiRoundRing(bx, by, bw, 40, 12, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 20, x + lw * 0.5f, by + 20, m->sel == PP_RESET ? UI_COL_TEXT : UI_COL_TEXT2, Tr(T_FX_RESET), UI_CENTER);
    }
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// ---- Paths page (Settings > Paths; ROM list only): the folders the emulator
// uses - the ROMs, the menus' music, the user's own interface sounds, the
// wallpapers, the saves and save states, the screenshots, the hi-res texture
// packs - each chosen with
// the folder picker (FolderPicker), or back to the usual ones. Every folder is
// made when it is chosen, and at each start when it is missing (paths_create).
enum { PA_ROMS = 0, PA_MUSIC, PA_SOUNDS, PA_WALLS, PA_SAVES, PA_SHOTS, PA_TEXTURES, PA_RESET };
static const int s_pathLabels[CONFIG_PATH_COUNT] = { T_PATH_ROMS, T_PATH_MUSIC, T_PATH_SOUNDS, T_WALLPAPERS, T_PATH_SAVES, T_PATH_SHOTS, T_PATH_TEXTURES };
static int s_pickWhich;   // the path the folder picker is choosing (settings_loop)
static int s_pathReset;   // the path to put back to its usual folder (PA_RESET: all of them), once confirmed (settings_loop)

// A folder and the ones it is in ("Usb0:\a\b": Usb0:\a, then Usb0:\a\b).
static void mkdir_p(const char *path)
{
    char buf[160];
    size_t i, n = strlen(path);
    if (n >= sizeof(buf)) return;
    strcpy(buf, path);
    for (i = 0; i < n; i++)
        if (buf[i] == '\\' && i > 0 && buf[i - 1] != ':')
        {
            buf[i] = 0;
            CreateDirectoryA(buf, NULL);
            buf[i] = '\\';
        }
    CreateDirectoryA(buf, NULL);
}

static void paths_create(const Config *c)
{
    int k;
    for (k = 0; k < CONFIG_PATH_COUNT; k++) mkdir_p(ConfigPath(c, k, "game"));
}

static void paths_values(UiMenu *m, const Config *c)
{
    int k;
    for (k = 0; k < CONFIG_PATH_COUNT; k++)
    {
        char fit[64];
        UiTextFit(UI_REGULAR, 21, 250, ConfigPath(c, k, "game"), fit, sizeof(fit));
        strcpy(m->value[k], fit);   // opens the picker: no arrows
    }
}

static void settings_paths(UiMenu *m, const Config *c)
{
    static const unsigned char icons[] = { UI_ICON_LIST, UI_ICON_MUSIC, UI_ICON_SOUND, UI_ICON_FRAME, UI_ICON_SAVE, UI_ICON_SCREEN, UI_ICON_TEXTURE,
                                           UI_ICON_RESET };
    int k;
    s_pathsPage = 1;
    UiMenuClear(m, Tr(T_PATHS), Tr(T_FOOT_PATHS));
    for (k = 0; k < CONFIG_PATH_COUNT; k++) UiMenuAdd(m, Tr(s_pathLabels[k]), "");
    UiMenuAdd(m, Tr(T_FX_RESET), "");
    UiMenuIcons(m, icons, sizeof(icons));
    paths_values(m, c);
}

static int paths_input(UiMenu *m, Config *c, WORD down)
{
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_main(m, c);
        m->sel = S_PATHS;
        return SET_NONE;
    }
    // Y on a path, or A on Reset (all of them): back to the usual folder, once confirmed (settings_loop).
    if ((down & XINPUT_GAMEPAD_Y) || ((down & XINPUT_GAMEPAD_A) && m->sel == PA_RESET))
    {
        s_pathReset = m->sel;
        return SET_PATH_RESET;
    }
    if (!(down & XINPUT_GAMEPAD_A)) return SET_NONE;
    s_pickWhich = m->sel;
    return SET_PICK_PATH;
}

// The confirmed reset (SET_PATH_RESET): one path or all of them, their folders made.
static void paths_reset(UiMenu *m, Config *c)
{
    if (s_pathReset >= 0 && s_pathReset < CONFIG_PATH_COUNT) c->path[s_pathReset][0] = 0;
    else memset(c->path, 0, sizeof(c->path));   // the usual folders
    paths_create(c);
    ui_from_config(c);
    paths_values(m, c);
}

// ---- Folder picker ----
// The drives, then the folders of the one opened: A opens a folder, B goes up
// (out, from the drives), Y takes the folder open now.
static const char *const s_pickDrives[] = { "game:", "Hdd1:", "Usb0:", "Usb1:", "Usb2:", "Dvd:" };

static bool pick_less(const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; }

static void pick_list(const char *cur, std::vector<std::string> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[200];
    size_t i;
    out->clear();
    if (!cur[0])
    {
        // The drives that are there (each must answer a listing).
        for (i = 0; i < sizeof(s_pickDrives) / sizeof(s_pickDrives[0]); i++)
        {
            _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", s_pickDrives[i]);
            pattern[sizeof(pattern) - 1] = 0;
            h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE) { FindClose(h); out->push_back(s_pickDrives[i]); }
            else if (GetFileAttributesA(s_pickDrives[i]) != 0xFFFFFFFF) out->push_back(s_pickDrives[i]);   // empty
        }
        return;
    }
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", cur);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) out->push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out->begin(), out->end(), pick_less);
}

// A small folder (or a drive: a box with a light) at (x, cy).
static void pick_icon(float x, float cy, int drive, D3DCOLOR c)
{
    if (drive)
    {
        UiRoundRing(x, cy - 8, 24, 16, 3, 1.6f, c);
        UiCircle(x + 19, cy, 2, c);
        return;
    }
    UiRoundRect(x, cy - 9, 10, 5, 2, c, c);
    UiRoundRing(x, cy - 6, 24, 15, 3, 1.6f, c);
}

int FolderPicker(IDirect3DDevice9 *dev, const char *start, char *out, int outSize)
{
    const float w = 900, h = 600, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f, rowH = 40, listY = y + 140;
    const int rows = 10;
    char cur[200];
    std::vector<std::string> items;
    UiInput in;
    UiAnim selAnim;
    int sel = 0, top = 0, result = -1;
    memset(&selAnim, 0, sizeof(selAnim));
    // Opened where the path is now, when it is there; else on the drives.
    strncpy(cur, start ? start : "", sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = 0;
    if (cur[0] && GetFileAttributesA(cur) == 0xFFFFFFFF) cur[0] = 0;
    pick_list(cur, &items);
    UiInputInit(&in);
    while (result < 0)
    {
        WORD down = UiInputPoll(&in);
        int n = (int)items.size() + (cur[0] ? 1 : 0);   // ".." first in a folder
        if (n && (down & XINPUT_GAMEPAD_DPAD_UP)) sel = (sel + n - 1) % n;
        if (n && (down & XINPUT_GAMEPAD_DPAD_DOWN)) sel = (sel + 1) % n;
        if (down & XINPUT_GAMEPAD_Y && cur[0])
        {
            strncpy(out, cur, outSize - 1);
            out[outSize - 1] = 0;
            result = 1;
        }
        else if ((down & XINPUT_GAMEPAD_B) || ((down & XINPUT_GAMEPAD_A) && cur[0] && sel == 0))
        {
            // Up: the folder this one is in, the drives from a drive's top, out from the drives.
            char *slash = strrchr(cur, '\\');
            if (!cur[0]) result = 0;
            else
            {
                std::string was = slash ? slash + 1 : cur;
                if (slash) *slash = 0;
                else cur[0] = 0;
                pick_list(cur, &items);
                sel = 0;
                {
                    size_t k;
                    for (k = 0; k < items.size(); k++)
                        if (!_stricmp(items[k].c_str(), was.c_str())) sel = (int)k + (cur[0] ? 1 : 0);
                }
            }
        }
        else if ((down & XINPUT_GAMEPAD_A) && n)
        {
            const std::string &name = items[sel - (cur[0] ? 1 : 0)];
            if (strlen(cur) + name.size() + 2 < sizeof(cur))
            {
                if (cur[0]) { strcat(cur, "\\"); strcat(cur, name.c_str()); }
                else strcpy(cur, name.c_str());
                pick_list(cur, &items);
                sel = 0;
            }
        }
        n = (int)items.size() + (cur[0] ? 1 : 0);
        if (sel >= n) sel = n ? n - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        browser_effect();
        UiDim();
        edge_glow(x, y, w, h, 22);
        UiPanel(x, y, w, h, 22);
        UiText(UI_BOLD, 30, x + 40, y + 50, UI_COL_TEXT, Tr(T_PICK_TITLE), UI_LEFT);
        {
            char fit[200];
            UiTextFit(UI_REGULAR, 19, w - 80, cur[0] ? cur : Tr(T_PICK_DRIVES), fit, sizeof(fit));
            UiText(UI_REGULAR, 19, x + 40, y + 96, UI_COL_ACCENT_HI, fit, UI_LEFT);
        }
        if (n)
        {
            int i;
            float sy = anim_step(&selAnim, listY + (sel - top) * rowH, 0);
            selection_bar(x + 28, sy, w - 56, rowH - 4);
            for (i = 0; i < rows && top + i < n; i++)
            {
                int k = top + i, s = k == sel, up = cur[0] && k == 0;
                float cy = listY + i * rowH + (rowH - 4) * 0.5f;
                const char *name = up ? ".." : items[k - (cur[0] ? 1 : 0)].c_str();
                char fit[160];
                pick_icon(x + 48, cy, !cur[0], s ? UI_COL_TEXT : UI_COL_TEXT2);
                UiTextFit(s ? UI_BOLD : UI_REGULAR, 20, w - 170, name, fit, sizeof(fit));
                UiText(s ? UI_BOLD : UI_REGULAR, 20, x + 88, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, fit, UI_LEFT);
            }
        }
        else UiText(UI_REGULAR, 19, x + w * 0.5f, listY + 60, UI_COL_TEXT3, Tr(T_PICK_EMPTY), UI_CENTER);
        UiRect(x + 24, y + h - 78, w - 48, 1, UI_COL_LINE);
        UiFooter(x + 40, y + h - 39, Tr(cur[0] ? T_FOOT_PICK : T_FOOT_PICK_DRIVES));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    H64_INFO("[menu] folder picker: %s", result ? out : "cancelled");
    return result;
}

// ---- File explorer (Settings > System > File explorer, from the ROM list) ----
// Two panels side by side - game: on the left, Hdd1: on the right at first -, each
// a folder and its contents (folders first). LB/RB pick the panel, A opens a
// folder, B goes up (from a drive: the drives; from the drives: out), X marks,
// Y copies the marked items (or the one under the cursor), LT cuts them, RT
// pastes into the panel's folder. The transfer runs on its own thread with a
// bar (B cancels); a toast says when it is over.
struct ExpItem { std::string name; int dir; unsigned long long size; int marked; };
struct ExpPane { char path[260]; std::vector<ExpItem> items; int sel, top; };

static bool exp_less(const ExpItem &a, const ExpItem &b)
{
    if (a.dir != b.dir) return a.dir > b.dir;   // folders first
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
}

static void exp_list(ExpPane *p)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[280];
    size_t i;
    p->items.clear();
    if (!p->path[0])
    {
        for (i = 0; i < sizeof(s_pickDrives) / sizeof(s_pickDrives[0]); i++)
        {
            ExpItem it;
            _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", s_pickDrives[i]);
            pattern[sizeof(pattern) - 1] = 0;
            h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
            else if (GetFileAttributesA(s_pickDrives[i]) == 0xFFFFFFFF) continue;
            it.name = s_pickDrives[i];
            it.dir = 2;   // a drive
            it.size = 0;
            it.marked = 0;
            p->items.push_back(it);
        }
    }
    else
    {
        _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", p->path);
        pattern[sizeof(pattern) - 1] = 0;
        h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                ExpItem it;
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                it.name = fd.cFileName;
                it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                it.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                it.marked = 0;
                p->items.push_back(it);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::sort(p->items.begin(), p->items.end(), exp_less);
    }
    if (p->sel >= (int)p->items.size()) p->sel = p->items.empty() ? 0 : (int)p->items.size() - 1;
    if (p->sel < 0) p->sel = 0;
}

static void exp_size_text(unsigned long long n, char *out, size_t size)
{
    if (n >= 1024ull * 1024 * 1024) _snprintf(out, size - 1, "%.1f GB", n / (1024.0 * 1024 * 1024));
    else if (n >= 1024 * 1024) _snprintf(out, size - 1, "%.1f MB", n / (1024.0 * 1024));
    else if (n >= 1024) _snprintf(out, size - 1, "%.0f KB", n / 1024.0);
    else _snprintf(out, size - 1, "%u B", (unsigned)n);
    out[size - 1] = 0;
}

// ---- The transfer, on its own thread ----
static struct
{
    std::vector<std::string> src;   // full paths
    std::string dst;                // the folder they go into
    int move;
    volatile LONG running, cancel, failed;
    volatile unsigned long long total, done;
    char current[128];
} s_xfer;

static unsigned long long xfer_size(const std::string &path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    unsigned long long n = 0;
    DWORD a = GetFileAttributesA(path.c_str());
    if (a == 0xFFFFFFFF) return 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
    {
        h = FindFirstFileA(path.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        FindClose(h);
        return ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    }
    h = FindFirstFileA((path + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) n += xfer_size(path + "\\" + fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// A name that is not taken in `dir`: "name", else "name (2)" ... (before the extension).
static std::string xfer_free_name(const std::string &dir, const std::string &name)
{
    std::string base = name, ext;
    size_t dot = name.rfind('.');
    int k;
    if (GetFileAttributesA((dir + "\\" + name).c_str()) == 0xFFFFFFFF) return name;
    if (dot != std::string::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
    for (k = 2; k < 100; k++)
    {
        char n[16];
        std::string cand;
        sprintf(n, " (%d)", k);
        cand = base + n + ext;
        if (GetFileAttributesA((dir + "\\" + cand).c_str()) == 0xFFFFFFFF) return cand;
    }
    return name;
}

static int xfer_copy(const std::string &src, const std::string &dstDir, const std::string &name)
{
    static char buf[256 * 1024];
    std::string dst = dstDir + "\\" + name;
    DWORD a = GetFileAttributesA(src.c_str());
    if (s_xfer.cancel) return 0;
    if (a == 0xFFFFFFFF) return 0;
    if (a & FILE_ATTRIBUTE_DIRECTORY)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h;
        int ok = 1;
        CreateDirectoryA(dst.c_str(), NULL);
        h = FindFirstFileA((src + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 1;   // empty
        do
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) ok &= xfer_copy(src + "\\" + fd.cFileName, dst, fd.cFileName);
        while (ok && FindNextFileA(h, &fd));
        FindClose(h);
        return ok;
    }
    {
        HANDLE in = CreateFileA(src.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL), out;
        DWORD got, put;
        int ok = 1;
        if (in == INVALID_HANDLE_VALUE) return 0;
        out = CreateFileA(dst.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
        strncpy(s_xfer.current, name.c_str(), sizeof(s_xfer.current) - 1);
        s_xfer.current[sizeof(s_xfer.current) - 1] = 0;
        for (;;)
        {
            if (s_xfer.cancel) { ok = 0; break; }
            if (!ReadFile(in, buf, sizeof(buf), &got, NULL)) { ok = 0; break; }
            if (!got) break;
            if (!WriteFile(out, buf, got, &put, NULL) || put != got) { ok = 0; break; }
            s_xfer.done += got;
        }
        CloseHandle(in);
        CloseHandle(out);
        if (!ok) DeleteFileA(dst.c_str());   // no half file left behind
        return ok;
    }
}

static void xfer_delete(const std::string &path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    if (a == 0xFFFFFFFF) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) xfer_delete(path + "\\" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryA(path.c_str());
    }
    else
        DeleteFileA(path.c_str());
}

static DWORD WINAPI xfer_thread(LPVOID)
{
    size_t i;
    int ok = 1;
    s_xfer.total = 0;
    for (i = 0; i < s_xfer.src.size(); i++) s_xfer.total += xfer_size(s_xfer.src[i]);
    for (i = 0; i < s_xfer.src.size() && !s_xfer.cancel; i++)
    {
        const std::string &src = s_xfer.src[i];
        size_t slash = src.rfind('\\'), colon = src.find(':');
        std::string name = slash != std::string::npos ? src.substr(slash + 1) : src, dstName;
        dstName = xfer_free_name(s_xfer.dst, name);
        // A move on the same drive: renamed in place, no copying.
        if (s_xfer.move && colon != std::string::npos && s_xfer.dst.compare(0, colon + 1, src, 0, colon + 1) == 0 &&
            MoveFileA(src.c_str(), (s_xfer.dst + "\\" + dstName).c_str()))
        {
            s_xfer.done += xfer_size(s_xfer.dst + "\\" + dstName);
            continue;
        }
        if (!xfer_copy(src, s_xfer.dst, dstName)) { ok = 0; break; }
        if (s_xfer.move) xfer_delete(src);
    }
    InterlockedExchange(&s_xfer.failed, ok && !s_xfer.cancel ? 0 : 1);
    H64_INFO("[explorer] %s of %u item(s) into %s: %s", s_xfer.move ? "move" : "copy", (unsigned)s_xfer.src.size(), s_xfer.dst.c_str(),
             s_xfer.failed ? "not finished" : "done");
    InterlockedExchange(&s_xfer.running, 0);
    return 0;
}

static void exp_pane_draw(const ExpPane *p, float x, float y, float w, float h, int active, int rows)
{
    const float rowH = 36, listY = y + 70;
    char buf[200], fit[200];
    int i;
    if (active) edge_glow(x, y, w, h, 18);
    UiPanel(x, y, w, h, 18);
    _snprintf(buf, sizeof(buf) - 1, "%s", p->path[0] ? p->path : Tr(T_EXP_DRIVES));
    buf[sizeof(buf) - 1] = 0;
    UiTextFit(UI_BOLD, 21, w - 60, buf, fit, sizeof(fit));
    UiText(UI_BOLD, 21, x + 28, y + 36, active ? UI_COL_TEXT : UI_COL_TEXT2, fit, UI_LEFT);
    UiRect(x + 20, y + 58, w - 40, 1, UI_COL_LINE);
    if (p->items.empty())
    {
        UiText(UI_REGULAR, 19, x + w * 0.5f, listY + 60, UI_COL_TEXT3, Tr(T_EXP_EMPTY), UI_CENTER);
        return;
    }
    for (i = 0; i < rows && p->top + i < (int)p->items.size(); i++)
    {
        const ExpItem *it = &p->items[p->top + i];
        float ry = listY + i * rowH, cy = ry + rowH * 0.5f;
        int sel = p->top + i == p->sel;
        D3DCOLOR col = sel && active ? UI_COL_TEXT : UI_COL_TEXT2;
        if (sel && active) selection_bar(x + 12, ry + 2, w - 24, rowH - 4);
        else if (sel) UiRoundRing(x + 12, ry + 2, w - 24, rowH - 4, 10, 1.0f, UI_COL_LINE);
        if (it->marked)
        {
            // The mark: a tick in the selection's colour.
            UiLine(x + 22, cy, x + 26, cy + 4, 2.2f, SEL_HI(255));
            UiLine(x + 26, cy + 4, x + 33, cy - 5, 2.2f, SEL_HI(255));
        }
        if (it->dir) pick_icon(x + 40, cy, it->dir == 2, col);
        else
        {
            UiRoundRing(x + 44, cy - 10, 16, 20, 2, 1.5f, col);   // a page
            UiRect(x + 48, cy - 3, 8, 1.5f, col);
            UiRect(x + 48, cy + 2, 8, 1.5f, col);
        }
        UiTextFit(UI_REGULAR, 19, w - (it->dir ? 110 : 220), it->name.c_str(), fit, sizeof(fit));
        UiText(sel ? UI_BOLD : UI_REGULAR, 19, x + 74, cy, col, fit, UI_LEFT);
        if (!it->dir)
        {
            exp_size_text(it->size, buf, sizeof(buf));
            UiText(UI_REGULAR, 17, x + w - 26, cy, UI_COL_TEXT3, buf, UI_RIGHT);
        }
    }
}

// Pressed this frame (LT, RT: the triggers past half way, which UiInputPoll does not report).
static int exp_triggers(int *prev)
{
    XINPUT_STATE st;
    int now = 0, down;
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
        now = (st.Gamepad.bLeftTrigger > 128 ? 1 : 0) | (st.Gamepad.bRightTrigger > 128 ? 2 : 0);
    down = now & ~*prev;
    *prev = now;
    return down;
}

static int confirm_ask(IDirect3DDevice9 *dev, int title, int question, int footer);   // below, with the other questions

// Deletes a file, or a folder and everything in it. 1 when all of it went.
static int exp_delete(const std::string &path)
{
    DWORD at = GetFileAttributesA(path.c_str());
    int ok = 1;
    if (at == 0xFFFFFFFF) return 0;
    if (at & FILE_ATTRIBUTE_DIRECTORY)
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                ok &= exp_delete(path + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return ok && RemoveDirectoryA(path.c_str());
    }
    if (at & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path.c_str(), at & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileA(path.c_str()) != 0;
}

static void FileExplorer(IDirect3DDevice9 *dev)
{
    const float pw = 604, ph = 590, py = 34, gap = 24;
    const int rows = 14;
    static ExpPane panes[2];
    UiInput in;
    int active = 0, trigPrev = 3, i, clipMove = 0;
    std::vector<std::string> clip;
    char msg[200];
    DWORD msgAt = 0;
    msg[0] = 0;
    if (!panes[0].path[0] && !panes[1].path[0])
    {
        strcpy(panes[0].path, "game:");
        strcpy(panes[1].path, GetFileAttributesA("Hdd1:\\") != 0xFFFFFFFF ? "Hdd1:" : "");
    }
    exp_list(&panes[0]);
    exp_list(&panes[1]);
    UiInputInit(&in);
    exp_triggers(&trigPrev);
    H64_INFO("[explorer] opened");
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int trig = exp_triggers(&trigPrev);
        ExpPane *p = &panes[active];
        if (s_xfer.running)
        {
            if (down & XINPUT_GAMEPAD_B) InterlockedExchange(&s_xfer.cancel, 1);
        }
        else
        {
            if (msgAt && s_xfer.src.size() && !s_xfer.running && msgAt == 1)
            {
                // A transfer just ended: both panels read again, and the word for it.
                strcpy(msg, Tr(s_xfer.failed ? T_EXP_FAILED : T_EXP_DONE));
                msgAt = GetTickCount();
                xb_audio_ui_sound(XB_SFX_NOTIFY);
                s_xfer.src.clear();
                exp_list(&panes[0]);
                exp_list(&panes[1]);
            }
            if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
                active = (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0;
            p = &panes[active];
            if (!p->items.empty())
            {
                if (down & XINPUT_GAMEPAD_DPAD_UP) p->sel = (p->sel + (int)p->items.size() - 1) % (int)p->items.size();
                if (down & XINPUT_GAMEPAD_DPAD_DOWN) p->sel = (p->sel + 1) % (int)p->items.size();
                if (down & XINPUT_GAMEPAD_DPAD_LEFT) p->sel = p->sel > rows ? p->sel - rows : 0;
                if (down & XINPUT_GAMEPAD_DPAD_RIGHT) p->sel = p->sel + rows < (int)p->items.size() ? p->sel + rows : (int)p->items.size() - 1;
            }
            if ((down & XINPUT_GAMEPAD_A) && !p->items.empty() && p->items[p->sel].dir)
            {
                // Into the folder (or the drive).
                if (p->items[p->sel].dir == 2) _snprintf(p->path, sizeof(p->path) - 1, "%s", p->items[p->sel].name.c_str());
                else
                {
                    char next[260];
                    _snprintf(next, sizeof(next) - 1, "%s\\%s", p->path, p->items[p->sel].name.c_str());
                    next[sizeof(next) - 1] = 0;
                    strcpy(p->path, next);
                }
                p->path[sizeof(p->path) - 1] = 0;
                p->sel = p->top = 0;
                exp_list(p);
            }
            else if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))
            {
                if (!p->path[0] || (down & XINPUT_GAMEPAD_BACK)) break;   // from the drives (or BACK): out
                {
                    // Up a folder; from a drive's top, the drives - the folder left stays under the cursor.
                    char *slash = strrchr(p->path, '\\');
                    std::string left = slash ? slash + 1 : p->path;
                    size_t k;
                    if (slash) *slash = 0;
                    else p->path[0] = 0;
                    exp_list(p);
                    for (k = 0; k < p->items.size(); k++)
                        if (!_stricmp(p->items[k].name.c_str(), left.c_str())) p->sel = (int)k;
                }
            }
            if ((down & XINPUT_GAMEPAD_X) && p->path[0] && !p->items.empty())
            {
                p->items[p->sel].marked = !p->items[p->sel].marked;
                if (p->sel + 1 < (int)p->items.size()) p->sel++;   // on to the next, to mark several in a row
            }
            if (((down & XINPUT_GAMEPAD_Y) || (trig & 1)) && p->path[0] && !p->items.empty())
            {
                // Copy (Y) or cut (LT): the marked items, or the one under the cursor.
                clip.clear();
                for (i = 0; i < (int)p->items.size(); i++)
                    if (p->items[i].marked) clip.push_back(std::string(p->path) + "\\" + p->items[i].name);
                if (clip.empty()) clip.push_back(std::string(p->path) + "\\" + p->items[p->sel].name);
                for (i = 0; i < (int)p->items.size(); i++) p->items[i].marked = 0;
                clipMove = (trig & 1) != 0;
                _snprintf(msg, sizeof(msg) - 1, Tr(clipMove ? T_EXP_CLIP_MOVE : T_EXP_CLIP_COPY), (int)clip.size());
                msg[sizeof(msg) - 1] = 0;
                msgAt = GetTickCount();
            }
            if ((down & XINPUT_GAMEPAD_START) && p->path[0] && !p->items.empty())
            {
                // Delete (START): the marked items, or the one under the cursor - after asking.
                std::vector<std::string> del;
                for (i = 0; i < (int)p->items.size(); i++)
                    if (p->items[i].marked) del.push_back(std::string(p->path) + "\\" + p->items[i].name);
                if (del.empty()) del.push_back(std::string(p->path) + "\\" + p->items[p->sel].name);
                if (confirm_ask(dev, T_EXP_DEL_TITLE, T_EXP_DEL_ASK, T_FOOT_EXP_DEL))
                {
                    size_t k;
                    int gone = 0, all = 1;
                    for (k = 0; k < del.size(); k++)
                    {
                        int ok = exp_delete(del[k]);
                        gone += ok;
                        all &= ok;
                        H64_INFO("[explorer] delete %s: %s", del[k].c_str(), ok ? "done" : "failed");
                    }
                    if (all) _snprintf(msg, sizeof(msg) - 1, Tr(T_EXP_DELETED), gone);
                    else _snprintf(msg, sizeof(msg) - 1, "%s", Tr(T_EXP_DEL_FAILED));
                    msg[sizeof(msg) - 1] = 0;
                    msgAt = GetTickCount();
                    exp_list(&panes[0]);
                    exp_list(&panes[1]);
                    if (p->sel >= (int)p->items.size()) p->sel = p->items.empty() ? 0 : (int)p->items.size() - 1;
                }
                UiInputInit(&in);
            }
            if ((trig & 2) && p->path[0] && !clip.empty())
            {
                // Paste into this panel's folder - never a folder into itself.
                size_t k;
                std::string dst = p->path;
                for (k = 0; k < clip.size(); k++)
                {
                    const std::string &s = clip[k];
                    if (!_strnicmp(dst.c_str(), s.c_str(), s.size()) && (dst.size() == s.size() || dst[s.size()] == '\\')) break;
                }
                if (k < clip.size())
                {
                    strcpy(msg, Tr(T_EXP_INTO_ITSELF));
                    msgAt = GetTickCount();
                }
                else
                {
                    HANDLE t;
                    s_xfer.src = clip;
                    s_xfer.dst = dst;
                    s_xfer.move = clipMove;
                    s_xfer.done = s_xfer.total = 0;
                    s_xfer.current[0] = 0;
                    s_xfer.cancel = s_xfer.failed = 0;
                    s_xfer.running = 1;
                    t = CreateThread(NULL, 64 * 1024, xfer_thread, NULL, 0, NULL);
                    if (t) CloseHandle(t);
                    else s_xfer.running = 0;
                    if (clipMove) clip.clear();   // moved: they are not where they were any more
                    msg[0] = 0;
                    msgAt = 1;   // the end of the transfer shows its word (above)
                }
            }
        }
        // Keep the cursor in view.
        for (i = 0; i < 2; i++)
        {
            ExpPane *q = &panes[i];
            if (q->sel < q->top) q->top = q->sel;
            if (q->sel >= q->top + rows) q->top = q->sel - rows + 1;
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        exp_pane_draw(&panes[0], (UI_WIDTH - 2 * pw - gap) * 0.5f, py, pw, ph, active == 0, rows);
        exp_pane_draw(&panes[1], (UI_WIDTH + gap) * 0.5f, py, pw, ph, active == 1, rows);
        if (s_xfer.running)
        {
            // The transfer: what, how far, and B to stop it.
            const float w = 640, h = 190, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
            unsigned long long tot = s_xfer.total, done = s_xfer.done;
            float f = tot ? (float)((double)done / (double)tot) : 0.0f;
            char a[32], b[32], line[96], fit[160];
            UiDim();
            edge_glow(x, y, w, h, 20);
            UiPanel(x, y, w, h, 20);
            UiText(UI_BOLD, 26, x + 36, y + 40, UI_COL_TEXT, Tr(s_xfer.move ? T_EXP_MOVING : T_EXP_COPYING), UI_LEFT);
            UiTextFit(UI_REGULAR, 18, w - 72, s_xfer.current, fit, sizeof(fit));
            UiText(UI_REGULAR, 18, x + 36, y + 76, UI_COL_TEXT2, fit, UI_LEFT);
            if (f > 1.0f) f = 1.0f;
            UiRoundRect(x + 36, y + 102, w - 72, 12, 6, D3DCOLOR_ARGB(50, 255, 255, 255), D3DCOLOR_ARGB(35, 255, 255, 255));
            UiRoundRect(x + 36, y + 102, (w - 72) * f, 12, 6, SEL_HI(255), SEL_LO(255));
            exp_size_text(done, a, sizeof(a));
            exp_size_text(tot, b, sizeof(b));
            _snprintf(line, sizeof(line) - 1, "%s / %s  (%d%%)", a, b, (int)(f * 100));
            line[sizeof(line) - 1] = 0;
            UiText(UI_REGULAR, 17, x + 36, y + 134, UI_COL_TEXT3, line, UI_LEFT);
            UiFooter(x + w - 36 - UiFooterWidth(Tr(T_FOOT_EXP_CANCEL)), y + 160, Tr(T_FOOT_EXP_CANCEL));
        }
        if (msg[0] && msgAt > 1)
        {
            DWORD age = GetTickCount() - msgAt;
            if (age < 3000) UiToast(msg, (float)age, 3000.0f);
            else msg[0] = 0;
        }
        UiScreenFooter(Tr(T_FOOT_EXPLORER));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    while (s_xfer.running) { InterlockedExchange(&s_xfer.cancel, 1); Sleep(20); }   // never leave one running behind
    H64_INFO("[explorer] closed");
}

// ---- Sound page: Volumen (opens the bars page above) and Margen de audio
// (an inline "< N ms >" cycle, moved here from the main page).
static void settings_sound(UiMenu *m, const Config *c)
{
    s_soundPage = 1;
    UiMenuClear(m, Tr(T_SOUND), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_VOLUME), "");
    UiMenuAdd(m, Tr(T_AUDIO_MARGIN), "");
    UiMenuAdd(m, Tr(T_AUDIO_OUTPUT), "");
    if (s_browser)
    {
        // The menus' own sounds and music: nothing to hear of them in game.
        UiMenuAdd(m, Tr(T_UI_SOUNDS), "");
        UiMenuAdd(m, Tr(T_SFX_STYLE), "");
        UiMenuAdd(m, Tr(T_MUSIC), "");
    }
    {
        static const unsigned char icons[] = { UI_ICON_SOUND, UI_ICON_CLOCK, UI_ICON_SPEAKERS, UI_ICON_PLAY, UI_ICON_SLIDERS, UI_ICON_MUSIC };
        UiMenuIcons(m, icons, sizeof(icons));
    }
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
    if (m->sel == SND_UISOUNDS && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->uiSounds = !c->uiSounds;
        xb_audio_ui_enable(c->uiSounds);
        if (c->uiSounds) xb_audio_ui_sound(XB_SFX_SELECT);   // heard at once, now that it is on
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SND_MONO && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->audioMono = !c->audioMono;
        if (!s_kodiSim) xb_audio_set_mono(c->audioMono);
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SND_SFXSTYLE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        c->uiSoundStyle = ((c->uiSoundStyle >= 0 && c->uiSoundStyle < XB_SFX_STYLE_COUNT ? c->uiSoundStyle : 0) + (dir ? dir : 1) +
                           XB_SFX_STYLE_COUNT) % XB_SFX_STYLE_COUNT;
        xb_audio_ui_style(c->uiSoundStyle);
        xb_audio_ui_sound(XB_SFX_SELECT);   // heard at once in the new style
        settings_values(m, c);
        return SET_CHANGED;
    }
    if (m->sel == SND_MUSIC && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // No music, in order, at random, then each track of game:\music\ on its own.
        int n = xb_audio_music_scan(MUSIC_DIR), states = 3 + n, k;
        int cur = c->musicMode == XB_MUSIC_OFF ? 0 : c->musicMode == XB_MUSIC_RANDOM ? 2 : 1;
        if (!dir) dir = 1;
        if (c->musicMode == XB_MUSIC_TRACK)
            for (k = 0; k < n; k++)
                if (!_stricmp(xb_audio_music_track(k), c->musicTrack)) cur = 3 + k;
        cur = (cur + dir + states) % states;
        c->musicMode = cur == 0 ? XB_MUSIC_OFF : cur == 1 ? XB_MUSIC_ORDER : cur == 2 ? XB_MUSIC_RANDOM : XB_MUSIC_TRACK;
        if (cur >= 3)
        {
            strncpy(c->musicTrack, xb_audio_music_track(cur - 3), sizeof(c->musicTrack) - 1);
            c->musicTrack[sizeof(c->musicTrack) - 1] = 0;
        }
        if (s_browser && !s_kodiSim) xb_audio_music_start(MUSIC_DIR, c->musicMode, c->musicTrack);   // heard at once (no music in game)
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

static void settings_graphics(UiMenu *m, const Config *c)
{
    s_graphicsPage = 1;
    s_gfxPick = -1;   // the preset as the settings say (the Screen page may have changed them)
    UiMenuClear(m, Tr(T_GRAPHICS), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PRESET), "");
    UiMenuAdd(m, Tr(T_TEXFILTER), "");
    UiMenuAdd(m, Tr(T_HIRESTEX), "");
    UiMenuAdd(m, Tr(T_FXAA), "");
    UiMenuAdd(m, Tr(T_SHARPEN), "");
    UiMenuAdd(m, Tr(T_BLUR), "");
    UiMenuAdd(m, Tr(T_ANTIFLICKER), "");
    UiMenuAdd(m, Tr(T_SCREEN_EFFECT), "");
    {
        static const unsigned char icons[] = { UI_ICON_SLIDERS, UI_ICON_TEXTURE, UI_ICON_HD, UI_ICON_SMOOTH, UI_ICON_SHARP, UI_ICON_BLUR, UI_ICON_EYE,
                                               UI_ICON_SCANLINES };
        UiMenuIcons(m, icons, sizeof(icons));
    }
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
    UiMenuAdd(m, Tr(T_RES_169), "");
    UiMenuAdd(m, Tr(T_STRETCH), "");
    UiMenuAdd(m, Tr(T_PIC_CAL), "");
    UiMenuAdd(m, Tr(T_BRIGHTNESS), "");
    UiMenuAdd(m, Tr(T_COLOR_STYLE), "");
    UiMenuAdd(m, Tr(T_VSYNC), "");
    {
        static const unsigned char icons[] = { UI_ICON_SCREEN, UI_ICON_SCREEN, UI_ICON_STRETCH, UI_ICON_CROSSHAIR, UI_ICON_SUN, UI_ICON_PALETTE, UI_ICON_VSYNC };
        UiMenuIcons(m, icons, sizeof(icons));
    }
    settings_values(m, c);
}

// ---- Image calibration page: size, left/right, up/down - opened from Screen > SCR_CAL.
static void settings_cal(UiMenu *m, const Config *c)
{
    s_calPage = 1;
    UiMenuClear(m, Tr(T_PIC_CAL), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_PIC_SIZE), "");
    UiMenuAdd(m, Tr(T_CAL_EDGES), "");
    {
        static const unsigned char icons[] = { UI_ICON_FRAME, UI_ICON_CROSSHAIR };
        UiMenuIcons(m, icons, sizeof(icons));
    }
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
        case SCR_RES: res_step(c, 0, dir); break;
        case SCR_RES169: res_step(c, 1, dir); break;
        case SCR_STRETCH: c->stretch = !c->stretch; break;
        case SCR_BRIGHT:   // the bar, 5 at a time (50: as the console)
            c->brightLevel = (c->brightLevel < 0 || c->brightLevel > 100 ? 50 : c->brightLevel) + dir * 5;
            if (c->brightLevel < 0) c->brightLevel = 0;
            if (c->brightLevel > 100) c->brightLevel = 100;
            break;
        case SCR_COLOR: c->colorStyle = ((c->colorStyle < 0 || c->colorStyle > 7 ? 0 : c->colorStyle) + dir + 8) % 8; break;
        case SCR_VSYNC: c->vsync = c->vsync == 2 ? 0 : 2; break;
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
    if (m->sel == CAL_EDGES && (down & XINPUT_GAMEPAD_A))
    {
        if (!s_kodiSim) settings_edges(m, c);
        return SET_NONE;
    }
    if (m->sel == CAL_SIZE && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        // 80 % to 140 %, 5 at a time, round the ends (its list).
        int z = (c->zoom < 80 ? 80 : c->zoom > 140 ? 140 : c->zoom) - 80;
        z = (z / 5 + (dir ? dir : 1) + 13) % 13;
        c->zoom = 80 + z * 5;
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

// ---- Edge calibration (Image calibration > Calibrate the edges) ----
// The picture as the games are shown (in game: the game itself; from the ROM
// list: a sample scene) with its four edges lit: the D-pad or the left stick
// moves it, LT/RT (or the right stick) zoom it, until the edges sit just inside
// the TV. A keeps it, B puts it back as it was.
static const Config *s_edgeCfg;
static int s_edgeSave[3];   // zoom, offsetX, offsetY when it opened
static float s_edgeAccX, s_edgeAccY, s_edgeAccZ;

static void settings_edges(UiMenu *m, const Config *c)
{
    UiMenuClear(m, Tr(T_CAL_EDGES), Tr(T_FOOT_EDGES));
    m->page = UI_PAGE_EDGES;
    m->count = 0;
    s_edgeCfg = c;
    s_edgeSave[0] = c->zoom; s_edgeSave[1] = c->offsetX; s_edgeSave[2] = c->offsetY;
    s_edgeAccX = s_edgeAccY = s_edgeAccZ = 0;
}

static int edges_input(UiMenu *m, Config *c, WORD down)
{
    XINPUT_STATE st;
    int ox = c->offsetX, oy = c->offsetY, z = c->zoom;
    if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B))
    {
        if (down & XINPUT_GAMEPAD_B) { c->zoom = s_edgeSave[0]; c->offsetX = s_edgeSave[1]; c->offsetY = s_edgeSave[2]; }
        else xb_audio_ui_sound(XB_SFX_SELECT);
        settings_cal(m, c);
        m->sel = CAL_EDGES;
        return SET_CHANGED;
    }
    if (down & XINPUT_GAMEPAD_DPAD_LEFT) c->offsetX -= 2;
    if (down & XINPUT_GAMEPAD_DPAD_RIGHT) c->offsetX += 2;
    if (down & XINPUT_GAMEPAD_DPAD_UP) c->offsetY -= 2;
    if (down & XINPUT_GAMEPAD_DPAD_DOWN) c->offsetY += 2;
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
    {
        // The sticks and triggers: smooth, a few pixels (or a percent) a frame at full tilt.
        const float lx = st.Gamepad.sThumbLX / 32767.0f, ly = st.Gamepad.sThumbLY / 32767.0f, ry = st.Gamepad.sThumbRY / 32767.0f;
        if (fabsf(lx) > 0.25f) s_edgeAccX += lx * 3.0f;
        if (fabsf(ly) > 0.25f) s_edgeAccY -= ly * 3.0f;
        if (fabsf(ry) > 0.25f) s_edgeAccZ += ry * 0.4f;
        if (st.Gamepad.bRightTrigger > 40) s_edgeAccZ += st.Gamepad.bRightTrigger / 255.0f * 0.4f;
        if (st.Gamepad.bLeftTrigger > 40) s_edgeAccZ -= st.Gamepad.bLeftTrigger / 255.0f * 0.4f;
        while (s_edgeAccX >= 1) { c->offsetX++; s_edgeAccX -= 1; }
        while (s_edgeAccX <= -1) { c->offsetX--; s_edgeAccX += 1; }
        while (s_edgeAccY >= 1) { c->offsetY++; s_edgeAccY -= 1; }
        while (s_edgeAccY <= -1) { c->offsetY--; s_edgeAccY += 1; }
        while (s_edgeAccZ >= 1) { c->zoom++; s_edgeAccZ -= 1; }
        while (s_edgeAccZ <= -1) { c->zoom--; s_edgeAccZ += 1; }
    }
    if (c->offsetX < -160) c->offsetX = -160;
    if (c->offsetX > 160) c->offsetX = 160;
    if (c->offsetY < -120) c->offsetY = -120;
    if (c->offsetY > 120) c->offsetY = 120;
    if (c->zoom < 80) c->zoom = 80;
    if (c->zoom > 140) c->zoom = 140;
    (void)m;
    return ox != c->offsetX || oy != c->offsetY || z != c->zoom ? SET_CHANGED : SET_NONE;
}

static void wiz_video_preview(const Config *c, float x, float y, float w, float h, float a);

static void edges_draw(const UiMenu *m)
{
    const Config *c = s_edgeCfg;
    float w, h, x, y, t = UiTime(), on = 0.65f + 0.35f * sinf(t * 4.0f), k;
    char buf[64];
    int i;
    if (!c) return;
    // Where the renderer puts the picture (display_quad): 4:3, or the whole width when 16:9.
    w = c->resWide || c->stretch ? (float)UI_WIDTH : UI_HEIGHT * 4.0f / 3.0f;
    h = (float)UI_HEIGHT;
    w *= c->zoom / 100.0f;
    h *= c->zoom / 100.0f;
    x = (UI_WIDTH - w) * 0.5f + c->offsetX;
    y = (UI_HEIGHT - h) * 0.5f + c->offsetY;
    if (s_browser)
    {
        // No game behind: a sample scene where the game would be.
        UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(255, 0, 0, 0));
        wiz_video_preview(c, x, y, w, h, 1.0f);
    }
    // The four edges, lit and breathing, each with an arrow out of its middle.
    for (i = 1; i <= 6; i++)
    {
        float f = 1.0f - i / 7.0f;
        UiRoundRing(x - i * 2, y - i * 2, w + i * 4, h + i * 4, 4 + i * 2, 2.0f, SEL_HI(70 * on * f));
    }
    UiRect(x, y, w, 4, SEL_HI(255 * on)); UiRect(x, y + h - 4, w, 4, SEL_HI(255 * on));
    UiRect(x, y, 4, h, SEL_HI(255 * on)); UiRect(x + w - 4, y, 4, h, SEL_HI(255 * on));
    k = 30;   // white corner brackets
    UiRect(x, y, k, 6, UI_COL_TEXT); UiRect(x, y, 6, k, UI_COL_TEXT);
    UiRect(x + w - k, y, k, 6, UI_COL_TEXT); UiRect(x + w - 6, y, 6, k, UI_COL_TEXT);
    UiRect(x, y + h - 6, k, 6, UI_COL_TEXT); UiRect(x, y + h - k, 6, k, UI_COL_TEXT);
    UiRect(x + w - k, y + h - 6, k, 6, UI_COL_TEXT); UiRect(x + w - 6, y + h - k, 6, k, UI_COL_TEXT);
    icon_tri(x + w * 0.5f, y + 22, 12, 0, UI_COL_TEXT);
    icon_tri(x + w * 0.5f, y + h - 22, 12, 2, UI_COL_TEXT);
    icon_tri(x + 22, y + h * 0.5f, 12, 3, UI_COL_TEXT);
    icon_tri(x + w - 22, y + h * 0.5f, 12, 1, UI_COL_TEXT);
    {
        // In the middle: what to do, the values, the buttons.
        const float pw = 640, ph = 150, px = (UI_WIDTH - pw) * 0.5f, py = (UI_HEIGHT - ph) * 0.5f;
        float fw = UiFooterWidth(m->footer);
        UiShadow(px, py + 8, pw, ph, 18, 28, UI_COL_SHADOW);
        UiRoundRect(px, py, pw, ph, 18, D3DCOLOR_ARGB(225, 24, 26, 34), D3DCOLOR_ARGB(225, 18, 20, 27));
        UiRoundRing(px, py, pw, ph, 18, 1.0f, SEL_HI(160));
        paragraph(UI_BOLD, 19, px + 28, py + 26, pw - 56, 2, UI_COL_TEXT, Tr(T_EDGES_HINT));
        _snprintf(buf, sizeof(buf) - 1, "Zoom %d%%   X %+d   Y %+d", c->zoom, c->offsetX, c->offsetY);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 18, px + pw * 0.5f, py + 84, UI_COL_TEXT2, buf, UI_CENTER);
        UiFooter(px + (pw - fw) * 0.5f, py + ph - 28, m->footer);
    }
}

// ---- Developer mode page (System > Developer mode) ----
static void settings_dev(UiMenu *m, const Config *c)
{
    s_systemPage = 0;
    s_devPage = 1;
    UiMenuClear(m, Tr(T_DEV_MODE), Tr(T_FOOT_SELECT));
    UiMenuAdd(m, Tr(T_GFX_LOG), "");
    {
        static const unsigned char icons[] = { UI_ICON_CHART };
        UiMenuIcons(m, icons, sizeof(icons));
    }
    settings_values(m, c);
}

static int dev_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        s_devPage = 0;
        settings_system(m, c);
        m->sel = SYS_DEV;
        return SET_NONE;
    }
    if (!dir && !(down & XINPUT_GAMEPAD_A)) return SET_NONE;
    if (m->sel == DEV_GFXLOG)
    {
        c->gfxLog = !c->gfxLog;
        settings_values(m, c);
    }
    return SET_CHANGED;
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

static void settings_hotkeys(UiMenu *m, const Config *c);

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
        // The screenshot combo became one of the button shortcuts: their own page.
        settings_hotkeys(m, c);
        return SET_NONE;
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
        c->rumble = c->rumble > 0 ? 0 : 100;   // a switch, for every player (the setting keeps a strength: off 0, on 100)
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
    if (item == C_RUMBLE) { sprintf(out, "%s", toggle(s_ctl.c ? s_ctl.c->rumble > 0 : 1)); return; }   // a switch
    const char *sticks[H64_STICK_COUNT] = { Tr(T_STICK_LEFT), Tr(T_STICK_RIGHT), Tr(T_STICK_DPAD), Tr(T_OFF) };
    if (item == C_STICK) sprintf(out, "< %s >", sticks[map->stick >= 0 && map->stick < H64_STICK_COUNT ? map->stick : 0]);
    else if (item == C_SHOTCOMBO) out[0] = 0;   // opens the shortcuts page
    else out[0] = 0;
}

// ---- The N64 controller drawn on the Controls page ----
// The picture of the controller (xb_pad.h, from n64.jpg: its background
// transparent, no maker's logo), as big as a box allows; the control being
// edited glows white, and blinks while it waits for an input.
static IDirect3DTexture9 *s_padTex;
static int s_padTried;

// Where `item` is, in the source picture's pixels (1500x1500, UI_PAD_SRC_*):
// a box and its corner radius; 0: not on it. Z is on the back: marked on the
// middle handle, where the finger finds it.
static int pad_shape(int item, float *x, float *y, float *w, float *h, float *r)
{
    static const float circles[][4] = {   // item, centre x, y, radius
        { H64_N64_A, 1048, 630, 47 }, { H64_N64_B, 968, 545, 47 }, { H64_N64_START, 748, 557, 44 },
        { H64_N64_CUP, 1135, 412, 35 }, { H64_N64_CDOWN, 1135, 548, 35 },
        { H64_N64_CLEFT, 1067, 480, 35 }, { H64_N64_CRIGHT, 1203, 480, 35 },
        { C_STICK, 750, 777, 118 }, { C_CALIB, 750, 777, 118 },
    };
    static const float boxes[][6] = {     // item, x, y, w, h, radius
        { H64_N64_DUP, 358, 428, 70, 62, 10 }, { H64_N64_DDOWN, 358, 548, 70, 62, 10 },
        { H64_N64_DLEFT, 298, 485, 62, 66, 10 }, { H64_N64_DRIGHT, 428, 485, 62, 66, 10 },
        { H64_N64_L, 282, 262, 228, 60, 22 }, { H64_N64_R, 990, 262, 228, 60, 22 },
        { H64_N64_Z, 708, 975, 84, 66, 20 },
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

static void draw_n64_pad(float bx, float by, float bw, float bh, int hiItem, int blink)
{
    float pw = bw, ph = pw * UI_PAD_H / UI_PAD_W, k, ox, oy, x, y, w, h, r;
    if (ph > bh) { ph = bh; pw = ph * UI_PAD_W / UI_PAD_H; }
    ox = bx + (bw - pw) * 0.5f;
    oy = by + (bh - ph) * 0.5f;
    k = pw / UI_PAD_SRC_W;   // screen pixels per source pixel
    if (!s_padTried && UiDevice())
    {
        int iw = 0, ih = 0;
        u8 *px = h64_png_decode(s_padPng, sizeof(s_padPng), &iw, &ih, UI_PAD_W * UI_PAD_H);
        s_padTried = 1;
        if (px) { s_padTex = UiImageCreate(UiDevice(), px, iw, ih); free(px); }
    }
#define PX(v) (ox + ((v) - UI_PAD_SRC_X) * k)
#define PY(v) (oy + ((v) - UI_PAD_SRC_Y) * k)
    UiShadow(ox + pw * 0.06f, oy + ph * 0.08f, pw * 0.88f, ph * 0.86f, pw * 0.12f, 22, UI_COL_SHADOW);
    if (s_padTex) UiImage(s_padTex, ox, oy, pw, ph, 0.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
    // The shoulders and Z by name (not printed on the controller).
    {
        const float fs = 34 * k < 11 ? 11 : 34 * k;
        const D3DCOLOR ink = D3DCOLOR_ARGB(200, 70, 72, 82);
        UiText(UI_BOLD, fs, PX(396), PY(292), ink, "L", UI_CENTER);
        UiText(UI_BOLD, fs, PX(1104), PY(292), ink, "R", UI_CENTER);
        UiRoundRect(PX(722), PY(985), 56 * k, 46 * k, 14 * k, D3DCOLOR_ARGB(150, 90, 92, 104), D3DCOLOR_ARGB(150, 70, 72, 82));
        UiText(UI_BOLD, fs, PX(750), PY(1008), D3DCOLOR_ARGB(235, 235, 237, 243), "Z", UI_CENTER);
    }
    // The control looked at: white with a glow round it, breathing; faster and filled
    // while it waits for an input.
    if (pad_shape(hiItem, &x, &y, &w, &h, &r))
    {
        float t = UiTime(), on = blink ? 0.5f + 0.5f * sinf(t * 12.566f) : 0.7f + 0.3f * sinf(t * 4.0f), pad = 2.5f;
        int i;
        x = PX(x); y = PY(y); w *= k; h *= k; r *= k;
        for (i = 1; i <= 9; i++)
        {
            float f = 1.0f - i / 10.0f;
            UiRoundRing(x - pad - i, y - pad - i, w + (pad + i) * 2, h + (pad + i) * 2, r + pad + i, 1.6f,
                        D3DCOLOR_ARGB((int)(90 * on * f * f), 255, 255, 255));
        }
        if (blink) UiRoundRect(x, y, w, h, r, D3DCOLOR_ARGB((int)(120 * on), 255, 255, 255), D3DCOLOR_ARGB((int)(80 * on), 255, 255, 255));
        UiRoundRing(x - pad, y - pad, w + pad * 2, h + pad * 2, r + pad, 2.2f, D3DCOLOR_ARGB((int)(140 + 115 * on), 255, 255, 255));
    }
#undef PX
#undef PY
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
    const float w = 820, h = 420, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
    char buf[200];
    UiDim();
    edge_glow(x, y, w, h, 22);
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
        const float padW = 300, padX = x + w - 40 - padW, padY = y + 92, padH = h - 78 - 92 - 10;
        const float textW = padX - 24 - (x + 40);
        const char *name = TrN64(s_ctl.capItem);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_AUTO_STEP), s_ctl.autoIdx + 1, AUTO_COUNT);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 18, x + w - 40, y + 50, UI_COL_TEXT3, buf, UI_RIGHT);
        _snprintf(buf, sizeof(buf) - 1, Tr(T_PRESS_FOR), name);
        buf[sizeof(buf) - 1] = 0;
        ctl_fit_text(UI_REGULAR, 20, x + 40, y + 140, textW, UI_COL_TEXT2, buf);
        ctl_fit_text(UI_BOLD, 56, x + 40, y + 200, textW, UI_COL_ACCENT_HI, name);
        draw_n64_pad(padX, padY, padW, padH, s_ctl.capItem, 1);
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
    edge_glow(x, y, CTL_W, CTL_H, 22);
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
            static const int labels[] = { T_ANALOG_STICK, T_CALIBRATION, T_HOTKEYS, T_RUMBLE, T_RESET_DEFAULT, T_AUTO_CONFIG };
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
        float top = ctl_row_y(2) + 8, bottom = ctl_row_y(9) - 8;
        draw_n64_pad(ctl_col_x(2), top, CTL_COLW, bottom - top, s_ctl.capturing ? s_ctl.capItem : selItem, s_ctl.capturing);
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
    edge_glow(x, y, CTL_W, CTL_H, 22);
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

// ---- Button shortcuts page (Controls > Button shortcuts, A) ----
// The in-game shortcuts that can be changed (Config::hotkey, HK_*), then the fixed
// ones for reference; under them what the selected one does. A on a changeable one
// waits for a combination: up to 3 buttons held, then let go.
enum { HKR_PAUSE = HK_COUNT, HKR_LIST, HKR_UISHOT, HKR_TEX, HKR_COUNT };
static const int s_hkLabels[HKR_COUNT] = { T_HK_SAVE, T_HK_LOAD, T_HK_SLOT_PREV, T_HK_SLOT_NEXT, T_HK_SHOT,
                                           T_HK_PAUSE, T_HK_LIST, T_HK_UISHOT, T_HK_TEX };
static const int s_hkDesc[HKR_COUNT] = { T_HK_D_SAVE, T_HK_D_LOAD, T_HK_D_SLOT, T_HK_D_SLOT, T_HK_D_SHOT,
                                         T_HK_D_PAUSE, T_HK_D_LIST, T_HK_D_UISHOT, T_HK_D_TEX };
static const Config *s_hkCfg;
static int s_hkCap = -1;          // the shortcut waiting for its combination (-1: none)
static int s_hkCapReleased;       // every button let go since A started it
static u32 s_hkAcc;               // the buttons held so far
static DWORD s_hkStart;
static char s_hkNote[160];        // a word after a change (taken from another shortcut, too many buttons)

// The buttons of a combination by name, "BACK + RB" (the pad sources' names: the Controls page's).
static void hk_text(u32 m, char *out, size_t size)
{
    static const u32 bits[] = { 0x0020, 0x0010, 0x0100, 0x0200, HK_LT, HK_RT, 0x1000, 0x2000, 0x4000, 0x8000,
                                0x0001, 0x0002, 0x0004, 0x0008, 0x0040, 0x0080 };
    static const int src[] = { -1, H64_SRC_START, H64_SRC_LB, H64_SRC_RB, H64_SRC_LT, H64_SRC_RT, H64_SRC_A, H64_SRC_B, H64_SRC_X,
                               H64_SRC_Y, H64_SRC_DUP, H64_SRC_DDOWN, H64_SRC_DLEFT, H64_SRC_DRIGHT, H64_SRC_LTHUMB, H64_SRC_RTHUMB };
    int k;
    out[0] = 0;
    if (!m) { _snprintf(out, size - 1, "%s", Tr(T_OFF)); out[size - 1] = 0; return; }
    for (k = 0; k < (int)(sizeof(bits) / sizeof(bits[0])); k++)
    {
        if (!(m & bits[k])) continue;
        if (out[0]) strncat(out, " + ", size - strlen(out) - 1);
        strncat(out, src[k] < 0 ? "BACK" : TrSource(src[k]), size - strlen(out) - 1);
    }
}

static int hk_count_bits(u32 m)
{
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static void settings_hotkeys(UiMenu *m, const Config *c)
{
    UiMenuClear(m, Tr(T_HOTKEYS), Tr(T_FOOT_HOTKEYS));
    m->page = UI_PAGE_HOTKEYS;
    m->count = HKR_COUNT;
    m->sel = 0;
    s_hkCfg = c;
    s_hkCap = -1;
    s_hkNote[0] = 0;
}

static int hk_input(UiMenu *m, Config *c, WORD down)
{
    s_hkCfg = c;
    if (s_hkCap >= 0)
    {
        // Waiting for the combination: first everything let go (the A that started
        // it), then the buttons held are gathered until all are let go again.
        XINPUT_STATE st;
        u32 b = 0;
        if (XInputGetState(0, &st) == ERROR_SUCCESS)
        {
            b = st.Gamepad.wButtons;
            if (st.Gamepad.bLeftTrigger > 128) b |= HK_LT;
            if (st.Gamepad.bRightTrigger > 128) b |= HK_RT;
        }
        if (!s_hkCapReleased)
        {
            if (!b) s_hkCapReleased = 1;
            return SET_NONE;
        }
        if (b) { s_hkAcc |= b; return SET_NONE; }
        if (!s_hkAcc)
        {
            if (GetTickCount() - s_hkStart > 8000) { s_hkCap = -1; strcpy(s_hkNote, Tr(T_NO_INPUT)); }
            return SET_NONE;
        }
        if (hk_count_bits(s_hkAcc) > 3)
        {
            strcpy(s_hkNote, Tr(T_HK_MANY));
            s_hkCap = -1;
            return SET_NONE;
        }
        {
            int k;
            s_hkNote[0] = 0;
            for (k = 0; k < HK_COUNT; k++)
                if (k != s_hkCap && c->hotkey[k] == s_hkAcc)
                {
                    c->hotkey[k] = 0;   // one combination, one shortcut
                    strcpy(s_hkNote, Tr(T_HK_TAKEN));
                }
        }
        c->hotkey[s_hkCap] = s_hkAcc;
        s_hkCap = -1;
        return SET_CHANGED;
    }
    UiMenuNavigate(m, down);
    if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN)) s_hkNote[0] = 0;   // the next row's own text
    if (down & XINPUT_GAMEPAD_B)
    {
        int i;
        controls_open(m, c);
        for (i = 0; i < C_COUNT; i++)
            if (s_ctlOrder[i] == C_SHOTCOMBO) m->sel = i;
        return SET_NONE;
    }
    if (m->sel < HK_COUNT && (down & XINPUT_GAMEPAD_A))
    {
        s_hkCap = m->sel;
        s_hkAcc = 0;
        s_hkCapReleased = 0;
        s_hkStart = GetTickCount();
        s_hkNote[0] = 0;
        return SET_NONE;
    }
    if (m->sel == HKR_TEX && (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)))
    {
        c->texR3 = !c->texR3;   // R3 x2 in game: the HD textures on and off
        s_hkNote[0] = 0;
        return SET_CHANGED;
    }
    if (m->sel < HK_COUNT && (down & XINPUT_GAMEPAD_X))
    {
        c->hotkey[m->sel] = 0;
        s_hkNote[0] = 0;
        return SET_CHANGED;
    }
    if (down & XINPUT_GAMEPAD_Y)
    {
        ConfigHotkeyDefaults(c);
        c->texR3 = 1;
        strcpy(s_hkNote, Tr(T_HK_DEFAULTS));
        return SET_CHANGED;
    }
    return SET_NONE;
}

static void hkpage_draw(const UiMenu *m)
{
    const float w = 820, h = 680, x = 24, y = 20, foot = 78;
    const Config *c = s_hkCfg;
    float ry = y + 92;
    char buf[160];
    int i;
    if (!c) return;
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    UiText(UI_BOLD, 30, x + 40, y + 48, UI_COL_TEXT, m->title, UI_LEFT);
    for (i = 0; i < HKR_COUNT; i++)
    {
        int sel = m->sel == i;
        if (i == HKR_PAUSE)
        {
            // The fixed ones under a line.
            UiRect(x + 44, ry + 4, w - 88, 1, UI_COL_LINE);
            ry += 12;
        }
        if (i < HK_COUNT)
        {
            if (s_hkCap == i) _snprintf(buf, sizeof(buf) - 1, "%s", ((int)(UiTime() * 2.5f) & 1) ? "..." : "");
            else hk_text(c->hotkey[i], buf, sizeof(buf));
        }
        else
        {
            static const int fixed[] = { T_HK_V_PAUSE, T_HK_V_LIST, T_HK_V_UISHOT, T_HK_V_TEX };
            _snprintf(buf, sizeof(buf) - 1, "%s", i == HKR_TEX && !c->texR3 ? Tr(T_OFF) : Tr(fixed[i - HKR_PAUSE]));
        }
        buf[sizeof(buf) - 1] = 0;
        if (sel) selection_bar(x + 28, ry, w - 56, 44);
        UiText(UI_BOLD, 20, x + 44, ry + 22, sel ? UI_COL_TEXT : i < HK_COUNT || i == HKR_TEX ? UI_COL_TEXT2 : UI_COL_TEXT3, Tr(s_hkLabels[i]), UI_LEFT);
        UiText(UI_BOLD, 20, x + w - 44, ry + 22, i < HK_COUNT || i == HKR_TEX ? UI_COL_TEXT : UI_COL_TEXT3, buf, UI_RIGHT);
        ry += 48;
    }
    // What the selected one does (or what to do now, waiting for a combination).
    ry += 8;
    UiRoundRect(x + 28, ry, w - 56, 96, 10, D3DCOLOR_ARGB(120, 20, 22, 30), D3DCOLOR_ARGB(120, 14, 15, 21));
    paragraph(UI_REGULAR, 19, x + 48, ry + 12, w - 96, 3, s_hkCap >= 0 ? UI_COL_TEXT : UI_COL_TEXT2,
              s_hkCap >= 0 ? Tr(T_HK_PRESS) : s_hkNote[0] ? s_hkNote : Tr(s_hkDesc[m->sel >= 0 && m->sel < HKR_COUNT ? m->sel : 0]));
    UiRect(x + 24, y + h - foot, w - 48, 1, UI_COL_LINE);
    UiFooter(x + 40, y + h - foot * 0.5f, m->footer);
}

// From the ROM list, the pack is chosen for the selected game (its packs are the ones
// offered) but went only into the menu's settings: a game with its own settings
// (config\<game>.ini, "Save for this game") kept the pack saved there, so the choice
// showed only after turning the packs off and on in game. The choice goes into that
// game's file too, when it has one.
static void profile_set_hires(const Config *c)
{
    char folder[64], path[256], line[512], buf[96];
    const char *slash;
    std::string out;
    int sawOn = 0, sawFile = 0;
    FILE *f;
    if (!s_browser || s_browserSel.empty() || !s_langPath[0]) return;
    if (!RomFolderName(s_browserSel.c_str(), folder, sizeof(folder))) return;
    slash = strrchr(s_langPath, '\\');
    if (!slash) return;
    _snprintf(path, sizeof(path) - 1, "%.*s\\%s.ini", (int)(slash - s_langPath), s_langPath, folder);
    path[sizeof(path) - 1] = 0;
    f = fopen(path, "r");
    if (!f) return;   // no settings of its own: the menu's are the ones used
    while (fgets(line, sizeof(line), f))
    {
        if (!strncmp(line, "hirestex=", 9))
        {
            sprintf(buf, "hirestex=%d\n", c->hiTex);
            out += buf;
            sawOn = 1;
        }
        else if (!strncmp(line, "hirestexfile=", 13))
        {
            if (c->hiTexFile[0]) { out += "hirestexfile="; out += c->hiTexFile; out += "\n"; }
            sawFile = 1;
        }
        else out += line;
    }
    fclose(f);
    if (!sawOn) { sprintf(buf, "hirestex=%d\n", c->hiTex); out += buf; }
    if (!sawFile && c->hiTexFile[0]) { out += "hirestexfile="; out += c->hiTexFile; out += "\n"; }
    f = fopen(path, "w");
    if (!f) return;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
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
            // Recommended, then the four presets (round).
            int pr = s_gfxPick >= 0 ? s_gfxPick : current_preset(c) >= 0 ? current_preset(c) + 1 : gfx_recommended(c) ? 0 : -1;
            const GfxPreset *p;
            pr = pr < 0 ? (dir > 0 ? 0 : 4) : (pr + dir + 5) % 5;
            s_gfxPick = pr;
            if (pr == 0) { gfx_apply_recommended(c); break; }
            p = &s_presets[pr - 1];
            c->texFilter = p->texFilter;
            c->smooth = p->smooth;
            c->sharpen = p->sharpen;
            c->blur = p->blur; c->sharpLevel = c->sharpen * 50; c->blurLevel = c->blur * 50;
            c->screen = p->screen;
            break;
        }
        case G_FILTER: c->texFilter = (c->texFilter + dir + 3) % 3; s_gfxPick = -1; break;
        case G_HIRES:
        {
            // Cycles Off, then one state per pack found for the ROM in context (see
            // HiresScanPacks) - same "dir or A moves it on" pattern as the rest of this page.
            int hn = HiresPackCount(), states = hn + 1, cur = 0, k;
            if (hn <= 1)
            {
                // One pack (or none): a switch.
                c->hiTex = !(c->hiTex && hn);
                if (hn) { strncpy(c->hiTexFile, HiresPackName(0), sizeof(c->hiTexFile) - 1); c->hiTexFile[sizeof(c->hiTexFile) - 1] = 0; }
                if (!s_kodiSim) profile_set_hires(c);
                break;
            }
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
            if (!s_kodiSim) profile_set_hires(c);
            break;
        }
        case G_SMOOTH: c->smooth = !c->smooth; s_gfxPick = -1; break;
        case G_SHARPEN:   // the bar, 10 at a time; above 0 it turns the blur off
            c->sharpLevel += dir * 10;
            if (c->sharpLevel < 0) c->sharpLevel = 0;
            if (c->sharpLevel > 100) c->sharpLevel = 100;
            if (c->sharpLevel) c->blurLevel = 0;
            levels_to_steps(c);
            break;
        case G_BLUR:
            c->blurLevel += dir * 10;
            if (c->blurLevel < 0) c->blurLevel = 0;
            if (c->blurLevel > 100) c->blurLevel = 100;
            if (c->blurLevel) c->sharpLevel = 0;
            levels_to_steps(c);
            break;
        case G_DEFLICKER: c->deflicker = !c->deflicker; break;
        case G_SCREEN: c->screen = ((c->screen < 0 || c->screen > 8 ? 0 : c->screen) + dir + 9) % 9; s_gfxPick = -1; break;
        }
        settings_values(m, c);
        return SET_CHANGED;
    }
    return SET_NONE;
}

static int settings_input(UiMenu *m, Config *c, int perGame, WORD down);

int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down)
{
    int r = settings_input(m, c, perGame, down);
    if (r == SET_CHANGED && (down & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)))
        xb_audio_ui_sound_instead((down & XINPUT_GAMEPAD_DPAD_LEFT) ? XB_SFX_VALUE_DOWN : XB_SFX_VALUE_UP);
    return r;
}

// Pages whose rows can be run dry (their handlers keep no state of their own beside the menu and settings).
static int kodi_dd_page(void)
{
    return (s_systemPage || s_graphicsPage || s_screenPage || s_soundPage || s_interfacePage || s_imagesPage || s_saverPage) &&
           !s_catsPage && !s_catEditPage && !s_pathsPage;
}

static int kodi_dd_open(UiMenu *m, Config *c, int perGame)
{
    static UiMenu mc;
    static Config cc;
    int row = m->sel, k, n = 0;
    if (!kodi_dd_page() || row < 0 || row >= m->count || !kodi_arrows(m->value[row])) return 0;
    if (!strcmp(m->label[row], Tr(T_LANGUAGE))) return 0;   // its own list (and a live change)
    mc = *m;
    cc = *c;
    strncpy(s_kodiDdItem[n++], m->value[row], 63);
    s_kodiSim = 1;
    for (k = 1; k <= KODI_DD_MAX; k++)
    {
        mc.sel = row;
        settings_input(&mc, &cc, perGame, XINPUT_GAMEPAD_DPAD_RIGHT);
        if (mc.sel != row || strcmp(mc.value[row], s_kodiDdItem[0]) == 0) break;
        if (n == KODI_DD_MAX) { n = 0; break; }
        strncpy(s_kodiDdItem[n], mc.value[row], 63);
        s_kodiDdItem[n][63] = 0;
        n++;
    }
    s_kodiSim = 0;
    ui_from_config(c);   // whatever the dry run set for the interface, as it is again
    if (n < 2 || k > KODI_DD_MAX) return 0;   // a long range (sizes, positions): left/right as before
    s_kodiDdRot = 0;
    {
        // The values come from the row's own on: a list with "Off" (the resolutions)
        // is turned so that Off is the last, as the list itself has it.
        char off[64], tmp[KODI_DD_MAX][64];
        _snprintf(off, sizeof(off) - 1, "< %s >", Tr(T_RES_OFF));
        off[sizeof(off) - 1] = 0;
        for (k = 0; k < n; k++)
            if (!strcmp(s_kodiDdItem[k], off)) s_kodiDdRot = (k + 1) % n;
        memcpy(tmp, s_kodiDdItem, sizeof(tmp));
        for (k = 0; k < n; k++) memcpy(s_kodiDdItem[k], tmp[(k + s_kodiDdRot) % n], 64);
    }
    s_kodiDd = row;
    s_kodiDdN = n;
    s_kodiDdSel = 0;
    s_kodiDdTop = 0;
    return 1;
}

// The open list of values: up/down, A picks, B or left closes it. A SET_* code, or -1 when no list is open.
static int dd_input(UiMenu *m, Config *c, int perGame, WORD down)
{
    int i;
    if (s_kodiDd < 0) return -1;
    {
        // The open list: up/down, A picks, B or left closes it.
        if (s_kodiDd != m->sel) { s_kodiDd = -1; return -1; }
        if (down & XINPUT_GAMEPAD_DPAD_UP) s_kodiDdSel = (s_kodiDdSel + s_kodiDdN - 1) % s_kodiDdN;
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) s_kodiDdSel = (s_kodiDdSel + 1) % s_kodiDdN;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_LEFT)) s_kodiDd = -1;
        if (down & XINPUT_GAMEPAD_A)
        {
            int steps = (s_kodiDdSel + s_kodiDdRot) % s_kodiDdN, r = SET_NONE, row = m->sel;
            s_kodiDd = -1;
            s_ddApply = 1;   // the steps go to the row itself: right must not open its list again
            for (i = 0; i < steps; i++)
            {
                int got;
                m->sel = row;   // the row stays selected
                got = settings_input(m, c, perGame, XINPUT_GAMEPAD_DPAD_RIGHT);
                if (got == SET_CHANGED) r = SET_CHANGED;
            }
            s_ddApply = 0;
            UiMenuPress();
            return r;
        }
        return SET_NONE;
    }
}

// The list style: A on a row with "< >" values opens their list (left and right still step them).
static int list_input(UiMenu *m, Config *c, int perGame, WORD down)
{
    int r, i;
    if (s_kodiSim || s_ddApply) return -1;
    r = dd_input(m, c, perGame, down);
    if (r >= 0) return r;
    if ((down & XINPUT_GAMEPAD_A) && m->page == UI_PAGE_LIST && m->sel >= 0 && m->sel < m->count && kodi_arrows(m->value[m->sel]) &&
        kodi_dd_open(m, c, perGame))
    {
        for (i = 0; i < s_kodiDdN; i++)
            if (!strcmp(s_kodiDdItem[i], m->value[m->sel])) s_kodiDdSel = i;
        return SET_NONE;
    }
    return -1;
}

// The page's own input in the big icons style, before the page's: a SET_* code, or -1 when not taken.
static int kodi_input(UiMenu *m, Config *c, int perGame, WORD down)
{
    KodiParts kp;
    int order[UI_MENU_MAX], n, pos = 0, i, part, r;
    if (s_kodiSim || s_ddApply) return -1;
    kodi_parts(m, &kp);
    if (m->sel < 0 || m->sel >= m->count) m->sel = 0;
    r = dd_input(m, c, perGame, down);
    if (r >= 0) return r;
    n = kodi_order(m, &kp, order);
    for (i = 0; i < n; i++)
        if (order[i] == m->sel) pos = i;
    part = kp.count >= 2 ? kp.rowPart[m->sel] : -1;
    if (s_kodiSide && kp.count >= 2)
    {
        if (down & XINPUT_GAMEPAD_DPAD_UP) s_kodiPart = (s_kodiPart + kp.count - 1) % kp.count;
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) s_kodiPart = (s_kodiPart + 1) % kp.count;
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT))
        {
            s_kodiSide = 0;
            for (i = 0; i < n; i++)
                if (kp.rowPart[order[i]] == s_kodiPart) { m->sel = order[i]; break; }
        }
        if (down & XINPUT_GAMEPAD_B) { s_kodiSide = 0; return -1; }   // B: the page's own (back)
        return SET_NONE;
    }
    s_kodiSide = 0;
    if (down & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN))
    {
        // Up and down in the order shown (parts may hold rows from anywhere in the page).
        pos = (pos + ((down & XINPUT_GAMEPAD_DPAD_UP) ? n - 1 : 1)) % n;
        m->sel = order[pos];
        return SET_NONE;
    }
    if (kodi_arrows(m->value[m->sel]) && (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT)) && kodi_dd_open(m, c, perGame))
    {
        // Its list, the value it has now selected.
        for (i = 0; i < s_kodiDdN; i++)
            if (!strcmp(s_kodiDdItem[i], m->value[m->sel])) s_kodiDdSel = i;
        return SET_NONE;
    }
    if (kp.count >= 2 && (down & XINPUT_GAMEPAD_B))
    {
        // B: back to the parts first, then out.
        s_kodiSide = 1;
        s_kodiPart = part;
        return SET_NONE;
    }
    if (kp.count >= 2 && (down & XINPUT_GAMEPAD_DPAD_LEFT) && m->value[m->sel][0] != '\x03')
    {
        // Left: to the parts (the values are in the lists now, only the bars take left).
        if (!kodi_arrows(m->value[m->sel]) || kodi_dd_page())
        {
            s_kodiSide = 1;
            s_kodiPart = part;
            return SET_NONE;
        }
    }
    return -1;
}

static int settings_input(UiMenu *m, Config *c, int perGame, WORD down)
{
    s_perGame = perGame;
    if (kodi_sub(m))
    {
        int r = kodi_input(m, c, perGame, down);   // big icons style: the side bar's parts, the values' lists
        if (r >= 0) return r;
    }
    else
    {
        int r;
        s_kodiSide = 0;
        r = !strcmp(m->footer, Tr(T_FOOT_SELECT)) ? list_input(m, c, perGame, down) : -1;   // the lists of values, list style
        if (r >= 0) return r;
        if (!strcmp(m->footer, Tr(T_FOOT_SELECT)) == 0) s_kodiDd = -1;
    }
    if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) UiMenuPress();   // the row shows the press
    if (s_catEditPage) return catedit_input(m, c, down);
    if (s_catsPage) return cats_input(m, c, down);
    if (s_graphicsPage) return graphics_input(m, c, down);
    if (m->page == UI_PAGE_EDGES) return edges_input(m, c, down);
    if (s_calPage) return cal_input(m, c, down);
    if (s_devPage) return dev_input(m, c, down);
    if (s_screenPage) return screen_input(m, c, down);
    if (s_saverPage) return saver_input(m, c, down);
    if (s_imagesPage) return images_input(m, c, down);
    if (s_pathsPage) return paths_input(m, c, down);
    if (s_interfacePage) return interface_input(m, c, down);
    if (m->page == UI_PAGE_CONTROLS) return controls_input(m, c, down);
    if (m->page == UI_PAGE_CALIB) return calib_input(m, c, down);
    if (m->page == UI_PAGE_VOLUME) return volume_input(m, c, down);
    if (m->page == UI_PAGE_FX) return fx_input(m, c, down);
    if (m->page == UI_PAGE_WALL) return wall_input(m, c, down);
    if (m->page == UI_PAGE_BORDER) return border_input(m, c, down);
    if (m->page == UI_PAGE_PERF) return perf_input(m, c, down);
    if (m->page == UI_PAGE_HOTKEYS) return hk_input(m, c, down);
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
    if (s_browser && m->sel == S_INTERFACE && (down & XINPUT_GAMEPAD_A))
    {
        settings_interface(m, c);
        return SET_NONE;
    }
    if (s_browser && m->sel == S_PATHS && (down & XINPUT_GAMEPAD_A))
    {
        settings_paths(m, c);
        return SET_NONE;
    }
    if (m->sel == s_rowAbout && (down & XINPUT_GAMEPAD_A)) return SET_ABOUT;
    settings_values(m, c);
    if (down & XINPUT_GAMEPAD_A)
    {
        if (m->sel == s_rowSaveAll) return SET_SAVE_ALL;
        if (m->sel == s_rowSaveGame) return SET_SAVE_GAME;
    }
    return SET_NONE;
}

// ---- Leaving ----
// A yes/no question's panel: `title` (-1: none) above the question, everything
// centred, the panel just big enough for its text (a little room around, no
// wide margins), its edge glowing.
static void confirm_draw(int title, int question, int footer)
{
    const float pad = 44.0f, titleH = title >= 0 ? 46.0f : 0.0f, qSize = title >= 0 ? 22.0f : 30.0f;
    float tw = title >= 0 ? UiTextWidth(UI_BOLD, 30, Tr(title)) : 0, qw = UiTextWidth(title >= 0 ? UI_REGULAR : UI_BOLD, qSize, Tr(question));
    float fw = UiFooterWidth(Tr(footer)), w = tw, h = 36 + titleH + 44 + 26 + 66, x, y, cx = UI_WIDTH * 0.5f;
    if (qw > w) w = qw;
    if (fw > w) w = fw;
    w += pad * 2;
    x = (UI_WIDTH - w) / 2;
    y = (UI_HEIGHT - h) / 2;
    edge_glow(x, y, w, h, 22);
    UiPanel(x, y, w, h, 22);
    if (title >= 0) UiText(UI_BOLD, 30, cx, y + 36 + 18, UI_COL_TEXT, Tr(title), UI_CENTER);
    UiText(title >= 0 ? UI_REGULAR : UI_BOLD, qSize, cx, y + 36 + titleH + 18, title >= 0 ? UI_COL_TEXT2 : UI_COL_TEXT, Tr(question), UI_CENTER);
    UiRect(x + 24, y + h - 66, w - 48, 1, UI_COL_LINE);
    UiFooter(cx - fw * 0.5f, y + h - 33, Tr(footer));
}

void UiConfirmDraw(int title, int question, int footer)
{
    UiDim();   // over whatever is behind it (the in-game menu)
    confirm_draw(title, question, footer);
}

// The same question on its own screen (the ROM list, Settings): 1 for A, 0 for B or BACK.
static int confirm_ask(IDirect3DDevice9 *dev, int title, int question, int footer)
{
    UiInput in;
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        confirm_draw(title, question, footer);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

static int confirm_quit(IDirect3DDevice9 *dev) { return confirm_ask(dev, T_QUIT_TITLE, T_QUIT_ASK, T_FOOT_QUIT); }

// The updater's screen while xb_update's thread downloads, checks and installs it:
// "Downloading LAIKA 64 x.y" and a bar filling as default.xex arrives (MB and %), then
// "Checking and installing"; moving dots while the size is not known yet.
// 1 installed (start it), 0 failed (the reason logged).
static int update_run(IDirect3DDevice9 *dev, const char *version)
{
    static UiAnim fill;
    char line[128], why[96], amount[64];
    int st;
    fill.live = 0;
    UpdateInstallStart(version);
    for (;;)
    {
        const float w = 640, h = 220, x = (UI_WIDTH - w) * 0.5f, y = (UI_HEIGHT - h) * 0.5f;
        const float bx = x + 60, bw = w - 120, by = y + 110, bh = 14;
        unsigned got, total;
        int phase, k;
        st = UpdateInstallState(why, sizeof(why));
        if (st == 2 || st < 0) break;
        phase = UpdateInstallProgress(&got, &total);
        if (phase == UPDATE_PHASE_INSTALL) _snprintf(line, sizeof(line) - 1, "%s", Tr(T_UPDATE_INSTALLING));
        else _snprintf(line, sizeof(line) - 1, Tr(T_UPDATE_WORKING), version);
        line[sizeof(line) - 1] = 0;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        edge_glow(x, y, w, h, 22);
        UiPanel(x, y, w, h, 22);
        UiText(UI_BOLD, 26, x + w * 0.5f, y + 62, UI_COL_TEXT, line, UI_CENTER);
        if (phase == UPDATE_PHASE_PREPARE || (phase == UPDATE_PHASE_DOWNLOAD && !total))
        {
            int dots = (int)(UiTime() * 3.0f) % 4;
            for (k = 0; k < 3; k++)
                UiCircle(x + w * 0.5f - 24 + k * 24, by + bh * 0.5f, 6, k < dots ? UI_COL_ACCENT_HI : UI_COL_LINE);
        }
        else
        {
            // The bar: its track, then the part done (gliding to it), full while it installs.
            float done = phase == UPDATE_PHASE_INSTALL ? 1.0f : (float)got / (float)total, shown;
            if (done > 1.0f) done = 1.0f;
            shown = anim_step_rate(&fill, done, !fill.live, 8.0f, 0.001f);
            UiRoundRect(bx, by, bw, bh, bh * 0.5f, D3DCOLOR_ARGB(50, 255, 255, 255), D3DCOLOR_ARGB(35, 255, 255, 255));
            if (shown > 0.0f)
            {
                selection_glow(bx, by, bw * shown, bh, bh * 0.5f, 0.5f);
                UiRoundRect(bx, by, bw * shown < bh ? bh : bw * shown, bh, bh * 0.5f, SEL_HI(255), SEL_LO(255));
            }
            if (phase == UPDATE_PHASE_DOWNLOAD)
            {
                _snprintf(amount, sizeof(amount) - 1, "%.1f / %.1f MB  -  %d%%", got / 1048576.0f, total / 1048576.0f, (int)(done * 100.0f));
                amount[sizeof(amount) - 1] = 0;
                UiText(UI_REGULAR, 17, x + w * 0.5f, by + bh + 22, UI_COL_TEXT2, amount, UI_CENTER);
            }
        }
        UiText(UI_REGULAR, 17, x + w * 0.5f, y + h - 34, UI_COL_TEXT3, Tr(T_UPDATE_WAIT), UI_CENTER);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    if (st == 2) return 1;
    H64_INFO("[update] %s", why);
    return 0;
}

// What is new in this version (NEWS_1..NEWS_COUNT, the menus' language), once
// after an update: the first start of a version newer than lastver= (the
// first start of all goes through the welcome and its set-up instead). A, B or
// START go on. Each release changes the NEWS_* texts (and NEWS_COUNT).
#define NEWS_COUNT 4
static void news_screen(IDirect3DDevice9 *dev)
{
    UiInput in;
    char title[96];
    const float openAt = UiTime();
    UiInputInit(&in);
    _snprintf(title, sizeof(title) - 1, Tr(T_NEWS_TITLE), H64_VERSION_STRING);
    title[sizeof(title) - 1] = 0;
    xb_audio_ui_sound(XB_SFX_NOTIFY);
    for (;;)
    {
        const float w = 820, h = 170 + NEWS_COUNT * 44, x = (UI_WIDTH - w) * 0.5f;
        const float e = ease_out((UiTime() - openAt) / 0.25f), y = (UI_HEIGHT - h) * 0.5f + (1.0f - e) * 30.0f;
        WORD down = UiInputPoll(&in);
        int i;
        if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) return;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        browser_effect();
        UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UiAlpha(UI_COL_DIM, e));
        edge_glow(x, y, w, h, 22);
        UiPanel(x, y, w, h, 22);
        UiBrand(x + 40, y + 48, 26);
        UiText(UI_BOLD, 28, x + w - 40, y + 48, UI_COL_TEXT, title, UI_RIGHT);
        UiRect(x + 24, y + 86, w - 48, 1, UI_COL_LINE);
        for (i = 0; i < NEWS_COUNT; i++)
        {
            const float ly = y + 120 + i * 44;
            UiCircle(x + 52, ly, 5, UI_COL_ACCENT_HI);
            UiText(UI_REGULAR, 20, x + 72, ly, UI_COL_TEXT, Tr(T_NEWS_1 + i), UI_LEFT);
        }
        UiRect(x + 24, y + h - 56, w - 48, 1, UI_COL_LINE);
        UiFooter(x + 40, y + h - 28, Tr(T_FOOT_NEWS));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- About ----
// Settings > About (and X in the ROM browser and the in-game menu): three tabs,
// the emulator (version, engine, licence), the credits (who made it, what is new
// in LAIKA 64) and the acknowledgements. In the menus' language; names and
// projects as they are.
static int s_aboutTab;   // 0 About, 1 Credits, 2 Acknowledgements
static IDirect3DTexture9 *s_profileTex;   // ErickaguirreD's picture (xb_profile.h, from luna.jpg)
static int s_profileTried;

int AboutInput(WORD down)
{
    if (down & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_LEFT))
    {
        s_aboutTab = (s_aboutTab + 2) % 3;
        xb_audio_ui_sound(XB_SFX_TAB);
    }
    if (down & (XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_DPAD_RIGHT))
    {
        s_aboutTab = (s_aboutTab + 1) % 3;
        xb_audio_ui_sound(XB_SFX_TAB);
    }
    if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) { s_aboutTab = 0; return 1; }
    return 0;
}

// One credits entry: a name in bold, what it is beside it.
static float credit_line(float x, float y, const char *name, const char *what)
{
    float a = UiText(UI_BOLD, 20, x, y, UI_COL_TEXT, name, UI_LEFT);
    UiText(UI_REGULAR, 18, x + a + 14, y, UI_COL_TEXT2, what, UI_LEFT);
    return 34;
}

void AboutDraw(void)
{
    const float x = 150, y = 46, w = 980, h = 628, cx = x + 52, col2 = x + 560;
    float ty, tx;
    char buf[160];
    int i;
    UiDim();
    edge_glow(x, y, w, h, 24);
    UiPanel(x, y, w, h, 24);
    UiBrand(cx, y + 72, BRAND_SIZE);
    _snprintf(buf, sizeof(buf) - 1, Tr(T_ABOUT_VERSION), H64_VERSION_STRING);
    buf[sizeof(buf) - 1] = 0;
    UiText(UI_REGULAR, 18, x + w - 52, y + 62, UI_COL_TEXT2, buf, UI_RIGHT);
    UiText(UI_REGULAR, 16, x + w - 52, y + 88, UI_COL_TEXT3, __DATE__, UI_RIGHT);
    // The tabs.
    tx = cx;
    for (i = 0; i < 3; i++)
    {
        const char *t = Tr(i == 2 ? T_CR_THANKS_TAB : i ? T_CREDITS : T_ABOUT);
        float tw = UiTextWidth(UI_BOLD, 20, t) + 36;
        if (i == s_aboutTab) UiRoundRect(tx, y + 122, tw, 36, 18, SEL_HI(255), SEL_HI(220));
        else UiRoundRing(tx, y + 122, tw, 36, 18, 1.0f, UI_COL_LINE);
        UiText(UI_BOLD, 20, tx + tw * 0.5f, y + 140, i == s_aboutTab ? UI_COL_TEXT : UI_COL_TEXT2, t, UI_CENTER);
        tx += tw + 12;
    }
    UiRect(x + 32, y + 176, w - 64, 1, UI_COL_LINE);
    ty = y + 210;
    if (!s_aboutTab)
    {
        UiText(UI_REGULAR, 24, cx, ty, UI_COL_TEXT, Tr(T_ABOUT_TAGLINE), UI_LEFT);
        ty += 60;
        heading(cx, ty, Tr(T_ABOUT_ENGINE));
        paragraph(UI_REGULAR, 19, cx, ty + 16, w - 104, 6, UI_COL_TEXT2, Tr(T_ABOUT_ENGINE_TEXT));
        ty += 150;
        heading(cx, ty, Tr(T_ABOUT_LICENCE));
        paragraph(UI_REGULAR, 18, cx, ty + 14, w - 104, 4, UI_COL_TEXT3, Tr(T_ABOUT_LICENCE_TEXT));
    }
    else if (s_aboutTab == 1)
    {
        // The credits: ErickaguirreD with the picture (a circle with a ring and a soft
        // glow), the original author under, then what LAIKA 64 brings in two columns.
        const float ps = 112, px = cx, py = ty + 22, nx = px + ps + 28;
        if (!s_profileTried && UiDevice())
        {
            int iw = 0, ih = 0;
            u8 *pic = h64_png_decode(s_profilePng, sizeof(s_profilePng), &iw, &ih, UI_PROFILE_SIZE * UI_PROFILE_SIZE);
            s_profileTried = 1;
            if (pic) { s_profileTex = UiImageCreate(UiDevice(), pic, iw, ih); free(pic); }
        }
        heading(cx, ty, Tr(T_CR_DEVELOPMENT));
        UiCircle(px + ps * 0.5f, py + ps * 0.5f, ps * 0.5f + 10, SEL_HI(40));
        UiCircle(px + ps * 0.5f, py + ps * 0.5f, ps * 0.5f + 1, D3DCOLOR_ARGB(255, 6, 7, 10));
        if (s_profileTex) UiImage(s_profileTex, px, py, ps, ps, ps * 0.5f, 0xFFFFFFFF);
        UiRoundRing(px - 4, py - 4, ps + 8, ps + 8, ps * 0.5f + 4, 2.0f, SEL_HI(255));
        UiText(UI_BOLD, 28, nx, py + 26, UI_COL_TEXT, "ErickaguirreD", UI_LEFT);
        UiText(UI_REGULAR, 19, nx, py + 58, UI_COL_TEXT2, Tr(T_CR_LAIKA), UI_LEFT);
        credit_line(nx, py + 92, "Mohamed Aymen (kernel64)", Tr(T_CR_BASED));
        ty = py + ps + 34;
        heading(cx, ty, Tr(T_CR_NEW));
        for (i = 0; i < 10; i++)
        {
            const float bx = i < 5 ? cx : x + 510, by = ty + 30 + (i % 5) * 30;
            UiCircle(bx + 4, by, 3.5f, UI_COL_ACCENT_HI);
            UiText(UI_REGULAR, 18, bx + 18, by, UI_COL_TEXT2, Tr(T_CR_NEW_1 + i), UI_LEFT);
        }
    }
    else
    {
        // The acknowledgements.
        heading(cx, ty, Tr(T_CR_THANKS));
        ty += 16 + paragraph(UI_REGULAR, 18, cx, ty + 16, w - 104, 3, UI_COL_TEXT2, Tr(T_CR_THANKS_TEXT)) + 22;
        heading(cx, ty, Tr(T_CR_DATA));
        paragraph(UI_REGULAR, 18, cx, ty + 16, 440, 3, UI_COL_TEXT2, Tr(T_CR_DATA_TEXT));
        heading(col2, ty, Tr(T_CR_FONTS));
        paragraph(UI_REGULAR, 18, col2, ty + 16, w - (col2 - x) - 52, 3, UI_COL_TEXT2, Tr(T_CR_FONTS_TEXT));
        UiText(UI_REGULAR, 18, cx, y + h - 102, UI_COL_TEXT3, Tr(T_CR_COMMUNITY), UI_LEFT);
    }
    UiRect(x + 32, y + h - 72, w - 64, 1, UI_COL_LINE);
    UiFooter(cx, y + h - 36, Tr(T_FOOT_ABOUT));
}

void AboutScreen(IDirect3DDevice9 *dev)
{
    UiInput in;
    UiInputInit(&in);
    s_aboutTab = 0;
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (AboutInput(down)) return;
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
// A list in a panel: each language by its own name and its code; the title and
// the button prompt follow the row under the cursor (no flags: a language is
// not a country).
#define LANG_ROW_H 52.0f
#define LANG_PANEL_W 560.0f

static int s_langAutoMs;
void LanguageScreenAuto(int ms) { s_langAutoMs = ms; }

int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart)
{
    UiInput in;
    UiAnim ay;
    int sel = initial >= 0 && initial < LANG_COUNT ? initial : LANG_EN, i, result = -2;
    DWORD openedAt = GetTickCount();
    const float ph = 40 + LANG_COUNT * LANG_ROW_H, px = (UI_WIDTH - LANG_PANEL_W) * 0.5f, py = 176;
    memset(&ay, 0, sizeof(ay));
    UiInputInit(&in);
    H64_INFO("[lang] language list (%s)", firstStart ? "first start" : "settings");
    while (result == -2)
    {
        WORD down = UiInputPoll(&in);
        float sy;
        if (down & XINPUT_GAMEPAD_DPAD_UP) sel = (sel + LANG_COUNT - 1) % LANG_COUNT;
        if (down & XINPUT_GAMEPAD_DPAD_DOWN) sel = (sel + 1) % LANG_COUNT;
        if ((down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) || (s_langAutoMs > 0 && GetTickCount() - openedAt >= (DWORD)s_langAutoMs)) result = sel;
        else if (!firstStart && (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK))) result = -1;

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        UiText(UI_BOLD, 34, UI_WIDTH * 0.5f, 120, UI_COL_TEXT, TrIn(sel, T_LANG_TITLE), UI_CENTER);
        edge_glow(px, py, LANG_PANEL_W, ph, 22);
        UiPanel(px, py, LANG_PANEL_W, ph, 22);
        sy = anim_step(&ay, py + 20 + sel * LANG_ROW_H, 0);
        selection_bar(px + 16, sy + 3, LANG_PANEL_W - 32, LANG_ROW_H - 6);
        for (i = 0; i < LANG_COUNT; i++)
        {
            const float ry = py + 20 + i * LANG_ROW_H + LANG_ROW_H * 0.5f;
            char code[8];
            int s = i == sel, k;
            {
                // A small badge: a letter of its own (Ñ, Ä, Ç, ع, Ã, Я, È).
                static const char *const mark[] = { "Aa", "\xC3\x91", "\xC3\x84", "\xC3\x87", "\xEF\xBB\x89", "\xC3\x83", "\xD0\xAF", "\xC3\x88" };
                const float bx = px + 58;
                UiCircle(bx, ry, 17, s ? SEL_HI(255) : D3DCOLOR_ARGB(255, 52, 56, 72));
                UiText(UI_BOLD, i == 0 ? 15 : 18, bx, ry, s ? D3DCOLOR_ARGB(255, 15, 16, 22) : UI_COL_TEXT2, i < (int)(sizeof(mark) / sizeof(mark[0])) ? mark[i] : "?", UI_CENTER);
            }
            UiText(s ? UI_BOLD : UI_REGULAR, 23, px + 92, ry, s ? UI_COL_TEXT : UI_COL_TEXT2, TrIn(i, T_LANG_NAME), UI_LEFT);
            for (k = 0; LangCode(i)[k] && k < 7; k++) code[k] = (char)toupper((u8)LangCode(i)[k]);
            code[k] = 0;
            UiText(UI_BOLD, 15, px + LANG_PANEL_W - 44, ry, s ? UI_COL_TEXT2 : UI_COL_TEXT3, code, UI_RIGHT);
        }
        UiScreenFooter(TrIn(sel, firstStart ? T_FOOT_LANG_FIRST : T_FOOT_LANG));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    H64_INFO("[lang] language list: %s", result >= 0 ? LangCode(result) : "left");
    return result;
}

// ---- Welcome (the first start, main.cpp) ----
// The LAIKA 64 logo, very large, fading in and out over black (about five
// seconds; A skips it), then a short set-up in the manner of a console's
// first start: one choice a page - the language, the controller (its buttons
// and stick calibration too), the picture (with a preview), a colour, a
// wallpaper, a background effect - and a last page that sums them up (Y
// skips to it). The nebula plays behind it until the effect page. Each page
// slides in; every choice shows at once (the menus behind are drawn with it).
// System > Setup assistant opens it again (without the logo). What is chosen
// is written to the menu's settings file by the caller.
#define WIZ_X 140.0f
#define WIZ_W (UI_WIDTH - 2 * WIZ_X)
#define WIZ_SLIDE 420.0f   // how far a page slides in or out
#define WIZ_SLIDE_S 0.38f
enum { WZ_LANG = 0, WZ_PAD, WZ_VIDEO, WZ_COLOR, WZ_WALL, WZ_FX, WZ_DONE, WZ_COUNT };

static float wiz_ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : 1 - (1 - t) * (1 - t) * (1 - t); }

// The splash: the logo, very large, fading in, held, fading out (about 5 s; A, B or START skips it).
static int s_splashShown;   // the logo was shown this session (the first start's welcome, or the intro)

static void wiz_splash(IDirect3DDevice9 *dev)
{
    const float total = 5.0f, start = UiTime();
    s_splashShown = 1;
    UiInput in;
    UiInputInit(&in);
    xb_audio_ui_sound(XB_SFX_LAUNCH);
    for (;;)
    {
        float t = UiTime() - start, a, k;
        WORD down = UiInputPoll(&in);
        if (t >= total || (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))) break;
        a = t < 0.8f ? wiz_ease(t / 0.8f) : t > total - 1.2f ? 1.0f - wiz_ease((t - (total - 1.2f)) / 1.2f) : 1.0f;
        // The logo comes out of the stars' vanishing point: from a speck to full size
        // over 2.6 s (eased out), then a slow push in.
        k = t < 2.6f ? 0.02f + 0.92f * wiz_ease(t / 2.6f) : 0.94f + 0.06f * wiz_ease((t - 2.6f) / 2.4f);
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        {
            // The hyperspace stars of the background effects, rushing out of the centre.
            const int keep = s_bgEffect;
            s_bgEffect = BGFX_WARP;
            browser_effect();
            s_bgEffect = keep;
        }
        {
            const float lw = 980 * k, lh = lw * 1.9f / UiBrandWidth(1.0f), lx = (UI_WIDTH - lw) * 0.5f, ly = (UI_HEIGHT - lh) * 0.5f;
            UiShadow(lx + lw * 0.1f, ly + lh * 0.1f, lw * 0.8f, lh * 0.8f, lh * 0.4f, 160, SEL_LO(90 * a));   // a soft light behind
            UiBrandFit(lx, ly, lw, lh);
            UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(255 * (1.0f - a)), 0, 0, 0));   // the fade, over black
        }
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// Leaving LAIKA 64 (main.cpp, before the dashboard): the intro the other way
// round - the hyperspace stars, the logo shrinking into their vanishing point,
// then black. 1.6 s; any button ends it sooner.
void OutroScreen(IDirect3DDevice9 *dev)
{
    const float total = 1.6f, start = UiTime();
    const int keep = s_bgEffect;
    UiInput in;
    if (!dev) return;
    UiInputInit(&in);
    xb_audio_ui_sound(XB_SFX_BACK);
    for (;;)
    {
        float t = UiTime() - start, p, k, fade;
        WORD down = UiInputPoll(&in);
        if (t >= total || (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))) break;
        p = t / total;
        k = 0.02f + 0.92f * (1.0f - p * p * p);          // full size to a speck, faster at the end
        fade = p < 0.72f ? 1.0f : 1.0f - (p - 0.72f) / 0.28f;   // then into black
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        s_bgEffect = BGFX_WARP;
        browser_effect();
        s_bgEffect = keep;
        {
            const float lw = 980 * k, lh = lw * 1.9f / UiBrandWidth(1.0f), lx = (UI_WIDTH - lw) * 0.5f, ly = (UI_HEIGHT - lh) * 0.5f;
            UiShadow(lx + lw * 0.1f, ly + lh * 0.1f, lw * 0.8f, lh * 0.8f, lh * 0.4f, 160, SEL_LO(90 * fade));
            UiBrandFit(lx, ly, lw, lh);
        }
        UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(255 * (1.0f - fade)), 0, 0, 0));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    dev->Present(NULL, NULL, NULL, NULL);
}

// The page's title and the line under it; the language page's in the language under the cursor.
static void wiz_heading(float ox, float a, const char *title, const char *sub)
{
    UiText(UI_BOLD, 40, WIZ_X + ox, 150, UiAlpha(UI_COL_TEXT, a), title, UI_LEFT);
    paragraph(UI_REGULAR, 21, WIZ_X + ox, 180, WIZ_W * 0.75f, 2, UiAlpha(UI_COL_TEXT2, a), sub);
}

static IDirect3DTexture9 *s_wizFlags[LANG_COUNT];   // the language page's flags (WelcomeScreen)

struct WizState
{
    Config *c;
    int page, lang, row;   // row: on the pages of several settings (Controller, Picture)
    int preset;            // the Picture page's preset picked (0 Recommended, 1.. s_presets), -1: as the settings say
    int dd;                // the row whose list of values is open (-1: none)
};

// The rows' height: closer when a page has many (the Picture page's seven).
static float wiz_row_h(int n) { return n > 7 ? 42.0f : n > 6 ? 48.0f : 56.0f; }

// A page's settings, one a row: the label at the left, "< value >" at the right.
static void wiz_rows(float ox, float a, const char *const *labels, const char *const *values, int n, int sel, float width)
{
    int i;
    const float rh = wiz_row_h(n);
    for (i = 0; i < n; i++)
    {
        float ry = 262.0f + i * rh, rx = WIZ_X + ox;
        char buf[128];
        int s = i == sel;
        if (s) selection_bar(rx - 14, ry, width + 20, rh - 10);
        UiText(s ? UI_BOLD : UI_REGULAR, 21, rx + 10, ry + (rh - 10) * 0.5f, UiAlpha(s ? UI_COL_TEXT : UI_COL_TEXT2, a), labels[i], UI_LEFT);
        if (values[i] && (switch_value(values[i]) >= 0 || values[i][0] == '\x03'))
        {
            draw_value(rx + width - 10, ry + (rh - 10) * 0.5f, values[i], s);   // a switch sliding as it turns, or a bar
            UiRect(rx, ry + rh - 4, width, 1, UiAlpha(UI_COL_LINE, a));
            continue;
        }
        if (values[i] && values[i][0] == '\x04')
        {
            // A colour of the background effect: its swatch ("Original" for none).
            const int k = atoi(values[i] + 1);
            const float cy = ry + (rh - 10) * 0.5f;
            if (k > 0 && k <= FX_PALETTE_COUNT)
            {
                UiCircle(rx + width - 24, cy, 13, UiAlpha(D3DCOLOR_ARGB(255, s_fxPalette[k][0], s_fxPalette[k][1], s_fxPalette[k][2]), a));
                if (s) UiRoundRing(rx + width - 41, cy - 17, 34, 34, 17, 2.0f, UiAlpha(UI_COL_TEXT, a));
            }
            else UiText(UI_BOLD, 21, rx + width - 10, cy, UiAlpha(s ? UI_COL_TEXT : UI_COL_TEXT2, a), Tr(T_FX_ORIGINAL), UI_RIGHT);
            UiRect(rx, ry + rh - 4, width, 1, UiAlpha(UI_COL_LINE, a));
            continue;
        }
        if (values[i])
        {
            // A value: its list opens with A (no left/right arrows); a small mark says so.
            float tw = UiText(UI_BOLD, 21, rx + width - 30, ry + (rh - 10) * 0.5f, UiAlpha(s ? UI_COL_TEXT : UI_COL_TEXT2, a), values[i], UI_RIGHT);
            (void)tw;
            icon_tri(rx + width - 12, ry + (rh - 10) * 0.5f + 1, 9, 2, UiAlpha(s ? SEL_HI(255) : UI_COL_TEXT3, a));
        }
        else
        {
            _snprintf(buf, sizeof(buf) - 1, "%s", UI_CH_RCHEVRON);   // opens a page (A)
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_BOLD, 21, rx + width - 10, ry + (rh - 10) * 0.5f, UiAlpha(s ? UI_COL_TEXT : UI_COL_TEXT2, a), buf, UI_RIGHT);
        }
        UiRect(rx, ry + rh - 4, width, 1, UiAlpha(UI_COL_LINE, a));
    }
}

static const char *wiz_stick_name(const Config *c)
{
    const int st = c->padMap[0].stick;
    return Tr(st == H64_STICK_RIGHT ? T_STICK_RIGHT : st == H64_STICK_DPAD ? T_STICK_DPAD : st == H64_STICK_NONE ? T_OFF : T_STICK_LEFT);
}

// Recommended: 1366x768 (16:9), brightness 50 %, colours as they are, no software stretch.
static int wiz_recommended(const Config *c)
{
    return c->resWide && c->res169 == 4 && c->brightLevel == 50 && c->colorStyle == 0 && !c->stretch;
}

static void wiz_apply_preset(WizState *w, int pick)
{
    Config *c = w->c;
    if (pick == 0)
    {
        c->resWide = 1; c->res169 = 4;
        c->texFilter = 1;   // bilinear
        c->brightLevel = 50;
        c->colorStyle = 0;
        c->stretch = 0;
    }
    else
    {
        // The Graphics page's presets: texture filter, smoothing, sharpening, blur, screen effect.
        const GfxPreset *p = &s_presets[pick - 1];
        c->texFilter = p->texFilter;
        c->smooth = p->smooth;
        c->sharpen = p->sharpen;
        c->blur = p->blur; c->sharpLevel = c->sharpen * 50; c->blurLevel = c->blur * 50;
        c->screen = p->screen;
    }
    w->preset = pick;
}

static int wiz_preset_now(const WizState *w)
{
    int pr;
    if (w->preset >= 0) return w->preset;
    if (wiz_recommended(w->c)) return 0;
    pr = current_preset(w->c);
    return pr < 0 ? -1 : pr + 1;
}

static const char *wiz_preset_name(const WizState *w)
{
    int pick = wiz_preset_now(w);
    return pick < 0 ? Tr(T_CUSTOM) : pick == 0 ? Tr(T_PRESET_RECOMMENDED) : Tr(s_presets[pick - 1].name);
}

// A resolution row's value (no arrows: the rows add them).
static const char *wiz_res_value(const Config *c, int wide)
{
    static char s[2][32];
    u32 w, h;
    if ((c->resWide != 0) != wide) return Tr(T_RES_OFF);
    ConfigResSize(wide, wide ? c->res169 : c->res43, &w, &h);
    sprintf(s[wide], "%ux%u", w, h);
    return s[wide];
}

// The value of a row of the Controller and Picture pages (NULL: a row that opens a page).
static const char *wiz_value(const WizState *w, int row)
{
    static const int scrLabels[9] = { T_SCREEN_NONE, T_SCANLINES, T_CRT, T_CRT_CURVED, T_LCD_GRID, T_LIGHT_SCANLINES, T_SCR_VIGNETTE, T_SCR_APERTURE, T_SCR_DOTS };
    static char lv[16];
    static char col[3][8];
    const Config *c = w->c;
    if (w->page == WZ_PAD)
    {
        // The buttons and the stick open their pages; the Rumble Pak last, a switch.
        return row == 2 ? toggle(c->rumble > 0) : NULL;
    }
    if (w->page == WZ_FX)
    {
        if (row == 0) return Tr(s_fxLabels[c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : 0]);
        if (row >= 1 && row <= 3)
        {
            sprintf(col[row - 1], "\x04%d", c->fxColor[row - 1] >= 0 && c->fxColor[row - 1] <= FX_PALETTE_COUNT ? c->fxColor[row - 1] : 0);
            return col[row - 1];
        }
        return NULL;
    }
    if (w->page != WZ_VIDEO) return NULL;
    switch (row)
    {
    case 0: return wiz_preset_name(w);
    case 1: return wiz_res_value(c, 0);
    case 2: return wiz_res_value(c, 1);
    case 3: return Tr(scrLabels[c->screen < 0 || c->screen > 8 ? 0 : c->screen]);
    case 4: sprintf(lv, "\x03%d", c->brightLevel < 0 || c->brightLevel > 100 ? 50 : c->brightLevel); return lv;   // bars, as in the settings
    case 5: sprintf(col[0], "\x03%d", c->sharpLevel < 0 || c->sharpLevel > 100 ? 0 : c->sharpLevel); return col[0];
    case 6: return Tr(s_colorStyleLabels[c->colorStyle < 0 || c->colorStyle > 7 ? 0 : c->colorStyle]);
    case 7: return toggle(c->stretch);
    }
    return NULL;
}

static int wiz_rows_of(int page) { return page == WZ_PAD ? 3 : page == WZ_VIDEO ? 8 : page == WZ_FX ? 4 : 0; }

// One step of a row's value (left/right, and the lists' dry runs).
static void wiz_step(WizState *w, int row, int dx)
{
    Config *c = w->c;
    if (w->page == WZ_PAD)
    {
        if (row == 2) c->rumble = c->rumble > 0 ? 0 : 100;   // the switch: off, or full strength
        return;
    }
    if (w->page == WZ_FX)
    {
        if (row == 0) c->bgEffect = ((c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : 0) + dx + BGFX_COUNT) % BGFX_COUNT;
        if (row >= 1 && row <= 3)
        {
            int *v = &c->fxColor[row - 1];
            *v = ((*v >= 0 && *v <= FX_PALETTE_COUNT ? *v : 0) + dx + FX_PALETTE_COUNT + 1) % (FX_PALETTE_COUNT + 1);
        }
        return;
    }
    if (w->page != WZ_VIDEO) return;
    if (row == 0)
    {
        int pick = wiz_preset_now(w);
        wiz_apply_preset(w, ((pick < 0 ? (dx > 0 ? -1 : 0) : pick) + dx + 5) % 5);
        return;
    }
    w->preset = -1;   // another setting: the preset is as the settings say again
    if (row == 1) res_step(c, 0, dx);
    if (row == 2) res_step(c, 1, dx);
    if (row == 3) c->screen = ((c->screen < 0 || c->screen > 8 ? 0 : c->screen) + dx + 9) % 9;
    if (row == 4)
    {
        // A bar, as in the settings: 5 at a time (50: as the console).
        c->brightLevel = (c->brightLevel < 0 || c->brightLevel > 100 ? 50 : c->brightLevel) + dx * 5;
        if (c->brightLevel < 0) c->brightLevel = 0;
        if (c->brightLevel > 100) c->brightLevel = 100;
    }
    if (row == 5)
    {
        // Sharpness, a bar: 10 at a time; above 0 it turns the blur off.
        c->sharpLevel = (c->sharpLevel < 0 || c->sharpLevel > 100 ? 0 : c->sharpLevel) + dx * 10;
        if (c->sharpLevel < 0) c->sharpLevel = 0;
        if (c->sharpLevel > 100) c->sharpLevel = 100;
        if (c->sharpLevel) c->blurLevel = 0;
        levels_to_steps(c);
    }
    if (row == 6) c->colorStyle = ((c->colorStyle < 0 || c->colorStyle > 7 ? 0 : c->colorStyle) + dx + 8) % 8;
    if (row == 7) c->stretch = !c->stretch;
}

// A row's list of values (as the settings' lists): its values from a dry run on a copy.
static int wiz_dd_open(WizState *w)
{
    WizState ww = *w;
    Config cc = *w->c;
    const char *v;
    char off[64], tmp[KODI_DD_MAX][64];
    int n = 0, k;
    ww.c = &cc;
    v = wiz_value(&ww, w->row);
    if (!v || switch_value(v) >= 0 || v[0] == '\x03' || v[0] == '\x04') return 0;   // a switch, a bar or a colour: left/right, no list
    _snprintf(s_kodiDdItem[n++], 63, "< %s >", v);
    for (k = 1; k <= KODI_DD_MAX; k++)
    {
        wiz_step(&ww, w->row, 1);
        v = wiz_value(&ww, w->row);
        _snprintf(tmp[0], 63, "< %s >", v);
        tmp[0][63] = 0;
        if (!strcmp(tmp[0], s_kodiDdItem[0]) || n == KODI_DD_MAX) break;
        strcpy(s_kodiDdItem[n++], tmp[0]);
    }
    if (n < 2) return 0;
    s_kodiDdRot = 0;
    _snprintf(off, sizeof(off) - 1, "< %s >", Tr(T_RES_OFF));
    off[sizeof(off) - 1] = 0;
    for (k = 0; k < n; k++)
        if (!strcmp(s_kodiDdItem[k], off)) s_kodiDdRot = (k + 1) % n;   // "Off" the last, as in the settings
    memcpy(tmp, s_kodiDdItem, sizeof(tmp));
    for (k = 0; k < n; k++) memcpy(s_kodiDdItem[k], tmp[(k + s_kodiDdRot) % n], 64);
    s_kodiDdN = n;
    s_kodiDdSel = (n - s_kodiDdRot) % n;   // the value it has now
    s_kodiDdTop = 0;
    w->dd = w->row;
    return 1;
}

// The Picture page's preview: the reference picture (xb_wizref.h) as the chosen
// settings would show it - its pixels as big as the resolution leaves them,
// with the colour style and the brightness applied - made into one texture
// when they change (it was thousands of rectangles a frame: the page slowed
// down), then the screen effect over it.
static u8 *s_wizRef;                 // the reference picture, RGBA
static IDirect3DTexture9 *s_wizPrev; // the preview as made for s_wizPrevKey
static int s_wizPrevKey[5] = { -1, -1, -1, -1, -1 };

static void wiz_preview_make(int nx, int ny, int cs, int lv)
{
    const int k = (512 + nx - 1) / nx, tw = nx * k, th = ny * k;   // upscaled by whole pixels (no blur between them)
    const float gam = lv >= 50 ? 1.0f - 0.4f * (lv - 50) / 50.0f : 1.0f + 0.6f * (50 - lv) / 50.0f;
    u8 *small, *big;
    int i, j, x, y;
    if (!s_wizRef)
    {
        int w = 0, h = 0;
        s_wizRef = h64_png_decode(s_wizRefPng, sizeof(s_wizRefPng), &w, &h, WIZREF_W * WIZREF_H);
        if (!s_wizRef) return;
    }
    small = (u8 *)malloc(nx * ny * 4);
    big = (u8 *)malloc(tw * th * 4);
    if (!small || !big) { free(small); free(big); return; }
    for (j = 0; j < ny; j++)
        for (i = 0; i < nx; i++)
        {
            // The area of the picture this pixel covers, averaged; then the style and brightness.
            int x0 = i * WIZREF_W / nx, x1 = (i + 1) * WIZREF_W / nx, y0 = j * WIZREF_H / ny, y1 = (j + 1) * WIZREF_H / ny, n = 0, q;
            float rgb[3] = { 0, 0, 0 }, o[3], l;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (y = y0; y < y1; y++)
                for (x = x0; x < x1; x++, n++)
                    for (q = 0; q < 3; q++) rgb[q] += s_wizRef[(y * WIZREF_W + x) * 4 + q];
            for (q = 0; q < 3; q++) rgb[q] /= 255.0f * n;
            l = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
            for (q = 0; q < 3; q++) o[q] = rgb[q];
            if (cs == 1 || cs == 2) for (q = 0; q < 3; q++) o[q] = l + (rgb[q] - l) * (cs == 1 ? 1.35f : 1.7f);
            if (cs == 3 || cs == 7) o[0] = o[1] = o[2] = l;
            if (cs == 4) { o[0] = l * 1.35f; o[1] = l * 1.2f; o[2] = l * 0.93f; }
            if (cs == 5) { o[0] *= 1.08f; o[2] *= 0.86f; }
            if (cs == 6) { o[0] *= 0.88f; o[1] *= 0.98f; o[2] *= 1.12f; }
            if (cs == 7) { o[0] *= 0.62f; o[1] *= 0.78f; o[2] *= 0.36f; }
            for (q = 0; q < 3; q++) small[(j * nx + i) * 4 + q] = (u8)(powf(o[q] < 0 ? 0 : o[q] > 1 ? 1 : o[q], gam) * 255.0f);
            small[(j * nx + i) * 4 + 3] = 255;
        }
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++)
            memcpy(big + (y * tw + x) * 4, small + ((y / k) * nx + x / k) * 4, 4);
    if (s_wizPrev) UiImageRelease(UiDevice(), s_wizPrev);
    s_wizPrev = UiImageCreate(UiDevice(), big, tw, th);
    free(small);
    free(big);
}

static void wiz_video_preview(const Config *c, float x, float y, float w, float h, float a)
{
    u32 rw, rh;
    int wide;
    ConfigResCurrent(c, &rw, &rh, &wide);
    const int scr = c->screen < 0 || c->screen > 8 ? 0 : c->screen;
    const int cs = c->colorStyle < 0 || c->colorStyle > 7 ? 0 : c->colorStyle;
    const float pw = wide || c->stretch ? w : h * 4.0f / 3.0f, px = x + (w - pw) * 0.5f;
    // The pixels: as many rows as the height gives (36 at 240 lines, 108 at 720), across as the shape gives.
    const int ny = 36 + (int)(rh - 240) * 72 / 480, nx = wide ? ny * 16 / 9 : ny * 4 / 3;
    const int lv = c->brightLevel < 0 || c->brightLevel > 100 ? 50 : c->brightLevel;
    const float bw = pw / nx, bh = h / ny;
    int i, j;
    if (s_wizPrevKey[0] != nx || s_wizPrevKey[1] != ny || s_wizPrevKey[2] != cs || s_wizPrevKey[3] != lv || !s_wizPrev)
    {
        s_wizPrevKey[0] = nx; s_wizPrevKey[1] = ny; s_wizPrevKey[2] = cs; s_wizPrevKey[3] = lv;
        wiz_preview_make(nx, ny, cs, lv);
    }
    UiRoundRect(x, y, w, h, 8, UiAlpha(D3DCOLOR_ARGB(255, 0, 0, 0), a), UiAlpha(D3DCOLOR_ARGB(255, 0, 0, 0), a));
    if (s_wizPrev) UiImage(s_wizPrev, px, y, pw, h, 0.0f, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), a));
    // The screen effect over it.
    if (scr == 1 || scr == 2 || scr == 3 || scr == 5)
        for (j = 0; j < ny; j++) UiRect(px, y + j * bh + bh * 0.55f, pw, bh * 0.45f, UiAlpha(D3DCOLOR_ARGB(scr == 5 ? 60 : 120, 0, 0, 0), a));
    if (scr == 4)
    {
        for (i = 0; i <= nx; i++) UiRect(px + i * bw - 0.5f, y, 1.0f, h, UiAlpha(D3DCOLOR_ARGB(110, 0, 0, 0), a));
        for (j = 0; j <= ny; j++) UiRect(px, y + j * bh - 0.5f, pw, 1.0f, UiAlpha(D3DCOLOR_ARGB(110, 0, 0, 0), a));
    }
    if (scr == 2 || scr == 3)
    {
        // A tube: darker towards its edges (and round corners on the curved one).
        UiRoundRing(px, y, pw, h, scr == 3 ? 26.0f : 6.0f, scr == 3 ? 14.0f : 6.0f, UiAlpha(D3DCOLOR_ARGB(200, 0, 0, 0), a));
        UiShadow(px + pw * 0.2f, y + h * 0.2f, pw * 0.6f, h * 0.6f, h * 0.3f, h * 0.3f, UiAlpha(D3DCOLOR_ARGB(28, 255, 255, 255), a));
    }
    UiRoundRing(x, y, w, h, 8, 1.0f, UiAlpha(UI_COL_LINE, a));
}

// The controller's buttons and the stick's calibration, the Controls page's own (B comes back here).
static void wiz_controls(IDirect3DDevice9 *dev, Config *c, int calib)
{
    UiMenu m;
    UiInput in;
    if (calib)
    {
        calib_open(&m, c);
        calib_start(CT_WIZARD);   // the automatic calibration at once: rest, then circles
    }
    else
    {
        // The automatic mapping at once (each N64 button asked in turn), without its question.
        controls_open(&m, c);
        s_ctl.autoSaved = c->padMap[s_ctl.player];
        s_ctl.autoMode = AUTO_RUN;
        s_ctl.autoIdx = 0;
        s_ctl.capturing = 1;
        s_ctl.capItem = s_autoOrder[0];
        s_ctl.capAdd = 0;
        s_ctl.capReleased = 0;
        s_ctl.capStart = GetTickCount();
    }
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        SettingsInput(&m, c, 0, down);
        if (m.page != UI_PAGE_CONTROLS && m.page != UI_PAGE_CALIB) break;   // B went back out of them
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        browser_effect();
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

static void wiz_page(const WizState *w, int page, float ox, float a)
{
    Config *c = w->c;
    char buf[160];
    int i;
    switch (page)
    {
    case WZ_LANG:
        wiz_heading(ox, a, TrIn(w->lang, T_WIZ_LANG_TITLE), TrIn(w->lang, T_WIZ_LANG_SUB));
        for (i = 0; i < LANG_COUNT; i++)
        {
            float ry = 262.0f + i * 44, rx = WIZ_X + ox;
            int s = i == w->lang;
            if (s) selection_bar(rx - 14, ry, 520, 40);
            UiText(s ? UI_BOLD : UI_REGULAR, 21, rx + 14, ry + 20, UiAlpha(s ? UI_COL_TEXT : UI_COL_TEXT2, a), TrIn(i, T_LANG_NAME), UI_LEFT);
            if (s_wizFlags[i])
            {
                // Its flag at the right of the row (Spanish: Mexico's).
                UiImage(s_wizFlags[i], rx + 452, ry + 7, 39, 26, 4, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), a));
                UiRoundRing(rx + 452, ry + 7, 39, 26, 4, 1.0f, UiAlpha(D3DCOLOR_ARGB(60, 255, 255, 255), a));
            }
        }
        break;
    case WZ_PAD:
    {
        const char *labels[3], *values[3];
        WizState shown = *w;
        shown.page = WZ_PAD;
        wiz_heading(ox, a, Tr(T_WIZ_PAD_TITLE), Tr(T_WIZ_PAD_SUB));
        labels[0] = Tr(T_WIZ_MAP_BUTTONS); values[0] = NULL;   // the automatic mapping, straight away
        labels[1] = Tr(T_WIZ_CAL_STICK); values[1] = NULL;     // the automatic calibration, straight away
        labels[2] = Tr(T_RUMBLE); values[2] = wiz_value(&shown, 2);   // a switch
        wiz_rows(ox, a, labels, values, 3, w->row, 760);
        if (w->dd >= 0 && page == w->page) dd_draw(WIZ_X + ox + 750, 262.0f + (w->dd + 1) * wiz_row_h(3) - 8, wiz_row_h(3));
        break;
    }
    case WZ_VIDEO:
    {
        static const int names[8] = { T_PRESET, T_RESOLUTION, T_RES_169, T_SCREEN_EFFECT, T_BRIGHTNESS, T_SHARPEN, T_COLOR_STYLE, T_STRETCH };
        const char *labels[8], *values[8];
        WizState shown = *w;
        shown.page = WZ_VIDEO;
        wiz_heading(ox, a, Tr(T_WIZ_VIDEO_TITLE), Tr(T_WIZ_VIDEO_SUB));
        for (i = 0; i < 8; i++) { labels[i] = Tr(names[i]); values[i] = wiz_value(&shown, i); }
        wiz_rows(ox, a, labels, values, 8, w->row, 540);
        wiz_video_preview(c, WIZ_X + 580 + ox, 268, WIZ_W - 580, (WIZ_W - 580) * 9.0f / 16.0f, a);   // how it looks
        if (w->dd >= 0 && page == w->page) dd_draw(WIZ_X + ox + 530, 262.0f + (w->dd + 1) * wiz_row_h(8) - 8, wiz_row_h(8));
        break;
    }
    case WZ_COLOR:
    {
        const float step = 96, r = 32, cy = 340, x0 = UI_WIDTH * 0.5f - (SEL_COLORS - 1) * step * 0.5f + ox;
        wiz_heading(ox, a, Tr(T_WIZ_COLOR_TITLE), Tr(T_WIZ_COLOR_SUB));
        for (i = 0; i < SEL_COLORS; i++)
        {
            float cx = x0 + i * step;
            const unsigned char *hi = s_selColors[i][0], *lo = s_selColors[i][1];
            UiShadow(cx - r, cy - r + 6, r * 2, r * 2, r, 14, UiAlpha(D3DCOLOR_ARGB(140, 0, 0, 0), a));
            UiRoundRect(cx - r, cy - r, r * 2, r * 2, r, UiAlpha(D3DCOLOR_ARGB(255, hi[0], hi[1], hi[2]), a), UiAlpha(D3DCOLOR_ARGB(255, lo[0], lo[1], lo[2]), a));
            if (i == c->selColor)
            {
                UiRoundRing(cx - r - 7, cy - r - 7, (r + 7) * 2, (r + 7) * 2, r + 7, 3.0f, UiAlpha(UI_COL_TEXT, a));
                UiText(UI_BOLD, 20, cx, cy + r + 34, UiAlpha(UI_COL_TEXT, a), Tr(s_selLabels[i]), UI_CENTER);
            }
        }
        // How it looks: a menu row, selected.
        {
            const float pw = 560, px = (UI_WIDTH - pw) * 0.5f + ox, py = 450;
            UiPanel(px, py, pw, 120, 18);
            selection_bar(px + 20, py + 18, pw - 40, 40);
            UiText(UI_BOLD, 20, px + 44, py + 38, UiAlpha(UI_COL_TEXT, a), Tr(T_SETTINGS), UI_LEFT);
            UiText(UI_REGULAR, 20, px + 44, py + 86, UiAlpha(UI_COL_TEXT2, a), Tr(T_INTERFACE), UI_LEFT);
        }
        break;
    }
    case WZ_WALL:
    {
        const float px = WIZ_X + 480 + ox, pw = WIZ_W - 480, ph = pw * UI_HEIGHT / UI_WIDTH, py = 262;
        const int n = UiWallpaperCount();
        wiz_heading(ox, a, Tr(T_WIZ_WALL_TITLE), Tr(T_WIZ_WALL_SUB));
        for (i = 0; i < 2; i++)
        {
            float ry = 262.0f + i * 50, rx = WIZ_X + ox;
            int s = (c->wallCustom != 0) == (i == 1);
            if (s) selection_bar(rx - 14, ry, 420, 44);
            UiText(s ? UI_BOLD : UI_REGULAR, 21, rx + 10, ry + 22, UiAlpha(i == 1 && !n ? UI_COL_TEXT3 : s ? UI_COL_TEXT : UI_COL_TEXT2, a),
                   Tr(i ? T_WALL_OPT_MINE : T_WALL_OPT_NONE), UI_LEFT);
        }
        UiRoundRect(px, py, pw, ph, 10, UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), a), UiAlpha(D3DCOLOR_ARGB(255, 10, 11, 15), a));
        if (!(c->wallCustom && a > 0.98f && UiWallpaperPreview(px, py, pw, ph)))
        {
            const char *say = Tr(n ? T_WALL_OFF_HINT : T_WALL_HINT);
            float th = paragraph(UI_REGULAR, 18, px + 28, -1000, pw - 56, 3, 0, say);
            paragraph(UI_REGULAR, 18, px + 28, py + (ph - th) * 0.5f, pw - 56, 3, UiAlpha(UI_COL_TEXT2, a), say);
        }
        UiRoundRing(px, py, pw, ph, 10, 1.0f, UiAlpha(UI_COL_LINE, a));
        break;
    }
    case WZ_FX:
    {
        // The effect (its list) and its three colours (left/right through the swatches;
        // "Original" keeps the effect's own); it plays behind as they change.
        static char colLabel[3][32];
        const char *labels[4], *values[4];
        WizState shown = *w;
        shown.page = WZ_FX;
        wiz_heading(ox, a, Tr(T_WIZ_FX_TITLE), Tr(T_WIZ_FX_SUB));
        labels[0] = Tr(T_FX_EFFECT);
        for (i = 0; i < 3; i++)
        {
            _snprintf(colLabel[i], sizeof(colLabel[i]) - 1, Tr(T_FX_COLOR), i + 1);
            colLabel[i][sizeof(colLabel[i]) - 1] = 0;
            labels[i + 1] = colLabel[i];
        }
        for (i = 0; i < 4; i++) values[i] = wiz_value(&shown, i);
        wiz_rows(ox, a, labels, values, 4, w->row, 640);
        if (w->dd >= 0 && page == w->page) dd_draw(WIZ_X + ox + 630, 262.0f + (w->dd + 1) * wiz_row_h(4) - 8, wiz_row_h(4));
        (void)buf;
        break;
    }
    default:
    {
        // Done: a tick, and what was chosen.
        const float cx = WIZ_X + 46 + ox, cy = 300;
        static const int labels[6] = { T_LANGUAGE, T_ANALOG_STICK, T_PRESET, T_SEL_COLOR, T_WALLPAPERS, T_BG_EFFECT };
        const char *values[6];
        wiz_heading(ox, a, Tr(T_WIZ_DONE_TITLE), Tr(T_WIZ_DONE_SUB));
        UiCircle(cx, cy, 40, UiAlpha(SEL_HI(255), a));
        UiLine(cx - 18, cy + 1, cx - 5, cy + 14, 6.0f, UiAlpha(D3DCOLOR_ARGB(255, 15, 16, 22), a));
        UiLine(cx - 5, cy + 14, cx + 20, cy - 13, 6.0f, UiAlpha(D3DCOLOR_ARGB(255, 15, 16, 22), a));
        values[0] = Tr(T_LANG_NAME);
        values[1] = wiz_stick_name(c);
        values[2] = wiz_preset_name(w);
        values[3] = Tr(s_selLabels[c->selColor >= 0 && c->selColor < SEL_COLORS ? c->selColor : 0]);
        values[4] = Tr(c->wallCustom ? T_WALL_OPT_MINE : T_WALL_OPT_NONE);
        values[5] = Tr(s_fxLabels[c->bgEffect >= 0 && c->bgEffect < BGFX_COUNT ? c->bgEffect : 0]);
        for (i = 0; i < 6; i++)
        {
            float ry = 248.0f + i * 46, rx = WIZ_X + 140 + ox;
            UiText(UI_REGULAR, 19, rx, ry + 16, UiAlpha(UI_COL_TEXT2, a), Tr(labels[i]), UI_LEFT);
            UiText(UI_BOLD, 20, rx + 330, ry + 16, UiAlpha(UI_COL_TEXT, a), values[i], UI_LEFT);
            if (i < 5) UiRect(rx, ry + 38, 600, 1, UiAlpha(UI_COL_LINE, a));
        }
        break;
    }
    }
}

// The steps' dots and "Step n of N" (left), the buttons (right): Back outlined, Next (or Start) filled.
static void wiz_chrome(const WizState *w)
{
    char buf[64];
    int i;
    _snprintf(buf, sizeof(buf) - 1, Tr(T_WIZ_STEP), w->page + 1, WZ_COUNT);
    buf[sizeof(buf) - 1] = 0;
    UiText(UI_BOLD, 16, WIZ_X, 96, UI_COL_TEXT3, buf, UI_LEFT);
    for (i = 0; i < WZ_COUNT; i++)
        UiRoundRect(WIZ_X + i * 22, 628, i == w->page ? 16.0f : 8.0f, 8, 4, i == w->page ? SEL_HI(255) : D3DCOLOR_ARGB(80, 255, 255, 255),
                    i == w->page ? SEL_LO(255) : D3DCOLOR_ARGB(80, 255, 255, 255));
    {
        // Each a rounded pill with its controller buttons in their circles (RB, and A
        // where A is not a list's), the way the prompts at the bottom show them.
        char next[96], back[96];
        float nw, bw, by = 604, bh = 50, nx;
        const int onNext = 0;
        _snprintf(next, sizeof(next) - 1, "%s:%s", w->page == WZ_LANG ? "A/RB" : "RB", Tr(w->page == WZ_DONE ? T_WIZ_START : T_WIZ_NEXT));
        next[sizeof(next) - 1] = 0;
        _snprintf(back, sizeof(back) - 1, "B/LB:%s", Tr(T_WIZ_BACK));
        back[sizeof(back) - 1] = 0;
        nw = UiFooterWidth(next) + 40;
        bw = UiFooterWidth(back) + 40;
        nx = UI_WIDTH - WIZ_X - nw;
        if (onNext && wiz_rows_of(w->page)) selection_glow(nx - 4, by - 4, nw + 8, bh + 8, bh * 0.5f + 4, selection_pulse());
        UiRoundRect(nx, by, nw, bh, bh * 0.5f, onNext ? SEL_LO(200) : D3DCOLOR_ARGB(235, 40, 44, 58), onNext ? SEL_LO(160) : D3DCOLOR_ARGB(235, 30, 33, 44));
        UiRoundRing(nx, by, nw, bh, bh * 0.5f, onNext ? 3.0f : 2.0f, SEL_HI(255));
        UiFooter(nx + 20, by + bh * 0.5f, next);
        if (w->page > 0)
        {
            float bx = nx - 14 - bw;
            UiRoundRect(bx, by, bw, bh, bh * 0.5f, D3DCOLOR_ARGB(200, 30, 33, 44), D3DCOLOR_ARGB(200, 24, 26, 35));
            UiRoundRing(bx, by, bw, bh, bh * 0.5f, 1.5f, D3DCOLOR_ARGB(140, 255, 255, 255));
            UiFooter(bx + 20, by + bh * 0.5f, back);
        }
    }
}

void WelcomeScreen(IDirect3DDevice9 *dev, Config *c, int splash)
{
    WizState w;
    UiInput in;
    int prev = -1, dir = 1, i, wasBrowser = s_browser;
    float changedAt = -10.0f;
    memset(&w, 0, sizeof(w));
    w.c = c;
    w.preset = -1;
    w.dd = -1;
    w.lang = LangFromCode(c->lang) >= 0 ? LangFromCode(c->lang) : LangFromConsole();
    LangSet(w.lang);
    s_browser = 1;   // the menus' look as in the ROM list (sounds, wallpapers)
    ui_from_config(c);
    H64_INFO("[menu] welcome");
    for (i = 0; i < LANG_COUNT && i < (int)(sizeof(s_flagPng) / sizeof(s_flagPng[0])); i++)
    {
        int fw = 0, fh = 0;
        u8 *px = h64_png_decode(s_flagPng[i], s_flagPngSize[i], &fw, &fh, FLAG_W * FLAG_H);
        s_wizFlags[i] = px ? UiImageCreate(dev, px, fw, fh) : NULL;
        free(px);
    }
    // Its own music: a space piece for Laika (assistant\laika_orbit.mp3), the menus' after.
    xb_audio_music_start("game:\\assistant", XB_MUSIC_TRACK, "laika_orbit.mp3");
    if (splash) wiz_splash(dev);
    changedAt = UiTime();   // the first page slides in too
    UiInputInit(&in);
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        int dx = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
        int dy = (down & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (down & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
        float now = UiTime(), t;
        if (w.dd >= 0)
        {
            // A row's list is open: up/down in it, A picks, B or left closes it.
            if (down & XINPUT_GAMEPAD_DPAD_UP) s_kodiDdSel = (s_kodiDdSel + s_kodiDdN - 1) % s_kodiDdN;
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) s_kodiDdSel = (s_kodiDdSel + 1) % s_kodiDdN;
            if (down & XINPUT_GAMEPAD_A)
            {
                int steps = (s_kodiDdSel + s_kodiDdRot) % s_kodiDdN, k;
                for (k = 0; k < steps; k++) wiz_step(&w, w.row, 1);
                UiMenuPress();
            }
            if (down & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_LEFT)) w.dd = -1;
            down = 0;   // nothing else this time
            dx = dy = 0;
        }
        else if ((down & XINPUT_GAMEPAD_A) && w.row < wiz_rows_of(w.page) && wiz_value(&w, w.row) && switch_value(wiz_value(&w, w.row)) >= 0)
        {
            wiz_step(&w, w.row, 1);   // A on a switch: it turns
            UiMenuPress();
            down = 0;
            dx = dy = 0;
        }
        else if ((down & XINPUT_GAMEPAD_A) && w.row < wiz_rows_of(w.page) && wiz_value(&w, w.row) && wiz_dd_open(&w))
        {
            down = 0;   // A on a row with values: its list
            dx = dy = 0;
        }
        // ---- The page's choice.
        switch (w.page)
        {
        case WZ_LANG:
            if (dy || dx) { w.lang = (w.lang + (dy ? dy : dx) + LANG_COUNT) % LANG_COUNT; LangSet(w.lang); }
            break;
        case WZ_PAD:
        case WZ_VIDEO:
        case WZ_FX:
            // The rows (RB goes on; A is the rows' own: lists, switches, pages).
            if (dy) w.row = (w.row + dy + wiz_rows_of(w.page)) % wiz_rows_of(w.page);
            if (dx && w.row < wiz_rows_of(w.page) && wiz_value(&w, w.row)) wiz_step(&w, w.row, dx);
            break;
        case WZ_COLOR:
            if (dx || dy) c->selColor = (c->selColor + (dx ? dx : dy) + SEL_COLORS) % SEL_COLORS;
            break;
        case WZ_WALL:
            if ((dx || dy) && UiWallpaperCount()) c->wallCustom = !c->wallCustom;
            break;
        }
        ui_from_config(c);
        // ---- On, back.
        if ((down & XINPUT_GAMEPAD_A) && w.page == WZ_PAD && w.row <= 1)
        {
            // Map the buttons, or calibrate the stick: the automatic set-up of the Controls pages, then back here.
            wiz_controls(dev, c, w.row == 1);
            UiInputInit(&in);
        }
        // RB / LB: next and back from any row (A on the pad page's last rows opens their pages,
        // and the D-pad changes the values: the Next and Back buttons could not be reached).
        else if (down & ((w.page == WZ_LANG ? XINPUT_GAMEPAD_A : 0) | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_RIGHT_SHOULDER))   // A only picks the language
        {
            if (w.page == WZ_LANG) lang_change(c, w.lang);   // kept at once, as Settings does
            if (w.page == WZ_DONE) break;
            prev = w.page++;
            dir = 1;
            changedAt = now;
            w.row = 0;
        }
        else if ((down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_LEFT_SHOULDER)) && w.page > 0)
        {
            prev = w.page--;
            dir = -1;
            changedAt = now;
            w.row = 0;
        }
        else if ((down & XINPUT_GAMEPAD_Y) && w.page != WZ_DONE)
        {
            // Skip: straight to the last page (what is not chosen stays as it is).
            if (w.page == WZ_LANG) lang_change(c, w.lang);
            prev = w.page;
            w.page = WZ_DONE;
            dir = 1;
            changedAt = now;
            w.row = 0;
        }
        // ---- Drawing: the menus' backdrop as chosen so far, a veil, the page sliding in.
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        if (w.page < WZ_FX && !(prev >= WZ_FX && now - changedAt < WIZ_SLIDE_S)) s_bgEffect = BGFX_NEBULA;   // the effect chosen shows from its page on
        browser_effect();
        UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB(110, 6, 7, 10));
        UiBrand((UI_WIDTH - UiBrandWidth(BRAND_SIZE * 0.85f)) * 0.5f, 52, BRAND_SIZE * 0.85f);
        t = wiz_ease((now - changedAt) / WIZ_SLIDE_S);
        if (t < 1.0f && prev >= 0) wiz_page(&w, prev, -dir * WIZ_SLIDE * t, 1.0f - t);
        wiz_page(&w, w.page, dir * WIZ_SLIDE * (1.0f - t), t < 1.0f ? t : 1.0f);
        wiz_chrome(&w);
        UiScreenFooter(Tr(w.page == WZ_DONE ? T_FOOT_WIZ_DONE : w.page ? T_FOOT_WIZ : T_FOOT_WIZ_FIRST));
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    for (i = 0; i < LANG_COUNT; i++)
        if (s_wizFlags[i]) { UiImageRelease(dev, s_wizFlags[i]); s_wizFlags[i] = NULL; }
    xb_audio_music_start(MUSIC_DIR, c->musicMode, c->musicTrack);   // the menus' music again
    s_browser = wasBrowser;
    H64_INFO("[menu] welcome done: %s, colour %d, wallpapers %d, effect %d", LangCode(w.lang), c->selColor, c->wallCustom, c->bgEffect);
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

// A file name as the list shows it: without the dump tags - "(U)", "(USA)",
// "(Rev 1)", "[!]", "[b1]"... - and the spaces they leave. A name that would
// be left empty stays as it is.
static std::string clean_name(const char *name, size_t len)
{
    std::string out;
    size_t i;
    int depth = 0;
    for (i = 0; i < len; i++)
    {
        char ch = name[i];
        if (ch == '(' || ch == '[') { depth++; continue; }
        if ((ch == ')' || ch == ']') && depth) { depth--; continue; }
        if (depth) continue;
        if ((ch == ' ' || ch == '_') && (out.empty() || out[out.size() - 1] == ' ')) continue;   // no double or leading space
        out += ch == '_' ? ' ' : ch;
    }
    while (!out.empty() && (out[out.size() - 1] == ' ' || out[out.size() - 1] == '-')) out.erase(out.size() - 1);
    return out.empty() ? std::string(name, len) : out;
}

// Whether UsbN answers (a memory is plugged in there): a listing, or one that finds nothing.
static int usb_present(int n)
{
    WIN32_FIND_DATAA fd;
    char pattern[16];
    HANDLE h;
    DWORD err;
    sprintf(pattern, "Usb%d:\\*", n);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return 1; }
    err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES;
}

// The ROMs on a memory: its top and its folders, 3 deep (e.g. Usb1:\roms\n64\...).
static void scan_rom_tree(const std::string &dir, int depth, std::vector<RomEntry> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const char *dot = strrchr(fd.cFileName, '.');
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth < 3 && out->size() < 4000) scan_rom_tree(dir + "\\" + fd.cFileName, depth + 1, out);
            continue;
        }
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64") && _stricmp(dot, ".zip"))) continue;
        {
            RomEntry e;
            e.file = dir + "\\" + fd.cFileName;
            e.shown = clean_name(fd.cFileName, dot - fd.cFileName);
            out->push_back(e);
        }
    } while (FindNextFileA(f, &fd) && out->size() < 4000);
    FindClose(f);
}

static void scan_roms(std::vector<RomEntry> *out)
{
    WIN32_FIND_DATAA fd;
    HANDLE f;
    char pattern[160];
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", s_romDir);   // Settings > Paths > ROMs
    pattern[sizeof(pattern) - 1] = 0;
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do
    {
        const char *dot = strrchr(fd.cFileName, '.');
        RomEntry e;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!dot || (_stricmp(dot, ".z64") && _stricmp(dot, ".n64") && _stricmp(dot, ".v64") && _stricmp(dot, ".zip"))) continue;
        e.file = std::string(s_romDir) + "\\" + fd.cFileName;
        e.shown = clean_name(fd.cFileName, dot - fd.cFileName);
        out->push_back(e);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    std::sort(out->begin(), out->end(), rom_less);
}

// ---- Game library: favourites and categories (the ROM list's tabs) ----
// Favourites: <drive>:\config\favorites.txt, one ROM file name per line.
// Categories: <drive>:\config\categories.txt, "field=value" per line - a
// category is every game whose information (the box art download's <key>.txt:
// ArtReadInfo) has that value in that field. The library's information is
// read once, when first wanted, and again after a box art download.
enum { CF_GENRE = 0, CF_REGION, CF_PLAYERS, CF_DEVELOPER, CF_PUBLISHER, CF_YEAR, CF_COUNT };
static const char *const s_catFieldKey[CF_COUNT] = { "genre", "region", "players", "developer", "publisher", "year" };
static const int s_catFieldLabel[CF_COUNT] = { T_CAT_GENRE, T_CAT_REGION, T_CAT_PLAYERS, T_CAT_DEVELOPER, T_CAT_PUBLISHER, T_CAT_YEAR };
#define MAX_CATS 16
struct GameCat { int field; char value[64]; };
struct LibGame { std::string file, field[CF_COUNT]; };
static GameCat s_cats[MAX_CATS];
static int s_catCount, s_catsLoaded, s_favsLoaded, s_libLoaded;
static std::set<std::string> s_favs;   // lower-case file names
static std::vector<LibGame> s_lib;
static char s_libDrive[16] = "game";

static void lib_file(const char *name, char *out, size_t size)
{
    _snprintf(out, size - 1, "%s:\\config\\%s", s_libDrive, name);
    out[size - 1] = 0;
}

static std::string fav_key(const std::string &file)
{
    const char *base = strrchr(file.c_str(), '\\');
    std::string k = base ? base + 1 : file;
    size_t i;
    for (i = 0; i < k.size(); i++) k[i] = (char)tolower((unsigned char)k[i]);
    return k;
}

static void favs_load(void)
{
    char path[64], line[300];
    FILE *f;
    if (s_favsLoaded) return;
    s_favsLoaded = 1;
    lib_file("favorites.txt", path, sizeof(path));
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        if (n) s_favs.insert(fav_key(line));
    }
    fclose(f);
}

static void favs_save(void)
{
    char path[64];
    FILE *f;
    std::set<std::string>::const_iterator it;
    lib_file("favorites.txt", path, sizeof(path));
    if (!(f = fopen(path, "w"))) { H64_WARN("[menu] cannot write %s", path); return; }
    for (it = s_favs.begin(); it != s_favs.end(); ++it) fprintf(f, "%s\n", it->c_str());
    fclose(f);
}

static int fav_is(const std::string &file) { favs_load(); return s_favs.count(fav_key(file)) != 0; }

// Toggles a favourite; returns 1 when it is one now.
static int fav_toggle(const std::string &file)
{
    std::string k = fav_key(file);
    int now;
    favs_load();
    if (s_favs.count(k)) { s_favs.erase(k); now = 0; }
    else { s_favs.insert(k); now = 1; }
    favs_save();
    return now;
}

static void cats_load(void)
{
    char path[64], line[200];
    FILE *f;
    if (s_catsLoaded) return;
    s_catsLoaded = 1;
    s_catCount = 0;
    lib_file("categories.txt", path, sizeof(path));
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f) && s_catCount < MAX_CATS)
    {
        char *eq = strchr(line, '=');
        size_t n = strlen(line);
        int k;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!eq) continue;
        *eq = 0;
        for (k = 0; k < CF_COUNT; k++)
            if (!strcmp(line, s_catFieldKey[k]) && eq[1])
            {
                s_cats[s_catCount].field = k;
                strncpy(s_cats[s_catCount].value, eq + 1, sizeof(s_cats[0].value) - 1);
                s_cats[s_catCount].value[sizeof(s_cats[0].value) - 1] = 0;
                s_catCount++;
                break;
            }
    }
    fclose(f);
}

static void cats_save(void)
{
    char path[64];
    FILE *f;
    int i;
    lib_file("categories.txt", path, sizeof(path));
    if (!(f = fopen(path, "w"))) { H64_WARN("[menu] cannot write %s", path); return; }
    for (i = 0; i < s_catCount; i++) fprintf(f, "%s=%s\n", s_catFieldKey[s_cats[i].field], s_cats[i].value);
    fclose(f);
}

// Every ROM's information, read now if it was not yet (a header and a small
// file each: a moment for a big library, once).
static void lib_load(void)
{
    std::vector<RomEntry> roms;
    size_t i;
    if (s_libLoaded) return;
    s_libLoaded = 1;
    s_lib.clear();
    scan_roms(&roms);
    for (i = 0; i < roms.size(); i++)
    {
        H64GameInfo gi;
        LibGame g;
        g.file = roms[i].file;
        if (ArtReadInfo(roms[i].file.c_str(), &gi))
        {
            g.field[CF_GENRE] = gi.genre;
            g.field[CF_REGION] = gi.region;
            g.field[CF_PLAYERS] = gi.players;
            g.field[CF_DEVELOPER] = gi.developer;
            g.field[CF_PUBLISHER] = gi.publisher;
            g.field[CF_YEAR] = gi.year;
        }
        s_lib.push_back(g);
    }
    H64_INFO("[menu] library information: %u games", (unsigned)s_lib.size());
}

// The values a field holds ("Action / Platform": "Action" and "Platform";
// "Japan, USA": both regions), each trimmed.
static void field_values(const std::string &text, int field, std::vector<std::string> *out)
{
    const char *seps = field == CF_GENRE ? "/,;|" : field == CF_REGION ? ",;/" : "";
    size_t start = 0, i;
    for (i = 0; i <= text.size(); i++)
        if (i == text.size() || (seps[0] && strchr(seps, text[i])))
        {
            size_t a = start, b = i;
            while (a < b && text[a] == ' ') a++;
            while (b > a && text[b - 1] == ' ') b--;
            if (b > a) out->push_back(text.substr(a, b - a));
            start = i + 1;
        }
}

static int game_in_cat(const std::string &file, const GameCat *cat)
{
    size_t i, k;
    for (i = 0; i < s_lib.size(); i++)
        if (!_stricmp(s_lib[i].file.c_str(), file.c_str()))
        {
            std::vector<std::string> vals;
            field_values(s_lib[i].field[cat->field], cat->field, &vals);
            for (k = 0; k < vals.size(); k++)
                if (!_stricmp(vals[k].c_str(), cat->value)) return 1;
            return 0;
        }
    return 0;
}

static bool str_iless(const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; }

// The values of a field found in the library, sorted, each once.
static void cat_values(int field, std::vector<std::string> *out)
{
    size_t i, k;
    lib_load();
    out->clear();
    for (i = 0; i < s_lib.size(); i++)
    {
        std::vector<std::string> vals;
        field_values(s_lib[i].field[field], field, &vals);
        for (k = 0; k < vals.size(); k++)
        {
            size_t j;
            int seen = 0;
            for (j = 0; j < out->size() && !seen; j++) seen = !_stricmp((*out)[j].c_str(), vals[k].c_str());
            if (!seen) out->push_back(vals[k]);
        }
    }
    std::sort(out->begin(), out->end(), str_iless);
}

// A value as shown: regions translated, players as "1-4 players".
static void cat_value_text(int field, const char *value, char *out, size_t size)
{
    if (field == CF_REGION) _snprintf(out, size - 1, "%s", TrRegion(value));
    else if (field == CF_PLAYERS && !strcmp(value, "1")) _snprintf(out, size - 1, "%s", Tr(T_ONE_PLAYER));
    else if (field == CF_PLAYERS) _snprintf(out, size - 1, Tr(T_N_PLAYERS), value);
    else _snprintf(out, size - 1, "%s", value);
    out[size - 1] = 0;
}

// "Genre: Platform".
static void cat_name(const GameCat *cat, char *out, size_t size)
{
    char v[96];
    cat_value_text(cat->field, cat->value, v, sizeof(v));
    _snprintf(out, size - 1, "%s: %s", Tr(s_catFieldLabel[cat->field]), v);
    out[size - 1] = 0;
}

// The ROM list's tabs (LT/RT): every game, the favourites, then each category.
static int s_tab, s_tabGames;   // the tab shown, and how many games it has
static int s_romsTotal;         // games in game:\roms\ (0: the "copy ROMs" screen, no tabs)

// A USB memory plugged in while the ROM list is up, its games added (RomBrowser): its drive (UsbN), -1 none.
static int s_usbMerged = -1;
static int tab_count(void) { cats_load(); return 2 + s_catCount + (s_usbMerged >= 0 ? 1 : 0); }
static int tab_is_usb(int tab) { return s_usbMerged >= 0 && tab == 2 + s_catCount; }
static int rom_on_usb(const std::string &file)
{
    char pre[8];
    sprintf(pre, "Usb%d:", s_usbMerged);
    return s_usbMerged >= 0 && !_strnicmp(file.c_str(), pre, 5);
}

static void tab_name(int tab, char *out, size_t size)
{
    if (tab == 0) _snprintf(out, size - 1, "%s", Tr(T_CAT_ALL));
    else if (tab == 1) _snprintf(out, size - 1, "%s", Tr(T_FAVORITES));
    else if (tab_is_usb(tab)) _snprintf(out, size - 1, "%s", Tr(T_CAT_USB));
    else cat_name(&s_cats[tab - 2], out, size);
    out[size - 1] = 0;
}

// The games of the tab shown, in the list's order.
static void tab_filter(const std::vector<RomEntry> &all, std::vector<RomEntry> *out)
{
    size_t i;
    out->clear();
    if (s_tab < 0 || s_tab >= tab_count()) s_tab = 0;
    if (s_tab >= 2 && !tab_is_usb(s_tab)) lib_load();
    for (i = 0; i < all.size(); i++)
        if (s_tab == 0 || (s_tab == 1 && fav_is(all[i].file)) || (tab_is_usb(s_tab) && rom_on_usb(all[i].file)) ||
            (s_tab >= 2 && !tab_is_usb(s_tab) && game_in_cat(all[i].file, &s_cats[s_tab - 2])))
            out->push_back(all[i]);
    s_tabGames = (int)out->size();
}

// The favourite mark: a small gold star (two triangles), `s` across.
static void fav_star(float cx, float cy, float s, float a)
{
    D3DCOLOR gold = UiAlpha(D3DCOLOR_ARGB(255, 255, 205, 70), a);
    icon_tri(cx, cy - s * 0.12f, s, 0, gold);
    icon_tri(cx, cy + s * 0.12f, s, 2, gold);
}

// ---- Settings > Interface > Categories ----
// The list: "Make a category", then each category (A changes it). Making or
// changing one: its field (genre, region...), its value among those the
// library's games have, then save - or delete it.
enum { CE_FIELD = 0, CE_VALUE, CE_SAVE, CE_DELETE };
static int s_ceIndex, s_ceField, s_ceValue;   // the category changed (-1: a new one), its field, its value's index
static std::vector<std::string> s_ceValues;   // that field's values in the library

static void settings_cats(UiMenu *m, const Config *c, int selCat)
{
    int i;
    (void)c;
    cats_load();
    s_interfacePage = 0;
    s_catEditPage = 0;
    s_catsPage = 1;
    UiMenuClear(m, Tr(T_CATEGORIES), Tr(T_FOOT_SELECT));
    m->icon[UiMenuAdd(m, Tr(T_CAT_NEW), "")] = UI_ICON_PLUS;
    for (i = 0; i < s_catCount; i++)
    {
        char name[160];
        cat_name(&s_cats[i], name, sizeof(name));
        m->icon[UiMenuAdd(m, name, "")] = UI_ICON_LIST;
    }
    if (selCat >= 0 && selCat + 1 < m->count) m->sel = selCat + 1;
}

static void catedit_values(UiMenu *m)
{
    char v[96];
    sprintf(m->value[CE_FIELD], "< %s >", Tr(s_catFieldLabel[s_ceField]));
    if (s_ceValues.empty()) _snprintf(v, sizeof(v) - 1, "%s", Tr(T_CAT_NO_DATA));
    else cat_value_text(s_ceField, s_ceValues[s_ceValue].c_str(), v, sizeof(v));
    v[sizeof(v) - 1] = 0;
    {
        char fit[48];
        UiTextFit(UI_BOLD, 21, 260, v, fit, sizeof(fit));
        sprintf(m->value[CE_VALUE], "< %s >", fit);
    }
}

static void settings_catedit(UiMenu *m, int index)
{
    size_t k;
    s_catsPage = 0;
    s_catEditPage = 1;
    s_ceIndex = index;
    s_ceField = index >= 0 ? s_cats[index].field : CF_GENRE;
    cat_values(s_ceField, &s_ceValues);   // the library's information: read now the first time
    s_ceValue = 0;
    if (index >= 0)
        for (k = 0; k < s_ceValues.size(); k++)
            if (!_stricmp(s_ceValues[k].c_str(), s_cats[index].value)) s_ceValue = (int)k;
    UiMenuClear(m, Tr(index >= 0 ? T_CATEGORIES : T_CAT_NEW), Tr(T_FOOT_SELECT));
    m->icon[UiMenuAdd(m, Tr(T_CAT_FIELD), "")] = UI_ICON_SLIDERS;
    m->icon[UiMenuAdd(m, Tr(T_CAT_VALUE), "")] = UI_ICON_LIST;
    m->icon[UiMenuAdd(m, Tr(T_CAT_SAVE), "")] = UI_ICON_SAVE;
    if (index >= 0) m->icon[UiMenuAdd(m, Tr(T_CAT_DELETE), "")] = UI_ICON_TRASH;
    catedit_values(m);
}

static int cats_input(UiMenu *m, Config *c, WORD down)
{
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_interface(m, c);
        m->sel = I_CATS;
        return SET_NONE;
    }
    if (down & XINPUT_GAMEPAD_A)
    {
        if (m->sel == 0 && s_catCount >= MAX_CATS)
        {
            xb_audio_ui_sound_instead(XB_SFX_ERROR);   // full
            return SET_NONE;
        }
        settings_catedit(m, m->sel - 1);
    }
    return SET_NONE;
}

static int catedit_input(UiMenu *m, Config *c, WORD down)
{
    int dir = (down & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (down & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
    UiMenuNavigate(m, down);
    if (down & XINPUT_GAMEPAD_B)
    {
        settings_cats(m, c, s_ceIndex);
        return SET_NONE;
    }
    if (m->sel == CE_FIELD && (dir || (down & XINPUT_GAMEPAD_A)))
    {
        s_ceField = (s_ceField + (dir ? dir : 1) + CF_COUNT) % CF_COUNT;
        cat_values(s_ceField, &s_ceValues);
        s_ceValue = 0;
        catedit_values(m);
        return SET_NONE;
    }
    if (m->sel == CE_VALUE && (dir || (down & XINPUT_GAMEPAD_A)) && !s_ceValues.empty())
    {
        int n = (int)s_ceValues.size();
        s_ceValue = (s_ceValue + (dir ? dir : 1) + n) % n;
        catedit_values(m);
        return SET_NONE;
    }
    if (m->sel == CE_SAVE && (down & XINPUT_GAMEPAD_A) && !s_ceValues.empty())
    {
        int at = s_ceIndex >= 0 ? s_ceIndex : s_catCount;
        if (at >= MAX_CATS) return SET_NONE;
        s_cats[at].field = s_ceField;
        strncpy(s_cats[at].value, s_ceValues[s_ceValue].c_str(), sizeof(s_cats[0].value) - 1);
        s_cats[at].value[sizeof(s_cats[0].value) - 1] = 0;
        if (s_ceIndex < 0) s_catCount++;
        cats_save();
        settings_cats(m, c, at);
        return SET_NONE;
    }
    if (m->sel == CE_DELETE && (down & XINPUT_GAMEPAD_A) && s_ceIndex >= 0)
    {
        int i;
        for (i = s_ceIndex; i + 1 < s_catCount; i++) s_cats[i] = s_cats[i + 1];
        s_catCount--;
        if (s_tab >= 2 + s_catCount) s_tab = 0;   // the ROM list was on that tab
        cats_save();
        settings_cats(m, c, s_ceIndex - 1);
        return SET_NONE;
    }
    return SET_NONE;
}

// The tab bar: "LT  Favourites (12)  RT", centred on `x` (the covers and the
// carousel, at the top) or starting at it (the list, under the logo: a lower
// pill, just around the LT/RT glyphs, so it sits closer over the list).
static void browser_tabs(float x, float cy, int centred, float maxW)
{
    const float h = centred ? 34.0f : 28.0f;
    char name[160], full[200], label[200];
    float w;
    tab_name(s_tab, name, sizeof(name));
    _snprintf(full, sizeof(full) - 1, "%s (%d)", name, s_tabGames);
    full[sizeof(full) - 1] = 0;
    UiTextFit(UI_BOLD, 19, maxW - 128, full, label, sizeof(label));
    w = UiTextWidth(UI_BOLD, 19, label);
    x = centred ? x - w * 0.5f : x + 64;
    UiRoundRect(x - 64, cy - h * 0.5f, w + 128, h, h * 0.5f, D3DCOLOR_ARGB(150, 30, 34, 46), D3DCOLOR_ARGB(150, 24, 27, 37));
    UiRoundRing(x - 64, cy - h * 0.5f, w + 128, h, h * 0.5f, 1.5f, SEL_HI(120));
    UiFooter(x - 58, cy, "LT");
    UiText(UI_BOLD, 19, x, cy, UI_COL_TEXT, label, UI_LEFT);
    UiFooter(x + w + 58 - UiFooterWidth("RT"), cy, "RT");
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
static char s_hiresDir[128] = "game:\\hires_texture";   // Settings > Paths > Textures

void HiresSetFolder(const char *folder)
{
    strncpy(s_hiresDir, folder && folder[0] ? folder : "game:\\hires_texture", sizeof(s_hiresDir) - 1);
    s_hiresDir[sizeof(s_hiresDir) - 1] = 0;
}

void HiresScanPacks(const char *romName)
{
    char dir[160], pattern[176];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    s_hiresPackCount = 0;
    if (!romName || !romName[0]) return;
    _snprintf(dir, sizeof(dir) - 1, "%s\\%s", s_hiresDir, romName);
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
        if (r == SET_PICK_PATH)
        {
            // Settings > Paths: a folder for it, made if it is not there.
            char out[128];
            if (FolderPicker(dev, ConfigPath(c, s_pickWhich, "game"), out, sizeof(out)))
            {
                strcpy(c->path[s_pickWhich], out);
                mkdir_p(out);
                ui_from_config(c);
            }
            settings_values(&m, c);
            UiInputInit(&in);
            continue;
        }
        if (r == SET_PATH_RESET)
        {
            // Settings > Paths: Y on a path, or Reset - asked first.
            int row = m.sel, one = s_pathReset >= 0 && s_pathReset < CONFIG_PATH_COUNT;
            if (confirm_ask(dev, T_FX_RESET, one ? T_PATH_RESET_ONE : T_PATH_RESET_ALL, T_FOOT_PATH_RESET))
            {
                paths_reset(&m, c);
                xb_audio_ui_sound(XB_SFX_VALUE_UP);
            }
            m.sel = row;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_ABOUT)
        {
            int row = m.sel;
            AboutScreen(dev);   // Settings > About
            m.sel = row;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_EXPLORER)
        {
            // System > File explorer: only after the warning.
            if (confirm_ask(dev, T_EXP_WARN_TITLE, T_EXP_WARN, T_FOOT_EXP_WARN)) FileExplorer(dev);
            settings_system(&m, c);
            m.sel = SYS_EXPLORER;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_WIZARD)
        {
            // System > Setup assistant: the first start's pages, kept at once.
            WelcomeScreen(dev, c, 0);
            H64_INFO("[menu] settings for all games %s", ConfigWriteMenuKeys(c, settingsPath, 1) ? "saved" : "NOT saved");
            settings_system(&m, c);
            m.sel = SYS_WIZARD;
            UiInputInit(&in);
            continue;
        }
        if (r == SET_SAVER)
        {
            saver_run(dev, c);
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
            // From the Screenshots and logos row: the question names those (the box art comes with them).
            const int extras = s_imagesPage && m.sel == IM_EXTRAS;
            if ((extras ? confirm_ask(dev, T_ART_EXTRAS, T_ART_ASK_EXTRAS, T_FOOT_ART_ASK) : confirm_ask(dev, -1, T_ART_ASK, T_FOOT_ART_ASK)))
            {
                c->artDownload = 1;
                if (extras) c->artExtras = 1;
                if (s_imagesPage && m.sel == IM_BACKGROUND) c->artBackground = 1;   // asked from Download while playing
            }
            settings_values(&m, c);
            UiInputInit(&in);
            continue;
        }
        if (r == SET_BACK || r == SET_SAVE_ALL)
        {
            // From the ROM list these settings are for all games: kept on leaving too (B), not only
            // through "Save for all games" - a volume set here was lost at the next start otherwise.
            H64_INFO("[menu] settings for all games %s", ConfigWriteMenuKeys(c, settingsPath, 1) ? "saved" : "NOT saved");
            s_browser = 0;
            return;
        }
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        UiBackdrop();
        browser_effect();   // the ROM list's, so a new choice shows at once
        UiMenuDraw(&m);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

// ---- Box art in the browser ----
// The browser's copies of what xb_art.cpp sends (textures and text), by ROM
// file; the least recently shown is dropped when the table is full. Room for
// a covers grid's page and the two read ahead around it (3 x 15 - big covers and
// the carousel ask for more), so turning to a page seen a moment ago shows it at
// once. 64 was less than some layouts ask for at once: covers were dropped and read
// again while the pages were turned (slow with many ROMs). 400x232 textures: ~47 MB
// at most, in the ROM list only (freed when a game starts).
#define ART_VIEWS 128
struct ArtView
{
    int used;
    char path[256];
    int hasInfo;
    H64GameInfo info;
    IDirect3DTexture9 *cover, *glow;
    int coverW, coverH;
    int kind, kinds;    // the picture's ART_KIND_* asked for, and the ones there are of the game
    int shownKind;      // the one it shows (the box when that one is not there): only a box is drawn in 3D
    float coverAt;      // UiTime() when the cover arrived (its fade-in)
    // The picture it showed before another kind of it came (RS, the automatic turns):
    // kept a moment so the two cross-fade (view_cover), then released.
    IDirect3DTexture9 *prev;
    int prevW, prevH, prevBox;
    float swapAt;
    DWORD lastUse;
};
static ArtView s_views[ART_VIEWS];
static DWORD s_viewClock;

static void view_free(IDirect3DDevice9 *dev, ArtView *v)
{
    if (v->cover) UiImageRelease(dev, v->cover);
    if (v->glow) UiImageRelease(dev, v->glow);
    if (v->prev) UiImageRelease(dev, v->prev);
    memset(v, 0, sizeof(*v));
}

// A game's picture changing kind: the one before fades out, growing a little, while
// the new one fades in from a little smaller (0 to 1 over VIEW_SWAP seconds).
#define VIEW_SWAP 0.5f
static float smooth01(float t);   // below
static float view_swap(const ArtView *v)
{
    return v->prev ? smooth01((UiTime() - v->swapAt) / VIEW_SWAP) : 1.0f;
}

// A game's picture in a box of the layout (all but the card, which keeps each one's
// own proportions), cross-fading from the one before while it changes.
static void view_cover(const ArtView *v, float x, float y, float w, float h, float r, D3DCOLOR color)
{
    const float e = view_swap(v);
    const int a = (int)(color >> 24);
    const D3DCOLOR rgb = color & 0xFFFFFF;
    if (e < 1.0f)
    {
        const float gw = w * 0.04f * e, gh = h * 0.04f * e, sw = w * 0.04f * (1.0f - e), sh = h * 0.04f * (1.0f - e);
        // The old one stays nearly whole until the new one is well in (no dip in between).
        cover_draw(v->prev, x - gw * 0.5f, y - gh * 0.5f, w + gw, h + gh, r, rgb | ((DWORD)(a * (1.0f - e * e)) << 24), v->prevBox);
        cover_draw(v->cover, x + sw * 0.5f, y + sh * 0.5f, w - sw, h - sh, r, rgb | ((DWORD)(a * e) << 24), v->shownKind == ART_KIND_BOX);
        return;
    }
    cover_draw(v->cover, x, y, w, h, r, color, v->shownKind == ART_KIND_BOX);
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

static void cine_take(IDirect3DDevice9 *dev);    // the selected game's bigger pictures (below)
static void cine_clear(IDirect3DDevice9 *dev);
static void game_backdrop(const std::string &file, int flags);

// Turns the worker's results into textures.
static void views_take(IDirect3DDevice9 *dev)
{
    ArtResult r;
    int made = 0, i;
    // The pictures cross-faded out: released.
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].prev && UiTime() - s_views[i].swapAt > VIEW_SWAP + 0.1f)
        {
            UiImageRelease(dev, s_views[i].prev);
            s_views[i].prev = NULL;
        }
    // At most 3 new pictures a frame: a whole page (or three) arriving at once made
    // dozens of textures in one frame - a visible hitch. The rest come next frames.
    while (made < 3 && ArtTake(&r))
    {
        ArtView *v;
        made++;
        if (r.kind != ArtKindFor(r.path))
        {
            // Read for the kind of picture shown before (Interface > Game images, the right stick): it is sent again.
            ArtResultFree(&r);
            continue;
        }
        v = view_find(r.path);
        float coverAt = UiTime();
        IDirect3DTexture9 *prev = NULL;
        int prevW = 0, prevH = 0, prevBox = 0;
        if (v)
        {
            // Newer files for a ROM already shown (a download finished): a cover it had does not fade in again.
            if (v->cover && (r.cover || r.dxt)) coverAt = v->coverAt;
            // Another kind of its picture (RS, the automatic turns): the one shown kept to cross-fade from.
            if (v->cover && (r.cover || r.dxt) && (r.kind != v->kind || r.shownKind != v->shownKind))
            {
                prev = v->cover;
                prevW = v->coverW;
                prevH = v->coverH;
                prevBox = v->shownKind == ART_KIND_BOX;
                v->cover = NULL;
            }
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
        v->kind = r.kind;
        v->kinds = r.kinds;
        v->shownKind = r.shownKind;
        if (r.dxt || r.cover)
        {
            // Compressed (the cover cache's): a straight copy, its padding cut off when drawn.
            v->cover = r.dxt ? UiImageCreateDxt1(dev, r.dxt, r.dxtW, r.dxtH) : UiImageCreate(dev, r.cover, r.coverW, r.coverH);
            v->coverW = r.coverW;
            v->coverH = r.coverH;
        }
        if (r.glow) v->glow = UiImageCreate(dev, r.glow, ART_GLOW_W, ART_GLOW_H);
        v->coverAt = coverAt;
        if (prev)
        {
            if (v->cover)
            {
                v->prev = prev;
                v->prevW = prevW;
                v->prevH = prevH;
                v->prevBox = prevBox;
                v->swapAt = UiTime();
            }
            else UiImageRelease(dev, prev);
        }
        v->lastUse = ++s_viewClock;
        ArtResultFree(&r);
    }
    cine_take(dev);
}

// The menus' music volume, changed with RS up/down in the ROM list: a small bar at the
// top that slides in, shows the level, and slides away after a moment.
static float s_musicVolAt = -10.0f;
#define VOLBAR_SHOW 3.0f    // seconds shown after the last change
#define VOLBAR_SLIDE 0.3f   // sliding in and out

// A volume step: the bar comes in, or - already in - stays put (each step restarting
// its slide made it vanish and come back while the stick was held).
static void music_volume_poke(void)
{
    float t = UiTime() - s_musicVolAt;
    s_musicVolAt = t >= 0 && t < VOLBAR_SHOW + VOLBAR_SLIDE ? UiTime() - VOLBAR_SLIDE : UiTime();
}

static void music_volume_bar(int volume)
{
    const float showFor = VOLBAR_SHOW, slide = VOLBAR_SLIDE, w = 300, h = 46;
    float t = UiTime() - s_musicVolAt, a, y;
    char num[16];
    if (t < 0 || t > showFor + slide) return;
    a = t < slide ? t / slide : t > showFor ? 1.0f - (t - showFor) / slide : 1.0f;
    a = a * a * (3.0f - 2.0f * a);
    y = -h + (24 + h) * a;
    UiShadow((UI_WIDTH - w) * 0.5f, y + 6, w, h, 14, 24, D3DCOLOR_ARGB((int)(120 * a), 0, 0, 0));
    UiRoundRect((UI_WIDTH - w) * 0.5f, y, w, h, 14, D3DCOLOR_ARGB((int)(230 * a), 33, 35, 46), D3DCOLOR_ARGB((int)(230 * a), 27, 29, 38));
    menu_icon(UI_ICON_MUSIC, (UI_WIDTH - w) * 0.5f + 28, y + h * 0.5f, UiAlpha(UI_COL_TEXT, a));
    {
        const float bx = (UI_WIDTH - w) * 0.5f + 54, bw = w - 54 - 70, by = y + h * 0.5f - 4;
        UiRoundRect(bx, by, bw, 8, 4, D3DCOLOR_ARGB((int)(50 * a), 255, 255, 255), D3DCOLOR_ARGB((int)(35 * a), 255, 255, 255));
        UiRoundRect(bx, by, bw * volume / 100.0f, 8, 4, UiAlpha(SEL_HI(255), a), UiAlpha(SEL_LO(255), a));
    }
    sprintf(num, "%d%%", volume);
    UiText(UI_BOLD, 19, (UI_WIDTH + w) * 0.5f - 18, y + h * 0.5f, UiAlpha(UI_COL_TEXT, a), num, UI_RIGHT);
}

// "Downloading", discreet, bottom right over the footer: a picture is being fetched (any view).
static void download_badge(void)
{
    const char *t = Tr(T_DOWNLOADING);
    float tw = UiTextWidth(UI_BOLD, 17, t), w = tw + 56, h = 32, x = UI_WIDTH - UI_SAFE_X - w, y = 612;
    int k;
    UiRoundRect(x, y, w, h, 16, D3DCOLOR_ARGB(200, 33, 35, 46), D3DCOLOR_ARGB(200, 27, 29, 38));
    UiRoundRing(x, y, w, h, 16, 1.0f, UI_COL_LINE);
    for (k = 0; k < 3; k++)   // three dots taking turns
    {
        float on = fmodf(UiTime() * 2.0f - k * 0.33f, 1.0f);
        UiCircle(x + 18 + k * 8, y + h * 0.5f, 2.5f, UiAlpha(SEL_HI(255), on < 0.5f ? 1.0f : 0.35f));
    }
    UiText(UI_BOLD, 17, x + w - 14, y + h * 0.5f, UI_COL_TEXT2, t, UI_RIGHT);
}

// The right stick, left or right, newly pushed (-1, 1; 0 none): the selected game's next picture.
static int right_stick_step(void)
{
    static int was;
    XINPUT_STATE st;
    int now = 0, step;
    if (XInputGetState(0, &st) == ERROR_SUCCESS)
        now = st.Gamepad.sThumbRX > 20000 ? 1 : st.Gamepad.sThumbRX < -20000 ? -1 : 0;
    step = now != was ? now : 0;
    was = now;
    return step;
}

// The selected game's picture in its next kind there is (box, a picture from the game, the title screen, the logo).
// 1 when it changed; `sound` 0 for the automatic turns (below).
static int art_step(const std::string &file, int dir, int sound = 1)
{
    const ArtView *v = view_find(file.c_str());
    int k, cur;
    if (!v || !dir || !(v->kinds & (v->kinds - 1))) return 0;   // one picture or none: nothing to go through
    cur = v->kind;
    for (k = 1; k < ART_KIND_COUNT; k++)
    {
        int next = ((cur + dir * k) % ART_KIND_COUNT + ART_KIND_COUNT) % ART_KIND_COUNT;
        if (v->kinds & (1 << next))
        {
            ArtShowKind(file.c_str(), next == ArtGetKind() ? -1 : next);
            if (sound) xb_audio_ui_sound(dir > 0 ? XB_SFX_VALUE_UP : XB_SFX_VALUE_DOWN);
            return 1;
        }
    }
    return 0;
}

// Staying on a game: after 2 s its pictures take turns by themselves (box, a picture
// from the game, the title screen, the logo - those there are), one every 2.5 s; the
// game left goes back to its own picture (the one Interface > Game images shows).
static void art_auto_turn(const std::string &file)
{
    static std::string at;
    static float next;
    static int turned;
    const float now = UiTime();
    if (file != at)
    {
        if (turned && !at.empty()) ArtShowKind(at.c_str(), -1);   // back to its own picture
        at = file;
        next = now + 2.0f;
        turned = 0;
        return;
    }
    if (!file.empty() && now >= next)
    {
        turned |= art_step(file, 1, 0);
        next = now + 2.5f;
    }
}

static void views_clear(IDirect3DDevice9 *dev)
{
    int i;
    for (i = 0; i < ART_VIEWS; i++)
        if (s_views[i].used) view_free(dev, &s_views[i]);
    cine_clear(dev);
    rec_free(&s_rec[0]);   // the wallpapers' recommendation: picked again in the list
    rec_free(&s_rec[1]);
    UiImageFlush(dev);   // freed now, not when the GPU is next looked at: a game may follow
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

// The picture area while a game's cover comes (or when it has none): a plain
// dark tile, no initials nor colours of its own - the page keeps its shape.
static void stage_placeholder(float x, float y, float w, float h, const char *name, float a)
{
    (void)name;
    if (a <= 0.01f) return;
    UiRoundRect(x, y, w, h, 16, UiAlpha(D3DCOLOR_ARGB(150, 38, 40, 52), a), UiAlpha(D3DCOLOR_ARGB(150, 30, 32, 42), a));
    UiRoundRing(x, y, w, h, 16, 1.0f, UiAlpha(D3DCOLOR_ARGB(26, 255, 255, 255), a));
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
    UiTextFit(UI_BOLD, 11, w, label, fit, sizeof(fit));
    UiText(UI_BOLD, 11, x, y, UiAlpha(UI_COL_TEXT3, a), fit, UI_LEFT);
    UiTextFit(UI_REGULAR, 15, w, value, fit, sizeof(fit));
    UiText(UI_REGULAR, 15, x, y + 18, UiAlpha(UI_COL_TEXT, a), fit, UI_LEFT);
}

// The card on the right: the game's picture (Interface > Game images; the
// right stick goes through the ones there are), its name under it, then the
// facts and a few lines about it. Nothing stands in for a missing picture
// (no initials, no frame: a list scrolled fast shows only what is there).
// `selAt`: when the selection changed (the card's text fades and slides in).
#define CARD_PIC_H 210.0f   // the picture's room (its own proportions, at most this high)
static void card_draw(const char *shown, const RomInfo *ri, const ArtView *v, int busy, float selAt)
{
    const float x = CARD_X, y = CARD_Y, w = CARD_W, h = CARD_B - CARD_Y, ix = x + 22, iw = w - 44;
    const float py = y + 20, divY = CARD_B - 52;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    float now = UiTime(), a = 0.3f + 0.7f * smooth01((now - selAt) / 0.2f), slide = (1.0f - smooth01((now - selAt) / 0.2f)) * 10.0f;
    float ca = 0, ty;
    char buf[1024];
    (void)busy;

    UiPanel(x, y, w, h, 22);

    // ---- The picture: its own proportions, centred in its room, framed by its own edge.
    if (v && v->cover)
        ca = smooth01((now - (v->coverAt > selAt ? v->coverAt : selAt)) / 0.25f);
    if (ca > 0)
    {
        float cw = (float)v->coverW, chh = (float)v->coverH, k = iw / cw, cx, cy;
        if (chh * k > CARD_PIC_H) k = CARD_PIC_H / chh;
        cw *= k;
        chh *= k;
        cx = ix + (iw - cw) * 0.5f;
        cy = py + (CARD_PIC_H - chh) * 0.5f + (1.0f - ca) * 6.0f;
        UiShadow(cx, cy + 6, cw, chh, 12, 18, UiAlpha(D3DCOLOR_ARGB(140, 0, 0, 0), ca * a));
        const float e = view_swap(v);
        if (e < 1.0f)
        {
            // The picture before, in its own proportions, fading out as this one comes.
            float pw = (float)v->prevW, ph = (float)v->prevH, pk = iw / pw, gk = 1.0f + 0.04f * e;
            if (ph * pk > CARD_PIC_H) pk = CARD_PIC_H / ph;
            pw *= pk * gk;
            ph *= pk * gk;
            cover_draw(v->prev, ix + (iw - pw) * 0.5f, py + (CARD_PIC_H - ph) * 0.5f, pw, ph, 12,
                       UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a * (1.0f - e * e)), v->prevBox);
        }
        cover_draw(v->cover, cx, cy, cw, chh, 12, UiAlpha(D3DCOLOR_ARGB(255, 255, 255, 255), ca * a * e), v->shownKind == ART_KIND_BOX);
        if (!s_cover3d || v->shownKind != ART_KIND_BOX) UiRoundRing(cx, cy, cw, chh, 12, 1.0f, UiAlpha(D3DCOLOR_ARGB(46, 255, 255, 255), ca * a));
    }
    // Which of its pictures this is, when it has more than one (RS: the next).
    if (v && (v->kinds & (v->kinds - 1)))
    {
        int k, n = 0, at = 0;
        float dx;
        for (k = 0; k < ART_KIND_COUNT; k++)
            if (v->kinds & (1 << k)) { if (k == v->kind) at = n; n++; }
        dx = ix + iw * 0.5f - (n - 1) * 7.0f;
        for (k = 0; k < n; k++) UiCircle(dx + k * 14.0f, py + CARD_PIC_H + 10, k == at ? 3.5f : 2.5f, k == at ? SEL_HI(230) : D3DCOLOR_ARGB(90, 255, 255, 255));
    }

    // ---- The name under the picture: one line as large as fits, else two smaller.
    ty = py + CARD_PIC_H + 38 + slide;
    {
        const char *title = gi && gi->title[0] ? gi->title : shown;
        float size = 24;
        int lines;
        if (UiTextWidth(UI_BOLD, size, title) > iw) size = 21;
        lines = UiTextWrap(UI_BOLD, size, iw, 2, title, buf, sizeof(buf));
        UiText(UI_BOLD, size, ix, ty, UiAlpha(UI_COL_TEXT, a), buf, UI_LEFT);
        ty += (lines - 1) * size * 1.35f + 28;
    }

    // ---- Year, developer, publisher.
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
        UiTextFit(UI_REGULAR, 16, iw, buf, fit, sizeof(fit));
        UiText(UI_REGULAR, 16, ix, ty, UiAlpha(gi ? UI_COL_TEXT2 : UI_COL_TEXT3, a), fit, UI_LEFT);
        ty += 28;
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
        if (any) ty += 28;
    }

    // ---- A few lines about the game (Wikipedia), as many as fit above the last row.
    if (gi && gi->description[0])
    {
        float lineH = 16 * 1.35f, room = divY - 24 - (ty + 2);
        int maxLines = (int)(room / lineH);
        if (maxLines > 0)
        {
            int lines = UiTextWrap(UI_REGULAR, 16, iw, maxLines, gi->description, buf, sizeof(buf));
            UiText(UI_REGULAR, 16, ix, ty + 2 + lineH * 0.5f, UiAlpha(UI_COL_TEXT2, a), buf, UI_LEFT);
            UiText(UI_REGULAR, 11, ix, ty + 2 + lines * lineH + 8, UiAlpha(UI_COL_TEXT3, a), Tr(T_WIKI_TEXT), UI_LEFT);
        }
    }

    // ---- What the emulator knows from the file: four small columns across the card.
    UiRect(ix, divY, iw, 1, UI_COL_LINE);
    if (!ri->ok)
        UiText(UI_REGULAR, 16, ix, divY + 26, UI_COL_ACCENT_HI, Tr(T_NOT_N64), UI_LEFT);
    else
    {
        detail(ix, divY + 15, 70, Tr(T_GAME_CODE), ri->code, 1.0f);
        detail(ix + 76, divY + 15, 64, Tr(T_SIZE), ri->size, 1.0f);
        detail(ix + 146, divY + 15, 160, Tr(T_SAVE), ri->save, 1.0f);
        // The header's raw internal name: what HiresOpen (main.cpp) matches under
        // game:\hires_texture\<name>\ - shown even when the game database found a
        // nicer display title above, so the user can see the folder name to use.
        detail(ix + 314, divY + 15, iw - 314, Tr(T_INTERNAL_NAME), ri->name, 1.0f);
    }
}

// ---- The ROM list's animated background effects (Settings > Interface > Background effect) ----
// Drawn right after the backdrop, behind every view, all made here from the
// UI's shapes (no pictures): each moves with its own clock (s_fxClock, run at
// the chosen speed). Kept faint enough for the covers and the text over them
// to stay the clearest thing on screen; the chosen opacity scales that.
// Colours: each effect has three of its own (s_fxDefault, along a gradient
// fx_pal walks from 0 to 1); up to three chosen ones (Config::fxColor, from
// s_fxPalette) take their place.
#define WAVE_SLICE 8.0f
static int s_fxRgb[3][3], s_fxRgbCount = 3;   // the colours drawn now (the effect's own or the chosen ones)

// A steady pseudo-random 0..1 for `i` (the particles' own places and speeds).
static float fx_rand(int i)
{
    u32 x = (u32)i * 2654435761u;
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return (x & 0xFFFF) / 65535.0f;
}

// Every colour of the effects comes through here: the chosen opacity applies to all.
static D3DCOLOR fx_col(int a, int r, int g, int b)
{
    a = a * s_fxOpacity / 100;
    return D3DCOLOR_ARGB(a < 0 ? 0 : a > 255 ? 255 : a, r, g, b);
}

// The colour `k` (0..1) along the effect's colours, lightened towards white by `white` (0..1).
static void fx_rgb(float k, float white, int *rgb)
{
    int i0 = 0, i1 = 0, i;
    float f = 0;
    if (k < 0) k = 0;
    if (k > 1) k = 1;
    if (s_fxRgbCount > 1)
    {
        float p = k * (s_fxRgbCount - 1);
        i0 = (int)p;
        if (i0 >= s_fxRgbCount - 1) i0 = s_fxRgbCount - 2;
        i1 = i0 + 1;
        f = p - i0;
    }
    for (i = 0; i < 3; i++)
    {
        float v = s_fxRgb[i0][i] + (s_fxRgb[i1][i] - s_fxRgb[i0][i]) * f;
        rgb[i] = (int)(v + (255 - v) * white);
    }
}
static D3DCOLOR fx_pal(float k, int a)
{
    int c[3];
    fx_rgb(k, 0, c);
    return fx_col(a, c[0], c[1], c[2]);
}
static D3DCOLOR fx_palw(float k, float white, int a)
{
    int c[3];
    fx_rgb(k, white, c);
    return fx_col(a, c[0], c[1], c[2]);
}

// Ribbons of light (the waves, the aurora): slanted slices following two sines
// (UiStrip, so the crest has no steps), a crest line then a fade downwards;
// `k` is each one's place in the colours.
static void fx_ribbons(int count, const float *base, const float *amp, const float *len, const float *speed, const float *thick,
                       const int *alpha, const float *k, float t)
{
    float x;
    int w;
    for (w = 0; w < count; w++)
    {
        // The crest one smooth ribbon (UiPolyline: piece by piece, its joints showed as a saw-tooth).
        int rgb[3], np = 0;
        float kk = 6.2831853f / len[w], px = 0, py = 0, ph = 0, pts[2 * ((int)(UI_WIDTH / WAVE_SLICE) + 3)];
        fx_rgb(k[w], 0, rgb);
        for (x = 0; x <= UI_WIDTH + WAVE_SLICE; x += WAVE_SLICE)
        {
            float y = base[w] + amp[w] * sinf(x * kk + t * speed[w] * 2.0f + w * 1.7f) + amp[w] * 0.4f * sinf(x * kk * 2.3f - t * speed[w] * 1.3f);
            float h = thick[w] * (0.75f + 0.25f * sinf(x * kk * 0.7f + t * 0.5f + w));
            if (x > 0) UiStrip(px, py, py + ph, x, y, y + h, fx_col(alpha[w], rgb[0], rgb[1], rgb[2]), fx_col(0, rgb[0], rgb[1], rgb[2]));
            if (np < (int)(sizeof(pts) / sizeof(pts[0])) / 2) { pts[2 * np] = x; pts[2 * np + 1] = y; np++; }
            px = x; py = y; ph = h;
        }
        UiPolyline(pts, np, 2.0f, 1.5f, fx_col(alpha[w] * 3, rgb[0], rgb[1], rgb[2]));
    }
}

static void fx_waves(float t)
{
    static const float base[4] = { 250, 375, 470, 565 }, amp[4] = { 26, 34, 22, 30 }, len[4] = { 620, 760, 540, 900 };
    static const float speed[4] = { 0.35f, -0.27f, 0.45f, -0.20f }, thick[4] = { 70, 90, 60, 80 }, k[4] = { 0, 0.33f, 0.67f, 1 };
    static const int alpha[4] = { 26, 34, 22, 30 };
    fx_ribbons(4, base, amp, len, speed, thick, alpha, k, t);
}

// Image 1: a neon ring, its rim of bars beating like a sound meter.
static void fx_pulse(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f, beat = 0.5f + 0.5f * sinf(t * 3.1f);
    float r = 120 + 6 * beat;
    int i, k;
    UiShadow(cx - r - 30, cy - r - 30, (r + 30) * 2, (r + 30) * 2, r + 30, 50, fx_pal(0.5f, 40));
    for (i = 0; i < 72; i++)
    {
        float a = i * 6.2831853f / 72, f = (float)i / 72;
        float len = 14 + 34 * (0.5f + 0.5f * sinf(t * 4.0f + i * 0.9f)) * (0.6f + 0.4f * sinf(t * 1.7f + i * 0.31f));
        D3DCOLOR c = fx_pal(f < 0.5f ? f * 2 : 2 - f * 2, 150);   // around the ring and back, no seam
        for (k = 0; k < 4; k++)
        {
            float d = r + 10 + len * k / 3.0f;
            UiCircle(cx + cosf(a) * d, cy + sinf(a) * d, 2.0f - k * 0.3f, c);
        }
    }
    UiRoundRing(cx - r, cy - r, r * 2, r * 2, r, 5.0f, fx_palw(0.3f, 0.2f, 200));
    UiRoundRing(cx - r + 14, cy - r + 14, r * 2 - 28, r * 2 - 28, r - 14, 2.0f, fx_palw(0.5f, 0.3f, 140));
}

// Image 2: falling streaks of light, each with a bright head.
static void fx_rain(float t)
{
    int i;
    for (i = 0; i < 90; i++)
    {
        float x = fx_rand(i) * UI_WIDTH, speed = 70 + fx_rand(i + 500) * 160, len = 60 + fx_rand(i + 900) * 140;
        float y = fmodf(t * speed + fx_rand(i + 1300) * 1000.0f, 1000.0f) - 200, k = fx_rand(i + 2100);
        int a = 40 + (int)(fx_rand(i + 1700) * 90);
        UiRoundRect(x - 1, y - len, 2, len, 1, fx_pal(k, 0), fx_pal(k, a));
        UiCircle(x, y, 2.5f + fx_rand(i + 2500) * 2.0f, fx_palw(k, 0.15f, a + 80));
        UiCircle(x, y, 7, fx_pal(k, a / 3));
    }
}

// Image 3: a vortex - dotted rings turning, each its own way and speed.
static void fx_vortex(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.52f;
    int ring, i;
    for (ring = 0; ring < 9; ring++)
    {
        float r = 40.0f + ring * 46, spin = (ring & 1 ? -1.0f : 1.0f) * (0.6f - ring * 0.04f), squash = 0.55f + ring * 0.02f;
        int dots = 24 + ring * 6;
        for (i = 0; i < dots; i++)
        {
            float a = i * 6.2831853f / dots + t * spin, fade = 0.5f + 0.5f * sinf(a * 2 + ring);
            UiCircle(cx + cosf(a) * r * 1.35f, cy + sinf(a) * r * squash, 1.6f + fade * 1.4f, fx_pal((float)ring / 8, (int)(40 + 110 * fade)));
        }
    }
    UiShadow(cx - 60, cy - 40, 120, 80, 40, 40, fx_pal(0.6f, 60));
}

// Image 4: rings of light spreading from the middle and fading out.
static void fx_ripples(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f;
    int i;
    for (i = 0; i < 12; i++)
    {
        float r = fmodf(t * 55 + i * 55, 660), fade = 1.0f - r / 660;
        if (r < 4) continue;
        UiRoundRing(cx - r, cy - r, r * 2, r * 2, r, 2.5f, fx_pal(r / 660, (int)(150 * fade)));
        UiRoundRing(cx - r - 4, cy - r - 4, r * 2 + 8, r * 2 + 8, r + 4, 5.0f, fx_pal(r / 660, (int)(40 * fade)));
    }
    UiShadow(cx - 50, cy - 50, 100, 100, 50, 40, fx_pal(0, 90));
}

// Image 5: silk - bundles of fine lines waving together.
static void fx_silk(float t)
{
    float x;
    int band, line;
    for (band = 0; band < 3; band++)
    {
        int rgb[3];
        float k = 6.2831853f / (700 + band * 160);
        fx_rgb(band * 0.5f, 0, rgb);
        for (line = 0; line < 10; line++)
        {
            // One smooth ribbon each (UiPolyline), every 8 pixels: 16-pixel pieces showed their corners and joints.
            float ph = line * 0.09f, pts[2 * ((int)(UI_WIDTH / WAVE_SLICE) + 3)];
            int a = 26 + (line == 0 || line == 9 ? 50 : 0), np = 0;
            for (x = 0; x <= UI_WIDTH + WAVE_SLICE && np < (int)(sizeof(pts) / sizeof(pts[0])) / 2; x += WAVE_SLICE)
            {
                pts[2 * np] = x;
                pts[2 * np + 1] = 380 + band * 25 + 70 * sinf(x * k + t * (0.5f + band * 0.15f) + ph * 3 + band) +
                                  28 * sinf(x * k * 2.1f - t * 0.7f + ph * 5) + line * 4.0f;
                np++;
            }
            UiPolyline(pts, np, 1.6f, 1.2f, fx_col(a, rgb[0], rgb[1], rgb[2]));
        }
    }
}

// Image 6: embers rising from the bottom right, flickering as they go.
static void fx_embers(float t)
{
    int i;
    UiShadow(900, 520, 420, 260, 130, 120, fx_pal(0, 60));
    for (i = 0; i < 120; i++)
    {
        float life = fmodf(t * (0.12f + fx_rand(i) * 0.18f) + fx_rand(i + 300), 1.0f);
        float x = 1280 - fx_rand(i + 600) * 700 * life - 60 * life + 30 * sinf(t * 2 + i);
        float y = 760 - life * (300 + fx_rand(i + 900) * 400);
        float flick = 0.6f + 0.4f * sinf(t * 12 + i * 2.3f);
        int a = (int)(220 * (1.0f - life) * flick);
        UiCircle(x, y, 1.2f + fx_rand(i + 1200) * 2.0f, fx_pal(0.5f + fx_rand(i + 1500) * 0.5f, a));
    }
}

// Image 7: a retro tunnel - a glowing grid floor and ceiling rushing towards you.
static void fx_grid(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f, scroll = fmodf(t * 0.8f, 1.0f);
    int i, k, side;
    for (side = -1; side <= 1; side += 2)
    {
        for (i = 0; i < 14; i++)
        {
            // Lines across, closer together towards the far end, moving out.
            float z = 1.0f / (i + 1 - scroll), y = cy + side * (40 + 320 * z * 0.9f), zz = z > 1 ? 1 : z;
            if (z > 1.2f) continue;
            UiRect(0, y, UI_WIDTH, 1.5f, fx_pal(zz, (int)(40 + 120 * zz)));
        }
        for (i = -10; i <= 10; i++)
        {
            // Lines along, from the vanishing point out to the edge (dotted).
            float ex = cx + i * 140, ey = cy + side * 380;
            for (k = 1; k <= 24; k++)
            {
                float f = k / 24.0f;
                UiCircle(cx + (ex - cx) * f * f, cy + side * 40 + (ey - cy - side * 40) * f, 1.1f, fx_pal(f, (int)(30 + 120 * f)));
            }
        }
    }
    UiShadow(cx - 90, cy - 40, 180, 80, 20, 40, fx_pal(1, 110));
}

// Image 8: light falling through water, rays swaying over a shimmering floor.
static void fx_underwater(float t)
{
    int i;
    for (i = 0; i < 9; i++)
    {
        float x = 300 + i * 85 + 40 * sinf(t * 0.4f + i * 1.3f), w = 30 + 40 * fx_rand(i + 40), k = i / 8.0f * 0.5f;
        int a = (int)(26 + 20 * sinf(t * 0.9f + i * 2.1f));
        UiRoundRect(x - w * 0.5f, 120, w, 480, w * 0.5f, fx_pal(k, a), fx_pal(k, 0));
    }
    for (i = 0; i < 46; i++)
    {
        float x = 220 + fx_rand(i) * 840 + 25 * sinf(t * 0.8f + i), y = 560 + fx_rand(i + 200) * 120 + 8 * sinf(t * 1.1f + i * 0.7f);
        float r = 14 + fx_rand(i + 400) * 26;
        UiRoundRing(x - r, y - r * 0.4f, r * 2, r * 0.8f, r * 0.4f, 1.5f, fx_pal(0.5f + fx_rand(i + 600) * 0.5f, (int)(60 + 40 * sinf(t * 2 + i))));
    }
}

// Image 9: an electric ring, lightning crackling out to both sides.
static void fx_energy(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f;
    int bolt, seg, k, step = (int)(t * 12);   // the bolts take a new shape 12 times a second
    float r = 110 + 4 * sinf(t * 9);
    UiShadow(cx - r - 40, cy - r - 40, (r + 40) * 2, (r + 40) * 2, r + 40, 60, fx_pal(0.5f, 50));
    for (bolt = 0; bolt < 4; bolt++)
    {
        float dir = bolt < 2 ? -1.0f : 1.0f, x0 = cx + dir * r, y0 = cy + (bolt & 1 ? -14.0f : 14.0f);
        for (seg = 0; seg < 12; seg++)
        {
            float x1 = x0 + dir * (50 + fx_rand(step * 97 + bolt * 13 + seg) * 20);
            float y1 = cy + (fx_rand(step * 131 + bolt * 7 + seg) - 0.5f) * 90 * (seg / 12.0f + 0.3f);
            for (k = 0; k < 8; k++)
            {
                float f = k / 8.0f;
                UiCircle(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, 1.6f, fx_pal(0, 170));
            }
            x0 = x1;
            y0 = y1;
        }
    }
    UiRoundRing(cx - r, cy - r, r * 2, r * 2, r, 6.0f, fx_palw(0.5f, 0.7f, 220));
    UiRoundRing(cx - r - 6, cy - r - 6, r * 2 + 12, r * 2 + 12, r + 6, 3.0f, fx_pal(1, 120));
}

// Image 10: an aurora - wide magenta and blue bands rolling across.
static void fx_aurora(float t)
{
    static const float base[4] = { 300, 380, 440, 520 }, amp[4] = { 50, 60, 45, 55 }, len[4] = { 900, 1100, 800, 1300 };
    static const float speed[4] = { 0.22f, -0.18f, 0.28f, -0.15f }, thick[4] = { 170, 150, 190, 160 }, k[4] = { 0, 0.5f, 0.25f, 1 };
    static const int alpha[4] = { 30, 28, 26, 24 };
    fx_ribbons(4, base, amp, len, speed, thick, alpha, k, t);
}

// Image 11: dark liquid, its swells catching the light.
static void fx_liquid(float t)
{
    float x;
    int row;
    for (row = 0; row < 10; row++)
    {
        // Each swell one smooth ribbon (UiPolyline) every 8 pixels, then its glints on top.
        float k = 6.2831853f / (380 + row * 30), y0 = 330.0f + row * 38, pts[2 * ((int)(UI_WIDTH / WAVE_SLICE) + 3)];
        int np = 0, i;
        for (x = 0; x <= UI_WIDTH + WAVE_SLICE && np < (int)(sizeof(pts) / sizeof(pts[0])) / 2; x += WAVE_SLICE)
        {
            pts[2 * np] = x;
            pts[2 * np + 1] = y0 + 22 * sinf(x * k + t * (0.6f + row * 0.05f) + row) + 9 * sinf(x * k * 2.7f - t * 0.9f);
            np++;
        }
        UiPolyline(pts, np, 2.0f, 1.5f, fx_pal(0, 30 + row * 4));
        for (i = 0; i < np; i += 3)   // the glints where they were (every 12 pixels or so)
        {
            float gx = pts[2 * i], slope = cosf(gx * k + t * (0.6f + row * 0.05f) + row), shine = slope > 0.85f ? (slope - 0.85f) / 0.15f : 0.0f;
            if (shine > 0) UiCircle(gx, pts[2 * i + 1], 2 + shine * 2, fx_pal(0.5f + 0.5f * fx_rand(row * 200 + (int)gx), (int)(200 * shine)));
        }
    }
}

// Image 12: a wave of glittering particles drifting across.
static void fx_particles(float t)
{
    int i;
    for (i = 0; i < 320; i++)
    {
        float x = fmodf(fx_rand(i) * 1400 + t * (12 + fx_rand(i + 50) * 18), 1400) - 60;
        float y = 470 + 60 * sinf(x * 0.006f + t * 0.5f) + 30 * sinf(x * 0.013f - t * 0.3f) + (fx_rand(i + 100) - 0.5f) * 150 * fx_rand(i + 150);
        float tw = 0.5f + 0.5f * sinf(t * 3 + i * 1.7f);
        UiCircle(x, y, 1.0f + fx_rand(i + 250) * 2.2f, fx_pal(fx_rand(i + 200), (int)(60 + 150 * tw)));
    }
}

// Image 13: smoke - soft clouds swirling slowly, one colour at each side and one between.
static void fx_smoke(float t)
{
    int i;
    for (i = 0; i < 27; i++)
    {
        int part = i % 3;
        float x = 300 + part * 340 + 200 * sinf(t * 0.13f + i * 1.1f) + (fx_rand(i) - 0.5f) * 200;
        float y = 360 + 150 * sinf(t * 0.17f + i * 0.7f) + (fx_rand(i + 50) - 0.5f) * 160;
        float r = 70 + 60 * (0.5f + 0.5f * sinf(t * 0.3f + i));
        UiShadow(x - r, y - r, r * 2, r * 2, r, r, fx_pal(part * 0.5f, part == 1 ? 24 : 34));
    }
}

// PS3: the XMB wave - a ribbon of fine lines twisting across the screen over
// a soft sheet of light, with a few specks of dust drifting by.
#define XMB_STEP 10
#define XMB_PTS (UI_WIDTH / XMB_STEP + 2)
static void fx_xmb(float t)
{
    const int lines = 16;
    float mid[XMB_PTS], spread[XMB_PTS];
    int i, j;
    for (j = 0; j < XMB_PTS; j++)
    {
        float x = (float)(j * XMB_STEP);
        mid[j] = 380 + 70 * sinf(x * 0.0042f + t * 0.35f) + 30 * sinf(x * 0.0091f - t * 0.22f);
        spread[j] = 90 * sinf(x * 0.0031f - t * 0.27f + 1.3f);   // the lines cross where it is 0: the ribbon's twist
    }
    for (j = 1; j < XMB_PTS; j++)
    {
        // The sheet between the ribbon's two edges, then the glow under it.
        float x0 = (float)((j - 1) * XMB_STEP), x1 = (float)(j * XMB_STEP);
        float h0 = fabsf(spread[j - 1]) * 0.5f + 6, h1 = fabsf(spread[j]) * 0.5f + 6;
        UiStrip(x0, mid[j - 1] - h0, mid[j - 1] + h0, x1, mid[j] - h1, mid[j] + h1, fx_pal(0.4f, 16), fx_pal(0.4f, 16));
        UiStrip(x0, mid[j - 1], mid[j - 1] + 180, x1, mid[j], mid[j] + 180, fx_pal(0.2f, 14), fx_pal(0.2f, 0));
    }
    for (i = 0; i < lines; i++)
    {
        // Each line one smooth ribbon (UiPolyline): drawn piece by piece, its joints showed as a saw-tooth.
        float f = (float)i / (lines - 1), pts[XMB_PTS * 2];
        int edge = i == 0 || i == lines - 1;
        for (j = 0; j < XMB_PTS; j++)
        {
            float x = (float)(j * XMB_STEP);
            pts[2 * j] = x;
            pts[2 * j + 1] = mid[j] + spread[j] * (f - 0.5f) + 6 * sinf(x * 0.012f + t * 0.6f + i * 0.4f);
        }
        UiPolyline(pts, XMB_PTS, edge ? 1.6f : 1.1f, 1.2f, fx_palw(f, 0.25f, edge ? 110 : 34));
    }
    for (i = 0; i < 60; i++)
    {
        float px = fmodf(fx_rand(i) * 1360 + t * (6 + fx_rand(i + 70) * 10), 1360) - 40;
        float py = 380 + 70 * sinf(px * 0.0042f + t * 0.35f) + (fx_rand(i + 140) - 0.5f) * 260;
        float tw = 0.5f + 0.5f * sinf(t * 1.6f + i * 2.1f);
        UiCircle(px, py, 1.0f + fx_rand(i + 210) * 1.6f, fx_palw(fx_rand(i + 280), 0.6f, (int)(30 + 150 * tw)));
    }
}

// PS4: wide bands of light sweeping slowly on the slant, each with a bright edge,
// and fine particles rising through them.
static void fx_ps4_flow(float t)
{
    float x;
    int b, i;
    for (b = 0; b < 3; b++)
    {
        // The band, then its bright edge as one smooth ribbon (UiPolyline: piece by piece, a saw-tooth).
        int rgb[3], hi[3], np = 0;
        float px = 0, py = 0, pts[2 * ((int)(UI_WIDTH / WAVE_SLICE) + 3)];
        fx_rgb(b * 0.5f, 0, rgb);
        fx_rgb(b * 0.5f, 0.5f, hi);
        for (x = 0; x <= UI_WIDTH + WAVE_SLICE; x += WAVE_SLICE)
        {
            float y = 620 - x * (0.22f + b * 0.06f) + b * 90 + 55 * sinf(x * (0.0035f + b * 0.0011f) + t * (0.25f + b * 0.07f) + b * 2.1f);
            if (x > 0) UiStrip(px, py, py + 240, x, y, y + 240, fx_col(22, rgb[0], rgb[1], rgb[2]), fx_col(0, rgb[0], rgb[1], rgb[2]));
            if (np < (int)(sizeof(pts) / sizeof(pts[0])) / 2) { pts[2 * np] = x; pts[2 * np + 1] = y; np++; }
            px = x; py = y;
        }
        UiPolyline(pts, np, 1.5f, 1.5f, fx_col(90, hi[0], hi[1], hi[2]));
    }
    for (i = 0; i < 70; i++)
    {
        float life = fmodf(t * (0.04f + fx_rand(i) * 0.05f) + fx_rand(i + 90), 1.0f);
        float px = fx_rand(i + 180) * UI_WIDTH + 40 * sinf(t * 0.5f + i), py = 740 - life * 800;
        int a = (int)(160 * sinf(life * 3.14159f));
        UiCircle(px, py, 1.0f + fx_rand(i + 270) * 1.5f, fx_palw(fx_rand(i + 360), 0.5f, a));
    }
}

// PS4: bokeh - soft discs of light, near ones big and blurred, far ones small
// and sharp, floating up and fading in and out.
static void fx_ps4_bokeh(float t)
{
    int i;
    for (i = 0; i < 44; i++)
    {
        float depth = fx_rand(i + 11), life = fmodf(t * (0.015f + depth * 0.03f) + fx_rand(i + 33), 1.0f);
        float r = 8 + depth * depth * 70, px = fx_rand(i + 55) * (UI_WIDTH + 200) - 100 + 50 * sinf(t * 0.2f + i * 1.7f);
        float py = UI_HEIGHT + r - life * (UI_HEIGHT + 2 * r);
        float fade = sinf(life * 3.14159f), k = fx_rand(i + 77);
        if (depth > 0.5f)
        {
            // Near: a blurred disc with a faint rim.
            UiShadow(px - r, py - r, r * 2, r * 2, r, r * 0.6f, fx_pal(k, (int)(26 * fade)));
            UiRoundRing(px - r, py - r, r * 2, r * 2, r, 1.5f, fx_palw(k, 0.3f, (int)(30 * fade)));
        }
        else
        {
            UiCircle(px, py, r * 0.5f, fx_pal(k, (int)(40 * fade)));
            UiCircle(px, py, r * 0.2f, fx_palw(k, 0.5f, (int)(120 * fade)));
        }
    }
}

// Image 26: a nebula - great soft clouds drifting slowly through a field of stars.
static void fx_nebula(float t)
{
    int i;
    for (i = 0; i < 14; i++)
    {
        float x = fx_rand(i) * 1400 - 60 + 60 * sinf(t * 0.05f + i), y = 200 + fx_rand(i + 20) * 380 + 40 * sinf(t * 0.07f + i * 1.3f);
        float r = 120 + fx_rand(i + 40) * 160;
        UiShadow(x - r, y - r, r * 2, r * 2, r, r, fx_pal(fx_rand(i + 60), 22));
    }
    for (i = 0; i < 160; i++)
    {
        float x = fmodf(fx_rand(i + 100) * 1300 + t * (2 + fx_rand(i + 300) * 4), 1300) - 10, y = fx_rand(i + 200) * UI_HEIGHT;
        float tw = 0.5f + 0.5f * sinf(t * (1 + fx_rand(i + 400) * 2) + i);
        UiCircle(x, y, 0.6f + fx_rand(i + 500) * 1.2f, fx_palw(fx_rand(i + 600), 0.8f, (int)(40 + 160 * tw)));
    }
    for (i = 0; i < 8; i++)
    {
        float x = fx_rand(i + 700) * UI_WIDTH, y = fx_rand(i + 800) * UI_HEIGHT, tw = 0.6f + 0.4f * sinf(t * 1.3f + i * 2);
        UiShadow(x - 10, y - 10, 20, 20, 10, 14, fx_palw(fx_rand(i + 900), 0.5f, (int)(80 * tw)));
        UiCircle(x, y, 1.8f, fx_palw(0.5f, 0.9f, (int)(230 * tw)));
    }
}

// Image 27: fire dust - a red haze, sparks streaking up through it.
static void fx_fire_dust(float t)
{
    int i;
    for (i = 0; i < 12; i++)
    {
        float x = 100 + fx_rand(i) * 1100 + 80 * sinf(t * 0.11f + i * 1.7f), y = 380 + fx_rand(i + 30) * 260 + 50 * sinf(t * 0.09f + i);
        float r = 140 + fx_rand(i + 60) * 140;
        UiShadow(x - r, y - r, r * 2, r * 2, r, r, fx_pal(0.15f, 26));
    }
    for (i = 0; i < 200; i++)
    {
        float life = fmodf(t * (0.05f + fx_rand(i) * 0.08f) + fx_rand(i + 100), 1.0f);
        float x = fmodf(fx_rand(i + 200) * 1400 + life * 260, 1400) - 60, y = 760 - life * (500 + fx_rand(i + 300) * 400);
        float flick = 0.6f + 0.4f * sinf(t * 10 + i * 1.9f);
        int a = (int)(230 * sinf(life * 3.14159f) * flick);
        if (fx_rand(i + 400) > 0.8f) UiLine(x, y, x - 7, y + 9, 1.4f, fx_pal(0.6f + fx_rand(i + 500) * 0.4f, a));   // a streak
        else UiCircle(x, y, 0.8f + fx_rand(i + 600) * 1.6f, fx_pal(0.6f + fx_rand(i + 500) * 0.4f, a));
    }
}

// Image 28: lava - dark rock split by cracks that glow and fade like embers.
static void fx_lava(float t)
{
    int i, s;
    for (i = 0; i < 30; i++)
    {
        float x = fx_rand(i * 7) * UI_WIDTH, y = fx_rand(i * 7 + 1) * UI_HEIGHT, dir = fx_rand(i * 7 + 2) * 6.2831853f;
        float glow = 0.55f + 0.45f * sinf(t * (0.5f + fx_rand(i * 7 + 3) * 0.6f) + i * 1.3f);
        for (s = 0; s < 8; s++)
        {
            // A random walk: each piece of the crack bends a little from the last.
            float len = 30 + fx_rand(i * 50 + s) * 50, x1, y1;
            dir += (fx_rand(i * 50 + s + 25) - 0.5f) * 1.6f;
            x1 = x + cosf(dir) * len;
            y1 = y + sinf(dir) * len;
            UiLine(x, y, x1, y1, 7.0f, fx_pal(0.3f, (int)(40 * glow)));
            UiLine(x, y, x1, y1, 2.2f - s * 0.15f, fx_pal(0.6f + 0.4f * glow, (int)(200 * glow)));
            x = x1;
            y = y1;
        }
    }
}

// Image 29: a prism - a twisting band of glass, its fine lines splitting the light into colours.
static void fx_prism(float t)
{
#define PRISM_STEP 8
#define PRISM_PTS (UI_WIDTH / PRISM_STEP + 2)
    const int lines = 22;
    float mid[PRISM_PTS], spread[PRISM_PTS], xy[PRISM_PTS * 2];
    int i, j;
    for (j = 0; j < PRISM_PTS; j++)
    {
        float u = (float)(j * PRISM_STEP) / UI_WIDTH;
        mid[j] = 520 - 260 * u + 90 * sinf(u * 5.0f + t * 0.4f);
        spread[j] = 170 * sinf(u * 3.4f - t * 0.3f + 0.8f);
    }
    for (i = 0; i < lines; i++)
    {
        // One smooth ribbon each (UiPolyline), every 8 pixels: 12-pixel pieces showed their joints as a saw-tooth.
        float f = (float)i / (lines - 1);
        int edge = i == 0 || i == lines - 1;
        for (j = 0; j < PRISM_PTS; j++)
        {
            xy[2 * j] = (float)(j * PRISM_STEP);
            xy[2 * j + 1] = mid[j] + spread[j] * (f - 0.5f);
        }
        UiPolyline(xy, PRISM_PTS, edge ? 2.0f : 1.2f, 1.2f, edge ? fx_palw(f, 0.7f, 150) : fx_palw(f, 0.15f, 46));
    }
    // The shine running along it.
    {
        float u = fmodf(t * 0.08f, 1.4f) - 0.2f, x = u * UI_WIDTH, y = 520 - 260 * u + 90 * sinf(u * 5.0f + t * 0.4f);
        UiShadow(x - 60, y - 40, 120, 80, 40, 40, fx_palw(0.5f, 0.9f, 60));
    }
}

// Image 30: topography - contour lines around three slowly moving hills, seen on the slant.
static void fx_topo(float t)
{
    int c, k, j;
    for (c = 0; c < 3; c++)
    {
        float cx = 260 + c * 380 + 90 * sinf(t * 0.07f + c * 2), cy = 420 + 50 * sinf(t * 0.05f + c * 1.4f);
        for (k = 0; k < 10; k++)
        {
            // Each contour a closed smooth ribbon (UiPolyline) of 96 points: 48 pieces showed corners and joints.
            float r = 24.0f + k * 26, xy[2 * 97];
            for (j = 0; j <= 96; j++)
            {
                float a = j * 6.2831853f / 96;
                float rr = r * (1 + 0.15f * sinf(3 * a + t * 0.5f + c) + 0.08f * sinf(5 * a - t * 0.3f + k * 0.4f));
                xy[2 * j] = cx + cosf(a) * rr * 1.7f;
                xy[2 * j + 1] = cy + sinf(a) * rr * 0.45f;
            }
            xy[2 * 96] = xy[0];   // exactly closed: no seam
            xy[2 * 96 + 1] = xy[1];
            UiPolyline(xy, 97, 1.3f, 1.2f, fx_pal(k & 1 ? 1.0f : 0.0f, k & 1 ? 70 : 110));
        }
        UiShadow(cx - 120, cy - 40, 240, 80, 40, 60, fx_pal(0.5f, 30));
    }
}

// One hexagon (flat top), radius r.
static void fx_hexagon(float cx, float cy, float r, float thick, D3DCOLOR c)
{
    int k;
    for (k = 0; k < 6; k++)
    {
        float a0 = k * 1.0471976f, a1 = (k + 1) * 1.0471976f;
        UiLine(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, thick, c);
    }
}

// Image 31: hexagons - a honeycomb wall, its cells lighting up in slow waves.
static void fx_hex(float t)
{
    const float r = 46, dx = r * 1.5f, dy = r * 1.7320508f;
    int col, row;
    for (col = 0; col < 20; col++)
        for (row = 0; row < 10; row++)
        {
            float cx = col * dx, cy = row * dy + (col & 1 ? dy * 0.5f : 0) - 20;
            float wave = sinf(t * 0.8f + col * 0.45f - row * 0.3f + fx_rand(col * 20 + row) * 6.0f);
            float glow = wave > 0 ? wave * wave * wave * wave : 0;
            fx_hexagon(cx, cy, r - 3, 1.2f, fx_pal(0.2f, 30));
            if (glow > 0.05f)
            {
                UiShadow(cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f, r * 0.8f, 20, fx_pal(0.5f, (int)(50 * glow)));
                fx_hexagon(cx, cy, r - 3, 2.2f, fx_palw(1.0f, 0.2f, (int)(220 * glow)));
            }
        }
}

// Image 32: a spiral - arms of dots wound round a bright centre, turning.
static void fx_spiral(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f;
    int arm, j;
    UiShadow(cx - 90, cy - 70, 180, 140, 70, 60, fx_palw(1.0f, 0.5f, 90));
    for (arm = 0; arm < 7; arm++)
        for (j = 2; j < 60; j++)
        {
            float r = 6 + j * j * 0.2f, a = arm * 6.2831853f / 7 + j * 0.085f + t * 0.45f;
            float f = (float)j / 60;
            UiCircle(cx + cosf(a) * r * 1.35f, cy + sinf(a) * r * 0.85f, 1.0f + f * 3.2f, fx_palw(1.0f - f, 0.2f, (int)(220 - 120 * f)));
        }
}

// Image 33: ink - plumes sinking through water, spreading and fading as they go.
static void fx_ink(float t)
{
    int i;
    for (i = 0; i < 40; i++)
    {
        int src = i % 4;
        float life = fmodf(t * (0.03f + fx_rand(i) * 0.02f) + fx_rand(i + 50), 1.0f);
        float sx = 220 + src * 280 + 60 * sinf(t * 0.07f + src);
        float x = sx + sinf(life * 6 + i) * 90 * life + (fx_rand(i + 100) - 0.5f) * 80 * life;
        float y = 80 + life * 520 + (fx_rand(i + 150) - 0.5f) * 40;
        float r = 18 + life * 110;
        float a = 46 * (1 - life) * (life * 6 < 1 ? life * 6 : 1);
        UiShadow(x - r, y - r, r * 2, r * 2, r, r * 0.7f, fx_pal(fx_rand(i + 200), (int)a));
        if (fx_rand(i + 250) > 0.6f) UiRoundRing(x - r * 0.6f, y - r * 0.4f, r * 1.2f, r * 0.8f, r * 0.4f, 1.2f, fx_palw(0.5f, 0.5f, (int)(a * 1.6f)));
    }
}

// Image 34: a hologram - a wireframe globe turning inside a scanner's frame, a beam on it.
static void fx_holo(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f, R = 190;
    const float rot = t * 0.4f, scan = cy + R * sinf(t * 0.9f);
    int i, j;
    for (i = 1; i < 8; i++)
    {
        // Latitudes: ellipses, the far half dimmer.
        float lat = -1.5707963f + i * 3.14159f / 8, y = cy + R * sinf(lat), rr = R * cosf(lat), px = 0, py = 0, pz = 0;
        for (j = 0; j <= 32; j++)
        {
            float a = j * 6.2831853f / 32 + rot, x = cx + rr * cosf(a), z = sinf(a), yy = y + rr * 0.18f * z;
            if (j) UiLine(px, py, x, yy, 1.2f, fx_pal(0.6f, (pz + z) > 0 ? 130 : 40));
            px = x; py = yy; pz = z;
        }
    }
    for (i = 0; i < 12; i++)
    {
        // Meridians.
        float lon = i * 3.14159f / 6 + rot, px = 0, py = 0, pz = 0;
        for (j = 0; j <= 24; j++)
        {
            float lat = -1.5707963f + j * 3.14159f / 24, x = cx + R * cosf(lat) * cosf(lon), z = cosf(lat) * sinf(lon);
            float y = cy + R * sinf(lat) + R * 0.18f * z * cosf(lat);
            if (j) UiLine(px, py, x, y, 1.1f, fx_pal(0.5f, (pz + z) > 0 ? 120 : 35));
            px = x; py = y; pz = z;
        }
    }
    // The scan line across it, the beams from the left, the frame's corners.
    UiShadow(cx - R, scan - 6, R * 2, 12, 6, 16, fx_palw(1.0f, 0.3f, 90));
    UiLine(cx - R * 1.05f, scan, cx + R * 1.05f, scan, 1.5f, fx_palw(1.0f, 0.5f, 200));
    for (i = 0; i < 3; i++)
    {
        float y0 = cy - 120 + i * 120, w = 0.6f + 0.4f * sinf(t * 1.5f + i * 2);
        UiStrip(0, y0 - 26, y0 + 26, cx - 30, cy - 1, cy + 1, fx_pal(0.4f, (int)(30 * w)), fx_pal(0.4f, (int)(30 * w)));
    }
    {
        const float bx = cx - R - 50, by = cy - R - 40, bw = (R + 50) * 2, bh = (R + 40) * 2, l = 40;
        D3DCOLOR c = fx_palw(0.8f, 0.2f, 180);
        UiLine(bx, by, bx + l, by, 2, c); UiLine(bx, by, bx, by + l, 2, c);
        UiLine(bx + bw, by, bx + bw - l, by, 2, c); UiLine(bx + bw, by, bx + bw, by + l, 2, c);
        UiLine(bx, by + bh, bx + l, by + bh, 2, c); UiLine(bx, by + bh, bx, by + bh - l, 2, c);
        UiLine(bx + bw, by + bh, bx + bw - l, by + bh, 2, c); UiLine(bx + bw, by + bh, bx + bw, by + bh - l, 2, c);
    }
}

// Image 35: a fractal - spiral arms of glowing dots, curls budding along them, turning slowly.
static void fx_fractal(float t)
{
    static const float centres[2][2] = { { 420, 330 }, { 860, 400 } };
    int c, arm, j, k;
    for (c = 0; c < 2; c++)
    {
        float cx = centres[c][0], cy = centres[c][1], spin = (c ? -0.18f : 0.2f) * t;
        UiShadow(cx - 50, cy - 50, 100, 100, 50, 50, fx_pal(0.5f, 60));
        for (arm = 0; arm < 4; arm++)
            for (j = 0; j < 70; j++)
            {
                // A logarithmic spiral: the arm opens as it goes out.
                float th = j * 0.11f, r = 10 * expf(th * 0.28f), a = arm * 1.5707963f + th + spin;
                float x = cx + cosf(a) * r, y = cy + sinf(a) * r * 0.8f, f = (float)j / 70;
                UiCircle(x, y, 1.0f + f * 1.6f, fx_palw(f, 0.15f, (int)(200 - 90 * f)));
                if (j % 10 == 9)
                    for (k = 0; k < 9; k++)
                    {
                        // A small curl, the arm's pattern again.
                        float ka = a + k * 0.7f + t * 0.6f, kr = 3 + k * 2.2f * (0.6f + f);
                        UiCircle(x + cosf(ka) * kr, y + sinf(ka) * kr, 1.0f, fx_palw(1.0f - f, 0.3f, 150 - k * 12));
                    }
            }
    }
}

// Image 36: a burst - particles streaming out of a bright centre along curving paths.
static void fx_burst(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f;
    int i;
    UiShadow(cx - 70, cy - 70, 140, 140, 70, 60, fx_palw(1.0f, 0.4f, 140));
    for (i = 0; i < 320; i++)
    {
        // Most of them along a few main rays, the rest anywhere.
        float th = fx_rand(i + 10) < 0.6f ? (int)(fx_rand(i + 20) * 9) * 0.6981317f + (fx_rand(i + 30) - 0.5f) * 0.25f : fx_rand(i + 40) * 6.2831853f;
        float bend = (fx_rand(i + 50) - 0.5f) * 0.9f, life = fmodf(t * (0.06f + fx_rand(i + 60) * 0.08f) + fx_rand(i + 70), 1.0f);
        float d = powf(life, 1.4f) * 760, a = th + bend * life;
        UiCircle(cx + cosf(a) * d, cy + sinf(a) * d * 0.85f, 0.9f + life * 1.6f, fx_palw(life, 0.3f, (int)(220 * (1 - life * 0.75f))));
    }
    UiCircle(cx, cy, 10, fx_palw(1.0f, 0.8f, 220));
}

// Image 37: hyperspace - streaks of light rushing past from the middle, longer as they come.
static void fx_warp(float t)
{
    const float cx = UI_WIDTH * 0.5f, cy = UI_HEIGHT * 0.5f;
    int i;
    for (i = 0; i < 170; i++)
    {
        float th = fx_rand(i) * 6.2831853f, life = fmodf(t * (0.25f + fx_rand(i + 100) * 0.3f) + fx_rand(i + 200), 1.0f);
        float r0 = 20 + life * life * 900, len = 20 + life * life * 320, c = cosf(th) * 1.25f, s = sinf(th);
        UiLine(cx + c * r0, cy + s * r0, cx + c * (r0 + len), cy + s * (r0 + len), 0.8f + life * 2.6f, fx_pal(fx_rand(i + 300), (int)(220 * life)));
    }
}

// Each effect's own three colours (the gradient fx_pal walks), by BGFX_*.
static const unsigned char s_fxDefault[BGFX_COUNT][3][3] = {
    { { 110, 190, 255 }, { 110, 190, 255 }, { 110, 190, 255 } },   // off (unused)
    { { 110, 190, 255 }, { 110, 190, 255 }, { 110, 190, 255 } },   // waves
    { { 60, 230, 230 }, { 90, 120, 255 }, { 240, 80, 200 } },      // pulse
    { { 80, 185, 255 }, { 70, 215, 235 }, { 60, 240, 210 } },      // rain
    { { 60, 160, 255 }, { 125, 120, 255 }, { 190, 80, 255 } },     // vortex
    { { 80, 170, 255 }, { 70, 145, 255 }, { 60, 120, 255 } },      // ripples
    { { 60, 220, 200 }, { 150, 90, 230 }, { 80, 200, 255 } },      // silk
    { { 255, 110, 30 }, { 255, 150, 60 }, { 255, 230, 60 } },      // embers
    { { 120, 60, 220 }, { 190, 50, 170 }, { 255, 40, 120 } },      // grid
    { { 60, 220, 210 }, { 70, 225, 215 }, { 80, 230, 220 } },      // underwater
    { { 150, 230, 255 }, { 200, 80, 255 }, { 230, 90, 220 } },     // energy
    { { 220, 70, 200 }, { 60, 150, 255 }, { 70, 200, 255 } },      // aurora
    { { 140, 150, 200 }, { 255, 230, 200 }, { 200, 120, 255 } },   // liquid
    { { 60, 210, 255 }, { 130, 150, 255 }, { 200, 90, 255 } },     // particles
    { { 230, 70, 200 }, { 140, 80, 230 }, { 60, 220, 230 } },      // smoke
    { { 70, 130, 255 }, { 140, 190, 255 }, { 225, 235, 255 } },    // XMB (the blue of its default theme to white)
    { { 30, 90, 220 }, { 60, 150, 255 }, { 140, 210, 255 } },      // PS4 flow
    { { 70, 140, 255 }, { 120, 200, 255 }, { 180, 120, 255 } },    // PS4 bokeh
    { { 60, 80, 220 }, { 150, 70, 220 }, { 240, 120, 220 } },  // nebula
    { { 150, 25, 10 }, { 255, 90, 20 }, { 255, 190, 60 } },    // fire dust
    { { 130, 20, 0 }, { 255, 80, 10 }, { 255, 200, 80 } },     // lava
    { { 255, 90, 90 }, { 90, 255, 170 }, { 100, 120, 255 } },  // prism
    { { 70, 220, 255 }, { 120, 120, 255 }, { 200, 80, 255 } }, // topography
    { { 30, 120, 140 }, { 60, 220, 240 }, { 180, 255, 255 } }, // hexagons
    { { 170, 180, 220 }, { 220, 225, 245 }, { 255, 255, 255 } }, // spiral
    { { 190, 195, 210 }, { 225, 228, 238 }, { 255, 255, 255 } }, // ink
    { { 40, 160, 170 }, { 90, 230, 230 }, { 200, 255, 255 } }, // hologram
    { { 60, 90, 255 }, { 90, 200, 255 }, { 200, 80, 255 } },   // fractal
    { { 40, 140, 200 }, { 120, 220, 255 }, { 220, 250, 255 } }, // burst
    { { 255, 60, 170 }, { 110, 80, 255 }, { 60, 220, 255 } },  // hyperspace
};

// The effect chosen (s_bgEffect: BGFX_*), right after UiBackdrop.
static void browser_effect(void)
{
    float now = UiTime(), dt = s_fxLast < 0 ? 0 : now - s_fxLast, t;
    int i, n = 0;
    if (dt < 0 || dt > 0.1f) dt = 0.016f;   // a pause (a game, a long load) does not jump the effect on
    s_fxLast = now;
    s_fxClock += dt * s_fxSpeed / 100.0f;
    t = s_fxClock;
    if (s_bgEffect <= BGFX_OFF || s_bgEffect >= BGFX_COUNT) return;
    // The colours: the chosen ones (those of the three set), or the effect's own when none is.
    for (i = 0; i < 3; i++)
        if (s_fxColor[i] > 0 && s_fxColor[i] <= FX_PALETTE_COUNT)
        {
            s_fxRgb[n][0] = s_fxPalette[s_fxColor[i]][0];
            s_fxRgb[n][1] = s_fxPalette[s_fxColor[i]][1];
            s_fxRgb[n][2] = s_fxPalette[s_fxColor[i]][2];
            n++;
        }
    if (!n)
        for (n = 0; n < 3; n++)
            for (i = 0; i < 3; i++) s_fxRgb[n][i] = s_fxDefault[s_bgEffect][n][i];
    s_fxRgbCount = n;
    switch (s_bgEffect)
    {
    case BGFX_WAVES: fx_waves(t); break;
    case BGFX_PULSE: fx_pulse(t); break;
    case BGFX_RAIN: fx_rain(t); break;
    case BGFX_VORTEX: fx_vortex(t); break;
    case BGFX_RIPPLES: fx_ripples(t); break;
    case BGFX_SILK: fx_silk(t); break;
    case BGFX_EMBERS: fx_embers(t); break;
    case BGFX_GRID: fx_grid(t); break;
    case BGFX_UNDERWATER: fx_underwater(t); break;
    case BGFX_ENERGY: fx_energy(t); break;
    case BGFX_AURORA: fx_aurora(t); break;
    case BGFX_LIQUID: fx_liquid(t); break;
    case BGFX_PARTICLES: fx_particles(t); break;
    case BGFX_SMOKE: fx_smoke(t); break;
    case BGFX_XMB: fx_xmb(t); break;
    case BGFX_PS4_FLOW: fx_ps4_flow(t); break;
    case BGFX_PS4_BOKEH: fx_ps4_bokeh(t); break;
    case BGFX_NEBULA: fx_nebula(t); break;
    case BGFX_FIRE_DUST: fx_fire_dust(t); break;
    case BGFX_LAVA: fx_lava(t); break;
    case BGFX_PRISM: fx_prism(t); break;
    case BGFX_TOPO: fx_topo(t); break;
    case BGFX_HEX: fx_hex(t); break;
    case BGFX_SPIRAL: fx_spiral(t); break;
    case BGFX_INK: fx_ink(t); break;
    case BGFX_HOLO: fx_holo(t); break;
    case BGFX_FRACTAL: fx_fractal(t); break;
    case BGFX_BURST: fx_burst(t); break;
    case BGFX_WARP: fx_warp(t); break;
    }
}

// ---- Screen saver (Interface > Screen saver): after Config::saverTime
// minutes without a button in the ROM list (or its Preview), over black:
// the background effect, the wallpapers slowly zooming in, the logo
// drifting about, or a clock. Any button, trigger or stick ends it.
static int saver_any_input(void)
{
    XINPUT_STATE st;
    int i;
    for (i = 0; i < 4; i++)
        if (XInputGetState(i, &st) == ERROR_SUCCESS)
        {
            const XINPUT_GAMEPAD *g = &st.Gamepad;
            if (g->wButtons || g->bLeftTrigger > 60 || g->bRightTrigger > 60 || abs(g->sThumbLX) > 12000 || abs(g->sThumbLY) > 12000 ||
                abs(g->sThumbRX) > 12000 || abs(g->sThumbRY) > 12000)
                return 1;
        }
    return 0;
}

// 0..1..0 as u goes 0..1..2 (bouncing between two walls).
static float saver_tri(float u) { return 1.0f - fabsf(fmodf(u, 2.0f) - 1.0f); }

static void saver_effect(float opacity)
{
    int keepFx = s_bgEffect, keepOp = s_fxOpacity;
    if (s_bgEffect == BGFX_OFF) s_bgEffect = BGFX_PS4_BOKEH;   // no effect chosen: one anyway
    s_fxOpacity = (int)(s_fxOpacity * opacity);
    browser_effect();
    s_bgEffect = keepFx;
    s_fxOpacity = keepOp;
}

static void saver_draw(int mode, float t)
{
    switch (mode)
    {
    case SAVER_WALL:
        if (UiWallpaperDraw(1)) break;
        saver_effect(1.5f);   // no wallpaper folder: the effect instead
        break;
    case SAVER_LOGO:
    {
        const float size = 40, h = size * 1.9f;
        float w = UiBrand(-4000, -4000, size);   // measured off screen
        float x = 40 + saver_tri(t * 60.0f / (UI_WIDTH - 80 - w)) * (UI_WIDTH - 80 - w);
        float cy = 40 + h * 0.5f + saver_tri(t * 45.0f / (UI_HEIGHT - 80 - h)) * (UI_HEIGHT - 80 - h);
        saver_effect(0.4f);
        UiShadow(x - 30, cy - h * 0.5f - 20, w + 60, h + 40, 40, 70, SEL_LO(70));
        UiBrand(x, cy, size);
        break;
    }
    case SAVER_CLOCK:
    {
        SYSTEMTIME st;
        char tm[16], dt[32];
        float ox, oy;
        GetLocalTime(&st);
        sprintf(tm, "%02d:%02d", st.wHour, st.wMinute);
        sprintf(dt, "%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
        // Somewhere else every minute, so nothing stays in one place on the screen.
        ox = (fx_rand(st.wHour * 60 + st.wMinute) - 0.5f) * 520;
        oy = (fx_rand(st.wHour * 60 + st.wMinute + 7000) - 0.5f) * 300;
        saver_effect(0.35f);
        UiTextShadowed(UI_BOLD, 130, UI_WIDTH * 0.5f + ox, 320 + oy, UI_COL_TEXT, tm, UI_CENTER);
        UiText(UI_REGULAR, 30, UI_WIDTH * 0.5f + ox, 420 + oy, UI_COL_TEXT2, dt, UI_CENTER);
        UiRoundRect(UI_WIDTH * 0.5f + ox - 150, 462 + oy, 300, 4, 2, D3DCOLOR_ARGB(40, 255, 255, 255), D3DCOLOR_ARGB(40, 255, 255, 255));
        UiRoundRect(UI_WIDTH * 0.5f + ox - 150, 462 + oy, 4 + 296 * (st.wSecond / 59.0f), 4, 2, SEL_HI(220), SEL_LO(220));
        break;
    }
    default:
        saver_effect(1.5f);
        break;
    }
}

static void saver_run(IDirect3DDevice9 *dev, const Config *c)
{
    const float start = UiTime();
    int mode = c->saverMode > SAVER_OFF && c->saverMode < SAVER_COUNT ? c->saverMode : SAVER_EFFECT, armed = 0;
    H64_INFO("[menu] screen saver %d", mode);
    xb_audio_ui_sound(XB_SFX_SAVER);
    for (;;)
    {
        float t = UiTime() - start;
        // A button held when it starts (the Preview's A) has to be let go first.
        if (!saver_any_input()) armed = 1;
        else if (armed) break;
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        UiBegin(dev);
        saver_draw(mode, t);
        if (t < 1.0f) UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(255 * (1.0f - t)), 0, 0, 0));   // fades in
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
    xb_audio_ui_sound(XB_SFX_OPEN);
    H64_INFO("[menu] screen saver ended");
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
    browser_effect();
    if (!roms.empty()) game_backdrop(roms[sel].file, 0);   // Interface > Background images
    // The logo as large as the room over the list allows, centred on it, just above its edge (the
    // tab - LT/RT - shows in a note as it changes; the version is on the About page).
    UiBrand(listX + (listW - UiBrandWidth(BRAND_SIZE)) * 0.5f, listY - 6 - BRAND_SIZE * 1.9f * 0.5f, BRAND_SIZE);
    UiPanel(listX, listY, listW, listH, 20);
    if (!n && s_romsTotal)
    {
        // A tab with no game in it (no favourites yet, an empty category).
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 24, listX + listW * 0.5f, cy - 20, UI_COL_TEXT, Tr(T_CAT_EMPTY), UI_CENTER);
        UiText(UI_REGULAR, 19, listX + listW * 0.5f, cy + 20, UI_COL_TEXT2, Tr(s_tab == 1 ? T_FAV_HINT : T_CAT_HINT), UI_CENTER);
    }
    else if (!n)
    {
        float cy = listY + listH * 0.5f;
        UiText(UI_BOLD, 30, listX + listW * 0.5f, cy - 50, UI_COL_TEXT, Tr(T_NO_ROM), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy, UI_COL_TEXT2, Tr(T_COPY_ROMS), UI_CENTER);
        UiText(UI_REGULAR, 21, listX + listW * 0.5f, cy + 32, UI_COL_TEXT2, Tr(T_INTO_ROMS), UI_CENTER);
    }
    else
    {
        float sy = anim_step(&selAnim, listY + pad + (sel - top) * rowH, 0), textW = listW - 180;
        selection_bar(listX + 10, sy + 1, listW - (n > rows ? 36 : 20), rowH - 2);
        UiRoundRect(listX + 12, sy + 6, 4, rowH - 12, 2, SEL_HI(255), SEL_LO(255));   // an accent down its left edge
        for (i = 0; i < rows && top + i < n; i++)
        {
            // Each row: the game's small cover (its box, as the list's card shows it), the
            // favourite star over its corner, the name, the year at the right.
            const float cy = listY + pad + i * rowH + rowH * 0.5f, th = rowH - 10, tw = th * 400 / 232, thx = listX + 26;
            const ArtView *rv = view_find(roms[top + i].file.c_str());
            int s = top + i == sel;
            UiRoundRect(thx, cy - th * 0.5f, tw, th, 4, D3DCOLOR_ARGB(90, 30, 32, 42), D3DCOLOR_ARGB(90, 24, 26, 34));
            if (rv && rv->cover) cover_draw(rv->cover, thx, cy - th * 0.5f, tw, th, 4, D3DCOLOR_ARGB(255, s ? 255 : 210, s ? 255 : 210, s ? 255 : 210), 0);
            if (fav_is(roms[top + i].file)) fav_star(thx + tw - 2, cy - th * 0.5f + 3, 10, 1.0f);
            UiTextFit(s ? UI_BOLD : UI_REGULAR, 21, textW, roms[top + i].shown.c_str(), buf, sizeof(buf));
            UiText(s ? UI_BOLD : UI_REGULAR, 21, thx + tw + 16, cy, s ? UI_COL_TEXT : UI_COL_TEXT2, buf, UI_LEFT);
            if (rv && rv->hasInfo && rv->info.year[0])
                UiText(UI_REGULAR, 16, listX + listW - (n > rows ? 32 : 20), cy, s ? UI_COL_TEXT2 : UI_COL_TEXT3, rv->info.year, UI_RIGHT);
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
        // "Download all" progress in the top right corner, over the card (the logo is over the
        // list, the prompts below take the whole width).
        const float bw = 120, right = UI_WIDTH - UI_SAFE_X, cy = CARD_Y - 20;
        float frac = st->total ? (float)st->done / st->total : 0;
        _snprintf(buf, sizeof(buf) - 1, Tr(T_ART_PROGRESS), st->done, st->total);   // "0 / N" while it connects: short enough for this spot
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
// BD_MOSAIC: 7 x 4, 28 covers 150 wide - the last row ends at 610.
static const GridLayout s_gridMosaic = { 7, 4, 70, 110, 150, 87, 14, 14, 8 };
#define GRID_LABEL_GAP 6.0f
#define GRID_LABEL_H 24.0f
#define GRID_SLIDE 120.0f   // how far a page slides as it changes (fading as it goes)

static const GridLayout *grid_layout(int design) { return design == BD_BIG ? &s_gridBig : design == BD_MOSAIC ? &s_gridMosaic : &s_gridCovers; }
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
    browser_effect();
    if (!roms.empty()) game_backdrop(roms[sel].file, 0);   // Interface > Background images
    UiBrand(UI_SAFE_X, 72, BRAND_SIZE);   // the logo alone (the version is on the About page)
    browser_tabs(UI_WIDTH * 0.5f, 72, 1, 520);
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
        // A little slower than the rest (6, was 9): the covers of the page coming in have time to arrive.
        pp = anim_step_rate(&pageAnim, (float)page, restart || fabsf(pageAnim.value - page) > 1.0f, 6.0f, 0.002f);
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
                if (v && v->cover) view_cover(v, cx, cy, g->tileW, g->imgH, 12, D3DCOLOR_ARGB((int)(255 * fade), 255, 255, 255));
                else stage_placeholder(cx, cy, g->tileW, g->imgH, roms[idx].shown.c_str(), fade);
                if (fav_is(roms[idx].file))
                {
                    // The favourite's badge, in the cover's top left corner.
                    UiCircle(cx + 18, cy + 18, 13, UiAlpha(D3DCOLOR_ARGB(200, 16, 18, 26), fade));
                    fav_star(cx + 18, cy + 18, 15, fade);
                }
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

static void browser_draw_spotlight(const std::vector<RomEntry> &roms, int sel);   // below

static void browser_draw_carousel(const std::vector<RomEntry> &roms, int sel)
{
    static UiAnim pos;
    int n = (int)roms.size(), order[2 * CAR_SPREAD + 3], cnt = 0, i, j;
    float p, a = selection_pulse();
    char buf[160];
    UiBackdrop();
    browser_effect();
    if (!roms.empty()) game_backdrop(roms[sel].file, 0);   // Interface > Background images
    UiBrand(UI_SAFE_X, 72, BRAND_SIZE);
    browser_tabs(UI_WIDTH * 0.5f, 72, 1, 520);
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    // The row glides; only a long jump (round the end of the list) cuts straight there.
    p = anim_step_rate(&pos, (float)sel, !pos.live || fabsf(pos.value - sel) > 3 * CAR_SPREAD, 6.0f, 0.002f);   // as the grid's pages
    {
        // Behind the middle cover, as the selection changes: a soft flash and a ring opening out, a few
        // streaks of light going the way the row moves - where the selection went, at a glance.
        static int lastSel = -1, way = 1;
        static float changedAt = -10.0f;
        const float now = UiTime(), cx = UI_WIDTH * 0.5f, cy = CAR_FLOOR - CAR_H * 0.5f;
        float t;
        if (sel != lastSel)
        {
            way = lastSel < 0 || sel > lastSel ? 1 : -1;
            if (lastSel >= 0) changedAt = now;
            lastSel = sel;
        }
        t = (now - changedAt) / 0.55f;
        if (t >= 0 && t < 1)
        {
            float e = 1 - (1 - t) * (1 - t), fade = 1 - t, r = 120 + 260 * e;
            int k;
            UiShadow(cx - CAR_W * 0.55f, cy - CAR_H * 0.6f, CAR_W * 1.1f, CAR_H * 1.2f, 40, 90, SEL_LO(120 * fade));
            UiRoundRing(cx - r, cy - r * 0.55f, r * 2, r * 1.1f, r * 0.55f, 3.0f, SEL_HI(150 * fade));
            for (k = 0; k < 6; k++)
            {
                // From the middle out to the side the row moves to.
                float ly = cy - CAR_H * 0.35f + k * CAR_H * 0.14f, lx0 = cx + way * (CAR_W * 0.3f + e * 160 + k * 9), len = 60 + 50 * fade;
                UiLine(lx0, ly, lx0 + way * len, ly, 2.0f, SEL_HI(110 * fade));
            }
        }
    }
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
        if (ad < 0.5f && !(s_cover3d && v && v->cover && v->shownKind == ART_KIND_BOX))
        {
            // The middle cover: the selection's glow and ring, fading as it slides away (not round a 3D box:
            // a glowing frame round a box seen at an angle looked wrong).
            float ga = a * (1.0f - 2.0f * ad);
            selection_glow(x - 5, y - 5, w + 10, h + 10, 16, ga);
            UiRoundRing(x - 5, y - 5, w + 10, h + 10, 16, 3.0f, SEL_RING(ga));
        }
        if (!(s_cover3d && v && v->cover && v->shownKind == ART_KIND_BOX))
            UiRoundRect(x, y, w, h, 12, UiAlpha(D3DCOLOR_ARGB(255, 22, 23, 31), alpha), UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), alpha));
        if (v && v->cover)
        {
            view_cover(v, x, y, w, h, 12, D3DCOLOR_ARGB((int)(255 * alpha), tint, tint, tint));
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
        // The name large, right under the covers' floor (over the top of the reflections), so it stands out.
        float ty = CAR_FLOOR + CAR_GAP + 50, tw;
        UiTextFit(UI_BOLD, 36, UI_WIDTH - 2 * UI_SAFE_X - 60, roms[sel].shown.c_str(), fit, sizeof(fit));
        tw = UiTextShadowed(UI_BOLD, 36, UI_WIDTH * 0.5f, ty, UI_COL_TEXT, fit, UI_CENTER);
        if (fav_is(roms[sel].file)) fav_star(UI_WIDTH * 0.5f - tw * 0.5f - 30, ty, 22, 1.0f);   // a favourite: its star before the name
    }
    UiScreenFooter(Tr(T_FOOT_BROWSER));
}

// ---- Spotlight: the selection big (its cover, name, facts and a few lines
// about it) over a row of the games around it, gliding as the selection moves.
#define SPOT_TW 150.0f
#define SPOT_TH 87.0f
#define SPOT_GAP 16.0f
#define SPOT_ROW_Y 500.0f
static void browser_draw_spotlight(const std::vector<RomEntry> &roms, int sel)
{
    static UiAnim pos;
    static int lastSel = -1;
    static float changedAt = -10.0f;
    const int n = (int)roms.size();
    const ArtView *v = n ? view_find(roms[sel].file.c_str()) : NULL;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    const float cx = 70, cy = 112, cw = 520, ch = cw * 232 / 400, tx = cx + cw + 44, tw = UI_WIDTH - UI_SAFE_X - tx;
    float p, a = selection_pulse(), fade, ty;
    char buf[200];
    int i;
    UiBackdrop();
    browser_effect();
    if (!roms.empty()) game_backdrop(roms[sel].file, 0);   // Interface > Background images
    UiBrand(UI_SAFE_X, 72, BRAND_SIZE);
    browser_tabs(UI_WIDTH * 0.5f, 72, 1, 520);
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    if (!n) { UiScreenFooter(Tr(T_FOOT_BROWSER)); return; }
    if (sel != lastSel) { lastSel = sel; changedAt = UiTime(); }
    fade = smooth01((UiTime() - changedAt) / 0.25f);   // the big part fades in as the selection changes
    // The big cover, glowing.
    selection_glow(cx - 6, cy - 6, cw + 12, ch + 12, 18, a * fade);
    UiRoundRect(cx, cy, cw, ch, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    if (v && v->cover) view_cover(v, cx, cy, cw, ch, 16, D3DCOLOR_ARGB((int)(255 * fade), 255, 255, 255));
    else stage_placeholder(cx, cy, cw, ch, roms[sel].shown.c_str(), 1.0f);
    // Its name, facts and description at the right.
    ty = cy + 18;
    ty += paragraph(UI_BOLD, 34, tx, ty, tw, 2, UiAlpha(UI_COL_TEXT, fade), gi && gi->title[0] ? gi->title : roms[sel].shown.c_str()) + 10;
    if (fav_is(roms[sel].file)) fav_star(tx + 12, ty + 4, 18, fade);
    if (gi)
    {
        _snprintf(buf, sizeof(buf) - 1, "%s%s%s%s%s", gi->year, gi->year[0] && gi->genre[0] ? "  -  " : "", gi->genre,
                  (gi->year[0] || gi->genre[0]) && gi->developer[0] ? "  -  " : "", gi->developer);
        buf[sizeof(buf) - 1] = 0;
        UiText(UI_REGULAR, 19, tx + (fav_is(roms[sel].file) ? 32 : 0), ty + 4, UiAlpha(UI_COL_TEXT2, fade), buf, UI_LEFT);
        ty += 40;
        if (gi->description[0]) paragraph(UI_REGULAR, 19, tx, ty, tw, 8, UiAlpha(UI_COL_TEXT2, fade), gi->description);
    }
    // The row: the games around the selection, gliding (a long jump cuts straight there).
    p = anim_step_rate(&pos, (float)sel, !pos.live || fabsf(pos.value - sel) > 8, 9.0f, 0.002f);
    for (i = (int)floorf(p) - 5; i <= (int)ceilf(p) + 5; i++)
    {
        float d = i - p, x = UI_WIDTH * 0.5f - SPOT_TW * 0.5f + d * (SPOT_TW + SPOT_GAP), y = SPOT_ROW_Y, al;
        const ArtView *rv;
        int s = i == sel;
        if (i < 0 || i >= n || x + SPOT_TW < 0 || x > UI_WIDTH) continue;
        al = 1.0f - fabsf(d) * 0.13f;
        if (al <= 0.05f) continue;
        rv = view_find(roms[i].file.c_str());
        if (s)
        {
            selection_glow(x - 5, y - 5, SPOT_TW + 10, SPOT_TH + 10, 12, a);
            UiRoundRing(x - 5, y - 5, SPOT_TW + 10, SPOT_TH + 10, 12, 3.0f, SEL_RING(a));
        }
        UiRoundRect(x, y, SPOT_TW, SPOT_TH, 10, UiAlpha(D3DCOLOR_ARGB(255, 22, 23, 31), al), UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), al));
        if (rv && rv->cover) view_cover(rv, x, y, SPOT_TW, SPOT_TH, 10, D3DCOLOR_ARGB((int)(255 * al), s ? 255 : 200, s ? 255 : 200, s ? 255 : 200));
        else stage_placeholder(x, y, SPOT_TW, SPOT_TH, roms[i].shown.c_str(), al);
    }
    UiScreenFooter(Tr(T_FOOT_BROWSER));
}

// ---- Cartridges: the games as N64 cartridges standing in a row on a shelf,
// each one's box art as its label; the selection lifted up out of the row,
// its name and facts under the shelf. The row glides as the carousel's does.
// Drawn, not a picture: the grey shell lit from above, the label's recess,
// the notches on its sides and the grip ridges under the label.
#define CART_W 300.0f        // the middle cartridge
#define CART_H 214.0f
#define CART_SHELF 470.0f    // they stand on this line
#define CART_SPREAD 4        // shown on each side
#define CART_LIFT 46.0f      // how far the selection comes up

// One cartridge at (x, y, w, h): `light` darkens the ones farther from the middle.
static void cartridge(float x, float y, float w, float h, const ArtView *v, float alpha, float light)
{
    const float k = w / CART_W, r = 16 * k;
    const float lw = w - 64 * k, lh = lw * 232.0f / 400.0f, lx = x + (w - lw) * 0.5f, ly = y + 18 * k;
    const int A = (int)(255 * alpha), L = (int)(255 * light);
    int i;
#define CART_RGB(rr, gg, bb) D3DCOLOR_ARGB(A, (int)((rr) * light), (int)((gg) * light), (int)((bb) * light))
    UiShadow(x + 8 * k, y + 14 * k, w - 16 * k, h - 6 * k, r, 24 * k, D3DCOLOR_ARGB(A * 120 / 255, 0, 0, 0));
    // The shell, its top edge catching the light.
    UiRoundRect(x, y, w, h, r, CART_RGB(132, 134, 142), CART_RGB(84, 86, 94));
    UiRect(x + r, y + 2 * k, w - 2 * r, 2 * k, CART_RGB(190, 192, 200));
    // The notches on its sides, low down.
    for (i = 0; i < 2; i++)
        UiRoundRect(i ? x + w - 7 * k : x - 1 * k, y + h * 0.66f, 8 * k, 18 * k, 3 * k, CART_RGB(46, 47, 54), CART_RGB(40, 41, 48));
    // The label's recess, then the label: the game's picture.
    UiRoundRect(lx - 6 * k, ly - 6 * k, lw + 12 * k, lh + 12 * k, 10 * k, CART_RGB(70, 72, 80), CART_RGB(98, 100, 108));
    if (v && v->cover) UiImage(v->cover, lx, ly, lw, lh, 6 * k, D3DCOLOR_ARGB(A, L, L, L));
    else UiRoundRect(lx, ly, lw, lh, 6 * k, CART_RGB(56, 58, 70), CART_RGB(44, 46, 56));
    // The grip ridges under it: a groove and the light on its lower edge.
    for (i = 0; i < 5; i++)
    {
        float gy = ly + lh + 16 * k + i * 7 * k;
        if (gy > y + h - 10 * k) break;
        UiRect(x + w * 0.2f, gy, w * 0.6f, 2 * k, CART_RGB(66, 68, 76));
        UiRect(x + w * 0.2f, gy + 2 * k, w * 0.6f, 1 * k, CART_RGB(150, 152, 160));
    }
#undef CART_RGB
}

static void browser_draw_cartridges(const std::vector<RomEntry> &roms, int sel)
{
    static UiAnim pos;
    int n = (int)roms.size(), order[2 * CART_SPREAD + 3], cnt = 0, i, j;
    float p, a = selection_pulse();
    char buf[200];
    UiBackdrop();
    browser_effect();
    if (!roms.empty()) game_backdrop(roms[sel].file, 0);   // Interface > Background images
    UiBrand(UI_SAFE_X, 72, BRAND_SIZE);
    browser_tabs(UI_WIDTH * 0.5f, 72, 1, 520);
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    if (!n) { UiScreenFooter(Tr(T_FOOT_BROWSER)); return; }
    p = anim_step_rate(&pos, (float)sel, !pos.live || fabsf(pos.value - sel) > 3 * CART_SPREAD, 7.0f, 0.002f);
    // The shelf: a lit edge, its shadow below.
    UiRoundRect(0, CART_SHELF - 10, UI_WIDTH, 10, 0, D3DCOLOR_ARGB(0, 255, 255, 255), D3DCOLOR_ARGB(26, 255, 255, 255));
    UiRect(0, CART_SHELF, UI_WIDTH, 2, D3DCOLOR_ARGB(70, 255, 255, 255));
    UiRoundRect(0, CART_SHELF + 2, UI_WIDTH, 60, 0, D3DCOLOR_ARGB(120, 0, 0, 0), D3DCOLOR_ARGB(0, 0, 0, 0));
    for (i = (int)floorf(p) - CART_SPREAD; i <= (int)ceilf(p) + CART_SPREAD && cnt < (int)(sizeof(order) / sizeof(order[0])); i++)
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
        float s = ad < 1 ? 1.0f - 0.3f * ad : 0.7f - 0.06f * (ad - 1);
        float off = ad < 1 ? ad * 270.0f : 270.0f + (ad - 1) * 205.0f;
        float alpha = ad < 1 ? 1.0f - 0.15f * ad : 0.85f - 0.2f * (ad - 1);
        float light = 1.0f - 0.35f * near1;
        float w = CART_W * s, h = CART_H * s, x = UI_WIDTH * 0.5f + (d < 0 ? -off : off) - w * 0.5f;
        float y = CART_SHELF - h - CART_LIFT * (1.0f - near1);
        if (alpha <= 0.02f || x > UI_WIDTH || x + w < 0) continue;
        if (ad < 0.5f)
        {
            float ga = a * (1.0f - 2.0f * ad);
            selection_glow(x - 6, y - 6, w + 12, h + 12, 20, ga);
            UiRoundRing(x - 5, y - 5, w + 10, h + 10, 20, 3.0f, SEL_RING(ga));
        }
        cartridge(x, y, w, h, view_find(roms[idx].file.c_str()), alpha, light);
        if (fav_is(roms[idx].file))
        {
            // A favourite: its badge on the shell's top left corner.
            UiCircle(x + 20 * s, y + 20 * s, 13, UiAlpha(D3DCOLOR_ARGB(200, 16, 18, 26), alpha));
            fav_star(x + 20 * s, y + 20 * s, 15, alpha);
        }
    }
    {
        // Its name under the shelf, and the year, genre and developer.
        const ArtView *v = view_find(roms[sel].file.c_str());
        const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
        char fit[160];
        UiTextFit(UI_BOLD, 34, UI_WIDTH - 2 * UI_SAFE_X - 60, gi && gi->title[0] ? gi->title : roms[sel].shown.c_str(), fit, sizeof(fit));
        UiTextShadowed(UI_BOLD, 34, UI_WIDTH * 0.5f, CART_SHELF + 58, UI_COL_TEXT, fit, UI_CENTER);
        if (gi)
        {
            _snprintf(buf, sizeof(buf) - 1, "%s%s%s%s%s", gi->year, gi->year[0] && gi->genre[0] ? "  -  " : "", gi->genre,
                      (gi->year[0] || gi->genre[0]) && gi->developer[0] ? "  -  " : "", gi->developer);
            buf[sizeof(buf) - 1] = 0;
            UiTextFit(UI_REGULAR, 19, UI_WIDTH - 2 * UI_SAFE_X, buf, fit, sizeof(fit));
            UiText(UI_REGULAR, 19, UI_WIDTH * 0.5f, CART_SHELF + 100, UI_COL_TEXT2, fit, UI_CENTER);
        }
    }
    UiScreenFooter(Tr(T_FOOT_BROWSER));
}

// ---- Cinema: the selected game's logo big at the bottom left, with its
// facts and a few words about it; a row of the games around it along the
// bottom. The logo is read larger than the covers (game_pics); until it
// comes, or without one, the game's name.
#define CINE_LOGO_W 560
#define CINE_LOGO_H 220
#define CINE_FADE 0.6f       // the bigger pictures fading in (game_pics)
#define CINE_REST 0.15f      // the selection still this long before they are asked for
#define CINE_TW 120.0f       // the row along the bottom
#define CINE_TH 70.0f
#define CINE_GAP 12.0f
#define CINE_ROW_Y 548.0f

// The selected game's bigger pictures, read on a thread of their own once the
// selection rests (the browser's pictures hold one kind of each game at a
// time, at the covers' size): its picture from the game behind every view
// (Interface > Background images, s_gameBg) and its logo (the cinema view).
#define GPIC_SNAP 1          // what is asked for: the picture from the game (up to 640 x 480)
#define GPIC_LOGO 2          // the logo (up to CINE_LOGO_W x CINE_LOGO_H, its alpha kept)
#define GPIC_SNAP_W 640
#define GPIC_SNAP_H 480
struct CinePic { char path[256]; int flags; u8 *snap, *logo; int sw, sh, lw, lh; };
struct CineShow { char path[256]; int flags; IDirect3DTexture9 *snap, *logo; int sw, sh, lw, lh; float at; };
static CRITICAL_SECTION s_cineLock;
static HANDLE s_cineEvt;
static int s_cineStarted;      // 1 running, -1 could not start
static char s_cineWant[256];   // the game asked for and what of it (under s_cineLock)
static int s_cineWantFlags;
static CinePic s_cineDone;     // read, not taken yet (under s_cineLock)
static int s_cineReady;
static CineShow s_cine;        // the textures shown
static int s_cineGen;          // counts cine_clear: asked again after it

static void cine_pic_free(CinePic *p)
{
    free(p->snap);
    free(p->logo);
    memset(p, 0, sizeof(*p));
}

static DWORD WINAPI cine_thread(LPVOID)
{
    for (;;)
    {
        CinePic p;
        WaitForSingleObject(s_cineEvt, INFINITE);
        memset(&p, 0, sizeof(p));
        EnterCriticalSection(&s_cineLock);
        strcpy(p.path, s_cineWant);
        p.flags = s_cineWantFlags;
        LeaveCriticalSection(&s_cineLock);
        if (!p.path[0]) continue;
        if (p.flags & GPIC_SNAP) ArtReadPicture(p.path, ART_KIND_SNAP, GPIC_SNAP_W, GPIC_SNAP_H, &p.snap, &p.sw, &p.sh);
        if (p.flags & GPIC_LOGO) ArtReadPicture(p.path, ART_KIND_LOGO, CINE_LOGO_W, CINE_LOGO_H, &p.logo, &p.lw, &p.lh);
        EnterCriticalSection(&s_cineLock);
        cine_pic_free(&s_cineDone);   // one not taken yet: this newer one instead
        s_cineDone = p;
        s_cineReady = 1;
        LeaveCriticalSection(&s_cineLock);
    }
}

static void cine_want(const char *path, int flags)
{
    if (!s_cineStarted)
    {
        HANDLE h;
        s_cineStarted = -1;
        InitializeCriticalSection(&s_cineLock);
        s_cineEvt = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!s_cineEvt) return;
        h = CreateThread(NULL, 256 * 1024, cine_thread, NULL, CREATE_SUSPENDED, NULL);
        if (!h) return;
        XSetThreadProcessor(h, 4);
        ResumeThread(h);
        CloseHandle(h);
        s_cineStarted = 1;
    }
    if (s_cineStarted < 0) return;
    EnterCriticalSection(&s_cineLock);
    strncpy(s_cineWant, path, sizeof(s_cineWant) - 1);
    s_cineWant[sizeof(s_cineWant) - 1] = 0;
    s_cineWantFlags = flags;
    LeaveCriticalSection(&s_cineLock);
    SetEvent(s_cineEvt);
}

static void cine_show_free(IDirect3DDevice9 *dev, CineShow *s)
{
    if (s->snap) UiImageRelease(dev, s->snap);
    if (s->logo) UiImageRelease(dev, s->logo);
    memset(s, 0, sizeof(*s));
}

// The thread's result into textures (views_take, outside UiBegin/UiEnd); one
// for a game left since, or asked for otherwise since, is dropped.
static void cine_take(IDirect3DDevice9 *dev)
{
    CinePic p;
    int got = 0;
    if (s_cineStarted <= 0) return;
    EnterCriticalSection(&s_cineLock);
    if (s_cineReady)
    {
        p = s_cineDone;
        memset(&s_cineDone, 0, sizeof(s_cineDone));
        s_cineReady = 0;
        got = !_stricmp(p.path, s_cineWant) && p.flags == s_cineWantFlags;
        if (!got) cine_pic_free(&p);
    }
    LeaveCriticalSection(&s_cineLock);
    if (!got) return;
    cine_show_free(dev, &s_cine);
    strcpy(s_cine.path, p.path);
    s_cine.flags = p.flags;
    if (p.snap && (s_cine.snap = UiImageCreate(dev, p.snap, p.sw, p.sh)) != NULL) { s_cine.sw = p.sw; s_cine.sh = p.sh; }
    if (p.logo && (s_cine.logo = UiImageCreate(dev, p.logo, p.lw, p.lh)) != NULL) { s_cine.lw = p.lw; s_cine.lh = p.lh; }
    s_cine.at = UiTime();
    cine_pic_free(&p);
}

static void cine_clear(IDirect3DDevice9 *dev)
{
    cine_show_free(dev, &s_cine);
    s_cineGen++;
    if (s_cineStarted > 0)
    {
        EnterCriticalSection(&s_cineLock);
        s_cineWant[0] = 0;
        LeaveCriticalSection(&s_cineLock);
    }
}

// The selected game's pictures (`flags`, and its picture from the game when
// Background images is on): asked for once the selection rests; 1 when
// s_cine holds them for it.
static int game_pics(const std::string &file, int flags)
{
    static std::string at, asked;
    static int askedFlags, askedGen = -1;
    static float since;
    const float now = UiTime();
    if (s_gameBg) flags |= GPIC_SNAP;
    if (file != at) { at = file; since = now; }
    if (!flags || file.empty()) return 0;
    if (now - since >= CINE_REST && (asked != file || askedFlags != flags || askedGen != s_cineGen))
    {
        asked = file;
        askedFlags = flags;
        askedGen = s_cineGen;
        cine_want(file.c_str(), flags);
    }
    return s_cine.path[0] && !_stricmp(s_cine.path, file.c_str()) && (s_cine.flags & flags) == flags;
}

// Background images: the selected game's picture from the game over the whole
// screen (cut to its shape, zooming in by 6 % over 14 s), fading in, dimmed so
// the view on top reads well. Every view draws it after its background.
static void game_backdrop(const std::string &file, int flags)
{
    float f, sa, ua, u0 = 0, u1 = 1, v0 = 0, v1 = 1, age, z, cu, cv, hu, hv;
    if (!game_pics(file, flags) || !s_gameBg || !s_cine.snap) return;
    age = UiTime() - s_cine.at;
    f = smooth01(age / CINE_FADE);
    sa = (float)s_cine.sw / s_cine.sh;
    ua = (float)UI_WIDTH / UI_HEIGHT;
    if (sa > ua) { u0 = (1 - ua / sa) * 0.5f; u1 = 1 - u0; }
    else { v0 = (1 - sa / ua) * 0.5f; v1 = 1 - v0; }
    z = 1.0f + 0.06f * (age < 14.0f ? age / 14.0f : 1.0f);
    cu = (u0 + u1) * 0.5f; cv = (v0 + v1) * 0.5f; hu = (u1 - u0) * 0.5f / z; hv = (v1 - v0) * 0.5f / z;
    UiImagePart(s_cine.snap, 0, 0, UI_WIDTH, UI_HEIGHT, cu - hu, cv - hv, cu + hu, cv + hv, D3DCOLOR_ARGB((int)(255 * f), 255, 255, 255));
    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, D3DCOLOR_ARGB((int)(120 * f), 8, 9, 14));
    UiRoundRect(0, 0, UI_WIDTH, 150, 0, D3DCOLOR_ARGB((int)(120 * f), 0, 0, 0), D3DCOLOR_ARGB(0, 0, 0, 0));
    UiRoundRect(0, 330, UI_WIDTH, UI_HEIGHT - 330, 0, D3DCOLOR_ARGB(0, 8, 9, 14), D3DCOLOR_ARGB((int)(200 * f), 8, 9, 14));
}

static void browser_draw_cinema(const std::vector<RomEntry> &roms, int sel)
{
    static UiAnim pos;
    static int lastSel = -1;
    static float changedAt = -10.0f;
    const int n = (int)roms.size();
    const ArtView *v = n ? view_find(roms[sel].file.c_str()) : NULL;
    const H64GameInfo *gi = v && v->hasInfo && v->info.found ? &v->info : NULL;
    const float tx = UI_SAFE_X + 6, now = UiTime();
    float p, a = selection_pulse(), fade, f = 0, ty;
    int mine, i;
    char buf[200];
    UiBackdrop();
    browser_effect();
    if (n) game_backdrop(roms[sel].file, GPIC_LOGO);
    UiBrand(UI_SAFE_X, 72, BRAND_SIZE);
    if (!n) { UiScreenFooter(Tr(T_FOOT_BROWSER)); return; }
    if (sel != lastSel) { lastSel = sel; changedAt = now; }
    fade = smooth01((now - changedAt) / 0.25f);
    mine = game_pics(roms[sel].file, GPIC_LOGO);
    if (mine) f = smooth01((now - s_cine.at) / CINE_FADE);
    browser_tabs(UI_WIDTH * 0.5f, 72, 1, 520);
    sprintf(buf, "%d / %d", sel + 1, n);
    UiText(UI_REGULAR, 19, UI_WIDTH - UI_SAFE_X, 64, UI_COL_TEXT2, buf, UI_RIGHT);
    // The logo big at the bottom left (its own proportions, up to 460 x 150), or the name.
    if (mine && s_cine.logo)
    {
        float k = 460.0f / s_cine.lw, lw, lh;
        if (150.0f / s_cine.lh < k) k = 150.0f / s_cine.lh;
        if (k > 1.6f) k = 1.6f;
        lw = s_cine.lw * k;
        lh = s_cine.lh * k;
        UiShadow(tx - 20, 430 - lh - 10, lw + 40, lh + 20, 30, 40, D3DCOLOR_ARGB((int)(90 * f), 0, 0, 0));
        UiImage(s_cine.logo, tx, 430 - lh, lw, lh, 0, D3DCOLOR_ARGB((int)(255 * f), 255, 255, 255));
    }
    if (!(mine && s_cine.logo) || f < 1.0f)
    {
        char fit[160];
        float na = mine && s_cine.logo ? fade * (1.0f - f) : fade;
        UiTextFit(UI_BOLD, 46, UI_WIDTH - 2 * UI_SAFE_X, gi && gi->title[0] ? gi->title : roms[sel].shown.c_str(), fit, sizeof(fit));
        UiTextShadowed(UI_BOLD, 46, tx, 400, UiAlpha(UI_COL_TEXT, na), fit, UI_LEFT);
    }
    // The year, genre and developer, a favourite's star before them; a few words about it.
    ty = 462;
    {
        float sx = tx;
        if (fav_is(roms[sel].file)) { fav_star(tx + 10, ty, 18, fade); sx += 30; }
        if (gi)
        {
            _snprintf(buf, sizeof(buf) - 1, "%s%s%s%s%s", gi->year, gi->year[0] && gi->genre[0] ? "  -  " : "", gi->genre,
                      (gi->year[0] || gi->genre[0]) && gi->developer[0] ? "  -  " : "", gi->developer);
            buf[sizeof(buf) - 1] = 0;
            UiText(UI_REGULAR, 20, sx, ty, UiAlpha(UI_COL_TEXT2, fade), buf, UI_LEFT);
            if (gi->description[0]) paragraph(UI_REGULAR, 18, tx, ty + 18, 760, 2, UiAlpha(UI_COL_TEXT2, fade), gi->description);
        }
    }
    // The row: the games around the selection, gliding (a long jump cuts straight there).
    p = anim_step_rate(&pos, (float)sel, !pos.live || fabsf(pos.value - sel) > 8, 9.0f, 0.002f);
    for (i = (int)floorf(p) - 6; i <= (int)ceilf(p) + 6; i++)
    {
        float d = i - p, x = UI_WIDTH * 0.5f - CINE_TW * 0.5f + d * (CINE_TW + CINE_GAP), y = CINE_ROW_Y, al;
        const ArtView *rv;
        int s = i == sel;
        if (i < 0 || i >= n || x + CINE_TW < 0 || x > UI_WIDTH) continue;
        al = 1.0f - fabsf(d) * 0.12f;
        if (al <= 0.05f) continue;
        rv = view_find(roms[i].file.c_str());
        if (s)
        {
            selection_glow(x - 5, y - 5, CINE_TW + 10, CINE_TH + 10, 12, a);
            UiRoundRing(x - 5, y - 5, CINE_TW + 10, CINE_TH + 10, 12, 3.0f, SEL_RING(a));
        }
        UiRoundRect(x, y, CINE_TW, CINE_TH, 10, UiAlpha(D3DCOLOR_ARGB(255, 22, 23, 31), al), UiAlpha(D3DCOLOR_ARGB(255, 16, 17, 23), al));
        if (rv && rv->cover) view_cover(rv, x, y, CINE_TW, CINE_TH, 10, D3DCOLOR_ARGB((int)(255 * al), s ? 255 : 190, s ? 255 : 190, s ? 255 : 190));
        else stage_placeholder(x, y, CINE_TW, CINE_TH, roms[i].shown.c_str(), al);
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
// `open`: 0..1 as it opens (the veil fades in, the panel rises into place).
static void cover_detail_draw(const RomEntry *rom, const RomInfo *ri, const ArtView *v, float open)
{
    const float e = ease_out(open), rise = (1.0f - e) * 44.0f;
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
    y += rise;
    boxX = ix;
    boxY = y + pad;
    divY = boxY + contentH + 28;
    detailY = divY + 22;
    footY = detailY + 39 + 24 + 15;

    UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UiAlpha(UI_COL_DIM, e));   // the grid behind, veiled
    edge_glow(x, y, w, h, 24);
    UiPanel(x, y, w, h, 24);
    {
        // The cover framed like the selected tile it came from.
        float a = selection_pulse();
        selection_glow(boxX - 4, boxY - 4, boxW + 8, boxH + 8, 18, a);
        UiRoundRing(boxX - 4, boxY - 4, boxW + 8, boxH + 8, 18, 3.0f, SEL_RING(a));
    }
    UiRoundRect(boxX, boxY, boxW, boxH, 16, D3DCOLOR_ARGB(255, 22, 23, 31), D3DCOLOR_ARGB(255, 16, 17, 23));
    if (v && v->cover) view_cover(v, boxX, boxY, boxW, boxH, 16, D3DCOLOR_ARGB(255, 255, 255, 255));
    else stage_placeholder(boxX, boxY, boxW, boxH, rom->shown.c_str(), 1.0f);

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
// to the grid. The view it was opened from (a grid's page, or the carousel)
// stays drawn behind it; the panel opens over it in OPEN_S.
#define DETAIL_OPEN_S 0.20f
static int cover_detail_screen(IDirect3DDevice9 *dev, const std::vector<RomEntry> &roms, int sel, int page, int design)
{
    UiInput in;
    RomInfo ri;
    const RomEntry *rom = &roms[sel];
    float openAt;
    UiInputInit(&in);
    rom_details(rom->file.c_str(), &ri);
    openAt = UiTime();
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        if (down & XINPUT_GAMEPAD_A) return 1;
        if (down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) return 0;
        art_step(rom->file, right_stick_step());   // RS: its next picture here too
        views_take(dev);                           // ... and it arrives while this stays open
        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        if (design == BD_CAROUSEL) browser_draw_carousel(roms, sel);
        else if (design == BD_SPOTLIGHT) browser_draw_spotlight(roms, sel);
        else if (design == BD_CARTRIDGE) browser_draw_cartridges(roms, sel);
        else if (design == BD_CINEMA) browser_draw_cinema(roms, sel);
        else browser_draw_covers(roms, sel, page, grid_layout(design));
        cover_detail_draw(rom, &ri, view_find(rom->file.c_str()), (UiTime() - openAt) / DETAIL_OPEN_S);
        UiEnd();
        dev->Present(NULL, NULL, NULL, NULL);
        Sleep(16);
    }
}

#define MSG_MS 6000   // how long a message (new version, box art) stays

// Box art may be downloaded here (the settings allow it; not in Xenia, whose
// network is not the console's, unless artdownload=2).
static int art_net_possible(const Config *c) { return (!c->xenia && h64_fenv_reliable()) || c->artDownload == 2; }

// The tab's games again (tab_filter) and their paths; returns where `keep`
// (a ROM file) is in it now, or 0.
static int browser_retab(const std::vector<RomEntry> &all, std::vector<RomEntry> *roms, std::vector<const char *> *paths, const std::string &keep)
{
    int i, at = 0;
    tab_filter(all, roms);
    paths->clear();
    for (i = 0; i < (int)roms->size(); i++)
    {
        paths->push_back((*roms)[i].file.c_str());
        if (!keep.empty() && !_stricmp((*roms)[i].file.c_str(), keep.c_str())) at = i;
    }
    return at;
}

int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize)
{
    static int s_booted;   // the first visit of this session (Settings > System > Game at start)
    std::vector<RomEntry> allRoms, roms;      // every game; those of the tab shown (LT/RT)
    std::vector<const char *> allPaths, paths;
    UiInput in;
    int sel = 0, top = 0, page = 0, detailsFor = -1, i, lastSel = -1, wasBulk = 0, trigPrev = 3;
    RomInfo details;
    const int rows = 13;
    char newVersion[32], msg[256], drive[16];
    DWORD msgAt = 0, demoAt = GetTickCount(), idleAt = GetTickCount();
    float selAt = 0;
    float viewAt = -10.0f;   // when B changed the view (its fade-in)
    msg[0] = 0;
    // Every folder of Settings > Paths, there to be filled from the first start on; the ROMs' read from its own.
    paths_apply(c);
    paths_create(c);
    scan_roms(&allRoms);
    s_romsTotal = (int)allRoms.size();
    for (i = 0; i < (int)allRoms.size(); i++) allPaths.push_back(allRoms[i].file.c_str());
    s_recPaths.clear();   // the wallpapers' recommendations come from these
    for (i = 0; i < (int)allRoms.size(); i++) s_recPaths.push_back(allRoms[i].file);
    roms = allRoms;   // the tab is applied once the box art (game information) is reachable, below
    for (i = 0; i < (int)roms.size(); i++)
    {
        paths.push_back(roms[i].file.c_str());
        if (!_stricmp(roms[i].file.c_str(), c->lastRom)) sel = i;
    }
    if (!s_booted && c->intro && !s_splashShown) wiz_splash(dev);   // Interface > Welcome intro: the LAIKA 64 logo
    if (!s_booted)
    {
        // Settings > System > Game at start: the last game played, or the one
        // chosen, straight away - unless BACK is held while the emulator starts.
        XINPUT_STATE st;
        const char *want = c->startRomMode == 1 ? c->lastRom : c->startRomMode == 2 ? c->startRom : "";
        s_booted = 1;
        if (want[0] && GetFileAttributesA(want) != (DWORD)0xFFFFFFFF &&
            !(XInputGetState(0, &st) == ERROR_SUCCESS && (st.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)))
        {
            strncpy(path, want, pathSize - 1);
            path[pathSize - 1] = 0;
            H64_INFO("[menu] game at start: %s", path);
            return 1;
        }
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
    if (c->bgEffect < 0 || c->bgEffect >= BGFX_COUNT) c->bgEffect = BGFX_WAVES;
    ui_from_config(c);
    page = roms.empty() ? 0 : sel / grid_per_page(grid_layout(c->browserDesign));   // a grid starts on sel's page, not always page 0
    // Box art: in the folder of the settings' drive ("game:\config\settings.ini" -> game).
    SettingsSetLangFile(settingsPath);
    for (i = 0; settingsPath[i] && settingsPath[i] != ':' && i < (int)sizeof(drive) - 1; i++) drive[i] = settingsPath[i];
    drive[i] = 0;
    if (!settingsPath[i] || !i) strcpy(drive, "game");
    ArtSetKind(art_kind_of(c));
    ArtStart(drive);
    ArtSetLang(art_lang(c));   // the games' descriptions in the menus' language
    ArtSetNetwork(c->artDownload && art_net_possible(c));
    ArtSetExtras(c->artExtras);
    ArtPause(0);
    strncpy(s_libDrive, drive, sizeof(s_libDrive) - 1);   // favourites and categories: <drive>:\config\ too
    if (s_tab)
    {
        // Back from a game on a tab other than "every game": that tab again, on the same game.
        std::string cur = roms.empty() ? std::string() : roms[sel].file;
        tab_filter(allRoms, &roms);
        paths.clear();
        sel = 0;
        for (i = 0; i < (int)roms.size(); i++)
        {
            paths.push_back(roms[i].file.c_str());
            if (!_stricmp(roms[i].file.c_str(), cur.c_str())) sel = i;
        }
        page = roms.empty() ? 0 : sel / grid_per_page(grid_layout(c->browserDesign));
    }
    else s_tabGames = (int)roms.size();
    UiSetSkinWant("");   // Skins are gone (Interface): an old choice in the file stays unused
    // The sound settings of config\settings.ini (main.cpp read only harissa64v2.ini's at start):
    // before the music starts, so it starts at its own volume.
    xb_audio_set_game_volume(c->volumeGame);
    xb_audio_set_music_volume(c->volumeMusic);
    xb_audio_set_sfx_volume(c->volumeSfx);
    xb_audio_set_mono(c->audioMono);
    xb_audio_music_start(MUSIC_DIR, c->musicMode, c->musicTrack);   // nothing there: no music
    xb_audio_ui_enable(c->uiSounds);
    xb_audio_ui_style(c->uiSoundStyle);

    UiInputInit(&in);
    memset(&details, 0, sizeof(details));
    if (c->controlsTest >= 0) settings_loop(dev, c, settingsPath, c->controlsTest);
    // The box art of every game, in the background: what is missing (artall=1,
    // checks: also retrying what was not found before).
    if (!allRoms.empty() && art_net_possible(c) && (c->artAll || c->artDownload)) ArtBulkStart(&allPaths[0], (int)allPaths.size(), c->artAll);
    // A newer release on GitHub: a message for a few seconds (the check runs
    // in the background and may answer later, or never without a network).
    // updatecheck=2 (testing): as if this were version 0.0.0, so any release is newer.
    if (c->updateCheck && !c->xenia && h64_fenv_reliable()) UpdateCheckStart(c->updateCheck == 2 ? "0.0.0" : H64_VERSION_STRING);
    // The first start of a new version (an update): what is new in it, once - not the
    // welcome's set-up again (that is for the first start of all, main.cpp).
    if (strcmp(c->lastVersion, H64_VERSION_STRING) && c->autoStart <= 0 && c->browserDemo <= 0 && settingsPath && settingsPath[0])
    {
        news_screen(dev);
        strncpy(c->lastVersion, H64_VERSION_STRING, sizeof(c->lastVersion) - 1);
        c->lastVersion[sizeof(c->lastVersion) - 1] = 0;
        ConfigWriteMenuKeys(c, settingsPath, 1);
        UiInputInit(&in);
    }
    for (;;)
    {
        WORD down = UiInputPoll(&in);
        ArtStatus st;
        int n = (int)roms.size();
        {
            // USB memories, every 2 s: one plugged in asks whether to add its games (and
            // gets the USB tab); taken out, its games and the tab go. Those there at the
            // start count as there.
            static int usbMask = -1;
            static DWORD usbAt;
            if (!c->xenia && GetTickCount() - usbAt >= 2000)
            {
                int k, now = 0;
                usbAt = GetTickCount();
                for (k = 0; k < 3; k++)
                    if (usb_present(k)) now |= 1 << k;
                if (usbMask < 0) usbMask = now;
                for (k = 0; k < 3; k++)
                {
                    int bit = 1 << k;
                    if ((now & bit) && !(usbMask & bit))
                    {
                        H64_INFO("[menu] memory plugged in: Usb%d", k);
                        xb_audio_ui_sound(XB_SFX_NOTIFY);
                        if (s_usbMerged < 0 && confirm_ask(dev, T_USB_FOUND, T_USB_SCAN_ASK, T_FOOT_USB))
                        {
                            std::vector<RomEntry> add;
                            std::string keep = n ? roms[sel].file : std::string();
                            char drv[8];
                            size_t j;
                            sprintf(drv, "Usb%d:", k);
                            scan_rom_tree(drv, 0, &add);
                            s_usbMerged = k;
                            for (j = 0; j < add.size(); j++) allRoms.push_back(add[j]);
                            std::sort(allRoms.begin(), allRoms.end(), rom_less);
                            allPaths.clear();
                            for (j = 0; j < allRoms.size(); j++) allPaths.push_back(allRoms[j].file.c_str());
                            s_romsTotal = (int)allRoms.size();
                            sel = browser_retab(allRoms, &roms, &paths, keep);
                            n = (int)roms.size();
                            _snprintf(msg, sizeof(msg) - 1, Tr(T_USB_ADDED), (int)add.size());
                            msg[sizeof(msg) - 1] = 0;
                            H64_INFO("[menu] Usb%d: %d game(s) added", k, (int)add.size());
                        }
                        else
                            _snprintf(msg, sizeof(msg) - 1, "%s", Tr(T_USB_FOUND));
                        msg[sizeof(msg) - 1] = 0;
                        msgAt = GetTickCount();
                        UiInputInit(&in);
                        down = 0;
                    }
                    if (!(now & bit) && (usbMask & bit))
                    {
                        H64_INFO("[menu] memory taken out: Usb%d", k);
                        if (s_usbMerged == k)
                        {
                            std::string keep = n ? roms[sel].file : std::string();
                            size_t j;
                            std::vector<RomEntry> left;
                            if (tab_is_usb(s_tab)) s_tab = 0;
                            for (j = 0; j < allRoms.size(); j++)
                                if (!rom_on_usb(allRoms[j].file)) left.push_back(allRoms[j]);
                            s_usbMerged = -1;
                            allRoms = left;
                            allPaths.clear();
                            for (j = 0; j < allRoms.size(); j++) allPaths.push_back(allRoms[j].file.c_str());
                            s_romsTotal = (int)allRoms.size();
                            sel = browser_retab(allRoms, &roms, &paths, keep);
                            n = (int)roms.size();
                        }
                        strcpy(msg, Tr(T_USB_REMOVED));
                        msgAt = GetTickCount();
                        xb_audio_ui_sound(XB_SFX_NOTIFY);
                    }
                }
                if (now != usbMask)
                {
                    usbMask = now;
                    top = sel >= rows ? sel - rows + 1 : 0;
                    page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
                    lastSel = detailsFor = -1;   // the pictures and the card asked for again
                }
            }
        }
        // The screen saver, after Config::saverTime minutes without any input.
        if (down || saver_any_input()) idleAt = GetTickCount();
        else if (c->saverMode > SAVER_OFF && c->saverMode < SAVER_COUNT && c->browserDemo <= 0 &&
                 GetTickCount() - idleAt >= (DWORD)saver_minutes(c) * 60000u)
        {
            saver_run(dev, c);
            idleAt = GetTickCount();
            UiInputInit(&in);
            continue;
        }
        if (UpdateCheckTake(newVersion, sizeof(newVersion)))
        {
            _snprintf(msg, sizeof(msg) - 1, Tr(T_UPDATE_AVAILABLE), newVersion);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
            xb_audio_ui_sound(XB_SFX_NOTIFY);
            H64_INFO("[update] %s", msg);
            // The updater: asked first; yes downloads, checks and installs it, then LAIKA 64 starts again.
            if (confirm_ask(dev, T_UPDATE_TITLE, T_UPDATE_ASK, T_FOOT_UPDATE))
            {
                views_clear(dev);   // the memory for the download
                ArtPause(1);
                if (update_run(dev, newVersion))
                {
                    xb_audio_music_stop();
                    XLaunchNewImage("game:\\default.xex", 0);   // the new version
                }
                ArtPause(0);
                lastSel = -1;   // the pictures again
                _snprintf(msg, sizeof(msg) - 1, "%s", Tr(T_UPDATE_FAILED));
                msg[sizeof(msg) - 1] = 0;
                msgAt = GetTickCount();
            }
            UiInputInit(&in);
        }
        if (ArtTakeNote(msg, sizeof(msg))) { msgAt = GetTickCount(); xb_audio_ui_sound(XB_SFX_NOTIFY); }
        if (BatteryCheck(c, msg, sizeof(msg))) { msgAt = GetTickCount(); xb_audio_ui_sound(XB_SFX_NOTIFY); }
        if (n) art_step(roms[sel].file, right_stick_step());   // RS: the selected game's next picture, in every view
        art_auto_turn(n ? roms[sel].file : std::string());     // and by itself after a moment on it
        {
            // RS up and down: the menus' music volume, 2 % a step while held; kept when the stick is let go.
            static DWORD stepAt;
            static int changed;
            XINPUT_STATE ps;
            int ry = 0, rx = 0;
            if (XInputGetState(0, &ps) == ERROR_SUCCESS) { ry = ps.Gamepad.sThumbRY; rx = ps.Gamepad.sThumbRX; }
            if ((ry > 20000 || ry < -20000) && abs(ry) > abs(rx))
            {
                DWORD now = GetTickCount();
                if (now - stepAt >= 70)
                {
                    c->volumeMusic += ry > 0 ? 2 : -2;
                    if (c->volumeMusic < 0) c->volumeMusic = 0;
                    if (c->volumeMusic > 100) c->volumeMusic = 100;
                    xb_audio_set_music_volume(c->volumeMusic);
                    music_volume_poke();
                    stepAt = now;
                    changed = 1;
                }
            }
            else if (changed)
            {
                changed = 0;
                ConfigWriteMenuKeys(c, settingsPath, 1);
            }
        }
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
                    // The game needs the memory and the GPU: the pictures go, the worker waits
                    // (or goes on with the downloads only: Game images > Download while playing).
                    views_clear(dev);
                    ArtPause(c->artBackground && c->artDownload ? 2 : 1);
                    xb_audio_music_stop();
                    xb_audio_ui_sound_instead(XB_SFX_LAUNCH);
                    return 1;
                }
                if (sel < top) top = sel;
                if (sel >= top + rows) top = sel - rows + 1;
                if (detailsFor != sel) { rom_details(roms[sel].file.c_str(), &details); detailsFor = sel; }
                if (sel != lastSel)
                {
                    // The selection first, then every row shown (each has its small cover now),
                    // then a row either side read ahead.
                    const char *want[64];
                    int k, cnt = 0;
                    want[cnt++] = paths[sel];
                    for (k = -1; k <= rows && cnt < 64; k++)
                    {
                        int idx = top + k;
                        if (idx < 0 || idx >= n || idx == sel) continue;
                        want[cnt++] = paths[idx];
                    }
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
                const int perPage = grid_per_page(g), carousel = design_row(c->browserDesign);
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
                    if (cover_detail_screen(dev, roms, sel, page, c->browserDesign))
                    {
                        strncpy(path, roms[sel].file.c_str(), pathSize - 1);
                        path[pathSize - 1] = 0;
                        strncpy(c->lastRom, path, sizeof(c->lastRom) - 1);
                        ConfigWriteMenuKeys(c, settingsPath, 1);
                        views_clear(dev);
                        ArtPause(c->artBackground && c->artDownload ? 2 : 1);
                        xb_audio_music_stop();
                        xb_audio_ui_sound(XB_SFX_LAUNCH);
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
                        // Cinema's row shows more of them, smaller.
                        const int spread = c->browserDesign == BD_CINEMA ? 6 : CAR_SPREAD;
                        want.push_back(paths[sel]);
                        for (k = 1; k <= spread; k++)
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
        {
            // LT / RT: the previous / next tab (every game, favourites, the categories).
            XINPUT_STATE ts;
            int held = 0, pressed, tabs;
            if (XInputGetState(0, &ts) == ERROR_SUCCESS)
                held = (ts.Gamepad.bLeftTrigger > 128 ? 1 : 0) | (ts.Gamepad.bRightTrigger > 128 ? 2 : 0);
            pressed = held & ~trigPrev;
            trigPrev = held;
            tabs = tab_count();
            if (pressed && s_romsTotal && tabs > 1)
            {
                std::string keep = n ? roms[sel].file : std::string();
                s_tab = (s_tab + ((pressed & 2) ? 1 : tabs - 1)) % tabs;
                sel = browser_retab(allRoms, &roms, &paths, keep);
                n = (int)roms.size();
                top = sel >= rows ? sel - rows + 1 : 0;
                page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
                lastSel = detailsFor = -1;
                xb_audio_ui_sound(XB_SFX_TAB);
                if (c->browserDesign == BD_LIST)
                {
                    // The list has no tab bar: the new tab's name and count in a note.
                    char name[160];
                    tab_name(s_tab, name, sizeof(name));
                    _snprintf(msg, sizeof(msg) - 1, "%s (%d)", name, s_tabGames);
                    msg[sizeof(msg) - 1] = 0;
                    msgAt = GetTickCount();
                }
            }
        }
        if ((down & XINPUT_GAMEPAD_X) && n)
        {
            // X: the selected game in or out of the favourites (on the Favourites tab: out of the list).
            int now = fav_toggle(roms[sel].file);
            xb_audio_ui_sound_instead(now ? XB_SFX_FAV_ON : XB_SFX_FAV_OFF);
            strncpy(msg, Tr(now ? T_FAV_ADDED : T_FAV_REMOVED), sizeof(msg) - 1);
            msg[sizeof(msg) - 1] = 0;
            msgAt = GetTickCount();
            if (s_tab == 1)
            {
                int keepAt = sel;
                browser_retab(allRoms, &roms, &paths, std::string());
                n = (int)roms.size();
                sel = keepAt < n ? keepAt : n - 1;
                if (sel < 0) sel = 0;
                top = sel >= rows ? sel - rows + 1 : 0;
                page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
                lastSel = detailsFor = -1;
            }
        }
        if (down & XINPUT_GAMEPAD_Y)
        {
            int artWas = c->artDownload, extrasWas = c->artExtras;
            std::string keep = n ? roms[sel].file : std::string();
            s_browserSel = keep;   // Settings > System > Game at start can pick it
            {
                char romsWas[128];
                strcpy(romsWas, s_romDir);
                settings_loop(dev, c, settingsPath, -1);
                xb_audio_music_start(MUSIC_DIR, c->musicMode, c->musicTrack);   // the music's folder may have changed
                if (_stricmp(romsWas, s_romDir))
                {
                    // Another ROMs folder (Settings > Paths): the list made again from it.
                    views_clear(dev);
                    return 2;
                }
            }
            detailsFor = -1;   // the card's texts again (the language may have changed)
            ArtSetLang(art_lang(c));   // the language may have changed: the descriptions follow it
            if (ArtGetKind() != art_kind_of(c))
            {
                // Another kind of picture (Interface > Game images): every view read again in it.
                views_clear(dev);
                ArtSetKind(art_kind_of(c));
            }
            ArtSetNetwork(c->artDownload && art_net_possible(c));
            ArtSetExtras(c->artExtras);
            if (((!artWas && c->artDownload) || (artWas && c->artDownload && !extrasWas && c->artExtras)) && !allRoms.empty() && art_net_possible(c))
                ArtBulkStart(&allPaths[0], (int)allPaths.size(), 0);   // downloads (or the screenshots and logos) just turned on: every game's
            // The categories may have changed: the tab again (or "every game" if its category went).
            sel = browser_retab(allRoms, &roms, &paths, keep);
            n = (int)roms.size();
            top = sel >= rows ? sel - rows + 1 : 0;
            page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
            lastSel = -1;
            UiInputInit(&in);
        }
        if ((down & XINPUT_GAMEPAD_X) && !s_romsTotal)
        {
            AboutScreen(dev);   // no ROMs: no view to change, X keeps the About page (FOOT_BROWSER_EMPTY)
            UiInputInit(&in);
        }
        else if ((down & XINPUT_GAMEPAD_B) && s_romsTotal)
        {
            // B, "Interface view": the next layout (list, covers, big covers, carousel; Interfaz > Diseño), kept at once.
            c->browserDesign = (c->browserDesign + 1) % BD_COUNT;
            viewAt = UiTime();   // it fades in (browser_view_veil), the covers arriving meanwhile
            ConfigWriteMenuKeys(c, settingsPath, 1);
            page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
            if (sel < top || sel >= top + rows) top = sel >= rows ? sel - rows + 1 : 0;
            lastSel = -1;   // the new view asks for its own box art
        }
        ArtGetStatus(&st);
        if (wasBulk && !st.bulk)
        {
            for (i = 0; i < (int)allPaths.size(); i++) ArtForget(allPaths[i]);   // the downloads are over: the covers shown are read again
            s_libLoaded = 0;   // and the games' information, for the categories
            if (s_tab >= 2)
            {
                std::string keep = n ? roms[sel].file : std::string();
                sel = browser_retab(allRoms, &roms, &paths, keep);
                n = (int)roms.size();
                top = sel >= rows ? sel - rows + 1 : 0;
                page = n ? sel / grid_per_page(grid_layout(c->browserDesign)) : 0;
                lastSel = detailsFor = -1;
            }
        }
        wasBulk = st.bulk;
        views_take(dev);

        dev->Clear(0, NULL, D3DCLEAR_TARGET, UI_COL_BG_BOTTOM, 1.0f, 0);
        UiBegin(dev);
        ui_from_config(c);   // also after the settings were left without saving
        if (c->browserDesign == BD_LIST || !n)
            browser_draw(roms, sel, top, rows, &details, n ? view_find(roms[sel].file.c_str()) : NULL,
                         n && !_stricmp(st.busyPath, roms[sel].file.c_str()), selAt, &st);
        else if (c->browserDesign == BD_CAROUSEL)
            browser_draw_carousel(roms, sel);
        else if (c->browserDesign == BD_SPOTLIGHT)
            browser_draw_spotlight(roms, sel);
        else if (c->browserDesign == BD_CARTRIDGE)
            browser_draw_cartridges(roms, sel);
        else if (c->browserDesign == BD_CINEMA)
            browser_draw_cinema(roms, sel);
        else
            browser_draw_covers(roms, sel, page, grid_layout(c->browserDesign));
        {
            // Another view: it comes in under a veil that lifts slowly (0.9 s), so the
            // covers it asks for have time to arrive instead of popping in one by one.
            float t = (UiTime() - viewAt) / 0.9f;
            if (t >= 0.0f && t < 1.0f)
            {
                float a = 1.0f - t * t * (3.0f - 2.0f * t);
                UiRect(0, 0, UI_WIDTH, UI_HEIGHT, UiAlpha(UI_COL_BG_BOTTOM, a));
            }
        }
        if (st.busyPath[0] && (c->browserDesign != BD_LIST || !n || _stricmp(st.busyPath, roms[sel].file.c_str())))
            download_badge();   // the list's card says it for the selection; the rest of the views here
        music_volume_bar(c->volumeMusic);
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
