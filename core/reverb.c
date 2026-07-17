/* reverb.c -- the SPU2's fixed reverb, streaming, per-sample.
 *
 * A port of tools/spu2rev.c (the offline oracle) into the live mixer. The algorithm
 * is the psx-spx SPU reverb formula -- a per-sample feedback network at the half
 * rate 24000 Hz -- with the bus resampled 48k<->24k through the hardware's 39-tap
 * half-band FIR (taps from PCSX2 ReverbResample.cpp, reproduced as a description of
 * hardware behavior, credited; the network itself is written from psx-spx, not from
 * PCSX2's GPL code -- the project's clean-room stance, see SYNTH_HANDOFF §7).
 *
 * NOTHING GAME-DERIVED IS BAKED IN: the preset coefficients are read out of Sony's
 * libsd.irx at runtime (ae3__load_libsd), exactly as bgm.load_reverb does.
 *
 * EXACTNESS CONTRACT with the offline oracle (see internal.h "GRID + LATENCY"):
 * every floating-point expression here keeps spu2rev.c's operand order, so with
 * -ffp-contract=off (Makefile-pinned) the doubles are bit-identical and the only
 * difference is the causal FIRs' fixed 38-sample delay. check.py leans on that.
 * The return-path sum iterates oldest-to-newest like the oracle's k-ascending loop;
 * its FIR indices come out mirrored (k -> 38-k), which is the same tap value -- the
 * table is symmetric -- so the products and their order match bit for bit. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/* SPU2 reverb-bus half-band FIR (down: /32768; up: same taps x2, over a zero-stuffed
 * stream). Values from PCSX2 pcsx2/SPU2/ReverbResample.cpp (GPL-3.0+); reproduced
 * here as a description of what the hardware's resampler does, credited accordingly. */
static const double FIR[AE3_REV_FIRTAPS] = {
    -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246,
    16384,
    10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
};

/* Preset field order as libsd stores it (same enum as spu2rev.c). */
enum {
    dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2,
    mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME,
    mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF,
    mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN, NCOEF
};

/* The boot pin (EE 0x0035fa1c, both cores, never changed): type 0x104 =
 * CLEAR_WA | SD_REV_MODE_STUDIO_C, depth 0x1e. libsd's tables for that type: */
#define LIBSD_SIZE_TBL   0x4db8u    /* u32 per preset: work-area size, 8-byte units */
#define LIBSD_PRESET_TBL 0x4de0u    /* 0x44-byte records; 32 u16 coefs at +4 */
#define LIBSD_STRIDE     0x44u
#define REV_TYPE         4          /* SD_REV_MODE_STUDIO_C */
#define REV_BOOT_DEPTH   30

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* libsd VA -> file offset via the PROGBITS section map (mirror of bgm.load_reverb's
 * va2off; libsd.irx is a linked ELF whose sections carry addresses). */
static long va2off(const uint8_t *d, size_t len, uint32_t va, uint32_t need)
{
    if (len < 0x34 || memcmp(d, "\x7f""ELF", 4) != 0)
        return -1;
    uint32_t shoff = rd32(d + 0x20);
    uint16_t shentsize = rd16(d + 0x2e), shnum = rd16(d + 0x30);
    if ((size_t)shoff + (size_t)shnum * shentsize > len)
        return -1;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (size_t)i * shentsize;
        uint32_t typ = rd32(sh + 4), addr = rd32(sh + 12);
        uint32_t off = rd32(sh + 16), size = rd32(sh + 20);
        if (typ != 1 || !size || va < addr || va >= addr + size)   /* SHT_PROGBITS */
            continue;
        if (va - addr + need > size || (size_t)off + (va - addr) + need > len)
            return -1;
        return (long)(off + (va - addr));
    }
    return -1;
}

