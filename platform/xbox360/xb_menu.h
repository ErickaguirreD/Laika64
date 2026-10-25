// Harissa64 V2 - front-end menus (Xbox 360): a generic list panel, the ROM
// browser shown at start-up and the settings screen.
#ifndef XB_MENU_H
#define XB_MENU_H

#include <xtl.h>

#include "xb_config.h"

#define UI_MENU_MAX 17

struct UiMenu
{
    char title[128];
    char label[UI_MENU_MAX][96];   // UTF-8 (xb_lang.h)
    char value[UI_MENU_MAX][64];   // shown right-aligned ("< 250 ms >"), empty for actions
    int count, sel;
    char footer[192];
    int page;                      // UI_PAGE_*: how UiMenuDraw shows it
    unsigned char icon[UI_MENU_MAX];   // UI_ICON_*: a small white symbol left of the label (0: none)
};
enum { UI_PAGE_LIST = 0, UI_PAGE_CONTROLS, UI_PAGE_VOLUME, UI_PAGE_CALIB, UI_PAGE_FX, UI_PAGE_WALL, UI_PAGE_BORDER, UI_PAGE_PERF };
enum { UI_ICON_NONE = 0, UI_ICON_SYSTEM, UI_ICON_GRAPHICS, UI_ICON_SCREEN, UI_ICON_SOUND, UI_ICON_CONTROLS,
       UI_ICON_INTERFACE, UI_ICON_SAVE, UI_ICON_PLAY, UI_ICON_STATES, UI_ICON_GEAR, UI_ICON_RESET, UI_ICON_LIST,
       UI_ICON_POWER, UI_ICON_SLOT, UI_ICON_LOAD, UI_ICON_BATTERY, UI_ICON_SLIDERS, UI_ICON_TEXTURE, UI_ICON_HD,
       UI_ICON_SMOOTH, UI_ICON_SHARP, UI_ICON_BLUR, UI_ICON_CHART, UI_ICON_STRETCH, UI_ICON_CROSSHAIR, UI_ICON_SUN,
       UI_ICON_VSYNC, UI_ICON_EYE, UI_ICON_SCANLINES, UI_ICON_FRAME, UI_ICON_ARROWS_H, UI_ICON_ARROWS_V, UI_ICON_CLOCK,
       UI_ICON_PALETTE, UI_ICON_GLOBE, UI_ICON_LAYOUT, UI_ICON_MUSIC, UI_ICON_SPEAKERS, UI_ICON_PLUS, UI_ICON_TRASH };
// Sets the icons of a menu's rows in order (n of them, from row 0).
void UiMenuIcons(UiMenu *m, const unsigned char *icons, int n);

void UiMenuClear(UiMenu *m, const char *title, const char *footer);
int UiMenuAdd(UiMenu *m, const char *label, const char *value);   // returns the item's index
// A side bar down the left over whatever is on the target (the frozen game frame in game).
void UiMenuDraw(const UiMenu *m);   // between UiBegin and UiEnd
// After UiMenuDraw: a picture under its rows, framed like the selected cover
// (the States page's slot picture); NULL tex: an empty box with emptyText.
void UiMenuThumb(const UiMenu *m, IDirect3DTexture9 *tex, const char *emptyText);
// Up/down move the selection (wrapping).
void UiMenuNavigate(UiMenu *m, WORD down);
// A button was pressed on the selected row: its highlight shows it for a moment.
void UiMenuPress(void);
// A yes/no question over what is drawn (the in-game menu's): `title` (T_*, -1:
// none), the question and its prompts, centred. Between UiBegin and UiEnd.
void UiConfirmDraw(int title, int question, int footer);

// ---- Settings (cpu, hle, audio margin, FPS display, the Graphics and Controls pages) ----
enum { SET_NONE = 0, SET_CHANGED, SET_BACK, SET_SAVE_ALL, SET_SAVE_GAME, SET_ART_ASK, SET_LANGUAGE, SET_PREVIEW, SET_SAVER };
// Fills the menu from *c; `perGame` adds "Save for this game".
void SettingsBuild(UiMenu *m, const Config *c, int perGame);
// Applies one input to *c and the menu; returns SET_*. The Language row
// changes the language at once (and keeps it in the file given to
// SettingsSetLangFile); from the ROM list, A on it returns SET_LANGUAGE (the
// list with the flags).
int SettingsInput(UiMenu *m, Config *c, int perGame, WORD down);
// The performance box's place, text size and colour, opacity, from *c (UiSetPerfStyle).
void PerfStyleApply(const Config *c);
// The menu settings file where a language change is kept (lang=); "" for none.
void SettingsSetLangFile(const char *path);

