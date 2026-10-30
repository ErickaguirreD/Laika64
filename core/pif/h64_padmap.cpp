// Harissa64 V2 - controller remapping (see h64_padmap.h).
#include "h64_padmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../system/h64_system.h"

struct SrcInfo { const char *name, *label; };
static const SrcInfo s_src[H64_SRC_COUNT] = {
    { "xa", "A" }, { "xb", "B" }, { "xx", "X" }, { "xy", "Y" }, { "lb", "LB" }, { "rb", "RB" }, { "lt", "LT" }, { "rt", "RT" },
    { "start", "START" }, { "dup", "D-pad up" }, { "ddown", "D-pad down" }, { "dleft", "D-pad left" }, { "dright", "D-pad right" },
    { "ls", "Left stick click" }, { "rs", "Right stick click" },
    { "lup", "Left stick up" }, { "ldown", "Left stick down" }, { "lleft", "Left stick left" }, { "lright", "Left stick right" },
    { "rup", "Right stick up" }, { "rdown", "Right stick down" }, { "rleft", "Right stick left" }, { "rright", "Right stick right" },
};

struct N64Info { const char *name, *label; u16 bit; };
static const N64Info s_n64[H64_N64_COUNT] = {
    { "a", "A", 0x8000 }, { "b", "B", 0x4000 }, { "z", "Z", 0x2000 }, { "start", "START", 0x1000 },
    { "dup", "D-pad up", 0x0800 }, { "ddown", "D-pad down", 0x0400 }, { "dleft", "D-pad left", 0x0200 }, { "dright", "D-pad right", 0x0100 },
    { "l", "L", 0x0020 }, { "r", "R", 0x0010 },
    { "cup", "C-up", 0x0008 }, { "cdown", "C-down", 0x0004 }, { "cleft", "C-left", 0x0002 }, { "cright", "C-right", 0x0001 },
};

#define BIT(s) (1u << (s))

void h64_padmap_default(H64PadMap *m)
{
    memset(m, 0, sizeof(*m));
    m->src[H64_N64_A] = BIT(H64_SRC_A);
    m->src[H64_N64_B] = BIT(H64_SRC_X);    // where B sits on the N64 pad, left of A (user choice, 2026-10-28)
    m->src[H64_N64_Z] = BIT(H64_SRC_LT);   // the trigger under the left hand
    m->src[H64_N64_START] = BIT(H64_SRC_START);
    m->src[H64_N64_DUP] = BIT(H64_SRC_DUP);
    m->src[H64_N64_DDOWN] = BIT(H64_SRC_DDOWN);
    m->src[H64_N64_DLEFT] = BIT(H64_SRC_DLEFT);
    m->src[H64_N64_DRIGHT] = BIT(H64_SRC_DRIGHT);
    m->src[H64_N64_L] = BIT(H64_SRC_LB);
    m->src[H64_N64_R] = BIT(H64_SRC_RB);
    m->src[H64_N64_CUP] = BIT(H64_SRC_RUP);
    m->src[H64_N64_CDOWN] = BIT(H64_SRC_RDOWN);
    m->src[H64_N64_CLEFT] = BIT(H64_SRC_RLEFT);
    m->src[H64_N64_CRIGHT] = BIT(H64_SRC_RRIGHT);
    m->stick = H64_STICK_LEFT;
    h64_padmap_cal_default(&m->cal[0]);
    h64_padmap_cal_default(&m->cal[1]);
    m->drift = H64_PAD_DRIFT_DEFAULT;
}

void h64_padmap_cal_default(H64StickCal *c)
{
    memset(c, 0, sizeof(*c));
    c->deadzone = H64_PAD_DEADZONE_DEFAULT;
    c->outer = H64_PAD_OUTER_DEFAULT;
    c->range = H64_PAD_RANGE_DEFAULT;
    c->curve = H64_PAD_CURVE_DEFAULT;
}

