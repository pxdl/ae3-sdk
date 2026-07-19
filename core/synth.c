/* synth.c -- instance lifecycle, event dispatch, and the 48 kHz render loop.
 *
 * Milestone 5: voices live in the driver's 48-slot pool (see internal.h for the
 * sg2slotctrl.c ground truth): note-ons take the round-robin first-free slot or are
 * DROPPED, and slots free only at the 60 Hz update tick once the envelope has died. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

int ae3__fail(ae3_synth *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof s->err, fmt, ap);
    va_end(ap);
    return -1;
}

/* Channel defaults per the driver's state init (FUN_003f7f58; the LFO/NRPN block
 * 0x300-0x31a is zeroed by FUN_003f8034/003f8160). */
static void reset_channels(ae3_synth *s)
{
    memset(s->chan_prog, 0, sizeof s->chan_prog);
    memset(s->chan_bend, 64, sizeof s->chan_bend);
    memset(s->chan_cc7, 127, sizeof s->chan_cc7);
    memset(s->chan_cc10, 64, sizeof s->chan_cc10);
    memset(s->chan_cc11, 127, sizeof s->chan_cc11);
    memset(s->chan_cc1, 0, sizeof s->chan_cc1);
    memset(s->chan_cc2, 0, sizeof s->chan_cc2);
    memset(s->chan_nrpn_msb, 0, sizeof s->chan_nrpn_msb);
    memset(s->chan_nrpn_lsb, 0, sizeof s->chan_nrpn_lsb);
    memset(s->chan_cc6, 0, sizeof s->chan_cc6);
}

/* The SMF walker's tick advance, transcribed double op for double op from the
 * disassembly so the corpus gates' independent mirror can require bitwise
 * equality (-ffp-contract=off is pinned in the Makefile). 0x1.47ae147ae147ap-7 is
 * DAT_0072c558's exact bits (the "0.01" the speed percent multiplies through);
 * 0x1.fffffffffffffp+0 is the play-open "2 quarters/sec" constant; the rate is the
 * boot global (60 NTSC -- gp-0x7a68 via FUN_003f63c0). Speed (+0x78) stays 100.0:
 * nothing in BGM moves it. */
#define AE3_WALK_G001  0x1.47ae147ae147ap-7
#define AE3_WALK_OPEN  0x1.fffffffffffffp+0
#define AE3_WALK_RATE  60.0
#define AE3_WALK_SPEED 100.0

static double walk_adv_open(double ppqn)
{
    return ((ppqn * AE3_WALK_OPEN) * (1.0 / AE3_WALK_RATE)) * AE3_WALK_SPEED
           * AE3_WALK_G001;
}

static double walk_adv_tempo(double ppqn, double uspqn)
{
    return (((1e6 * (1.0 / uspqn)) * ppqn) * (1.0 / AE3_WALK_RATE)) * AE3_WALK_SPEED
           * AE3_WALK_G001;
}

ae3_synth *ae3_synth_new(void)
{
    ae3_synth *s = calloc(1, sizeof(ae3_synth));
    if (s) {
        ae3_pitch_tbl_et(s->pitch_tbl);   /* until ae3_synth_load_pitch_irx */
        ae3__lfo_triangle(s->lfo_tri);    /* the driver's default LFO waveform */
        s->song_vol_l = s->song_vol_r = 127;   /* driver init (FUN_003f7f58) */
        s->nslots = AE3_NVOICES;
        s->gaussian = true;               /* the hardware kernel */
        ae3__interp_build(s);
        ae3__cue_init(&s->cue);           /* M8 cue layer; off until enabled */
        s->timing_exact = true;   /* user-settled dial (2026-07-16, memory
                                     project_ae3_bgm_findings): the console's tick
                                     bursts read as "notes are late / emulator
                                     slowdown"; exact is the default, tick stays
                                     available via ae3_synth_event_timing */
        reset_channels(s);
    }
    return s;
}

int ae3_synth_load_pitch_irx(ae3_synth *s, const void *irx, size_t irx_len)
{
    return ae3__load_pitch_irx(s, irx, irx_len);
}

int ae3_synth_load_reverb_irx(ae3_synth *s, const void *libsd, size_t libsd_len)
{
    return ae3__load_libsd(s, libsd, libsd_len);
}

void ae3_synth_reverb_depth(ae3_synth *s, int depth)
{
    ae3__rev_set_depth(&s->rev, depth);
}

void ae3_synth_free(ae3_synth *s)
{
    if (!s)
        return;
    ae3__bank_free(&s->bank);
    ae3__seq_free(&s->seq);
    ae3__rev_free(&s->rev);
    free(s);
}

const char *ae3_synth_error(const ae3_synth *s)
{
    return s->err;
}

