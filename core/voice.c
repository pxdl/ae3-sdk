/* voice.c -- streaming PS-ADPCM decoder + SPU2 ADSR envelope.
 *
 * Decoder mirrors the offline reference implementation bit for bit (verified
 * corpus-wide by the private gates); envelope timing mirrors it exactly. Both run per output
 * sample at AE3_RATE -- the envelope is a live amplitude multiplier, which is the
 * behavior SF2 could not express and the reason the offline pipeline baked fades. */
#include <string.h>

#include "internal.h"

/* ---- PS-ADPCM ----------------------------------------------------------- */

static const int32_t COEF[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 },
};

void ae3_adpcm_init(ae3_adpcm *d, const uint8_t *bd, size_t bd_len, uint32_t start)
{
    memset(d, 0, sizeof *d);
    d->bd = bd;
    d->bd_len = bd_len;
    d->start = d->frame = start;
    d->loop_frame = -1;
    d->pos = 28;
}

static void decode_frame(ae3_adpcm *d)
{
    if ((size_t)d->frame + 16 > d->bd_len) {   /* ran off the body: treat as one-shot end */
        d->stopped = true;
        return;
    }
    const uint8_t *f = d->bd + d->frame;
    int shift = f[0] & 0x0F, filt = (f[0] >> 4) & 0x0F;
    if (shift > 12)                            /* hardware quirk clamps */
        shift = 9;
    if (filt > 4)
        filt = 0;
    d->flags = f[1];
    if (d->flags & AE3_AF_LOOPSTART)
        d->loop_frame = (int32_t)d->frame;
    int32_t c0 = COEF[filt][0], c1 = COEF[filt][1];
    int32_t h1 = d->h1, h2 = d->h2;
    for (int i = 0; i < 14; i++) {
        uint8_t b = f[2 + i];
        for (int n = 0; n < 2; n++) {
            int32_t s = (int16_t)((uint16_t)((n ? b >> 4 : b & 0x0F) << 12));
            s = (s >> shift) + ((h1 * c0 + h2 * c1) >> 6);
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            d->buf[i * 2 + n] = (int16_t)s;
            h2 = h1;
            h1 = s;
        }
    }
    d->h1 = h1;
    d->h2 = h2;
    d->pos = 0;
}

bool ae3_adpcm_next(ae3_adpcm *d, int16_t *out)
{
    if (d->stopped)
        return false;
    if (d->pos == 28) {
        if (d->primed) {                       /* advance past the frame just consumed */
            if (d->flags & AE3_AF_END) {
                if (!(d->flags & AE3_AF_REPEAT)) {
                    d->stopped = true;         /* one-shot: voice ends here */
                    return false;
                }
                /* Loop to the LOOP_START frame, or to the waveform's own start if none
                 * was flagged (SPU default; measured 2026-07-16: all 785 looped
                 * waveforms in the corpus flag LOOP_START explicitly, so the fallback
                 * never fires here -- kept because it is what the hardware does).
                 * History is CARRIED across the seam, as the hardware's reader does. */
                d->frame = d->loop_frame >= 0 ? (uint32_t)d->loop_frame : d->start;
                d->loops++;
            } else {
                d->frame += 16;
            }
        }
        decode_frame(d);
        if (d->stopped)
            return false;
        d->primed = true;
    }
    *out = d->buf[d->pos++];
    return true;
}

/* ---- voice render: gaussian resample x envelope -------------------------- */

/* One output sample at AE3_RATE: interpolate the 4-sample window at the counter's
 * 8-bit phase (psx-spx "4-Point Gaussian Interpolation", exact integer form: each
 * product SAR 15 individually), multiply by the envelope, then advance the counter
 * by the pitch step (hardware-clamped at 4x) and pull new samples as it carries.
 *
 * Output-then-advance with a zeroed window at key-on mirrors the hardware's reset
 * decode buffer; the exact sub-sample alignment vs the console (+-1 sample) is
 * unresolvable without a hardware capture and inaudible. When a one-shot's data ends
 * the envelope is forced to 0 (data_end below, the hardware's END-frame behavior),
 * silencing the window's ~3-sample tail and letting the slot poll see ENVX 0. */