static int cal_equal(const H64StickCal *a, const H64StickCal *b)
{
    return a->cx == b->cx && a->cy == b->cy && a->deadzone == b->deadzone && a->outer == b->outer && a->range == b->range &&
           a->curve == b->curve && a->invert == b->invert;
}

int h64_padmap_is_default(const H64PadMap *m)
{
    H64PadMap d;
    int i;
    h64_padmap_default(&d);
    for (i = 0; i < H64_N64_COUNT; i++)
        if (m->src[i] != d.src[i]) return 0;
    return m->stick == d.stick && cal_equal(&m->cal[0], &d.cal[0]) && cal_equal(&m->cal[1], &d.cal[1]) && m->drift == d.drift;
}

u32 h64_padmap_sources(const H64PadInput *in)
{
    static const u16 buttons[] = { H64_XB_A, H64_XB_B, H64_XB_X, H64_XB_Y, H64_XB_LEFT_SHOULDER, H64_XB_RIGHT_SHOULDER };
    const int t = H64_PAD_STICK_THRESHOLD;
    u32 s = 0;
    int i;
    for (i = 0; i < 6; i++)
        if (in->buttons & buttons[i]) s |= BIT(H64_SRC_A + i);
    if (in->lt > H64_PAD_TRIGGER_THRESHOLD) s |= BIT(H64_SRC_LT);
    if (in->rt > H64_PAD_TRIGGER_THRESHOLD) s |= BIT(H64_SRC_RT);
    if (in->buttons & H64_XB_START) s |= BIT(H64_SRC_START);
    if (in->buttons & H64_XB_DPAD_UP) s |= BIT(H64_SRC_DUP);
    if (in->buttons & H64_XB_DPAD_DOWN) s |= BIT(H64_SRC_DDOWN);
    if (in->buttons & H64_XB_DPAD_LEFT) s |= BIT(H64_SRC_DLEFT);
    if (in->buttons & H64_XB_DPAD_RIGHT) s |= BIT(H64_SRC_DRIGHT);
    if (in->buttons & H64_XB_LEFT_THUMB) s |= BIT(H64_SRC_LTHUMB);
    if (in->buttons & H64_XB_RIGHT_THUMB) s |= BIT(H64_SRC_RTHUMB);
    if (in->ly > t) s |= BIT(H64_SRC_LUP);
    if (in->ly < -t) s |= BIT(H64_SRC_LDOWN);
    if (in->lx < -t) s |= BIT(H64_SRC_LLEFT);
    if (in->lx > t) s |= BIT(H64_SRC_LRIGHT);
    if (in->ry > t) s |= BIT(H64_SRC_RUP);
    if (in->ry < -t) s |= BIT(H64_SRC_RDOWN);
    if (in->rx < -t) s |= BIT(H64_SRC_RLEFT);
    if (in->rx > t) s |= BIT(H64_SRC_RRIGHT);
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void h64_padmap_stick_norm(const H64StickCal *c, int rx, int ry, float *ox, float *oy)
{
    int dead = clampi(c->deadzone, 0, 32000), outer = clampi(c->outer, 16000, 32767);
    float x = (float)(rx - c->cx), y = (float)(ry - c->cy), mag = sqrtf(x * x + y * y), n, k, e;
    *ox = *oy = 0.0f;
    if (outer < dead + 1000) outer = dead + 1000;
    if (mag <= (float)dead || mag < 1.0f) return;
    // Radial: the distance past the dead zone, as a share of the travel left
    // (beyond 1 in a corner: each axis is clamped below, the direction kept).
    n = (mag - dead) / (float)(outer - dead);
    e = clampi(c->curve, 50, 250) / 100.0f;
    k = e == 1.0f ? n : powf(n, e);
    x = x / mag * k;
    y = y / mag * k;
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    if (c->invert & H64_INVERT_X) x = -x;
    if (c->invert & H64_INVERT_Y) y = -y;
    *ox = x;
    *oy = y;
}

// The N64 stick's full deflection: 80 (a real N64 stick's) times `range`.
static int stick_max(int range)
{
    int max = 80 * range / 100;
    return max > 127 ? 127 : max < 1 ? 1 : max;
}

static void stick_out(const H64StickCal *c, int rx, int ry, H64Pad *out)
{
    float x, y;
    int max = stick_max(c->range);
    h64_padmap_stick_norm(c, rx, ry, &x, &y);
    out->x = (s8)(int)floorf(x * max + 0.5f);
    out->y = (s8)(int)floorf(y * max + 0.5f);
}

static s16 centred(int v, int centre) { return (s16)clampi(v - centre, -32768, 32767); }

void h64_padmap_apply(const H64PadMap *m, const H64PadInput *in, H64Pad *out)
{
    H64PadInput c = *in;
    u32 s;
    u16 b = 0;
    int i;
    // A stick used as buttons (C buttons...) is read from its calibrated centre too.
    c.lx = centred(in->lx, m->cal[0].cx);
    c.ly = centred(in->ly, m->cal[0].cy);
    c.rx = centred(in->rx, m->cal[1].cx);
    c.ry = centred(in->ry, m->cal[1].cy);
    s = h64_padmap_sources(&c);
    // The D-pad as the stick: its directions press no button.
    if (m->stick == H64_STICK_DPAD) s &= ~(BIT(H64_SRC_DUP) | BIT(H64_SRC_DDOWN) | BIT(H64_SRC_DLEFT) | BIT(H64_SRC_DRIGHT));
    for (i = 0; i < H64_N64_COUNT; i++)
        if (s & m->src[i]) b |= s_n64[i].bit;
    out->buttons = b;
    if (m->stick == H64_STICK_LEFT) stick_out(&m->cal[0], in->lx, in->ly, out);
    else if (m->stick == H64_STICK_RIGHT) stick_out(&m->cal[1], in->rx, in->ry, out);
    else if (m->stick == H64_STICK_DPAD)
    {
        // Full deflection, about 70 % on each axis in a diagonal (the stick's
        // circle, as a real stick's octagon gate); the left stick's range and inversion.
        int max = stick_max(m->cal[0].range), dx = 0, dy = 0;
        if (in->buttons & H64_XB_DPAD_LEFT) dx--;
        if (in->buttons & H64_XB_DPAD_RIGHT) dx++;
        if (in->buttons & H64_XB_DPAD_DOWN) dy--;
        if (in->buttons & H64_XB_DPAD_UP) dy++;
        if (dx && dy) max = max * 7 / 10;
        if (m->cal[0].invert & H64_INVERT_X) dx = -dx;
        if (m->cal[0].invert & H64_INVERT_Y) dy = -dy;
        out->x = (s8)(dx * max);
        out->y = (s8)(dy * max);
    }
    else
        out->x = out->y = 0;
}

const char *h64_padmap_source_name(int src) { return src >= 0 && src < H64_SRC_COUNT ? s_src[src].name : ""; }
const char *h64_padmap_source_label(int src) { return src >= 0 && src < H64_SRC_COUNT ? s_src[src].label : ""; }
const char *h64_padmap_n64_name(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].name : ""; }
const char *h64_padmap_n64_label(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].label : ""; }
u16 h64_padmap_n64_bit(int n64) { return n64 >= 0 && n64 < H64_N64_COUNT ? s_n64[n64].bit : 0; }

