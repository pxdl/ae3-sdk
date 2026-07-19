/* ae3synth.h -- Ape Escape 3 native SPU2 BGM synth: public API.
 *
 * Pure, engine-agnostic C core: no engine dependency, builds and tests headless.
 * Format + driver ground truth: docs/formats/BGM.md.
 *
 * LEGAL: consumes Sony's bank/sequence data at runtime; never embeds or redistributes it.
 */
#ifndef AE3SYNTH_H
#define AE3SYNTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SPU2 core rate. The sequencer advances by CONSUMED SAMPLES against this clock --
 * sample-accurate, decoupled from any host frame rate. */
#define AE3_RATE 48000

/* The SPU2's 48 hardware voices (one pool shared by music and SE; the driver
 * grants round-robin and never steals -- docs/formats/BGM.md) and the driver's
 * 60 Hz update tick, on which slots free and tick-mode events dispatch. */
#define AE3_NVOICES      48
#define AE3_TICK_HZ      60                        /* NTSC; PAL boots with 50 */
#define AE3_TICK_SAMPLES (AE3_RATE / AE3_TICK_HZ)  /* 800 */

typedef struct ae3_synth ae3_synth;

/* Parse-time facts about the loaded bank + sequence, for harnesses and A/B checks. */
typedef struct {
    /* bank */
    uint32_t prog_slots;      /* program table entries, incl. unused (0xFFFF) slots */
    uint32_t progs_used;
    uint32_t tones;
    /* sequence */
    uint16_t ppqn;
    uint32_t events;          /* channel events (note/cc/pc/bend/pressure) */
    uint32_t note_ons;        /* 0x9n with velocity > 0 */
    uint32_t note_offs;       /* 0x8n, plus 0x9n velocity 0 */
    uint32_t ccs, prog_changes, pitch_bends;
    uint32_t tempo_changes;   /* FF 51 metas */
    uint32_t meta_skipped;    /* other meta/sysex (parsed past, not used) */
    uint32_t end_tick;        /* tick of FF 2F end-of-track */
    uint64_t end_sample;      /* end-of-track position on the 48 kHz clock,
                                 loop markers ignored (single play-through) */
    /* sequencer-consumed CCs (the game's SMF walker FUN_00402108 eats these; they
     * never reach the driver's channel state) */
    uint32_t loop_starts;     /* CC99 val 20 markers seen at parse */
    uint32_t loop_ends;       /* CC99 val 30 markers seen at parse */
    uint32_t loop_sets;       /* CC102 loop-count events (corpus: never) */
    uint32_t hooks;           /* CC90 game-callback events (corpus: never) */
    /* FNV-1a 64 over the event stream, mirrored by the corpus gates */
    uint64_t hash_ch, hash_tempo;
    /* playback (accumulated while rendering) */
    uint32_t voices_started;
    uint32_t noteons_aborted;   /* raw bend byte 0xFF sentinel hit */
    uint32_t notes_dropped;     /* all 48 slots busy: the driver drops the note-on,
                                   remaining stack layers included (FUN_003facb8) */
    uint32_t peak_voices;       /* peak slots in use (allocation pressure; slots free
                                   only at the 60 Hz update tick, so this exceeds
                                   audible polyphony) */
    uint32_t slots_freed_live;  /* slot freed while its voice was still rendering --
                                   unreachable with the game's data (see internal.h) */
    uint32_t pitch_idx_clamped; /* pitch_tbl index out of range (game data: never) */
    uint32_t pitch_step_clamped;/* pitch register above 0x3FFF (game data: never) */
    /* dry-bus behavior: the SPU2 sums voices into a 16-bit-saturated bus (psx-spx /
     * PCSX2 clamp_mix), so dense passages CAN clip on real hardware. Track how hard. */
    uint32_t bus_clipped;       /* samples (L+R counted separately) that saturated */
    uint32_t bus_peak;          /* max |pre-clamp bus sample|; 32767 = full scale */
    /* wet (reverb-send) bus, same saturation semantics; only reverb-flagged voices
     * (tone flag 0x80) sum into it, and only while a reverb preset is loaded */
    uint32_t wet_clipped;
    uint32_t wet_peak;
    /* driver-side CC dispatch (table at EE 0x0069dea8, pinned M7 -- cc/NOTES.md).
     * Counters exist so a data set that exercises a path the corpus never touches
     * screams instead of silently diverging. */
    uint32_t cc_lfo;            /* CC1 depth / CC2 rate stores (M9: vibrato renders
                                   on any channel with both nonzero -- 4 songs) */
    uint32_t cc_nrpn;           /* CC98/CC99 state stores reaching the DRIVER (the
                                   walker eats 20/30, so corpus: 0) */
    uint32_t cc6_shadow;        /* CC6 with NRPN (0|1, 0..3): shadow reverb config
                                   write, never applied without (x, 0x7f) (corpus:
                                   62, all inert -- NRPN state is (0,0) throughout) */
    uint32_t cc6_rev_apply;     /* CC6-triggered reverb APPLY (NRPN lsb 0x7f) or a
                                   (2,0) song-state write: would change live audio;
                                   NOT implemented, corpus never fires it */
    uint32_t cc_stub;           /* CCs on the driver's jr-ra stub (8/9/66 in corpus)
                                   or assigned-but-unmodelled handlers */
    uint32_t loops_taken;       /* loop-end jumps performed while looping enabled */
} ae3_stats;