int ae3_synth_load_bank(ae3_synth *s, const void *hd, size_t hd_len,
                        const void *bd, size_t bd_len)
{
    ae3__bank_free(&s->bank);
    s->have_bank = false;
    s->have_se = false;
    memset(s->voices, 0, sizeof s->voices);   /* voices reference the old bank body */
    s->st.prog_slots = s->st.progs_used = s->st.tones = 0;
    if (ae3__parse_bank(s, hd, hd_len, bd, bd_len)) {
        ae3__bank_free(&s->bank);
        return -1;
    }
    s->have_bank = true;
    return 0;
}
int ae3__synth_load_se(ae3_synth *s, int bank, int request)
{
    ae3_stats bank_st = s->st;
    ae3__seq_free(&s->seq);
    s->have_seq = false;
    s->have_se = false;
    memset(&s->st, 0, sizeof s->st);
    s->st.prog_slots = bank_st.prog_slots;
    s->st.progs_used = bank_st.progs_used;
    s->st.tones = bank_st.tones;
    if (ae3__se_load(s, bank, request))
        return -1;
    s->have_se = true;
    s->pos = 0;
    s->next_tick = 0;
    s->cursor = 0;
    reset_channels(s);
    memset(s->voices, 0, sizeof s->voices);
    ae3__rev_reset(&s->rev);
    s->wet_ever = false;
    s->rev_tail_left = AE3_REV_TAIL_SAMPLES;
    return 0;
}


int ae3_synth_load_seq(ae3_synth *s, const void *mid, size_t mid_len)
{
    ae3_stats bank_st = s->st;
    ae3__seq_free(&s->seq);
    s->have_se = false;
    s->have_seq = false;
    memset(&s->st, 0, sizeof s->st);
    s->st.prog_slots = bank_st.prog_slots;
    s->st.progs_used = bank_st.progs_used;
    s->st.tones = bank_st.tones;
    if (ae3__parse_seq(s, mid, mid_len)) {
        ae3__seq_free(&s->seq);
        return -1;
    }
    s->have_seq = true;
    s->pos = 0;
    s->next_tick = 0;
    s->cursor = 0;     /* driver boot state; the console's cursor free-runs across
                          songs, so per-song determinism is a (documented) choice */
    reset_channels(s);
    memset(s->voices, 0, sizeof s->voices);
    ae3__rev_reset(&s->rev);   /* the network state, not the preset */
    s->wet_ever = false;
    s->rev_tail_left = AE3_REV_TAIL_SAMPLES;
    /* walker state (play-open init, FUN_00400e80): loop count from the config knob
     * (console BGM: 0x7f), loop return = track start, tick clock rewound */
    s->loop_live = s->loop_cfg;
    s->loop_from = 0;
    s->loop_from_tick = 0;
    s->tick_offset = 0;
    s->walk_cur = 0.0;
    s->walk_adv = walk_adv_open((double)s->seq.ppqn);
    s->song_word18 = 0;
    return 0;
}

void ae3_synth_set_loop(ae3_synth *s, int count)
{
    s->loop_cfg = count;
    s->loop_live = count;
}

void ae3_synth_event_timing(ae3_synth *s, bool exact)
{
    s->timing_exact = exact;
}

void ae3_synth_gaussian(ae3_synth *s, bool on)
{
    s->gaussian = on;
    ae3__interp_build(s);
}

/* ---- voices -------------------------------------------------------------- */

static void trace(ae3_synth *s, char ev, int slot, int ch, int key)
{
    if (s->slot_trace)
        s->slot_trace(s->slot_trace_user, ev, s->pos, slot, ch, key);
}

/* FUN_004009c8: first free slot scanning round-robin from the persistent cursor;
 * the cursor advances past the grant. NULL = all 48 busy (caller drops the note). */
static ae3_voice *alloc_voice(ae3_synth *s)
{
    for (int i = 0; i < s->nslots; i++) {
        int idx = s->cursor + i;
        if (idx >= s->nslots)
            idx -= s->nslots;
        if (!s->voices[idx].in_use) {
            s->cursor = idx + 1 >= s->nslots ? 0 : idx + 1;
            return &s->voices[idx];
        }
    }
    return NULL;
}

/* FUN_003ffc70 (update tick, callback #3): age every bound slot; from the tick where
 * age > 2, free it when the envelope reads below 2 -- the driver's ENVX<2 poll. The
 * envelope, not `active`, is the condition: a data-ended one-shot forced its envelope
 * to 0 (voice.c), which is exactly what the hardware ENVX would report. M9: the same
 * per-voice pass runs the LFO for every armed voice that survived the poll (the
 * flush's 0x400 block -- the free path jumps past it), overwriting the pitch
 * register each tick. */
