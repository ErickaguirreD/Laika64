// Controller remapping (core/pif/h64_padmap.cpp): the default mapping gives
// the front end's original buttons exactly and its stick on the axes (the dead
// zone is radial now, so diagonals differ on purpose), custom mappings,
// calibration and the settings keys work.
#include <string.h>

#include "../../core/common/h64_test.h"
#include "../../core/pif/h64_padmap.h"
#include "../../core/system/h64_system.h"

// The Xbox front end's PadFromXInput before remapping existed (mapping as V1).
static s8 ref_axis(int x)
{
    const int dead = 7000;
    if (x > -dead && x < dead) return 0;
    x = x > 0 ? (x - dead) * 80 / (32767 - dead) : (x + dead) * 80 / (32768 - dead);
    return (s8)(x > 80 ? 80 : x < -80 ? -80 : x);
}

static void ref_pad(const H64PadInput *g, H64Pad *pad)
{
    u16 b = 0;
    if (g->buttons & H64_XB_A) b |= 0x8000;
    if (g->buttons & H64_XB_B) b |= 0x4000;
    if ((g->buttons & (H64_XB_X | H64_XB_Y)) || g->lt > 64 || g->rt > 64) b |= 0x2000;
    if (g->buttons & H64_XB_START) b |= 0x1000;
    if (g->buttons & H64_XB_DPAD_UP) b |= 0x0800;
    if (g->buttons & H64_XB_DPAD_DOWN) b |= 0x0400;
    if (g->buttons & H64_XB_DPAD_LEFT) b |= 0x0200;
    if (g->buttons & H64_XB_DPAD_RIGHT) b |= 0x0100;
    if (g->buttons & H64_XB_LEFT_SHOULDER) b |= 0x0020;
    if (g->buttons & H64_XB_RIGHT_SHOULDER) b |= 0x0010;
    if (g->ry > 16000) b |= 0x0008;
    if (g->ry < -16000) b |= 0x0004;
    if (g->rx < -16000) b |= 0x0002;
    if (g->rx > 16000) b |= 0x0001;
    pad->buttons = b;
    pad->x = ref_axis(g->lx);
    pad->y = ref_axis(g->ly);
}

