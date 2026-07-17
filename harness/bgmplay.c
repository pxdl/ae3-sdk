/* bgmplay -- SDL2 GUI harness: real-time song playback with a piano-roll note
 * visualizer, the 48-slot voice pool, and a scrolling stereo waveform strip.
 *
 * Same standing as wavdump: a test harness over the pure core (includes internal.h
 * to peek voice/sequencer state read-only; the core itself stays engine-agnostic).
 * Song list, bank pairing and AUTHORED per-song volumes come from the game's own
 * mastering table (research/bgm_volume_scale.tsv -- trunc(127 x volume_scale) at
 * slider 1.0), so orphan sequences resolve to the bank the game's cue table names.
 *
 * Audio: SDL callback pulls ae3_synth_render under a mutex (the core renders ~460x
 * real time, so the lock is held microseconds per block). The GUI thread takes the
 * same lock briefly per frame to copy a snapshot (voices, stats, clock). Seeking
 * rebuilds the synth and fast-forwards headless -- audio drops out for the ~0.1 s
 * that takes; fine for a test tool.
 *
 * Keys: SPACE play/pause - ENTER play selected - UP/DOWN select - L loop -
 * T exact/tick clock - R reverb on/off - ,/. reverb depth - -/= song volume -
 * A authored volume - Z/X zoom - click list/timeline - ESC quit.
 */
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

/* ---- layout ------------------------------------------------------------- */
#define WIN_W     1280
#define WIN_H     800
#define SIDE_W    252
#define HDR_H     70
#define BAR_H     16
#define SLOT_H    42
#define WAVE_H    160
#define FOOT_H    44
#define ROLL_Y    (HDR_H + BAR_H)
#define ROLL_H    (WIN_H - ROLL_Y - SLOT_H - WAVE_H - FOOT_H)
#define SLOT_Y    (ROLL_Y + ROLL_H)
#define WAVE_Y    (SLOT_Y + SLOT_H)
#define FOOT_Y    (WAVE_Y + WAVE_H)
#define MAIN_X    SIDE_W
#define MAIN_W    (WIN_W - SIDE_W)

