// LAIKA 64 - libdragon HLE: commands of libdragon's RSP queue (rspq) run in C.
//
// Homebrew built with libdragon keeps one microcode running on the RSP: rspq,
// a loop that reads commands from RDRAM and jumps to the overlay that owns
// each one (rdpq, tiny3d, the audio mixer...), loading it into IMEM first.
// The loop stays LLE (a few instructions a command); a command whose code is
// known runs in C instead, on the same DMEM and registers, and returns to the
// loop as the microcode would. Known means the same code: the first words of
// the function are hashed where the loop jumps to it ("jr cmd" with
// "li ra, RSPQ_Loop" in its delay slot, rsp_queue.inc). Another version of an
// overlay does not match and stays LLE.
#include "h64_rsp_libdragon.h"

#include <string.h>

#include "../common/h64_endian.h"
#include "../common/h64_log.h"
#include "../system/h64_system.h"

#include <math.h>

#include "h64_rsp.h"
#include "../rdp/h64_rdp.h"

#define HASH_WORDS 32   // instructions hashed from a command's entry

struct LdCommand
{
    u32 hash;           // of HASH_WORDS instructions from the entry
    const char *name;
    int (*run)(H64System *sys, u32 *rcpCycles);   // 1: done (the loop goes on), 0: left to the LLE RSP
};

static u32 code_hash_range(const u8 *imem, u32 from, u32 to)
{
    u32 h = 0x811C9DC5u, pc;
    for (pc = from; pc < to; pc += 4) h = (h ^ h64_load_be32(imem + (pc & 0xFFC))) * 16777619u;
    return h;
}

static u32 dm16(const H64System *sys, u32 a) { return h64_load_be16(sys->spMem + (a & 0xFFE)); }
static u32 dm32(const H64System *sys, u32 a) { return h64_load_be32(sys->spMem + (a & 0xFFC)); }
static void dm_set16(H64System *sys, u32 a, u32 v) { h64_store_be16(sys->spMem + (a & 0xFFE), (u16)v); }
static void dm_set32(H64System *sys, u32 a, u32 v) { h64_store_be32(sys->spMem + (a & 0xFFC), v); }

// ---- libdragon's rspq/rdpq state in DMEM (rsp_queue.inc) ----
#define RDPQ_CURRENT           0x1D0
#define RDPQ_SENTINEL          0x1D4
#define RDPQ_SYNCFULL_ONGOING  0x1E1

// ---- tiny3d (github.com/HailToDodongo/tiny3d), the build Halo 64 carries ----
// Its overlay state in DMEM (read from its code: the loads of each variable).
#define T3D_TRI_COMMAND        0x410
#define T3D_FACE_CULLING       0x418
#define T3D_CLIP_CODE_ADDR     0x420   // first byte: the clipping code is in IMEM
#define T3D_TRI_BUFF_OFFSET    0x360
#define T3D_SEND_HASH          0x90898132u   // RDPQ_Triangle_Send_Async .. Send_End (IMEM 280-6F0)
#define T3D_RDP_CMD_MAX_SIZE   176
// Its vertex in DMEM (rspq_triangle.rspl): XY s13.2, Z, clip/reject codes, RGBA8, ST s10.5, 1/W s16.16.
#define VTX_XY     0x00
#define VTX_Z      0x04
#define VTX_CLIP   0x06
#define VTX_RGBA   0x08
#define VTX_ST     0x0C
#define VTX_INVWI  0x20
#define VTX_INVWF  0x22

static int s_sendOk = -1;   // the shared triangle code is the one known (-1: not checked since IMEM changed)

static int t3d_send_known(H64System *sys)
{
    if (s_sendOk < 0) s_sendOk = code_hash_range(sys->spMem + 0x1000, 0x280, 0x6F0) == T3D_SEND_HASH;
    return s_sendOk;
}

// A triangle as libdragon's rdpq_triangle_cpu builds it (rdpq_tri.c, public domain / Unlicense), from tiny3d's
// screen-space vertices. Float where the RSP is fixed point: the same primitive within rounding.
struct TriVtx { float x, y, z, rgba[4], s, t, invw; };

static void tri_vtx(const H64System *sys, u32 a, TriVtx *v)
{
    const s32 z = (s16)dm16(sys, a + VTX_Z);
    int k;
    v->x = (float)(s16)dm16(sys, a + VTX_XY) * 0.25f;
    v->y = (float)(s16)dm16(sys, a + VTX_XY + 2) * 0.25f;
    v->z = (float)(z < 0 ? 0 : z) / 32767.0f;
    for (k = 0; k < 4; k++) v->rgba[k] = (float)sys->spMem[(a + VTX_RGBA + k) & 0xFFF] / 255.0f;
    v->s = (float)(s16)dm16(sys, a + VTX_ST) / 32.0f;
    v->t = (float)(s16)dm16(sys, a + VTX_ST + 2) / 32.0f;
    v->invw = (float)(s16)dm16(sys, a + VTX_INVWI) + (float)dm16(sys, a + VTX_INVWF) / 65536.0f;
}

static s32 to_s16_16(float f)
{
    if (f >= 32768.0f) return 0x7FFFFFFF;
    if (f < -32768.0f) return (s32)0x80000000;
    return (s32)floor(f * 65536.0f);
}

