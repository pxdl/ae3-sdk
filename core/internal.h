/* internal.h -- private structs shared by the core's translation units. */
#ifndef AE3_INTERNAL_H
#define AE3_INTERNAL_H

#include "ae3synth.h"

/* ---- bank ------------------------------------------------------------- */

/* Tone record, 16 bytes in the .hd (docs/formats/BGM.md).
 * Byte 10 is read by nothing in the game and is not stored. */
typedef struct {
    uint8_t  lo, hi;      /* key window (drums: key = position, bytes 0/1 are zero) */
    uint8_t  root;
    int8_t   tune;        /* byte 3, SIGNED, 1/16 semitone (6.25 cents) -- NOT cents */
    uint16_t addr;        /* byte 4-5, SPU 8-byte units; *8 = offset in .bd; 0xFFFF silent */
    uint16_t adsr1, adsr2;
    uint8_t  vol, pan, flags;
    uint8_t  bend;        /* resolved: prog[4] if flags&0x10, else byte 13 */
    uint8_t  bend_raw;    /* byte 13 unresolved: the note-on aborts on RAW 0xFF
                             (FUN_003facb8 checks pbVar18[0xd] BEFORE the 0x10 resolution) */
    uint8_t  lfo;         /* resolved: prog[5] if flags&0x40, else byte 14; 0x7F = none */
} ae3_tone;

/* tone flag bits (byte 15) */
#define AE3_TF_REVERB        0x80u
#define AE3_TF_USE_PROG_LFO  0x40u
#define AE3_TF_LFO_ON        0x20u
#define AE3_TF_USE_PROG_BEND 0x10u
#define AE3_TF_SE_ONLY       0x01u   /* inert for BGM */

#define AE3_NO_SAMPLE 0xFFFFu

typedef struct {
    bool     present;     /* false = 0xFFFF slot in the program table */
    bool     drum;        /* header byte 0 == 0xFF: one tone per key over [key0,key1] */
    bool     stack;       /* header bit 7 (non-drum): keep matching tones past the first */
    uint8_t  vol, bend, lfo, key0, key1;   /* header bytes 1, 4, 5, 6, 7 */
    int      ntones;
    ae3_tone *tones;
} ae3_prog;

typedef struct {
    ae3_prog *progs;
    int       nprogs;     /* table slots, incl. absent ones */
    uint8_t  *bd;         /* owned copy of the raw PS-ADPCM body */
    size_t    bd_len;
    uint16_t  vel_count;  /* 0 in all 62 banks => curve never consulted */
    uint8_t   vel[128];   /* identity in all 62 banks; kept so a non-identity bank screams */
    ae3_waveform *waves;  /* introspection table: unique tone addrs, ascending */
    int       nwaves;
} ae3_bank;

/* ---- voice: ADPCM decoder + ADSR envelope (voice.c) --------------------- */

/* PS-ADPCM frame flag bits (byte 1) */
#define AE3_AF_END       0x01u
#define AE3_AF_REPEAT    0x02u
#define AE3_AF_LOOPSTART 0x04u

/* Streaming decoder: pulls one PCM sample at a time, decoding 16-byte frames
 * (28 samples) on demand. Mirrors bgm.decode_adpcm bit for bit; history is carried
 * across the loop seam (the loop-start frame is re-decoded with live history). */
typedef struct {
    const uint8_t *bd;
    size_t   bd_len;
    uint32_t start, frame;      /* byte offsets into the .bd */
    int32_t  h1, h2;
    int32_t  loop_frame;        /* byte offset of the LOOP_START frame; -1 = none seen */
    uint32_t loops;             /* loop jumps taken so far (test harness reads this) */
    uint8_t  flags;             /* flag byte of the frame currently in buf */
    int16_t  buf[28];
    int      pos;               /* next sample in buf; 28 = refill needed */
    bool     primed, stopped;
} ae3_adpcm;

void ae3_adpcm_init(ae3_adpcm *d, const uint8_t *bd, size_t bd_len, uint32_t start);
bool ae3_adpcm_next(ae3_adpcm *d, int16_t *out);   /* false = one-shot ended */