int ae3__load_libsd(ae3_synth *s, const uint8_t *d, size_t len)
{
    ae3_rev *r = &s->rev;
    long so = va2off(d, len, LIBSD_SIZE_TBL + REV_TYPE * 4, 4);
    long po = va2off(d, len, LIBSD_PRESET_TBL + REV_TYPE * LIBSD_STRIDE + 4,
                     2 * AE3_REV_NCOEF);
    if (so < 0 || po < 0)
        return ae3__fail(s, "libsd: reverb tables not mapped (wrong irx?)");
    uint32_t units = rd32(d + so);
    uint16_t raw[AE3_REV_NCOEF];
    for (int i = 0; i < AE3_REV_NCOEF; i++)
        raw[i] = rd16(d + po + 2 * i);
    /* the same self-check bgm.load_reverb does: every address tap must fit the
     * work area (this is what catches reading the u32 size table as u16) */
    for (int i = mLSAME; i <= mRAPF2; i++)
        if (raw[i] >= units)
            return ae3__fail(s, "libsd: preset tap %u does not fit area %u",
                             raw[i], units);
    /* the network reads below some taps (the "-1" slot, the APF delay); keep those
     * subtractions non-negative (true of libsd's presets; spu2rev.c assumes it) */
    if (raw[mLAPF1] < raw[dAPF1] || raw[mRAPF1] < raw[dAPF1] ||
        raw[mLAPF2] < raw[dAPF2] || raw[mRAPF2] < raw[dAPF2] ||
        !raw[mLSAME] || !raw[mRSAME] || !raw[mLDIFF] || !raw[mRDIFF])
        return ae3__fail(s, "libsd: preset taps read before the work area");

    double *buf = calloc((size_t)units * 4, sizeof(double));
    if (!buf)
        return ae3__fail(s, "libsd: cannot allocate %u reverb slots", units * 4);
    ae3__rev_free(r);
    memset(r, 0, sizeof *r);
    memcpy(r->raw, raw, sizeof raw);
    r->units = units;
    r->nslots = (long)units * 4;
    r->buf = buf;
    for (int i = 0; i < AE3_REV_NCOEF; i++) {
        /* v-coefs are signed 1.15; the m- and d- taps are 8-byte units, x4 to slots */
        r->v[i] = (double)(int16_t)raw[i] / 32768.0;
        r->m[i] = (long)raw[i] * 4;
    }
    ae3__rev_set_depth(r, REV_BOOT_DEPTH);
    s->rev_tail_left = AE3_REV_TAIL_SAMPLES;
    return 0;
}

void ae3__rev_set_depth(ae3_rev *r, int depth)
{
    if (depth < 0)
        depth = 0;
    if (depth > 127)
        depth = 127;
    r->depth = depth;
    int e = depth * 32767 / 127;         /* EE 0x3f67c8: integer, truncating */
    r->evol = (double)e / 32768.0;       /* applied to the return as 1.15 */
    r->on = r->buf != NULL && depth > 0;
}

void ae3__rev_reset(ae3_rev *r)
{
    if (r->buf)
        memset(r->buf, 0, (size_t)r->nslots * sizeof(double));
    memset(r->dn_l, 0, sizeof r->dn_l);
    memset(r->dn_r, 0, sizeof r->dn_r);
    memset(r->up_l, 0, sizeof r->up_l);
    memset(r->up_r, 0, sizeof r->up_r);
    r->nsamp = r->mcnt = 0;
}

void ae3__rev_free(ae3_rev *r)
{
    free(r->buf);
    r->buf = NULL;
    r->on = false;
}

