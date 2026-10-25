// Harissa64 V2 - front-end configuration (Xbox 360).
//
// Read in layers: defaults, game:\harissa64v2.ini (developer keys and
// scripted runs), then the menu's files: <drive>:\config\settings.ini (all
// games) and <drive>:\config\<game folder>.ini (one game's profile).
#ifndef XB_CONFIG_H
#define XB_CONFIG_H

#include "../../core/common/h64_types.h"
#include "../../core/pif/h64_input_script.h"
#include "../../core/pif/h64_padmap.h"

struct Config
{
    char mode[32];      // mode=play|jittest
    char cpu[32];       // cpu=dynarec|interp
    char rom[256];      // rom=PATH: run this ROM directly (no ROM browser)
    char lastRom[256];  // lastrom=PATH: the ROM browser's selection (written by the menu)
    int hle;            // hle=0: RSP tasks on the LLE RSP
    int softRenderer;   // renderer=soft: the software RDP draws into RDRAM, Xenos only shows RDRAM (diagnosis)
    int xenosDebug;     // xenosdebug=1..4: renderer debug output (h64_xenos_set_debug)
    u32 pauseAt;        // pauseat=N: hold the frame shown at VI N for 20 s (window captures in Xenia)
    H64InputScript input;   // input=SCRIPT: scripted controller 1 (h64test --input syntax) instead of the pad
    u32 shots[64];      // shots=f1,f2,...: save the frame shown at these VIs (debug, scripted runs)
    int shotCount;
    u32 exitAfter;      // exitafter=N: return to the dashboard after N VIs (scripted runs)
    u32 trace, traceStep;   // trace=N tracestep=C: log N state hashes every C cycles (h64test --trace-frames)
    int fpuFlags;       // fpuflags=0: never read the host FPU flags (FPSCR)
    int jitFpu;         // jitfpu=0: the recompiler leaves COP1 arithmetic to the interpreter
    int regCache;       // regcache=0: no MIPS registers kept in host registers (diagnosis)
    int fpCache;        // fpcache=0: no COP1 registers kept in host FPRs (diagnosis)
    int strict;         // strict=1: no ROM hack tolerance (the console's exact CPU and PI behaviour)
    int statusFast;     // statusfast=0: MTC0 Status always in the interpreter (A/B check)
    int fullExits;      // fullexits=1: the dynarec's older inline linked exits (A/B check)
    int superblocks;    // superblocks=0: blocks end at every branch (A/B check)
    int fastFpu;        // fastfpu=0: exact COP1 (FCR31 cause/flag bits kept); 1 by default
    int smooth;         // smooth=0: no edge smoothing when frames are shown
    int resScale;       // resolution=1|2|3: internal resolution, native 320x240 x1, x2, x3 (default)
    int hiTex;          // hirestex=0|1: use a hi-res texture pack (.hts) of the game when there is one (default 1)
    char hiTexFile[64]; // hirestexfile=NAME: which .hts under game:\hires_texture\<rom>\ (empty: the first one found)
    int texFilter;      // texfilter=0|1|2: N64 3-point, bilinear (default), nearest
    int sharpen;        // sharpen=0|1|2: contrast-adaptive sharpening (off, low, high)
    int blur;           // blur=0|1|2: blur (off, soft, strong)
    int screen;         // screen=0..4: none, scanlines, CRT, curved CRT, LCD grid (crt=1: curved CRT)
    int brightness;     // brightness=0|1|2: as the console, lighter, much lighter
    int vsync;          // vsync=1|0: frames wait for the TV's refresh (default 1)
    int deflicker;      // deflicker=0|1: each picture shown mixed with the previous one (default 0)
    int zoom;           // zoom=N: picture size in percent (default 100)
    int offsetX, offsetY;   // posx=, posy=: picture position in output pixels (default 0)
    int stretch;        // stretch=0|1: the picture stretched to 16:9 (any resolution; HD then stretches instead of widening)
    int showFps;        // showfps=1: frames shown per second in a corner; 2: also CPU and RAM; 3: and the resolution; 4: and the GPU (graphics worker) load
    int logCpu;         // logcpu=N: hardware thread of the log writer (5)
    int gameFlags;      // gameflags=HEX: the game's profile flags instead of the table's (checks; -1: the table)
    int updateCheck;    // updatecheck=0: do not ask GitHub for a newer release (xb_update.cpp); 2: as if running 0.0.0 (testing)
    int artDownload;    // artdownload=0: the ROM browser uses only the box art and game information already in
                        // <drive>:\art\games\ (xb_art.cpp); 1 (default): it downloads what is missing; 2: also in Xenia
    int browserDemo;    // browserdemo=N: the ROM browser moves its selection down every N ms (captures in Xenia)
    char browserSel[128]; // browsersel=FILE: the ROM browser starts on that file name (console checks), over lastrom=
    int artAll;         // artall=1: the ROM browser starts "Get box art for all games" by itself (checks)
    int browserDesign;  // browserdesign=0..3: the ROM browser's layout, the list+card of old (0), a covers grid (1), big covers (2), a carousel (3)
    int bgEffect;       // bgeffect=0..29: the ROM browser's animated background, none (0), the waves (1, default) ... (xb_menu.cpp BGFX_*)
    int fxColor[3];     // fxcolor1..3=0..13: the effect's colours, from xb_menu.cpp's palette; fxcolor1=0: the effect's own, 2 and 3 = 0: unused
    int fxOpacity;      // fxopacity=10..200: how strongly the effect shows, in % (default 100)
    int fxSpeed;        // fxspeed=25..300: how fast it moves, in % (default 100)
    int selColor;       // selcolor=0..8: the menus' selection colour (xb_menu.cpp s_selColors; 0: light blue)
    int wallCustom;     // wallcustom=1: the pictures of game:\wallpapers\ (and its folders) behind the menus, in turn
    char wallFile[128]; // wallfile=NAME: the wallpaper shown (under game:\wallpapers\, "folder\pic.jpg" too), the others after it
    int wallInterval;   // wallinterval=SECONDS: how long each wallpaper shows (default 30; 0: it never changes)
    int wallTransition; // walltransition=0..5: fade, slide, zoom, wipe, blinds, random (xb_ui.h UI_TRANS_*)
    int wallDim;        // walldim=0..80: how much the wallpaper is darkened, in % (default 40)
    int wallFit;        // wallfit=0..4: fill, fit, stretch, centre, tile (xb_ui.h UI_FIT_*)
    int wallShuffle;    // wallshuffle=1: the wallpapers at random, not in name order
    int borderColor;    // bordercolor=0..13: the menus' edges - the selection's colour (0), or xb_menu.cpp's swatches
    int borderOpacity;  // borderopacity=0..100: how strong the edges are, in % (default 100)
    int borderWidth;    // borderwidth=0..4: the panels' own edge in pixels (0: none; default 1)
    int borderGlow;     // borderglow=0|1|2: the glow around the menus - pulsing, steady, none
    int panelOpacity;   // panelopacity=20..100: how solid the panels are, in % (default 70)
    int panelRound;     // panelround=0..3: the panels' corners - square, soft, round (default), rounder
    int uiSoundStyle;   // uisoundstyle=0..3: the interface sounds' style - glass, soft, retro, digital (xb_audio.h XB_SFX_STYLE_*)
    int saverMode;      // savermode=0..4: the screen saver - none, the background effect, the wallpapers, the logo, a clock
    int saverTime;      // savertime=MINUTES: idle time in the ROM list before it starts (default 5)
    char skinName[64];  // skin=NAME: folder under game:\skins\<NAME>\ for the logo/background/music override (empty: none)
    int volumeGame;     // volumegame=0..100: the game's audio (default 100)
    int volumeMusic;    // volumemusic=0..100: the menus' music, game:\music\ (default 100)
    int volumeSfx;      // volumesfx=0..100: the menus' button sounds (default 100)
    int audioMono;      // audiomono=0|1: the game's and the music's sound mixed into both speakers (default 0: stereo)
    int musicMode;      // musicmode=0|1|2|3: game:\music\ in name order, at random, musictrack over and over, or none
    char musicTrack[96];    // musictrack=FILE: the track of musicmode=2 (a file name in game:\music\)
    int asyncGfx;       // asyncgfx=0: graphics tasks and presents on the CPU thread
    int parseWorker;    // parseworker=0: display lists parsed on the graphics worker too (no second worker)
    int asyncAudio;     // asyncaudio=0: audio HLE tasks on the CPU thread
    u32 audioCycles;    // audiocycles=N: CPU cycles an asynchronous audio task keeps the RSP busy
    u32 gfxCycles;      // gfxcycles=N: CPU cycles an asynchronous graphics task keeps the RSP busy
    int gfxTiming;      // gfxtiming=1: graphics tasks last the real microcode's estimated time (at least gfxcycles)
    int lateParse;      // lateparse=0/1: graphics tasks end without waiting for their parse (experimental; -1: per game)
    int cpi;            // cpi=N: CPU cycles per instruction (2 by default; mupen64plus's CountPerOp N is 2N)
    int audioMs;        // audioms=N: audio queue target in ms (pacing threshold)
    int stateSlot;      // stateslot=N: save state slot at start (1..9)
    int loadState;      // loadstate=1: load the slot's state at start (scripted runs)
    u32 saveStateAt;    // savestateat=N: save a state at VI N (scripted runs)
    u32 menuAt;         // menuat=N: open the in-game menu at VI N (remote checks; does not skip the ROM browser)
    u32 aboutAt;        // aboutat=N: the same, showing the About panel
    int controlsTest;   // controlstest=N: the ROM list opens Settings > Controls on item N; 100+N: also
                        // waiting for an input, again and again; 200+N: the Settings page on item N
                        // (remote checks of the drawing; -1: off)
    int autoStart;      // autostart=N: the ROM browser starts N games by itself, the last selection then the
                        // next ones in the list (remote checks of game changes)
    int autoIndex;      // games the browser started by itself so far
    u32 browserAt;      // browserat=N: back to the ROM list at VI N (as the menu's "Back to the ROM list")
    int xenia;          // xenia=1: running in Xenia (no FPSCR access, no return to the dashboard)
    char lang[8];       // lang=en|es|de|fr|ar|pt|ru: the menus' language (settings.ini; empty: not chosen yet)
    char uiLang[8];     // uilang=CODE (harissa64v2.ini, checks): this language whatever settings.ini says, no language list
    int langPick;       // langpick=1: the language list at start even when a language is set (checks); 2..8: on LANG_* N-2
    int langAuto;       // langauto=MS: the language list confirms its selection by itself after MS ms (checks)
    int rumble;         // rumble=0|1: a Rumble Pak in each controller of the games that support it (default 1)
    int padLoss;        // padloss=0|1|2: a controller unplugged during a game saves a state (autosaveslot) and
                        // pauses (1), or only pauses (2); 0: nothing (default)
    int autoSaveSlot;   // autosaveslot=1..9: that save's slot (9 by default), kept for it while padloss=1
    int batteryWarn;    // batterywarn=0|10|25|50: a message when a wireless controller's battery is at or under
                        // that percent (0: off, the default)
    int uiSounds;       // uisounds=0|1: the menus' button sounds (default 1)
    int startRomMode;   // startrommode=0|1|2: when the emulator starts, the ROM list (0), the last game played (1)
                        // or startrom (2) - BACK held at start skips it
    char startRom[256]; // startrom=PATH: the game of startrommode=2
    H64PadMap padMap[4];    // controls of N64 ports 1-4 (map_a=xa,... ; p2_map_a=... for port 2; core/pif/h64_padmap.h)
    int shotBtn1, shotBtn2; // shotbtn1=, shotbtn2=: H64_SRC_* source held together to take a screenshot (-1: not set)
};

enum { PADLOSS_OFF = 0, PADLOSS_SAVE, PADLOSS_PAUSE, PADLOSS_COUNT };   // Config::padLoss: save and pause, pause

void ConfigDefaults(Config *c);
// Applies the key=value lines of a file over *c. Returns 0 when the file is missing.
int ConfigParseFile(Config *c, const char *path);
// Writes the keys the menu edits (cpu, hle, audioms, showfps, the graphics
// keys, the controls, and lastrom and lang when `withLastRom`). Returns 0 on failure.
int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom);
// Sets lang= in the menu's settings file (the other lines stay; the file is
// made when missing). Returns 0 on failure.
int ConfigWriteLang(const char *path, const char *code);

#endif