int h64_padmap_parse_sources(const char *text, u32 *mask)
{
    u32 m = 0;
    int any = 0;
    while (*text)
    {
        char tok[16];
        int n = 0, i;
        while (*text == ',' || *text == ' ' || *text == '\t' || *text == '+') text++;
        while (*text && *text != ',' && *text != ' ' && *text != '\t' && *text != '+')
        {
            char ch = *text++;
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (n < (int)sizeof(tok) - 1) tok[n++] = ch;
        }
        tok[n] = 0;
        if (!n) continue;
        if (!strcmp(tok, "none")) { any = 1; continue; }
        for (i = 0; i < H64_SRC_COUNT; i++)
            if (!strcmp(tok, s_src[i].name)) { m |= BIT(i); any = 1; break; }
    }
    if (!any) return -1;
    *mask = m;
    return 0;
}

void h64_padmap_format_sources(u32 mask, char *out, int size)
{
    int i, len = 0;
    if (size <= 0) return;
    out[0] = 0;
    for (i = 0; i < H64_SRC_COUNT; i++)
    {
        int n;
        if (!(mask & BIT(i))) continue;
        n = (int)strlen(s_src[i].name);
        if (len + (len ? 1 : 0) + n >= size) break;
        if (len) out[len++] = ',';
        memcpy(out + len, s_src[i].name, n + 1);
        len += n;
    }
    if (!len && size > 4) strcpy(out, "none");
}

