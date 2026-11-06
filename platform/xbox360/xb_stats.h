// LAIKA 64 - anonymous play statistics (Settings > System > Anonymous statistics).
//
// With the user's consent (Config::statsShare, asked once), leaving a game sends
// a short summary to a Google Form: the game (header name, game code, CRC), the
// version, the frames per second, where the time went, the main settings, how
// the session ended, the one-time "how did it run?" vote, and a random console id
// (to count consoles, not people). Nothing personal (no file names, saves or
// accounts). A Google Form takes a plain GET of its formResponse address, so no
// key or token ships in the executable.
//
// A session that never returns leaves its summary in <drive>:\config\stats_pending.txt,
// sent as "sin cerrar" at the next start: a freeze, or a game left by the Guide button
// (the two cannot be told apart).
#ifndef XB_STATS_H
#define XB_STATS_H

#include <stddef.h>
#include "../../core/common/h64_types.h"

// One game session's numbers (filled by main.cpp's RunGame).
struct StatsSession
{
    char game[24];      // the ROM header's name
    char code[24];      // "NSME / 635A2BFF": game code and CRC 1
    char settings[64];  // "1366x768 vsync0 hd1"
    char console[12];   // StatsConsoleId
    double fpsSum;      // the per-second FPS samples counted, and their sum
    u32 seconds;
    double fpsMin;
    u32 below;          // samples under 92 % of the game's refresh rate
    double runMs, gfxMs, texMs, copyMs;   // totals, divided by `frames` when sent
    u32 frames;
    DWORD startTick;
};

void StatsSessionStart(StatsSession *s);
// This console's id: 8 random hex digits made once and kept in <drive>:\config\stats_console.txt.
void StatsConsoleId(const char *drive, char *out, size_t size);
// Played long enough to be worth sending (a minute).
int StatsSessionWorth(const StatsSession *s);
// Minutes played so far.
int StatsSessionMinutes(const StatsSession *s);

// Sends the summary on a background thread (returns at once). `vote`, `reason`: "" when not asked.
void StatsSend(const StatsSession *s, const char *ending, const char *vote, const char *reason);
// Settings > Report a game: a report sent at once without playing ("reporte" as the
// ending, no FPS), whatever the statistics setting - the user sends it. `code`: "NSME / 635A2BFF".
void StatsSendReport(const char *drive, const char *game, const char *code, const char *vote, const char *reason);
// Waits (up to `ms`) for the summaries still being sent: before leaving for the dashboard.
void StatsWaitSent(int ms);

// The freeze marker: written now and then while a game runs, removed when it ends normally.
void StatsPendingWrite(const char *drive, const StatsSession *s);
void StatsPendingClear(const char *drive);
// At start: a marker left by a session that never ended is sent (`send` 1) or dropped.
void StatsPendingFlush(const char *drive, int send);

// The "how did it run?" vote is asked once per game: <drive>:\config\stats_voted.txt.
int StatsVoted(const char *drive, const char *code);
void StatsMarkVoted(const char *drive, const char *code);

#endif