static u32 rng(u32 *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static s16 pick_axis(u32 *s)
{
    static const s16 edges[] = { 0, 6999, 7000, 7001, -6999, -7000, -7001, 16000, 16001, -16000, -16001, 32767, -32768, -32767 };
    if (rng(s) & 1) return edges[rng(s) % (sizeof(edges) / sizeof(edges[0]))];
    return (s16)(rng(s) & 0xFFFF);
}

void test_padmap(H64TestContext *ctx)
{
    H64PadMap m, d;
    H64PadInput in;
    H64Pad got, want;
    u32 seed = 12345, mask;
    int i, same = 1;
    char text[256];

    // The default mapping: the original buttons bit for bit; the stick still
    // 0 inside the (now radial) dead zone, never the other way, and on an axis
    // the original value (to within the rounding: it used to truncate).
    h64_padmap_default(&m);
    H64_CHECK(ctx, h64_padmap_is_default(&m));
    for (i = 0; i < 20000; i++)
    {
        int dx, inside;
        in.buttons = (u16)rng(&seed);
        in.lt = (u8)(rng(&seed) & 1 ? 64 + (rng(&seed) % 3) - 1 : rng(&seed));
        in.rt = (u8)rng(&seed);
        in.lx = pick_axis(&seed);
        in.ly = (rng(&seed) & 3) ? pick_axis(&seed) : 0;   // a quarter on the X axis alone
        in.rx = pick_axis(&seed);
        in.ry = pick_axis(&seed);
        h64_padmap_apply(&m, &in, &got);
        ref_pad(&in, &want);
        inside = (s64)in.lx * in.lx + (s64)in.ly * in.ly <= (s64)H64_PAD_DEADZONE_DEFAULT * H64_PAD_DEADZONE_DEFAULT;
        dx = got.x - want.x;
        if (got.buttons != want.buttons || (inside && (got.x || got.y)) || (got.x && (got.x > 0) != (in.lx > 0)) ||
            (got.y && (got.y > 0) != (in.ly > 0)) || (!in.ly && (got.y || dx < -1 || dx > 1)))
        { same = 0; break; }
    }
    H64_CHECK(ctx, same);

    // A custom mapping: A on X, B on A, Z only on RT, C buttons on the D-pad,
    // the analog stick on the right stick with a smaller dead zone.
    m.src[H64_N64_A] = 1u << H64_SRC_X;
    m.src[H64_N64_B] = 1u << H64_SRC_A;
    m.src[H64_N64_Z] = 1u << H64_SRC_RT;
    m.src[H64_N64_DUP] = m.src[H64_N64_DDOWN] = m.src[H64_N64_DLEFT] = m.src[H64_N64_DRIGHT] = 0;
    m.src[H64_N64_CUP] = 1u << H64_SRC_DUP;
    m.src[H64_N64_CDOWN] = 1u << H64_SRC_DDOWN;
    m.src[H64_N64_CLEFT] = 1u << H64_SRC_DLEFT;
    m.src[H64_N64_CRIGHT] = 1u << H64_SRC_DRIGHT;
    m.stick = H64_STICK_RIGHT;
    m.cal[1].deadzone = 0;
    H64_CHECK(ctx, !h64_padmap_is_default(&m));
    memset(&in, 0, sizeof(in));
    in.buttons = H64_XB_X | H64_XB_DPAD_UP | H64_XB_BACK;
    in.lt = 255;
    in.rx = 32767;
    in.ry = -32768;
    in.lx = 32767;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.buttons, 0x8000 | 0x0008);
    H64_CHECK_EQ(ctx, got.x, 80);
    H64_CHECK_EQ(ctx, got.y, (u64)(s64)-80);
    in.buttons = H64_XB_A;
    in.rt = 200;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.buttons, 0x4000 | 0x2000);
    m.stick = H64_STICK_NONE;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.x == 0 && got.y == 0);
    m.stick = H64_STICK_LEFT;
    m.cal[0].range = 150;
    m.cal[0].deadzone = 7000;
    in.lx = 32767;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.x, 120);

    // Calibration: the resting position comes off first (a drifting stick at
    // rest reads 0), the dead zone is radial, a worn stick's shorter travel
    // (outer) still reaches full deflection, the curve softens the middle.
    h64_padmap_default(&m);
    memset(&in, 0, sizeof(in));
    m.cal[0].cx = 5000;
    m.cal[0].cy = -4000;
    in.lx = 5000;
    in.ly = -4000;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.x == 0 && got.y == 0);
    m.cal[0].cx = m.cal[0].cy = 0;
    in.lx = 6000;   // 6000 on each axis: 8485 from the centre - past the 7000 radial dead zone, though neither axis is alone
    in.ly = 6000;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.x > 0 && got.y > 0);
    in.lx = 26000;
    in.ly = 0;
    m.cal[0].outer = 26000;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.x, 80);
    m.cal[0].outer = 32767;
    in.lx = 19884;   // halfway through the travel past the dead zone
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.x, 40);
    m.cal[0].curve = 200;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.x, 20);
    // The right stick's centre also applies when it gives the C buttons.
    h64_padmap_default(&m);
    memset(&in, 0, sizeof(in));
    in.rx = 17000;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.buttons, 0x0001);
    m.cal[1].cx = 9000;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.buttons, 0);

    // Settings keys.
    H64_CHECK(ctx, h64_padmap_parse_sources("xa,XX, lt", &mask) == 0);
    H64_CHECK_EQ(ctx, mask, (1u << H64_SRC_A) | (1u << H64_SRC_X) | (1u << H64_SRC_LT));
    h64_padmap_format_sources(mask, text, sizeof(text));
    H64_CHECK(ctx, !strcmp(text, "xa,xx,lt"));
    mask = 7;
    H64_CHECK(ctx, h64_padmap_parse_sources("bogus", &mask) == -1 && mask == 7);
    H64_CHECK(ctx, h64_padmap_parse_sources("none", &mask) == 0 && mask == 0);
    h64_padmap_format_sources(0, text, sizeof(text));
    H64_CHECK(ctx, !strcmp(text, "none"));

    h64_padmap_default(&d);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_a", "xb,rt"));
    H64_CHECK_EQ(ctx, d.src[H64_N64_A], (1u << H64_SRC_B) | (1u << H64_SRC_RT));
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_b", "nonsense"));     // ignored
    H64_CHECK_EQ(ctx, d.src[H64_N64_B], 1u << H64_SRC_B);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_stick", "right"));
    H64_CHECK_EQ(ctx, d.stick, H64_STICK_RIGHT);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_stick", "sideways"));  // ignored
    H64_CHECK_EQ(ctx, d.stick, H64_STICK_RIGHT);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_deadzone", "abc"));    // ignored
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_deadzone", "99999"));  // out of range: ignored
    H64_CHECK_EQ(ctx, d.cal[0].deadzone, 7000);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_deadzone", "5000"));   // an older file's: both sticks
    H64_CHECK(ctx, d.cal[0].deadzone == 5000 && d.cal[1].deadzone == 5000);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_rdeadzone", "3000"));
    H64_CHECK(ctx, d.cal[0].deadzone == 5000 && d.cal[1].deadzone == 3000);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_lcenter", "-1200,800"));
    H64_CHECK(ctx, d.cal[0].cx == -1200 && d.cal[0].cy == 800);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_lcenter", "12"));       // ignored: needs X,Y
    H64_CHECK(ctx, d.cal[0].cx == -1200 && d.cal[0].cy == 800);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_rcurve", "150") && d.cal[1].curve == 150);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_louter", "28000") && d.cal[0].outer == 28000);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_drift", "8") && d.drift == 8);
    H64_CHECK(ctx, !h64_padmap_parse_key(&d, "mapx", "1"));
    H64_CHECK(ctx, !h64_padmap_parse_key(&d, "smooth", "1"));
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_unknown", "xa") == 0);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_invert", "y"));
    H64_CHECK(ctx, d.cal[0].invert == H64_INVERT_Y && d.cal[1].invert == H64_INVERT_Y);
    H64_CHECK(ctx, !h64_padmap_is_default(&d));
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map_stick", "dpad"));
    H64_CHECK_EQ(ctx, d.stick, H64_STICK_DPAD);
    H64_CHECK(ctx, h64_padmap_parse_key(&d, "map", "default"));
    H64_CHECK(ctx, h64_padmap_is_default(&d));

    // The D-pad as the stick (its directions press no button), inverted axes.
    h64_padmap_default(&m);
    m.stick = H64_STICK_DPAD;
    memset(&in, 0, sizeof(in));
    in.buttons = H64_XB_DPAD_UP | H64_XB_A;
    in.lx = 32767;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK_EQ(ctx, got.buttons, 0x8000);
    H64_CHECK(ctx, got.x == 0 && got.y == 80);
    in.buttons = H64_XB_DPAD_DOWN | H64_XB_DPAD_LEFT;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.buttons == 0 && got.x == -56 && got.y == -56);
    m.cal[0].invert = H64_INVERT_X | H64_INVERT_Y;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.x == 56 && got.y == 56);
    m.stick = H64_STICK_LEFT;
    m.cal[0].invert = H64_INVERT_Y;
    in.buttons = H64_XB_DPAD_UP;
    in.lx = 32767;
    in.ly = 32767;
    h64_padmap_apply(&m, &in, &got);
    H64_CHECK(ctx, got.buttons == 0x0800 && got.x == 80 && got.y == -80);

    // Every name round-trips.
    for (i = 0; i < H64_SRC_COUNT; i++)
    {
        H64_CHECK(ctx, h64_padmap_parse_sources(h64_padmap_source_name(i), &mask) == 0 && mask == (1u << i));
        H64_CHECK(ctx, h64_padmap_source_label(i)[0] != 0);
    }
}
