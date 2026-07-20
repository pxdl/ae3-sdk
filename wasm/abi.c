/* abi.c -- wasm-target-only flatteners for the public structs.
 *
 * The JS binding must not depend on C struct layout (padding, u64 alignment),
 * so every struct the API returns crosses the boundary as a flat scalar array
 * written field-by-field here. Order is the ae3synth.h declaration order; the
 * matching reader lives in js/ae3synth.mjs (STATS_FIELDS etc.) and the two
 * change together. Public API only -- this file never includes internal.h.
 *
 * u64 fields ride as doubles where their value is bounded far below 2^53
 * (sample positions, tick counts); the two FNV hashes are exact bit patterns,
 * so they split into lo32/hi32 pairs instead. */
#include "ae3synth.h"

/* Packed voice snapshot: flags (1 in_use, 2 active, 4 released), ch, key, env,
 * env_phase, se_prog, source_kind, waveform, source_samples, source_loop_start,
 * source_phase_q12, source_loops. Integers are exact as doubles (the largest,
 * source_phase_q12, is bounded below 2^44 for the supported bank format). */
#define AE3W_NVOICE 12
int ae3w_voice(const ae3_synth *s, int i, double out[AE3W_NVOICE])
{
    ae3_voice_state v;
    if (!ae3_synth_voice(s, i, &v))
        return 0;
    out[0] = (v.in_use ? 1 : 0) | (v.active ? 2 : 0) | (v.released ? 4 : 0);
    out[1] = v.ch;
    out[2] = v.key;
    out[3] = v.env;
    out[4] = v.env_phase;
    out[5] = v.se_prog;
    out[6] = v.source_kind;
    out[7] = v.waveform;
    out[8] = v.source_samples;
    out[9] = v.source_loop_start;
    out[10] = (double)v.source_phase_q12;
    out[11] = v.source_loops;
    return 1;
}

/* tick, kind, status, a, b, uspqn */
int ae3w_seq_event(const ae3_synth *s, int i, uint32_t out[6])
{
    ae3_seq_event e;
    if (!ae3_synth_seq_event(s, i, &e))
        return 0;
    out[0] = e.tick;
    out[1] = e.kind;
    out[2] = e.status;
    out[3] = e.a;
    out[4] = e.b;
    out[5] = e.uspqn;
    return 1;
}

/* seg_tick, seg_sample, spt, tick_offset */
void ae3w_clock(const ae3_synth *s, double out[4])
{
    ae3_clock c;
    ae3_synth_clock(s, &c);
    out[0] = (double)c.seg_tick;
    out[1] = c.seg_sample;
    out[2] = c.spt;
    out[3] = (double)c.tick_offset;
}

#define AE3W_NSTATS 39

/* ae3_stats in declaration order; hash_ch/hash_tempo each as lo32,hi32. */
void ae3w_stats(const ae3_synth *s, double out[AE3W_NSTATS])
{
    ae3_stats t;
    ae3_synth_get_stats(s, &t);
    int i = 0;
    out[i++] = t.prog_slots;
    out[i++] = t.progs_used;
    out[i++] = t.tones;
    out[i++] = t.ppqn;
    out[i++] = t.events;
    out[i++] = t.note_ons;
    out[i++] = t.note_offs;
    out[i++] = t.ccs;
    out[i++] = t.prog_changes;
    out[i++] = t.pitch_bends;
    out[i++] = t.tempo_changes;
    out[i++] = t.meta_skipped;
    out[i++] = t.end_tick;
    out[i++] = (double)t.end_sample;
    out[i++] = t.loop_starts;
    out[i++] = t.loop_ends;
    out[i++] = t.loop_sets;
    out[i++] = t.hooks;
    out[i++] = (double)(uint32_t)t.hash_ch;
    out[i++] = (double)(uint32_t)(t.hash_ch >> 32);
    out[i++] = (double)(uint32_t)t.hash_tempo;
    out[i++] = (double)(uint32_t)(t.hash_tempo >> 32);
    out[i++] = t.voices_started;
    out[i++] = t.noteons_aborted;
    out[i++] = t.notes_dropped;
    out[i++] = t.peak_voices;
    out[i++] = t.slots_freed_live;
    out[i++] = t.pitch_idx_clamped;
    out[i++] = t.pitch_step_clamped;
    out[i++] = t.bus_clipped;
    out[i++] = t.bus_peak;
    out[i++] = t.wet_clipped;
    out[i++] = t.wet_peak;
    out[i++] = t.cc_lfo;
    out[i++] = t.cc_nrpn;
    out[i++] = t.cc6_shadow;
    out[i++] = t.cc6_rev_apply;
    out[i++] = t.cc_stub;
    out[i++] = t.loops_taken;
}

/* ae3_synth_pos as a double (bounded: 2^53 samples is ~6000 years at 48 kHz). */
double ae3w_pos(const ae3_synth *s)
{
    return (double)ae3_synth_pos(s);
}

/* EXST header: channels, rate, loop, loop_start, length, vol_l[8], vol_r[8],
 * reverb[8] -- 29 u32s in ae3_exst_header declaration order. */
int ae3w_exst_parse(const void *hdr, size_t len, uint32_t out[29])
{
    ae3_exst_header h;
    if (ae3_exst_parse(hdr, len, &h) != 0)
        return 0;
    out[0] = h.channels;
    out[1] = h.rate;
    out[2] = h.loop;
    out[3] = h.loop_start;
    out[4] = h.length;
    for (int i = 0; i < AE3_EXST_MAXCH; i++) {
        out[5 + i] = h.vol_l[i];
        out[13 + i] = h.vol_r[i];
        out[21 + i] = h.reverb[i];
    }
    return 1;
}

/* The binding allocates decode states with malloc; sizeof crosses here so JS
 * never assumes the struct layout. */
int ae3w_exst_state_size(void)
{
    return (int)sizeof(ae3_exst);
}

/* addr, samples, loop_start, prog, tone, root, tune, refs */
int ae3w_bank_waveform(const ae3_synth *s, int i, int32_t out[8])
{
    ae3_waveform w;
    if (!ae3_synth_bank_waveform(s, i, &w))
        return 0;
    out[0] = (int32_t)w.addr;
    out[1] = (int32_t)w.samples;
    out[2] = w.loop_start;
    out[3] = w.prog;
    out[4] = w.tone;
    out[5] = w.root;
    out[6] = w.tune;
    out[7] = w.refs;
    return 1;
}