static void slot_tick(ae3_synth *s)
{
    ae3__se_tick(s);   /* SE voice ramps share this callback, before LFO/pitch flush */
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->in_use)
            continue;
        v->age++;
        if (v->age > 2 && v->env.level < 2) {
            v->in_use = false;
            trace(s, 'F', i, v->ch, v->key);
            if (v->active) {
                s->st.slots_freed_live++;
                trace(s, 'Z', i, v->ch, v->key);
            }
            continue;
        }
        if (v->lfo_on)
            ae3__lfo_tick(s, v);
    }
}

static void set_voice_pitch(ae3_synth *s, ae3_voice *v)
{
    v->pitch = ae3__pitch_reg(s, v->key, v->tone->root, v->tone->tune,
                              s->chan_bend[v->ch], v->tone->bend);
    if (v->pitch > 0x3FFF)
        s->st.pitch_step_clamped++;
}

void ae3__voice_refresh(ae3_synth *s, ae3_voice *v)
{
    ae3__voice_regs(s->song_vol_l, s->song_vol_r, v->vvol, v->vel,
                    v->cpan, v->tpan, &v->voll, &v->volr);
}

static bool start_voice(ae3_synth *s, uint8_t ch, uint8_t key, uint8_t vel,
                        const ae3_prog *p, const ae3_tone *t)
{
    if (s->bank.se && t->cut_group) {
        for (int i = 0; i < AE3_NVOICES; i++) {
            ae3_voice *old = &s->voices[i];
            if (old->active && old->se_voice && old->cut_group == t->cut_group) {
                /* FUN_003ff7f8 overwrites ADSR1/2 with (0,8), then keys off. */
                old->env.a1 = 0;
                old->env.a2 = 8;
                old->released = true;
                ae3_env_keyoff(&old->env);
            }
        }
    }
    ae3_voice *v = alloc_voice(s);
    if (!v) {
        s->st.notes_dropped++;   /* FUN_00400b58 == -1: the driver aborts the whole
                                    note-on, remaining stack layers included */
        trace(s, 'D', -1, ch, key);
        return false;
    }
    memset(v, 0, sizeof *v);     /* a regrant cuts any unowned voice still ringing */
    v->in_use = true;            /* freed only by slot_tick's ENVX poll */
    v->active = true;
    v->ch = ch;
    v->key = key;
    v->vel = vel;                /* raw; the shipped velocity chunk is identity */
    v->tone = t;
    v->se_voice = s->bank.se;
    v->se_prog = ch;
    v->cut_group = t->cut_group;
    ae3_adpcm_init(&v->dec, s->bank.bd, s->bank.bd_len, (uint32_t)t->addr * 8);
    /* SE note-on FUN_003f8690's flag-0x02 branch sends command 5 mode 0 with
     * (slot, tone byte 2). sg2iopm1 ev_set_core_attr maps slot/24 to
     * SD_CORE_NOISE_CLK; libsd sceSdSetCoreAttr masks the value to six bits. */
    if (s->bank.se && (t->flags & AE3_TF_NOISE))
        s->noise_clk[(int)(v - s->voices) / 24] = t->root & 0x3f;
    ae3_env_keyon(&v->env, t->adsr1, t->adsr2);
    set_voice_pitch(s, v);
    /* LFO waveform: indexed Jam entry when the bank supplies one, otherwise the
     * driver's computed default triangle. Sparse-table holes are kept NULL; the
     * shipped hard-armed SE tones all resolve to live entries. */
    if (!s->bank.lfo) {
        v->lfo_tbl = s->lfo_tri;
    } else if (t->lfo < s->bank.nlfo) {
        const uint8_t *o = s->bank.lfo + 2 + (size_t)t->lfo * 2;
        uint16_t off = (uint16_t)(o[0] | o[1] << 8);
        v->lfo_tbl = off == 0xffff ? NULL : s->bank.lfo + off;
    } else {
        v->lfo_tbl = NULL;
    }
    if ((t->flags & AE3_TF_LFO_ON) || s->chan_cc2[ch] + s->chan_cc1[ch] != 0) {
        ae3__lfo_set_rate(v, s->chan_cc2[ch]);
        ae3__lfo_set_depth(v, s->chan_cc1[ch]);
    }
    if (s->bank.se && (t->flags & AE3_TF_LFO_ON)) {
        ae3__lfo_set_rate(v, 10);
        ae3__lfo_set_depth(v, 127);
    }
    v->cpan = (uint8_t)ae3__cpan_clamp(s->chan_cc10[ch]);
    v->tpan = (uint8_t)ae3__tpan_clamp(t->pan);
    v->vvol = ae3__vol_product(s->chan_cc7[ch], s->chan_cc11[ch], p->vol, t->vol);
    ae3__voice_refresh(s, v);
    if (s->rev.on && (t->flags & AE3_TF_REVERB))
        s->wet_ever = true;    /* gates the post-song reverb tail */
    s->st.voices_started++;
    trace(s, 'G', (int)(v - s->voices), ch, key);
    uint32_t live = 0;
    for (int i = 0; i < AE3_NVOICES; i++)
        live += s->voices[i].in_use;   /* allocation pressure, not audibility */
    if (live > s->st.peak_voices)
        s->st.peak_voices = live;
    return true;
}