ae3_synth  *ae3_synth_new(void);
void        ae3_synth_free(ae3_synth *s);
const char *ae3_synth_error(const ae3_synth *s);  /* last error; valid until next load call */

/* Load a Sony "Jam" bank: NAME.hd (header) + NAME.bd (raw PS-ADPCM body, no headers --
 * the .hd size prefix is cross-checked against bd_len). Buffers are copied as needed;
 * the caller may free them. Returns 0, or -1 with ae3_synth_error() set. */
int ae3_synth_load_bank(ae3_synth *s, const void *hd, size_t hd_len,
                        const void *bd, size_t bd_len);

/* Load a sequence: standard MIDI file, format 0 (all 64 songs are). Resets playback
 * to sample 0. Returns 0, or -1 with ae3_synth_error() set. */
int ae3_synth_load_seq(ae3_synth *s, const void *mid, size_t mid_len);

/* Load Sony's pitch table from the game's own sg2iopm1.irx (extracted at runtime,
 * never embedded). Optional: without it a computed equal-temperament table is used,
 * within ±0.42 cents of the real one except its two corrupt entries. */
int ae3_synth_load_pitch_irx(ae3_synth *s, const void *irx, size_t irx_len);

/* Load the SPU2 reverb preset from the game's own libsd.irx (extracted at runtime,
 * never embedded -- same stance as the pitch table). The game sets STUDIO_C depth 30
 * once at boot on both cores and never touches it again (EE 0x0035fa1c; CC91/CC98
 * are never sent), so that is what this enables: reverb-flagged voices feed a wet
 * bus that runs the hardware's half-rate feedback network live in the mixer.
 * Optional: without it the render is pure dry (the pre-M6 behavior, bit-identical). */
int ae3_synth_load_reverb_irx(ae3_synth *s, const void *libsd, size_t libsd_len);

/* Resampler kernel A/B. on=true (default) = the SPU2's 4-tap gaussian (psx-spx
 * table) -- the hardware behavior: darker treble, and below-root notes genuinely
 * image (the s_9 percussion "frying" is authentic). on=false = a catmull-rom
 * 4-tap kernel: brighter/cleaner, NOT what the console does -- a listening preference
 * offered against the native faithful default. Applies to live voices. */
void ae3_synth_gaussian(ae3_synth *s, bool on);

/* Reverb depth 0..127 (EVOL = depth*32767/127, EE 0x3f67c8). Boot/default = 30;
 * 0 disables the bus entirely (exact dry path). A knob for A/B only -- the game
 * never moves it. NOTE: whether the EVOL register scales /2 like the voice-volume
 * format (which would make the return 2x hotter than the oracle's 1.15 reading) is
 * unresolved until a hardware capture; this follows the current oracle. */
void ae3_synth_reverb_depth(ae3_synth *s, int depth);

/* Direct channel events, same paths the sequencer dispatches into. Used by test
 * harnesses now; later by the game's cue layer / sound effects. */
void ae3_synth_note_on(ae3_synth *s, int ch, int key, int vel);
void ae3_synth_note_off(ae3_synth *s, int ch, int key);
void ae3_synth_program(ae3_synth *s, int ch, int prog);