/* psx-spx: an END frame without REPEAT keys the envelope into release AND forces it
 * to 0 -- the voice keeps "playing" silence and the driver's ENVX<2 poll sees 0.
 * Without this the frozen 4-sample window would keep ringing after the data ended. */
static void data_end(ae3_voice *v)
{
    v->active = false;
    v->env.phase = AE3_ENV_RELEASE;
    v->env.level = 0;
    v->env.wait = 0;
}

bool ae3_voice_tick(ae3_voice *v, const int16_t (*interp)[4], int32_t *out)
{
    if (!v->active)
        return false;
    if (!v->win_primed) {
        int16_t s0;
        if (!ae3_adpcm_next(&v->dec, &s0)) {
            data_end(v);
            return false;
        }
        v->win[3] = s0;
        v->win_primed = true;
    }
    int i = (int)(v->counter >> 4) & 0xFF;
    const int16_t *w = interp[i];   /* gaussian: same values/order as the raw table */
    int32_t acc;
    acc  = ((int32_t)w[0] * v->win[0]) >> 15;
    acc += ((int32_t)w[1] * v->win[1]) >> 15;
    acc += ((int32_t)w[2] * v->win[2]) >> 15;
    acc += ((int32_t)w[3] * v->win[3]) >> 15;

    int32_t lvl = ae3_env_tick(&v->env);
    if (ae3_env_dead(&v->env)) {
        v->active = false;
        return false;
    }

    uint32_t step = v->pitch;
    if (step > 0x3FFF)
        step = 0x4000;             /* psx-spx: Step>3FFFh -> 4000h (never hit by BGM) */
    v->counter += step;
    while (v->counter >= 0x1000) {
        v->counter -= 0x1000;
        int16_t sn;
        if (!ae3_adpcm_next(&v->dec, &sn)) {
            data_end(v);           /* this sample still emits at the pre-end level;
                                      the next call returns false via !active */
            break;
        }
        v->win[0] = v->win[1];
        v->win[1] = v->win[2];
        v->win[2] = v->win[3];
        v->win[3] = sn;
    }
    *out = (acc * lvl) >> 15;      /* ADSR multiply, s16 range (psx-spx); the
                                      mixer applies VOLL/VOLR the same way */
    return true;
}

/* ---- ADSR envelope ------------------------------------------------------ */

typedef struct {
    int shift, step;
    bool exp, rising;
    int32_t target;
} phase_params;

static phase_params get_phase(const ae3_env *e, int phase)
{
    phase_params p;
    switch (phase) {
    case AE3_ENV_ATTACK:
        p.shift = (e->a1 >> 10) & 0x1F;
        p.step = 7 - ((e->a1 >> 8) & 3);
        p.exp = (e->a1 >> 15) & 1;
        p.rising = true;
        p.target = 0x7FFF;
        break;
    case AE3_ENV_DECAY:                        /* always exponential decrease, step -8 */
        p.shift = (e->a1 >> 4) & 0x0F;
        p.step = -8;
        p.exp = true;
        p.rising = false;
        p.target = e->sus_level;
        break;
    case AE3_ENV_SUSTAIN:
        p.shift = (e->a2 >> 8) & 0x1F;
        p.exp = (e->a2 >> 15) & 1;
        if ((e->a2 >> 14) & 1) {               /* decreasing (all 3479 tones in the game) */
            p.step = -8 + ((e->a2 >> 6) & 3);
            p.rising = false;
            p.target = 0;
        } else {
            p.step = 7 - ((e->a2 >> 6) & 3);
            p.rising = true;
            p.target = 0x7FFF;
        }
        break;
    default:                                   /* AE3_ENV_RELEASE */
        p.shift = e->a2 & 0x1F;
        p.step = -8;
        p.exp = (e->a2 >> 5) & 1;
        p.rising = false;
        p.target = 0;
        break;
    }
    return p;
}