/* ---- event dispatch ----------------------------------------------------- */

/* Note-on per the decompiled FUN_003facb8. Drums (h[0]==0xFF) address ONE tone by
 * key - key0; melodic programs scan tones in table order, skip out-of-window tones,
 * and stop after the first match unless header bit 7 (stack) is set. A reached tone
 * whose RAW byte 13 is 0xFF aborts the whole note-on (silent sentinel). */
void ae3__note_on(ae3_synth *s, uint8_t ch, uint8_t key, uint8_t vel)
{
    if (!s->have_bank || s->chan_prog[ch] >= s->bank.nprogs)
        return;
    const ae3_prog *p = &s->bank.progs[s->chan_prog[ch]];
    if (!p->present)
        return;

    if (p->drum) {
        int idx = key - p->key0;
        if (idx < 0 || idx >= p->ntones)   /* driver has no upper guard; data never hits it */
            return;
        const ae3_tone *t = &p->tones[idx];
        if (t->bend_raw == 0xFF || t->addr == AE3_NO_SAMPLE) {
            s->st.noteons_aborted++;
            return;
        }
        start_voice(s, ch, key, vel, p, t);
        return;
    }
    for (int i = 0; i < p->ntones; i++) {
        const ae3_tone *t = &p->tones[i];
        if (key < t->lo || key > t->hi)
            continue;
        if (t->bend_raw == 0xFF) {
            s->st.noteons_aborted++;
            return;                        /* aborts the WHOLE note-on, layers included */
        }
        if (!start_voice(s, ch, key, vel, p, t))
            return;                        /* no free voice: driver bails likewise */
        if (!p->stack)
            return;
    }
}

void ae3__note_off(ae3_synth *s, uint8_t ch, uint8_t key)
{
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (v->active && !v->released && v->ch == ch && v->key == key) {
            v->released = true;
            ae3_env_keyoff(&v->env);       /* release from the CURRENT level */
        }
    }
}

/* Mirror of FUN_003fab98, which the CC7/CC10/CC11 handlers all tail into: every
 * live voice on the channel (released ones included -- they are still allocated to
 * it) gets its channel pan re-clamped and its volume product recomputed. The prog
 * volume is read from the channel's CURRENT program header, the tone volume from
 * the tone stashed at note-on -- asymmetric, but that is what the driver does.
 * Velocity and tone pan are untouched. The driver defers the register write to its
 * update tick; we apply it at the event sample (tighter, same values). */
static void cc_refresh(ae3_synth *s, uint8_t ch)
{
    const ae3_prog *p = NULL;
    if (s->have_bank && s->chan_prog[ch] < s->bank.nprogs &&
        s->bank.progs[s->chan_prog[ch]].present)
        p = &s->bank.progs[s->chan_prog[ch]];
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->active || v->ch != ch)
            continue;
        v->cpan = (uint8_t)ae3__cpan_clamp(s->chan_cc10[ch]);
        if (p)   /* no valid current program: keep the note-on product (the driver
                    would chase a stale header pointer; unreachable in the corpus) */
            v->vvol = ae3__vol_product(s->chan_cc7[ch], s->chan_cc11[ch],
                                       p->vol, v->tone->vol);
        ae3__voice_refresh(s, v);
    }
}

void ae3_synth_song_volume(ae3_synth *s, int l, int r)
{
    s->song_vol_l = l;
    s->song_vol_r = r;
    for (int i = 0; i < AE3_NVOICES; i++) {  /* like the driver's fade tick */
        ae3_voice *v = &s->voices[i];
        if (v->active)
            ae3__voice_refresh(s, v);
    }
}

/* The driver's CC dispatch, mirrored from the 128-entry table at EE 0x0069dea8
 * (every handler decompiled from the game and pinned).
 * Corpus ledger: 7/10/11 audible (M4); 1/2 = LFO depth/rate (M9: audible on any
 * channel with both nonzero -- s_20_park ch4, b_4_fast_brass_2/b_4_slow_brass ch2,
 * s_3_update ch5; tone flag 0x20 is inert, decomp/functions_bgm/lfo/NOTES.md);
 * 6/98/99 = the NRPN machine (inert on this corpus: the walker eats CC99 20/30, so
 * the NRPN pair stays (0,0) and every CC6 lands in the never-applied reverb-type
 * shadow); 8/9/66 = the bare jr-ra stub. CC99 values 20/30, CC102 and CC90 never
 * get here -- the SEQUENCER consumes them (seq.c reclassifies; the loop events). */