typedef struct ae3_voice ae3_voice;
/* interp = the instance's 256-phase x 4-tap kernel (gaussian or bright) */
bool ae3_voice_tick(ae3_voice *v, const int16_t (*interp)[4], int32_t *out);

/* ---- voice slots: the driver's allocator (sg2slotctrl.c) -------------------
 *
 * Ground truth (raw MIPS of the game's slot controller, verified):
 * 48 slots (EE 0x0074B700, stride 0xE8), 1:1 with the SPU2's 48 hardware voices.
 * FUN_004009c8 grants the first free slot scanning ROUND-ROBIN from a persistent
 * cursor (cursor = granted+1 mod 48; the core-range args FUN_00400b58 computes are
 * never read -- BGM and SE share the single pool). All 48 busy -> -1, and the BGM
 * note-on FUN_003facb8 returns: the note is DROPPED, remaining stack layers included.
 * There is no stealing: the sibling path's "fallbacks" (FUN_003fdc30/68) are
 * sg2slotctrl.c asserts that return -1.
 *
 * Slots are freed ONLY by the driver's update tick FUN_003ffc70 (callback #3 of the
 * 60 Hz NTSC / 50 Hz PAL sound-update thread; the sequencer is callback #2, so
 * note-ons at a tick run BEFORE that tick's frees): every bound slot ages by 1 per
 * tick, and from the tick where age > 2 the SPU2 ENVX is polled -- ENVX < 2 frees
 * the slot. The 3-tick grace covers the attack starting at ENVX 0; the corpus' max
 * attack shift (17) reaches level >= 4 within 64 samples, so no real tone is still
 * below 2 when its first poll lands (a freed-while-rising voice would keep playing
 * unowned until its slot is regranted -- unreachable here, counted if it ever fires).
 * AE3_NVOICES / AE3_TICK_* live in the public header. */

/* SPU2 envelope, clocked per output sample (ENV_HZ == AE3_RATE). Level 0..0x7FFF is a
 * straight amplitude multiplier -- the whole point of the native synth (no dB shapes). */
enum { AE3_ENV_ATTACK, AE3_ENV_DECAY, AE3_ENV_SUSTAIN, AE3_ENV_RELEASE };

typedef struct {
    uint16_t a1, a2;
    int      phase;
    int32_t  level;             /* 0..0x7FFF */
    int32_t  sus_level;
    uint32_t wait;              /* ticks until the next level step; 0 = holding */
} ae3_env;

void    ae3_env_keyon(ae3_env *e, uint16_t a1, uint16_t a2);
void    ae3_env_keyoff(ae3_env *e);
int32_t ae3_env_tick(ae3_env *e);
bool    ae3_env_dead(const ae3_env *e);   /* the driver's ENVX<2 reclaim condition */
/* cycles for one phase run start->target; shared with the live stepper, mirrored by
 * the corpus gates against the offline reference */
uint64_t ae3__env_phase_cycles(int shift, int step, bool exp, bool rising,
                               int32_t level, int32_t target);

struct ae3_voice {
    bool     in_use;               /* slot allocation state: set at note-on, cleared
                                      only by the update tick's ENVX<2 poll */
    uint32_t age;                  /* update ticks since the note-on bound the slot */
    bool     active, released;    /* active = still rendering (env alive) */
    uint8_t  ch, key, vel;
    const ae3_tone *tone;
    ae3_adpcm dec;
    ae3_env   env;
    /* pitch: hardware counter, 12-bit fraction. Bits 4-11 index the gaussian table;
     * integer carries advance the 4-sample window (win[3] newest). */
    uint16_t pitch;                /* register as the IRX computes it (16 bits) */
    uint32_t counter;              /* fractional accumulator, kept < 0x1000 */
    int16_t  win[4];
    bool     win_primed;
    /* volume chain (vol.c). vvol/cpan refresh on CC7/10/11; vel and tpan are fixed
     * at note-on (FUN_003fab98 refreshes exactly the first two). */
    int32_t  vvol;                 /* voice +0x44: CC7*CC11*prog[1]*tone[11]/127^3 */
    uint8_t  cpan, tpan;           /* voice +0x58/+0x5c, stored pre-clamped */
    uint16_t voll, volr;           /* the SPU2 registers (0..0x3FFF, /2 format) */
};

