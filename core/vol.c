/* vol.c -- the driver's LINEAR volume chain and the SPU2 pan table.
 *
 * Everything here was read from the game ELF with Ghidra -- no formula is
 * inherited from the offline reference without a code read:
 *
 *   FUN_00400c00   per-voice VOLL/VOLR register math (disassembly verified word by
 *                  word); mirrored by ae3__voice_regs and, independently, the corpus gates.
 *   FUN_003facb8   note-on: voice vol = CC7*CC11*prog[1]*tone[11] (FUN_00421a80
 *                  multiply chain) / 0x1F417F = 127^3 (FUN_00422d30); channel pan
 *                  clamp(1..127); tone pan >127 -> 127, ==0 -> 1.
 *   FUN_003fab98   CC7/CC10/CC11 handlers re-run exactly those two computations on
 *                  every live voice of the channel (velocity and tone pan stay).
 *   FUN_003f7f58   state defaults: pre-volumes state[0xb]/[0xc] = 127/127 (the "127"
 *                  in the register formula; the song-fade API can move them -- never
 *                  during normal BGM playback), pan mid term state[0x10] = 0x40,
 *                  phase words +0x7c/+0x7e = +1 (only CC92 -- never sent in all 68
 *                  sequences -- writes the -1 that would flip a side's sign).
 *   FUN_0035f97c   boot: output shift = 0 (FUN_003ffc58(0) overrides the init's 1),
 *                  master MVOLL/MVOLR = 0x3FFF on both cores (FUN_003f6948) = unity.
 *   FUN_003676f8   sound_output_method: "stereo"/"dolby" -> mode 2, "monaural" -> 1;
 *                  FUN_00400c00 tests only bit 0, so BGM always takes the pan-table
 *                  path (mono would pin the index to 64).
 *
 * The pan table lives at EE 0x0069DD60 (u16 per entry, hi byte = left gain, lo =
 * right, both 0..127). It is a RULE, not arbitrary data, so it is rebuilt here the
 * same way the offline reference does (the corpus gates verify all 128 entries
 * against the ELF bytes): one side pins at 127 while the other ramps by 2 to index 49 then by 1 to
 * 63; 64 is special-cased at 120/120; 65..127 mirror 63..1. NOT constant power:
 * centre (120/120) carries 2.52 dB more total power than hard pan (127/0) -- the
 * game's mix leans on this, do not "fix" it. */
#include "internal.h"

const uint16_t *ae3__pan_lut(void)
{
    static uint16_t t[128];                    /* entry 0 stays (0,0): silent */
    if (!t[64]) {
        for (int i = 1; i < 64; i++)
            t[i] = (uint16_t)(127 << 8 | (i <= 49 ? 2 * (i - 1) : 96 + (i - 49)));
        t[64] = 120 << 8 | 120;
        for (int i = 65; i < 128; i++)
            t[i] = (uint16_t)((t[128 - i] & 0xFF) << 8 | t[128 - i] >> 8);
    }
    return t;
}

/* Note-on / CC-refresh product (voice +0x44). Sequential multiplies then ONE
 * truncating division, exactly as the FUN_00421a80/FUN_00422d30 chain does it
 * (max 127^4 = 260 M, no 32-bit overflow). Result 0..127. */
int32_t ae3__vol_product(int cc7, int cc11, int progvol, int tonevol)
{
    return ((cc7 * cc11) * progvol) * tonevol / 0x1F417F;
}

int ae3__cpan_clamp(int v)                     /* FUN_003facb8 / FUN_003fab98 */
{
    return v > 0x7F ? 0x7F : v < 1 ? 1 : v;
}

int ae3__tpan_clamp(int v)                     /* FUN_003facb8: ==0 (not <1) -> 1 */
{
    return v > 0x7F ? 0x7F : v == 0 ? 1 : v;
}

/* The FUN_00400c00 register math, with the boot-pinned constants folded in: pan
 * mid term 0x40, output shift 0, voice type 1 (the type-2 extra scale never runs
 * for BGM), phase +1 (no sign flip). svl/svr are the song-volume pre-volumes
 * (state[0xb]/[0xc], voice +0x38/+0x3c) -- 127/127 at driver init, moved by the
 * game's fade/volume API. Registers land in the SPU2's fixed-volume /2 format:
 * 0..0x3FFF = amplitude 0..0x7FFE/0x8000, hence the 0x3FFF clamp (max value at
 * default inputs is 127*127 = 0x3F01, so it only guards hot fade-API values). */
void ae3__voice_regs(int svl, int svr, int32_t vvol, int vel, int cpan, int tpan,
                     uint16_t *voll, uint16_t *volr)
{
    int idx = 0x40 + cpan + tpan - 0x80;
    if (idx > 0x7F)
        idx = 0x7F;
    if (idx < 1)
        idx = 1;
    uint16_t e = ae3__pan_lut()[idx];
    int32_t linl = (svl * vvol * vel) / 0x3F01;  /* 0x3F01 = 127^2 */
    int32_t linr = (svr * vvol * vel) / 0x3F01;
    if (linl < 0)
        linl = 0;
    if (linr < 0)
        linr = 0;
    int32_t l = linl * (e >> 8);
    int32_t r = linr * (e & 0xFF);
    *voll = (uint16_t)(l > 0x3FFF ? 0x3FFF : l);
    *volr = (uint16_t)(r > 0x3FFF ? 0x3FFF : r);
}