static void control(ae3_synth *s, uint8_t ch, uint8_t cc, uint8_t val)
{
    switch (cc) {
    case 7:  s->chan_cc7[ch] = val; break;
    case 11: s->chan_cc11[ch] = val; break;
    case 10: s->chan_cc10[ch] = val; break;   /* raw; clamped 1..127 at use */
    case 1:                    /* 0x3f9e40: depth store chan+0x304, then walk every
                                  BOUND voice (FUN_003fe108 collects on the slot
                                  flag, so released-but-ringing ones included) */
        s->chan_cc1[ch] = val;
        s->st.cc_lfo++;
        for (int i = 0; i < AE3_NVOICES; i++)
            if (s->voices[i].in_use && s->voices[i].ch == ch)
                ae3__lfo_set_depth(&s->voices[i], val);
        return;
    case 2:                    /* 0x3f9ed8: rate store chan+0x300 + the same walk */
        s->chan_cc2[ch] = val;
        s->st.cc_lfo++;
        for (int i = 0; i < AE3_NVOICES; i++)
            if (s->voices[i].in_use && s->voices[i].ch == ch)
                ae3__lfo_set_rate(&s->voices[i], val);
        return;
    case 6: {                  /* 0x3fa100: NRPN data entry, dispatch on (msb,lsb) */
        s->chan_cc6[ch] = val;
        uint8_t msb = s->chan_nrpn_msb[ch], lsb = s->chan_nrpn_lsb[ch];
        if (msb <= 1) {
            if (lsb <= 3) {            /* shadow reverb config for core msb: type/
                                          depth/word0xc/word0x10 -- STORED ONLY, the
                                          hardware keeps STUDIO_C until an apply */
                s->rev_shadow[msb][lsb] = val;
                s->st.cc6_shadow++;
            } else if (lsb == 0x7f) {  /* FUN_003f6768: the APPLY -- would resend
                                          the IOP reverb config; corpus never */
                s->st.cc6_rev_apply++;
            }
        } else if (msb == 2 && lsb == 0) {
            s->song_word18 = val;      /* FUN_003f9158 mode 0x800: song-state+0x18;
                                          no reader found in the driver -- pinned
                                          unknown, counted so a data set screams */
            s->st.cc6_rev_apply++;
        }
        return;
    }
    case 98:                   /* 0x3fa6f0 / 0x3fa738: the NRPN pair stores */
        s->chan_nrpn_lsb[ch] = val;
        s->st.cc_nrpn++;
        return;
    case 99:
        s->chan_nrpn_msb[ch] = val;
        s->st.cc_nrpn++;
        return;
    default:                   /* the jr-ra stub (8/9/66 here), plus assigned
                                  handlers BGM never sends (0/3/4/5/32/38/64/65/
                                  70-73/84/91-93/120/123) -- unmodelled, counted */
        s->st.cc_stub++;
        return;
    }
    cc_refresh(s, ch);
}

/* The wheel updates every ACTIVE voice on the channel (the driver's CC paths walk the
 * live voice list; released voices are still on the channel and keep bending). The
 * LSB is ignored -- the IOP command carries only the MSB. */
static void pitch_bend(ae3_synth *s, uint8_t ch, uint8_t lsb, uint8_t msb)
{
    (void)lsb;
    s->chan_bend[ch] = msb;
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (v->active && v->ch == ch && !v->lfo_on)
            set_voice_pitch(s, v);   /* while the LFO is armed the flush overwrites
                                        the wheel MSB every tick, so the wheel is
                                        ignored (corpus never mixes the two) */
    }
}

static void dispatch(ae3_synth *s, const ae3_event *e)
{
    ae3_seq *q = &s->seq;
    uint64_t eff = e->tick + s->tick_offset;
    switch (e->kind) {
    case AE3_EV_TEMPO:
        q->seg_sample = q->seg_sample + (double)(eff - q->seg_tick) * q->spt;
        q->seg_tick = eff;
        q->spt = (double)AE3_RATE * (double)e->uspqn / (1e6 * q->ppqn);
        s->walk_adv = walk_adv_tempo((double)q->ppqn, (double)e->uspqn);
        break;
    case AE3_EV_END:
        q->ended = true;
        break;
    case AE3_EV_LOOP_START:      /* walker: save resume point + nothing audible */
        s->loop_from = (int)(e - q->ev) + 1;
        s->loop_from_tick = e->tick;
        break;
    case AE3_EV_LOOP_COUNT:      /* CC102 writes the live counter (walker +0x70) */
        if (s->loop_cfg)
            s->loop_live = e->b;
        break;
    case AE3_EV_LOOP_END:
        /* walker rule: 0x7f never decrements; jump while the count stays positive.
         * Deltas are preserved across the seam (the delta after the start marker
         * applies from the end marker's tick), which the effective-tick offset
         * reproduces exactly. */
        if (s->loop_cfg) {
            if (s->loop_live != AE3_LOOP_FOREVER)
                s->loop_live--;
            if (s->loop_live > 0) {
                s->tick_offset += e->tick - s->loop_from_tick;
                q->next = s->loop_from;
                s->st.loops_taken++;
            }
        }
        break;
    case AE3_EV_HOOK:            /* CC90: the walker's game-callback tap; no synth
                                    effect (parse counted it in st.hooks) */
        break;
    case AE3_EV_CH: {
        uint8_t ch = e->status & 0x0F;
        switch (e->status & 0xF0) {
        case 0x90: e->b ? ae3__note_on(s, ch, e->a, e->b)
                        : ae3__note_off(s, ch, e->a); break;
        case 0x80: ae3__note_off(s, ch, e->a); break;
        case 0xB0: control(s, ch, e->a, e->b); break;
        case 0xC0: s->chan_prog[ch] = e->a; break;
        case 0xE0: pitch_bend(s, ch, e->a, e->b); break;
        default: break;   /* 0xA0/0xD0 pressure: unused by the driver */
        }
        break;
    }
    }
}