/* gauss.c: the SPU2's 512-entry 4-tap interpolation table (psx-spx transcription),
 * plus the builder that expands it (or the bright catmull-rom alternative) into the
 * instance's phase-major interp[256][4] used by the voice tick. */
extern const int16_t ae3_gauss[512];
void ae3__interp_build(ae3_synth *s);

/* vol.c: the driver's linear volume chain + the SPU2 pan table (see vol.c header
 * for the Ghidra ground truth each piece was read from). */
const uint16_t *ae3__pan_lut(void);            /* 128 entries, hi byte = L, lo = R */
int32_t ae3__vol_product(int cc7, int cc11, int progvol, int tonevol);
int     ae3__cpan_clamp(int v);                /* channel pan: clamp(v, 1, 127) */
int     ae3__tpan_clamp(int v);                /* tone pan: >127 -> 127, ==0 -> 1 */
void    ae3__voice_regs(int svl, int svr, int32_t vvol, int vel, int cpan, int tpan,
                        uint16_t *voll, uint16_t *volr);

/* Frames mixed per inner block: bounds the integer bus accumulators on the stack. */
#define AE3_MIX_CHUNK 512

/* ---- reverb bus (reverb.c) -------------------------------------------------
 *
 * The SPU2's fixed reverb, ported from the project's offline oracle (psx-spx formula) into a
 * STREAMING per-sample form: voices with tone flag 0x80 sum into a 16-bit-saturated
 * wet bus; the bus is decimated 48k->24k through the hardware's 39-tap half-band FIR,
 * runs one feedback-network step per 24 kHz frame, and the return is interpolated
 * back to 48k through the same FIR (x2, zero-stuffed). The preset (STUDIO_C, boot-
 * pinned) is read out of libsd.irx at runtime, never embedded.
 *
 * GRID + LATENCY (pinned so the corpus gates can diff against the offline oracle EXACTLY):
 * the offline spu2rev applies both FIRs zero-phase (non-causal, +-19 taps); real time
 * cannot look ahead, so here they are causal -- the network steps on ODD 48 kHz
 * frames over the last 39 wet samples, the return is stuffed on odd frames likewise.
 * With identical FP operation order (and -ffp-contract=off, already pinned) the
 * output equals the offline render delayed by EXACTLY 19+19 = 38 samples (0.79 ms --
 * the latency the hardware's own causal FIRs have). The corpus gate feeds the oracle a
 * 38-sample-delayed wet stem and requires bitwise equality. Which parity the console
 * decimates on (odd vs even, a +-1-sample return shift) is unknowable short of a
 * hardware capture and inaudible; odd is what lands on the offline grid. */
#define AE3_REV_FIRTAPS 39
#define AE3_REV_NCOEF   32
/* Feed the reverb this much silence after the song so the tail decays instead of
 * being chopped (STUDIO_C RT60 ~2 s). Ours, matching the offline reference -- the
 * console just rings until the next song. */
#define AE3_REV_TAIL_SAMPLES (4 * AE3_RATE)

typedef struct {
    bool     on;              /* preset loaded AND depth > 0: the mixer's gate */
    int      depth;           /* 0..127; boot value 30 */
    double   evol;            /* (depth*32767/127)/32768.0 -- EE 0x3f67c8 as 1.15 */
    uint16_t raw[AE3_REV_NCOEF];   /* preset words as read from libsd (for --dump) */
    uint32_t units;           /* work-area size, 8-byte units (libsd size table) */
    double   v[AE3_REV_NCOEF];     /* signed 1.15 gains */
    long     m[AE3_REV_NCOEF];     /* address taps in buffer slots (raw * 4) */
    long     nslots;          /* units * 4 */
    double  *buf;             /* the feedback network's ring; NULL until loaded */
    uint64_t nsamp;           /* 48 kHz frames processed (parity = step trigger) */
    uint64_t mcnt;            /* 24 kHz network steps run (ring base = mcnt % nslots) */
    int16_t  dn_l[64], dn_r[64];   /* last wet samples for the decimation FIR */
    double   up_l[32], up_r[32];   /* last network outputs for the return FIR */
} ae3_rev;

