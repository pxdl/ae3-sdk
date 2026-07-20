/* serender -- headless renderer for one embedded SE (outer bank, request) stream.
 * Output is stereo s16 WAV at 48 kHz through the same core used by bgmplay. */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ae3synth.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path); exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static void wav_header(FILE *f, uint32_t bytes)
{
    uint8_t h[44] = {0};
    uint32_t riff = 36 + bytes, rate = AE3_RATE, byte_rate = AE3_RATE * 4, fmt = 16;
    uint16_t pcm = 1, chans = 2, align = 4, bits = 16;
    memcpy(h, "RIFF", 4);       memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt, 4);
    memcpy(h + 20, &pcm, 2);     memcpy(h + 22, &chans, 2);
    memcpy(h + 24, &rate, 4);    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);   memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);  memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, sizeof h, f);
}

static int integer(const char *s, const char *what)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 0);
    if (errno || *s == 0 || *end || v < 0 || v > INT32_MAX) {
        fprintf(stderr, "bad %s: %s\n", what, s); exit(2);
    }
    return (int)v;
}

static const char *env_name(uint8_t phase)
{
    static const char *const names[] = { "ATTACK", "DECAY", "SUSTAIN", "RELEASE", "OFF" };
    return phase <= AE3_ENV_OFF ? names[phase] : "?";
}

static const char *source_name(uint8_t kind)
{
    static const char *const names[] = { "NONE", "ONESHOT", "LOOPED", "NOISE" };
    return kind <= AE3_SOURCE_NOISE ? names[kind] : "?";
}

static void state_line(const char *tag, int slot, uint64_t frame,
                       const ae3_voice_state *v)
{
    printf("%s slot=%d frame=%llu in_use=%d active=%d released=%d key=%u"
           " env=%d env_phase=%s se_prog=%u source=%s waveform=%d samples=%u"
           " loop_start=%d phase_q12=%lld loops=%u\n",
           tag, slot, (unsigned long long)frame, v->in_use, v->active, v->released,
           v->key, v->env, env_name(v->env_phase), v->se_prog,
           source_name(v->source_kind), v->waveform, v->source_samples,
           v->source_loop_start, (long long)v->source_phase_q12, v->source_loops);
}

static int active_slot(const ae3_synth *s, uint8_t key)
{
    ae3_voice_state v;
    for (int slot = 0; slot < AE3_NVOICES; slot++) {
        if (ae3_synth_voice(s, slot, &v) && v.active && v.key == key)
            return slot;
    }
    return -1;
}

