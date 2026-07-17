/* bank.c -- Sony "Jam" bank (.hd) parser. C mirror of tools/bgm.py parse_hd(); every
 * validation there is a hard error here too, so a bank this code misreads refuses to load
 * instead of playing wrong. Format provenance and proofs: research/BGM.md. */
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void ae3__bank_free(ae3_bank *b)
{
    for (int i = 0; i < b->nprogs; i++)
        free(b->progs[i].tones);
    free(b->progs);
    free(b->bd);
    memset(b, 0, sizeof *b);
}

int ae3__parse_bank(ae3_synth *s, const uint8_t *hd, size_t hd_len,
                    const uint8_t *bd, size_t bd_len)
{
    ae3_bank *bk = &s->bank;

    if (hd_len < 0x80)
        return ae3__fail(s, "hd too small: %zu bytes", hd_len);
    uint32_t hd_sz = rd32(hd), bd_sz = rd32(hd + 4), zero = rd32(hd + 8);
    if (hd_sz != hd_len || zero != 0)
        return ae3__fail(s, "size prefix: hd=%u vs %zu, pad=%u", hd_sz, hd_len, zero);
    if (bd_sz != bd_len)
        return ae3__fail(s, "prefix says bd=%u, real .bd is %zu", bd_sz, bd_len);
    if (memcmp(hd + 0x0C, "SShd", 4) != 0)
        return ae3__fail(s, "no SShd magic at 0x0C");

    /* Six signed chunk offsets at 0x10; -1 = absent. The three SE chunks are -1 in all
     * 62 music banks; only s_20_park has an LFO chunk (not parsed yet -- milestone 9). */
    int32_t prog_off = (int32_t)rd32(hd + 0x10), vel_off = (int32_t)rd32(hd + 0x14);
    int32_t se_seq = (int32_t)rd32(hd + 0x1C), unk5 = (int32_t)rd32(hd + 0x20);
    int32_t se_prog = (int32_t)rd32(hd + 0x24);
    if (prog_off != 0x80)
        return ae3__fail(s, "program chunk at %#x, expected 0x80", prog_off);
    if (se_seq != -1 || unk5 != -1 || se_prog != -1)
        return ae3__fail(s, "unexpected SE chunks: %#x %#x %#x", se_seq, unk5, se_prog);
    if (vel_off < 0 || (size_t)vel_off + 2 + 128 > hd_len)
        return ae3__fail(s, "velocity chunk at %#x out of range", vel_off);

    size_t B = (size_t)prog_off;
    /* Count field is the LAST INDEX, not the count (proof in bgm.py: the MIDI's highest
     * program number equals it exactly on p_7 / j_6 / s_25_jungle). */
    int nprog = rd16(hd + B) + 1;
    if (B + 2 + (size_t)nprog * 2 > hd_len)
        return ae3__fail(s, "program offset table overruns hd");
    const uint8_t *offs = hd + B + 2;
    if (rd16(offs) != 2 + (unsigned)nprog * 2)
        return ae3__fail(s, "first offset %#x != table size %#x", rd16(offs), 2 + nprog * 2);

    bk->progs = calloc((size_t)nprog, sizeof *bk->progs);
    if (!bk->progs)
        return ae3__fail(s, "out of memory");
    bk->nprogs = nprog;

    for (int i = 0; i < nprog; i++) {
        uint16_t o = rd16(offs + i * 2);
        if (o == 0xFFFF)                       /* unused program slot */
            continue;
        size_t a = B + o;
        /* end = next used slot's offset, or the velocity chunk for the last program */
        size_t b = (size_t)vel_off;
        for (int j = i + 1; j < nprog; j++) {
            uint16_t oj = rd16(offs + j * 2);
            if (oj != 0xFFFF) { b = B + oj; break; }
        }
        if (b <= a || b > hd_len)
            return ae3__fail(s, "prog %d: bad span %zu..%zu", i, a, b);
        size_t size = b - a;
        if (size < 8 || (size - 8) % 16)
            return ae3__fail(s, "prog %d: size %zu is not 8+16N", i, size);
        int n = (int)((size - 8) / 16);
        const uint8_t *h = hd + a;

        ae3_prog *p = &bk->progs[i];
        p->present = true;
        p->drum = h[0] == 0xFF;
        p->stack = !p->drum && (h[0] & 0x80);
        p->vol = h[1]; p->bend = h[4]; p->lfo = h[5]; p->key0 = h[6]; p->key1 = h[7];
        if (p->drum) {
            /* One tone per key over bytes 6-7's range; verified on all 7 drum kits. */
            if (h[7] - h[6] + 1 != n)
                return ae3__fail(s, "prog %d: drum keys %u-%u vs %d tones", i, h[6], h[7], n);
        } else if ((h[0] & 0x7F) != n - 1) {
            /* & 0x7F, not GT4SoundTool's & 0x0F: 0x97 means 24 tones, bit 7 is a flag. */
            return ae3__fail(s, "prog %d: header %#x vs %d tones", i, h[0], n);
        }

        p->tones = calloc((size_t)n, sizeof *p->tones);
        if (!p->tones)
            return ae3__fail(s, "out of memory");
        p->ntones = n;
        for (int t = 0; t < n; t++) {
            const uint8_t *r = h + 8 + t * 16;
            ae3_tone *x = &p->tones[t];
            if (p->drum) {
                /* Key comes from POSITION; a drum tone's own bytes 0/1 are zero. */
                x->lo = x->hi = (uint8_t)(h[6] + t);
            } else {
                x->lo = r[0]; x->hi = r[1];
            }
            x->root = r[2];
            x->tune = (int8_t)r[3];            /* signed: `lb` at EE 0x003f896c */
            x->addr = rd16(r + 4);
            x->adsr1 = rd16(r + 6);
            x->adsr2 = rd16(r + 8);
            x->vol = r[11]; x->pan = r[12]; x->flags = r[15];
            x->bend_raw = r[13];
            x->bend = (x->flags & AE3_TF_USE_PROG_BEND) ? h[4] : r[13];
            x->lfo  = (x->flags & AE3_TF_USE_PROG_LFO)  ? h[5] : r[14];
            s->st.tones++;
        }
        s->st.progs_used++;
    }
    s->st.prog_slots = (uint32_t)nprog;

    bk->vel_count = rd16(hd + vel_off);
    memcpy(bk->vel, hd + vel_off + 2, 128);

    bk->bd = malloc(bd_len ? bd_len : 1);
    if (!bk->bd)
        return ae3__fail(s, "out of memory");
    memcpy(bk->bd, bd, bd_len);
    bk->bd_len = bd_len;
    return 0;
}
