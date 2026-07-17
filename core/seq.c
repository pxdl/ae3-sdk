/* seq.c -- MIDI (format 0) parser + the sample-accurate tempo clock.
 *
 * Event walking mirrors the offline reference parser byte for byte (including its running-
 * status behavior), then collapses to what the synth dispatches: channel events, tempo
 * (FF 51), end-of-track (FF 2F). Other meta/sysex are parsed past and counted.
 *
 * The clock: sample(tick) = seg_sample + (tick - seg_tick) * spt, where spt =
 * AE3_RATE * uspqn / (1e6 * ppqn). Tempo events start a new segment at their own exact
 * sample position, so conversion never drifts and never rounds twice. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define FNV_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

static uint64_t fnv_tick(uint64_t h, uint32_t tick)
{
    uint8_t b[4] = { (uint8_t)tick, (uint8_t)(tick >> 8),
                     (uint8_t)(tick >> 16), (uint8_t)(tick >> 24) };
    return fnv1a(h, b, 4);
}

void ae3__seq_free(ae3_seq *q)
{
    free(q->ev);
    memset(q, 0, sizeof *q);
}

void ae3__seq_reset_clock(ae3_seq *q)
{
    q->next = 0;
    q->ended = false;
    q->seg_tick = 0;
    q->seg_sample = 0.0;
    q->spt = (double)AE3_RATE * 500000.0 / (1e6 * q->ppqn);   /* MIDI default 120 bpm */
}

static int vlq(const uint8_t *d, size_t len, size_t *p, uint32_t *out)
{
    uint32_t v = 0;
    do {
        if (*p >= len) return -1;
        v = (v << 7) | (d[*p] & 0x7F);
    } while (d[(*p)++] & 0x80);
    *out = v;
    return 0;
}

