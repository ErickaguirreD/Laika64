// LAIKA 64 - cheats: GameShark (Action Replay) codes written into RDRAM.
//
// The codes of a game come from the mupen64plus database (mupencheat.txt: a
// "crc CRC1-CRC2-C:country" line per ROM, then "cn" name, "cd" description and
// "ADDRESS VALUE" lines, VALUE "????" with its choices after it) and from the
// user's own file in the same format (no crc line needed). Each enabled cheat
// is applied once a frame (h64_cheats_apply), as the GameShark did:
//   80/A0 aaaaaa 00vv   write 8 bits         81/A1 aaaaaa vvvv   write 16 bits
//   88/89               the same (the GameShark button's codes: always on)
//   D0/D1 aaaaaa vvvv   the next code only when the 8/16 bits there are vvvv
//   D2/D3               ... only when they are not
//   50 00nnss vvvv      the next code nn times, address + ss, value + vvvv each time
//   F0/F1               written once, at the start (boot patches)
// Others (FF, EE, DE, 2A...: the GameShark's own settings) are left out.
#ifndef H64_CHEAT_H
#define H64_CHEAT_H

#include <string>
#include <vector>

#include "../common/h64_types.h"

struct H64System;

struct H64CheatCode
{
    u32 addr;     // the code's first word: type (top byte) and address
    u16 value;
    int choice;   // the value is the chosen option's ("????")
};

struct H64CheatOption
{
    u16 value;
    std::string name;
};

struct H64Cheat
{
    std::string name;   // "Infinite\Lives": a group and its cheat
    std::string desc;
    std::vector<H64CheatCode> codes;
    std::vector<H64CheatOption> options;   // the choices of its "????" value (none: a plain cheat)
    int option;         // the one chosen
    int enabled;
    int custom;         // from the user's own file
};

struct H64Cheats
{
    std::vector<H64Cheat> list;
    std::string game;   // the database's name of the game ("gn")
    int booted;         // the F0/F1 codes written
};

// The cheats of a ROM (its header's CRC1, CRC2 and country byte) from the database file;
// returns how many. The user's own file adds its cheats (crc lines there are ignored).
int h64_cheats_load_db(H64Cheats *cs, const char *path, u32 crc1, u32 crc2, u8 country);
int h64_cheats_load_custom(H64Cheats *cs, const char *path);

// Which are on, kept per game ("name=option" a line, for the enabled ones).
int h64_cheats_load_state(H64Cheats *cs, const char *path);
int h64_cheats_save_state(const H64Cheats *cs, const char *path);
int h64_cheats_enabled(const H64Cheats *cs);

// The enabled cheats written into RDRAM (once a frame).
void h64_cheats_apply(H64Cheats *cs, H64System *sys);

#endif