/* ---- 5x7 font (uppercase), authored as string art ----------------------- */
static const struct { char c; const char *r[7]; } FONTDEF[] = {
{'A',{" ### ","#   #","#   #","#####","#   #","#   #","#   #"}},
{'B',{"#### ","#   #","#   #","#### ","#   #","#   #","#### "}},
{'C',{" ### ","#   #","#    ","#    ","#    ","#   #"," ### "}},
{'D',{"#### ","#   #","#   #","#   #","#   #","#   #","#### "}},
{'E',{"#####","#    ","#    ","#### ","#    ","#    ","#####"}},
{'F',{"#####","#    ","#    ","#### ","#    ","#    ","#    "}},
{'G',{" ### ","#   #","#    ","# ###","#   #","#   #"," ### "}},
{'H',{"#   #","#   #","#   #","#####","#   #","#   #","#   #"}},
{'I',{" ### ","  #  ","  #  ","  #  ","  #  ","  #  "," ### "}},
{'J',{"  ###","   # ","   # ","   # ","   # ","#  # "," ##  "}},
{'K',{"#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"}},
{'L',{"#    ","#    ","#    ","#    ","#    ","#    ","#####"}},
{'M',{"#   #","## ##","# # #","# # #","#   #","#   #","#   #"}},
{'N',{"#   #","##  #","# # #","#  ##","#   #","#   #","#   #"}},
{'O',{" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}},
{'P',{"#### ","#   #","#   #","#### ","#    ","#    ","#    "}},
{'Q',{" ### ","#   #","#   #","#   #","# # #","#  # "," ## #"}},
{'R',{"#### ","#   #","#   #","#### ","# #  ","#  # ","#   #"}},
{'S',{" ####","#    ","#    "," ### ","    #","    #","#### "}},
{'T',{"#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}},
{'U',{"#   #","#   #","#   #","#   #","#   #","#   #"," ### "}},
{'V',{"#   #","#   #","#   #","#   #","#   #"," # # ","  #  "}},
{'W',{"#   #","#   #","#   #","# # #","# # #","## ##","#   #"}},
{'X',{"#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"}},
{'Y',{"#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "}},
{'Z',{"#####","    #","   # ","  #  "," #   ","#    ","#####"}},
{'0',{" ### ","#   #","#  ##","# # #","##  #","#   #"," ### "}},
{'1',{"  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### "}},
{'2',{" ### ","#   #","    #","   # ","  #  "," #   ","#####"}},
{'3',{" ### ","#   #","    #","  ## ","    #","#   #"," ### "}},
{'4',{"   # ","  ## "," # # ","#  # ","#####","   # ","   # "}},
{'5',{"#####","#    ","#### ","    #","    #","#   #"," ### "}},
{'6',{" ### ","#    ","#    ","#### ","#   #","#   #"," ### "}},
{'7',{"#####","    #","   # ","  #  "," #   "," #   "," #   "}},
{'8',{" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "}},
{'9',{" ### ","#   #","#   #"," ####","    #","    #"," ### "}},
{'.',{"     ","     ","     ","     ","     "," ##  "," ##  "}},
{':',{"     "," ##  "," ##  ","     "," ##  "," ##  ","     "}},
{'/',{"    #","    #","   # ","  #  "," #   ","#    ","#    "}},
{'-',{"     ","     ","     ","#####","     ","     ","     "}},
{'_',{"     ","     ","     ","     ","     ","     ","#####"}},
{'(',{"   # ","  #  "," #   "," #   "," #   ","  #  ","   # "}},
{')',{" #   ","  #  ","   # ","   # ","   # ","  #  "," #   "}},
{'[',{" ### "," #   "," #   "," #   "," #   "," #   "," ### "}},
{']',{" ### ","   # ","   # ","   # ","   # ","   # "," ### "}},
{'+',{"     ","  #  ","  #  ","#####","  #  ","  #  ","     "}},
{'=',{"     ","     ","#####","     ","#####","     ","     "}},
{'%',{"##   ","##  #","   # ","  #  "," #   ","#  ##","   ##"}},
{'#',{" # # "," # # ","#####"," # # ","#####"," # # "," # # "}},
{'!',{"  #  ","  #  ","  #  ","  #  ","  #  ","     ","  #  "}},
{'?',{" ### ","#   #","    #","   # ","  #  ","     ","  #  "}},
{',',{"     ","     ","     ","     "," ##  "," ##  "," #   "}},
{'\'',{"  #  ","  #  ","     ","     ","     ","     ","     "}},
{'<',{"   # ","  #  "," #   ","#    "," #   ","  #  ","   # "}},
{'>',{" #   ","  #  ","   # ","    #","   # ","  #  "," #   "}},
{'*',{"     ","# # #"," ### ","#####"," ### ","# # #","     "}},
};
static uint8_t Gbits[128][7];

static void font_init(void)
{
    for (size_t i = 0; i < sizeof FONTDEF / sizeof *FONTDEF; i++)
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 5; x++)
                if (FONTDEF[i].r[y][x] == '#')
                    Gbits[(int)FONTDEF[i].c][y] |= (uint8_t)(1u << (4 - x));
}

static SDL_Renderer *R;

static void dtextv(int x, int y, int sc, SDL_Color c, const char *s)
{
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    for (; *s; s++, x += 6 * sc) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 0 || ch > 127) continue;
        for (int gy = 0; gy < 7; gy++) {
            uint8_t row = Gbits[ch][gy];
            for (int gx = 0; gx < 5; gx++)
                if (row & (1u << (4 - gx))) {
                    SDL_Rect px = { x + gx * sc, y + gy * sc, sc, sc };
                    SDL_RenderFillRect(R, &px);
                }
        }
    }
}

static void dtext(int x, int y, int sc, SDL_Color c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    dtextv(x, y, sc, c, buf);
}

/* ---- song table (from the game's mastering TSV) ------------------------- */
typedef struct {
    char name[64];              /* midi basename without .mid */
    char mid[64], hd[64], bd[64];
    int  songvol;               /* authored: trunc(127 x volume_scale), slider 1.0 */
} song_t;

static song_t *Songs;
static int NSongs;

/* natural order: digit runs compare numerically, so s_9 < s_10 and p_2 < p_10
 * (plain strcmp scattered them: '0' < '_' put p_10 before p_1_retake) */
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long va = strtol(a, (char **)&a, 10);
            long vb = strtol(b, (char **)&b, 10);
            if (va != vb) return va < vb ? -1 : 1;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++; b++;
        }
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int song_cmp(const void *a, const void *b)
{
    return natcmp(((const song_t *)a)->name, ((const song_t *)b)->name);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int load_song_table(const char *tsv_path)
{
    size_t n;
    char *txt = (char *)read_file(tsv_path, &n);
    if (!txt) return -1;
    txt = realloc(txt, n + 1);
    txt[n] = 0;
    Songs = calloc(128, sizeof *Songs);
    for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#') continue;
        char *col[8] = {0};
        int nc = 0;
        for (char *p = line; p && nc < 8; ) {
            col[nc++] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (nc < 7) continue;
        size_t ml = strlen(col[1]);
        if (ml < 5 || ml >= 64 || strcmp(col[1] + ml - 4, ".mid") != 0) continue;
        int dup = 0;
        for (int i = 0; i < NSongs; i++)
            if (!strcmp(Songs[i].mid, col[1])) { dup = 1; break; }
        if (dup || NSongs >= 128) continue;
        song_t *s = &Songs[NSongs++];
        snprintf(s->mid, sizeof s->mid, "%s", col[1]);
        snprintf(s->hd,  sizeof s->hd,  "%s", col[2]);
        snprintf(s->bd,  sizeof s->bd,  "%s", col[3]);
        snprintf(s->name, sizeof s->name, "%.*s", (int)(ml - 4), col[1]);
        s->songvol = atoi(col[6]);
        if (s->songvol < 1 || s->songvol > 127) s->songvol = 44;
    }
    free(txt);
    qsort(Songs, NSongs, sizeof *Songs, song_cmp);
    return NSongs > 0 ? 0 : -1;
}

/* ---- note timeline (built from the parsed sequence at load) ------------- */
typedef struct { double t0, t1; uint8_t ch, key, vel; } vnote_t;
typedef struct { double tick, sample, spt; } tseg_t;

/* ---- waveform history: one min/max column per 60 Hz tick ---------------- */
typedef struct { float lmin, lmax, rmin, rmax; uint8_t clip; } wcol_t;
#define WCOL_SAMPLES AE3_TICK_SAMPLES
#define WCOL_N 4096

/* ---- voice snapshot ----------------------------------------------------- */
typedef struct {
    uint8_t in_use, active, released, ch, key;
    int32_t env;
} vsnap_t;

/* ---- player state (audio-callback side guarded by mx) ------------------- */
static struct {
    SDL_mutex *mx;
    ae3_synth *s;
    int  playing;               /* callback renders only when set */
    int  finished;
    /* current song's raw files, kept for fast reload/seek */
    uint8_t *hd, *bd, *mid; size_t hn, bn, mn;
    uint8_t *irx, *libsd; size_t irxn, libsdn;
    /* config mirrored into the synth */
    int songvol, authored, loop_on, exact, rev_depth, gaussian;
    /* snapshot written each callback */
    uint64_t pos;
    vsnap_t  vs[AE3_NVOICES];
    ae3_stats st;
    uint64_t seg_tick, tick_offset;
    double   seg_sample, spt;
    float    peak_l, peak_r;
    wcol_t   wcol[WCOL_N];
    int      whead;
    wcol_t   cur; int cur_n;
    /* timeline, rebuilt at song load (read-only while playing) */
    vnote_t *notes; int nnotes;
    tseg_t  *tmap;  int ntmap;
    double   len_samp, loop_s0, loop_s1;
    int      key_lo, key_hi;
    uint16_t ppqn;
    char     err[256];
    /* WAV export subprocess (wavdump), run on an SDL thread */
    SDL_atomic_t exporting;     /* 1 while the thread runs */
    char     export_msg[300];
} P;

static char BaseDir[1024];      /* executable's dir (where wavdump lives) */
static char Root[1024];         /* repo root, trailing slash */

typedef struct { char *cmd; char ok_msg[300]; } export_job;

static int export_thread(void *arg)
{
    export_job *j = arg;
    int rc = system(j->cmd);
    if (rc != 0)
        snprintf(P.export_msg, sizeof P.export_msg, "EXPORT FAILED (RC %d)", rc);
    else
        snprintf(P.export_msg, sizeof P.export_msg, "%s", j->ok_msg);
    free(j->cmd);
    free(j);
    SDL_AtomicSet(&P.exporting, 0);
    return 0;
}

/* Export a song through wavdump -- the exact pipeline the listening set under
 * extracted/synth/ was rendered with. songvol/rev/exact parameterized so the
 * dropdown can offer current settings, the authored originals, or a looped
 * render (wavdump --loop N: the loop body plays N times total). */
static void export_wav(int idx, int songvol, int rev_depth, int exact,
                       int bright, int loopn, const char *suffix)
{
    if (idx < 0 || idx >= NSongs || SDL_AtomicGet(&P.exporting)) return;
    char wavdump[1100];
    snprintf(wavdump, sizeof wavdump, "%swavdump", BaseDir);
    if (access(wavdump, X_OK) != 0) {
        snprintf(P.export_msg, sizeof P.export_msg,
                 "NO WAVDUMP BINARY - RUN MAKE FIRST");
        return;
    }
    const char *B = "extracted/databin/debug/us/sound/bgm";
    char cmd[4096];
    int n = snprintf(cmd, sizeof cmd,
        "mkdir -p \"%sextracted/synth/gui\" && \"%s\" \"%s%s/%s\" \"%s%s/%s\" \"%s%s/%s\"",
        Root, wavdump, Root, B, Songs[idx].hd, Root, B, Songs[idx].bd,
        Root, B, Songs[idx].mid);
    if (P.irx)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n,
                      " \"%sextracted/irx/sg2iopm1.irx\"", Root);
    if (P.libsd && rev_depth > 0)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n,
                      " --libsd \"%sextracted/irx/libsd.irx\" --rev-depth %d",
                      Root, rev_depth);
    if (!exact)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --tick-events");
    if (bright)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --bright");
    if (loopn >= 2)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --loop %d", loopn);
    snprintf(cmd + n, sizeof cmd - (size_t)n,
             " --songvol %d -o \"%sextracted/synth/gui/%s%s.wav\"",
             songvol, Root, Songs[idx].name, suffix);
    snprintf(P.export_msg, sizeof P.export_msg,
             "EXPORTING %s%s.WAV (VOL %d%s)...",
             Songs[idx].name, suffix, songvol, loopn >= 2 ? " LOOPED" : "");
    export_job *j = malloc(sizeof *j);
    j->cmd = strdup(cmd);
    snprintf(j->ok_msg, sizeof j->ok_msg, "EXPORTED EXTRACTED/SYNTH/GUI/%s%s.WAV",
             Songs[idx].name, suffix);
    SDL_AtomicSet(&P.exporting, 1);
    SDL_Thread *t = SDL_CreateThread(export_thread, "export", j);
    if (!t) {
        SDL_AtomicSet(&P.exporting, 0);
        free(j->cmd); free(j);
        snprintf(P.export_msg, sizeof P.export_msg, "EXPORT THREAD FAILED");
        return;
    }
    SDL_DetachThread(t);
}