/* Song volume L/R, 0..127 -- the driver state the game's fade/volume API moves
 * (FUN_003f9158; the pre-volume pair in every voice's register math). Driver init
 * = 127/127. NOTE (2026-07-16): at the full default the corpus' dense songs sum
 * past the SPU2's 16-bit bus and saturate (peak measured 3.1x on s_28_hongkong /
 * b_8) -- faithful to the register math, but whether the real game plays BGM at
 * full song volume is unresolved until the PCSX2 in-game dump. Applies to live
 * voices immediately, like the driver's fade tick. */
void ae3_synth_song_volume(ae3_synth *s, int l, int r);

/* Song looping, the sequencer's own mechanism (pinned M7 from the game's SMF walker,
 * FUN_00402108 / FUN_00400e80): CC99 val 20 marks
 * the loop start, CC99 val 30 the loop end; both are consumed by the walker and never
 * reach the driver. At a loop end the count is decremented (0x7f = never) and while it
 * stays positive playback jumps back, deltas preserved across the seam. 64 of the 68
 * songs carry exactly one marker pair; the console plays BGM with count 0x7f (loop
 * forever, the play-open init).
 *   count = 0                 ignore the markers, play through once (the DEFAULT here:
 *                             the headless A/B oracles are single-pass)
 *   count = 1..126            take the loop-end jump while --count > 0 (walker rule;
 *                             count n plays the loop body n times total)
 *   count = AE3_LOOP_FOREVER  the console's BGM behavior; ae3_synth_done() then never
 *                             fires -- the host stops playback
 * A CC102 in the sequence overwrites the live counter (the walker's set-loop-count;
 * never sent by this corpus). Takes effect from the next loop-end marker reached. */
#define AE3_LOOP_FOREVER 0x7f
void ae3_synth_set_loop(ae3_synth *s, int count);

/* Event dispatch timing. The console's sequencer runs as callbacks of the 60 Hz sound
 * thread: each tick the SMF walker advances a double tick-accumulator and fires EVERY
 * event due, in a burst, into the driver (pinned M7 -- cc/NOTES.md §4). So on hardware
 * event dispatch quantizes to the 800-sample tick grid (up to 16.7 ms late vs exact).
 *   exact = true   (default) dispatch each event at its exact sample position
 *   exact = false  the console's tick-grid burst dispatch
 * SETTLED DIAL (user listening verdict 2026-07-16, memory project_ae3_bgm_findings):
 * tick mode is the console-accurate behavior but READS AS A DEFECT to the ear --
 * "notes are late, like the game is running on an emulator with slowdown" -- so
 * exact is the shipping default. Keep tick mode: it is the right setting for
 * emulation-faithful output and for lining up against a PCSX2 in-game dump.
 * The grid phase is chosen so a tick-0 event fires at sample 0 (on the console the
 * phase between play() and the sound thread is arbitrary within one tick). */
void ae3_synth_event_timing(ae3_synth *s, bool exact);

/* Render interleaved stereo float at AE3_RATE, dispatching sequence events per
 * ae3_synth_event_timing. Returns frames written (short only at end of song);
 * 0 once the sequence has ended and no voices remain; -1 if nothing is loaded.
 * With a bank but no sequence loaded, renders exactly nframes (driven by the
 * direct-event API above). */
int ae3_synth_render(ae3_synth *s, float *out, int nframes);

bool ae3_synth_done(const ae3_synth *s);
void ae3_synth_get_stats(const ae3_synth *s, ae3_stats *out);

/* ---- playback introspection (read-only) ---------------------------------------
 * The surface a visualizer needs: piano roll, slot grid, loop-unwound playhead.
 * Promoted in W1 from the GUI harness's former white-box peeks; bgmplay is the
 * reference consumer, the web player's worklet snapshot is built from the same
 * calls. */

/* Absolute sample position on the 48 kHz clock (monotonic; loops keep counting). */
uint64_t ae3_synth_pos(const ae3_synth *s);

/* One voice slot's live state. Slots free only at the 60 Hz update tick (the
 * driver's ENVX<2 poll), so in_use outlives audibility slightly -- the real
 * allocator behavior, worth showing truthfully in a slot view. */
typedef struct {
    bool    in_use;             /* slot bound to a note */
    bool    active;             /* still rendering (envelope alive) */
    bool    released;           /* key-off seen, envelope in release */
    uint8_t ch, key;
    int32_t env;                /* SPU2 envelope level, 0..0x7FFF, linear */
} ae3_voice_state;

/* Copy slot i's state (0..AE3_NVOICES-1). Returns false if i is out of range. */
bool ae3_synth_voice(const ae3_synth *s, int i, ae3_voice_state *out);

