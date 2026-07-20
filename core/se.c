/* se.c -- embedded SE sequence walker and its voice-local controls.
 * Ground truth: docs/formats/SE.md section 6 and the preserved EE functions in
 * decomp/functions_bgm/se/. The bytecode is interpreted from the caller's bank;
 * no game data or sequence bytes are compiled into this library. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

#define SE_TICK_SAMPLES (AE3_RATE / 480u)  /* bytecode clock: 480 ticks/s */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static bool vlq(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        if (p == end)
            return false;
        uint8_t b = *p++;
        v = v * 128u + (b & 0x7fu);
        if (!(b & 0x80u)) {
            *pp = p;
            *out = v;
            return true;
        }
    }
    return false;
}

/* Validate one selected stream and locate its actual FF 2F 00 end. This keeps
 * malformed caller data out of the real-time render path. */
static const uint8_t *stream_end(const uint8_t *start, const uint8_t *limit)
{
    const uint8_t *p = start;
    uint8_t running = 0;
    while (p < limit) {
        uint8_t status = *p;
        if (status & 0x80u) {
            p++;
            if (status == 0xff) {
                if (limit - p >= 2 && p[0] == 0x2f && p[1] == 0)
                    return p + 2;
                return NULL;
            }
            running = status;
        } else {
            status = running;
        }
        if (status == 0xa0) {
            if (limit - p < 3)
                return NULL;
            p += 3;
        } else if (status == 0xb0) {
            if (p == limit)
                return NULL;
            uint8_t cmd = *p;
            size_t n = cmd == 7 || cmd == 10 || cmd == 0x41 ? 5 : 4;
            if ((cmd != 1 && cmd != 2 && cmd != 7 && cmd != 10 &&
                 cmd != 0x41 && cmd != 0x60) || (size_t)(limit - p) < n)
                return NULL;
            p += n;
        } else {
            return NULL;
        }
        uint32_t delay;
        if (!vlq(&p, limit, &delay))
            return NULL;
    }
    return NULL;
}

static bool table_entry(const uint8_t *base, size_t len, const uint8_t *table,
                        int index, const uint8_t **out)
{
    if (table < base || (size_t)(table - base) + 4 > len || index < 0)
        return false;
    int count = (int16_t)rd16(table) + 1;
    if (count < 1 || index >= count ||
        (size_t)(table - base) + 2 + (size_t)count * 2 > len)
        return false;
    uint16_t off = rd16(table + 2 + (size_t)index * 2);
    if (off == 0xffff || off >= len)
        return false;
    *out = base + off;  /* both table levels are relative to the seseq base */
    return true;
}

int ae3_synth_se_banks(const ae3_synth *s)
{
    if (!s || !s->have_bank || !s->bank.se || !s->bank.seseq)
        return 0;
    return (int16_t)rd16(s->bank.seseq) + 1;
}

int ae3_synth_se_requests(const ae3_synth *s, int bank)
{
    if (!s || !s->have_bank || !s->bank.se || !s->bank.seseq)
        return 0;
    const uint8_t *inner;
    if (!table_entry(s->bank.seseq, s->bank.seseq_len,
                     s->bank.seseq, bank, &inner))
        return 0;
    if ((size_t)(inner - s->bank.seseq) + 2 > s->bank.seseq_len)
        return 0;
    int count = (int16_t)rd16(inner) + 1;
    return count > 0 ? count : 0;
}