/* the dropdown's three actions */
static int DropOpen, LoopN = 2, HelpOpen;

/* ---- help overlay: the console's quirks, from the reverse-engineering notes
 * (research/SYNTH_HANDOFF.md, decomp/functions_bgm/) -------------------------- */
static const struct { int head; const char *s; } HELP[] = {
{1, "WHAT THIS IS"},
{0, "A native re-implementation of the game's PS2 sound driver. It performs streaming ADPCM"},
{0, "decode, live SPU2 envelopes, gaussian resampling, the driver's integer volume and pan math,"},
{0, "and the hardware reverb network. Everything is driven by the game's own data (banks,"},
{0, "sequences, and tables read from the game's IRX modules at runtime) and renders on the"},
{0, "SPU2's 48000 HZ clock."},
{1, "AUTHORED VOLUME  (VOL NN AUTH, KEYS - = A)"},
{0, "The sound driver boots with song volume 127, but the game never plays music that loud."},
{0, "Every music cue computes SONGVOL = TRUNC(127 X VOLUME_SCALE X OPTIONS_SLIDER X DOLBY),"},
{0, "where VOLUME_SCALE is a per-song value between 0.28 and 0.56 stored in the game's own BGM"},
{0, "database. Sony mastered each song individually. At 127, dense songs overdrive the 16-bit"},
{0, "mix bus; at authored levels only occasional transient peaks clip, and the real console"},
{0, "clips those too. Use - and = to override, and A to return to the authored value."},
{1, "EXACT VS TICK CLOCK  (KEY T)"},
{0, "The console's sequencer runs as a callback of the 60 HZ sound thread. Every MIDI event due"},
{0, "within a tick fires in one burst, so event timing quantizes to a 16.7 MS grid. TICK"},
{0, "reproduces that, and it reads as notes arriving late, like an emulator with slowdown."},
{0, "EXACT fires events at their exact sample positions and is the default. Use TICK when"},
{0, "comparing against a recording of the real game, since the console bakes that timing into"},
{0, "any capture."},
{1, "SLOTS  (THE 48-CELL GRID)"},
{0, "The SPU2 has 48 hardware voices, shared by music and sound effects. The driver grants them"},
{0, "round-robin and never steals a playing voice. If all 48 are busy, the note is simply"},
{0, "dropped. Voices free up only at the 60 HZ tick, once their envelope has fallen below 2."},
{0, "This game's music peaks at 31 of 48 voices (33 on the tick clock), so it never actually"},
{0, "drops a note."},
{1, "LOOPING  (KEY L)"},
{0, "CC99 values 20 and 30 in the MIDI mark the loop start and end. They are consumed by the"},
{0, "game's sequence walker and never reach the sound driver. The console loops music forever."},
{0, "64 songs carry exactly one marker pair, and the four Genie Dancer jingles play once by"},
{0, "design."},
{0, "The export menu can bounce the loop body any number of times."},
{1, "REVERB  (KEYS R , .)"},
{0, "The SPU2's hardware reverb, preset STUDIO_C at depth 30, is set once at boot and never"},
{0, "touched again by the game. Only tones flagged for reverb feed the wet bus, and they stay"},
{0, "in the dry mix as well, since it works as a send. The network runs at 24 KHZ behind the"},
{0, "hardware's 39-tap half-band filter. The depth knob here is purely for A/B listening; the"},
{0, "game never moves it."},
{1, "GAUSS VS BRIGHT  (KEY G)"},
{0, "The SPU2 resamples every voice with a 4-tap gaussian kernel, which is why the console"},
{0, "sounds darker than a modern resampler. It also lets notes struck below their root pitch"},
{0, "alias audibly, and that grit is part of the real console sound. BRIGHT swaps in a"},
{0, "catmull-rom kernel with cleaner treble, which is not what the hardware does. The authored"},
{0, "export always uses gaussian."},
{1, "BUS SATURATION  (RED WAVEFORM COLUMNS)"},
{0, "Voices sum on a saturating 16-bit bus, exactly like the chip, so dense passages can"},
{0, "genuinely crunch on real hardware. At authored volumes it amounts to rare transient"},
{0, "clipping. The CLIP counter and red waveform columns show every saturated sample."},
{1, "ODD BUT REAL"},
{0, "Velocity is linear; the banks carry an identity velocity curve, so there is no squared"},
{0, "taper. The pan table is not constant power, so a centred tone is about 2.5 DB louder than"},
{0, "a hard-panned one. Both come straight from the driver's own math and are kept, not fixed."},
};

static void export_current(int idx)    /* what you hear (minus the looping) */
{
    export_wav(idx, P.songvol, P.rev_depth, P.exact, !P.gaussian, 0, "");
}

static void export_original(int idx)   /* the curated listening-set render:
                                          authored vol, rev 30, exact, gaussian */
{
    if (idx < 0 || idx >= NSongs) return;
    export_wav(idx, Songs[idx].songvol, 30, 1, 0, 0, "_authored");
}

static void export_looped(int idx)     /* current settings, loop body x N */
{
    char suf[32];
    snprintf(suf, sizeof suf, "_loop%d", LoopN);
    export_wav(idx, P.songvol, P.rev_depth, P.exact, !P.gaussian, LoopN, suf);
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    SDL_LockMutex(P.mx);
    if (P.s && P.playing) {
        int got = ae3_synth_render(P.s, out, frames);
        if (got < 0) got = 0;
        float pl = 0, pr = 0;
        for (int i = 0; i < got; i++) {
            float l = out[2 * i], r = out[2 * i + 1];
            wcol_t *c = &P.cur;
            if (P.cur_n == 0) { c->lmin = c->lmax = l; c->rmin = c->rmax = r; c->clip = 0; }
            else {
                if (l < c->lmin) c->lmin = l; if (l > c->lmax) c->lmax = l;
                if (r < c->rmin) c->rmin = r; if (r > c->rmax) c->rmax = r;
            }
            if (fabsf(l) >= 0.9999f || fabsf(r) >= 0.9999f) c->clip = 1;
            if (++P.cur_n >= WCOL_SAMPLES) {
                P.wcol[P.whead] = *c;
                P.whead = (P.whead + 1) % WCOL_N;
                P.cur_n = 0;
            }
            float al = fabsf(l), ar = fabsf(r);
            if (al > pl) pl = al;
            if (ar > pr) pr = ar;
        }
        P.peak_l = fmaxf(pl, P.peak_l * 0.86f);
        P.peak_r = fmaxf(pr, P.peak_r * 0.86f);
        if (ae3_synth_done(P.s)) { P.playing = 0; P.finished = 1; }
        for (int i = 0; i < AE3_NVOICES; i++) {
            const ae3_voice *v = &P.s->voices[i];
            P.vs[i] = (vsnap_t){ v->in_use, v->active, v->released,
                                 v->ch, v->key, v->env.level };
        }
        ae3_synth_get_stats(P.s, &P.st);
        P.pos = P.s->pos;
        P.seg_tick = P.s->seq.seg_tick;
        P.seg_sample = P.s->seq.seg_sample;
        P.spt = P.s->seq.spt;
        P.tick_offset = P.s->tick_offset;
    }
    SDL_UnlockMutex(P.mx);
}

