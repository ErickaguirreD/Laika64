// LAIKA 64 - libdragon HLE: commands of libdragon's RSP queue run in C (see h64_rsp_libdragon.cpp).
#ifndef H64_RSP_LIBDRAGON_H
#define H64_RSP_LIBDRAGON_H

#include "../common/h64_types.h"

struct H64System;

// The rspq loop jumps to a command at IMEM `entry` (first byte of the command: cmdByte; its words are in a0-a3).
// 1 when it ran in C (the RSP goes on at RSPQ_Loop; *rcpCycles: its time), 0 when the LLE RSP runs it.
int h64_rsp_libdragon_command(H64System *sys, u32 entry, u32 cmdByte, u32 *rcpCycles);
// IMEM was written (an SP DMA): commands are looked for again.
void h64_rsp_libdragon_imem_written(void);

#endif
