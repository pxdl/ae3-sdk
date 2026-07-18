/* bank.c -- Sony "Jam" bank (.hd) parser. C mirror of the project's offline reference
 * parser; every validation there is a hard error here too, so a bank this code misreads refuses to load
 * instead of playing wrong. Format provenance and proofs: docs/formats/BGM.md. */
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
    free(b->waves);
    memset(b, 0, sizeof *b);
}

/* ---- waveform introspection table (public API at the bottom) -------------- */

/* Single-pass decode of the waveform at byte offset `start`: the note-on
 * path's streaming decoder run to the END frame (or off the body's end).
 * out may be NULL / short (count-only: at most `max` samples are written,
 * the full count is still returned). Sets *loop_start to the sample index
 * the END+REPEAT frame jumps back to, -1 for a one-shot. */
static uint32_t wave_decode(const uint8_t *bd, size_t bd_len, uint32_t start,
                            int16_t *out, uint32_t max, int32_t *loop_start)
{
    ae3_adpcm d;
    ae3_adpcm_init(&d, bd, bd_len, start);
    uint32_t n = 0;
    int32_t loop = -1;
    int16_t v;
    for (;;) {
        if (!ae3_adpcm_next(&d, &v))
            break;                    /* one-shot ended (or ran off the body) */
        if (d.loops) {                /* jumped: v is the first post-seam sample */
            loop = d.loop_frame >= 0 ? (d.loop_frame - (int32_t)d.start) / 16 * 28
                                     : 0;   /* SPU default: loop to the start */
            break;
        }
        if (out && n < max)
            out[n] = v;
        n++;
    }
    *loop_start = loop;
    return n;
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

static int cmp_wave_addr(const void *key, const void *elem)
{
    uint32_t k = *(const uint32_t *)key;
    const ae3_waveform *w = elem;
    return k < w->addr ? -1 : k > w->addr ? 1 : 0;
}

static int build_waves(ae3_synth *s)
{
    ae3_bank *bk = &s->bank;
    int nt = 0;
    for (int i = 0; i < bk->nprogs; i++)
        for (int t = 0; bk->progs[i].present && t < bk->progs[i].ntones; t++)
            if (bk->progs[i].tones[t].addr != AE3_NO_SAMPLE)
                nt++;
    if (nt == 0)
        return 0;                     /* nwaves stays 0 */

    uint16_t *addrs = malloc((size_t)nt * sizeof *addrs);
    if (!addrs)
        return ae3__fail(s, "out of memory");
    int na = 0;
    for (int i = 0; i < bk->nprogs; i++)
        for (int t = 0; bk->progs[i].present && t < bk->progs[i].ntones; t++)
            if (bk->progs[i].tones[t].addr != AE3_NO_SAMPLE)
                addrs[na++] = bk->progs[i].tones[t].addr;
    qsort(addrs, (size_t)na, sizeof *addrs, cmp_u16);

    int nu = 0;
    for (int i = 0; i < na; i++)
        if (i == 0 || addrs[i] != addrs[i - 1])
            nu++;
    bk->waves = calloc((size_t)nu, sizeof *bk->waves);
    if (!bk->waves) {
        free(addrs);
        return ae3__fail(s, "out of memory");
    }
    for (int i = 0, w = 0; i < na; i++)
        if (i == 0 || addrs[i] != addrs[i - 1])
            bk->waves[w++].addr = addrs[i];
    bk->nwaves = nu;
    free(addrs);

    for (int w = 0; w < nu; w++)
        bk->waves[w].samples = wave_decode(bk->bd, bk->bd_len,
                                           bk->waves[w].addr * 8, NULL, 0,
                                           &bk->waves[w].loop_start);

    /* first-referencing (prog, tone) + reference counts, in slot/tone order */
    for (int i = 0; i < bk->nprogs; i++)
        for (int t = 0; bk->progs[i].present && t < bk->progs[i].ntones; t++) {
            const ae3_tone *x = &bk->progs[i].tones[t];
            if (x->addr == AE3_NO_SAMPLE)
                continue;
            uint32_t key = x->addr;
            ae3_waveform *wv = bsearch(&key, bk->waves, (size_t)nu,
                                       sizeof *bk->waves, cmp_wave_addr);
            if (wv->refs++ == 0) {
                wv->prog = (uint16_t)i;
                wv->tone = (uint16_t)t;
                wv->root = x->root;
                wv->tune = x->tune;
            }
        }
    return 0;
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
    /* Count field is the LAST INDEX, not the count (proof: the MIDI's highest
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
    return build_waves(s);
}

/* ---- bank introspection: the public read-only surface --------------------- */

int ae3_synth_bank_waveforms(const ae3_synth *s)
{
    return s->have_bank ? s->bank.nwaves : 0;
}

bool ae3_synth_bank_waveform(const ae3_synth *s, int i, ae3_waveform *out)
{
    if (!s->have_bank || i < 0 || i >= s->bank.nwaves)
        return false;
    *out = s->bank.waves[i];
    return true;
}

int ae3_synth_bank_waveform_pcm(const ae3_synth *s, int i,
                                int16_t *out, uint32_t max)
{
    if (!s->have_bank || i < 0 || i >= s->bank.nwaves)
        return -1;
    int32_t loop;
    return (int)wave_decode(s->bank.bd, s->bank.bd_len,
                            s->bank.waves[i].addr * 8, out, max, &loop);
}
