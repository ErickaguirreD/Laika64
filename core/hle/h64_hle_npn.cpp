// LAIKA 64: Pokemon Puzzle League's movie decoder (OSTask type 7, microcode
// byte sum 0xD33E, 0x5D0 bytes). Its intro is decoded by this task straight
// into the frames the VI shows; on the LLE RSP each frame took 100-280 ms on
// the console (the intro ran at 18-27 VI/s). GitHub issue #11.
//
// What the microcode does: macroblocks of four 4x4 luma blocks (two when the
// macroblock is 8x4) and a 4x4 U and V block, each coded as a flat colour plus
// gain-shape vectors taken from DMEM, a flat colour plus a residual, 16 raw
// pixels, or 4 weighted base patterns; a macroblock can be skipped (the frame
// before stays). Then YUV to RGBA5551, written by DMA into the frame.
//
// This is the microcode translated instruction by instruction (IMEM addresses
// in the comments), with the vector unit's exact semantics
// (core/rsp/h64_rsp_vu.cpp) for the instructions it uses, so the frames come
// out the same. The vector carry flags are clear after its first VSUB and only
// VADD/VSUB/VGE/VLT touch them, so those compare and add without them.
#include <string.h>

#include "h64_hle_internal.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

typedef u16 Vec[8];

struct NpnCtx
{
    H64System *sys;
    struct hle_t *hle;
    u8 *dm;                  // DMEM
    Vec v[32];
    s64 acc[8];              // the accumulator, 48 bits sign-extended
    s16 divin, divout;
    int divdp;
    // The task's scalar registers that cross subroutines
    u32 in, inEnd, bufA, bufB, src, t1, s3;
    u32 ins;                 // instructions run (an estimate, for the task's length)
    u32 outLo, outHi;        // the RDRAM written
};

static const u8 s_sel[16][8] = {
    { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 0, 2, 2, 4, 4, 6, 6 }, { 1, 1, 3, 3, 5, 5, 7, 7 },
    { 0, 0, 0, 0, 4, 4, 4, 4 }, { 1, 1, 1, 1, 5, 5, 5, 5 }, { 2, 2, 2, 2, 6, 6, 6, 6 }, { 3, 3, 3, 3, 7, 7, 7, 7 },
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 }, { 2, 2, 2, 2, 2, 2, 2, 2 }, { 3, 3, 3, 3, 3, 3, 3, 3 },
    { 4, 4, 4, 4, 4, 4, 4, 4 }, { 5, 5, 5, 5, 5, 5, 5, 5 }, { 6, 6, 6, 6, 6, 6, 6, 6 }, { 7, 7, 7, 7, 7, 7, 7, 7 } };

static s32 clamp16(s64 x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : (s32)x; }
static s64 clip48(s64 x) { return (s64)((u64)x << 16) >> 16; }

// ---- DMEM ----
static u32 dm8(NpnCtx *c, u32 a) { return c->dm[a & 0xFFF]; }
static u32 dm16(NpnCtx *c, u32 a) { return dm8(c, a) << 8 | dm8(c, a + 1); }
static u32 dm32(NpnCtx *c, u32 a) { return dm16(c, a) << 16 | dm16(c, a + 2); }

static void set_byte(u16 *v, u32 i, u8 b)
{
    i &= 15;
    if (i & 1) v[i >> 1] = (u16)((v[i >> 1] & 0xFF00) | b);
    else v[i >> 1] = (u16)((v[i >> 1] & 0x00FF) | (b << 8));
}
static u8 get_byte(const u16 *v, u32 i) { i &= 15; return (u8)(i & 1 ? v[i >> 1] : v[i >> 1] >> 8); }

// ---- Vector loads and stores (h64_rsp_lwc2 / h64_rsp_swc2) ----
static void lbv(NpnCtx *c, u32 vt, u32 e, u32 a) { set_byte(c->v[vt], e, (u8)dm8(c, a)); }
static void lsv(NpnCtx *c, u32 vt, u32 e, u32 a) { u32 k, end = e + 2 < 16 ? e + 2 : 16; for (k = e; k < end; k++) set_byte(c->v[vt], k, (u8)dm8(c, a++)); }
static void ldv(NpnCtx *c, u32 vt, u32 e, u32 a) { u32 k, end = e + 8 < 16 ? e + 8 : 16; for (k = e; k < end; k++) set_byte(c->v[vt], k, (u8)dm8(c, a++)); }
static void lqv(NpnCtx *c, u32 vt, u32 a)
{
    u32 k, end = 16 - (a & 15);
    for (k = 0; k < end; k++) set_byte(c->v[vt], k, (u8)dm8(c, a++));
}
static void lpuv(NpnCtx *c, u32 vt, u32 a, u32 shift)   // LPV (8), LUV (7); element 0
{
    u32 index = a & 7, k;
    a &= ~7u;
    for (k = 0; k < 8; k++) c->v[vt][k] = (u16)(dm8(c, a + ((index + k) & 15)) << shift);
}
static void sqv(NpnCtx *c, u32 vt, u32 a)
{
    u32 k, end = 16 - (a & 15);
    for (k = 0; k < end; k++) c->dm[(a++) & 0xFFF] = get_byte(c->v[vt], k);
}