static s32 clamp_s32(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

// Two halves of a 16.16 pair: integer parts, then fractions (the RDP's attribute layout).
static u32 hi_pair(s32 a, s32 b) { return ((u32)a & 0xFFFF0000) | (((u32)b >> 16) & 0xFFFF); }
static u32 lo_pair(s32 a, s32 b) { return (u32)a << 16 | ((u32)b & 0xFFFF); }

// The RDP words of one triangle into w; returns the word count. cmdHalf: the upper half of word 0 without the
// left-major bit (command with its shade/texture/depth bits, mipmap levels, tile).
static u32 tri_build(u32 *w, u32 cmdHalf, const TriVtx *a, const TriVtx *b, const TriVtx *c)
{
    const TriVtx *v1 = a, *v2 = b, *v3 = c, *t;
    float hx, hy, mx, my, lx, ly, nz, fac, ish, ism, isl, fy, y1, y2, y3;
    u32 n = 0;
    if (v1->y > v2->y) { t = v1; v1 = v2; v2 = t; }
    if (v2->y > v3->y) { t = v2; v2 = v3; v3 = t; }
    if (v1->y > v2->y) { t = v1; v1 = v2; v2 = t; }
    y1 = floorf(v1->y * 4) / 4;
    y2 = floorf(v2->y * 4) / 4;
    y3 = floorf(v3->y * 4) / 4;
    hx = v3->x - v1->x; hy = y3 - y1;
    mx = v2->x - v1->x; my = y2 - y1;
    lx = v3->x - v2->x; ly = y3 - y2;
    nz = hx * my - hy * mx;
    fac = fabsf(nz) > 1e-30f ? -1.0f / nz : 0.0f;
    ish = fabsf(hy) > 1e-30f ? hx / hy : 0.0f;
    ism = fabsf(my) > 1e-30f ? mx / my : 0.0f;
    isl = fabsf(ly) > 1e-30f ? lx / ly : 0.0f;
    fy = floorf(y1) - y1;
    {
        const s32 y1f = clamp_s32((s32)floorf(v1->y * 4), -4096 * 4, 4095 * 4);
        const s32 y2f = clamp_s32((s32)floorf(v2->y * 4), -4096 * 4, 4095 * 4);
        const s32 y3f = clamp_s32((s32)floorf(v3->y * 4), -4096 * 4, 4095 * 4);
        w[n++] = cmdHalf << 16 | (nz < 0 ? 0x00800000u : 0) | ((u32)y3f & 0x3FFF);
        w[n++] = ((u32)y2f & 0x3FFF) << 16 | ((u32)y1f & 0x3FFF);
        w[n++] = (u32)to_s16_16(v2->x);
        w[n++] = (u32)to_s16_16(isl);
        w[n++] = (u32)to_s16_16(v1->x + fy * ish);
        w[n++] = (u32)to_s16_16(ish);
        w[n++] = (u32)to_s16_16(v1->x + fy * ism);
        w[n++] = (u32)to_s16_16(ism);
    }
    if (cmdHalf & 0x0400)
    {
        // Shade: r g b a and their gradients (__rdpq_write_shade_coeffs).
        s32 f[4], dx[4], de[4], dy[4];
        int k;
        for (k = 0; k < 4; k++)
        {
            const float m = (v2->rgba[k] - v1->rgba[k]) * 255.0f, h = (v3->rgba[k] - v1->rgba[k]) * 255.0f;
            const float ddx = (hy * m - my * h) * fac, ddy = (mx * h - hx * m) * fac, dde = ddy + ddx * ish;
            f[k] = to_s16_16(v1->rgba[k] * 255.0f + fy * dde);
            dx[k] = to_s16_16(ddx);
            de[k] = to_s16_16(dde);
            dy[k] = to_s16_16(ddy);
        }
        w[n++] = hi_pair(f[0], f[1]);   w[n++] = hi_pair(f[2], f[3]);
        w[n++] = hi_pair(dx[0], dx[1]); w[n++] = hi_pair(dx[2], dx[3]);
        w[n++] = lo_pair(f[0], f[1]);   w[n++] = lo_pair(f[2], f[3]);
        w[n++] = lo_pair(dx[0], dx[1]); w[n++] = lo_pair(dx[2], dx[3]);
        w[n++] = hi_pair(de[0], de[1]); w[n++] = hi_pair(de[2], de[3]);
        w[n++] = hi_pair(dy[0], dy[1]); w[n++] = hi_pair(dy[2], dy[3]);
        w[n++] = lo_pair(de[0], de[1]); w[n++] = lo_pair(de[2], de[3]);
        w[n++] = lo_pair(dy[0], dy[1]); w[n++] = lo_pair(dy[2], dy[3]);
    }
    if (cmdHalf & 0x0200)
    {
        // Texture with perspective: s t w (__rdpq_write_tex_coeffs; 1/W relative to the nearest vertex).
        float s1 = v1->s * 32, t1 = v1->t * 32, w1 = v1->invw, s2 = v2->s * 32, t2 = v2->t * 32, w2 = v2->invw;
        float s3 = v3->s * 32, t3 = v3->t * 32, w3 = v3->invw, maxw = w1 > w2 ? w1 : w2, minw;
        s32 fs, ft, fw, sx, tx, wx, se, te, we, sy, ty, wy;
        if (w3 > maxw) maxw = w3;
        minw = maxw != 0.0f ? 1.0f / maxw : 0.0f;
        w1 *= minw; w2 *= minw; w3 *= minw;
        s1 *= w1; t1 *= w1; s2 *= w2; t2 *= w2; s3 *= w3; t3 *= w3;
        w1 *= 0x7FFF; w2 *= 0x7FFF; w3 *= 0x7FFF;
        {
            const float ms = s2 - s1, mt = t2 - t1, mw = w2 - w1, hs = s3 - s1, ht = t3 - t1, hw = w3 - w1;
            const float dsx = (hy * ms - my * hs) * fac, dtx = (hy * mt - my * ht) * fac, dwx = (hy * mw - my * hw) * fac;
            const float dsy = (mx * hs - hx * ms) * fac, dty = (mx * ht - hx * mt) * fac, dwy = (mx * hw - hx * mw) * fac;
            const float dse = dsy + dsx * ish, dte = dty + dtx * ish, dwe = dwy + dwx * ish;
            fs = to_s16_16(s1 + fy * dse); ft = to_s16_16(t1 + fy * dte); fw = to_s16_16(w1 + fy * dwe);
            sx = to_s16_16(dsx); tx = to_s16_16(dtx); wx = to_s16_16(dwx);
            se = to_s16_16(dse); te = to_s16_16(dte); we = to_s16_16(dwe);
            sy = to_s16_16(dsy); ty = to_s16_16(dty); wy = to_s16_16(dwy);
        }
        w[n++] = hi_pair(fs, ft); w[n++] = (u32)fw & 0xFFFF0000;
        w[n++] = hi_pair(sx, tx); w[n++] = (u32)wx & 0xFFFF0000;
        w[n++] = lo_pair(fs, ft); w[n++] = (u32)fw << 16;
        w[n++] = lo_pair(sx, tx); w[n++] = (u32)wx << 16;
        w[n++] = hi_pair(se, te); w[n++] = (u32)we & 0xFFFF0000;
        w[n++] = hi_pair(sy, ty); w[n++] = (u32)wy & 0xFFFF0000;
        w[n++] = lo_pair(se, te); w[n++] = (u32)we << 16;
        w[n++] = lo_pair(sy, ty); w[n++] = (u32)wy << 16;
    }
    if (cmdHalf & 0x0100)
    {
        // Depth (__rdpq_write_zbuf_coeffs).
        const float z1 = v1->z * 0x7FFF, mz = v2->z * 0x7FFF - z1, hz = v3->z * 0x7FFF - z1;
        const float dzx = (hy * mz - my * hz) * fac, dzy = (mx * hz - hx * mz) * fac, dze = dzy + dzx * ish;
        w[n++] = (u32)to_s16_16(z1 + fy * dze);
        w[n++] = (u32)to_s16_16(dzx);
        w[n++] = (u32)to_s16_16(dze);
        w[n++] = (u32)to_s16_16(dzy);
    }
    return n;
}

// What tiny3d's RDPQ_Triangle_Send_Async does with vertices a b c (DMEM) and culling mode cull:
// 0 rejected or culled, 1 drawn, -1 it needs clipping (left to the LLE RSP).
static int t3d_tri_kind(const H64System *sys, u32 a, u32 b, u32 c, u32 cull)
{
    const u32 clip = dm16(sys, a + VTX_CLIP) | dm16(sys, b + VTX_CLIP) | dm16(sys, c + VTX_CLIP);
    float ax, ay, bx, by, cx, cy, nzo;
    if ((clip & 0xFF) != 0xFF) return 0;   // all three outside one plane (the reject codes are stored negated)
    if (clip & 0x1F00) return -1;
    // Back or front face: the sign of the triangle in its own vertex order. The microcode sorts the vertices by Y
    // and keeps the sort's compare results (vlt: 1 where a pair was already in order, so their parity is the
    // permutation's flipped): culled when the sign matches cull ^ 1.
    ax = (float)(s16)dm16(sys, a); ay = (float)(s16)dm16(sys, a + 2);
    bx = (float)(s16)dm16(sys, b); by = (float)(s16)dm16(sys, b + 2);
    cx = (float)(s16)dm16(sys, c); cy = (float)(s16)dm16(sys, c + 2);
    nzo = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (nzo == 0.0f) return 0;
    if ((cull == 0 || cull == 1) && (u32)(nzo < 0) != cull) return 0;
    return 1;
}

// One triangle into RDRAM at RDPQ_CURRENT, where the microcode's DMA puts it.
static void t3d_tri_draw(H64System *sys, u32 a, u32 b, u32 c)
{
    u32 w[44], n, k;
    const u32 cur = dm32(sys, RDPQ_CURRENT), addr = cur & 0xFFFFF8;
    const u32 clip = dm16(sys, a + VTX_CLIP) | dm16(sys, b + VTX_CLIP) | dm16(sys, c + VTX_CLIP);
    TriVtx va, vb, vc;
    tri_vtx(sys, a, &va);
    tri_vtx(sys, b, &vb);
    tri_vtx(sys, c, &vc);
    n = tri_build(w, (dm32(sys, T3D_TRI_COMMAND) & 0xFF78) | (clip >> 13), &va, &vb, &vc);
    for (k = 0; k < n; k++) h64_store_be32(sys->rdram + ((addr + k * 4) & (H64_RDRAM_SIZE - 1)), w[k]);
    h64_jit_notify_write(sys, addr, n * 4);
    dm_set32(sys, RDPQ_CURRENT, cur + n * 4);
    dm_set16(sys, T3D_TRI_BUFF_OFFSET, dm16(sys, T3D_TRI_BUFF_OFFSET) ^ T3D_RDP_CMD_MAX_SIZE);
}

// Room in the RDP buffer for `tris` triangles and no SYNC_FULL in it (else the microcode would switch buffers or
// wait: left to the LLE RSP).
static int t3d_room(const H64System *sys, u32 tris)
{
    const u32 cur = dm32(sys, RDPQ_CURRENT) & 0xFFFFFF, end = dm32(sys, RDPQ_SENTINEL) & 0xFFFFFF;
    return !sys->spMem[RDPQ_SYNCFULL_ONGOING] && cur + tris * T3D_RDP_CMD_MAX_SIZE <= end;
}

static void t3d_commit(H64System *sys)
{
    h64_dp_write(sys, 1, dm32(sys, RDPQ_CURRENT) & 0xFFFFFF);   // DP_END: the RDP goes on to here
}

// T3DCmd_TriDraw_Cmd (command 0): one triangle, vertices a0, a1, a1 >> 16.
static int t3d_tri_draw_cmd(H64System *sys, u32 *rcpCycles)
{
    const u32 a = sys->rsp.r[4] & 0xFFF, b = sys->rsp.r[5] & 0xFFF, c = (sys->rsp.r[5] >> 16) & 0xFFF;
    int kind;
    if (!t3d_send_known(sys)) return 0;
    while (sys->rsp.dmaBusy) h64_sp_dma_event(sys);
    kind = t3d_tri_kind(sys, a, b, c, sys->spMem[T3D_FACE_CULLING]);
    if (kind < 0 || !t3d_room(sys, 1)) return 0;
    if (kind)
    {
        t3d_tri_draw(sys, a, b, c);
        t3d_commit(sys);
    }
    *rcpCycles = kind ? 200 : 40;
    return 1;
}

// T3DCmd_TriDraw_Strip (command 11): the strip's DMEM vertex pointers come by DMA from RDRAM (a0) into DMEM at
// a1 >> 16 (negative: a sync at the end), a1 & 0xFFFF the DMA length (bytes - 1); each step draws
// (p[1], p[2], p[0]) with the winding flipped each time, and a negative p[2] restarts the strip (rsp_tiny3d.rspl).
static int t3d_tri_strip(H64System *sys, u32 *rcpCycles)
{
    static u32 tris[512][3];
    const u32 a0 = sys->rsp.r[4], a1 = sys->rsp.r[5], len = a1 & 0xFFFF;
    s32 p = (s32)a1 >> 16, end;
    u32 count = 0, steps = 0, k, i, cull = 0;
    int fresh = 1;
    if (!t3d_send_known(sys)) return 0;
    while (sys->rsp.dmaBusy) h64_sp_dma_event(sys);
    {
        // The DMA of the pointers (whole 8-byte units, as the SP DMA).
        const u32 dram = a0 & 0xFFFFF8, mem = (u32)p & 0xFF8, bytes = (len & 0xFF8) + 8;
        for (i = 0; i < bytes; i++) sys->spMem[(mem + i) & 0xFFF] = dram + i < H64_RDRAM_SIZE ? sys->rdram[dram + i] : 0;
    }
    end = p + (s32)len - 3;
    p -= 2;
    // Dry run: the triangles, and whether all of them can be drawn here.
    for (;;)
    {
        if (++steps > 4096) return 0;
        if (fresh) { cull = sys->spMem[T3D_FACE_CULLING]; p += 2; fresh = 0; }
        cull ^= 1;
        if (p == end) break;
        {
            const u32 i0 = dm16(sys, (u32)p + 2) & 0xFFF, i2 = dm16(sys, (u32)p) & 0xFFF;
            const s32 i1 = (s16)dm16(sys, (u32)p + 4);
            int kind;
            p += 2;
            if (i1 < 0) { fresh = 1; continue; }
            kind = t3d_tri_kind(sys, i0, (u32)i1 & 0xFFF, i2, cull);
            if (kind < 0) return 0;
            if (kind)
            {
                if (count >= 512) return 0;
                tris[count][0] = i0;
                tris[count][1] = (u32)i1 & 0xFFF;
                tris[count][2] = i2;
                count++;
            }
        }
    }
    if (!t3d_room(sys, count)) return 0;
    if (end < 0 && sys->spMem[T3D_CLIP_CODE_ADDR]) return 0;   // its sync would bring tiny3d's own code back
    for (k = 0; k < count; k++) t3d_tri_draw(sys, tris[k][0], tris[k][1], tris[k][2]);
    if (end < 0) dm_set16(sys, T3D_TRI_BUFF_OFFSET, 0);   // T3DCmd_TriSync -> RDPQ_Triangle_Send_End
    if (count || end < 0) t3d_commit(sys);
    *rcpCycles = 60 + count * 200 + (steps - count) * 20;
    return 1;
}

// ---- RSP vector arithmetic, one lane at a time (the rules of h64_rsp_vu.cpp, for byte-exact results) ----
// The accumulator: 48 bits, kept sign-extended in an s64.
static s64 w48(s64 v) { return (s64)((u64)v << 16) >> 16; }
static u16 acc_h(s64 a) { return (u16)((u64)a >> 32); }
static u16 acc_m(s64 a) { return (u16)((u64)a >> 16); }
static u16 acc_l(s64 a) { return (u16)a; }
static u16 acc_sat(s64 a, int mid, u16 neg, u16 pos)
{
    const u16 h = acc_h(a), m = acc_m(a);
    if ((s16)h < 0) { if (h != 0xFFFF || (s16)m >= 0) return neg; }
    else if (h != 0 || (s16)m < 0) return pos;
    return mid ? m : acc_l(a);
}
static u16 v_mudn(s64 *a, u16 s, u16 t) { *a = (s64)((s32)s * (s32)(s16)t); return acc_l(*a); }
static u16 v_madn(s64 *a, u16 s, u16 t) { *a = w48(*a + (s64)((s32)s * (s32)(s16)t)); return acc_sat(*a, 0, 0x0000, 0xFFFF); }
static u16 v_madh(s64 *a, u16 s, u16 t)
{
    const s32 res = (s32)(u32)((u64)*a >> 16) + (s32)(s16)s * (s32)(s16)t;
    *a = w48((s64)res * 65536 + acc_l(*a));
    return acc_sat(*a, 1, 0x8000, 0x7FFF);
}
static u16 v_mudl(s64 *a, u16 s, u16 t) { *a = (s64)(((u32)s * (u32)t) >> 16); return acc_l(*a); }
static u16 v_madm(s64 *a, u16 s, u16 t) { *a = w48(*a + (s64)((s32)(s16)s * (s32)t)); return acc_sat(*a, 1, 0x8000, 0x7FFF); }
static u16 v_mudm(s64 *a, u16 s, u16 t) { *a = (s64)((s32)(s16)s * (s32)t); return acc_m(*a); }
static u16 v_mudh(s64 *a, u16 s, u16 t) { *a = w48((s64)((s32)(s16)s * (s32)(s16)t) * 65536); return acc_sat(*a, 1, 0x8000, 0x7FFF); }
static u16 v_mulf(s64 *a, u16 s, u16 t) { *a = w48((s64)(s16)s * (s16)t * 2 + 0x8000); return acc_sat(*a, 1, 0x8000, 0x7FFF); }
static u16 v_macf(s64 *a, u16 s, u16 t) { *a = w48(*a + (s64)(s16)s * (s16)t * 2); return acc_sat(*a, 1, 0x8000, 0x7FFF); }
static u16 v_mulu(s64 *a, u16 s, u16 t)
{
    *a = w48((s64)(s16)s * (s16)t * 2 + 0x8000);
    return (s16)acc_h(*a) < 0 ? 0x0000 : ((s16)acc_h(*a) ^ (s16)acc_m(*a)) < 0 ? 0xFFFF : acc_m(*a);
}
static u16 v_macu(s64 *a, u16 s, u16 t)
{
    *a = w48(*a + (s64)(s16)s * (s16)t * 2);
    return (s16)acc_h(*a) < 0 ? 0x0000 : ((s16)acc_h(*a) || (s16)acc_m(*a) < 0) ? 0xFFFF : acc_m(*a);
}
static s32 clamp16(s32 v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

// VCH then VCL on one lane (s against t, high halves then low halves): the VCC bits (low: s <= -t, high: s >= t).
struct LaneFlags { int ccl, cch, col, coh, ce; };
static void v_ch(LaneFlags *f, u16 vs, u16 vt)
{
    const s16 s = (s16)vs, t = (s16)vt;
    if ((s ^ t) < 0)
    {
        const s16 res = (s16)(s + t);
        f->ccl = res <= 0; f->cch = t < 0; f->col = 1;
        f->coh = res != 0 && vs != (u16)(vt ^ 0xFFFF); f->ce = res == -1;
    }
    else
    {
        const s16 res = (s16)(s - t);
        f->ccl = t < 0; f->cch = res >= 0; f->col = 0;
        f->coh = res != 0 && vs != (u16)(vt ^ 0xFFFF); f->ce = 0;
    }
}
static void v_cl(LaneFlags *f, u16 vs, u16 vt)
{
    if (f->col)
    {
        if (!f->coh)
        {
            const u16 sum = (u16)(vs + vt);
            const int carry = ((u32)vs + vt) != sum;
            f->ccl = f->ce ? (!sum || !carry) : (!sum && !carry);
        }
    }
    else if (!f->coh) f->cch = (s32)vs - (s32)vt >= 0;
    f->col = f->coh = f->ce = 0;
}

// VRCPL on a 32-bit input (VRCPH gave its upper half): the reciprocal from the table (h64_rsp_vu.cpp).
static s32 v_rcp32(const H64Rsp *r, s32 input)
{
    const s32 mask = input >> 31;
    u32 data = (u32)(input ^ mask), shift, index;
    s32 result;
    if (input > -32768) data -= (u32)mask;
    if (data == 0) return 0x7FFFFFFF;
    if (input == -32768) return (s32)0xFFFF0000u;
    shift = 0;
    while (!(data << shift & 0x80000000u)) shift++;
    index = (u32)((((u64)data << shift) & 0x7FC00000u) >> 22);
    result = r->reciprocals[index];
    result = (0x10000 | result) << 14;
    return (result >> (31 - shift)) ^ mask;
}

// ---- tiny3d T3DCmd_VertLoad (command 4) ----
// Its DMEM state (read from its code): matrices, screen and fog constants, lights, vertex effect.
#define T3D_MATRIX_MVP         0x270   // 4 rows: 4 x s16 integer parts, then 4 x u16 fractions
#define T3D_MATRIX_NORMAL      0x2F0   // 3 rows of 4 x s16 fractions
#define T3D_VERT_CONSTS        0x308
#define T3D_NORMAL_MASK_SHIFT  0x320
#define T3D_SCREEN_SCALE       0x348   // 4 x s16 integer parts, then 4 x u16 fractions (w: the fog slope)
#define T3D_SCREEN_OFFSET      0x358
#define T3D_NORM_SCALE_W       0x368
#define T3D_UV_GEN_PARAMS      0x372
#define T3D_COLOR_EXPOSURE     0x376
#define T3D_SEGMENT_TABLE      0x378
#define T3D_COLOR_AMBIENT      0x398
#define T3D_LIGHT_DIR_COLOR    0x3A0   // 16 bytes a light: RGBA8 x2, direction s8 x4 x2 (byte 15: a point light)
#define T3D_FOG_STORE_OFFSET   0x419
#define T3D_ACTIVE_LIGHT_SIZE  0x41A
#define T3D_VERTEX_FX_FUNC     0x41C
#define T3D_BASE_TILE_MASK     0x424   // CLIP_CODE_ORG_ADDR's first byte
#define T3D_LOOP_TRAIL         0x808   // VertexFX "none": stores the UVs and loops
#define T3D_VERT_HASH          0x2D6D4545u   // the loop and its setup, IMEM 70C-A3C (checked at the first use)
#define TRI_SIZE               0x24

static int s_vertOk = -1;

static void dm_put(H64System *sys, u32 a, u8 v) { sys->spMem[a & 0xFFF] = v; }
static void dm_put16(H64System *sys, u32 a, u16 v) { dm_put(sys, a, (u8)(v >> 8)); dm_put(sys, a + 1, (u8)v); }

static int t3d_vert_load(H64System *sys, u32 *rcpCycles)
{
    H64Rsp *r = &sys->rsp;
    const u32 a0 = r->r[4], a1 = r->r[5], a2 = r->r[6];
    const u32 prt = (a2 >> 16) & 0xFFFF, size = a0 & 0xFFFF, out = a2 & 0xFFFF, end = prt + size;
    const u8 *m = sys->spMem;
    u32 lightEnd, ambient, l, p, i, j, pair;
    u16 Mi[4][4], Mf[4][4], N[3][4], nmask[4], nshift[4], scI[4], scF[4], off[4], nsw[4], uvp[8];
    u16 fogClamp, fogSlopeF, exposure, v03hi[4], lastUv[4] = { 0, 0, 0, 0 };
    int vcol[8];
    if (s_vertOk < 0) s_vertOk = code_hash_range(sys->spMem + 0x1000, 0x70C, 0xA3C) == T3D_VERT_HASH;
    if (!s_vertOk) return 0;
    if ((dm16(sys, T3D_VERTEX_FX_FUNC) & 0xFFF) != T3D_LOOP_TRAIL || !size || (size & 0x1F) || end > 0x1000) return 0;
    lightEnd = m[0x410] ? T3D_LIGHT_DIR_COLOR : T3D_LIGHT_DIR_COLOR + m[T3D_ACTIVE_LIGHT_SIZE];
    if ((lightEnd - T3D_LIGHT_DIR_COLOR) & 15 || lightEnd > 0x410) return 0;
    for (l = T3D_LIGHT_DIR_COLOR; l != lightEnd; l += 16)
        if (m[l + 15]) return 0;   // a point light: left to the LLE RSP
    while (r->dmaBusy) h64_sp_dma_event(sys);
    {
        // The vertices' DMA (the segment's base added), then -1 written after them (the white of "no light").
        const u32 src = a1 + dm32(sys, T3D_SEGMENT_TABLE + ((a1 >> 24) & 0xFC)), dram = src & 0xFFFFF8;
        const u32 mem = prt & 0xFF8, bytes = ((size - 1) & 0xFF8) + 8;
        for (i = 0; i < bytes; i++) sys->spMem[(mem + i) & 0xFFF] = dram + i < H64_RDRAM_SIZE ? sys->rdram[dram + i] : 0;
        for (i = 0; i < 8; i++) dm_put(sys, end + i, 0xFF);
    }
    ambient = m[0x410] ? end : T3D_COLOR_AMBIENT;
    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++) { Mi[i][j] = (u16)dm16(sys, T3D_MATRIX_MVP + i * 16 + j * 2); Mf[i][j] = (u16)dm16(sys, T3D_MATRIX_MVP + i * 16 + 8 + j * 2); }
        if (i < 3) for (j = 0; j < 4; j++) N[i][j] = (u16)dm16(sys, T3D_MATRIX_NORMAL + i * 8 + j * 2);
        nmask[i] = (u16)dm16(sys, T3D_NORMAL_MASK_SHIFT + i * 2);
        nshift[i] = (u16)dm16(sys, T3D_NORMAL_MASK_SHIFT + 8 + i * 2);
        scI[i] = (u16)dm16(sys, T3D_SCREEN_SCALE + i * 2);
        scF[i] = (u16)dm16(sys, T3D_SCREEN_SCALE + 8 + i * 2);
        off[i] = (u16)dm16(sys, T3D_SCREEN_OFFSET + i * 2);
        nsw[i] = (u16)dm16(sys, T3D_NORM_SCALE_W + i * 2);
        v03hi[i] = r->vr[3][4 + i];
    }
    nmask[3] = (u16)dm16(sys, T3D_SCREEN_SCALE + 6);   // the fog slope's integer half rides in lane w
    fogClamp = (u16)dm16(sys, T3D_VERT_CONSTS + 8);
    fogSlopeF = (u16)dm16(sys, T3D_SCREEN_SCALE + 14);
    exposure = (u16)dm16(sys, T3D_COLOR_EXPOSURE);
    uvp[0] = uvp[2] = uvp[4] = (u16)dm16(sys, T3D_UV_GEN_PARAMS);
    uvp[1] = uvp[3] = uvp[5] = (u16)dm16(sys, T3D_UV_GEN_PARAMS + 2);
    uvp[6] = exposure;
    uvp[7] = (u16)dm16(sys, 0x362);   // SCREEN_UVGEN_SCALE
    for (i = 0; i < 8; i++) vcol[i] = 1;   // VTEMP = 0 - 1 (vsubc) before the loop
    for (pair = 0, p = prt; p != end; p += 0x20, pair++)
    {
        const u32 outA = out + pair * 2 * TRI_SIZE, outB = outA + TRI_SIZE, fogAt = outA + m[T3D_FOG_STORE_OFFSET];
        const u8 baseTile = (u8)(m[(p + 0x13) & 0xFFF] & m[T3D_BASE_TILE_MASK]);
        u16 oI[8], oF[8], pI[8], pF[8], nn[8], lc[8], col8[8], iI[2], iF[2], sI[8], fog[2], uv[8];
        u8 clip[2], rej[2];
        s64 a;
        for (i = 0; i < 2; i++)
        {
            // One vertex in lanes 4i..4i+3: x y z w.
            const u32 v = p + i * 8, L = i * 4;
            const u16 px = (u16)dm16(sys, v), py = (u16)dm16(sys, v + 2), pz = (u16)dm16(sys, v + 4), nrm = (u16)dm16(sys, v + 6);
            u16 n3[3];
            for (j = 0; j < 3; j++) n3[j] = v_mudn(&a, (u16)(nrm & nmask[j]), nshift[j]);
            for (j = 0; j < 4; j++)
            {
                v_mulf(&a, N[0][j], n3[0]);
                v_macf(&a, N[1][j], n3[1]);
                nn[L + j] = v_macf(&a, N[2][j], n3[2]);
                // object space to clip space (mulMat4Vec3), then x/y over the guard band (NORM_SCALE_W)
                v_mudn(&a, Mf[0][j], px); v_madh(&a, Mi[0][j], px);
                v_madn(&a, Mf[1][j], py); v_madh(&a, Mi[1][j], py);
                v_madn(&a, Mf[2][j], pz); v_madh(&a, Mi[2][j], pz);
                oF[L + j] = v_madn(&a, Mf[3][j], 1);
                oI[L + j] = v_madh(&a, Mi[3][j], 1);
                v_mudl(&a, oF[L + j], nsw[j]);
                pI[L + j] = v_madm(&a, oI[L + j], nsw[j]);
                pF[L + j] = v_madn(&a, 0, 0);
                lc[L + j] = (u16)(m[(ambient + L + j) & 0xFFF] << 7);
                col8[L + j] = (u16)(m[(p + 0x10 + L + j) & 0xFFF] << 7);
            }
        }
        for (l = T3D_LIGHT_DIR_COLOR; l != lightEnd; l += 16)
        {
            // Directional lights: color * dot(normal, direction), accumulated into the light color.
            u16 lds[8], t;
            for (j = 0; j < 8; j++) lds[j] = v_mulf(&a, nn[j], (u16)((s16)(s8)m[l + 8 + j] * 256));
            for (j = 0; j < 8; j++)
            {
                const u32 b = j & 4;
                const u16 c = (u16)(m[l + j] << 7);
                v_mulu(&a, c, lds[b]);
                v_macu(&a, c, lds[b + 1]);
                t = v_macu(&a, c, lds[b + 2]);
                lc[j] = (u16)clamp16((s16)lc[j] + (s16)t + vcol[j]);
            }
            for (j = 0; j < 8; j++) vcol[j] = 0;
        }
        for (i = 0; i < 2; i++)
        {
            const u32 L = i * 4;
            u32 bitsRej = 0, bitsClip = 0;
            s32 y;
            u16 tF3, d, lo2, hi2;
            int carry;
            for (j = 0; j < 4; j++)
            {
                LaneFlags f;
                v_ch(&f, oI[L + j], oI[L + 3]); v_cl(&f, oF[L + j], oF[L + 3]);
                bitsRej |= (u32)f.ccl << j | (u32)f.cch << (8 + j);
                v_ch(&f, pI[L + j], oI[L + 3]); v_cl(&f, pF[L + j], oF[L + 3]);
                bitsClip |= (u32)f.ccl << j | (u32)f.cch << (8 + j);
            }
            {
                const u32 rc = bitsRej & 0x70B, cc = bitsClip & 0x303;
                rej[i] = (u8)~(rc | rc >> 4);
                clip[i] = (u8)(cc | cc >> 5 | baseTile);
            }
            // 1/W: the table's reciprocal and one Newton-Raphson step (0.5/w), as the microcode's vector ops
            y = v_rcp32(r, (s32)((u32)pI[L + 3] << 16 | pF[L + 3]));
            v_mudl(&a, pF[L + 3], (u16)y); v_madm(&a, pI[L + 3], (u16)y);
            tF3 = v_madn(&a, pF[L + 3], (u16)(y >> 16));
            d = (u16)(nsw[2] - tF3);
            lo2 = (u16)((u32)(u16)y * 2);
            carry = ((u32)(u16)y * 2) >> 16 != 0;
            hi2 = (u16)clamp16((s16)(u16)(y >> 16) * 2 + carry);
            v_mudl(&a, lo2, d);
            iI[i] = v_madm(&a, hi2, d);
            iF[i] = v_madn(&a, 0, 0);
            for (j = 0; j < 4; j++)
            {
                // perspective division, then screen space (SCREEN_SCALE, SCREEN_OFFSET)
                u16 qI, qF;
                v_mudl(&a, pF[L + j], iF[i]); v_madm(&a, pI[L + j], iF[i]);
                qF = v_madn(&a, pF[L + j], iI[i]);
                qI = v_madh(&a, pI[L + j], iI[i]);
                v_mudl(&a, qF, scF[j]); v_madm(&a, qI, scF[j]);
                v_madn(&a, qF, scI[j]); v_madh(&a, qI, scI[j]);
                sI[L + j] = v_madh(&a, off[j], 1);
            }
            // fog: offset + slope * clip z, at most VERT_CONSTS' clamp
            v_mudh(&a, off[3], 1);
            v_madn(&a, fogSlopeF, pI[L + 2]);
            fog[i] = v_madh(&a, nmask[3], pI[L + 2]);
            if (!((s16)fog[i] < (s16)fogClamp)) fog[i] = fogClamp;
            for (j = 0; j < 4; j++)
            {
                // color * light color, then the exposure
                const u16 mm = v_mulf(&a, col8[L + j], lc[L + j]), q = v_mudm(&a, mm, 0x0200);
                col8[L + j] = v_mudh(&a, q, exposure);
            }
        }
        for (j = 0; j < 4; j++) uv[j] = (u16)dm16(sys, p + 0x18 + j * 2);
        for (j = 0; j < 4; j++) uv[4 + j] = v03hi[j];
        for (j = 0; j < 8; j++)
        {
            // + UV_GEN_PARAMS (vaddc: its carries go into the next pair's light sums)
            const u32 sum = (u32)uv[j] + uvp[j];
            uv[j] = (u16)sum;
            vcol[j] = sum >> 16 != 0;
        }
        for (j = 0; j < 4; j++) { v03hi[j] = uv[4 + j]; lastUv[j] = uv[j]; }
        // The stores, in the microcode's order (the fog byte may land on the alpha, or past the vertex).
        for (j = 0; j < 4; j++) dm_put16(sys, outA + 0x10 + j * 2, pI[j]);
        for (j = 0; j < 4; j++) dm_put16(sys, outB + 0x18 + j * 2, pF[4 + j]);
        for (j = 0; j < 4; j++) dm_put16(sys, outB + 0x10 + j * 2, pI[4 + j]);
        dm_put(sys, outA + 7, rej[0]);
        dm_put16(sys, outA + 0x20, iI[0]); dm_put16(sys, outA + 0x22, iF[0]);
        dm_put16(sys, outB + 0x22, iF[1]); dm_put16(sys, outB + 0x20, iI[1]);
        for (j = 0; j < 4; j++) dm_put(sys, outB + 8 + j, (u8)(col8[4 + j] >> 7));
        for (j = 0; j < 4; j++) dm_put16(sys, outA + 0x18 + j * 2, pF[j]);
        dm_put(sys, outB + 7, rej[1]);
        for (j = 0; j < 4; j++) dm_put(sys, outA + 8 + j, (u8)(col8[j] >> 7));
        dm_put(sys, fogAt + TRI_SIZE, (u8)fog[1]);
        dm_put(sys, outB + 6, clip[1]);
        dm_put(sys, fogAt, (u8)fog[0]);
        dm_put(sys, outA + 6, clip[0]);
        dm_put16(sys, outB, sI[4]); dm_put16(sys, outB + 2, sI[5]); dm_put16(sys, outB + 4, sI[6]);
        dm_put16(sys, outA + 4, sI[2]); dm_put16(sys, outA, sI[0]); dm_put16(sys, outA + 2, sI[1]);
        dm_put16(sys, outA + 0x0C, uv[0]); dm_put16(sys, outA + 0x0E, uv[1]);
        dm_put16(sys, outB + 0x0C, uv[2]); dm_put16(sys, outB + 0x0E, uv[3]);
    }
    // What a later command may read of the registers this one leaves: v03 (its upper lanes carry on from one
    // command to the next) and the carries of its last vaddc.
    for (j = 0; j < 4; j++) { r->vr[3][j] = lastUv[j]; r->vr[3][4 + j] = v03hi[j]; }
    for (j = 0; j < 8; j++) { r->vcol[j] = (u16)(vcol[j] ? 0xFFFF : 0); r->vcoh[j] = 0; }
    *rcpCycles = 40 + pair * 110;
    return 1;
}