/* mx held. Fresh instance from the cached buffers; all config re-applied. */
static int reload_locked(void)
{
    if (P.s) ae3_synth_free(P.s);
    P.s = ae3_synth_new();
    P.err[0] = 0;
    if (P.irx && ae3_synth_load_pitch_irx(P.s, P.irx, P.irxn))
        snprintf(P.err, sizeof P.err, "pitch irx: %s", ae3_synth_error(P.s));
    if (ae3_synth_load_bank(P.s, P.hd, P.hn, P.bd, P.bn) ||
        ae3_synth_load_seq(P.s, P.mid, P.mn)) {
        snprintf(P.err, sizeof P.err, "%s", ae3_synth_error(P.s));
        return -1;
    }
    if (P.libsd && ae3_synth_load_reverb_irx(P.s, P.libsd, P.libsdn))
        snprintf(P.err, sizeof P.err, "libsd: %s", ae3_synth_error(P.s));
    ae3_synth_reverb_depth(P.s, P.rev_depth);
    ae3_synth_event_timing(P.s, P.exact != 0);
    ae3_synth_gaussian(P.s, P.gaussian != 0);
    ae3_synth_set_loop(P.s, P.loop_on ? AE3_LOOP_FOREVER : 0);
    ae3_synth_song_volume(P.s, P.songvol, P.songvol);
    P.finished = 0;
    P.pos = 0;
    P.seg_tick = 0; P.seg_sample = 0; P.spt = 0; P.tick_offset = 0;
    memset(P.vs, 0, sizeof P.vs);
    memset(&P.st, 0, sizeof P.st);
    return 0;
}

/* mx held. Single-pass tempo map + note spans from the freshly parsed sequence. */
static void build_timeline_locked(void)
{
    free(P.notes); P.notes = NULL; P.nnotes = 0;
    free(P.tmap);  P.tmap = NULL;  P.ntmap = 0;
    P.len_samp = 0; P.loop_s0 = P.loop_s1 = -1;
    P.key_lo = 127; P.key_hi = 0;
    if (!P.s || !P.s->have_seq) return;

    const ae3_seq *q = &P.s->seq;
    P.notes = malloc((size_t)(q->nev + 1) * sizeof *P.notes);
    P.tmap  = malloc((size_t)(q->nev + 2) * sizeof *P.tmap);
    double spt = (double)AE3_RATE * 500000.0 / (1e6 * q->ppqn);
    double seg_s = 0; uint32_t seg_t = 0;
    P.tmap[P.ntmap++] = (tseg_t){ 0, 0, spt };
    int *open = malloc((size_t)q->nev * sizeof *open);
    int nopen = 0;
    double last = 0;

    for (int i = 0; i < q->nev; i++) {
        const ae3_event *e = &q->ev[i];
        double ts = seg_s + (double)(e->tick - seg_t) * spt;
        if (ts > last) last = ts;
        switch (e->kind) {
        case AE3_EV_TEMPO:
            seg_s = ts; seg_t = e->tick;
            spt = (double)AE3_RATE * (double)e->uspqn / (1e6 * q->ppqn);
            P.tmap[P.ntmap++] = (tseg_t){ (double)e->tick, ts, spt };
            break;
        case AE3_EV_LOOP_START: P.loop_s0 = ts; break;
        case AE3_EV_LOOP_END:   P.loop_s1 = ts; break;
        case AE3_EV_END:        P.len_samp = ts; break;
        case AE3_EV_CH: {
            uint8_t hi = e->status & 0xF0, ch = e->status & 0x0F;
            if (hi == 0x90 && e->b > 0) {
                P.notes[P.nnotes] = (vnote_t){ ts, -1, ch, e->a, e->b };
                open[nopen++] = P.nnotes++;
                if (e->a < P.key_lo) P.key_lo = e->a;
                if (e->a > P.key_hi) P.key_hi = e->a;
            } else if (hi == 0x80 || (hi == 0x90 && e->b == 0)) {
                for (int k = 0; k < nopen; k++) {   /* oldest matching first */
                    vnote_t *v = &P.notes[open[k]];
                    if (v->ch == ch && v->key == e->a) {
                        v->t1 = ts;
                        memmove(open + k, open + k + 1,
                                (size_t)(nopen - k - 1) * sizeof *open);
                        nopen--;
                        break;
                    }
                }
            }
            break;
        }
        default: break;
        }
    }
    if (P.len_samp <= 0) P.len_samp = last;
    for (int k = 0; k < nopen; k++)
        P.notes[open[k]].t1 = P.len_samp;
    free(open);
    if (P.key_lo > P.key_hi) { P.key_lo = 48; P.key_hi = 72; }
}

/* single-pass sample position of an ORIGINAL tick, via the timeline tempo map */
static double tmap_sample(double otick)
{
    if (P.ntmap == 0) return 0;
    int i = P.ntmap - 1;
    while (i > 0 && P.tmap[i].tick > otick) i--;
    return P.tmap[i].sample + (otick - P.tmap[i].tick) * P.tmap[i].spt;
}

/* map the absolute render position back onto the single-pass timeline (loops
 * unwound through the sequencer's own effective-tick clock) */
static double display_pos(uint64_t pos, uint64_t seg_tick, double seg_sample,
                          double spt, uint64_t tick_offset)
{
    if (tick_offset == 0 || spt <= 0) return (double)pos;
    double eff = (double)seg_tick + ((double)pos - seg_sample) / spt;
    return tmap_sample(eff - (double)tick_offset);
}

static int Sel, PlayingIdx = -1;

static int load_song(int idx)
{
    if (idx < 0 || idx >= NSongs) return -1;
    char path[1200];
    uint8_t *hd, *bd, *mid; size_t hn, bn, mn;
    snprintf(path, sizeof path, "%sextracted/databin/debug/us/sound/bgm/%s",
             Root, Songs[idx].hd);
    hd = read_file(path, &hn);
    snprintf(path, sizeof path, "%sextracted/databin/debug/us/sound/bgm/%s",
             Root, Songs[idx].bd);
    bd = read_file(path, &bn);
    snprintf(path, sizeof path, "%sextracted/databin/debug/us/sound/bgm/%s",
             Root, Songs[idx].mid);
    mid = read_file(path, &mn);
    if (!hd || !bd || !mid) {
        free(hd); free(bd); free(mid);
        snprintf(P.err, sizeof P.err, "missing files for %s", Songs[idx].name);
        return -1;
    }
    SDL_LockMutex(P.mx);
    free(P.hd); free(P.bd); free(P.mid);
    P.hd = hd; P.hn = hn; P.bd = bd; P.bn = bn; P.mid = mid; P.mn = mn;
    if (P.authored) P.songvol = Songs[idx].songvol;
    int rc = reload_locked();
    build_timeline_locked();
    if (rc == 0) {
        P.ppqn = P.s->seq.ppqn;
        for (int i = 0; i < P.ntmap; i++)     /* initial tempo, for BPM display */
            if (P.tmap[i].tick <= 0) P.spt = P.tmap[i].spt;
    }
    memset(P.wcol, 0, sizeof P.wcol);
    P.whead = 0; P.cur_n = 0;
    P.playing = (rc == 0);
    SDL_UnlockMutex(P.mx);
    if (rc == 0) PlayingIdx = idx;
    return rc;
}