// ---- Vector computations (op_vector in h64_rsp_vu.cpp) ----
static void vsel(Vec out, const u16 *t, u32 e) { u32 n; for (n = 0; n < 8; n++) out[n] = t[s_sel[e][n]]; }

static void vadd(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = (u16)clamp16((s16)c->v[s][n] + (s16)te[n]);
}
static void vsub(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = (u16)clamp16((s16)c->v[s][n] - (s16)te[n]);
}
static void vzero(NpnCtx *c, u32 d) { memset(c->v[d], 0, sizeof(Vec)); }   // VSUB v,v,v (no carry)
static void vor(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = c->v[s][n] | te[n];
}
static void vand(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = c->v[s][n] & te[n];
}
static void vge(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = (s16)c->v[s][n] >= (s16)te[n] ? c->v[s][n] : te[n];
}
static void vlt(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++) c->v[d][n] = (s16)c->v[s][n] < (s16)te[n] ? c->v[s][n] : te[n];
}
static void vabs(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        s16 x = (s16)c->v[s][n];
        if (x < 0) c->v[d][n] = (s16)te[n] == -32768 ? 0x7FFF : (u16)-(s16)te[n];
        else if (x > 0) c->v[d][n] = te[n];
        else c->v[d][n] = 0;
    }
}
static void vmudm(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        s32 p = (s32)(s16)c->v[s][n] * (s32)te[n];
        c->acc[n] = p;
        c->v[d][n] = (u16)((u32)p >> 16);
    }
}
static void vmudn(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        s32 p = (s32)c->v[s][n] * (s32)(s16)te[n];
        c->acc[n] = p;
        c->v[d][n] = (u16)p;
    }
}
static void vmudl(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        u32 p = ((u32)c->v[s][n] * (u32)te[n]) >> 16;
        c->acc[n] = p;
        c->v[d][n] = (u16)p;
    }
}
static void vmudh(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        s32 p = (s32)(s16)c->v[s][n] * (s32)(s16)te[n];
        c->acc[n] = (s64)p << 16;
        c->v[d][n] = (u16)clamp16(p);
    }
}
static void vmadh(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        c->acc[n] = clip48(c->acc[n] + ((s64)((s32)(s16)c->v[s][n] * (s32)(s16)te[n]) << 16));
        c->v[d][n] = (u16)clamp16(c->acc[n] >> 16);
    }
}
static void vmadm(NpnCtx *c, u32 d, u32 s, u32 t, u32 e)
{
    Vec te; u32 n;
    vsel(te, c->v[t], e);
    for (n = 0; n < 8; n++)
    {
        c->acc[n] = clip48(c->acc[n] + (s64)((s32)(s16)c->v[s][n] * (s32)te[n]));
        c->v[d][n] = (u16)clamp16(c->acc[n] >> 16);
    }
}
static void vmov(NpnCtx *c, u32 d, u32 de, u32 t, u32 e) { Vec te; vsel(te, c->v[t], e); c->v[d][de] = te[de]; }
static void vrcph(NpnCtx *c, u32 d, u32 de, u32 t, u32 e)
{
    c->v[d][de] = (u16)c->divout;
    c->divdp = 1;
    c->divin = (s16)c->v[t][e & 7];
}
static void vrcpl(NpnCtx *c, u32 d, u32 de, u32 t, u32 e)
{
    s32 input = c->divdp ? (s32)((u32)(u16)c->divin << 16 | c->v[t][e & 7]) : (s32)(s16)c->v[t][e & 7];
    s32 mask = input >> 31, result;
    u32 data = (u32)(input ^ mask);
    if (input > -32768) data -= (u32)mask;
    if (data == 0) result = 0x7FFFFFFF;
    else if (input == -32768) result = (s32)0xFFFF0000u;
    else
    {
        u32 shift = 0, index, x = data;
        while (!(x & 0x80000000u)) { x <<= 1; shift++; }
        index = (u32)((((u64)data << shift) & 0x7FC00000u) >> 22);
        result = c->sys->rsp.reciprocals[index];
        result = (0x10000 | result) << 14;
        result = (result >> (31 - shift)) ^ mask;
    }
    c->divdp = 0;
    c->divout = (s16)(result >> 16);
    c->v[d][de] = (u16)result;
}

