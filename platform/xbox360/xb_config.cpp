// Harissa64 V2 - front-end configuration (see xb_config.h).
#include "xb_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xb_audio.h"

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// shotbtn1=/shotbtn2=: one source name (h64_padmap_parse_sources's syntax), the
// lowest one when the text names more than one. -1 ("none" or unknown text).
static int parse_one_source(const char *text)
{
    u32 mask = 0;
    int i;
    if (h64_padmap_parse_sources(text, &mask) < 0 || !mask) return -1;
    for (i = 0; i < H64_SRC_COUNT; i++)
        if (mask & (1u << i)) return i;
    return -1;
}

// The internal resolutions. The N64 target and its depth must fit the EDRAM
// beside the back buffer and the copy-back target (h64_xenos.cpp): about 904
// tiles of 80x16 each, so 1120x840 and 1366x768 are the largest.
static const u16 s_res43[][2] = { { 320, 240 }, { 480, 360 }, { 640, 480 }, { 800, 600 }, { 960, 720 }, { 1024, 768 }, { 1120, 840 } };
static const u16 s_res169[][2] = { { 432, 240 }, { 640, 360 }, { 848, 480 }, { 1280, 720 }, { 1366, 768 } };

int ConfigResCount(int wide)
{
    return wide ? (int)(sizeof(s_res169) / sizeof(s_res169[0])) : (int)(sizeof(s_res43) / sizeof(s_res43[0]));
}

void ConfigResSize(int wide, int index, u32 *w, u32 *h)
{
    const u16 (*t)[2] = wide ? s_res169 : s_res43;
    int n = ConfigResCount(wide);
    if (index < 0 || index >= n) index = wide ? 3 : 4;
    *w = t[index][0];
    *h = t[index][1];
}

void ConfigResCurrent(const Config *c, u32 *w, u32 *h, int *wide)
{
    *wide = c->resWide != 0;
    ConfigResSize(*wide, *wide ? c->res169 : c->res43, w, h);
}

void ConfigDefaults(Config *c)
{
    strcpy(c->mode, "play");
    strcpy(c->cpu, "dynarec");
    c->rom[0] = 0;
    c->lastRom[0] = 0;
    c->showFps = 0;
    c->perfItems = 0;
    c->perfPos = 2;
    c->perfSize = 1;
    c->perfColor = 0;
    c->perfBox = 75;
    c->perfFahrenheit = 0;
    c->artKind = 0;
    c->uiFont = 1;   // Roboto
    memset(c->path, 0, sizeof(c->path));
    c->colorStyle = 0;
    c->updateCheck = 1;   // GitHub ErickaguirreD/Laika64: the check, and the updater when there is a newer release
    c->artDownload = 1;
    c->artExtras = 1;
    c->artBackground = 0;
    c->browserDemo = 0;
    c->browserSel[0] = 0;
    c->artAll = 0;
    c->gameFlags = -1;
    c->logCpu = 5;
    c->menuAt = 0;
    c->aboutAt = 0;
    c->controlsTest = -1;
    c->autoStart = 0;
    c->autoIndex = 0;
    c->browserAt = 0;
    c->hle = 1;
    c->shotCount = 0;
    c->softRenderer = 0;
    c->xenosDebug = 0;
    c->pauseAt = 0;
    memset(&c->input, 0, sizeof(c->input));
    c->trace = c->traceStep = 0;
    c->xenia = 0;
    c->lang[0] = 0;
    c->uiLang[0] = 0;
    c->langPick = 0;
    c->rumble = 100;
    c->padLoss = PADLOSS_OFF;
    c->autoSaveSlot = 9;
    c->batteryWarn = 0;
    c->uiSounds = 1;
    c->startRomMode = 0;
    c->startRom[0] = 0;
    c->shotBtn1 = c->shotBtn2 = -1;
    c->browserDesign = 0;
    c->bgEffect = 18;   // the nebula (BGFX_NEBULA): what the setup assistant shows
    c->fxColor[0] = c->fxColor[1] = c->fxColor[2] = 0;   // the effect's own colours
    c->fxOpacity = 100;
    c->fxSpeed = 100;
    c->selColor = 0;
    c->wallCustom = 0;
    c->wallFile[0] = 0;
    c->uiSoundStyle = 0;
    c->wallInterval = 30;
    c->wallTransition = 0;
    c->wallDim = 40;
    c->wallFit = 0;
    c->wallShuffle = 0;
    c->borderColor = 0;
    c->borderOpacity = 100;
    c->borderWidth = 1;
    c->borderGlow = 0;
    c->panelOpacity = 30;
    c->panelColor = 0;
    c->intro = 1;
    c->settingsView = 1;   // big icons
    c->brightLevel = -1;
    c->sharpLevel = c->blurLevel = -1;
    c->panelRound = 2;
    c->saverMode = 0;
    c->saverTime = 5;
    c->skinName[0] = 0;
    c->volumeGame = 100;
    c->volumeMusic = 100;
    c->volumeSfx = 100;
    c->audioMono = 0;
    // LAIKA 64's own piece (the assistant's, also in music\): over and over, until another is chosen.
    c->musicMode = 2;   // XB_MUSIC_TRACK
    strcpy(c->musicTrack, "laika_orbit.mp3");
    c->langAuto = 0;
    c->fpuFlags = 1;
    c->jitFpu = 1;
    c->regCache = 1;
    c->fpCache = 1;
    c->fastFpu = 1;  // host FPU without FCR31 cause/flag tracking, as mupen64plus
    c->fullExits = 0;
    c->statusFast = 1;
    c->strict = 0;
    c->superblocks = 1;
    c->smooth = 1;
    c->res43 = 4;    // 960x720
    c->res169 = 4;   // 1366x768: with the defaults below, the assistant's "Recommended"
    c->resWide = 1;
    c->wallRecommend = 0;
    c->descTranslate = 1;
    c->texFilter = 1;
    c->hiTex = 1;
    c->hiTexFile[0] = 0;
    c->sharpen = 0;
    c->blur = 0;
    c->screen = 0;
    c->brightness = 0;
    c->vsync = 0;   // off by default (as 2.0.1): with it, waits on the GPU also wait for the TV
    c->deflicker = 0;
    c->zoom = 100;
    c->offsetX = c->offsetY = 0;
    c->devMode = c->gfxLog = 0;
    c->texR3 = 1;   // R3 x2: the HD textures on and off in game
    ConfigHotkeyDefaults(c);
    c->stretch = 0;
    c->asyncGfx = 1;
    c->parseWorker = 1;
    c->asyncAudio = 1;
    c->audioCycles = 100000;
    c->gfxCycles = 400000;
    c->gfxTiming = 0;
    c->lateParse = -1;   // per game (main.cpp)
    c->cpi = 2;      // mupen64plus's CountPerOp 1 (its exact setting; its default 2 is our 4)
    c->exitAfter = 0;
    c->audioMs = XB_AUDIO_DEFAULT_MS;
    c->stateSlot = 1;
    c->loadState = 0;
    c->saveStateAt = 0;
    {
        int i;
        for (i = 0; i < 4; i++) h64_padmap_default(&c->padMap[i]);
    }
}

