/* exst.c -- EXST (.x) streamed-ADPCM decoder (sound/stream family).
 *
 * Format ground truth: docs/formats/EXST.md (every header field provenanced
 * against the EE parser, sg2exst.c module in SCUS_975.01). Payload is the
 * shared PS-ADPCM codec; the frame math mirrors voice.c's decode_frame --
 * and thereby bgm.decode_adpcm -- bit for bit, which the private corpus
 * gates verify across all 1158 shipped files. Unlike the bank decoder,
 * payload flags never stop decode: streams decode to the end of the payload
 * (the EE keys voices on/off itself and never reads the flags; the corpus
 * carries exactly two stray END flags mid-file -- EXST.md §2). */
#include <string.h>

#include "internal.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int ae3_exst_parse(const void *hdr, size_t len, ae3_exst_header *out)
{
    const uint8_t *h = hdr;
    if (!h || len < AE3_EXST_HDR || memcmp(h, "EXST", 4) != 0)
        return -1;
    /* channel count = s16 at +0x06 (the stored u32 is ch<<16); the EE parser
     * accepts 1..8 and so does this one */
    uint16_t ch = (uint16_t)(h[0x06] | h[0x07] << 8);
    if (ch < 1 || ch > AE3_EXST_MAXCH)
        return -1;
    out->channels = ch;
    out->rate = rd32(h + 0x08);
    out->loop = rd32(h + 0x0c);
    out->loop_start = rd32(h + 0x10);
    out->length = rd32(h + 0x14);
    for (int i = 0; i < AE3_EXST_MAXCH; i++) {
        out->vol_l[i] = rd32(h + 0x18 + i * 4);
        out->vol_r[i] = rd32(h + 0x38 + i * 4);
        out->reverb[i] = rd32(h + 0x58 + i * 4);
    }
    return 0;
}

/* Per-sector decode slices the sector 2048/ch bytes per channel (the EE's own
 * SPU slice, FUN_003f6bd0); a slice must be whole 16-byte frames, so ch is
 * restricted to {1,2,4,8}. Every shipped file is mono or stereo. */
static int slice_bytes(int channels)
{
    if (channels < 1 || channels > AE3_EXST_MAXCH ||
        AE3_EXST_SECTOR % (channels * 16) != 0)
        return 0;
    return AE3_EXST_SECTOR / channels;
}

int ae3_exst_reset(ae3_exst *d, int channels)
{
    memset(d, 0, sizeof *d);
    if (!slice_bytes(channels))
        return -1;
    d->channels = channels;
    return 0;
}

int ae3_exst_decode(ae3_exst *d, const void *sector, int16_t *pcm)
{
    int ch = d->channels;
    int per = slice_bytes(ch);
    if (!per)
        return -1;
    const uint8_t *sec = sector;
    for (int c = 0; c < ch; c++) {
        const uint8_t *p = sec + c * per;
        int32_t h1 = d->h1[c], h2 = d->h2[c];
        int16_t *o = pcm + c;
        for (int f = 0; f < per / 16; f++, p += 16) {
            int shift = p[0] & 0x0F, filt = (p[0] >> 4) & 0x0F;
            if (shift > 12)                    /* hardware quirk clamps, as voice.c */
                shift = 9;
            if (filt > 4)
                filt = 0;
            int32_t c0 = ae3__adpcm_coef[filt][0], c1 = ae3__adpcm_coef[filt][1];
            for (int i = 0; i < 14; i++) {
                uint8_t b = p[2 + i];
                for (int n = 0; n < 2; n++) {
                    int32_t s = (int16_t)((uint16_t)((n ? b >> 4 : b & 0x0F) << 12));
                    s = (s >> shift) + ((h1 * c0 + h2 * c1) >> 6);
                    s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
                    *o = (int16_t)s;
                    o += ch;
                    h2 = h1;
                    h1 = s;
                }
            }
        }
        d->h1[c] = h1;
        d->h2[c] = h2;
    }
    return per / 16 * 28;
}

uint32_t ae3_exst_trailing_pad(const void *payload, size_t len, int channels)
{
    static const uint8_t zero14[14];
    int per = slice_bytes(channels);
    if (!per)
        return 0;
    size_t nsec = len / AE3_EXST_SECTOR;
    const uint8_t *pay = payload;
    uint32_t min_run = 0;
    for (int c = 0; c < channels; c++) {
        uint32_t run = 0;
        for (size_t s = nsec; s-- > 0; ) {
            for (int f = per / 16; f-- > 0; ) {
                const uint8_t *p = pay + s * AE3_EXST_SECTOR + c * per + f * 16;
                /* flag byte EXACTLY 0x02 + silent data; byte 0 is ignored --
                 * the oracle's rule (exst.trailing_pad_frames), matched by
                 * every pad frame in the corpus */
                if (p[1] != AE3_AF_REPEAT || memcmp(p + 2, zero14, 14) != 0)
                    goto done;
                run++;
            }
        }
    done:
        if (c == 0 || run < min_run)
            min_run = run;
    }
    return min_run;
}
