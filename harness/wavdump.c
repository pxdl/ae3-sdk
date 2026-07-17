/* wavdump -- headless harness: render a song to WAV, or dump parsed/decoded state
 * for synth/check.py to diff against tools/bgm.py (the reference oracle).
 *
 * usage: wavdump [MODE] [--tail SEC] [--songvol 0..127] [--libsd PATH]
 *                [--rev-depth 0..127] [--stems PREFIX] [--tick-events]
 *                [--loop N] [-o OUT.wav] FILES...
 * FILES are classified by extension: .hd + .bd (bank, both or neither), .mid, and
 * .irx (the PITCH table, sg2iopm1.irx; the reverb's libsd.irx goes via --libsd).
 * --songvol sets the driver's song volume (default 127 = driver init; the game's
 * fade/volume API moves it -- see ae3_synth_song_volume).
 * --libsd loads the SPU2 reverb preset (STUDIO_C depth 30, the boot pin) from the
 * extracted libsd.irx; without it renders are pure dry, bit-identical to pre-M6.
 * --rev-depth overrides the depth knob (0 = dry). --stems PREFIX (needs --libsd)
 * writes PREFIX.dry.raw / PREFIX.wet.raw (the saturated s16 buses) and
 * PREFIX.rev.raw (dry + EVOL*reverb quantized exactly as tools/spu2rev.c writes
 * its output) alongside the render, for check.py's bitwise oracle diff.
 * --tick-events dispatches events in the console's 60 Hz walker bursts (M7) instead
 * of exact sample positions (the default -- the user-settled dial: tick reads as
 * "notes late / emulator slowdown"; use tick for emulation-faithful output and
 * PCSX2-dump comparisons). --exact-events selects the default explicitly.
 * --loop N takes the sequence's CC99 20/30 loop markers N times (1..126; default 0
 * = play through once, which is what every oracle A/B expects. The console plays
 * BGM with the API's AE3_LOOP_FOREVER).
 * --bright swaps the SPU2's gaussian resampler kernel for catmull-rom (brighter,
 * NOT hardware -- the ae3_synth_gaussian A/B; renders and checks default gaussian).
 *
 * Modes (all print machine-readable lines and skip song rendering):
 *   --dump               bank programs/tones + sequence stats
 *   --decode             every unique waveform: sample count, loop point, FNV hash,
 *                        and a second-pass hash across the loop seam
 *   --envdump            per distinct (ADSR1,ADSR2): per-phase envelope cycle counts
 *   --voldump            pan table + VOLL/VOLR registers over a CC/velocity grid
 *   --slotdump           render the song discarding audio, tracing the voice-slot
 *                        lifecycle: G(rant)/D(rop)/F(ree)/Z(ombie) lines + S totals
 *   --eventdump          render the song discarding audio, tracing every dispatched
 *                        event: EV lines (pos/index/kind) + an EVS totals line, for
 *                        check.py's independent SMF-walker mirror
 *   --tone P:K:V:ON[:TOT]  render one tone via the real voice path to -o OUT.wav
 * Default mode renders .hd+.bd+.mid to -o. Output is s16 stereo 48 kHz, the same
 * shape as bgm.py's WAVs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ae3synth.h"
#include "internal.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;             memcpy(h + 16, &fmtlen, 4);
    uint16_t pcm = 1, chans = 2;      memcpy(h + 20, &pcm, 2); memcpy(h + 22, &chans, 2);
    uint32_t rate = AE3_RATE;         memcpy(h + 24, &rate, 4);
    uint32_t bps = AE3_RATE * 4;      memcpy(h + 28, &bps, 4);
    uint16_t align = 4, bits = 16;    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fwrite(h, 1, 44, f);
}

static void dump(const ae3_synth *s, bool have_bank, bool have_seq)
{
    if (have_bank) {
        const ae3_bank *b = &s->bank;
        bool velid = true;
        for (int i = 0; i < 128; i++)
            if (b->vel[i] != i) velid = false;
        printf("BANK slots=%d used=%u tones=%u velcount=%u velid=%d bd=%zu\n",
               b->nprogs, s->st.progs_used, s->st.tones, b->vel_count, velid, b->bd_len);
        for (int i = 0; i < b->nprogs; i++) {
            const ae3_prog *p = &b->progs[i];
            if (!p->present)
                continue;
            printf("P i=%d vol=%u bend=%u lfo=%u drum=%d stack=%d key0=%u key1=%u n=%d\n",
                   i, p->vol, p->bend, p->lfo, p->drum, p->stack, p->key0, p->key1,
                   p->ntones);
            for (int t = 0; t < p->ntones; t++) {
                const ae3_tone *x = &p->tones[t];
                printf("T p=%d i=%d lo=%u hi=%u root=%u tune=%d addr=%u a1=%u a2=%u "
                       "vol=%u pan=%u flags=%u bend=%u lfo=%u\n",
                       i, t, x->lo, x->hi, x->root, x->tune, x->addr, x->adsr1, x->adsr2,
                       x->vol, x->pan, x->flags, x->bend, x->lfo);
            }
        }
    }
    if (have_seq) {
        const ae3_stats *st = &s->st;
        printf("SEQ ppqn=%u events=%u non=%u noff=%u cc=%u pc=%u bend=%u tempo=%u "
               "skipped=%u end_tick=%u end_sample=%llu hash_ch=%016llx hash_tempo=%016llx\n",
               st->ppqn, st->events, st->note_ons, st->note_offs, st->ccs,
               st->prog_changes, st->pitch_bends, st->tempo_changes, st->meta_skipped,
               st->end_tick, (unsigned long long)st->end_sample,
               (unsigned long long)st->hash_ch, (unsigned long long)st->hash_tempo);
    }
    if (s->rev.buf) {          /* preset as loaded, for check.py vs bgm.load_reverb */
        printf("REV units=%u depth=%d evol=%.17g raw=", s->rev.units, s->rev.depth,
               s->rev.evol);
        for (int i = 0; i < AE3_REV_NCOEF; i++)
            printf("%s%u", i ? "," : "", s->rev.raw[i]);
        printf("\n");
    }
}