int ConfigParseFile(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    static char line[8192];   // input= scripts run long (one line)
    // Menu files written before settingsver=2 saved V-Sync on by default
    // (2.0.2-2.0.4): their vsync=1 is not a choice, and V-Sync made busy
    // games slower (Mario Tennis); it is off by default again.
    int menuFile = 0, settingsVer = 0, vsync = -1;
    int shotSeen = 0, hkShotSeen = 0;   // an old screenshot combo, and the shortcut that replaces it
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        trim(line);
        if (!strncmp(line, "# Written by the Harissa64 V2 menu", 34)) menuFile = 1;
        if (line[0] == '#' || line[0] == ';' || !eq) continue;
        *eq = 0;
        // Outside the chain below: it is at the compiler's limit of nested else-ifs.
        if (!strcmp(line, "devmode")) c->devMode = atoi(eq + 1);
        if (!strcmp(line, "gfxlog")) c->gfxLog = atoi(eq + 1);
        if (!strcmp(line, "texr3")) c->texR3 = atoi(eq + 1);
        if (!strcmp(line, "artextras")) c->artExtras = atoi(eq + 1);
        if (!strcmp(line, "artbg")) c->artBackground = atoi(eq + 1);
        if (!strcmp(line, "panelcolor")) c->panelColor = atoi(eq + 1);
        if (!strcmp(line, "intro")) c->intro = atoi(eq + 1);
        if (!strcmp(line, "setview")) c->settingsView = atoi(eq + 1);
        if (!strcmp(line, "brightlevel")) c->brightLevel = atoi(eq + 1);
        if (!strcmp(line, "sharplevel")) c->sharpLevel = atoi(eq + 1);
        if (!strcmp(line, "blurlevel")) c->blurLevel = atoi(eq + 1);
        if (!strcmp(line, "res43")) c->res43 = atoi(eq + 1);
        if (!strcmp(line, "desctr")) c->descTranslate = atoi(eq + 1) != 0;
        if (!strcmp(line, "wallrec")) c->wallRecommend = atoi(eq + 1) != 0;
        if (!strcmp(line, "res169")) c->res169 = atoi(eq + 1);
        if (!strcmp(line, "reswide")) c->resWide = atoi(eq + 1) != 0;
        if (!strcmp(line, "resolution"))
        {
            // Older files: 1-3 native x1, x2, x3 (4:3), 4 HD 1280x720 (16:9).
            int r = atoi(eq + 1);
            if (r == 4) { c->resWide = 1; c->res169 = 3; }
            else if (r >= 1 && r <= 3) { c->resWide = 0; c->res43 = (r - 1) * 2; }
        }
        {
            static const char *const hk[HK_COUNT] = { "hk_save", "hk_load", "hk_slotprev", "hk_slotnext", "hk_shot" };
            int k;
            for (k = 0; k < HK_COUNT; k++)
                if (!strcmp(line, hk[k])) { c->hotkey[k] = (u32)strtoul(eq + 1, NULL, 16); if (k == HK_SHOT) hkShotSeen = 1; }
        }
        if (!strcmp(line, "shotbtn1") || !strcmp(line, "shotbtn2")) shotSeen = 1;
        if (!strcmp(line, "mode")) { strncpy(c->mode, eq + 1, sizeof(c->mode) - 1); c->mode[sizeof(c->mode) - 1] = 0; }
        else if (!strcmp(line, "cpu")) { strncpy(c->cpu, eq + 1, sizeof(c->cpu) - 1); c->cpu[sizeof(c->cpu) - 1] = 0; }
        else if (!strcmp(line, "rom")) { strncpy(c->rom, eq + 1, sizeof(c->rom) - 1); c->rom[sizeof(c->rom) - 1] = 0; }
        else if (!strcmp(line, "lastrom")) { strncpy(c->lastRom, eq + 1, sizeof(c->lastRom) - 1); c->lastRom[sizeof(c->lastRom) - 1] = 0; }
        else if (!strcmp(line, "showfps"))
        {
            // The old levels as items (a perfitems= further on wins).
            static const int levels[5] = { 0, CONFIG_PERF_FPS, CONFIG_PERF_FPS | CONFIG_PERF_CPU | CONFIG_PERF_RAM,
                                           CONFIG_PERF_FPS | CONFIG_PERF_CPU | CONFIG_PERF_RAM | CONFIG_PERF_RES,
                                           CONFIG_PERF_FPS | CONFIG_PERF_CPU | CONFIG_PERF_RAM | CONFIG_PERF_RES | CONFIG_PERF_GPU };
            c->showFps = atoi(eq + 1);
            c->perfItems = levels[c->showFps >= 0 && c->showFps <= 4 ? c->showFps : 0];
        }
        else if (!strcmp(line, "perfitems")) c->perfItems = atoi(eq + 1) & CONFIG_PERF_ALL;
        else if (!strcmp(line, "perfpos")) c->perfPos = atoi(eq + 1);
        else if (!strcmp(line, "perfsize")) c->perfSize = atoi(eq + 1);
        else if (!strcmp(line, "perfcolor")) c->perfColor = atoi(eq + 1);
        else if (!strcmp(line, "perfbox")) c->perfBox = atoi(eq + 1);
        else if (!strcmp(line, "perffahrenheit")) c->perfFahrenheit = atoi(eq + 1);
        else if (!strcmp(line, "artkind")) c->artKind = atoi(eq + 1);
        else if (!strcmp(line, "uifont")) c->uiFont = atoi(eq + 1);
        else if (!strcmp(line, "romdir") || !strcmp(line, "musicdir") || !strcmp(line, "sounddir") || !strcmp(line, "walldir") ||
                 !strcmp(line, "savedir") || !strcmp(line, "shotdir") || !strcmp(line, "texdir"))
        {
            static const char *const keys[CONFIG_PATH_COUNT] = { "romdir", "musicdir", "sounddir", "walldir", "savedir", "shotdir", "texdir" };
            int k;
            for (k = 0; k < CONFIG_PATH_COUNT; k++)
                if (!strcmp(line, keys[k]))
                {
                    size_t n = strlen(eq + 1);
                    strncpy(c->path[k], eq + 1, sizeof(c->path[k]) - 1);
                    c->path[k][sizeof(c->path[k]) - 1] = 0;
                    if (n && n < sizeof(c->path[k]) && c->path[k][n - 1] == '\\') c->path[k][n - 1] = 0;   // no trailing backslash
                }
        }
        else if (!strcmp(line, "colorstyle")) c->colorStyle = atoi(eq + 1);
        else if (!strcmp(line, "updatecheck")) c->updateCheck = atoi(eq + 1);
        else if (!strcmp(line, "artdownload")) c->artDownload = atoi(eq + 1);
        else if (!strcmp(line, "browserdemo")) c->browserDemo = atoi(eq + 1);
        else if (!strcmp(line, "browsersel")) { strncpy(c->browserSel, eq + 1, sizeof(c->browserSel) - 1); c->browserSel[sizeof(c->browserSel) - 1] = 0; }
        else if (!strcmp(line, "artall")) c->artAll = atoi(eq + 1);
        else if (!strcmp(line, "browserdesign")) c->browserDesign = atoi(eq + 1);
        else if (!strcmp(line, "bgeffect")) c->bgEffect = atoi(eq + 1);
        else if (!strncmp(line, "fxcolor", 7) && line[7] >= '1' && line[7] <= '3' && !line[8]) c->fxColor[line[7] - '1'] = atoi(eq + 1);
        else if (!strcmp(line, "fxopacity")) c->fxOpacity = atoi(eq + 1);
        else if (!strcmp(line, "fxspeed")) c->fxSpeed = atoi(eq + 1);
        else if (!strcmp(line, "selcolor")) c->selColor = atoi(eq + 1);
        else if (!strcmp(line, "wallcustom")) c->wallCustom = atoi(eq + 1);
        else if (!strcmp(line, "wallfile")) { strncpy(c->wallFile, eq + 1, sizeof(c->wallFile) - 1); c->wallFile[sizeof(c->wallFile) - 1] = 0; }
        else if (!strcmp(line, "wallfolder")) c->wallCustom = eq[1] != 0;   // written by 2.0.6 test builds: a folder chosen = on
        else if (!strcmp(line, "uisoundstyle")) c->uiSoundStyle = atoi(eq + 1);
        else if (!strcmp(line, "wallinterval")) c->wallInterval = atoi(eq + 1);
        else if (!strcmp(line, "walltransition")) c->wallTransition = atoi(eq + 1);
        else if (!strcmp(line, "walldim")) c->wallDim = atoi(eq + 1);
        else if (!strcmp(line, "wallfit")) c->wallFit = atoi(eq + 1);
        else if (!strcmp(line, "wallshuffle")) c->wallShuffle = atoi(eq + 1);
        else if (!strcmp(line, "bordercolor")) c->borderColor = atoi(eq + 1);
        else if (!strcmp(line, "borderopacity")) c->borderOpacity = atoi(eq + 1);
        else if (!strcmp(line, "borderwidth")) c->borderWidth = atoi(eq + 1);
        else if (!strcmp(line, "borderglow")) c->borderGlow = atoi(eq + 1);
        else if (!strcmp(line, "panelopacity")) c->panelOpacity = atoi(eq + 1);
        else if (!strcmp(line, "panelround")) c->panelRound = atoi(eq + 1);
        else if (!strcmp(line, "savermode")) c->saverMode = atoi(eq + 1);
        else if (!strcmp(line, "savertime")) c->saverTime = atoi(eq + 1);
        else if (!strcmp(line, "skin")) { strncpy(c->skinName, eq + 1, sizeof(c->skinName) - 1); c->skinName[sizeof(c->skinName) - 1] = 0; }
        else if (!strcmp(line, "volumegame")) c->volumeGame = atoi(eq + 1);
        else if (!strcmp(line, "volumemusic")) c->volumeMusic = atoi(eq + 1);
        else if (!strcmp(line, "volumesfx")) c->volumeSfx = atoi(eq + 1);
        else if (!strcmp(line, "audiomono")) c->audioMono = atoi(eq + 1) ? 1 : 0;
        else if (!strcmp(line, "musicmode"))
        {
            int v = atoi(eq + 1);
            c->musicMode = v >= 0 && v <= 3 ? v : 0;
        }
        else if (!strcmp(line, "musictrack"))
        {
            strncpy(c->musicTrack, eq + 1, sizeof(c->musicTrack) - 1);
            c->musicTrack[sizeof(c->musicTrack) - 1] = 0;
        }
        else if (!strcmp(line, "gameflags")) c->gameFlags = (int)strtoul(eq + 1, NULL, 16);   // strtoul: bit 31 is a flag
        else if (!strcmp(line, "logcpu")) c->logCpu = atoi(eq + 1);
        else if (!strcmp(line, "menuat")) c->menuAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "aboutat")) c->aboutAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "controlstest")) c->controlsTest = atoi(eq + 1);
        else if (!strcmp(line, "autostart")) c->autoStart = atoi(eq + 1);
        else if (!strcmp(line, "browserat")) c->browserAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "hle")) c->hle = atoi(eq + 1);
        else if (!strcmp(line, "exitafter")) c->exitAfter = (u32)atoi(eq + 1);
        else if (!strcmp(line, "stateslot")) c->stateSlot = atoi(eq + 1);
        else if (!strcmp(line, "audioms")) c->audioMs = atoi(eq + 1);
        else if (!strcmp(line, "loadstate")) c->loadState = atoi(eq + 1);
        else if (!strcmp(line, "savestateat")) c->saveStateAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "renderer")) c->softRenderer = !strcmp(eq + 1, "soft");
        else if (!strcmp(line, "trace")) c->trace = (u32)atoi(eq + 1);
        else if (!strcmp(line, "xenia")) c->xenia = atoi(eq + 1);
        else if (!strcmp(line, "lang")) { strncpy(c->lang, eq + 1, sizeof(c->lang) - 1); c->lang[sizeof(c->lang) - 1] = 0; }
        else if (!strcmp(line, "uilang")) { strncpy(c->uiLang, eq + 1, sizeof(c->uiLang) - 1); c->uiLang[sizeof(c->uiLang) - 1] = 0; }
        else if (!strcmp(line, "langpick")) c->langPick = atoi(eq + 1);
        else if (!strcmp(line, "rumble")) { c->rumble = atoi(eq + 1); if (c->rumble == 1) c->rumble = 100; if (c->rumble < 0 || c->rumble > 100) c->rumble = 100; }
        else if (!strcmp(line, "padloss"))
        {
            int v = atoi(eq + 1);
            c->padLoss = v >= 0 && v < PADLOSS_COUNT ? v : PADLOSS_OFF;
        }
        else if (!strcmp(line, "autosaveslot"))
        {
            int v = atoi(eq + 1);
            if (v >= 1 && v <= 9) c->autoSaveSlot = v;
        }
        else if (!strcmp(line, "uisounds")) c->uiSounds = atoi(eq + 1) ? 1 : 0;
        else if (!strcmp(line, "startrommode"))
        {
            int v = atoi(eq + 1);
            c->startRomMode = v >= 0 && v <= 2 ? v : 0;
        }
        else if (!strcmp(line, "startrom"))
        {
            strncpy(c->startRom, eq + 1, sizeof(c->startRom) - 1);
            c->startRom[sizeof(c->startRom) - 1] = 0;
        }
        else if (!strcmp(line, "batterywarn"))
        {
            int v = atoi(eq + 1);
            c->batteryWarn = v == 10 || v == 25 || v == 50 ? v : 0;
        }
        else if (!strcmp(line, "langauto")) c->langAuto = atoi(eq + 1);
        else if (!strcmp(line, "fpuflags")) c->fpuFlags = atoi(eq + 1);
        else if (!strcmp(line, "jitfpu")) c->jitFpu = atoi(eq + 1);
        else if (!strcmp(line, "regcache")) c->regCache = atoi(eq + 1);
        else if (!strcmp(line, "fpcache")) c->fpCache = atoi(eq + 1);
        else if (!strcmp(line, "fastfpu")) c->fastFpu = atoi(eq + 1);
        else if (!strcmp(line, "fullexits")) c->fullExits = atoi(eq + 1);
        else if (!strcmp(line, "statusfast")) c->statusFast = atoi(eq + 1);
        else if (!strcmp(line, "strict")) c->strict = atoi(eq + 1);
        else if (!strcmp(line, "superblocks")) c->superblocks = atoi(eq + 1);
        else if (!strcmp(line, "smooth")) c->smooth = atoi(eq + 1);
        else if (!strcmp(line, "texfilter")) c->texFilter = atoi(eq + 1);
        else if (!strcmp(line, "hirestex")) c->hiTex = atoi(eq + 1) != 0;
        else if (!strcmp(line, "hirestexfile")) { strncpy(c->hiTexFile, eq + 1, sizeof(c->hiTexFile) - 1); c->hiTexFile[sizeof(c->hiTexFile) - 1] = 0; }
        else if (!strcmp(line, "shotbtn1")) c->shotBtn1 = parse_one_source(eq + 1);
        else if (!strcmp(line, "shotbtn2")) c->shotBtn2 = parse_one_source(eq + 1);
        else if (!strcmp(line, "sharpen")) c->sharpen = atoi(eq + 1);
        else if (!strcmp(line, "crt")) c->screen = atoi(eq + 1) ? 3 : 0;
        else if (!strcmp(line, "screen")) c->screen = atoi(eq + 1);
        else if (!strcmp(line, "brightness")) c->brightness = atoi(eq + 1);
        else if (!strcmp(line, "vsync")) vsync = atoi(eq + 1);
        else if (!strcmp(line, "settingsver")) settingsVer = atoi(eq + 1);
        else if (!strcmp(line, "deflicker")) c->deflicker = atoi(eq + 1);
        else if (!strcmp(line, "zoom")) c->zoom = atoi(eq + 1);
        else if (!strcmp(line, "posx")) c->offsetX = atoi(eq + 1);
        else if (!strcmp(line, "posy")) c->offsetY = atoi(eq + 1);
        else if (!strcmp(line, "stretch")) c->stretch = atoi(eq + 1);
        else if (!strcmp(line, "blur")) c->blur = atoi(eq + 1);
        else if (!strcmp(line, "asyncgfx")) c->asyncGfx = atoi(eq + 1);
        else if (!strcmp(line, "parseworker")) c->parseWorker = atoi(eq + 1);
        else if (!strcmp(line, "asyncaudio")) c->asyncAudio = atoi(eq + 1);
        else if (!strcmp(line, "audiocycles")) c->audioCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "gfxcycles")) c->gfxCycles = (u32)atoi(eq + 1);
        else if (!strcmp(line, "gfxtiming")) c->gfxTiming = atoi(eq + 1);
        else if (!strcmp(line, "lateparse")) c->lateParse = atoi(eq + 1);
        else if (!strcmp(line, "cpi")) c->cpi = atoi(eq + 1);
        else if (!strcmp(line, "xenosdebug")) c->xenosDebug = atoi(eq + 1);
        else if (!strcmp(line, "pauseat")) c->pauseAt = (u32)atoi(eq + 1);
        else if (!strcmp(line, "input")) h64_input_script_parse(&c->input, eq + 1);
        else if (!strcmp(line, "tracestep")) c->traceStep = (u32)strtoul(eq + 1, NULL, 10);
        else if (!strncmp(line, "map", 3)) h64_padmap_parse_key(&c->padMap[0], line, eq + 1);
        else if (line[0] == 'p' && line[1] >= '2' && line[1] <= '4' && line[2] == '_' && !strncmp(line + 3, "map", 3))
            h64_padmap_parse_key(&c->padMap[line[1] - '1'], line + 3, eq + 1);
        else if (!strcmp(line, "shots"))
        {
            char *p = eq + 1;
            while (*p && c->shotCount < 64)
            {
                c->shots[c->shotCount++] = (u32)strtoul(p, &p, 10);
                while (*p == ',' || *p == ' ') p++;
            }
        }
    }
    fclose(f);
    if (vsync >= 0 && !(menuFile && settingsVer < 2 && vsync == 1)) c->vsync = vsync;
    if (c->vsync == 1) c->vsync = 2;   // the "normal" mode is gone: V-Sync on is the one that always waits
    if (c->brightLevel < 0 || c->brightLevel > 100)
    {
        // The five steps of before (normal, lighter, much lighter, darker, much darker) on the bar.
        static const int level[5] = { 50, 75, 94, 29, 4 };
        c->brightLevel = level[c->brightness >= 0 && c->brightness <= 4 ? c->brightness : 0];
    }
    // The sharpening and blur steps of before (off, low/soft, high/strong) on the bars.
    if (c->sharpLevel < 0 || c->sharpLevel > 100) c->sharpLevel = (c->sharpen >= 0 && c->sharpen <= 2 ? c->sharpen : 0) * 50;
    if (c->blurLevel < 0 || c->blurLevel > 100) c->blurLevel = (c->blur >= 0 && c->blur <= 2 ? c->blur : 0) * 50;
    if (shotSeen && !hkShotSeen)
    {
        // A file from before the shortcuts page: its screenshot combo (two pad sources) as one.
        static const u32 bit[H64_SRC_LTHUMB + 2] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, HK_LT, HK_RT, 0x0010,
                                                       0x0001, 0x0002, 0x0004, 0x0008, 0x0040, 0x0080 };
        u32 m = 0;
        if (c->shotBtn1 >= 0 && c->shotBtn1 <= H64_SRC_RTHUMB) m |= bit[c->shotBtn1];
        if (c->shotBtn2 >= 0 && c->shotBtn2 <= H64_SRC_RTHUMB) m |= bit[c->shotBtn2];
        c->hotkey[HK_SHOT] = c->shotBtn1 >= 0 && c->shotBtn2 >= 0 ? m : 0;
    }
    return 1;
}