int  ae3__load_libsd(ae3_synth *s, const uint8_t *d, size_t len);
void ae3__rev_set_depth(ae3_rev *r, int depth);
void ae3__rev_reset(ae3_rev *r);         /* clear state, keep the preset */
void ae3__rev_free(ae3_rev *r);
/* One 48 kHz frame: push the (saturated) wet bus, run the half-rate network on odd
 * frames, return the interpolated reverb return (pre-EVOL, +-1.0 full scale). */
void ae3__rev_sample(ae3_rev *r, int16_t wl, int16_t wr, double *outl, double *outr);

/* Full-mix trace hook for the test harness (wavdump --stems); NULL in normal use.
 * Fires per mixed block, reverb path only: dry/wet are the saturated s16 buses,
 * rev is the combined dry+EVOL*return quantized EXACTLY as the offline oracle writes
 * its output ((short)(l * 32767.0), pre-MVOL) so the gates can diff the two bitwise. */
typedef void (*ae3_mix_tap)(void *user, const int16_t *dry, const int16_t *wet,
                            const int16_t *rev, int nframes);

/* Slot-lifecycle trace hook for the test harness (wavdump --slotdump); NULL in
 * normal use. Events: 'G' grant (slot, ch, key), 'D' drop (slot -1, ch, key),
 * 'F' tick-free (slot), 'Z' freed-while-rendering (slot; unreachable in the corpus). */
typedef void (*ae3_slot_trace)(void *user, char ev, uint64_t pos,
                               int slot, int ch, int key);


/* pitch.c */
#define AE3_PITCH_TBL_N 608        /* entries in sg2iopm1.irx pitch_tbl (measured) */
void     ae3_pitch_tbl_et(uint16_t *tbl);
int      ae3__load_pitch_irx(ae3_synth *s, const uint8_t *d, size_t len);
uint16_t ae3__pitch_reg(ae3_synth *s, int note, int root, int fine, int bend_msb,
                        int range);

/* ---- sequence ---------------------------------------------------------- */

/* Event record; kinds are the public AE3_EV_* (classification happens at parse
 * time, AFTER the event-stream hash -- the hash covers the raw stream). */
typedef struct {
    uint32_t tick;
    uint8_t  kind;
    uint8_t  status, a, b, nd;  /* AE3_EV_CH: status byte + nd data bytes */
    uint32_t uspqn;             /* AE3_EV_TEMPO */
} ae3_event;

/* Event-dispatch trace hook (wavdump --eventdump); NULL in normal use. Fires once
 * per dispatched sequence event with the dispatch sample and the event's index in
 * the parsed array (the index goes BACKWARDS across a loop-end jump) -- the corpus
 * gates' walker mirror diffs the (pos, idx) stream exactly. */
typedef void (*ae3_ev_trace)(void *user, uint64_t pos, int idx, const ae3_event *e);

typedef struct {
    ae3_event *ev;
    int        nev, next;
    uint16_t   ppqn;
    /* current tempo segment: sample(tick) = seg_sample + (tick - seg_tick) * spt.
     * Load-time precompute and playback use this same arithmetic, so they agree
     * exactly. Ticks here are EFFECTIVE (ev.tick + the synth's tick_offset), so the
     * clock keeps running across loop-end jumps; u64 because offsets accumulate. */
    uint64_t   seg_tick;
    double     seg_sample;
    double     spt;             /* samples per tick at AE3_RATE */
    bool       ended;
} ae3_seq;

/* ---- instance ---------------------------------------------------------- */