// ---- SP DMA (h64_sp_dma_event), done at once ----
static void dma_read(NpnCtx *c, u32 mem, u32 dram, u32 len)
{
    u32 i, k, bytes = (len & 0xFF8) + 8;
    mem &= 0xFF8; dram &= 0xFFFFF8;
    for (i = 0; i < bytes; i += 8, mem = (mem + 8) & 0xFF8, dram = (dram + 8) & 0xFFFFF8)
        for (k = 0; k < 8; k++) c->dm[mem + k] = dram + k < H64_RDRAM_SIZE ? c->sys->rdram[dram + k] : 0;
}
static void dma_write(NpnCtx *c, u32 mem, u32 dram, u32 value)
{
    u32 bytes = (value & 0xFF8) + 8, count = ((value >> 12) & 0xFF) + 1, skip = (value >> 20) & 0xFF8, i, k, row;
    mem &= 0xFF8; dram &= 0xFFFFF8;
    if (dram < c->outLo) c->outLo = dram;
    if (dram + count * (bytes + skip) > c->outHi) c->outHi = dram + count * (bytes + skip);
    for (row = 0; row < count; row++)
    {
        hle_dram_dirty(c->hle, dram, bytes);
        for (i = 0; i < bytes; i += 8, mem = (mem + 8) & 0xFF8, dram = (dram + 8) & 0xFFFFF8)
            for (k = 0; k < 8; k++) if (dram + k < H64_RDRAM_SIZE) c->sys->rdram[dram + k] = c->dm[mem + k];
        dram = (dram + skip) & 0xFFFFF8;
    }
}

// 460 (and 49C): the stream's next 0x80 bytes once the current buffer is used up.
static void refill(NpnCtx *c)
{
    c->ins += 2;
    if (c->in != c->inEnd) return;
    c->in = c->bufB;
    c->inEnd = c->in + 0x80;
    c->bufB = c->bufA;
    c->bufA = c->in;
    dma_read(c, c->bufB, c->src, 0x7F);
    c->src += 0x80;
    c->ins += 12;
}

// 49C: a gain-shape vector from DMEM into v14/v15.
static void shape(NpnCtx *c)
{
    u32 t2, t3, kind;
    s32 gain;
    refill(c);
    t2 = dm16(c, c->in + 4) + 0x81;
    kind = dm8(c, c->in);
    t3 = dm16(c, c->in + 6);
    if (kind != 0)
    {
        // 4E8: every other byte (half the width), as is
        vzero(c, 14);
        lbv(c, 14, 1, t2); lbv(c, 14, 3, t2 + 2); lbv(c, 14, 5, t2 + 4); lbv(c, 14, 7, t2 + 6);
        t2 += t3;
        lbv(c, 14, 9, t2); lbv(c, 14, 11, t2 + 2); lbv(c, 14, 13, t2 + 4); lbv(c, 14, 15, t2 + 6);
        vzero(c, 15);
        t2 += t3;
        lbv(c, 15, 1, t2); lbv(c, 15, 3, t2 + 2); lbv(c, 15, 5, t2 + 4); lbv(c, 15, 7, t2 + 6);
        t2 += t3;
        lbv(c, 15, 9, t2); lbv(c, 15, 11, t2 + 2); lbv(c, 15, 13, t2 + 4); lbv(c, 15, 15, t2 + 6);
        c->ins += 27;
    }
    else
    {
        // 540: four rows of four bytes
        lpuv(c, 14, t2, 7); t2 += t3;
        lpuv(c, 16, t2, 7); t2 += t3;
        lpuv(c, 15, t2, 7); t2 += t3;
        lpuv(c, 17, t2, 7);
        vmov(c, 14, 4, 16, 8); vmov(c, 14, 5, 16, 9); vmov(c, 14, 6, 16, 10); vmov(c, 14, 7, 16, 11);
        vmov(c, 15, 4, 17, 8); vmov(c, 15, 5, 17, 9); vmov(c, 15, 6, 17, 10); vmov(c, 15, 7, 17, 11);
        vmudm(c, 14, 14, 2, 9);
        vmudm(c, 15, 15, 2, 9);
        c->ins += 23;
    }
    // 584: less its mean, scaled by gain / its largest value
    gain = (s32)(s16)dm16(c, c->in + 2);
    vadd(c, 16, 14, 15, 0);
    vadd(c, 16, 16, 16, 3);
    vadd(c, 16, 16, 16, 6);
    vadd(c, 16, 16, 16, 12);
    vadd(c, 16, 16, 3, 8);
    vmudm(c, 16, 16, 2, 10);
    gain = (s32)((u32)gain << 3);
    vsub(c, 14, 14, 16, 8);
    vsub(c, 15, 15, 16, 8);
    vabs(c, 16, 14, 14, 0);
    vabs(c, 17, 15, 15, 0);
    c->v[19][0] = (u16)gain;   // MTC2 element 0
    vge(c, 16, 16, 17, 0);
    vge(c, 16, 16, 16, 3);
    vge(c, 16, 16, 16, 6);
    vge(c, 16, 16, 16, 12);
    vrcph(c, 18, 0, 16, 8);
    vrcpl(c, 17, 0, 0, 8);
    vrcph(c, 18, 0, 0, 8);
    vmudn(c, 17, 17, 19, 8);
    vmadh(c, 18, 18, 19, 8);
    vmudm(c, 16, 1, 2, 12);
    vmadm(c, 16, 14, 17, 8);
    vmadh(c, 14, 14, 18, 8);
    vmudm(c, 16, 1, 2, 12);
    vmadm(c, 16, 15, 17, 8);
    vmadh(c, 15, 15, 18, 8);
    c->ins += 29;
}