int ae3__se_load(ae3_synth *s, int bank, int request)
{
    ae3_bank *bk = &s->bank;
    if (!s->have_bank || !bk->se || !bk->seseq)
        return ae3__fail(s, "loaded bank has no embedded SE sequence");
    const uint8_t *inner, *start;
    if (!table_entry(bk->seseq, bk->seseq_len, bk->seseq, bank, &inner))
        return ae3__fail(s, "SE bank index %d is absent or out of range", bank);
    if (!table_entry(bk->seseq, bk->seseq_len, inner, request, &start))
        return ae3__fail(s, "SE request %d is absent or out of range in bank %d",
                         request, bank);
    const uint8_t *end = stream_end(start, bk->seseq + bk->seseq_len);
    if (!end)
        return ae3__fail(s, "SE bank %d request %d has malformed bytecode", bank, request);

    memset(&s->se, 0, sizeof s->se);
    s->se.start = s->se.p = start;
    s->se.end = end;
    s->se.active = true;
    return 0;
}

static void lfo_control(ae3_synth *s, uint8_t cmd, const uint8_t *a)
{
    uint8_t value = a[0], prog = a[1], note = a[2];
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->active || !v->se_voice || v->se_prog != prog || v->key != note)
            continue;
        if (cmd == 1)
            ae3__lfo_set_depth(v, value);
        else
            ae3__lfo_set_rate(v, value);
    }
}

static void automation(ae3_synth *s, uint8_t cmd, const uint8_t *a)
{
    uint8_t duration = a[0], target = a[1], prog = a[2], note = a[3];
    float duration4 = (float)duration * 4.0f;
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->active || !v->se_voice || v->se_prog != prog || v->key != note)
            continue;
        if (cmd == 7) {
            /* FUN_003fb818 converts authored 480 Hz duration units to the 60 Hz
             * voice flush: step = delta * (480/rate) / (duration*4). */
            v->vel_target = target;
            v->vel_ramp = 0.0f;
            v->vel_step = duration4 ? ((float)target - v->vel) * 8.0f / duration4 : 0.0f;
            v->vel_ramp_on = duration4 != 0.0f && target != v->vel;
            if (!v->vel_ramp_on) {
                v->vel = target;
                ae3__voice_refresh(s, v);
            }
        } else if (cmd == 10) {
            v->pan_target = target;
            v->pan_ramp = 0.0f;
            v->pan_step = duration4 ? ((float)target - v->tpan) * 8.0f / duration4 : 0.0f;
            v->pan_ramp_on = duration4 != 0.0f && target != v->tpan;
            if (!v->pan_ramp_on) {
                v->tpan = (uint8_t)ae3__tpan_clamp(target);
                ae3__voice_refresh(s, v);
            }
        } else {
            /* Type 2 stores signed tenths of a semitone. The EE multiplies by
             * exactly float 1.2, yielding the flush's 12-units-per-semitone scale. */
            v->glide = 0.0f;
            v->glide_target = (float)(int8_t)target * 1.2000000476837158f;
            v->glide_step = duration4 ? v->glide_target / duration4 : 0.0f;
            v->glide_on = duration4 != 0.0f && v->glide_target != 0.0f;
        }
    }
}

static void add_delay(ae3_synth *s, uint32_t delay)
{
    if (s->timing_exact) {
        s->se.next_sample += (uint64_t)delay * SE_TICK_SAMPLES;
    } else if (delay) {
        /* The EE resets its elapsed counter after each event, so quantization is
         * per relative delay, not a ceil() over the cumulative stream clock. */
        s->se.next_sample += ((uint64_t)delay + 7u) / 8u * AE3_TICK_SAMPLES;
    }
}