struct ae3_synth {
    ae3_bank  bank;
    ae3_seq   seq;
    bool      have_bank, have_seq;
    uint64_t  pos;              /* absolute sample position on the 48 kHz clock */
    uint8_t   chan_prog[16];    /* last program change per channel */
    uint8_t   chan_bend[16];    /* pitch wheel MSB, 64 = centre */
    uint8_t   chan_cc7[16];     /* volume, default 127 (127 in 572/573 events) */
    uint8_t   chan_cc10[16];    /* pan, default 64; clamped 1..127 at use */
    uint8_t   chan_cc11[16];    /* expression, default 127 (genuinely varies) */
    /* M7 driver CC state, all init 0 (chan-state init FUN_003f8034/003f8160 zeroes
     * them). CC1/CC2 = the LFO depth/rate stores (chan+0x304/+0x300); the LFO itself
     * renders in M9 -- until then these are state + stats only. CC98/CC99 = the NRPN
     * pair (chan+0x319/+0x318) CC6 dispatches on; chan_cc6 = data entry (+0x31a). */
    uint8_t   chan_cc1[16], chan_cc2[16];
    uint8_t   chan_nrpn_msb[16], chan_nrpn_lsb[16], chan_cc6[16];
    uint8_t   rev_shadow[2][4]; /* the never-applied per-core reverb config the NRPN
                                   path writes (DAT_00746ec4 block: type/depth/
                                   word0xc/word0x10) -- kept so --dump can show it */
    uint8_t   song_word18;      /* song-state+0x18 (NRPN (2,0) target; consumer
                                   unknown, corpus never writes it) */
    int32_t   song_vol_l, song_vol_r;   /* state[0xb]/[0xc], driver init 127/127 */
    ae3_voice voices[AE3_NVOICES];
    int       nslots;           /* pool size: AE3_NVOICES; the harness shrinks it to
                                   force drops (the game data never fills 48) */
    int       cursor;           /* allocator round-robin cursor (gp-0x7b1c; 0 at boot) */
    uint64_t  next_tick;        /* next update-tick sample (multiples of AE3_TICK_SAMPLES) */
    /* M7 sequencer state (the SMF walker's, cc/NOTES.md §3-4). Loop bookkeeping is
     * in ORIGINAL tick space; tick_offset maps it to the unrolled "effective" ticks
     * every sample-position conversion uses (eff = ev.tick + tick_offset). */
    bool      timing_exact;     /* false = the console's 60 Hz burst dispatch */
    int       loop_cfg;         /* ae3_synth_set_loop value; 0 = markers ignored */
    int       loop_live;        /* walker +0x70: live counter (CC102 overwrites) */
    int       loop_from;        /* event index the loop-end jump returns to */
    uint32_t  loop_from_tick;   /* tick of the loop-start marker (walker +0x6c) */
    uint64_t  tick_offset;      /* accumulated (loop_end - loop_start) tick shifts */
    double    walk_cur;         /* walker +0x48: tick accumulator, += walk_adv per
                                   60 Hz tick (tick-mode dispatch fires evt <= cur) */
    double    walk_adv;         /* walker +0x40: ticks per tick-grid step; init and
                                   FF 51 recompute use the console's exact doubles */
    ae3_rev   rev;              /* reverb bus; inert until libsd is loaded */
    bool      wet_ever;         /* a reverb-flagged voice has played: gate the tail */
    uint64_t  rev_tail_left;    /* tail samples still to render after song+voices end */
    ae3_slot_trace slot_trace;  /* harness hook, normally NULL */
    void     *slot_trace_user;
    ae3_mix_tap mix_tap;        /* harness hook (--stems), normally NULL */
    void     *mix_tap_user;
    ae3_ev_trace ev_trace;      /* harness hook (--eventdump), normally NULL */
    void     *ev_trace_user;
    uint16_t  pitch_tbl[AE3_PITCH_TBL_N];
    bool      pitch_verbatim;   /* true = loaded from sg2iopm1.irx; false = ET fallback */
    /* resampler kernel, phase-major. gaussian=true (default) expands ae3_gauss with
     * the hardware's exact index pattern -- bit-identical to reading the table raw.
     * false = catmull-rom "bright" (ae3_synth_gaussian; NOT hardware behavior). */
    int16_t   interp[256][4];
    bool      gaussian;
    ae3_stats st;
    char      err[256];
};

/* synth.c */
int  ae3__fail(ae3_synth *s, const char *fmt, ...);
void ae3__slot_flush(ae3_synth *s);   /* harness: tick until every slot frees */
/* bank.c */
int  ae3__parse_bank(ae3_synth *s, const uint8_t *hd, size_t hd_len,
                     const uint8_t *bd, size_t bd_len);
void ae3__bank_free(ae3_bank *b);
/* seq.c */
int  ae3__parse_seq(ae3_synth *s, const uint8_t *mid, size_t len);
void ae3__seq_free(ae3_seq *q);
void ae3__seq_reset_clock(ae3_seq *q);

#endif /* AE3_INTERNAL_H */