void ConfigHotkeyDefaults(Config *c)
{
    c->hotkey[HK_SAVE] = 0x0020 | 0x0200;        // BACK + RB
    c->hotkey[HK_LOAD] = 0x0020 | 0x0100;        // BACK + LB
    c->hotkey[HK_SLOT_PREV] = 0x0020 | 0x0004;   // BACK + D-pad left
    c->hotkey[HK_SLOT_NEXT] = 0x0020 | 0x0008;   // BACK + D-pad right
    c->hotkey[HK_SHOT] = 0;
}

int ConfigWriteMenuKeys(const Config *c, const char *path, int withLastRom)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return 0;
    fprintf(f, "# Written by the Harissa64 V2 menu\nsettingsver=2\ncpu=%s\nhle=%d\naudioms=%d\nsmooth=%d\nshowfps=%d\n", c->cpu, c->hle, c->audioMs,
            c->smooth, c->perfItems ? 1 : 0);
    fprintf(f, "perfitems=%d\nperfpos=%d\nperfsize=%d\nperfcolor=%d\nperfbox=%d\nperffahrenheit=%d\n", c->perfItems, c->perfPos, c->perfSize,
            c->perfColor, c->perfBox, c->perfFahrenheit);
    fprintf(f, "res43=%d\nres169=%d\nreswide=%d\nwallrec=%d\ndesctr=%d\n", c->res43, c->res169, c->resWide, c->wallRecommend, c->descTranslate);
    fprintf(f, "texfilter=%d\nsharpen=%d\nblur=%d\nscreen=%d\nbrightness=%d\nvsync=%d\nstretch=%d\ndeflicker=%d\n",
            c->texFilter, c->sharpen, c->blur, c->screen, c->brightness, c->vsync, c->stretch, c->deflicker);
    fprintf(f, "hirestex=%d\n", c->hiTex);
    if (c->hiTexFile[0]) fprintf(f, "hirestexfile=%s\n", c->hiTexFile);
    fprintf(f, "shotbtn1=%s\nshotbtn2=%s\n", c->shotBtn1 >= 0 ? h64_padmap_source_name(c->shotBtn1) : "none",
            c->shotBtn2 >= 0 ? h64_padmap_source_name(c->shotBtn2) : "none");
    fprintf(f, "zoom=%d\nposx=%d\nposy=%d\nrumble=%d\n", c->zoom, c->offsetX, c->offsetY, c->rumble);
    fprintf(f, "devmode=%d\ngfxlog=%d\ntexr3=%d\n", c->devMode, c->gfxLog, c->texR3);
    fprintf(f, "setview=%d\n", c->settingsView);
    fprintf(f, "panelcolor=%d\nintro=%d\nbrightlevel=%d\nsharplevel=%d\nblurlevel=%d\n", c->panelColor, c->intro, c->brightLevel,
            c->sharpLevel, c->blurLevel);
    fprintf(f, "hk_save=%X\nhk_load=%X\nhk_slotprev=%X\nhk_slotnext=%X\nhk_shot=%X\n", c->hotkey[HK_SAVE], c->hotkey[HK_LOAD],
            c->hotkey[HK_SLOT_PREV], c->hotkey[HK_SLOT_NEXT], c->hotkey[HK_SHOT]);
    fprintf(f, "padloss=%d\nautosaveslot=%d\nbatterywarn=%d\nuisounds=%d\n", c->padLoss, c->autoSaveSlot, c->batteryWarn, c->uiSounds);
    if (withLastRom) fprintf(f, "artdownload=%d\nartextras=%d\nartbg=%d\nbrowserdesign=%d\nbgeffect=%d\n", c->artDownload, c->artExtras, c->artBackground, c->browserDesign, c->bgEffect);
    if (withLastRom)
        fprintf(f, "fxcolor1=%d\nfxcolor2=%d\nfxcolor3=%d\nfxopacity=%d\nfxspeed=%d\n", c->fxColor[0], c->fxColor[1], c->fxColor[2],
                c->fxOpacity, c->fxSpeed);
    if (withLastRom)
        fprintf(f, "selcolor=%d\nwallinterval=%d\nwalltransition=%d\nwalldim=%d\nsavermode=%d\nsavertime=%d\n", c->selColor, c->wallInterval,
                c->wallTransition, c->wallDim, c->saverMode, c->saverTime);
    if (withLastRom) fprintf(f, "wallcustom=%d\nuisoundstyle=%d\nartkind=%d\n", c->wallCustom, c->uiSoundStyle, c->artKind);
    fprintf(f, "uifont=%d\ncolorstyle=%d\n", c->uiFont, c->colorStyle);
    {
        static const char *const keys[CONFIG_PATH_COUNT] = { "romdir", "musicdir", "sounddir", "walldir", "savedir", "shotdir", "texdir" };
        int k;
        for (k = 0; k < CONFIG_PATH_COUNT; k++)
            if (c->path[k][0]) fprintf(f, "%s=%s\n", keys[k], c->path[k]);
    }
    if (withLastRom && c->wallFile[0]) fprintf(f, "wallfile=%s\n", c->wallFile);
    if (withLastRom)
        fprintf(f, "wallfit=%d\nwallshuffle=%d\nbordercolor=%d\nborderopacity=%d\nborderwidth=%d\nborderglow=%d\npanelopacity=%d\npanelround=%d\n",
                c->wallFit, c->wallShuffle, c->borderColor, c->borderOpacity, c->borderWidth, c->borderGlow, c->panelOpacity, c->panelRound);
    if (withLastRom && c->skinName[0]) fprintf(f, "skin=%s\n", c->skinName);
    if (withLastRom) fprintf(f, "volumegame=%d\nvolumemusic=%d\nvolumesfx=%d\nmusicmode=%d\n", c->volumeGame, c->volumeMusic, c->volumeSfx, c->musicMode);
    if (withLastRom && c->musicTrack[0]) fprintf(f, "musictrack=%s\n", c->musicTrack);
    if (withLastRom) fprintf(f, "startrommode=%d\n", c->startRomMode);
    if (withLastRom && c->startRom[0]) fprintf(f, "startrom=%s\n", c->startRom);
    fprintf(f, "audiomono=%d\n", c->audioMono);
    // Controls: "map=default" first (a game's profile then undoes custom
    // controls of settings.ini), then every key of a changed mapping.
    for (i = 0; i < 4; i++)
    {
        const H64PadMap *m = &c->padMap[i];
        char pre[8];
        int k;
        pre[0] = 0;
        if (i) sprintf(pre, "p%d_", i + 1);
        fprintf(f, "%smap=default\n", pre);
        if (h64_padmap_is_default(m)) continue;
        for (k = 0; k < H64_N64_COUNT; k++)
        {
            char src[256];
            h64_padmap_format_sources(m->src[k], src, sizeof(src));
            fprintf(f, "%smap_%s=%s\n", pre, h64_padmap_n64_name(k), src);
        }
        fprintf(f, "%smap_stick=%s\n", pre,
                m->stick == H64_STICK_RIGHT ? "right" : m->stick == H64_STICK_DPAD ? "dpad" : m->stick == H64_STICK_NONE ? "none" : "left");
        for (k = 0; k < 2; k++)
        {
            const H64StickCal *cal = &m->cal[k];
            const char lr = k ? 'r' : 'l';
            fprintf(f, "%smap_%ccenter=%d,%d\n%smap_%cdeadzone=%d\n%smap_%couter=%d\n%smap_%crange=%d\n%smap_%ccurve=%d\n%smap_%cinvert=%s\n",
                    pre, lr, cal->cx, cal->cy, pre, lr, cal->deadzone, pre, lr, cal->outer, pre, lr, cal->range, pre, lr, cal->curve,
                    pre, lr, cal->invert == 3 ? "xy" : cal->invert == 2 ? "y" : cal->invert == 1 ? "x" : "none");
        }
        fprintf(f, "%smap_drift=%d\n", pre, m->drift);
    }
    if (withLastRom && c->lastRom[0]) fprintf(f, "lastrom=%s\n", c->lastRom);
    if (withLastRom && c->lang[0]) fprintf(f, "lang=%s\n", c->lang);
    fclose(f);
    return 1;
}