/* --slotdump: one line per slot event for check.py's independent policy mirror. */
static void slot_trace_print(void *user, char ev, uint64_t pos, int slot,
                             int ch, int key)
{
    (void)user;
    printf("%c pos=%llu slot=%d ch=%d key=%d\n", ev, (unsigned long long)pos,
           slot, ch, key);
}

static void slot_dump(ae3_synth *s)
{
    s->slot_trace = slot_trace_print;
    float buf[1024 * 2];
    while (ae3_synth_render(s, buf, 1024) > 0)
        ;
    ae3__slot_flush(s);   /* trailing frees, at the positions the console would */
    s->slot_trace = NULL;
    ae3_stats st;
    ae3_synth_get_stats(s, &st);
    printf("S started=%u dropped=%u aborted=%u peak=%u zombie=%u\n",
           st.voices_started, st.notes_dropped, st.noteons_aborted,
           st.peak_voices, st.slots_freed_live);
}

/* --eventdump: one line per dispatched event for check.py's SMF-walker mirror. */
static void ev_trace_print(void *user, uint64_t pos, int idx, const ae3_event *e)
{
    (void)user;
    printf("EV pos=%llu i=%d k=%u st=%u a=%u b=%u u=%u\n", (unsigned long long)pos,
           idx, e->kind, e->status, e->a, e->b, e->uspqn);
}

static void event_dump(ae3_synth *s)
{
    s->ev_trace = ev_trace_print;
    float buf[1024 * 2];
    while (ae3_synth_render(s, buf, 1024) > 0)
        ;
    s->ev_trace = NULL;
    ae3_stats st;
    ae3_synth_get_stats(s, &st);
    printf("EVS lstart=%u lend=%u lset=%u hooks=%u loops=%u cclfo=%u ccnrpn=%u "
           "cc6shadow=%u cc6apply=%u ccstub=%u\n",
           st.loop_starts, st.loop_ends, st.loop_sets, st.hooks, st.loops_taken,
           st.cc_lfo, st.cc_nrpn, st.cc6_shadow, st.cc6_rev_apply, st.cc_stub);
}