// ---- Language ----
// The list of languages with their flags, full screen: returns the language
// chosen with A (LANG_*), or -1 when B leaves it (never on the first start,
// `firstStart`, where a language must be chosen). Starts on `initial`.
int LanguageScreen(IDirect3DDevice9 *dev, int initial, int firstStart);
// Checks (ini langauto=): the list confirms its selection by itself after `ms` ms (0: never).
void LanguageScreenAuto(int ms);

// Reads a ROM file, or the first N64 ROM inside a .zip. Returns a malloc'd
// buffer, or NULL.
u8 *RomFileLoad(const char *path, u32 *size);

// The header of a ROM file (plain or zipped) in *rom (free with h64_rom_free).
// Returns the ROM size, 0 when it is not an N64 ROM. Any thread.
struct H64Rom;
u32 RomReadHeader(const char *path, H64Rom *rom);

// The game's save/profile folder name (h64_save_folder_name) from the ROM
// file's header. Returns 0 when the file is not an N64 ROM.
int RomFolderName(const char *path, char *out, size_t size);

// ---- About ----
// The credits panel (centred, over a veil), and a full screen showing it until B.
void AboutDraw(void);   // between UiBegin and UiEnd
void AboutScreen(IDirect3DDevice9 *dev);

// ---- Hi-res texture packs (a game can have more than one .hts) ----
#define HIRES_PACKS_MAX 8
// Lists game:\hires_texture\<romName>\*.hts into a shared, static list the
// Graphics settings row (G_HIRES) reads from. Call whenever the ROM in
// context changes: the browser's selection (rom_details) and HiresOpen
// (main.cpp, at launch) both call this, so the picker always matches
// whichever ROM is selected or running. Cheap - just a directory listing,
// no file is opened.
void HiresScanPacks(const char *romName);
int HiresPackCount(void);
const char *HiresPackName(int i);   // i: 0..HiresPackCount()-1, just the .hts file name
const char *HiresPackPath(int i);   // the full game:\hires_texture\... path, to open it

// A preview of the pack G_HIRES currently points at (Config::hiTex/hiTexFile):
// one representative texture, decoded from the pack itself (packs carry no
// preview image of their own). Same split as the About panel: the ROM browser
// (which has its own IDirect3DDevice9*) uses the blocking HiresPreviewScreen;
// in-game (no dev of its own outside the renderer's overlay hook, which does
// not carry a Config*) calls HiresPreviewOpen once (when Y opens it) then sets
// a flag and calls HiresPreviewDraw() from the overlay hook - see main.cpp.
void HiresPreviewOpen(const Config *c);   // remembers which pack to show; decodes lazily on first draw
void HiresPreviewDraw(IDirect3DDevice9 *dev);   // between UiBegin and UiEnd
void HiresPreviewScreen(IDirect3DDevice9 *dev, const Config *c);

// ---- Skins (logo, background, menu music - see xb_ui.h and xb_audio.h) ----
#define SKINS_MAX 8
// Lists the folders under game:\skins\* into a shared, static list the
// Interface > Skins settings row reads from. Call once before that page is
// shown (settings_skins) - cheap, just a directory listing.
void SkinScan(void);
int SkinCount(void);
const char *SkinName(int i);   // i: 0..SkinCount()-1, just the folder name (also Config::skinName)

// ---- Low battery warning ----
// Every few seconds, when Config::batteryWarn is on: 1 with `msg` ("Controller
// 2: battery low") when a wireless controller is at or under that percent
// (said again after a few minutes while it stays low), else 0.
int BatteryCheck(const Config *c, char *msg, int size);

// ---- ROM browser ----
// Lists game:\roms\ (.z64 .n64 .v64 .zip). Returns 1 with the chosen path, 0 when
// the user leaves to the dashboard. Y opens the settings for all games,
// saved to `settingsPath` (also where the last selection is kept).
int RomBrowser(IDirect3DDevice9 *dev, Config *c, const char *settingsPath, char *path, size_t pathSize);

#endif