void ae3_synth_note_on(ae3_synth *s, int ch, int key, int vel)
{
    if (vel > 0)
        ae3__note_on(s, (uint8_t)ch, (uint8_t)key, (uint8_t)vel);
    else
        ae3__note_off(s, (uint8_t)ch, (uint8_t)key);
}

void ae3_synth_note_off(ae3_synth *s, int ch, int key)
{
    ae3__note_off(s, (uint8_t)ch, (uint8_t)key);
}

void ae3_synth_program(ae3_synth *s, int ch, int prog)
{
    s->chan_prog[ch & 0x0F] = (uint8_t)prog;
}

static bool any_voice_active(const ae3_synth *s)
{
    for (int i = 0; i < AE3_NVOICES; i++)
        if (s->voices[i].active)
            return true;
    return false;
}

bool ae3_synth_done(const ae3_synth *s)
{
    if (s->have_seq && !s->seq.ended)
        return false;
    if (s->have_se && !s->se.ended)
        return false;
    if (any_voice_active(s))
        return false;
    /* keep rendering while the reverb tail decays (only if the song ever fed it) */
    return !(s->rev.on && s->wet_ever && s->rev_tail_left);
}

/* Saturate one bus sample to s16, tracking peak/clip stats (psx-spx / PCSX2
 * clamp_mix: the SPU2's buses are 16-bit and dense passages CAN clip). */
static int32_t bus_sat(int32_t m, uint32_t *peak, uint32_t *clipped)
{
    uint32_t mag = (uint32_t)(m < 0 ? -m : m);
    if (mag > *peak)
        *peak = mag;
    if (m < -32768 || m > 32767) {
        (*clipped)++;
        m = m < 0 ? -32768 : 32767;
    }
    return m;
}

/* One sample of a core's shared SPU2 noise source. The six-bit clock controls
 * both the integer period and its fractional correction; the feedback bit is
 * XNOR parity over output bits 10,11,12,15. Return-before-step matches the SPU2
 * mixer: voices read NoiseOut, then the core advances it for the next sample. */
static int16_t noise_tick(ae3_synth *s, int core)
{
    static const uint8_t frac[4] = { 0, 84, 140, 180 };
    uint32_t clk = s->noise_clk[core];
    uint32_t level = (0x8000u >> (clk >> 2)) << 16;
    uint32_t cnt = s->noise_cnt[core] + 0x10000u + frac[clk & 3];
    int16_t out = (int16_t)s->noise_out[core];
    if ((cnt & 0xffffu) >= 210u)
        cnt += 0x10000u - frac[clk & 3];
    while (cnt >= level) {
        cnt -= level;
        uint32_t x = s->noise_out[core];
        uint32_t bit = 1u ^ ((x >> 10) & 1u) ^ ((x >> 11) & 1u)
                          ^ ((x >> 12) & 1u) ^ ((x >> 15) & 1u);
        s->noise_out[core] = (x << 1) | bit;
    }
    s->noise_cnt[core] = cnt;
    return out;
}

/* Mix all live voices into n (<= AE3_MIX_CHUNK) frames, integer all the way in the
 * hardware's order and widths: gauss -> ADSR (>>15, inside voice_tick) -> voice
 * volume ((x * reg*2) >> 15; the register is the SPU2's /2 fixed-volume format) ->
 * bus sum saturated to s16 -> master MVOL 0x3FFF (boot pin, FUN_003f6948) -> float.
 * Voice-major iteration is decoder-cache-friendly; addition commutes, so the bus
 * total is identical to sample-major.
 *
 * With a reverb preset loaded, reverb-flagged voices ALSO sum into a wet bus (the
 * send: they stay in the dry mix), which is saturated likewise, run through the
 * half-rate network (reverb.c), and returned as dry + EVOL*reverb -- the exact
 * combine the offline oracle performs, in doubles, then MVOL. Without a preset the
 * integer-only path below is byte-identical to pre-M6. */