static int source_state_trace(ae3_synth *s)
{
    float mix[2];
    ae3_voice_state current, previous;
    uint64_t frame = 0;

    ae3_synth_voice(s, 0, &current);
    state_line("idle", 0, frame, &current);
    printf("out_of_range=%d\n", ae3_synth_voice(s, AE3_NVOICES, &current));

    ae3_synth_note_on(s, 0, 60, 100);
    int loop_slot = active_slot(s, 60);
    if (loop_slot < 0 || !ae3_synth_voice(s, loop_slot, &current))
        return 1;
    state_line("loop_unprimed", loop_slot, frame, &current);
    if (ae3_synth_render(s, mix, 1) != 1)
        return 1;
    frame++;
    ae3_synth_voice(s, loop_slot, &current);
    state_line("loop_primed", loop_slot, frame, &current);

    previous = current;
    while (current.source_loops < 2 && frame < 2000) {
        if (ae3_synth_render(s, mix, 1) != 1)
            return 1;
        frame++;
        ae3_synth_voice(s, loop_slot, &current);
        if (current.source_loops != previous.source_loops) {
            const char *before = current.source_loops == 1
                ? "loop1_before" : "loop2_before";
            const char *after = current.source_loops == 1
                ? "loop1_after" : "loop2_after";
            state_line(before, loop_slot, frame - 1, &previous);
            state_line(after, loop_slot, frame, &current);
        }
        previous = current;
    }
    if (current.source_loops != 2)
        return 1;

    ae3_synth_note_off(s, 0, 60);
    ae3_synth_voice(s, loop_slot, &current);
    state_line("loop_release", loop_slot, frame, &current);

    ae3_synth_note_on(s, 0, 61, 100);
    int noise_slot = active_slot(s, 61);
    if (noise_slot < 0 || !ae3_synth_voice(s, noise_slot, &current))
        return 1;
    state_line("noise", noise_slot, frame, &current);

    ae3_synth_note_on(s, 0, 62, 100);
    int one_slot = active_slot(s, 62);
    if (one_slot < 0 || !ae3_synth_voice(s, one_slot, &current))
        return 1;
    state_line("oneshot", one_slot, frame, &current);
    while (current.active && frame < 5000) {
        if (ae3_synth_render(s, mix, 1) != 1)
            return 1;
        frame++;
        ae3_synth_voice(s, one_slot, &current);
    }
    if (current.active)
        return 1;
    state_line("ended", one_slot, frame, &current);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = "se.wav", *libsd = NULL, *pitch = NULL;
    int seconds = 10, volume = 96, loop = AE3_LOOP_FOREVER;
    bool exact = true, gaussian = true, source_state = false;
    int a = 1;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "-o") && a + 1 < argc) out = argv[++a];
        else if (!strcmp(argv[a], "--seconds") && a + 1 < argc) seconds = integer(argv[++a], "seconds");
        else if (!strcmp(argv[a], "--volume") && a + 1 < argc) volume = integer(argv[++a], "volume");
        else if (!strcmp(argv[a], "--loop") && a + 1 < argc) loop = integer(argv[++a], "loop");
        else if (!strcmp(argv[a], "--libsd") && a + 1 < argc) libsd = argv[++a];
        else if (!strcmp(argv[a], "--pitch-irx") && a + 1 < argc) pitch = argv[++a];
        else if (!strcmp(argv[a], "--tick-events")) exact = false;
        else if (!strcmp(argv[a], "--exact-events")) exact = true;
        else if (!strcmp(argv[a], "--bright")) gaussian = false;
        else if (!strcmp(argv[a], "--source-state")) source_state = true;
        else { fprintf(stderr, "unknown option: %s\n", argv[a]); return 2; }
        a++;
    }
    int required = source_state ? 2 : 4;
    if (argc - a != required || seconds < 1 || volume < 0 || volume > 127 ||
        loop > AE3_LOOP_FOREVER) {
        fprintf(stderr, "usage: serender [OPTIONS] BANK.hd BANK.bd [BANK REQUEST]\n"
                        "  -o FILE.wav  --seconds N  --volume 0..127 (default 96)\n"
                        "  --loop 0..127 (default 127)  --pitch-irx FILE  --libsd FILE\n"
                        "  --tick-events  --bright  --source-state\n");
        return 2;
    }
    int bank = source_state ? 0 : integer(argv[a + 2], "bank");
    int request = source_state ? 0 : integer(argv[a + 3], "request");
    size_t nh, nb;
    uint8_t *hd = slurp(argv[a], &nh), *bd = slurp(argv[a + 1], &nb);
    ae3_synth *s = ae3_synth_new();
    if (!s) { fprintf(stderr, "out of memory\n"); return 1; }
    if (ae3_synth_load_bank(s, hd, nh, bd, nb) ||
        (!source_state && ae3_synth_load_se(s, bank, request))) {
        fprintf(stderr, "%s\n", ae3_synth_error(s)); return 1;
    }
    free(hd); free(bd);
    if (pitch) {
        size_t n; uint8_t *p = slurp(pitch, &n);
        if (ae3_synth_load_pitch_irx(s, p, n)) { fprintf(stderr, "%s\n", ae3_synth_error(s)); return 1; }
        free(p);
    }
    if (libsd) {
        size_t n; uint8_t *p = slurp(libsd, &n);
        if (ae3_synth_load_reverb_irx(s, p, n)) { fprintf(stderr, "%s\n", ae3_synth_error(s)); return 1; }
        free(p);
    }
    ae3_synth_event_timing(s, exact);
    ae3_synth_gaussian(s, gaussian);
    ae3_synth_set_loop(s, loop);
    ae3_synth_song_volume(s, volume, volume);
    if (source_state) {
        int rc = source_state_trace(s);
        ae3_synth_free(s);
        return rc;
    }

    FILE *f = fopen(out, "wb+");
    if (!f) { fprintf(stderr, "cannot create %s\n", out); return 1; }
    wav_header(f, 0);
    float mix[1024 * 2];
    int16_t pcm[1024 * 2];
    uint64_t frames = 0, cap = (uint64_t)seconds * AE3_RATE;
    while (frames < cap) {
        int want = cap - frames < 1024 ? (int)(cap - frames) : 1024;
        int got = ae3_synth_render(s, mix, want);
        if (got < 0) { fprintf(stderr, "%s\n", ae3_synth_error(s)); return 1; }
        if (!got) break;
        for (int i = 0; i < got * 2; i++) {
            float x = mix[i] * 32768.0f;
            int v = (int)lrintf(x);
            pcm[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        fwrite(pcm, sizeof *pcm, (size_t)got * 2, f);
        frames += (uint64_t)got;
    }
    if (frames > UINT32_MAX / 4) { fprintf(stderr, "WAV too large\n"); return 1; }
    fseek(f, 0, SEEK_SET);
    wav_header(f, (uint32_t)frames * 4);
    fclose(f);
    ae3_stats st;
    ae3_synth_get_stats(s, &st);
    printf("SE bank=%d request=%d frames=%llu seconds=%.6f ended=%d voices=%u peak=%u clipped=%u output=%s\n",
           bank, request, (unsigned long long)frames, (double)frames / AE3_RATE,
           ae3_synth_done(s), st.voices_started, st.peak_voices, st.bus_clipped, out);
    ae3_synth_free(s);
    return 0;
}