/* Parsed-sequence event kinds. The LOOP/HOOK kinds are CCs the game's SMF
 * walker consumes (FUN_00402108) -- classified at parse time and never
 * dispatched into the driver's channel state, like hardware. LOOP_START =
 * CC99 v20, LOOP_END = CC99 v30, LOOP_COUNT = CC102, HOOK = CC90. */
enum { AE3_EV_CH, AE3_EV_TEMPO, AE3_EV_END,
       AE3_EV_LOOP_START, AE3_EV_LOOP_END, AE3_EV_LOOP_COUNT, AE3_EV_HOOK };

typedef struct {
    uint32_t tick;              /* original-timeline MIDI tick */
    uint8_t  kind;              /* AE3_EV_* */
    uint8_t  status, a, b;      /* AE3_EV_CH: status byte + data bytes */
    uint32_t uspqn;             /* AE3_EV_TEMPO: microseconds per quarter note */
} ae3_seq_event;

/* Event count (0 when no sequence is loaded), per-event copy (false = index out
 * of range), and the sequence's PPQN (0 when none loaded). Timeline builders
 * walk these once at load to derive note spans, tempo map and loop markers. */
int      ae3_synth_seq_events(const ae3_synth *s);
bool     ae3_synth_seq_event(const ae3_synth *s, int i, ae3_seq_event *out);
uint16_t ae3_synth_seq_ppqn(const ae3_synth *s);

/* ---- bank introspection (read-only) --------------------------------------
 * The surface a sample-kit exporter needs: the loaded bank's distinct
 * waveforms (unique tone sample addresses -- tones commonly share one; the
 * hard-panned L/R pairs are two tones over one waveform), each with its
 * single-pass PCM through the live note-on ADPCM decoder, its loop point,
 * and a first-referencing (prog, tone, root, tune) for naming. Waveforms are
 * ordered by ascending .bd offset -- the bank's own layout order. */

typedef struct {
    uint32_t addr;        /* tone sample address, SPU 8-byte units (*8 = .bd byte offset) */
    uint32_t samples;     /* single-pass PCM length: 28 per frame through the END frame */
    int32_t  loop_start;  /* sample index the END+REPEAT frame jumps back to; -1 = one-shot.
                             REPEAT with no LOOP_START-flagged frame loops to 0, the SPU
                             default (docs/formats/BGM.md; corpus always flags it) */
    uint16_t prog, tone;  /* first (program slot, tone index) referencing it */
    uint8_t  root;        /* that tone's root key (the key it plays at 44100 Hz) */
    int8_t   tune;        /* that tone's fine tune, 1/16 semitone units */
    uint16_t refs;        /* how many tones reference this waveform */
} ae3_waveform;

/* Waveform count (0 when no bank is loaded) and per-waveform copy (false =
 * index out of range). The table is built at bank load. */
int  ae3_synth_bank_waveforms(const ae3_synth *s);
bool ae3_synth_bank_waveform(const ae3_synth *s, int i, ae3_waveform *out);

/* Decode waveform i's single pass (loop not unrolled) into out, writing at
 * most max samples. Returns the full single-pass sample count (size from
 * ae3_waveform.samples, or call with max 0 to measure), or -1 if no bank is
 * loaded / i is out of range. Same streaming decoder as the note-on path,
 * so the corpus gates hold it bit-exact against the offline oracle. */
int  ae3_synth_bank_waveform_pcm(const ae3_synth *s, int i,
                                 int16_t *out, uint32_t max);

/* The sequencer's live tempo segment + loop bookkeeping -- everything needed to
 * map the render position back onto the single-pass timeline:
 *     eff_tick      = seg_tick + (pos - seg_sample) / spt
 *     original tick = eff_tick - tick_offset
 * then original tick -> sample through a tempo map built from the events above
 * (bgmplay's display_pos/tmap_sample; the TS port mirrors it). tick_offset
 * accumulates one loop-body length per loop-end jump; 0 = no jump taken yet.
 * BPM display: 60 * AE3_RATE / (spt * ppqn). */
typedef struct {
    uint64_t seg_tick;          /* effective tick at the tempo-segment origin */
    double   seg_sample;        /* sample position of the segment origin */
    double   spt;               /* samples per tick at AE3_RATE */
    uint64_t tick_offset;       /* accumulated loop-unroll tick shift */
} ae3_clock;

void ae3_synth_clock(const ae3_synth *s, ae3_clock *out);

#endif /* AE3SYNTH_H */