/* --stems: dry/wet buses + the spu2rev-quantized combine, straight to disk. */
typedef struct { FILE *fd, *fw, *fr; } stems_files;

static void stems_tap(void *user, const int16_t *dry, const int16_t *wet,
                      const int16_t *rev, int nframes)
{
    stems_files *sf = user;
    fwrite(dry, sizeof(int16_t), (size_t)nframes * 2, sf->fd);
    fwrite(wet, sizeof(int16_t), (size_t)nframes * 2, sf->fw);
    fwrite(rev, sizeof(int16_t), (size_t)nframes * 2, sf->fr);
}

static FILE *stem_open(const char *prefix, const char *suffix)
{
    char path[1024];
    snprintf(path, sizeof path, "%s%s", prefix, suffix);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    return f;
}

#define FNV_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv_s16(uint64_t h, int16_t v)
{
    h ^= (uint8_t)v;         h *= FNV_PRIME;
    h ^= (uint8_t)(v >> 8);  h *= FNV_PRIME;
    return h;
}

static int addr_cmp(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* Stream-decode every unique waveform through the live decoder. Pass 1 runs to the
 * END frame; for looped waveforms a second pass re-runs the loop region so check.py
 * can verify the carried-history seam. */
static void decode_dump(const ae3_synth *s)
{
    const ae3_bank *b = &s->bank;
    uint16_t addrs[4096];
    int na = 0;
    for (int i = 0; i < b->nprogs; i++)
        for (int t = 0; b->progs[i].present && t < b->progs[i].ntones; t++) {
            uint16_t a = b->progs[i].tones[t].addr;
            if (a != AE3_NO_SAMPLE)
                addrs[na++] = a;
        }
    qsort(addrs, (size_t)na, sizeof *addrs, addr_cmp);
    for (int i = 0; i < na; i++) {
        if (i && addrs[i] == addrs[i - 1])
            continue;
        ae3_adpcm d;
        ae3_adpcm_init(&d, b->bd, b->bd_len, (uint32_t)addrs[i] * 8);
        uint64_t h = FNV_BASIS, h2 = FNV_BASIS;
        uint32_t n = 0, n2 = 0;
        int16_t v;
        for (;;) {                            /* pass 1 = the samples up to the END frame */
            if (!ae3_adpcm_next(&d, &v))
                break;                        /* one-shot ended */
            if (d.loops)
                break;                        /* jumped: v is the first post-seam sample */
            h = fnv_s16(h, v);
            n++;
        }
        int32_t loop = -1;
        if (d.loops) {                        /* looped: v is the first post-seam sample */
            loop = (d.loop_frame - (int32_t)d.start) / 16 * 28;
            uint32_t want = n - (uint32_t)loop;   /* one full second pass */
            h2 = fnv_s16(h2, v);
            for (n2 = 1; n2 < want && ae3_adpcm_next(&d, &v); n2++)
                h2 = fnv_s16(h2, v);
        }
        printf("W addr=%u n=%u loop=%d hash=%016llx n2=%u hash2=%016llx\n",
               addrs[i], n, loop, (unsigned long long)h, n2, (unsigned long long)h2);
    }
}

/* Pitch table + register grid for check.py: PTBL = the active table; R lines = the
 * register for every tone at its window edges/root under a spread of bend values,
 * mirrored in Python straight from the ev_set_pitch disassembly. */
static void pitch_dump(ae3_synth *s)
{
    printf("PVER verbatim=%d\n", s->pitch_verbatim);
    printf("PTBL");
    for (int i = 0; i < AE3_PITCH_TBL_N; i++)
        printf(" %u", s->pitch_tbl[i]);
    printf("\n");
    printf("GAUSS");
    for (int i = 0; i < 512; i++)
        printf(" %d", ae3_gauss[i]);
    printf("\n");
    const ae3_bank *b = &s->bank;
    static const int bends[] = { 0, 1, 32, 63, 64, 65, 96, 127 };
    for (int i = 0; i < b->nprogs; i++)
        for (int t = 0; b->progs[i].present && t < b->progs[i].ntones; t++) {
            const ae3_tone *x = &b->progs[i].tones[t];
            if (x->addr == AE3_NO_SAMPLE || x->bend_raw == 0xFF)
                continue;
            int notes[5] = { x->lo, x->hi, (x->lo + x->hi) / 2, x->root,
                             x->root + 7 };
            for (int nn = 0; nn < 5; nn++) {
                int note = notes[nn];
                if (note < x->lo || note > x->hi)
                    continue;
                for (size_t bb = 0; bb < sizeof bends / sizeof *bends; bb++)
                    printf("R note=%d root=%u fine=%d bmsb=%d range=%u reg=%u\n",
                           note, x->root, x->tune, bends[bb], x->bend,
                           ae3__pitch_reg(s, note, x->root, x->tune, bends[bb],
                                          x->bend));
            }
        }
}

/* Pan table + the VOLL/VOLR registers for every real tone over a grid of velocity,
 * CC7, CC11 and CC10 values -- check.py mirrors the FUN_00400c00 math independently
 * and diffs the ELF's own pan-table bytes against PANLUT. */
static void vol_dump(const ae3_synth *s)
{
    const uint16_t *lut = ae3__pan_lut();
    printf("PANLUT");
    for (int i = 0; i < 128; i++)
        printf(" %u", lut[i]);
    printf("\n");
    static const int vels[]  = { 1, 20, 64, 127 };
    static const int cc711[][2] = { { 127, 127 }, { 127, 90 }, { 64, 32 }, { 100, 1 } };
    static const int cc10s[] = { 64, 1, 127, 32 };
    static const int svs[][2] = { { 127, 127 }, { 32, 96 } };
    const ae3_bank *b = &s->bank;
    for (int i = 0; i < b->nprogs; i++)
        for (int t = 0; b->progs[i].present && t < b->progs[i].ntones; t++) {
            const ae3_prog *p = &b->progs[i];
            const ae3_tone *x = &p->tones[t];
            if (x->addr == AE3_NO_SAMPLE || x->bend_raw == 0xFF)
                continue;
            for (size_t a = 0; a < sizeof vels / sizeof *vels; a++)
                for (size_t c = 0; c < sizeof cc711 / sizeof *cc711; c++)
                    for (size_t d = 0; d < sizeof cc10s / sizeof *cc10s; d++)
                        for (size_t e = 0; e < sizeof svs / sizeof *svs; e++) {
                            int32_t vv = ae3__vol_product(cc711[c][0], cc711[c][1],
                                                          p->vol, x->vol);
                            uint16_t vl, vr;
                            ae3__voice_regs(svs[e][0], svs[e][1], vv, vels[a],
                                            ae3__cpan_clamp(cc10s[d]),
                                            ae3__tpan_clamp(x->pan), &vl, &vr);
                            printf("V pvol=%u tvol=%u tpan=%u vel=%d cc7=%d "
                                   "cc11=%d cc10=%d svl=%d svr=%d voll=%u volr=%u\n",
                                   p->vol, x->vol, x->pan, vels[a], cc711[c][0],
                                   cc711[c][1], cc10s[d], svs[e][0], svs[e][1],
                                   vl, vr);
                        }
        }
}

/* Per distinct (a1,a2): isolated per-phase cycle counts, mirrored by check.py against
 * bgm._phase_time. Phases run exactly as adsr_times does: attack 0->max, decay
 * max->sus, sustain sus->0 (decrease dir only), release max->0. */
static void env_dump(const ae3_synth *s)
{
    const ae3_bank *b = &s->bank;
    uint32_t seen[4096];
    int ns = 0;
    for (int i = 0; i < b->nprogs; i++)
        for (int t = 0; b->progs[i].present && t < b->progs[i].ntones; t++) {
            const ae3_tone *x = &b->progs[i].tones[t];
            if (x->addr == AE3_NO_SAMPLE)
                continue;
            uint32_t key = (uint32_t)x->adsr1 << 16 | x->adsr2;
            bool dup = false;
            for (int j = 0; j < ns && !dup; j++)
                dup = seen[j] == key;
            if (dup)
                continue;
            seen[ns++] = key;
            uint16_t a1 = x->adsr1, a2 = x->adsr2;
            int32_t sus = ((a1 & 0x0F) + 1) * 0x800;
            if (sus > 0x7FFF)
                sus = 0x7FFF;
            int sdir = (a2 >> 14) & 1;
            uint64_t atk = ae3__env_phase_cycles((a1 >> 10) & 0x1F, 7 - ((a1 >> 8) & 3),
                                                 (a1 >> 15) & 1, true, 0, 0x7FFF);
            uint64_t dec = ae3__env_phase_cycles((a1 >> 4) & 0x0F, -8, true, false,
                                                 0x7FFF, sus);
            uint64_t susf = sdir ? ae3__env_phase_cycles((a2 >> 8) & 0x1F,
                                                         -8 + ((a2 >> 6) & 3),
                                                         (a2 >> 15) & 1, false, sus, 0)
                                 : 0;
            uint64_t rel = ae3__env_phase_cycles(a2 & 0x1F, -8, (a2 >> 5) & 1, false,
                                                 0x7FFF, 0);
            printf("E a1=%u a2=%u sus=%d sdir=%d atk=%llu dec=%llu susf=%llu rel=%llu\n",
                   a1, a2, sus, sdir, (unsigned long long)atk, (unsigned long long)dec,
                   (unsigned long long)susf, (unsigned long long)rel);
        }
}

int main(int argc, char **argv)
{
    const char *hd_path = NULL, *bd_path = NULL, *mid_path = NULL, *out_path = NULL;
    const char *tone_arg = NULL, *irx_path = NULL;
    const char *libsd_path = NULL, *stems_prefix = NULL;
    double tail = 1.0;
    int songvol = 127;           /* driver init; see ae3_synth_song_volume */
    int rev_depth = -1;          /* -1 = leave the boot value (30) */
    int nslots = 0;              /* 0 = the real 48 */
    int loop = 0;                /* markers ignored; the console uses FOREVER */
    bool exact_events = true;    /* settled dial: exact by default, tick opt-in */
    bool bright = false;         /* gaussian (hardware) unless --bright */
    bool do_dump = false, do_decode = false, do_env = false, do_pitch = false;
    bool do_vol = false, do_slot = false, do_ev = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dump")) {
            do_dump = true;
        } else if (!strcmp(a, "--decode")) {
            do_decode = true;
        } else if (!strcmp(a, "--envdump")) {
            do_env = true;
        } else if (!strcmp(a, "--pitchdump")) {
            do_pitch = true;
        } else if (!strcmp(a, "--voldump")) {
            do_vol = true;
        } else if (!strcmp(a, "--slotdump")) {
            do_slot = true;
        } else if (!strcmp(a, "--eventdump")) {
            do_ev = true;
        } else if (!strcmp(a, "--exact-events")) {
            exact_events = true;
        } else if (!strcmp(a, "--tick-events")) {
            exact_events = false;
        } else if (!strcmp(a, "--bright")) {
            bright = true;         /* catmull-rom kernel (NOT hardware; see
                                      ae3_synth_gaussian) */
        } else if (!strcmp(a, "--loop") && i + 1 < argc) {
            loop = atoi(argv[++i]);
            if (loop < 0 || loop >= AE3_LOOP_FOREVER) {
                fprintf(stderr, "--loop wants 0..%d (the API's AE3_LOOP_FOREVER "
                                "would render forever)\n", AE3_LOOP_FOREVER - 1);
                return 1;
            }
        } else if (!strcmp(a, "--slots") && i + 1 < argc) {
            nslots = atoi(argv[++i]);   /* test-only: shrink the pool to force the
                                           drop path (the game data never fills 48) */
            if (nslots < 1 || nslots > AE3_NVOICES) {
                fprintf(stderr, "--slots wants 1..%d\n", AE3_NVOICES);
                return 1;
            }
        } else if (!strcmp(a, "--tone") && i + 1 < argc) {
            tone_arg = argv[++i];
        } else if (!strcmp(a, "--tail") && i + 1 < argc) {
            tail = atof(argv[++i]);
        } else if (!strcmp(a, "--songvol") && i + 1 < argc) {
            songvol = atoi(argv[++i]);
        } else if (!strcmp(a, "--libsd") && i + 1 < argc) {
            libsd_path = argv[++i];
        } else if (!strcmp(a, "--rev-depth") && i + 1 < argc) {
            rev_depth = atoi(argv[++i]);
            if (rev_depth < 0 || rev_depth > 127) {
                fprintf(stderr, "--rev-depth wants 0..127\n");
                return 1;
            }
        } else if (!strcmp(a, "--stems") && i + 1 < argc) {
            stems_prefix = argv[++i];
        } else if (!strcmp(a, "-o") && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            const char *dot = strrchr(a, '.');
            if (dot && !strcmp(dot, ".hd")) hd_path = a;
            else if (dot && !strcmp(dot, ".bd")) bd_path = a;
            else if (dot && !strcmp(dot, ".mid")) mid_path = a;
            else if (dot && !strcmp(dot, ".irx")) irx_path = a;
            else { fprintf(stderr, "unrecognized argument: %s\n", a); return 1; }
        }
    }
    if (!!hd_path != !!bd_path) {
        fprintf(stderr, ".hd and .bd must be given together\n");
        return 1;
    }
    if (!hd_path && !mid_path) {
        fprintf(stderr, "usage: wavdump [--dump] [--tail SEC] [-o OUT.wav] "
                        "NAME.hd NAME.bd NAME.mid\n");
        return 1;
    }

    ae3_synth *s = ae3_synth_new();
    ae3_synth_event_timing(s, exact_events);
    ae3_synth_gaussian(s, !bright);
    ae3_synth_set_loop(s, loop);
    if (irx_path) {
        size_t in;
        uint8_t *irx = slurp(irx_path, &in);
        if (ae3_synth_load_pitch_irx(s, irx, in)) {
            fprintf(stderr, "%s: %s\n", irx_path, ae3_synth_error(s));
            return 1;
        }
        free(irx);
    }
    if (hd_path) {
        size_t hn, bn;
        uint8_t *hd = slurp(hd_path, &hn), *bd = slurp(bd_path, &bn);
        if (ae3_synth_load_bank(s, hd, hn, bd, bn)) {
            fprintf(stderr, "%s: %s\n", hd_path, ae3_synth_error(s));
            return 1;
        }
        free(hd); free(bd);
    }
    if (mid_path) {
        size_t mn;
        uint8_t *mid = slurp(mid_path, &mn);
        if (ae3_synth_load_seq(s, mid, mn)) {
            fprintf(stderr, "%s: %s\n", mid_path, ae3_synth_error(s));
            return 1;
        }
        free(mid);
    }
    if (libsd_path) {
        size_t ln;
        uint8_t *libsd = slurp(libsd_path, &ln);
        if (ae3_synth_load_reverb_irx(s, libsd, ln)) {
            fprintf(stderr, "%s: %s\n", libsd_path, ae3_synth_error(s));
            return 1;
        }
        free(libsd);
    }
    if (rev_depth >= 0) {
        if (!libsd_path) {
            fprintf(stderr, "--rev-depth needs --libsd\n");
            return 1;
        }
        ae3_synth_reverb_depth(s, rev_depth);
    }
    if (stems_prefix && !libsd_path) {
        fprintf(stderr, "--stems needs --libsd (the tap fires on the reverb path)\n");
        return 1;
    }
    ae3_synth_song_volume(s, songvol, songvol);
    if (nslots)
        s->nslots = nslots;

    if (do_dump || do_decode || do_env || do_pitch || do_vol || do_slot || do_ev) {
        if ((do_decode || do_env || do_pitch || do_vol) && !hd_path) {
            fprintf(stderr, "--decode/--envdump/--pitchdump/--voldump need a bank\n");
            return 1;
        }
        if (do_slot && (!hd_path || !mid_path)) {
            fprintf(stderr, "--slotdump needs .hd + .bd + .mid\n");
            return 1;
        }
        if (do_ev && !mid_path) {
            fprintf(stderr, "--eventdump needs a .mid\n");
            return 1;
        }
        if (do_dump)
            dump(s, hd_path != NULL, mid_path != NULL);
        if (do_decode)
            decode_dump(s);
        if (do_env)
            env_dump(s);
        if (do_pitch)
            pitch_dump(s);
        if (do_vol)
            vol_dump(s);
        if (do_slot)
            slot_dump(s);
        if (do_ev)
            event_dump(s);
        ae3_synth_free(s);
        return 0;
    }

    if (tone_arg) {                            /* --tone P:K:V:ON[:TOT] */
        if (!hd_path || !out_path) {
            fprintf(stderr, "--tone needs a bank and -o OUT.wav\n");
            return 1;
        }
        int prog, key, vel;
        double on_s, tot_s = -1;
        if (sscanf(tone_arg, "%d:%d:%d:%lf:%lf", &prog, &key, &vel, &on_s, &tot_s) < 4) {
            fprintf(stderr, "--tone wants P:K:V:ON_SEC[:TOTAL_SEC]\n");
            return 1;
        }
        if (tot_s < 0)
            tot_s = on_s + 3.0;
        FILE *f = fopen(out_path, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
        uint64_t on = (uint64_t)llround(on_s * AE3_RATE);
        uint64_t total = (uint64_t)llround(tot_s * AE3_RATE);
        wav_header(f, (uint32_t)(total * 4));
        ae3_synth_program(s, 0, prog);
        ae3_synth_note_on(s, 0, key, vel);
        enum { TB = 4096 };
        float buf[TB * 2];
        int16_t pcm[TB * 2];
        bool off_sent = false;
        for (uint64_t done = 0; done < total; ) {
            if (!off_sent && done >= on) {
                ae3_synth_note_off(s, 0, key);
                off_sent = true;
            }
            uint64_t stop = off_sent ? total : (on < total ? on : total);
            int n = (int)(stop - done < TB ? stop - done : TB);
            int got = ae3_synth_render(s, buf, n);
            if (got < n)                       /* voice died: pad with silence */
                memset(buf + 2 * got, 0, (size_t)(n - got) * 2 * sizeof(float));
            for (int i = 0; i < n * 2; i++) {
                /* the core's floats are exact s16/32768 values: recover them exactly */
                float v = buf[i] * 32768.0f;
                pcm[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : lrintf(v));
            }
            fwrite(pcm, sizeof(int16_t), (size_t)n * 2, f);
            done += (uint64_t)n;
        }
        fclose(f);
        ae3_stats st;
        ae3_synth_get_stats(s, &st);
        printf("tone: prog %d key %d vel %d, on %.2fs of %.2fs -> %s (%u voice(s))\n",
               prog, key, vel, on_s, tot_s, out_path, st.voices_started);
        ae3_synth_free(s);
        return 0;
    }

    if (!hd_path || !mid_path || !out_path) {
        fprintf(stderr, "rendering needs .hd + .bd + .mid and -o OUT.wav\n");
        return 1;
    }

    ae3_stats st;
    ae3_synth_get_stats(s, &st);
    stems_files sf = { NULL, NULL, NULL };
    if (stems_prefix) {
        sf.fd = stem_open(stems_prefix, ".dry.raw");
        sf.fw = stem_open(stems_prefix, ".wet.raw");
        sf.fr = stem_open(stems_prefix, ".rev.raw");
        s->mix_tap = stems_tap;
        s->mix_tap_user = &sf;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out_path); return 1; }
    wav_header(f, 0);   /* patched below */

    enum { BLOCK = 4096 };
    float buf[BLOCK * 2];
    int16_t pcm[BLOCK * 2];
    uint64_t frames = 0;
    for (;;) {
        int n = ae3_synth_render(s, buf, BLOCK);
        if (n < 0) { fprintf(stderr, "render: %s\n", ae3_synth_error(s)); return 1; }
        if (n == 0)
            break;
        for (int i = 0; i < n * 2; i++) {
            /* the core's floats are exact s16/32768 values: recover them exactly */
            float v = buf[i] * 32768.0f;
            pcm[i] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : lrintf(v));
        }
        fwrite(pcm, sizeof(int16_t), (size_t)n * 2, f);
        frames += (uint64_t)n;
    }
    uint64_t tail_frames = (uint64_t)llround(tail * AE3_RATE);
    memset(pcm, 0, sizeof pcm);
    for (uint64_t left = tail_frames; left; ) {
        uint64_t n = left < BLOCK ? left : BLOCK;
        fwrite(pcm, sizeof(int16_t), (size_t)n * 2, f);
        left -= n;
    }
    uint32_t data_bytes = (uint32_t)((frames + tail_frames) * 4);
    fseek(f, 0, SEEK_SET);
    wav_header(f, data_bytes);
    fclose(f);
    if (stems_prefix) {
        fclose(sf.fd);
        fclose(sf.fw);
        fclose(sf.fr);
        s->mix_tap = NULL;
    }

    printf("bank: %u/%u programs, %u tones, bd %zu bytes\n",
           st.progs_used, st.prog_slots, st.tones, s->bank.bd_len);
    printf("seq : %u ppqn, %u events (%u on / %u off / %u cc / %u pc / %u bend), "
           "%u tempo changes\n", st.ppqn, st.events, st.note_ons, st.note_offs,
           st.ccs, st.prog_changes, st.pitch_bends, st.tempo_changes);
    ae3_synth_get_stats(s, &st);
    printf("dur : %.2f s (%llu samples) + %.2f s tail\n",
           (double)frames / AE3_RATE, (unsigned long long)frames, tail);
    printf("mix : %u voices started, peak %u/%d slots, %u note-ons aborted, "
           "%u dropped (48-slot pool)%s\n",
           st.voices_started, st.peak_voices, AE3_NVOICES, st.noteons_aborted,
           st.notes_dropped,
           st.slots_freed_live ? " [ZOMBIE FREES -- investigate]" : "");
    if (loop)
        printf("loop: %d pass(es) configured, %u loop-end jumps taken\n",
               loop, st.loops_taken);
    printf("pit : %s table (%s events), %u idx clamps, %u step clamps\n",
           irx_path ? "verbatim sg2iopm1" : "computed ET",
           exact_events ? "exact" : "60 Hz tick", st.pitch_idx_clamped,
           st.pitch_step_clamped);
    printf("bus : peak %.2fx full scale, %u samples saturated (hardware behavior; "
           "PCSX2 dump will arbitrate audibility)\n",
           st.bus_peak / 32767.0, st.bus_clipped);
    if (s->rev.on)
        printf("rev : STUDIO_C depth %d (EVOL %.5f), wet bus peak %.2fx, "
               "%u samples saturated, %s tail\n",
               s->rev.depth, s->rev.evol, st.wet_peak / 32767.0, st.wet_clipped,
               s->wet_ever ? "4 s" : "no wet voice -> no");
    printf("out : %s\n", out_path);
    ae3_synth_free(s);
    return 0;
}