// The commands known: tiny3d's triangles and vertex loads (the build Halo 64 carries).
static const LdCommand s_commands[] = {
    { 0x93AF97F2u, "tiny3d TriDraw", t3d_tri_draw_cmd },
    { 0xEBB9E2E9u, "tiny3d TriStrip", t3d_tri_strip },
    { 0x8E0A84B2u, "tiny3d VertLoad", t3d_vert_load },
};

// Per IMEM word: the command found there (index + 1), or 0 when not looked at
// since IMEM changed, or -1 when not a known one.
static int s_verdict[1024];

static u32 code_hash(const u8 *imem, u32 pc)
{
    u32 h = 0x811C9DC5u, i;
    for (i = 0; i < HASH_WORDS; i++) h = (h ^ h64_load_be32(imem + ((pc + i * 4) & 0xFFC))) * 16777619u;
    return h;
}

void h64_rsp_libdragon_imem_written(void)
{
    memset(s_verdict, 0, sizeof(s_verdict));
    s_sendOk = -1;
    s_vertOk = -1;
}

int h64_rsp_libdragon_command(H64System *sys, u32 entry, u32 cmdByte, u32 *rcpCycles)
{
    const u8 *imem = sys->spMem + 0x1000;
    int *v = &s_verdict[(entry >> 2) & 1023];
    if (!*v)
    {
        u32 h = code_hash(imem, entry), k;
        *v = -1;
        for (k = 0; k < sizeof(s_commands) / sizeof(s_commands[0]); k++)
            if (s_commands[k].run && s_commands[k].hash == h) { *v = (int)k + 1; break; }
        {
            // Each command seen, once: its byte, entry and hash (what a new entry in s_commands needs).
            static u32 s_seen[64];
            static int s_seenN;
            int j, told = 0;
            for (j = 0; j < s_seenN; j++) if (s_seen[j] == h) told = 1;
            if (!told && s_seenN < 64)
            {
                s_seen[s_seenN++] = h;
                H64_INFO("[ldhle] command %02X at IMEM %03X: code %08X%s%s", cmdByte, entry & 0xFFC, h,
                         *v > 0 ? " -> C: " : " (LLE)", *v > 0 ? s_commands[*v - 1].name : "");
            }
        }
    }
    if (*v <= 0) return 0;
    return s_commands[*v - 1].run(sys, rcpCycles);
}