int ConfigWriteLang(const char *path, const char *code)
{
    static char in[16384], out[16384 + 64];
    size_t len = 0, used = 0, pos = 0;
    int found = 0;
    FILE *f = fopen(path, "rb");
    if (f)
    {
        len = fread(in, 1, sizeof(in), f);
        fclose(f);
        if (len == sizeof(in)) return 0;   // not a settings file the menu wrote
    }
    while (pos < len)
    {
        // One line (with its end of line, if any): copied, or replaced when it is lang=.
        size_t end = pos;
        while (end < len && in[end] != '\n') end++;
        if (end < len) end++;
        if (end - pos >= 5 && !memcmp(in + pos, "lang=", 5))
        {
            if (!found) used += sprintf(out + used, "lang=%s\n", code);
            found = 1;
        }
        else
        {
            memcpy(out + used, in + pos, end - pos);
            used += end - pos;
            if (end == len && out[used - 1] != '\n') out[used++] = '\n';
        }
        pos = end;
    }
    if (!found) used += sprintf(out + used, "lang=%s\n", code);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(out, 1, used, f) != used) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

const char *ConfigPath(const Config *c, int which, const char *drive)
{
    static const char *const names[CONFIG_PATH_COUNT] = { "roms", "music", "sounds", "wallpapers", "saves", "screenshots", "hires_texture" };
    static char usual[CONFIG_PATH_COUNT][32];
    if (which < 0 || which >= CONFIG_PATH_COUNT) return "";
    if (c && c->path[which][0]) return c->path[which];
    // The ROMs, music, sounds, wallpapers and textures sit next to the emulator; saves and screenshots on `drive`.
    _snprintf(usual[which], sizeof(usual[which]) - 1, "%s:\\%s",
              which <= CONFIG_PATH_WALLS || which == CONFIG_PATH_TEXTURES || !drive || !drive[0] ? "game" : drive, names[which]);
    usual[which][sizeof(usual[which]) - 1] = 0;
    return usual[which];
}
