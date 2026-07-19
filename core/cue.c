/* cue.c -- the game's cue-layer volume model (M8): per-song volume + ducking.
 *
 * This is the layer ABOVE the driver -- the game's C++ sound system, decompiled
 * and pinned in decomp/functions_bgm/cue/NOTES.md + cc/NOTES.md §6 (private
 * repo). Per frame the game (1) steps each duck group's float linearly toward
 * its held target (FUN_005b82fc; the per-frame step is |A-B|/seconds x frame dt,
 * baked at construction -- FUN_003672b4), then (2) recomputes the cue's songvol
 * from the full float product (FUN_0035ccf0/FUN_003679a4) and re-sends it to the
 * driver as an absolute volume set. The driver's own fade machinery (port mode
 * 0x100) is never used for this.
 *
 * Here the "frame" is the render clock's 60 Hz tick (AE3_TICK_SAMPLES), so the
 * layer is sample-clocked and engine-agnostic; a 0.5 s crossfade is exactly
 * 0.5 s of output. The float product preserves the game's operation order and
 * its 1/127 constant (127 x 0.007874016f == 1.0f exactly, verified against all
 * 115 bgm_desc records: float order and the tools' double order never disagree).
 */
#include "internal.h"

/* FUN_0035ccf0's base/127 constant. base is the cue volume int, fixed at its
 * ctor default here (1.0f -> 127; nothing in the BGM path is pinned moving it). */
#define AE3_CUE_K127 0.007874016f

void ae3__cue_init(ae3_cue *c)
{
    c->on = false;
    c->scale = 1.0f;
    c->slider = 1.0f;
    c->dolby = false;
    for (int i = 0; i < AE3_NDUCKS; i++)
        c->duck[i] = (ae3_duck){ 1.0f, 0.7f, 1.0f, false,
                                 /* the disc's sound_config crossfades, per tick */
                                 0.3f / 0.5f / (float)AE3_TICK_HZ,
                                 0.3f / 2.0f / (float)AE3_TICK_HZ };
    c->songvol = -1;
}

/* FUN_005b82fc: constant-rate linear step toward the held target, overshoot
 * clamped. (The game then clears the assert byte; our held bool persists.) */
static void duck_step(ae3_duck *g)
{
    float target = g->held ? g->b : g->a;
    float step   = g->held ? g->step_in : g->step_out;
    if (g->cur < target) {
        g->cur += step;
        if (g->cur > target)
            g->cur = target;
    } else if (g->cur > target) {
        g->cur -= step;
        if (g->cur < target)
            g->cur = target;
    }
}

void ae3__cue_apply(ae3_synth *s)
{
    ae3_cue *c = &s->cue;
    if (!c->on)
        return;
    /* FUN_003679a4's product, its float order: (base x 1/127) x slider x
     * phone(mgr+0x18) x demo(mgr+0x14) x master(1.0, never moves) x dolby;
     * then FUN_0035ccf0: scale x product x 127, truncated, 0..127 -- the
     * driver side of the set clamps 127 to 126 (M7, PINE-verified). */
    float prod = (127.0f * AE3_CUE_K127) * c->slider
               * c->duck[AE3_DUCK_PHONE].cur
               * c->duck[AE3_DUCK_DEMO].cur
               * (c->dolby ? 0.6f : 1.0f);
    float f = c->scale * prod * 127.0f;
    int v = !(f > 0.0f) ? 0 : f < 127.0f ? (int)f : 127;
    if (v > 126)
        v = 126;
    if (v != c->songvol) {
        c->songvol = v;
        ae3_synth_song_volume(s, v, v);
    }
}

void ae3__cue_tick(ae3_synth *s)
{
    if (!s->cue.on)
        return;
    for (int i = 0; i < AE3_NDUCKS; i++)
        duck_step(&s->cue.duck[i]);
    ae3__cue_apply(s);
}

void ae3_synth_cue_enable(ae3_synth *s, bool on)
{
    s->cue.on = on;
    if (on) {
        s->cue.songvol = -1;   /* force the first apply even at the same value */
        ae3__cue_apply(s);
    }
}

void ae3_synth_cue_scale(ae3_synth *s, float volume_scale)
{
    s->cue.scale = volume_scale;
    ae3__cue_apply(s);
}

void ae3_synth_cue_slider(ae3_synth *s, float bgm_volume)
{
    s->cue.slider = bgm_volume;
    ae3__cue_apply(s);
}

void ae3_synth_cue_dolby(ae3_synth *s, bool on)
{
    s->cue.dolby = on;
    ae3__cue_apply(s);
}

void ae3_synth_cue_duck(ae3_synth *s, int which, bool active)
{
    if (which < 0 || which >= AE3_NDUCKS)
        return;
    s->cue.duck[which].held = active;   /* the ramp starts at the next tick */
}

void ae3_synth_cue_duck_config(ae3_synth *s, int which,
                               float level, float in_secs, float out_secs)
{
    if (which < 0 || which >= AE3_NDUCKS)
        return;
    ae3_duck *g = &s->cue.duck[which];
    g->b = level;
    float dist = g->a - level;
    if (dist < 0.0f)
        dist = -dist;
    g->step_in  = in_secs  > 0.0f ? dist / in_secs  / (float)AE3_TICK_HZ : dist;
    g->step_out = out_secs > 0.0f ? dist / out_secs / (float)AE3_TICK_HZ : dist;
}

int ae3_synth_cue_songvol(const ae3_synth *s)
{
    return s->cue.on ? s->cue.songvol : -1;
}

float ae3_synth_cue_duck_level(const ae3_synth *s, int which)
{
    if (which < 0 || which >= AE3_NDUCKS)
        return 1.0f;
    return s->cue.duck[which].cur;
}