// 350: one 4x4 block into DMEM s3 (0x20 bytes). 0: the macroblock is skipped.
static int block(NpnCtx *c)
{
    u32 t0, skip;
    c->ins += 3;
    vzero(c, 16);
    refill(c);
    t0 = dm8(c, c->in);
    skip = t0 & c->t1;
    c->t1 = t0 & 7;
    c->ins += 4;
    if (skip)
    {
        c->in += 8;   // 458: j 324
        c->ins += 2;
        return 0;
    }
    lbv(c, 16, 1, c->in + 1);
    if (t0 == 0)
    {
        // 3FC: four weighted base patterns
        vzero(c, 17);
        lbv(c, 17, 1, c->in + 2); lbv(c, 17, 3, c->in + 3); lbv(c, 17, 5, c->in + 4); lbv(c, 17, 7, c->in + 5);
        vmudh(c, 12, 10, 16, 8);
        vmadh(c, 12, 5, 17, 8);
        vmadh(c, 12, 6, 17, 9);
        vmadh(c, 12, 7, 17, 10);
        vmadh(c, 12, 8, 17, 11);
        vmudh(c, 13, 11, 16, 8);
        vmadh(c, 13, 5, 17, 8);
        vmadh(c, 13, 6, 17, 9);
        vmadh(c, 13, 8, 17, 10);
        vmadh(c, 13, 9, 17, 11);
        vadd(c, 12, 12, 2, 15);
        vadd(c, 13, 13, 2, 15);
        vmudm(c, 12, 12, 2, 11);
        vmudm(c, 13, 13, 2, 11);
        c->ins += 19;
    }
    else
    {
        t0 &= 0xF;
        vadd(c, 12, 0, 16, 8);
        c->ins += 3;
        if (t0 == 0)
        {
            // 3D8: 16 raw pixels
            c->in += 8; refill(c); lpuv(c, 12, c->in, 7);
            c->in += 8; refill(c); lpuv(c, 13, c->in, 7);
            vmudm(c, 12, 12, 2, 9);
            vmudm(c, 13, 13, 2, 9);
            c->ins += 7;
        }
        else
        {
            vadd(c, 13, 0, 16, 8);
            c->ins += 2;
            if (c->t1 != 0)
            {
                // 388: flat colour plus t1 shapes
                do
                {
                    c->in += 8;
                    shape(c);
                    vadd(c, 12, 12, 14, 0);
                    vadd(c, 13, 13, 15, 0);
                    c->t1--;
                    t0 &= 8;
                    c->ins += 6;
                } while (c->t1 != 0);
                c->ins += 2;
                if (t0 == 0) goto store;
            }
            // 3AC: plus a residual
            c->in += 8; refill(c); lpuv(c, 14, c->in, 8);
            c->in += 8; refill(c); lpuv(c, 15, c->in, 8);
            vmudm(c, 14, 14, 2, 8);
            vadd(c, 12, 12, 14, 0);
            vmudm(c, 15, 15, 2, 8);
            vadd(c, 13, 13, 15, 0);
            c->ins += 9;
        }
    }
store:
    // 448
    c->in += 8;
    sqv(c, 12, c->s3);
    sqv(c, 13, c->s3 + 0x10);
    c->ins += 4;
    return 1;
}