/* restart from sample 0 and fast-forward headless to target (timeline samples) */
static void seek_to(double target)
{
    if (!P.s || !P.hd) return;
    if (target < 0) target = 0;
    if (target > P.len_samp - 1) target = P.len_samp - 1;
    SDL_LockMutex(P.mx);
    int was = P.playing || P.finished;
    if (reload_locked() == 0) {
        float buf[1024 * 2];
        while ((double)P.s->pos < target)
            if (ae3_synth_render(P.s, buf, 1024) <= 0) break;
        P.pos = P.s->pos;
        P.seg_tick = P.s->seq.seg_tick;
        P.seg_sample = P.s->seq.seg_sample;
        P.spt = P.s->seq.spt;
        P.tick_offset = P.s->tick_offset;
        P.playing = was;
    }
    SDL_UnlockMutex(P.mx);
}

/* ---- drawing ------------------------------------------------------------ */
static const SDL_Color C_BG     = {16, 17, 22, 255};
static const SDL_Color C_PANEL  = {24, 26, 33, 255};
static const SDL_Color C_TEXT   = {214, 218, 228, 255};
static const SDL_Color C_DIM    = {120, 126, 140, 255};
static const SDL_Color C_ACCENT = {255, 196, 64, 255};
static const SDL_Color C_GOOD   = {110, 220, 130, 255};
static const SDL_Color C_BAD    = {240, 90, 80, 255};

static SDL_Color ch_color(int ch)
{
    /* 16 hues, fixed s/v */
    float h = (float)ch * 22.5f, s = 0.72f, v = 1.0f;
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    int hh = (int)(h / 60);
    switch (hh) {
    case 0: r = c; g = x; break;  case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;  case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;  default: r = c; b = x; break;
    }
    return (SDL_Color){ (Uint8)((r + m) * 255), (Uint8)((g + m) * 255),
                        (Uint8)((b + m) * 255), 255 };
}

static void fill(int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_Rect rc = { x, y, w, h };
    SDL_RenderFillRect(R, &rc);
}

static int ListScroll, ListFollow = -1;   /* row to scroll into view, -1 = none */
#define LIST_Y   36
#define LIST_ROW 11

static void draw_sidebar(void)
{
    fill(0, 0, SIDE_W, WIN_H, C_PANEL);
    dtext(10, 8, 2, C_ACCENT, "AE3 BGM SYNTH");
    /* the footer only covers the main area, so the list gets the full height
     * (68 songs fit exactly; the clamp keeps scrolling sane for bigger sets) */
    int rows_vis = (WIN_H - LIST_Y) / LIST_ROW;
    if (ListFollow >= 0) {
        if (ListFollow < ListScroll) ListScroll = ListFollow;
        if (ListFollow >= ListScroll + rows_vis)
            ListScroll = ListFollow - rows_vis + 1;
        ListFollow = -1;
    }
    if (ListScroll > NSongs - rows_vis) ListScroll = NSongs - rows_vis;
    if (ListScroll < 0) ListScroll = 0;
    for (int i = ListScroll; i < NSongs && i - ListScroll < rows_vis; i++) {
        int y = LIST_Y + (i - ListScroll) * LIST_ROW;
        if (i == Sel) fill(0, y - 1, SIDE_W, LIST_ROW, (SDL_Color){52, 58, 74, 255});
        SDL_Color c = (i == PlayingIdx) ? C_GOOD : (i == Sel ? C_TEXT : C_DIM);
        dtext(18, y, 1, c, "%s", Songs[i].name);
        if (i == PlayingIdx) dtext(6, y, 1, C_GOOD, ">");
    }
}

