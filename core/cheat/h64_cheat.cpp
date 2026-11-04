// LAIKA 64 - cheats (see h64_cheat.h).

#include "h64_cheat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/h64_log.h"
#include "../system/h64_system.h"

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// The choices after a "????" value: 0001:"1 Lap",0002:"2 Laps".
static void parse_options(const char *s, H64Cheat *ch)
{
    while (*s)
    {
        H64CheatOption o;
        char *end;
        const char *q;
        s = skip_space(s);
        if (*s == ',') { s++; continue; }
        o.value = (u16)strtoul(s, &end, 16);
        if (end == s || *end != ':') break;
        s = end + 1;
        if (*s == '"')
        {
            q = strchr(s + 1, '"');
            if (!q) break;
            o.name.assign(s + 1, q - s - 1);
            s = q + 1;
        }
        else
        {
            q = strchr(s, ',');
            o.name.assign(s, q ? (size_t)(q - s) : strlen(s));
            s = q ? q : s + strlen(s);
        }
        ch->options.push_back(o);
    }
}

// One line of a cheat's body: a code ("8033B21D 0064", "8033B21D ???? 0001:..."); 0 when not one.
static int parse_code(const char *s, H64Cheat *ch)
{
    H64CheatCode c;
    char *end;
    s = skip_space(s);
    c.addr = (u32)strtoul(s, &end, 16);
    if (end - s != 8) return 0;
    s = skip_space(end);
    c.choice = 0;
    c.value = 0;
    if (!strncmp(s, "????", 4) || !strncmp(s, "??", 2))
    {
        c.choice = 1;
        s += s[2] == '?' ? 4 : 2;
        parse_options(s, ch);
    }
    else
    {
        c.value = (u16)strtoul(s, &end, 16);
        if (end == s) return 0;
    }
    ch->codes.push_back(c);
    return 1;
}

// The cheats of a file from where it is to the next "crc" line (or its end).
static int parse_cheats(FILE *f, H64Cheats *cs, int custom)
{
    char line[2048];
    int n = 0;
    H64Cheat *cur = NULL;
    while (fgets(line, sizeof(line), f))
    {
        const char *s;
        trim_end(line);
        s = skip_space(line);
        if (!strncmp(s, "crc ", 4)) { if (!custom) break; continue; }
        if (!strncmp(s, "gn ", 3)) { if (!custom) cs->game = s + 3; continue; }
        if (!strncmp(s, "cn ", 3))
        {
            H64Cheat ch;
            ch.name = s + 3;
            ch.option = 0;
            ch.enabled = 0;
            ch.custom = custom;
            cs->list.push_back(ch);
            cur = &cs->list.back();
            n++;
            continue;
        }
        if (!cur) continue;
        if (!strncmp(s, "cd ", 3)) { cur->desc = s + 3; continue; }
        parse_code(s, cur);
    }
    // A cheat without codes (a header, a bad line) is left out.
    {
        size_t i = 0;
        while (i < cs->list.size())
            if (cs->list[i].codes.empty()) { cs->list.erase(cs->list.begin() + i); n--; }
            else i++;
    }
    return n;
}

int h64_cheats_load_db(H64Cheats *cs, const char *path, u32 crc1, u32 crc2, u8 country)
{
    char line[2048], want[40], wantAny[32];
    FILE *f = fopen(path, "r");
    long anyAt = -1;
    int n = 0;
    if (!f) return 0;
    _snprintf(want, sizeof(want) - 1, "crc %08X-%08X-C:%02X", crc1, crc2, country);
    want[sizeof(want) - 1] = 0;
    _snprintf(wantAny, sizeof(wantAny) - 1, "crc %08X-%08X-", crc1, crc2);
    wantAny[sizeof(wantAny) - 1] = 0;
    while (fgets(line, sizeof(line), f))
    {
        trim_end(line);
        if (!_strnicmp(line, want, strlen(want))) { n = parse_cheats(f, cs, 0); anyAt = -2; break; }
        if (anyAt == -1 && !_strnicmp(line, wantAny, strlen(wantAny))) anyAt = ftell(f);   // the same ROM, another country byte
    }
    if (anyAt >= 0 && !fseek(f, anyAt, SEEK_SET)) n = parse_cheats(f, cs, 0);
    fclose(f);
    if (n) H64_INFO("[cheat] %s: %d cheats in the database", cs->game.c_str(), n);
    return n;
}

