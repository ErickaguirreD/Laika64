// Harissa64 V2 - controller remapping: a host gamepad (Xbox 360 layout) as
// an N64 controller.
//
// Portable (no platform headers) so that the unit tests check it. The Xbox
// front end copies XINPUT_GAMEPAD into H64PadInput; its button bits are
// XInput's (H64_XB_* below, checked against XINPUT_GAMEPAD_* in main.cpp).
//
// A mapping gives each N64 button a set of sources (any of them pressed
// presses it) and picks the stick that drives the analog stick. The default
// mapping is V1's: X, Y, LT and RT all give Z, the right stick gives the C
// buttons, the left stick the analog stick.
//
// Each physical stick (left, right) has its own calibration (H64StickCal):
// its resting position is taken off first, then a radial dead zone, then the
// rest of the travel (up to `outer`) goes through the response curve and is
// scaled to the N64 stick's range. The stick driving the N64 one uses all of
// it; a stick used as buttons (C buttons) gets the centre correction.
//
// Settings keys (xb_config.cpp): map_a=xa,xb ... (one per N64 control,
// sources by name, "none" for no source), map_stick=left|right|dpad|none
// (dpad: the D-pad moves the stick and presses no button; it uses the left
// stick's range and inversion); per stick, with an l or r after "map_":
// map_lcenter=X,Y (raw resting position), map_ldeadzone=N (0..32000 raw
// units, 7000 by default), map_louter=N (16000..32767: the raw distance of
// full deflection), map_lrange=N (percent of the N64 stick's usual 80,
// 50..150), map_lcurve=N (response exponent in percent, 50..250, 100 linear),
// map_linvert=none|x|y|xy; map_drift=N (percent: the drift warning
// threshold of the calibration page). map_deadzone, map_range and map_invert
// (older files) set both sticks. map=default resets the mapping first. Ports
// 2-4 use the same keys with a p2_ / p3_ / p4_ prefix.
#ifndef H64_PADMAP_H
#define H64_PADMAP_H

#include "../common/h64_types.h"

struct H64Pad;

// Gamepad buttons (XInput's bit values).
#define H64_XB_DPAD_UP        0x0001
#define H64_XB_DPAD_DOWN      0x0002
#define H64_XB_DPAD_LEFT      0x0004
#define H64_XB_DPAD_RIGHT     0x0008
#define H64_XB_START          0x0010
#define H64_XB_BACK           0x0020
#define H64_XB_LEFT_THUMB     0x0040
#define H64_XB_RIGHT_THUMB    0x0080
#define H64_XB_LEFT_SHOULDER  0x0100
#define H64_XB_RIGHT_SHOULDER 0x0200
#define H64_XB_A              0x1000
#define H64_XB_B              0x2000
#define H64_XB_X              0x4000
#define H64_XB_Y              0x8000

// The state of one gamepad (XINPUT_GAMEPAD's fields).
struct H64PadInput
{
    u16 buttons;
    u8 lt, rt;
    s16 lx, ly, rx, ry;
};

// Sources: what a mapping can bind (BACK is the front end's and never one).
enum
{
    H64_SRC_A = 0, H64_SRC_B, H64_SRC_X, H64_SRC_Y, H64_SRC_LB, H64_SRC_RB, H64_SRC_LT, H64_SRC_RT, H64_SRC_START,
    H64_SRC_DUP, H64_SRC_DDOWN, H64_SRC_DLEFT, H64_SRC_DRIGHT, H64_SRC_LTHUMB, H64_SRC_RTHUMB,
    H64_SRC_LUP, H64_SRC_LDOWN, H64_SRC_LLEFT, H64_SRC_LRIGHT, H64_SRC_RUP, H64_SRC_RDOWN, H64_SRC_RLEFT, H64_SRC_RRIGHT,
    H64_SRC_COUNT
};

// N64 controls, in the order of the editor's list; the buttons' bits in H64Pad.buttons.
enum
{
    H64_N64_A = 0, H64_N64_B, H64_N64_Z, H64_N64_START, H64_N64_DUP, H64_N64_DDOWN, H64_N64_DLEFT, H64_N64_DRIGHT,
    H64_N64_L, H64_N64_R, H64_N64_CUP, H64_N64_CDOWN, H64_N64_CLEFT, H64_N64_CRIGHT,
    H64_N64_COUNT
};

enum { H64_STICK_LEFT = 0, H64_STICK_RIGHT, H64_STICK_DPAD, H64_STICK_NONE, H64_STICK_COUNT };
enum { H64_INVERT_X = 1, H64_INVERT_Y = 2 };

#define H64_PAD_DEADZONE_DEFAULT 7000
#define H64_PAD_RANGE_DEFAULT 100
#define H64_PAD_OUTER_DEFAULT 32767
#define H64_PAD_CURVE_DEFAULT 100
#define H64_PAD_DRIFT_DEFAULT 5
#define H64_PAD_TRIGGER_THRESHOLD 64      // a trigger counts as pressed above this
#define H64_PAD_STICK_THRESHOLD 16000     // a stick direction counts as pressed beyond this

// One physical stick's calibration (H64PadMap::cal[H64_STICK_LEFT / RIGHT]).
struct H64StickCal
{
    int cx, cy;               // its resting position (raw units), taken off first
    int deadzone;             // radial, raw units from the centre (0..32000)
    int outer;                // the raw distance that counts as full deflection (16000..32767)
    int range;                // percent of the usual full deflection (80), 50..150
    int curve;                // response exponent in percent: 100 linear, more = finer near the centre (50..250)
    int invert;               // H64_INVERT_X | H64_INVERT_Y
};

struct H64PadMap
{
    u32 src[H64_N64_COUNT];   // per N64 button: bit n = source n
    int stick;                // H64_STICK_*
    H64StickCal cal[2];       // the left and right sticks
    int drift;                // percent: the calibration page's drift warning threshold
};

void h64_padmap_default(H64PadMap *m);
void h64_padmap_cal_default(H64StickCal *c);
int h64_padmap_is_default(const H64PadMap *m);
// A raw stick position through its calibration, as -1..+1 per axis (inverted
// as set): what the N64 stick gets, before its range.
void h64_padmap_stick_norm(const H64StickCal *c, int rx, int ry, float *x, float *y);
// The sources active in *in (bit n = source n).
u32 h64_padmap_sources(const H64PadInput *in);
// *in through *m into the N64 controller *out (buttons and stick).
void h64_padmap_apply(const H64PadMap *m, const H64PadInput *in, H64Pad *out);

// Names: sources ("xa", "lt", "rup"...) for the settings files, labels
// ("A", "LT", "Right stick up"...) for the editor, N64 controls ("a",
// "cup"... / "A", "C-Up"...).
const char *h64_padmap_source_name(int src);
const char *h64_padmap_source_label(int src);
const char *h64_padmap_n64_name(int n64);
const char *h64_padmap_n64_label(int n64);
u16 h64_padmap_n64_bit(int n64);
// "xa,xx" -> *mask (unknown names skipped; "none": 0). Returns -1, *mask
// untouched, when the text names no valid source and is not "none".
int h64_padmap_parse_sources(const char *text, u32 *mask);
// mask -> "xa,xx" ("none" when empty).
void h64_padmap_format_sources(u32 mask, char *out, int size);
// Applies one settings key (without the pN_ prefix) to *m; returns 1 when the key is a mapping key.
int h64_padmap_parse_key(H64PadMap *m, const char *key, const char *value);

#endif
