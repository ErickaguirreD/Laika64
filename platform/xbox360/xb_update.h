// Harissa64 - is a newer release on GitHub? (checked once, in the background)

#ifndef XB_UPDATE_H
#define XB_UPDATE_H

#include <stddef.h>

// Starts the check (first call only): a thread asks GitHub for the latest
// release and compares it with `current` (H64_VERSION_STRING).
void UpdateCheckStart(const char *current);

// 1 (once) when the check found a newer release: its version ("2.0.2") goes
// to `version`.
int UpdateCheckTake(char *version, size_t size);

// The updater: downloads the release's default.xex (and its .sha1) in a thread,
// checks it (an XEX2 file of a sane size whose SHA-1 matches), then puts it in
// place of game:\default.xex, the running one kept as default.xex.bak.
// UpdateInstallState: 0 not started, 1 working, 2 installed (restart now),
// -1 failed (`why`: a short reason for the log and the screen).
void UpdateInstallStart(const char *version);
int UpdateInstallState(char *why, size_t size);

#endif