static void npn_run(NpnCtx *c)
{
    u32 t4, t5, t6, t7 = 0, t8, s0, s1, s2, s4, s5, s6, s7, n;
    H64Rsp *rsp = &c->sys->rsp;
    // 080: constants
    lqv(c, 2, 0x00);
    lqv(c, 3, 0x10);
    lqv(c, 4, 0x20);
    for (n = 0; n < 8; n++) c->v[0][n] = rsp->vcol[n] ? 0xFFFF : 0;   // VSUB v0,v0,v0 with the carry left by the task before
    vadd(c, 1, 0, 2, 13);
    for (n = 0; n < 7; n++) lpuv(c, 5 + n, 0x30 + n * 8, 8);
    for (n = 0; n < 7; n++) vmudm(c, 5 + n, 5 + n, 2, 8);
    // 118: the task's header, then the stream's first 0x100 bytes
    dma_read(c, 0x70, dm32(c, 0xFF0), 0xA74);
    t5 = dm32(c, 0x70);
    dma_read(c, 0xAF0, t5, 0xFF);
    t5 += 0x100;
    c->in = 0xAF0; c->inEnd = 0xB70; c->bufA = 0xAF0; c->bufB = 0xB70;
    c->src = t5;
    t6 = dm32(c, 0x74);
    s1 = dm16(c, 0x7E);
    s0 = 0;
    c->ins += 70;
    // 188
    vzero(c, 20);
    lbv(c, 20, 1, 0x80);
    s2 = dm16(c, 0x78);
    t4 = (s2 - 8) << 21;
    t8 = dm8(c, 0x7B);
    vadd(c, 20, 0, 20, 8);
    t4 |= ((t8 << 2) - 1) << 12;
    vmudm(c, 20, 20, 2, 9);
    t4 |= 0xF;
    t8--;
    s2 <<= 3;
    if (t8 != 0) s2 += s2;
    c->ins += 18;
    for (;;)
    {
        // 1D0 (a yield request is not looked at: the task ends before the CPU runs again)
        c->ins += 6;
        if (s0 == 0)
        {
            u32 rows = s1--;
            if (rows == 0) break;
            s0 = dm16(c, 0x7C);
            t7 = t6;
            t6 = t7 + s2;
            c->ins += 5;
        }
        // 1FC
        c->t1 = 0x80;
        s4 = 0xC70;
        c->ins += 4;
        if (t8 != 0)
        {
            s4 = 0xC50;
            c->s3 = 0xC50; if (!block(c)) goto skipped;
            c->s3 = 0xC90; if (!block(c)) goto skipped;
            c->ins += 5;
        }
        c->s3 = 0xC70; if (!block(c)) goto skipped;
        c->s3 = 0xCB0; if (!block(c)) goto skipped;
        c->s3 = 0xC30; if (!block(c)) goto skipped;
        c->s3 = 0xC10; if (!block(c)) goto skipped;
        c->ins += 10;
        // 23C: YUV to RGBA5551
        s5 = 0xCD0;
        s6 = 4;
        do
        {
            vzero(c, 24);
            vzero(c, 25);
            lsv(c, 24, 0, c->s3 + 0x20); lsv(c, 24, 4, c->s3 + 0x22); lsv(c, 24, 8, c->s3 + 0x24); lsv(c, 24, 12, c->s3 + 0x26);
            lsv(c, 25, 0, c->s3 + 0x00); lsv(c, 25, 4, c->s3 + 0x02); lsv(c, 25, 8, c->s3 + 0x04); lsv(c, 25, 12, c->s3 + 0x06);
            s7 = dm8(c, 0x7B);
            vor(c, 24, 24, 24, 2);
            vor(c, 25, 25, 25, 2);
            vsub(c, 24, 24, 3, 11);
            vsub(c, 25, 25, 3, 11);
            vmudh(c, 28, 24, 4, 8);
            vmudh(c, 27, 24, 4, 9);
            vmadh(c, 27, 25, 4, 10);
            vmudh(c, 26, 25, 4, 11);
            c->ins += 19;
            do
            {
                ldv(c, 23, 0, s4);
                ldv(c, 23, 8, s4 + 0x40);
                s4 += 8;
                vmudh(c, 23, 23, 3, 10);
                vadd(c, 23, 23, 3, 9);
                vadd(c, 29, 23, 26, 0);
                vadd(c, 30, 23, 27, 0);
                vadd(c, 31, 23, 28, 0);
                vge(c, 29, 29, 0, 0);
                vge(c, 30, 30, 0, 0);
                vge(c, 31, 31, 0, 0);
                vlt(c, 29, 29, 4, 12);
                vlt(c, 30, 30, 4, 12);
                vlt(c, 31, 31, 4, 12);
                vand(c, 29, 29, 4, 13);
                vand(c, 30, 30, 4, 13);
                vand(c, 31, 31, 4, 13);
                vmudn(c, 29, 29, 2, 15);
                vmudl(c, 30, 30, 2, 11);
                vmudl(c, 31, 31, 2, 8);
                vor(c, 29, 29, 20, 0);
                vor(c, 29, 29, 30, 0);
                vor(c, 29, 29, 31, 0);
                sqv(c, 29, s5);
                s5 += 0x10;
                c->ins += 27;
            } while (--s7 != 0);
            c->s3 += 8;
            c->ins += 3;
        } while (--s6 != 0);
        dma_write(c, 0xCD0, t7, t4);
        c->ins += 10;
        // 324
    skipped:
        s0--;
        t7 += 0x10;
        c->ins += 3;
    }
    c->ins += 10;
}

