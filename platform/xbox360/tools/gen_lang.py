#!/usr/bin/env python3
"""Harissa64 V2 - builds the menu text tables (platform/xbox360/xb_lang_table.h).

Reads platform/xbox360/lang/<code>.txt (UTF-8, KEY = text lines; en.txt is the
reference) and checks every language before writing anything:
  - the same keys as English, each text non-empty;
  - the same %d / %s in the same order (they are filled by sprintf);
  - button prompts ("A:Select|B:Back") with the same buttons as English, and
    labels without ':' or '|';
  - every character in the menu font (xb_ui_atlas.h);
  - the texts fit the places they are shown in (widths measured with the
    atlas's advances, as the console draws them).
Arabic is turned into its joined presentation forms here (isolated, initial,
medial, final letters, lam-alef ligatures), in logical order: the UI shows
lines holding Arabic right to left.

Run from anywhere:
    python platform/xbox360/tools/gen_lang.py [--check]
--check writes nothing: it fails when a text is wrong or xb_lang_table.h is
not what the texts give (tests/scripts/run_all.ps1 runs it).
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
LANG_DIR = os.path.join(ROOT, "platform", "xbox360", "lang")
ATLAS = os.path.join(ROOT, "platform", "xbox360", "xb_ui_atlas.h")
OUT = os.path.join(ROOT, "platform", "xbox360", "xb_lang_table.h")
# The order of the language list (and of LANG_* in xb_lang.h).
LANGS = ["en", "es", "de", "fr", "ar", "pt", "ru"]

# ---- Arabic shaping ----
# Letter -> (isolated, final, initial, medial) presentation forms; None where
# the letter has no such form (right-joining letters do not join to the next).
FORMS = {
    0x0621: (0xFE80, None, None, None),
    0x0622: (0xFE81, 0xFE82, None, None),
    0x0623: (0xFE83, 0xFE84, None, None),
    0x0624: (0xFE85, 0xFE86, None, None),
    0x0625: (0xFE87, 0xFE88, None, None),
    0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),
    0x0627: (0xFE8D, 0xFE8E, None, None),
    0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92),
    0x0629: (0xFE93, 0xFE94, None, None),
    0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98),
    0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),
    0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0),
    0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),
    0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8),
    0x062F: (0xFEA9, 0xFEAA, None, None),
    0x0630: (0xFEAB, 0xFEAC, None, None),
    0x0631: (0xFEAD, 0xFEAE, None, None),
    0x0632: (0xFEAF, 0xFEB0, None, None),
    0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8),
    0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0),
    0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8),
    0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0),
    0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8),
    0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0),
    0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8),
    0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x0648: (0xFEED, 0xFEEE, None, None),
    0x0649: (0xFEEF, 0xFEF0, None, None),
    0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}
# Lam followed by one of these alefs: one ligature (isolated, final).
LAM_ALEF = {0x0622: (0xFEF5, 0xFEF6), 0x0623: (0xFEF7, 0xFEF8), 0x0625: (0xFEF9, 0xFEFA), 0x0627: (0xFEFB, 0xFEFC)}


def joins_next(c):
    """The letter connects to the letter after it (dual-joining)."""
    return c in FORMS and FORMS[c][2] is not None


def shape_arabic(text):
    cps = [ord(ch) for ch in text]
    out = []
    i = 0
    while i < len(cps):
        c = cps[i]
        if c not in FORMS:
            out.append(c)
            i += 1
            continue
        prev = cps[i - 1] if i > 0 else None
        joined_before = prev is not None and joins_next(prev)
        if c == 0x0644 and i + 1 < len(cps) and cps[i + 1] in LAM_ALEF:
            out.append(LAM_ALEF[cps[i + 1]][1 if joined_before else 0])
            i += 2
            continue
        nxt = cps[i + 1] if i + 1 < len(cps) else None
        joined_after = joins_next(c) and nxt in FORMS
        iso, fin, ini, med = FORMS[c]
        if joined_before and joined_after:
            f = med
        elif joined_before:
            f = fin
        elif joined_after:
            f = ini
        else:
            f = iso
        out.append(f if f is not None else iso)
        i += 1
    return "".join(chr(c) for c in out)


# ---- Reading ----
def read_lang(code):
    path = os.path.join(LANG_DIR, code + ".txt")
    keys, texts = [], {}
    for n, line in enumerate(open(path, encoding="utf-8-sig"), 1):
        line = line.rstrip("\r\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if " = " not in line:
            raise SystemExit("%s:%d: expected KEY = text" % (path, n))
        k, v = line.split(" = ", 1)
        k, v = k.strip(), v.strip().replace("\\n", "\n")
        if not re.match(r"^[A-Z][A-Z0-9_]*$", k):
            raise SystemExit("%s:%d: bad key %r" % (path, n, k))
        if k in texts:
            raise SystemExit("%s:%d: %s given twice" % (path, n, k))
        keys.append(k)
        texts[k] = v
    return keys, texts


def read_atlas():
    """Code points and advances (em units) of both fonts, from xb_ui_atlas.h."""
    src = open(ATLAS, encoding="utf-8").read()
    base = float(re.search(r"#define UI_ATLAS_BASE ([0-9.]+)", src).group(1))
    adv = [{}, {}]
    font = -1
    for line in src.splitlines():
        if line.startswith("  {   // "):
            font += 1
        m = re.match(r"\s*\{ \d+, \d+, \d+, \d+, [-0-9.]+f, [-0-9.]+f, ([-0-9.]+)f \},   // U\+([0-9A-F]{4})", line)
        if m and 0 <= font < 2:
            adv[font][int(m.group(2), 16)] = float(m.group(1))
    return base, adv


def main():
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        print("ERROR:", msg)

    base, adv = read_atlas()

    def width(text, size, bold):
        a = adv[1 if bold else 0]
        return max(sum(a.get(ord(ch), a[ord("?")]) for ch in line) * size / base for line in text.split("\n"))

    btn_r, pill_h, hint = 15.0, 26.0, 19.0

    def footer_width(hints):
        x, first = 0.0, True
        for item in hints.split("|"):
            label = None
            if ":" in item:
                item, label = item.split(":", 1)
            if not first:
                x += 30
            first = False
            btns = item.split("/")
            for i, b in enumerate(btns):
                if b in ("A", "B", "X", "Y", "DPAD"):
                    x += btn_r * 2
                else:
                    x += max(width(b, 14, True) + 22, pill_h + 8)
                if i + 1 < len(btns):
                    x += 6
            if label:
                x += 10 + width(label, hint, False)
        return x

    def value_width(v):
        # draw_value: "< text >" as the text between two chevrons.
        return width(v, 21, True) + 2 * (width("›", 30, True) + 10)

    en_keys, en = read_lang("en")
    langs = {}
    for code in LANGS:
        keys, t = read_lang(code)
        name = code + ".txt"
        for k in en_keys:
            if k not in t:
                fail("%s: missing %s" % (name, k))
        for k in keys:
            if k not in en:
                fail("%s: %s is not in en.txt" % (name, k))
        for k in en_keys:
            if k not in t:
                continue
            v = t[k]
            if re.findall(r"%[a-z]", v) != re.findall(r"%[a-z]", en[k]) or v.count("%%") != en[k].count("%%"):
                fail("%s: %s must keep %s in the same order: %r" % (name, k, re.findall(r"%[a-z]", en[k]), v))
            if k.startswith("FOOT_"):
                eb = [it.split(":", 1)[0] for it in en[k].split("|")]
                vb = [it.split(":", 1)[0] for it in v.split("|")]
                if eb != vb:
                    fail("%s: %s must keep the buttons %s: %r" % (name, k, eb, v))
                for it in v.split("|"):
                    if it.count(":") > 1:
                        fail("%s: %s: a label holds ':' (%r)" % (name, k, it))
        if code == "ar":
            t = dict((k, shape_arabic(v)) for k, v in t.items())
        for k, v in t.items():
            for ch in v:
                if ch != "\n" and ord(ch) not in adv[0]:
                    fail("%s: %s: %r (U+%04X) is not in the menu font" % (name, k, ch, ord(ch)))
        langs[code] = t
    if not ok:
        sys.exit(1)

    # ---- Widths (the layouts of xb_menu.cpp and main.cpp) ----
    def check(code, what, w, limit):
        if w > limit + 0.5:
            fail("%s: %s is %.0f pixels wide, at most %.0f" % (code, what, w, limit))

    for code in LANGS:
        t = langs[code]
        names = [langs[c]["LANG_NAME"] for c in LANGS]
        onoff = [t["ON"], t["OFF"]]
        menus = {
            "settings": [("SYSTEM", [], 1), ("GRAPHICS", [], 1), ("SCREEN", [], 1), ("SOUND", [], 1),
                         ("CONTROLS", [t["CUSTOM"], t["DEFAULT"]], 0),
                         ("INTERFACE", [], 1), ("SAVE_ALL", [], 1), ("SAVE_GAME", [], 1)],
            "categories": [("CAT_NEW", [], 1), ("CAT_FIELD", [t[k] for k in ("CAT_GENRE", "CAT_REGION", "CAT_PLAYERS", "CAT_DEVELOPER",
                                                                                 "CAT_PUBLISHER", "CAT_YEAR")], 1),
                           # a value is cut to 260 pixels (catedit_values)
                           ("CAT_VALUE", [t["CAT_NO_DATA"], "W" * 11], 1), ("CAT_SAVE", [], 1), ("CAT_DELETE", [], 1)],
            "interface": [("DESIGN", [t["DESIGN_DETAILED"], t["DESIGN_COVERS"], t["DESIGN_BIG"], t["DESIGN_CAROUSEL"]], 1), ("SKINS", [t["SKIN_NONE"]], 1),
                          ("BG_EFFECT", [t[k] for k in ("OFF", "FX_WAVES", "FX_PULSE", "FX_RAIN", "FX_VORTEX", "FX_RIPPLES", "FX_SILK",
                                                        "FX_EMBERS", "FX_GRID", "FX_UNDERWATER", "FX_ENERGY", "FX_AURORA",
                                                        "FX_LIQUID", "FX_PARTICLES", "FX_SMOKE", "FX_XMB", "FX_PS4_FLOW",
                                                        "FX_PS4_BOKEH",
                                 "FX_NEBULA", "FX_FIRE_DUST", "FX_LAVA", "FX_PRISM", "FX_TOPO", "FX_HEX", "FX_SPIRAL", "FX_INK", "FX_HOLO", "FX_FRACTAL", "FX_BURST", "FX_WARP")], 1),
                          ("LANGUAGE", names, 1), ("ART_DOWNLOADS", onoff, 1),
                          ("SEL_COLOR", [t[k] for k in ("COL_BLUE", "COL_RED", "COL_ORANGE", "COL_YELLOW", "COL_GREEN", "COL_TEAL",
                                                        "COL_PURPLE", "COL_PINK", "COL_WHITE")], 1),
                          # a folder's name is cut to 230 pixels (settings_values)
                          ("WALLPAPERS", [t["OFF"], t["WALL_NONE_FOUND"], "W" * 6 + " (99/99)"], 1),
                          ("SAVER", [t[k] for k in ("OFF", "BG_EFFECT", "WALLPAPERS", "SAVER_LOGO", "SAVER_CLOCK")], 1),
                          ("BORDERS", [], 1)],
            "saver": [("SAVER_MODE", [t[k] for k in ("OFF", "BG_EFFECT", "WALLPAPERS", "SAVER_LOGO", "SAVER_CLOCK")], 1),
                      ("SAVER_TIME", ["30 min"], 1), ("SAVER_PREVIEW", [], 1)],
            "system": [("CPU_NEXT", [t["RECOMPILER"], t["INTERPRETER"]], 1), ("RSP_NEXT", ["HLE"], 1),
                       ("PAD_LOSS", [t["OFF"], t["PAD_LOSS_SAVE"], t["PAD_LOSS_PAUSE"]], 1), ("AUTOSAVE_SLOT", ["9"], 1),
                       ("BATTERY_WARN", [t["OFF"], "50%"], 1),
                       # a game's name is cut to 230 pixels (start_rom_text)
                       ("START_ROM", [t["OFF"], t["START_LAST"], "W" * 10], 1)],
            "sound": [("VOLUME", [], 1), ("AUDIO_MARGIN", ["500 ms"], 1), ("UI_SOUNDS", onoff, 1),
                      ("SFX_STYLE", [t[k] for k in ("STYLE_GLASS", "STYLE_SOFT", "STYLE_RETRO", "STYLE_DIGITAL")], 1),
                      # a track's name is cut to 220 pixels (settings_values)
                      ("MUSIC", [t["MUSIC_ORDER"], t["MUSIC_RANDOM"], t["MUSIC_NONE"], "W" * 9], 1),
                      ("AUDIO_OUTPUT", [t["AUDIO_STEREO"], t["AUDIO_MONO"]], 1)],
            "graphics": [("PRESET", [t["PRESET_ORIGINAL"], t["PRESET_ENHANCED"], t["PRESET_SMOOTH"], t["PRESET_CRT"], t["CUSTOM"]], 1),
                         ("TEXFILTER", [t["FILTER_3POINT"], t["FILTER_BILINEAR"], t["FILTER_NEAREST"]], 1), ("FXAA", onoff, 1),
                         ("SHARPEN", [t["OFF"], t["LOW"], t["HIGH"]], 1), ("BLUR", [t["OFF"], t["SOFT"], t["STRONG"]], 1),
                         ("SHOW_FPS", [t["OFF"], t["FPS_ONLY"], t["FPS_CPU"], t["FPS_ALL"], t["FPS_GPU"]], 1)],
            "screen": [("RESOLUTION", [t["RES_NATIVE"], "HD 1280x720 (16:9)"], 1), ("STRETCH", onoff, 1), ("PIC_CAL", [], 1),
                       ("BRIGHTNESS", [t["NORMAL"], t["LIGHTER"], t["MUCH_LIGHTER"]], 1), ("VSYNC", onoff, 1), ("ANTIFLICKER", onoff, 1),
                       ("SCREEN_EFFECT", [t["SCREEN_NONE"], t["SCANLINES"], t["CRT"], t["CRT_CURVED"], t["LCD_GRID"], t["LIGHT_SCANLINES"]], 1)],
            "cal": [("PIC_SIZE", ["100%"], 1), ("PIC_LR", ["+160"], 1), ("PIC_UD", ["+120"], 1)],
            "pause": [("RESUME", [], 1), ("STATES", [], 1), ("SETTINGS", [], 1),
                      ("RESET_GAME", [], 1), ("BACK_TO_LIST", [], 1), ("QUIT_DASHBOARD", [], 1)],
            "states": [("STATE_SLOT", ["9", "9 · " + t["AUTO_TAG"]], 1), ("LOAD_STATE", [], 1), ("SAVE_STATE", [], 1)],
        }
        # UiMenuDraw: the 690-pixel side bar, labels 102 pixels in (after the rows' symbols), values 40
        # from the right, some room between; the page's title centred under the logo, in capitals.
        for menu, rows in menus.items():
            for key, values, chev in rows:
                lw = width(t[key], 22, True)
                vw = max([value_width(v) if chev else width(v, 21, False) for v in values] + [0])
                check(code, "%s row %s with its widest value" % (menu, key), lw + vw + (24 if values else 0), 690 - 102 - 40)
        for key in ("SETTINGS", "GRAPHICS", "SCREEN", "PIC_CAL", "SOUND", "SYSTEM", "CONTROLS", "PAUSED", "INTERFACE", "SKINS",
                    "VOLUME", "STATES", "WALLPAPERS", "SAVER", "BORDERS"):
            check(code, "title " + key + " in capitals", width(t[key].upper(), 30, True), 690 - 80)
        # Volumen page (volume_draw): each bar's label and its own "100%" at
        # the row's two ends, 660 - 2*44 wide.
        for key in ("VOLUME_GAME", "VOLUME_MUSIC", "UI_SOUNDS"):
            check(code, "volume row " + key, width(t[key], 20, True) + width("100%", 20, True) + 24, 660 - 88)
        # Background effect page (fxpage_draw): a 600-pixel panel, each row's label at
        # its left (20, bold) and its value or "100%" at its right, 600 - 2*44 wide.
        fxvals = [t[k] for k in ("OFF", "FX_WAVES", "FX_PULSE", "FX_RAIN", "FX_VORTEX", "FX_RIPPLES", "FX_SILK", "FX_EMBERS",
                                 "FX_GRID", "FX_UNDERWATER", "FX_ENERGY", "FX_AURORA", "FX_LIQUID", "FX_PARTICLES",
                                 "FX_SMOKE", "FX_XMB", "FX_PS4_FLOW", "FX_PS4_BOKEH",
                                 "FX_NEBULA", "FX_FIRE_DUST", "FX_LAVA", "FX_PRISM", "FX_TOPO", "FX_HEX", "FX_SPIRAL", "FX_INK", "FX_HOLO", "FX_FRACTAL", "FX_BURST", "FX_WARP")]
        check(code, "effect page row FX_EFFECT", width(t["FX_EFFECT"], 20, True) + max(width(v, 20, True) for v in fxvals) + 60, 600 - 88)
        for key, values in (("FX_COLOR", [t["FX_ORIGINAL"], t["OFF"]]), ("FX_OPACITY", ["200%"]), ("FX_SPEED", ["300%"]), ("FX_RESET", [])):
            check(code, "effect page row " + key, width(t[key].replace("%d", "3"), 20, True) + max([width(v, 20, True) for v in values] + [0]) + 24, 600 - 88)
        check(code, "title BG_EFFECT in capitals", width(t["BG_EFFECT"].upper(), 30, True), 600 - 80)
        # Wallpapers and Borders pages (wallpage_draw, borderpage_draw): the same
        # 600-pixel panel, a row's label (20, bold) and its "< value >" at its ends.
        def chev(v):
            return width("< " + v + " >", 20, True)
        panel_rows = [
            # a picture's name and "(99/99)" are cut to 300 pixels (wall_choice_text)
            ("WALL_CUSTOM", [t["OFF"], t["WALL_NONE_FOUND"], "W" * 11 + " (99/99)"]),
            ("WALL_FIT", [t[k] for k in ("FIT_FILL", "FIT_FIT", "FIT_STRETCH", "FIT_CENTER", "FIT_TILE")]),
            ("WALL_INTERVAL", ["10 min", t["WALL_NEVER"]]),
            ("WALL_TRANSITION", [t[k] for k in ("TR_FADE", "TR_SLIDE", "TR_ZOOM", "TR_WIPE", "TR_BLINDS", "TR_RANDOM")]),
            ("WALL_ORDER", [t["ORDER_NAME"], t["ORDER_RANDOM"]]),
            ("BORDER_WIDTH", [t["OFF"], "4 px"]),
            ("BORDER_GLOW", [t["GLOW_PULSE"], t["GLOW_STEADY"], t["OFF"]]),
            ("PANEL_CORNERS", [t[k] for k in ("CORNER_SQUARE", "CORNER_SOFT", "CORNER_ROUND", "CORNER_ROUNDER")]),
        ]
        for key, values in panel_rows:
            check(code, "panel row " + key, width(t[key], 20, True) + max(chev(v) for v in values) + 24, 600 - 88)
        # the wallpapers' preview: its hints wrapped in 3 lines of the 578-pixel box less 60
        for key in ("WALL_HINT", "WALL_OFF_HINT"):
            check(code, key + " in 3 lines", width(t[key], 19, False), 3 * (578 - 60) * 0.9)
        check(code, "panel row BORDER_COLOR", width(t["BORDER_COLOR"], 20, True) + width(t["COL_SELECTION"], 19, False) + 24, 600 - 88)
        for key in ("WALL_DIM", "BORDER_OPACITY", "PANEL_OPACITY"):
            check(code, "panel bar " + key, width(t[key], 20, True) + width("100%", 20, True) + 24, 600 - 88)
        for key in ("FOOT_SELECT", "FOOT_PAUSED"):
            check(code, key, footer_width(t[key]), 660 - 80)
        # Controls page: 352-pixel columns, text 24 in, prompts 18 from the right.
        col = 352 - 24 - 18 - 12
        dirs = {"N64_DUP": "DIR_UP", "N64_DDOWN": "DIR_DOWN", "N64_DLEFT": "DIR_LEFT", "N64_DRIGHT": "DIR_RIGHT",
                "N64_CUP": "DIR_UP", "N64_CDOWN": "DIR_DOWN", "N64_CLEFT": "DIR_LEFT", "N64_CRIGHT": "DIR_RIGHT"}
        for key, d in dirs.items():
            pad = ("DPAD:" if key.startswith("N64_D") else "RS:") + t[d]
            check(code, "controls row %s with its default input" % key, width(t[key], 20, True) + footer_width(pad), col)
            check(code, "controls row %s while waiting" % key, width(t[key], 20, True) + width(t["PRESS_INPUT"], 18, True), col)
        for key, values in (("ANALOG_STICK", [t["STICK_LEFT"], t["STICK_RIGHT"], t["STICK_DPAD"], t["OFF"]]),
                            ("CALIBRATION", []), ("RUMBLE", [t["ON"], t["OFF"]]),
                            ("SCREENSHOT_COMBO", [t["ON"], t["OFF"], "START"]),
                            ("RESET_DEFAULT", []), ("AUTO_CONFIG", [])):
            check(code, "controls row %s with its widest value" % key,
                  width(t[key], 20, True) + max([value_width(v) for v in values] + [0]), col)
        for key in ("HEAD_BUTTONS", "HEAD_DPAD", "HEAD_CBUTTONS", "HEAD_STICK"):
            check(code, key, width(t[key], 15, True), 330)
        longest_n64 = max([t[k] for k in dirs] + ["START"], key=lambda s: width(s, 20, True))
        longest_src = max([t[k] for k in t if k.startswith("SRC_")], key=lambda s: width(s, 18, True))
        foot = 1140 - 80
        check(code, "FOOT_CTL_BUTTON and BACK_RESERVED", footer_width(t["FOOT_CTL_BUTTON"]) + 30 + width(t["BACK_RESERVED"], 16, False), foot)
        check(code, "FOOT_CTL_OTHER and BACK_RESERVED", footer_width(t["FOOT_CTL_OTHER"]) + 30 + width(t["BACK_RESERVED"], 16, False), foot)
        check(code, "FOOT_CTL_VALUE and BACK_RESERVED", footer_width(t["FOOT_CTL_VALUE"]) + 30 + width(t["BACK_RESERVED"], 16, False), foot)
        check(code, "FOOT_CTL_OTHER and AUTO_DONE", footer_width(t["FOOT_CTL_OTHER"]) + 30 + width(t["AUTO_DONE"], 18, True), foot)
        note = t["INPUT_ADDED"].replace("%s", longest_n64, 1).replace("%s", longest_src, 1)
        check(code, "FOOT_CTL_BUTTON and the longest INPUT_ADDED note", footer_width(t["FOOT_CTL_BUTTON"]) + 30 + width(note, 18, True), foot)
        # Stick calibration page (calib_draw): title and stick selector on top,
        # the readings column 280 wide, the settings column 370, the result card's
        # two columns 293, the test card 322 inside.
        check(code, "CALIBRATION title and the stick selector", width(t["CALIBRATION"], 30, True) + 40 +
              max(width(t["CAL_LEFT"], 22, True), width(t["CAL_RIGHT"], 22, True)) + 32 + footer_width("LB") + footer_width("RB"), 1140 - 80)
        mid = [("CAL_XPOS", "-0.00"), ("CAL_YPOS", "-0.00"), ("CAL_DIST", "0.000"), ("CAL_OUT", "0.000"),
               ("CAL_REST", "-0.00, -0.00"), ("CAL_DRIFT", "0.000"), ("CAL_STABILITY", "0.000"),
               ("CAL_RANGE_X", "-1.00 / -1.00"), ("CAL_RANGE_Y", "-1.00 / -1.00"), ("CAL_CIRC", "100%"), ("CAL_JUMPS", "99")]
        mid += [("DEAD_ZONE", "40%"), ("CAL_STATE", max(t["CAL_ACTIVE"], t["CAL_IGNORED"], key=lambda v: width(v, 18, True)))]
        for key, sample in mid:
            check(code, "calibration readings row " + key, width(t[key], 16, False) + 12 + width(sample, 18, True), 280)
        for key in ("CAL_STATUS_OK", "CAL_STATUS_DRIFT"):
            check(code, "calibration status " + key, width(t[key], 18, True), 280)
        for key in ("CAL_HEAD_LIVE", "CAL_HEAD_DIAG", "CAL_HEAD_SET"):
            check(code, key, width(t[key], 15, True), 270)
        for key, values in (("DEAD_ZONE", ["40%"]), ("SENSITIVITY", ["150%"]), ("CAL_CURVE", ["2.5"]),
                            ("INVERT", [t["OFF"], "X", "Y", t["X_AND_Y"]]), ("CAL_DRIFT_THR", ["30%"]),
                            ("CAL_RAW", [t["ON"], t["OFF"]]), ("CAL_DIAG", []), ("CAL_WIZARD", []), ("RESET_DEFAULT", [])):
            check(code, "calibration settings row " + key,
                  width(t[key], 19, True) + max([value_width(v) for v in values] + [0]) + 22 + 14 + 12, 370)
        check(code, "calibration settings row CAL_CENTER", width(t["CAL_CENTER"], 19, True) + width("-0.00, -0.00", 21, False) + 48, 370)
        for key, sample in (("CAL_REST", "-0.00, -0.00"), ("CAL_DRIFT", "0.000"), ("CAL_STABILITY", "0.000"),
                            ("CAL_SUGGEST", "40%"), ("CAL_TRAVEL", "100%"), ("CAL_CIRC", "100%")):
            check(code, "calibration results row " + key, width(t[key], 16, False) + 12 + width(sample, 18, True), 293)
        for key in ("CAL_WARN_RANGE", "CAL_WARN_ASYM", "CAL_WARN_CENTER", "CAL_WARN_NONE", "CAL_LEFT", "CAL_RIGHT"):
            check(code, "calibration results " + key, width(t[key], 16, True), 293)
        for key in ("CAL_CENTER", "CAL_DIAG", "CAL_WIZARD"):
            check(code, "calibration test card title " + key, width(t[key], 22, True), 322)
        check(code, "CAL_STEP", width(t["CAL_STEP"] % (3, 3), 16, False), 322)
        check(code, "CAL_COVERAGE", width(t["CAL_COVERAGE"] % 100, 16, False), 322)
        check(code, "CAL_NO_PAD", width(t["CAL_NO_PAD"] % 4, 18, True), 322)
        check(code, "CAL_NOTE", width(t["CAL_NOTE"], 15, False), 1100)
        for key in ("CAL_RELEASE", "CAL_CIRCLE_L", "CAL_CIRCLE_R"):
            for word in t[key].split():
                check(code, "a word of " + key, width(word, 20, False), 322)
        for key in ("FOOT_CAL", "FOOT_CAL_TASK", "FOOT_CAL_RESULT"):
            check(code, key, footer_width(t[key]), 1140 - 80)
        # Its notes take CAL_NOTE's line, above the prompts, for a few seconds.
        for key in ("CAL_MOVED", "CAL_APPLIED", "CAL_CENTER_SET", "CAL_DIAG_DONE"):
            check(code, "calibration note " + key, width(t[key], 18, True), 1100)
        # Auto setup card (ctl_auto_draw): 760 wide, 40 in at both sides.
        card = 760 - 80
        check(code, "AUTO_CONFIG and AUTO_STEP", width(t["AUTO_CONFIG"], 30, True) + 24 + width(t["AUTO_STEP"] % (14, 14), 18, False), card)
        check(code, "AUTO_ASK", width(t["AUTO_ASK"], 24, True), card)
        check(code, "AUTO_ASK_NOTE", width(t["AUTO_ASK_NOTE"] % 4, 18, False), card)
        check(code, "FOOT_AUTO_ASK", footer_width(t["FOOT_AUTO_ASK"]), card)
        set_note = t["INPUT_SET"].replace("%s", longest_n64, 1).replace("%s", longest_src, 1)
        check(code, "the longest INPUT_SET note and FOOT_CANCEL", width(set_note, 18, True) + 30 + footer_width(t["FOOT_CANCEL"]), card)
        check(code, "PRESS_FOR, its bar and FOOT_CANCEL",
              width(t["PRESS_FOR"].replace("%s", longest_n64), 20, True) + 40 + 220 + footer_width(t["FOOT_CANCEL"]), foot)
        check(code, "PLAYER, USED_WHEN", width(t["USED_WHEN"], 16, False), 700)
        # Quit dialog, browser, picker, messages.
        # confirm_ask sizes its panel to the text: only keep it well inside the screen.
        check(code, "QUIT_TITLE", width(t["QUIT_TITLE"], 30, True), 1000)
        check(code, "QUIT_ASK", width(t["QUIT_ASK"], 22, False), 1000)
        check(code, "ART_ASK", width(t["ART_ASK"], 30, True), 1000)
        check(code, "LIST_ASK", width(t["LIST_ASK"], 30, True), 1000)
        check(code, "RESET_ASK", width(t["RESET_ASK"], 30, True), 1000)
        check(code, "FOOT_RESET_ASK", footer_width(t["FOOT_RESET_ASK"]), 1000)
        # The States page's empty picture box (UiMenuThumb: 4:3, about 315 wide under 3 rows).
        check(code, "NO_STATE in the empty picture box", width(t["NO_STATE"] % 9, 18, False), 300)
        check(code, "FOOT_ART_ASK", footer_width(t["FOOT_ART_ASK"]), 640 - 96)
        check(code, "FOOT_BROWSER", footer_width(t["FOOT_BROWSER"]), 1280 - 128)
        # The box art progress: at the tab row's right end (300 pixels), text then a 120-pixel bar.
        check(code, "ART_PROGRESS and its bar", width(t["ART_PROGRESS"] % (888, 888), 17, False) + 16 + 120, 300)
        check(code, "DOWNLOADING", width(t["DOWNLOADING"], 15, False), 300)
        check(code, "GAME_CODE", width(t["GAME_CODE"], 13, True), 90)
        check(code, "SIZE", width(t["SIZE"], 13, True), 70)
        check(code, "SAVE", width(t["SAVE"], 13, True), 130)
        check(code, "INTERNAL_NAME", width(t["INTERNAL_NAME"], 13, True), 208)
        check(code, "NOT_N64", width(t["NOT_N64"], 19, False), 520)
        for key in ("NO_ROM",):
            check(code, key, width(t[key], 30, True), 540)
        for key in ("COPY_ROMS", "INTO_ROMS"):
            check(code, key, width(t[key], 21, False), 540)
        check(code, "WIKI_TEXT", width(t["WIKI_TEXT"], 13, False), 520)
        # The ROM list's empty tab (browser_draw: the list panel, 556 wide).
        check(code, "CAT_EMPTY", width(t["CAT_EMPTY"], 24, True), 520)
        for key in ("FAV_HINT", "CAT_HINT"):
            check(code, key, width(t[key], 19, False), 540)
        for key in ("UPDATE_AVAILABLE", "ART_DONE_LOST", "AUTOSAVED", "PAD_UNPLUGGED", "SLOT_RESERVED", "BATTERY_LOW", "FAV_ADDED", "FAV_REMOVED", "STATE_SAVED", "STATE_WRITE_FAILED", "NO_STATE",
                    "STATE_LOAD_FAILED", "STATE_LOADED", "KEEP_HOLDING", "NOT_SAVED_SCRIPT", "SAVED_ALL", "SAVED_GAME",
                    "SETTINGS_WRITE_FAILED", "GAME_RESET", "SCREENSHOT_SAVED", "SCREENSHOT_FAILED"):
            check(code, "toast " + key, width(t[key], 21, False) + 72, 1280 - 128)
        check(code, "LANG_TITLE", width(t["LANG_TITLE"], 34, True), 1000)
        check(code, "LANG_NAME", width(t["LANG_NAME"], 22, True), 230)
        for key in ("CRASHED", "CANNOT_READ", "RENDERER_FAILED", "NO_ROM"):
            check(code, "message title " + key, width(t[key], 30, True), 1100)
    if not ok:
        sys.exit(1)

    # ---- Writing ----
    def c_string(s):
        out, prev_hex = '"', False
        for b in s.encode("utf-8"):
            ch = chr(b)
            if b >= 0x80:
                out += "\\x%02X" % b
                prev_hex = True
                continue
            if prev_hex and ch in "0123456789abcdefABCDEF":
                out += '" "'
            prev_hex = False
            if ch == "\\":
                out += "\\\\"
            elif ch == '"':
                out += '\\"'
            elif ch == "\n":
                out += "\\n"
            else:
                out += ch
        return out + '"'

    lines = []
    lines.append("// Harissa64 V2 - menu texts. GENERATED by platform/xbox360/tools/gen_lang.py from platform/xbox360/lang/*.txt: do not edit.")
    lines.append("// UTF-8; Arabic already in its joined presentation forms (logical order).")
    lines.append("#ifndef XB_LANG_TABLE_H")
    lines.append("#define XB_LANG_TABLE_H")
    lines.append("")
    lines.append("#define LANG_COUNT %d" % len(LANGS))
    lines.append("")
    lines.append("enum")
    lines.append("{")
    for i, k in enumerate(en_keys):
        # The English text as a comment, without backslashes (one at the end would continue the comment).
        lines.append("    T_%s,   // %s" % (k, en[k].replace("\n", " ").replace("\\", "/")))
    lines.append("    T_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("#ifdef XB_LANG_DEFINE_TABLES   // xb_lang.cpp only")
    lines.append("static const char *const s_langCode[LANG_COUNT] = { %s };" % ", ".join('"%s"' % c for c in LANGS))
    lines.append("")
    lines.append("static const char *const s_langText[LANG_COUNT][T_COUNT] = {")
    for code in LANGS:
        lines.append("  {   // %s" % code)
        for k in en_keys:
            lines.append("    %s," % c_string(langs[code][k]))
        lines.append("  },")
    lines.append("};")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif")
    text = "\n".join(lines) + "\n"
    if "--check" in sys.argv[1:]:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if old.replace("\r\n", "\n") != text:
            print("ERROR: %s is not up to date: run platform/xbox360/tools/gen_lang.py" % OUT)
            sys.exit(1)
        print("texts OK: %d languages, %d texts, %s up to date" % (len(LANGS), len(en_keys), os.path.basename(OUT)))
        return
    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)
    print("wrote %s: %d languages, %d texts" % (OUT, len(LANGS), len(en_keys)))


if __name__ == "__main__":
    main()