static uint32_t cycles_for(const phase_params *p, int32_t level)
{
    uint32_t c = 1u << (p->shift > 11 ? p->shift - 11 : 0);
    if (p->exp && p->rising && level > 0x6000)
        c *= 4;                                /* exponential attack slows near the top */
    return c;
}

static int32_t apply_step(const phase_params *p, int32_t level)
{
    int32_t st = p->step * (1 << (p->shift < 11 ? 11 - p->shift : 0));
    if (p->exp && !p->rising) {
        st = (st * level) >> 15;
        if (st == 0)
            st = -1;                           /* never stall completely */
    }
    level += st;
    return level < 0 ? 0 : level > 0x7FFF ? 0x7FFF : level;
}

static bool phase_done(const phase_params *p, int32_t level)
{
    return p->rising ? level >= p->target : level <= p->target;
}

/* Enter `phase`, falling through any phase whose target is already met (e.g. decay
 * with sustain level at max). Sets wait=0 (hold) when the final phase is complete. */
static void enter_phase(ae3_env *e, int phase)
{
    for (;;) {
        e->phase = phase;
        phase_params p = get_phase(e, phase);
        if (!phase_done(&p, e->level)) {
            e->wait = cycles_for(&p, e->level);
            return;
        }
        if (phase == AE3_ENV_ATTACK)
            phase = AE3_ENV_DECAY;
        else if (phase == AE3_ENV_DECAY)
            phase = AE3_ENV_SUSTAIN;
        else {                                 /* sustain/release at target: hold there */
            e->wait = 0;
            return;
        }
    }
}

void ae3_env_keyon(ae3_env *e, uint16_t a1, uint16_t a2)
{
    e->a1 = a1;
    e->a2 = a2;
    e->sus_level = ((a1 & 0x0F) + 1) * 0x800;
    if (e->sus_level > 0x7FFF)
        e->sus_level = 0x7FFF;
    e->level = 0;
    enter_phase(e, AE3_ENV_ATTACK);
}

void ae3_env_keyoff(ae3_env *e)
{
    enter_phase(e, AE3_ENV_RELEASE);           /* from the CURRENT level, exactly */
}

int32_t ae3_env_tick(ae3_env *e)
{
    if (e->wait == 0)                          /* holding (phase target reached) */
        return e->level;
    if (--e->wait == 0) {
        phase_params p = get_phase(e, e->phase);
        e->level = apply_step(&p, e->level);
        if (phase_done(&p, e->level)) {
            if (e->phase == AE3_ENV_ATTACK)
                enter_phase(e, AE3_ENV_DECAY);
            else if (e->phase == AE3_ENV_DECAY)
                enter_phase(e, AE3_ENV_SUSTAIN);
            else
                e->wait = 0;                   /* sustain fell to 0 / release done */
        } else {
            e->wait = cycles_for(&p, e->level);
        }
    }
    return e->level;
}

bool ae3_env_dead(const ae3_env *e)
{
    /* Stop RENDERING once the envelope can only sit at 0/1 (output rounds to 0).
     * Attack starts at 0, so restrict to the falling phases. SLOT reclaim is separate
     * (M5, synth.c slot_tick): the driver polls ENVX < 2 at its 60 Hz update tick
     * with a 3-tick age guard and no phase restriction (FUN_003ffc70) -- equivalent
     * outcomes on this corpus, where every attack passes level 2 within 64 samples. */
    if (e->level >= 2)
        return false;
    return e->phase == AE3_ENV_RELEASE ||
           (e->phase == AE3_ENV_SUSTAIN && ((e->a2 >> 14) & 1));
}

/* Isolated phase timing, identical accounting to bgm._phase_time: each step charges
 * the cycle count computed from the level BEFORE the step; completion charges nothing. */
uint64_t ae3__env_phase_cycles(int shift, int step, bool exp, bool rising,
                               int32_t level, int32_t target)
{
    phase_params p = { shift, step, exp, rising, target };
    uint64_t t = 0;
    while (!phase_done(&p, level)) {
        t += cycles_for(&p, level);
        int32_t next = apply_step(&p, level);
        if (next == level)                     /* stalled (level 0): cannot converge */
            break;
        level = next;
    }
    return t;
}