static bool next_event(ae3_synth *s)
{
    ae3_seseq *q = &s->se;
    const uint8_t *p = q->p;
    if (p >= q->end) {
        q->active = false;
        q->ended = true;
        return false;
    }
    uint8_t status = *p;
    if (status & 0x80u) {
        p++;
        if (status == 0xff) {
            q->p = q->end;
            q->active = false;
            q->ended = true;
            return false;
        }
        q->running = status;
    } else {
        status = q->running;
    }

    bool delta_consumed = false;
    if (status == 0xa0) {
        uint8_t key = p[0], vel = p[1], prog = p[2];
        p += 3;
        if (prog < 16) {
            s->chan_prog[prog] = prog;
            if (vel)
                ae3__note_on(s, prog, key, vel);
            else
                ae3__note_off(s, prog, key);
        }
    } else {
        uint8_t cmd = *p++;
        if (cmd == 1 || cmd == 2) {
            lfo_control(s, cmd, p);
            p += 3;
        } else if (cmd == 7 || cmd == 10 || cmd == 0x41) {
            automation(s, cmd, p);
            p += 4;
        } else { /* B0 60: loop command; its handler owns the following delta. */
            uint16_t target = rd16(p);
            uint8_t count = p[2];
            p += 3;
            uint32_t delay;
            if (!vlq(&p, q->end, &delay)) {
                q->active = false;
                q->ended = true;
                return false;
            }
            add_delay(s, delay);
            delta_consumed = true;
            int loop_limit = count ? count : s->loop_cfg;
            bool jump = loop_limit >= AE3_LOOP_FOREVER ||
                        q->jump_count != (uint32_t)loop_limit;
            if (jump) {
                if (loop_limit < AE3_LOOP_FOREVER)
                    q->jump_count++;
                q->p = q->start + target;
                s->st.loops_taken++;
            } else {
                q->jump_count = 0;
                q->p = p;
            }
        }
    }
    q->events++;

    if (!delta_consumed) {
        uint32_t delay;
        if (!vlq(&p, q->end, &delay)) {
            q->active = false;
            q->ended = true;
            return false;
        }
        q->p = p;
        add_delay(s, delay);
    }
    return true;
}

void ae3__se_fire_due(ae3_synth *s)
{
    int guard = 0;
    while (s->se.active && s->se.next_sample <= s->pos) {
        if (!next_event(s))
            break;
        /* Protect a host from malformed zero-delay infinite bytecode even though
         * ae3__se_load validated the grammar. The shipped infinite loops delay. */
        if (++guard == 65536) {
            s->se.active = false;
            s->se.ended = true;
            break;
        }
    }
}

uint64_t ae3__se_next_sample(const ae3_synth *s)
{
    return s->se.next_sample;
}

static bool reached(float step, float value, float target)
{
    return step >= 0.0f ? value >= target : value <= target;
}

void ae3__se_tick(ae3_synth *s)
{
    for (int i = 0; i < AE3_NVOICES; i++) {
        ae3_voice *v = &s->voices[i];
        if (!v->active || !v->se_voice)
            continue;
        if (v->vel_ramp_on) {
            v->vel_ramp += v->vel_step;
            int d = (int)v->vel_ramp;
            if (d) {
                v->vel = (uint8_t)((int)v->vel + d);
                v->vel_ramp -= d;
                if (reached(v->vel_step, v->vel, v->vel_target)) {
                    v->vel = (uint8_t)v->vel_target;
                    v->vel_ramp_on = false;
                }
                ae3__voice_refresh(s, v);
            }
        }
        if (v->pan_ramp_on) {
            v->pan_ramp += v->pan_step;
            int d = (int)v->pan_ramp;
            if (d) {
                int pan = (int)v->tpan + d;
                v->pan_ramp -= d;
                if (reached(v->pan_step, pan, v->pan_target)) {
                    pan = (int)v->pan_target;
                    v->pan_ramp_on = false;
                }
                v->tpan = (uint8_t)ae3__tpan_clamp(pan);
                ae3__voice_refresh(s, v);
            }
        }
        if (v->glide_on) {
            v->glide += v->glide_step;
            if (reached(v->glide_step, v->glide, v->glide_target)) {
                v->glide = v->glide_target;
                v->glide_on = false;
            }
            int off = (int)v->glide;
            int semis = off / 12;
            int fine = (off % 12) * 4 / 3;
            v->pitch = ae3__pitch_reg(s, v->key + semis, v->tone->root,
                                      v->tone->tune + fine,
                                      s->chan_bend[v->ch], v->tone->bend);
        }
    }
}