void ae3__rev_sample(ae3_rev *r, int16_t wl, int16_t wr, double *outl, double *outr)
{
    uint64_t n = r->nsamp++;
    r->dn_l[n & 63] = wl;
    r->dn_r[n & 63] = wr;

    if (n & 1) {
        /* 48k -> 24k: one decimated input through the half-band FIR, over the last
         * 39 wet samples (oldest first -- the oracle's k-ascending order). The <0
         * guard matches its j-range skip; the ring is long enough that live taps
         * never alias. */
        double sl = 0, sr = 0;
        for (int k = 0; k < AE3_REV_FIRTAPS; k++) {
            int64_t j = (int64_t)n - (AE3_REV_FIRTAPS - 1) + k;
            if (j >= 0) {
                sl += FIR[k] * r->dn_l[j & 63];
                sr += FIR[k] * r->dn_r[j & 63];
            }
        }
        double inl = sl / (32768.0 * 32768.0);   /* /32768 FIR gain, /32768 s16 */
        double inr = sr / (32768.0 * 32768.0);

        /* one network step at the reverb clock -- spu2rev.c's loop body verbatim
         * ("-2" in psx-spx is 2 bytes = 1 slot; note the L/R swap on the
         * different-side source taps) */
        const double *v = r->v;
        const long *m = r->m;
        long p = (long)(r->mcnt % (uint64_t)r->nslots);
#define R(off) r->buf[(p + (off)) % r->nslots]
        double Lin = v[vLIN] * inl;
        double Rin = v[vRIN] * inr;
        double a;
        a = R(m[mLSAME] - 1); R(m[mLSAME]) = (Lin + R(m[dLSAME]) * v[vWALL] - a) * v[vIIR] + a;
        a = R(m[mRSAME] - 1); R(m[mRSAME]) = (Rin + R(m[dRSAME]) * v[vWALL] - a) * v[vIIR] + a;
        a = R(m[mLDIFF] - 1); R(m[mLDIFF]) = (Lin + R(m[dRDIFF]) * v[vWALL] - a) * v[vIIR] + a;
        a = R(m[mRDIFF] - 1); R(m[mRDIFF]) = (Rin + R(m[dLDIFF]) * v[vWALL] - a) * v[vIIR] + a;
        double Lo = v[vCOMB1] * R(m[mLCOMB1]) + v[vCOMB2] * R(m[mLCOMB2])
                  + v[vCOMB3] * R(m[mLCOMB3]) + v[vCOMB4] * R(m[mLCOMB4]);
        double Ro = v[vCOMB1] * R(m[mRCOMB1]) + v[vCOMB2] * R(m[mRCOMB2])
                  + v[vCOMB3] * R(m[mRCOMB3]) + v[vCOMB4] * R(m[mRCOMB4]);
        a = R(m[mLAPF1] - m[dAPF1]); Lo -= v[vAPF1] * a; R(m[mLAPF1]) = Lo; Lo = Lo * v[vAPF1] + a;
        a = R(m[mRAPF1] - m[dAPF1]); Ro -= v[vAPF1] * a; R(m[mRAPF1]) = Ro; Ro = Ro * v[vAPF1] + a;
        a = R(m[mLAPF2] - m[dAPF2]); Lo -= v[vAPF2] * a; R(m[mLAPF2]) = Lo; Lo = Lo * v[vAPF2] + a;
        a = R(m[mRAPF2] - m[dAPF2]); Ro -= v[vAPF2] * a; R(m[mRAPF2]) = Ro; Ro = Ro * v[vAPF2] + a;
#undef R
        r->up_l[r->mcnt & 31] = Lo;
        r->up_r[r->mcnt & 31] = Ro;
        r->mcnt++;
    }

    /* 24k -> 48k: zero-stuff the network output on odd frames and convolve with
     * the same FIR at x2 gain. Walking network outputs t oldest-to-newest, frame
     * n - (2t+1) back, keeps the oracle's summation order (see file header). */
    int64_t tmax = ((int64_t)n - 1) >> 1;
    int64_t tmin = tmax - (n & 1 ? 19 : 18);
    if (tmin < 0)
        tmin = 0;
    double sl = 0, sr = 0;
    for (int64_t t = tmin; t <= tmax; t++) {
        int k = (int)((int64_t)n - 2 * t - 1);
        sl += FIR[k] * r->up_l[t & 31];
        sr += FIR[k] * r->up_r[t & 31];
    }
    *outl = sl * (2.0 / 32768.0);
    *outr = sr * (2.0 / 32768.0);
}