int ae3__parse_seq(ae3_synth *s, const uint8_t *mid, size_t len)
{
    ae3_seq *q = &s->seq;

    if (len < 22 || memcmp(mid, "MThd", 4) != 0)
        return ae3__fail(s, "not a MIDI file (no MThd)");
    uint32_t hlen = (uint32_t)mid[4] << 24 | (uint32_t)mid[5] << 16
                  | (uint32_t)mid[6] << 8 | mid[7];
    unsigned fmt = (unsigned)mid[8] << 8 | mid[9];
    unsigned ntrk = (unsigned)mid[10] << 8 | mid[11];
    unsigned div = (unsigned)mid[12] << 8 | mid[13];
    if (hlen != 6 || fmt != 0 || ntrk != 1)
        return ae3__fail(s, "expected format 0 / 1 track, got fmt=%u ntrk=%u", fmt, ntrk);
    if (div & 0x8000)
        return ae3__fail(s, "SMPTE division unsupported");
    q->ppqn = (uint16_t)div;

    if (memcmp(mid + 14, "MTrk", 4) != 0)
        return ae3__fail(s, "no MTrk at 14");
    uint32_t tlen = (uint32_t)mid[18] << 24 | (uint32_t)mid[19] << 16
                  | (uint32_t)mid[20] << 8 | mid[21];
    if (22 + (size_t)tlen > len)
        return ae3__fail(s, "MTrk length %u overruns file", tlen);
    const uint8_t *d = mid + 22;

    /* Worst case one event per 2 track bytes; one alloc, no growth. */
    q->ev = malloc((tlen / 2 + 2) * sizeof *q->ev);
    if (!q->ev)
        return ae3__fail(s, "out of memory");

    uint64_t hch = FNV_BASIS, htempo = FNV_BASIS;
    size_t p = 0;
    uint32_t tick = 0;
    uint8_t run = 0;
    bool ended = false;

    while (p < tlen) {
        if (ended)
            return ae3__fail(s, "events after end-of-track at byte %zu", p);
        uint32_t dt;
        if (vlq(d, tlen, &p, &dt))
            return ae3__fail(s, "truncated delta at %zu", p);
        tick += dt;
        if (p >= tlen)
            return ae3__fail(s, "truncated event at %zu", p);
        if (d[p] & 0x80)
            run = d[p++];
        uint8_t st = run;

        if (st == 0xFF) {
            if (p >= tlen)
                return ae3__fail(s, "truncated meta at %zu", p);
            uint8_t type = d[p++];
            uint32_t ln;
            if (vlq(d, tlen, &p, &ln) || p + ln > tlen)
                return ae3__fail(s, "truncated meta payload at %zu", p);
            if (type == 0x51 && ln == 3) {
                uint32_t uspqn = (uint32_t)d[p] << 16 | (uint32_t)d[p + 1] << 8 | d[p + 2];
                q->ev[q->nev++] = (ae3_event){ .tick = tick, .kind = AE3_EV_TEMPO,
                                               .uspqn = uspqn };
                htempo = fnv_tick(htempo, tick);
                htempo = fnv1a(htempo, d + p, 3);
                s->st.tempo_changes++;
            } else if (type == 0x2F) {
                q->ev[q->nev++] = (ae3_event){ .tick = tick, .kind = AE3_EV_END };
                s->st.end_tick = tick;
                ended = true;
            } else {
                s->st.meta_skipped++;
            }
            p += ln;
        } else if (st == 0xF0 || st == 0xF7) {
            uint32_t ln;
            if (vlq(d, tlen, &p, &ln) || p + ln > tlen)
                return ae3__fail(s, "truncated sysex at %zu", p);
            s->st.meta_skipped++;
            p += ln;
        } else if (st & 0x80) {
            uint8_t hi = st & 0xF0;
            uint8_t nd = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
            if (p + nd > tlen)
                return ae3__fail(s, "truncated channel event at %zu", p);
            ae3_event *e = &q->ev[q->nev++];
            *e = (ae3_event){ .tick = tick, .kind = AE3_EV_CH, .status = st,
                              .a = d[p], .b = nd == 2 ? d[p + 1] : 0, .nd = nd };
            hch = fnv_tick(hch, tick);
            hch = fnv1a(hch, &st, 1);
            hch = fnv1a(hch, d + p, nd);
            s->st.events++;
            switch (hi) {
            case 0x90: if (e->b) s->st.note_ons++; else s->st.note_offs++; break;
            case 0x80: s->st.note_offs++; break;
            case 0xB0:
                s->st.ccs++;
                /* CCs the game's SMF walker consumes (FUN_00402108): reclassify so
                 * they never dispatch into driver channel state -- like hardware.
                 * The hash above covers the raw stream, so these stay in it. */
                if (e->a == 99 && e->b == 20) {
                    e->kind = AE3_EV_LOOP_START;
                    s->st.loop_starts++;
                } else if (e->a == 99 && e->b == 30) {
                    e->kind = AE3_EV_LOOP_END;
                    s->st.loop_ends++;
                } else if (e->a == 102) {
                    e->kind = AE3_EV_LOOP_COUNT;
                    s->st.loop_sets++;
                } else if (e->a == 90) {
                    e->kind = AE3_EV_HOOK;
                    s->st.hooks++;
                }
                break;
            case 0xC0: s->st.prog_changes++; break;
            case 0xE0: s->st.pitch_bends++; break;
            default: break;
            }
            p += nd;
        } else {
            return ae3__fail(s, "data byte %#x with no running status at %zu", st, p);
        }
    }
    if (!ended)
        return ae3__fail(s, "track has no end-of-track meta");

    s->st.ppqn = q->ppqn;
    s->st.hash_ch = hch;
    s->st.hash_tempo = htempo;

    /* Precompute end-of-track on the 48 kHz clock with the same segment arithmetic
     * playback uses, so the reported duration IS the rendered duration. */
    ae3__seq_reset_clock(q);
    for (int i = 0; i < q->nev; i++) {
        ae3_event *e = &q->ev[i];
        double at = q->seg_sample + (double)(e->tick - q->seg_tick) * q->spt;
        if (e->kind == AE3_EV_TEMPO) {
            q->seg_sample = at;
            q->seg_tick = e->tick;
            q->spt = (double)AE3_RATE * (double)e->uspqn / (1e6 * q->ppqn);
        } else if (e->kind == AE3_EV_END) {
            s->st.end_sample = (uint64_t)llround(at);
        }
    }
    ae3__seq_reset_clock(q);
    return 0;
}