int main(int argc, char **argv)
{
    int selftest = 0, secs = 5, autoplay = 0;
    const char *want = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--play")) autoplay = 1;
        else if (!strcmp(argv[i], "--drop")) DropOpen = 1;   /* screenshot/debug */
        else if (!strcmp(argv[i], "--helpshow")) HelpOpen = 1;   /* screenshot/debug */
        else if (!strcmp(argv[i], "--song") && i + 1 < argc) want = argv[++i];
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
        else snprintf(Root, sizeof Root, "%s/", argv[i]);
    }

    font_init();
    if (SDL_Init(selftest ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO)) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    {
        char *base = SDL_GetBasePath();
        snprintf(BaseDir, sizeof BaseDir, "%s", base ? base : "./");
        if (!Root[0])
            snprintf(Root, sizeof Root, "%s../", BaseDir);
        SDL_free(base);
    }

    char path[1200];
    snprintf(path, sizeof path, "%sresearch/bgm_volume_scale.tsv", Root);
    if (load_song_table(path)) {
        fprintf(stderr, "cannot read song table %s (pass the repo root as arg)\n", path);
        return 1;
    }
    snprintf(path, sizeof path, "%sextracted/irx/sg2iopm1.irx", Root);
    P.irx = read_file(path, &P.irxn);
    snprintf(path, sizeof path, "%sextracted/irx/libsd.irx", Root);
    P.libsd = read_file(path, &P.libsdn);

    P.mx = SDL_CreateMutex();
    P.authored = 1;
    P.loop_on = 1;
    P.exact = 1;
    P.rev_depth = 30;
    P.songvol = 44;
    P.gaussian = 1;             /* the hardware kernel; G toggles bright */

    if (want)
        for (int i = 0; i < NSongs; i++)
            if (!strcmp(Songs[i].name, want)) { Sel = i; break; }

    if (selftest) {
        printf("songs=%d pitch_irx=%s libsd=%s\n", NSongs,
               P.irx ? "ok" : "MISSING", P.libsd ? "ok" : "MISSING");
        if (load_song(Sel)) { fprintf(stderr, "load: %s\n", P.err); return 1; }
        printf("loaded %s: notes=%d tmap=%d len=%.1fs keys=%d..%d loop=%.1f..%.1fs\n",
               Songs[Sel].name, P.nnotes, P.ntmap, P.len_samp / AE3_RATE,
               P.key_lo, P.key_hi, P.loop_s0 / AE3_RATE, P.loop_s1 / AE3_RATE);
        float *buf = malloc(sizeof(float) * 2 * 1024);
        for (int i = 0; i < secs * 48000 / 1024; i++)
            audio_cb(NULL, (Uint8 *)buf, 1024 * 2 * (int)sizeof(float));
        int used = 0;
        for (int i = 0; i < AE3_NVOICES; i++) used += P.vs[i].in_use;
        double dp = display_pos(P.pos, P.seg_tick, P.seg_sample, P.spt, P.tick_offset);
        printf("rendered %ds: pos=%llu disp=%.2f voices=%d peak=%u started=%u "
               "buspeak=%u wcols=%d peakL=%.3f\n",
               secs, (unsigned long long)P.pos, dp, used, P.st.peak_voices,
               P.st.voices_started, P.st.bus_peak, P.whead, (double)P.peak_l);
        printf("dbg: loop_s0=%.4f loop_s1=%.4f tick_offset=%llu loops=%u "
               "seg_tick=%llu seg_sample=%.4f spt=%.10f\n",
               P.loop_s0, P.loop_s1, (unsigned long long)P.tick_offset,
               P.st.loops_taken, (unsigned long long)P.seg_tick,
               P.seg_sample, P.spt);
        for (int i = 0; i < P.ntmap; i++)
            printf("dbg: tmap[%d] tick=%.0f sample=%.4f spt=%.10f\n",
                   i, P.tmap[i].tick, P.tmap[i].sample, P.tmap[i].spt);
        /* bright-kernel A/B: same seconds, different resampler -> must differ */
        P.gaussian = 0;
        load_song(Sel);
        for (int i = 0; i < secs * 48000 / 1024; i++)
            audio_cb(NULL, (Uint8 *)buf, 1024 * 2 * (int)sizeof(float));
        printf("bright : buspeak=%u peakL=%.3f (gaussian run above must differ)\n",
               P.st.bus_peak, (double)P.peak_l);
        P.gaussian = 1;
        load_song(Sel);
        export_current(Sel);
        while (SDL_AtomicGet(&P.exporting)) SDL_Delay(50);
        printf("export: %s\n", P.export_msg);
        LoopN = 2;
        export_looped(Sel);
        while (SDL_AtomicGet(&P.exporting)) SDL_Delay(50);
        printf("export: %s\n", P.export_msg);
        export_original(Sel);
        while (SDL_AtomicGet(&P.exporting)) SDL_Delay(50);
        printf("export: %s\n", P.export_msg);
        return 0;
    }

    SDL_Window *win = SDL_CreateWindow("AE3 BGM synth",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    R = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(R, WIN_W, WIN_H);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);

    SDL_AudioSpec want_spec = {0}, have;
    want_spec.freq = AE3_RATE;
    want_spec.format = AUDIO_F32SYS;
    want_spec.channels = 2;
    want_spec.samples = 1024;
    want_spec.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want_spec, &have, 0);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return 1; }
    SDL_PauseAudioDevice(dev, 0);

    load_song(Sel);
    if (!autoplay) {   /* default: load ready but paused, no surprise audio */
        SDL_LockMutex(P.mx); P.playing = 0; SDL_UnlockMutex(P.mx);
    }
    PlayingIdx = Sel;
    ListFollow = Sel;

    double zoom_s = 12.0;       /* piano-roll window, seconds */
    uint32_t clip_flash = 0, prev_clip = 0;
    int running = 1;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                SDL_LockMutex(P.mx);
                switch (k) {
                case SDLK_ESCAPE:
                    if (HelpOpen) HelpOpen = 0;
                    else if (DropOpen) DropOpen = 0;
                    else running = 0;
                    break;
                case SDLK_h: HelpOpen = !HelpOpen; break;
                case SDLK_g:
                    P.gaussian = !P.gaussian;
                    if (P.s) ae3_synth_gaussian(P.s, P.gaussian != 0);
                    break;
                case SDLK_SPACE:
                    if (P.finished) { SDL_UnlockMutex(P.mx); load_song(PlayingIdx);
                                      SDL_LockMutex(P.mx); }
                    else P.playing = !P.playing;
                    break;
                case SDLK_UP:   if (Sel > 0) Sel--; ListFollow = Sel; break;
                case SDLK_DOWN: if (Sel < NSongs - 1) Sel++; ListFollow = Sel; break;
                case SDLK_RETURN:
                    SDL_UnlockMutex(P.mx); load_song(Sel); SDL_LockMutex(P.mx);
                    break;
                case SDLK_l:
                    P.loop_on = !P.loop_on;
                    if (P.s) ae3_synth_set_loop(P.s, P.loop_on ? AE3_LOOP_FOREVER : 0);
                    break;
                case SDLK_t: {
                    P.exact = !P.exact;
                    double dp = display_pos(P.pos, P.seg_tick, P.seg_sample,
                                            P.spt, P.tick_offset);
                    SDL_UnlockMutex(P.mx); seek_to(dp); SDL_LockMutex(P.mx);
                    break;
                }
                case SDLK_r:
                    P.rev_depth = P.rev_depth ? 0 : 30;
                    if (P.s) ae3_synth_reverb_depth(P.s, P.rev_depth);
                    break;
                case SDLK_COMMA:
                    if (P.rev_depth > 0) P.rev_depth -= 2;
                    if (P.s) ae3_synth_reverb_depth(P.s, P.rev_depth);
                    break;
                case SDLK_PERIOD:
                    if (P.rev_depth < 127) P.rev_depth += 2;
                    if (P.s) ae3_synth_reverb_depth(P.s, P.rev_depth);
                    break;
                case SDLK_MINUS:
                    if (P.songvol > 1) P.songvol--;
                    P.authored = 0;
                    if (P.s) ae3_synth_song_volume(P.s, P.songvol, P.songvol);
                    break;
                case SDLK_EQUALS:
                    if (P.songvol < 127) P.songvol++;
                    P.authored = 0;
                    if (P.s) ae3_synth_song_volume(P.s, P.songvol, P.songvol);
                    break;
                case SDLK_a:
                    P.authored = 1;
                    if (PlayingIdx >= 0) P.songvol = Songs[PlayingIdx].songvol;
                    if (P.s) ae3_synth_song_volume(P.s, P.songvol, P.songvol);
                    break;
                case SDLK_z: zoom_s = fmax(3.0,  zoom_s * 0.75); break;
                case SDLK_x: zoom_s = fmin(48.0, zoom_s / 0.75); break;
                case SDLK_e: export_current(PlayingIdx); break;
                default: break;
                }
                SDL_UnlockMutex(P.mx);
            } else if (e.type == SDL_MOUSEWHEEL) {
                /* SDL_GetMouseState is raw window coords; the renderer's logical
                 * scaling only converts event coords, so map by hand. macOS
                 * already bakes natural-scroll direction into the delta -- do
                 * NOT re-flip on SDL_MOUSEWHEEL_FLIPPED (that read inverted). */
                int mx, my;
                float lx, ly;
                SDL_GetMouseState(&mx, &my);
                SDL_RenderWindowToLogical(R, mx, my, &lx, &ly);
                static float acc;
                acc += e.wheel.preciseY * 3.0f;
                int rows = (int)acc;
                if (rows) {
                    acc -= (float)rows;
                    if (lx < SIDE_W) ListScroll -= rows;
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = e.button.x, my = e.button.y;
                int dx0 = WIN_W - 238;      /* dropdown panel */
                if (HelpOpen) {
                    HelpOpen = 0;
                } else if (DropOpen) {
                    if (mx >= dx0 && mx < WIN_W - 8 && my >= 30 && my < 96) {
                        int row = (my - 30) / 22;
                        if (row == 0) { export_current(PlayingIdx); DropOpen = 0; }
                        else if (row == 1) { export_original(PlayingIdx); DropOpen = 0; }
                        else if (mx >= dx0 + 152 && mx < dx0 + 176) {
                            if (LoopN > 2) LoopN--;
                        } else if (mx >= dx0 + 180 && mx < dx0 + 204) {
                            if (LoopN < 99) LoopN++;
                        } else { export_looped(PlayingIdx); DropOpen = 0; }
                    } else if (mx >= WIN_W - 168 && my < 30) {
                        DropOpen = 0;       /* button toggles closed */
                    } else {
                        DropOpen = 0;       /* click-away closes */
                    }
                } else if (mx < SIDE_W && my >= LIST_Y) {
                    int idx = ListScroll + (my - LIST_Y) / LIST_ROW;
                    if (idx >= 0 && idx < NSongs) { Sel = idx; load_song(idx); }
                } else if (mx >= WIN_W - 168 && my < 30) {
                    DropOpen = 1;
                } else if (mx >= MAIN_X && my >= HDR_H && my < HDR_H + BAR_H) {
                    double frac = (double)(mx - MAIN_X - 8) / (MAIN_W - 16);
                    seek_to(frac * P.len_samp);
                }
            }
        }

        /* --- snapshot under the lock --- */
        SDL_LockMutex(P.mx);
        uint64_t pos = P.pos, seg_t = P.seg_tick, toff = P.tick_offset;
        double seg_s = P.seg_sample, spt = P.spt;
        vsnap_t vs[AE3_NVOICES];
        memcpy(vs, P.vs, sizeof vs);
        ae3_stats st = P.st;
        int playing = P.playing, finished = P.finished;
        int whead = P.whead;
        static wcol_t wcol[WCOL_N];
        memcpy(wcol, P.wcol, sizeof wcol);
        float pkl = P.peak_l, pkr = P.peak_r;
        SDL_UnlockMutex(P.mx);

        double dpos = display_pos(pos, seg_t, seg_s, spt, toff);
        double lens = P.len_samp > 0 ? P.len_samp : 1;

        /* active (ch,key) -> envelope, for note highlighting */
        static int32_t act[16][128];
        memset(act, 0, sizeof act);
        int used = 0;
        for (int i = 0; i < AE3_NVOICES; i++)
            if (vs[i].in_use) {
                used++;
                if (vs[i].active && vs[i].env > act[vs[i].ch & 15][vs[i].key & 127])
                    act[vs[i].ch & 15][vs[i].key & 127] = vs[i].env;
            }

        if (st.bus_clipped + st.wet_clipped > prev_clip) {
            prev_clip = st.bus_clipped + st.wet_clipped;
            clip_flash = SDL_GetTicks();
        }

        /* --- draw --- */
        fill(0, 0, WIN_W, WIN_H, C_BG);
        draw_sidebar();

        /* header */
        const char *nm = PlayingIdx >= 0 ? Songs[PlayingIdx].name : "?";
        dtext(MAIN_X + 12, 8, 3, C_TEXT, "%s", nm);
        double cs = dpos / AE3_RATE, ts = lens / AE3_RATE;
        double bpm = (spt > 0 && P.ppqn) ? 60.0 * AE3_RATE / (spt * P.ppqn) : 0;
        /* status line flows left to right so the green loop counter can sit
         * inline without colliding with the kernel label */
        char stat[256];
        snprintf(stat, sizeof stat,
                 "%02d:%04.1f/%02d:%04.1f  %s  BPM %.0f  VOL %d%s  REV %d  %s  LOOP %s",
                 (int)(cs / 60), fmod(cs, 60), (int)(ts / 60), fmod(ts, 60),
                 playing ? "PLAYING" : (finished ? "END" : "PAUSED"),
                 bpm, P.songvol, P.authored ? " AUTH" : "", P.rev_depth,
                 P.exact ? "EXACT" : "TICK", P.loop_on ? "ON" : "OFF");
        dtext(MAIN_X + 12, 40, 2, C_DIM, "%s", stat);
        int sx = MAIN_X + 12 + (int)strlen(stat) * 12;
        if (st.loops_taken) {
            char lx[16];
            snprintf(lx, sizeof lx, " X%u", st.loops_taken);
            dtext(sx, 40, 2, C_GOOD, "%s", lx);
            sx += (int)strlen(lx) * 12;
        }
        dtext(sx, 40, 2, C_DIM, "  %s", P.gaussian ? "GAUSS" : "BRIGHT");
        /* export button, top right */
        {
            int exporting = SDL_AtomicGet(&P.exporting);
            fill(WIN_W - 168, 4, 158, 24, (SDL_Color){40, 44, 58, 255});
            dtext(WIN_W - 160, 9, 2, exporting ? C_DIM : C_ACCENT,
                  exporting ? "EXPORTING" : "EXPORT WAV");
        }
        if (P.err[0])
            dtext(MAIN_X + 12, 56, 1, C_BAD, "%s", P.err);
        else if (P.export_msg[0])
            dtext(MAIN_X + 12, 56, 1,
                  strncmp(P.export_msg, "EXPORTED", 8) == 0 ? C_GOOD :
                  (strncmp(P.export_msg, "EXPORTING", 9) == 0 ? C_DIM : C_BAD),
                  "%s", P.export_msg);

        /* progress bar */
        {
            int bx = MAIN_X + 8, bw = MAIN_W - 16, by = HDR_H + 2, bh = BAR_H - 6;
            fill(bx, by, bw, bh, (SDL_Color){38, 41, 52, 255});
            if (P.loop_s0 >= 0 && P.loop_s1 > P.loop_s0)
                fill(bx + (int)(P.loop_s0 / lens * bw), by,
                     (int)((P.loop_s1 - P.loop_s0) / lens * bw), bh,
                     (SDL_Color){50, 62, 50, 255});
            fill(bx, by, (int)(dpos / lens * bw), bh, (SDL_Color){96, 120, 190, 255});
            fill(bx + (int)(dpos / lens * bw) - 1, by - 2, 3, bh + 4, C_ACCENT);
        }

        /* piano roll */
        {
            int rx = MAIN_X, ry = ROLL_Y, rw = MAIN_W, rh = ROLL_H;
            fill(rx, ry, rw, rh, (SDL_Color){20, 21, 27, 255});
            int klo = P.key_lo - 2, khi = P.key_hi + 2;
            if (khi <= klo) { klo = 40; khi = 80; }
            double rowh = (double)rh / (khi - klo + 1);
            double T = zoom_s * AE3_RATE;
            double v0 = dpos - 0.35 * T;
            /* octave guides (C rows) */
            for (int key = klo; key <= khi; key++)
                if (key % 12 == 0)
                    fill(rx, ry + (int)((khi - key) * rowh), rw, 1,
                         (SDL_Color){38, 40, 50, 255});
            /* loop marker lines */
            double span = (P.loop_s1 > P.loop_s0 && P.loop_s0 >= 0)
                          ? P.loop_s1 - P.loop_s0 : 0;
            for (int k = 0; k < (P.loop_on && span > 0 ? 3 : 1); k++) {
                double off = k * span;
                if (P.loop_s0 >= 0) {
                    int x = rx + (int)((P.loop_s0 + off - v0) / T * rw);
                    if (x >= rx && x < rx + rw)
                        fill(x, ry, 1, rh, (SDL_Color){70, 110, 70, 255});
                }
                if (P.loop_s1 >= 0) {
                    int x = rx + (int)((P.loop_s1 + off - v0) / T * rw);
                    if (x >= rx && x < rx + rw)
                        fill(x, ry, 1, rh, (SDL_Color){110, 80, 60, 255});
                }
            }
            /* notes: pass 0 verbatim; when looping, ghost passes k>=1 for the body */
            int npass = (P.loop_on && span > 0) ? 3 : 1;
            for (int k = 0; k < npass; k++) {
                double off = k * span;
                for (int i = 0; i < P.nnotes; i++) {
                    const vnote_t *n = &P.notes[i];
                    if (k > 0 && n->t0 < P.loop_s0) continue;
                    double t0 = n->t0 + off, t1 = n->t1 + off;
                    if (t1 < v0 || t0 > v0 + T) continue;
                    int x0 = rx + (int)((t0 - v0) / T * rw);
                    int x1 = rx + (int)((t1 - v0) / T * rw);
                    if (x0 < rx) x0 = rx;
                    if (x1 > rx + rw) x1 = rx + rw;
                    if (x1 - x0 < 2) x1 = x0 + 2;
                    int y = ry + (int)((khi - n->key) * rowh);
                    int hh = (int)rowh - 1; if (hh < 2) hh = 2;
                    SDL_Color c = ch_color(n->ch);
                    int32_t env = act[n->ch][n->key];
                    int sounding = env > 0 && t0 <= dpos && dpos <= t1;
                    float bri = sounding
                        ? 0.65f + 0.35f * sqrtf((float)env / 32767.0f)
                        : 0.28f + 0.30f * (n->vel / 127.0f);
                    SDL_Color cc = { (Uint8)(c.r * bri), (Uint8)(c.g * bri),
                                     (Uint8)(c.b * bri), 255 };
                    fill(x0, y, x1 - x0, hh, cc);
                    if (sounding)
                        fill(x0, y, x1 - x0, 1, (SDL_Color){255, 255, 255, 180});
                }
            }
            /* playhead */
            fill(rx + (int)(0.35 * rw) - 1, ry, 2, rh, (SDL_Color){255, 255, 255, 90});
        }

        /* slot grid */
        {
            fill(MAIN_X, SLOT_Y, MAIN_W, SLOT_H, C_PANEL);
            dtext(MAIN_X + 8, SLOT_Y + 4, 1, C_DIM, "SLOTS %d/48 PEAK %u DROPS %u",
                  used, st.peak_voices, st.notes_dropped);
            int cw = (MAIN_W - 16) / AE3_NVOICES;
            for (int i = 0; i < AE3_NVOICES; i++) {
                int x = MAIN_X + 8 + i * cw, y = SLOT_Y + 16, h = SLOT_H - 22;
                fill(x, y, cw - 2, h, (SDL_Color){34, 36, 45, 255});
                if (vs[i].in_use) {
                    float lv = sqrtf((float)vs[i].env / 32767.0f);
                    SDL_Color c = ch_color(vs[i].ch);
                    if (vs[i].released) { c.r = (Uint8)(c.r * 0.6f + 90);
                                          c.g = (Uint8)(c.g * 0.5f);
                                          c.b = (Uint8)(c.b * 0.5f); }
                    int bh = (int)(lv * h); if (bh < 2) bh = 2;
                    fill(x, y + h - bh, cw - 2, bh, c);
                }
            }
        }

        /* waveform strip */
        {
            int wx = MAIN_X, wy = WAVE_Y, ww = MAIN_W, wh = WAVE_H;
            fill(wx, wy, ww, wh, (SDL_Color){14, 15, 19, 255});
            int half = wh / 2;
            int lc = wy + half / 2, rc = wy + half + half / 2;
            fill(wx, lc, ww, 1, (SDL_Color){40, 44, 54, 255});
            fill(wx, rc, ww, 1, (SDL_Color){40, 44, 54, 255});
            float sc = (float)(half / 2 - 3);
            for (int i = 0; i < ww; i++) {
                int idx = (whead - 1 - i + 2 * WCOL_N) % WCOL_N;
                const wcol_t *c = &wcol[idx];
                if (c->lmin == 0 && c->lmax == 0 && c->rmin == 0 && c->rmax == 0
                    && !c->clip) continue;
                int x = wx + ww - 1 - i;
                SDL_Color col = c->clip ? C_BAD : (SDL_Color){118, 205, 130, 255};
                SDL_SetRenderDrawColor(R, col.r, col.g, col.b, 255);
                int y0 = lc - (int)(fminf(c->lmax, 1.f) * sc);
                int y1 = lc - (int)(fmaxf(c->lmin, -1.f) * sc);
                SDL_RenderDrawLine(R, x, y0, x, y1 == y0 ? y0 + 1 : y1);
                y0 = rc - (int)(fminf(c->rmax, 1.f) * sc);
                y1 = rc - (int)(fmaxf(c->rmin, -1.f) * sc);
                SDL_RenderDrawLine(R, x, y0, x, y1 == y0 ? y0 + 1 : y1);
            }
            dtext(wx + 8, wy + 4, 1, C_DIM, "L");
            dtext(wx + 8, wy + half + 4, 1, C_DIM, "R");
            /* peak meters, right edge */
            int mh = wh - 12;
            float dbl = fmaxf(0.f, 1.f + 20.f * log10f(fmaxf(pkl, 1e-4f)) / 60.f);
            float dbr = fmaxf(0.f, 1.f + 20.f * log10f(fmaxf(pkr, 1e-4f)) / 60.f);
            fill(wx + ww - 22, wy + 6, 6, mh, (SDL_Color){30, 32, 40, 255});
            fill(wx + ww - 12, wy + 6, 6, mh, (SDL_Color){30, 32, 40, 255});
            fill(wx + ww - 22, wy + 6 + (int)(mh * (1 - dbl)), 6,
                 (int)(mh * dbl), pkl > 0.999f ? C_BAD : C_GOOD);
            fill(wx + ww - 12, wy + 6 + (int)(mh * (1 - dbr)), 6,
                 (int)(mh * dbr), pkr > 0.999f ? C_BAD : C_GOOD);
        }

        /* footer (main area only -- the sidebar keeps its full height) */
        {
            fill(MAIN_X, FOOT_Y, MAIN_W, FOOT_H, C_PANEL);
            SDL_Color cc = C_DIM;
            uint32_t now = SDL_GetTicks();
            dtext(MAIN_X + 8, FOOT_Y + 6, 1, C_DIM,
                  "VOICES STARTED %u  BUS PEAK %u/32767  CLIP %u  WET PEAK %u  WET CLIP %u",
                  st.voices_started, st.bus_peak, st.bus_clipped,
                  st.wet_peak, st.wet_clipped);
            if (clip_flash && now - clip_flash < 700)
                dtext(MAIN_X + MAIN_W - 90, FOOT_Y + 6, 1, C_BAD, "BUS CLIP");
            dtext(MAIN_X + 8, FOOT_Y + 22, 1, cc,
                  "SPACE PLAY/PAUSE  ENTER PLAY SEL  L LOOP  T CLOCK  R REVERB  ,. DEPTH  "
                  "-= VOL  A AUTHVOL  G KERNEL  Z/X ZOOM  E EXPORT  H HELP  CLICK BAR SEEK");
        }

        /* help overlay, above everything */
        if (HelpOpen) {
            fill(MAIN_X, 0, MAIN_W, WIN_H, (SDL_Color){14, 15, 20, 255});
            dtext(MAIN_X + 20, 10, 3, C_ACCENT, "HOW THE CONSOLE PLAYS THIS");
            dtext(MAIN_X + MAIN_W - 140, 16, 1, C_DIM, "H OR ESC CLOSES");
            int y = 44;
            for (size_t i = 0; i < sizeof HELP / sizeof *HELP; i++) {
                if (HELP[i].head) {
                    y += 6;
                    dtext(MAIN_X + 20, y, 2, C_ACCENT, "%s", HELP[i].s);
                    y += 20;
                } else {
                    dtext(MAIN_X + 26, y, 1, C_TEXT, "%s", HELP[i].s);
                    y += 11;
                }
            }
        }

        /* export dropdown, above everything */
        if (DropOpen) {
            int dx0 = WIN_W - 238, dw = 230;
            fill(dx0, 30, dw, 66, (SDL_Color){44, 48, 62, 255});
            fill(dx0, 30, dw, 1, (SDL_Color){70, 76, 96, 255});
            dtext(dx0 + 8, 34, 2, C_TEXT, "CURRENT SETTINGS");
            dtext(dx0 + 8, 56, 2, C_TEXT, "AUTHORED ORIGINAL");
            dtext(dx0 + 8, 78, 2, C_TEXT, "LOOP X%d", LoopN);
            fill(dx0 + 152, 76, 24, 18, (SDL_Color){60, 66, 84, 255});
            fill(dx0 + 180, 76, 24, 18, (SDL_Color){60, 66, 84, 255});
            dtext(dx0 + 160, 79, 2, C_ACCENT, "-");
            dtext(dx0 + 188, 79, 2, C_ACCENT, "+");
        }

        SDL_RenderPresent(R);
    }

    SDL_CloseAudioDevice(dev);
    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