int h64_cheats_load_custom(H64Cheats *cs, const char *path)
{
    FILE *f = fopen(path, "r");
    int n;
    if (!f) return 0;
    n = parse_cheats(f, cs, 1);
    fclose(f);
    if (n) H64_INFO("[cheat] %s: %d cheats of the user's", path, n);
    return n;
}

int h64_cheats_load_state(H64Cheats *cs, const char *path)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    int n = 0;
    size_t i;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
    {
        char *eq;
        trim_end(line);
        eq = strrchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        for (i = 0; i < cs->list.size(); i++)
            if (cs->list[i].name == line)
            {
                int o = atoi(eq + 1);
                cs->list[i].enabled = 1;
                cs->list[i].option = o >= 0 && o < (int)cs->list[i].options.size() ? o : 0;
                n++;
                break;
            }
    }
    fclose(f);
    return n;
}

int h64_cheats_save_state(const H64Cheats *cs, const char *path)
{
    FILE *f = fopen(path, "w");
    size_t i;
    if (!f) return 0;
    for (i = 0; i < cs->list.size(); i++)
        if (cs->list[i].enabled) fprintf(f, "%s=%d\n", cs->list[i].name.c_str(), cs->list[i].option);
    fclose(f);
    return 1;
}

int h64_cheats_enabled(const H64Cheats *cs)
{
    size_t i;
    int n = 0;
    for (i = 0; i < cs->list.size(); i++) n += cs->list[i].enabled != 0;
    return n;
}

static u32 rd(H64System *sys, u32 a, int bytes)
{
    a &= H64_RDRAM_SIZE - 1;
    return bytes == 1 ? sys->rdram[a] : (u32)sys->rdram[a] << 8 | sys->rdram[(a + 1) & (H64_RDRAM_SIZE - 1)];
}

static void wr(H64System *sys, u32 a, u32 v, int bytes)
{
    a &= 0x00FFFFFF;
    if (a + bytes > H64_RDRAM_SIZE) return;
    if (bytes == 1) sys->rdram[a] = (u8)v;
    else { sys->rdram[a] = (u8)(v >> 8); sys->rdram[a + 1] = (u8)v; }
    h64_jit_notify_write(sys, a, bytes);   // code written (rare): its blocks compiled again
}

// One code; returns 0 when the code after it is to be skipped (a condition not met).
static int run_code(H64System *sys, u32 word, u32 value, int boot)
{
    u32 type = word >> 24, a = word & 0x00FFFFFF;
    switch (type)
    {
    case 0x80: case 0xA0: case 0x88: wr(sys, a, value & 0xFF, 1); break;
    case 0x81: case 0xA1: case 0x89: wr(sys, a, value, 2); break;
    case 0xF0: if (boot) wr(sys, a, value & 0xFF, 1); break;
    case 0xF1: if (boot) wr(sys, a, value, 2); break;
    case 0xD0: return rd(sys, a, 1) == (value & 0xFF);
    case 0xD1: return rd(sys, a, 2) == value;
    case 0xD2: return rd(sys, a, 1) != (value & 0xFF);
    case 0xD3: return rd(sys, a, 2) != value;
    }
    return 1;
}

void h64_cheats_apply(H64Cheats *cs, H64System *sys)
{
    size_t i, k;
    int boot = !cs->booted;
    for (i = 0; i < cs->list.size(); i++)
    {
        const H64Cheat *ch = &cs->list[i];
        if (!ch->enabled) continue;
        for (k = 0; k < ch->codes.size(); k++)
        {
            const H64CheatCode *c = &ch->codes[k];
            u32 v = c->choice && !ch->options.empty() ? ch->options[ch->option].value : c->value;
            if ((c->addr >> 24) == 0x50 && k + 1 < ch->codes.size())
            {
                // The repeater: the next code nn times, its address + ss and its value + vvvv each time.
                const H64CheatCode *nx = &ch->codes[k + 1];
                u32 count = (c->addr >> 8) & 0xFF, step = c->addr & 0xFF, j;
                u32 addr = nx->addr, val = nx->choice && !ch->options.empty() ? ch->options[ch->option].value : nx->value;
                for (j = 0; j < count; j++)
                {
                    run_code(sys, addr, val & 0xFFFF, boot);
                    addr = (addr & 0xFF000000u) | ((addr + step) & 0x00FFFFFF);
                    val += v;
                }
                k++;
                continue;
            }
            if (!run_code(sys, c->addr, v, boot)) k++;   // a condition not met: its code skipped
        }
    }
    cs->booted = 1;
}