static void mix(ae3_synth *s, float *out, int n)
{
    int32_t acc[2 * AE3_MIX_CHUNK];
    int32_t wac[2 * AE3_MIX_CHUNK];
    int16_t noise[2][AE3_MIX_CHUNK];
    bool rev = s->rev.on;
    memset(acc, 0, (size_t)n * 2 * sizeof(int32_t));
    if (rev)
        memset(wac, 0, (size_t)n * 2 * sizeof(int32_t));
    for (int j = 0; j < n; j++) {
        noise[0][j] = noise_tick(s, 0);
        noise[1][j] = noise_tick(s, 1);
    }
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->active)
            continue;
        int32_t gl = v->voll << 1, gr = v->volr << 1;
        bool wet = rev && (v->tone->flags & AE3_TF_REVERB);
        bool is_noise = v->se_voice && (v->tone->flags & AE3_TF_NOISE);
        const int16_t *ns = noise[(int)(v - s->voices) / 24];
        for (int j = 0; j < n; j++) {
            int32_t x;
            if (!ae3_voice_tick(v, (const int16_t (*)[4])s->interp,
                                is_noise, ns[j], &x))
                break;
            int32_t pl = (x * gl) >> 15, pr = (x * gr) >> 15;
            acc[2 * j] += pl;
            acc[2 * j + 1] += pr;
            if (wet) {
                wac[2 * j] += pl;
                wac[2 * j + 1] += pr;
            }
        }
    }
    if (!rev) {
        for (int j = 0; j < 2 * n; j++) {
            int32_t m = bus_sat(acc[j], &s->st.bus_peak, &s->st.bus_clipped);
            m = (m * 0x7FFE) >> 15;        /* MVOL 0x3FFF in the same /2 format */
            out[j] = (float)m * (1.0f / 32768.0f);
        }
        return;
    }
    int16_t d16[2 * AE3_MIX_CHUNK], w16[2 * AE3_MIX_CHUNK], r16[2 * AE3_MIX_CHUNK];
    for (int j = 0; j < n; j++) {
        for (int c = 0; c < 2; c++) {
            d16[2 * j + c] = (int16_t)bus_sat(acc[2 * j + c], &s->st.bus_peak,
                                              &s->st.bus_clipped);
            w16[2 * j + c] = (int16_t)bus_sat(wac[2 * j + c], &s->st.wet_peak,
                                              &s->st.wet_clipped);
        }
        double rl, rr;
        ae3__rev_sample(&s->rev, w16[2 * j], w16[2 * j + 1], &rl, &rr);
        for (int c = 0; c < 2; c++) {
            /* the oracle's combine, operand for operand (the corpus gates diff it bitwise) */
            double l = (double)d16[2 * j + c] / 32768.0
                     + s->rev.evol * (c ? rr : rl);
            if (l > 1.0) l = 1.0;
            if (l < -1.0) l = -1.0;
            r16[2 * j + c] = (int16_t)(l * 32767.0);   /* the oracle's quantization */
            /* MVOL 0x3FFF (/2 format) on the summed output, like the integer path */
            out[2 * j + c] = (float)(l * (32766.0 / 32768.0));
        }
    }
    if (s->mix_tap)
        s->mix_tap(s->mix_tap_user, d16, w16, r16, n);
}

/* Dispatch the next event, tracing (pos, index) for the --eventdump harness. */
static void fire(ae3_synth *s)
{
    ae3_seq *q = &s->seq;
    int idx = q->next;
    const ae3_event *e = &q->ev[q->next++];
    if (s->ev_trace)
        s->ev_trace(s->ev_trace_user, s->pos, idx, e);
    dispatch(s, e);   /* a loop-end jump rewinds q->next; the callers' while loops
                         then re-evaluate the jumped-to event's due time */
}