// A whole number in [lo, hi]; returns 0 when `text` is not one.
static int parse_int(const char *text, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end || v < lo || v > hi) return 0;
    *out = (int)v;
    return 1;
}

// "none|x|y|xy" -> *invert; untouched when it is none of them.
static void parse_invert(const char *value, int *invert)
{
    if (!strcmp(value, "none")) *invert = 0;
    else if (!strcmp(value, "x")) *invert = H64_INVERT_X;
    else if (!strcmp(value, "y")) *invert = H64_INVERT_Y;
    else if (!strcmp(value, "xy")) *invert = H64_INVERT_X | H64_INVERT_Y;
}

// One stick's key ("center", "deadzone"... without the l/r); returns 1 when it is one.
static int parse_cal_key(H64StickCal *c, const char *key, const char *value)
{
    if (!strcmp(key, "center"))
    {
        int x, y;
        const char *comma = strchr(value, ',');
        char first[16];
        size_t n = comma ? (size_t)(comma - value) : 0;
        if (comma && n < sizeof(first))
        {
            memcpy(first, value, n);
            first[n] = 0;
            if (parse_int(first, -32767, 32767, &x) && parse_int(comma + 1, -32767, 32767, &y)) { c->cx = x; c->cy = y; }
        }
        return 1;
    }
    if (!strcmp(key, "deadzone")) { parse_int(value, 0, 32000, &c->deadzone); return 1; }
    if (!strcmp(key, "outer")) { parse_int(value, 16000, 32767, &c->outer); return 1; }
    if (!strcmp(key, "range")) { parse_int(value, 50, 150, &c->range); return 1; }
    if (!strcmp(key, "curve")) { parse_int(value, 50, 250, &c->curve); return 1; }
    if (!strcmp(key, "invert")) { parse_invert(value, &c->invert); return 1; }
    return 0;
}

int h64_padmap_parse_key(H64PadMap *m, const char *key, const char *value)
{
    int i;
    if (strncmp(key, "map", 3)) return 0;
    if (!strcmp(key, "map"))
    {
        if (!strcmp(value, "default")) h64_padmap_default(m);
        return 1;
    }
    if (key[3] != '_') return 0;
    key += 4;
    if (!strcmp(key, "stick"))
    {
        if (!strcmp(value, "left")) m->stick = H64_STICK_LEFT;
        else if (!strcmp(value, "right")) m->stick = H64_STICK_RIGHT;
        else if (!strcmp(value, "dpad")) m->stick = H64_STICK_DPAD;
        else if (!strcmp(value, "none")) m->stick = H64_STICK_NONE;
        return 1;
    }
    if (!strcmp(key, "drift")) { parse_int(value, 1, 30, &m->drift); return 1; }
    // Older files' single settings: both sticks.
    if (!strcmp(key, "invert") || !strcmp(key, "deadzone") || !strcmp(key, "range"))
    {
        parse_cal_key(&m->cal[0], key, value);
        parse_cal_key(&m->cal[1], key, value);
        return 1;
    }
    if ((key[0] == 'l' || key[0] == 'r') && parse_cal_key(&m->cal[key[0] == 'r'], key + 1, value)) return 1;
    for (i = 0; i < H64_N64_COUNT; i++)
        if (!strcmp(key, s_n64[i].name))
        {
            h64_padmap_parse_sources(value, &m->src[i]);
            return 1;
        }
    return 0;
}