// ---- The same with the microcode's constants folded in ----
// npn_run above, instruction for instruction, costs 30-90 ms a frame on the
// console (each vector result stored and read straight back). With the task
// data below (DMEM 0x00-0x6F) and no carry left in VCO, its vectors reduce to
// this integer arithmetic, lane by lane; DMEM is written as the microcode does
// (the shapes may be read from anywhere in it).
static const u8 s_npnData[0x70] = {
    0x01, 0x00, 0x02, 0x00, 0x10, 0x00, 0x20, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04,
    0x00, 0x08, 0x00, 0x20, 0x00, 0x40, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x71, 0xFF, 0xEA, 0xFF, 0xD2, 0x00, 0x5A, 0x3F, 0xC0, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0xFF, 0xFF, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x02, 0xFF, 0xFF, 0x00, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x02, 0x06, 0x08, 0x08, 0x06, 0x08, 0x0A, 0x0A, 0x08,
    0x08, 0x0A, 0x0A, 0x08, 0x06, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
// v5..v11 (LPV, then VMUDM by 0x100: the signed bytes at 0x30-0x67)
static const s32 s_p5[8] = { 2, 0, -1, -1, 2, 0, -1, -1 }, s_p6[8] = { -1, -1, 0, 2, -1, -1, 0, 2 },
                 s_p7[8] = { 2, 2, 2, 2, 0, 0, 0, 0 }, s_p9[8] = { 0, 0, 0, 0, 2, 2, 2, 2 },
                 s_p10[8] = { 6, 8, 8, 6, 8, 10, 10, 8 }, s_p11[8] = { 8, 10, 10, 8, 6, 8, 8, 6 };   // v8: all -1

static s32 c16(s32 x) { return x < -32768 ? -32768 : x > 32767 ? 32767 : x; }

// 49C, folded: the shape into x[16]
static void shape_fast(NpnCtx *c, s32 *x)
{
    u32 t2, t3, r, k, m = 0;
    s32 gain, sum = 0, mean, g8, a, khi, rhi, rlo;
    const u8 *d = c->dm;
    refill(c);
    t2 = dm16(c, c->in + 4) + 0x81;
    t3 = dm16(c, c->in + 6);
    gain = (s16)dm16(c, c->in + 2);
    if (d[c->in & 0xFFF])
    {
        for (r = 0; r < 4; r++, t2 += t3)
            for (k = 0; k < 4; k++) x[r * 4 + k] = d[(t2 + k * 2) & 0xFFF];   // LBV: every other byte
    }
    else
    {
        for (r = 0; r < 4; r++, t2 += t3)
            for (k = 0; k < 4; k++) x[r * 4 + k] = d[(t2 + k) & 0xFFF];       // LUV, VMUDM by 0x200: the bytes
    }
    for (k = 0; k < 16; k++) sum += x[k];
    mean = (sum + 8) >> 4;                       // VADD 8, VMUDM by 0x1000
    for (k = 0; k < 16; k++)
    {
        u32 v;
        x[k] -= mean;
        v = (u32)(x[k] < 0 ? -x[k] : x[k]);
        if (v > m) m = v;
    }
    // VRCPH/VRCPL of m << 16
    if (m == 0) { rlo = 0xFFFF; rhi = 0x7FFF; }
    else
    {
        u32 data = m << 16, shift = 0, index, t = data;
        s32 result;
        while (!(t & 0x80000000u)) { t <<= 1; shift++; }
        index = (u32)((((u64)data << shift) & 0x7FC00000u) >> 22);
        result = (s32)((0x10000u | c->sys->rsp.reciprocals[index]) << 14) >> (31 - shift);
        rlo = (u16)result;
        rhi = (s16)(result >> 16);
        c->divout = (s16)(result >> 16);
    }
    if (m == 0) c->divout = 0x7FFF;
    c->divin = 0; c->divdp = 1;
    g8 = (s16)(u16)((u32)gain << 3);
    a = rlo * g8;                                                   // VMUDN
    khi = c16((s32)(((s64)a + ((s64)(rhi * g8) << 16)) >> 16));    // VMADH
    for (k = 0; k < 16; k++)                                        // VMUDM 1 x 0x8000, VMADM, VMADH
        x[k] = c16((s32)((32768 + (s64)x[k] * (u16)a + ((s64)(x[k] * khi) << 16)) >> 16));
    c->ins += 52;
}

static void store_block(NpnCtx *c, const s32 *o)
{
    u32 k, a = c->s3;
    for (k = 0; k < 16; k++, a += 2) { c->dm[a & 0xFFF] = (u8)(o[k] >> 8); c->dm[(a + 1) & 0xFFF] = (u8)o[k]; }
}

// 350, folded: a block into o[16] (and DMEM s3). 0: the macroblock is skipped.
static int block_fast(NpnCtx *c, s32 *o)
{
    u32 t0, skip, k, dc;
    const u8 *d = c->dm;
    refill(c);
    t0 = d[c->in & 0xFFF];
    skip = t0 & c->t1;
    c->t1 = t0 & 7;
    c->ins += 7;
    if (skip)
    {
        c->in += 8;
        c->ins += 2;
        return 0;
    }
    dc = d[(c->in + 1) & 0xFFF];
    if (t0 == 0)
    {
        // 3FC: four weighted base patterns, +4, >>3
        s32 b0 = d[(c->in + 2) & 0xFFF], b1 = d[(c->in + 3) & 0xFFF], b2 = d[(c->in + 4) & 0xFFF], b3 = d[(c->in + 5) & 0xFFF];
        for (k = 0; k < 8; k++)
        {
            o[k] = c16(c16(s_p10[k] * (s32)dc + s_p5[k] * b0 + s_p6[k] * b1 + s_p7[k] * b2 - b3) + 4) >> 3;
            o[k + 8] = c16(c16(s_p11[k] * (s32)dc + s_p5[k] * b0 + s_p6[k] * b1 - b2 + s_p9[k] * b3) + 4) >> 3;
        }
        c->ins += 19;
    }
    else
    {
        t0 &= 0xF;
        c->ins += 3;
        if (t0 == 0)
        {
            // 3D8: 16 raw pixels
            c->in += 8; refill(c);
            for (k = 0; k < 8; k++) o[k] = d[(c->in + k) & 0xFFF];
            c->in += 8; refill(c);
            for (k = 0; k < 8; k++) o[k + 8] = d[(c->in + k) & 0xFFF];
            c->ins += 7;
        }
        else
        {
            for (k = 0; k < 16; k++) o[k] = (s32)dc;
            c->ins += 2;
            if (c->t1 != 0)
            {
                s32 x[16];
                do
                {
                    c->in += 8;
                    shape_fast(c, x);
                    for (k = 0; k < 16; k++) o[k] = c16(o[k] + x[k]);
                    c->t1--;
                    t0 &= 8;
                    c->ins += 6;
                } while (c->t1 != 0);
                c->ins += 2;
                if (t0 == 0) goto store;
            }
            // 3AC: plus a residual (signed bytes)
            c->in += 8; refill(c);
            for (k = 0; k < 8; k++) o[k] = c16(o[k] + (s8)d[(c->in + k) & 0xFFF]);
            c->in += 8; refill(c);
            for (k = 0; k < 8; k++) o[k + 8] = c16(o[k + 8] + (s8)d[(c->in + k) & 0xFFF]);
            c->ins += 9;
        }
    }
store:
    c->in += 8;
    store_block(c, o);
    c->ins += 4;
    return 1;
}

// One colour component: clamped to 0..0x3FC0, 5 bits kept (VGE, VLT, VAND).
static u32 comp5(s32 x) { return (u32)(x < 0 ? 0 : x < 0x3FC0 ? x : 0x3FC0) & 0x3E00; }

static void npn_run_fast(NpnCtx *c)
{
    u32 t4, t5, t6, t7 = 0, t8, s0, s1, s2, s7, alpha;
    s32 blk[6][16];   // by DMEM place: C10 U, C30 V, C50, C70, C90, CB0 luma
    dma_read(c, 0x70, dm32(c, 0xFF0), 0xA74);
    t5 = dm32(c, 0x70);
    dma_read(c, 0xAF0, t5, 0xFF);
    c->in = 0xAF0; c->inEnd = 0xB70; c->bufA = 0xAF0; c->bufB = 0xB70;
    c->src = t5 + 0x100;
    t6 = dm32(c, 0x74);
    s1 = dm16(c, 0x7E);
    s0 = 0;
    s2 = dm16(c, 0x78);
    t8 = dm8(c, 0x7B);
    alpha = dm8(c, 0x80) >> 7;                 // v20: VMUDM by 0x200
    t4 = ((s2 - 8) << 21) | (((t8 << 2) - 1) << 12) | 0xF;
    s7 = t8;
    s2 <<= 3;
    if (t8 != 1) s2 += s2;
    c->ins += 88;
    for (;;)
    {
        u32 r, j, x, y0, row;
        c->ins += 6;
        if (s0 == 0)
        {
            if (s1-- == 0) break;
            s0 = dm16(c, 0x7C);
            t7 = t6;
            t6 = t7 + s2;
            c->ins += 5;
        }
        c->t1 = 0x80;
        c->ins += 4;
        if (t8 != 1)
        {
            c->s3 = 0xC50; if (!block_fast(c, blk[2])) goto skipped;
            c->s3 = 0xC90; if (!block_fast(c, blk[4])) goto skipped;
            c->ins += 5;
        }
        c->s3 = 0xC70; if (!block_fast(c, blk[3])) goto skipped;
        c->s3 = 0xCB0; if (!block_fast(c, blk[5])) goto skipped;
        c->s3 = 0xC30; if (!block_fast(c, blk[1])) goto skipped;
        c->s3 = 0xC10; if (!block_fast(c, blk[0])) goto skipped;
        c->ins += 10;
        // 23C: YUV to RGBA5551 into DMEM 0xCD0, a row of 8 pixels each 16 bytes
        y0 = t8 != 1 ? 2 : 3;                  // the luma rows start at C50 (8 rows) or C70 (4)
        row = 0;
        for (r = 0; r < 4; r++)
        {
            s32 ca[4], cb[4], cc[4];
            for (j = 0; j < 4; j++)
            {
                s32 v = c16(blk[1][r * 4 + j] - 128), u = c16(blk[0][r * 4 + j] - 128);
                cc[j] = c16(v * 113);              // v28
                cb[j] = c16(v * -22 + u * -46);    // v27
                ca[j] = c16(u * 90);               // v26
            }
            c->ins += 19;
            for (j = 0; j < s7; j++, row++)
            {
                u32 a = 0xCD0 + row * 16;
                for (x = 0; x < 8; x++)
                {
                    // luma row `row` of the left (lanes 0-3) or right (4-7) blocks; the left
                    // column is C50 then C70, the right C90 then CB0 (+0x40)
                    u32 lr = (y0 == 2 ? row : row + 4), b = (lr >> 2) + (x >> 2 ? 4 : 2);
                    s32 yy = c16(c16(blk[b][(lr & 3) * 4 + (x & 3)] * 64) + 32), ch = x >> 1;
                    u32 px = ((comp5(c16(yy + ca[ch])) << 2) & 0xFFFF) | (comp5(c16(yy + cb[ch])) >> 3) |
                             (comp5(c16(yy + cc[ch])) >> 8) | alpha;
                    c->dm[(a + x * 2) & 0xFFF] = (u8)(px >> 8);
                    c->dm[(a + x * 2 + 1) & 0xFFF] = (u8)px;
                }
                c->ins += 27;
            }
            c->ins += 3;
        }
        dma_write(c, 0xCD0, t7, t4);
        c->ins += 10;
    skipped:
        s0--;
        t7 += 0x10;
        c->ins += 3;
    }
    c->ins += 10;
}

// The task's length in CPU cycles: 1.61 RCP cycles an instruction counted
// above (the LLE RSP took 0.28-1.7 million RCP cycles a frame for 0.17-1.03
// million of them, 30 frames measured), RCP cycles = 2/3 CPU's.
static u32 npn_cycles(u32 ins)
{
    return (u32)((u64)ins * 161 * 3 / 200);
}

// Checked against the LLE RSP on the console: the first 30 frames of the
// intro, every byte written the same (the literal and the folded versions).
void npn_video_task(struct hle_t *hle)
{
    H64System *sys = hle->sys;
    NpnCtx *c;
    static NpnCtx ctx;
    u32 ud = *dmem_u32(hle, TASK_UCODE_DATA), uds = *dmem_u32(hle, TASK_UCODE_DATA_SIZE), i;
    int fast;

    // A resumed task (OS_TASK_YIELDED, its DMEM from the yield buffer) or a
    // yield already asked for: the LLE RSP.
    if ((*dmem_u32(hle, TASK_FLAGS) & 1) || (sys->rsp.status & 0x80))
    {
        hle->forwarded = 1;
        return;
    }
    c = &ctx;
    memset(c, 0, sizeof(*c));
    c->sys = sys;
    c->hle = hle;
    c->dm = hle->dmem;
    c->outLo = 0xFFFFFFFFu;
    c->divin = sys->rsp.divin; c->divout = sys->rsp.divout; c->divdp = sys->rsp.divdp;
    // The boot microcode loads the task's data at DMEM 0.
    for (i = 0; i < uds && i < 0x1000; i++) c->dm[i] = ud + i < H64_RDRAM_SIZE ? sys->rdram[(ud + i) & 0x7FFFFF] : 0;
    {
        // The folded version: the data it was folded with, no carry in VCO, 8x8 or 8x4 macroblocks.
        u32 hdr = (u32)*dmem_u32(hle, TASK_DATA_PTR) & 0x7FFFFF, t8 = sys->rdram[(hdr + 0xB) & 0x7FFFFF];
        fast = uds >= 0x70 && !memcmp(c->dm, s_npnData, 0x70) && (t8 == 1 || t8 == 2);
        for (i = 0; i < 8; i++) if (sys->rsp.vcol[i]) fast = 0;
    }
    if (fast) npn_run_fast(c); else npn_run(c);
    if (c->outHi > c->outLo)
    {
        // The frame it wrote is shown from RDRAM (a movie after the RDP drew
        // there: the title screen stayed while the story's movie played).
        sys->rspPictureLo = c->outLo;
        sys->rspPictureHi = c->outHi;
        sys->rspPictureGen++;
    }
    sys->rsp.status &= ~0x300u;   // 080: SIG1 and SIG2 cleared
    sys->rsp.semaphore = 0;       // 330
    sys->rsp.divin = c->divin; sys->rsp.divout = c->divout; sys->rsp.divdp = c->divdp;
    hle->taskCycles = npn_cycles(c->ins);
    rsp_break(hle, SP_STATUS_TASKDONE);
}