int ae3_synth_render(ae3_synth *s, float *out, int nframes)
{
    if (!s->have_seq && !s->have_bank && !s->have_se)
        return ae3__fail(s, "nothing loaded");
    ae3_seq *q = &s->seq;
    int done = 0;
    while (done < nframes && !ae3_synth_done(s)) {
        if (s->have_se)
            ae3__se_fire_due(s);
        if (s->have_seq) {
            if (s->timing_exact) {
                /* fire everything due at or before the current sample */
                while (q->next < q->nev && !q->ended) {
                    double at = q->seg_sample
                              + (double)(q->ev[q->next].tick + s->tick_offset
                                         - q->seg_tick) * q->spt;
                    if (at > (double)s->pos)
                        break;
                    fire(s);
                }
            } else if (s->pos == s->next_tick) {
                /* the console's dispatch: the SMF walker (callback #0) fires every
                 * event due this 60 Hz tick in one burst (cc/NOTES.md §4); the mix
                 * cap below lands pos exactly on each boundary, so this runs once
                 * per tick. walk_cur/walk_adv are the walker's own doubles; the
                 * advance uses the adv as left by this tick's tempo events. */
                while (q->next < q->nev && !q->ended &&
                       (double)(q->ev[q->next].tick + s->tick_offset) <= s->walk_cur)
                    fire(s);
                s->walk_cur += s->walk_adv;
            }
        }
        /* driver update tick -- AFTER the events at the same sample: per tick the
         * sequencer is callbacks #0/#2 and the slot control #3, so a note-on landing
         * on a tick competes for slots before that tick frees any. */
        while (s->pos >= s->next_tick) {
            ae3__cue_tick(s);   /* the game-frame side of the tick: duck lerp +
                                   songvol recompute (no-op unless enabled) */
            slot_tick(s);
            s->next_tick += AE3_TICK_SAMPLES;
        }
        if (ae3_synth_done(s))
            break;
        /* render up to the next event (exact mode) or update tick (or chunk end) */
        int n = nframes - done;
        if (n > AE3_MIX_CHUNK)
            n = AE3_MIX_CHUNK;
        if (s->timing_exact && s->have_seq && !q->ended && q->next < q->nev) {
            double at = q->seg_sample
                      + (double)(q->ev[q->next].tick + s->tick_offset
                                 - q->seg_tick) * q->spt;
            double gap = at - (double)s->pos;
            if (gap < 1.0)
                gap = 1.0;
            if ((double)n > gap)
                n = (int)ceil(gap);
        }
        if (s->have_se && !s->se.ended) {
            uint64_t at = ae3__se_next_sample(s);
            uint64_t gap = at > s->pos ? at - s->pos : 1;
            if ((uint64_t)n > gap)
                n = (int)gap;
        }
        if ((uint64_t)n > s->next_tick - s->pos)
            n = (int)(s->next_tick - s->pos);
        mix(s, out + 2 * done, n);
        s->pos += (uint64_t)n;
        done += n;
        /* reverb tail: once the song and every voice have ended, run the network
         * on silence until it has had AE3_REV_TAIL_SAMPLES to decay (block-
         * granular, so the exact tail length varies by caller buffer size) */
        if (s->rev.on && s->wet_ever && !any_voice_active(s) &&
            (!s->have_seq || s->seq.ended) && (!s->have_se || s->se.ended))
            s->rev_tail_left -= (uint64_t)n < s->rev_tail_left ? (uint64_t)n
                                                               : s->rev_tail_left;
    }
    return done;
}

/* Harness helper: after the song has ended, keep running update ticks until every
 * slot is free. On the console the tick thread free-runs forever, so these frees
 * happen at exactly these positions; the render loop just has no reason to reach
 * them once no voice is audible. */
void ae3__slot_flush(ae3_synth *s)
{
    for (int guard = 0; guard < 16; guard++) {
        bool busy = false;
        for (int i = 0; i < AE3_NVOICES; i++)
            busy = busy || s->voices[i].in_use;
        if (!busy)
            return;
        s->pos = s->next_tick;
        slot_tick(s);
        s->next_tick += AE3_TICK_SAMPLES;
    }
}

void ae3_synth_get_stats(const ae3_synth *s, ae3_stats *out)
{
    *out = s->st;
}

/* ---- playback introspection (ae3synth.h) -------------------------------- */

uint64_t ae3_synth_pos(const ae3_synth *s)
{
    return s->pos;
}

bool ae3_synth_voice(const ae3_synth *s, int i, ae3_voice_state *out)
{
    if (i < 0 || i >= AE3_NVOICES)
        return false;
    const ae3_voice *v = &s->voices[i];
    *out = (ae3_voice_state){ v->in_use, v->active, v->released,
                              v->ch, v->key, v->env.level };
    return true;
}

int ae3_synth_seq_events(const ae3_synth *s)
{
    return s->have_seq ? s->seq.nev : 0;
}

bool ae3_synth_seq_event(const ae3_synth *s, int i, ae3_seq_event *out)
{
    if (!s->have_seq || i < 0 || i >= s->seq.nev)
        return false;
    const ae3_event *e = &s->seq.ev[i];
    *out = (ae3_seq_event){ e->tick, e->kind, e->status, e->a, e->b, e->uspqn };
    return true;
}

uint16_t ae3_synth_seq_ppqn(const ae3_synth *s)
{
    return s->have_seq ? s->seq.ppqn : 0;
}

void ae3_synth_clock(const ae3_synth *s, ae3_clock *out)
{
    *out = (ae3_clock){ s->seq.seg_tick, s->seq.seg_sample, s->seq.spt,
                        s->tick_offset };
}
