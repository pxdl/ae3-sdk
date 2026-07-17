# BGM — sequenced music (HD/BD banks + standard MIDI)

The Ape Escape 3 soundtrack is **sequenced, not streamed**: each song is a
soundbank plus a MIDI file, so tempo, instrumentation and arrangement are all
addressable data rather than a wall of finished audio.

> **Data policy.** The music, samples and sequences on the disc are Sony's. This
> document describes the *format and driver behavior* so a clean-room synthesizer
> can play the user's own copy; it ships no game data. See `../../NOTICE.md`.

The corpus is 62 soundbanks and 68 sequences; every structural claim below is a
census over those files.

---

## 1. Overview

Each song is a triplet under `debug/us/sound/bgm/` on the disc:

| File | Role |
|---|---|
| `NAME.hd` | bank header: programs → tones (key range, root note, ADSR, vol, pan) |
| `NAME.bd` | bank body: raw PS-ADPCM waveforms, back to back, no headers, no lengths |
| `NAME.mid` | the sequence — a **genuine standard MIDI file** |

The `.mid` half is an ordinary MIDI: `MThd`, format 0, 480 ppqn, with real
tempo, time-signature and key-signature meta events. Any DAW opens them. All of
the reverse-engineering effort is in the bank; the bank plus the sequence fully
specify playback, so any synthesizer that honours the fields documented here
reproduces the song.

## 2. Identifying the format

This is Sony's **"Jam"** bank. `.hd`/`.bd` PS2 banks are routinely conflated;
there are at least three unrelated formats:

| Format | Fingerprint | What it is |
|---|---|---|
| **ADS / SS2** | `SShd` at **0x00** | streaming audio — *not a bank* (vgmstream `sshd.c`, ffmpeg `ads.c`) |
| **SCEI HD/BD** | `IECSsreV` at 0x00 | the "classic" PS2 bank (VGMTrans reads it fully) |
| **Jam** | `SShd` at **0x0C** | ← **this one** |

Fastest disambiguation: read u32 at 0x00. Here it is the `.hd` file's own size,
`SShd` sits at 0x0C, and u32 at 0x04 equals the `.bd` size exactly (verified on
all 62 banks).

No public spec exists. The Sony originals are referenced by path but are not
online (`PS2SDK/P-sound/atools/Sndtool111/.../sformat.htm`; *PS2 IOP Library
Reference 3.0.2 — Sound Libraries*, pp. 66-68). The only third-party Jam parser
is [GT4SoundTool](https://github.com/Nenkai/GT4SoundTool), whose author states
their variant "simplifies some contents & arrangement."

> ### GT4SoundTool's layout does not fit AE3
> It is the only reference available, and it is wrong here in three ways. Each is
> falsifiable against these files:
>
> | GT4 says | AE3 reality | Test |
> |---|---|---|
> | split count = `countOrFlag & 0x0F` | `& 0x7F` | bank `s_23` has `0x97`: `&0x0F`→8, `&0x7F`→24. Section is 392 B = `8+24*16`. **`&0x7F` validates 533/533** |
> | sample addr = `u24` at byte 5 | `u16` at bytes 4-5 | bytes 6-7 are pinned at `0xff`,`0x80` in 3219/3537 tones — impossible for an address, exact for ADSR1 |
> | byte offset = `SSA * 0x10` | `SSA * 0x08` | `*0x10` overruns `p_7`'s `.bd` (389440 > 204048); `*0x08` fits (194720) and passes 2450/2450 |
>
> Reading bytes 6-7 as ADSR1 through the SPU2 bit layout decodes `0x80ff` to
> *exponential attack, shift 0 (instant), sustain level max* — a textbook
> sustained-instrument default. Under GT4's offsets that field would mean a very
> slow attack on nearly every tone in the game, which is implausible.
>
> Where GT4 **is** right is the field *semantics*: `prog[5]`=`0x7f` in 540/540 is
> `lfoTableIndex = none`; `prog[2]`=`0x40` in 540/540 is an unused pan; tone byte
> 15 bit `0x80` is a reverb send. Trust its meanings, not its offsets.

## 3. `.hd` — JamHeader

```
0x00  u32  hd_size            == the .hd file size            (62/62)
0x04  u32  bd_size            == the .bd file size            (62/62)
0x08  u32  0
0x0C  char[4] "SShd"
0x10  s32  program chunk off  == 0x80                         (62/62)
0x14  s32  velocity chunk off (varies; 58 distinct)
0x18  s32  LFO table off      (-1 in 61; 0x4a0 in s_20_park only)
0x1C  s32  SE sequence chunk  (-1 in all 62)
0x20  s32  unknown            (-1 in all 62)
0x24  s32  SE program chunk   (-1 in all 62)
0x28..0x80                    (-1 filler in all 62)
```

The `0xffff` fill from 0x18 on is **not padding** — it is a row of `-1` "chunk
absent" markers. `s_20_park`, the one bank with an LFO table, confirms this: its
`0x18` points to exactly `velocity_chunk + 130`, i.e. immediately after the
velocity chunk. A fixed-field header would not land contiguously by chance.

### Chunk convention (used by the program chunk *and* the LFO chunk)

```
s16 count                 <- LAST INDEX, not the count!
s16 offsets[count+1]      <- relative to the chunk start
```

**`count` is the last index.** There are `count+1` entries, and the final
entry's end comes from the *next chunk's* offset, not from the table. Three
independent proofs:

1. `first_offset == 2 + (count+1)*2` in all 62 — the table exactly abuts its
   first entry.
2. The MIDI's highest program number equals this field exactly: `p_7` 6=6, `j_6`
   6=6, `s_25_jungle` 10=10.
3. `p_7`'s "missing" 7th program sits at 0x280 with header `00 …` (1 tone) and
   ends at `SShd+0x14`.

The same convention parses `s_20_park`'s LFO chunk (count=0 → 1 entry,
`offsets[0]`=4 = `2+1*2`). One convention, two chunks, no special cases.

### Program record (8 bytes, tones follow inline)

```
0  u8  countOrFlag   0xFF => drum kit, tones = (byte7 - byte6) + 1
                     else => tones = (b0 & 0x7F) + 1 ; bit 7 is a separate flag
1  u8  base volume   multiplies each tone's volume
2  u8  pan           0x40 in 540/540 -> unused
3  u8  0             in 540/540
4  u8  pitch-related 0x0c in 540/540 (used only if a tone's flags & 0x10)
5  u8  LFO index     0x7f = none, in 540/540
6  u8  drum key low  } non-zero ONLY on drum kits
7  u8  drum key high }
```

Bit 7 of byte 0 (`countOrFlag`) is the "stack all matching tones" flag — see §8.

### Tone record (16 bytes)

```
0  u8  key low       } ignored on drum kits -- see the drum-key trap below
1  u8  key high      }
2  u8  root key      the key at which the sample plays at 44100 Hz (§6)
3  s8  fine tune     in 1/16 SEMITONE = 6.25 cents per step -- NOT cents (§6)
4  u16 sample addr   in SPU 8-byte units. 0xFFFF = silent key (drum kits only)
6  u16 ADSR1         raw SPU2 voice register
8  u16 ADSR2         raw SPU2 voice register
10 u8  0             read by nothing (§8)
11 u8  volume
12 u8  pan           0x40 = center; 0x00/0x7f pairs are hard-L/hard-R stereo layers
13 u8  BEND RANGE    pitch-bend range in SEMITONES (0x0c = 12 in 3247/3537)
14 u8  LFO index     0x7f = none, in 3477/3537
15 u8  flags         0x80 reverb send | 0x40 use prog LFO | 0x10 use prog bend range
                     | 0x20 pitch mod | 0x01 unknown (1196 tones, EE 0x003fed10, §8)
```

Key ranges tile correctly: program 1 of `p_7` has two tones covering **0–55 and
56–127** — no gap, no overlap. Melodic programs tile; drum programs set
`low == high`.

**Drum-key trap.** A drum tone's key comes from its *position* in the program's
range (`note - prog[6]`, per the note-on in §8); its own bytes 0/1 are zero.
Using bytes 0/1 as the key maps every drum sound onto key 0.

## 4. `.bd` — raw PS-ADPCM body

Same codec as the FMV audio (see `FMV.md`): 16-byte frames → 28 samples, 5
filter pairs, `shift > 12 → 9`. Frame flags (byte 1): `0x01` end, `0x02` repeat,
`0x04` loop start.

**Loop default.** `repeat` set with no explicit loop-start frame means loop to
the waveform's own start. Treating it as "no loop" silently kills the loop on
785 of 2450 waveforms and makes sustained instruments decay away.

### Address layout and the end-flag proof

Waveform lengths are stored nowhere; the end flag is the only terminator. That
is what makes the address test meaningful:

> If bytes 4-5 are a sample address in 8-byte units, every address must land on a
> frame boundary **immediately after** a frame carrying the end flag — because
> that is precisely what separates one waveform from the next.
>
> **2450 / 2450 addresses pass, across all 62 banks.**

Census over the corpus:

```
banks         : 62 parsed, 62 clean
programs      : 540
tones         : 3537  (58 silent-key sentinels, all inside drum kits)
sample addrs  : 2450/2450 frame-aligned AND preceded by an end-flagged frame
```

## 5. ADSR — raw SPU2 registers

Bit layout per psx-spx and VGMTrans `PSXSPU.h` (independent, agreeing sources):

```
ADSR1: 15 AttackMode(1=Exp) | 14-10 AttackShift | 9-8 AttackStep | 7-4 DecayShift | 3-0 SustainLevel
ADSR2: 15 SustainMode | 14 SustainDir | 13 - | 12-8 SustainShift | 7-6 SustainStep
       | 5 ReleaseMode | 4-0 ReleaseShift
```

**PS2 clocks the envelope at 48000 Hz, not the PS1's 44100** (VGMTrans
`PSXSPU.h:151`).

The dominant value `ADSR1=0x80ff, ADSR2=0x5fcb` (857 tones) decodes to:
exponential attack, shift 0 (instant); sustain level 15 → max, so decay never
engages; sustain shift 31 → hold; linear release, shift 11 → ~85 ms. A textbook
sustained instrument, and the sanity check that pins the field offsets (§2).

Not every tone holds flat: some author a **decreasing sustain** via
`SustainDir=1` (amplitude-linear fall), so a voice fades over its life from
note-on. A faithful renderer must honour the sustain mode/direction/shift/step
above, not treat sustain as a hold.

## 6. Pitch

The sound driver is not stripped: the IOP module `irx/3.0/sg2iopm1.irx` ships a
full `.symtab` (151 symbols including `ev_set_pitch` and `pitch_tbl`). The
**sequencer runs on the EE**, not the IOP; the IRX only executes voice commands.
Pitch is read from that code, not guessed:

```
R    = |note-root| % 12 ;  q = |note-root| / 12
idx  = (note>=root ? R*16 : (12-R)*16) + 208 + fine + (bendMSB-64)*range/4
p    = note>=root ? pitch_tbl[idx] << q : pitch_tbl[idx] >> (q+1)
pitch = (441*p/480) * F/4096            # F = 4096 by default (voice init 0x003fda2c/0x003fda80)
```

**Root key ⇒ 44100 Hz, and pitch 0x1000 is NOT the root.** `pitch_tbl[208] =
4096` exactly, and index 208 *is* `note == rootKey`. The tonal path then
multiplies by **441/480** (`ev_set_pitch` IOP `0x00000d34`-`0x00000d60`; the
magic `0x88888889 == ceil(2^40/480)` exactly), giving `4096 * 441/480 = 3763` →
`3763/4096 * 48000 = 44097.7 Hz`. The hardware fact that **pitch 0x1000 = 48000
Hz** is real (`ev_adpstm_set_pitch` IOP `0x0000306c`, magic `0x057619f1 ==
ceil(2^42/48000)`) — the tonal path deliberately un-does it. Assuming root →
0x1000 → 48000 leaves you **+1.47 semitones sharp** (480/441 = 1.0884).

> Independently corroborated: measuring 377 tonal looped waveforms by FFT (`R =
> f(root) × period`) gives a median of **43548 Hz**, with 47.5% within 6% of
> 44100 and **0.5% of 48000**. Two unrelated methods, same answer. That also
> confirms the **root key** reading, since a wrong root would skew R by 2^(n/12).

### Fine tune (byte 3) is 1/16 semitone, not cents

Byte 3 is loaded signed (`lb` at EE `0x003f896c`) and added **straight** to the
`pitch_tbl` index, which itself steps in 1/16 semitone (192 entries per octave);
no scaling is applied anywhere (IOP `0x00000bc4`). So `fine = -16` is exactly
**-1.00 semitone**, and the game's observed range of -2..-14 means
**-12.5..-87.5 cents**. Reading it as cents leaves 90% of tones (3129/3479) up to
**78.8 cents sharp** — nearly a semitone.

### Pitch bend range = tone byte 13 (semitones)

The game has **no RPN**: CC 100/101 both dispatch to a bare `jr ra` stub shared
with 102 other unassigned CCs, so the range is bank data, not a synth default.
Note-on picks it at EE `0x003f8830` (a branch-*likely* pair — the delay slot
runs only when taken): `flags & 0x10` → use `prog[4]`, else `tone[13]`.

| effective range | tones |
|---|---|
| **12 semitones** | **3247 (93.3%)** |
| 0,1,2,3,4,6,7,8,11 | 232 |

`prog[4] = 12` in 540/540. The 58 tones setting `flags & 0x10` are exactly the 58
silent drum sentinels (byte 13 = 0xff), which fall back to `prog[4]`. Full
deflection: the driver computes `(bendMSB-64)*range/4` sixteenths of a semitone,
so `|D-64|=64` → exactly `range` semitones. **The LSB is discarded**
(`sra s0,s0,7`), so `+8191 → +11.81 st` but `-8192 → -12.00 st`.

Because the range is per-tone, a program whose tones disagree has no single
channel-wide range. **6 of 540 programs mix bend ranges across their tones, and
all 6 are actually bent by their sequence:**

| bank | prog | tone ranges | keys the sequence bends |
|---|---|---|---|
| `s_9` | 3 | [0, 8] | 6 keys→8, 4 keys→0 |
| `j_7` | 2 | [0, 12] | 1 key→12, 5→0 |
| `s_19_less_chord` | 2 | [0, 12] | 2 keys→12, 4→0 |
| `s_14` | 8 | [1, 12] | 6 keys→12, 2→1 |
| `s_29` | 3 | [1, 12] | 20 keys→12, 3→1 |
| `s_20_christmas` | 4 | [11, 12] | 2 keys→12, 1→11 |

A synth with per-zone bend range reproduces these directly. A per-channel-only
synth (e.g. General MIDI) must resolve the range under each key the channel
actually plays and split minority keys onto spare channels (never channel 9,
which GM forces to percussion); where no channel is free, the dominant range
applies. `s_20_christmas` is the only bank that runs out of channels.

### The 0x3FFF pitch ceiling — respected by the data

The SPU2 clamps the voice pitch register at **0x3FFF = 4× the 48 kHz core rate**
(psx-spx; PCSX2 `UpdatePitch`). With the 441/480 scale that is **root + 25.47
semitones** on a 44.1k sample. A scan of every note-on in all 68 sequences (fine
tune and live bend state included) finds **zero strikes past it**, and the margin
is no accident: the hottest note in the game (`s_19_less_chord` key 91 on a
root-65/tune-9 tone) lands at **+25.44**, and that zone's key range tops out at
91 exactly because **key 92 would cross the ceiling**. The layered "shimmer"
voicings therefore genuinely run at up to ×4.35 speed on the console — that is
their intended sound, not a defect — and no renderer clamp is needed on this
corpus.

### Two `pitch_tbl` entries are corrupt in the game

606/608 entries match `round(2048 * 2^((i-16)/192))` within ±1. Two do not, and
both break monotonicity — they look like digit transpositions in Sony's table.
The table is at **file offset `0x43b0`** in `sg2iopm1.irx` (located by requiring
`[208] == 4096`; 608 u16 entries); these two are the *only* two monotonicity
breaks in the whole table:

| index | shipped | should be | looks like |
|---|---|---|---|
| `[64]` | 2360 (`0x0938`) | 2435 (`0x0983`) | digit transposition `938`↔`983` |
| `[172]` | 3340 (`0x0D0C`) | 3597 (`0x0E0D`) | digit transposition `D0C`↔`E0D` |

**Both are reachable.** `[172]` is hit by a note at its root bent down ~1.1
semitones — a common gesture — producing a ~128-cent error. To reproduce this
path faithfully, **copy the table bytes; do not regenerate it from the formula**,
or you will "fix" an audible artifact of the original. A renderer that computes
pitch continuously (rather than indexing the shipped table) will not reproduce
it.

## 7. Songs and sequences

70 stems = 62 banks + 8 bank-less sequences. Two banks (`b_4`, `b_3_stage`) ship
**no sequence of their own** and are shared by prefix-matched ones. The program
counts fit *exactly*, which confirms the sharing rather than merely suggesting
it:

| Bank | programs | Sequences | highest program used |
|---|---|---|---|
| `b_3_stage` | 17 (0-16) | `b_3_stage_a`, `b_3_stage_b` | 6, **16** |
| `b_4` | 13 (0-12) | `b_4_slow_brass`, `b_4_fast_brass_2` | 11, **12** |

**60 self-contained + 4 shared = 64 playable songs.**

Four orphan sequences have no bank at all: `j_2_f`, `j_3`, `j_4`, `j_9_retake`
need 10, 8, 8 and 9 programs, but the only `j_` banks (`j_6`, `j_7`) have **7**.
They fit nothing on the disc — apparently unused leftovers.

Name prefixes: `s_` stage, `b_` boss, `p_` ?, `j_` ?, `m_` menu/cinema, `o_` ?.

### The MIDI header is parsed at EE `0x00400d90`

Searching the ELF for `MThd`/`MTrk` finds nothing, which looks like the game
ignores the header. It does not: `FUN_00400d90` reads the chunk and simply
**never compares the magic**, only its contents. It validates the BE u32 at +4
`== 6` (header length), the BE u16 at +8 `== 0` (format), the BE u16 at +10 `==
1` (ntrks) — returning -1 on any mismatch — then reads the **BE u16 at +12 (the
division)**, converts it to a double (`jal 0x00424278`) into `seq+0x38`, and
advances the pointer **+14**. A sibling at `0x00400e50` then skips the MTrk
chunk's 8 bytes. `seq+0x38` is consumed by the tempo math at `0x00402568`. So
**ppqn comes from the file and 480 is correct** — the sequencer is not running
off a hardcoded division.

### Tempo is exact

Meta 0x51 @ `0x004024ac` reads a 24-bit µs/quarter and computes `60000000.0 /
µs` in software doubles.

### Two defects in the game's own sequencer

- **Time-signature meta 0x58** @ EE `0x0040249c` computes the denominator as
  `d*d` instead of `2^d` (`mult t7,t7,t7`). Correct only for 4/4 (d=2 → 4); 6/8
  (d=3) yields 9, not 8. It affects bar bookkeeping, not µs/quarter, so it does
  not shift tempo — and it is **inert**: the result is stored to `seq+0x20` (at
  `0x004024a8`, `0x00402a1c`, `0x00402a48`) and that field is never read anywhere
  in the sequencer (write-only). `s_9` is the one live non-4/4 case (opening bar
  1/8, d=3 → 9) and its timing still does not shift.
- **Silent command drops** @ EE `0x003fbd28`: `slti t7,t7,1024; beq
  t7,zero,<skip>` drops the command with no error when the 1024-entry queue is
  full. The path is a 4-stage pipeline (MIDI → byte FIFO `0x0074e840` → 512-byte
  ring → synth → `com_q`), so congestion degrades silently.

## 8. Driver behavior

### BGM note-on: `FUN_003facb8`

This is the definitive note-on for sequenced music. (`FUN_003f8690` is the
separate SE / 4-byte-event API; its `(note - key0) * 16` indexing produces
garbage and out-of-range reads for melodic programs, refuting it as the BGM
path.) The algorithm:

- **drum program** (`h[0] == 0xFF`): index the single tone at `note - h[6]`, per
  key;
- **melodic**: scan tones **in table order**; skip while the key is outside
  `[lo, hi]`; a matching tone whose bend byte 13 is `0xFF` **aborts the whole
  note-on** (the silent sentinel); otherwise start the voice — then **return
  immediately unless `h[0]` bit 7 is set**. **Bit 7 of the program count byte =
  "stack all matching tones."** 261 melodic programs set it (including every
  layered design, e.g. `s_19`/6 and `s_9`/3); 272 do not.
- Census over all 62 banks: in first-match-only programs, **zero keys have more
  than one matching tone.** The data is authored unambiguously, so play-first and
  play-all are indistinguishable on this corpus.

The same function reads each field: root=byte2 / fine=byte3 → pitch; addr bytes
4-5 (`<< bank_shift + bank_base`); ADSR1/ADSR2 (with per-channel override slots);
byte 11 volume into the linear multiply chain; byte 12 pan **clamped 1..127**;
byte 13 / `prog[4]` bend range; byte 14 / `prog[5]` LFO; flags `0x80` reverb bus
/ `0x20` LFO-on / `0x10` use-prog-bend / `0x40` use-prog-LFO. **Byte 10 is read
by nothing** — confirmed unused.

### Volume

Program `base volume` (byte 1) multiplies each tone's `volume` (byte 11), which
feeds the driver's linear multiply chain at note-on.

### CC census and voice modulation

Full CC census over the 68 sequences: CC1 ×2104, CC2 ×4, CC6 ×62, CC7 ×573, CC8
×1, CC9 ×1, CC10 ×2507, CC11 ×1755, CC66 ×1, CC99 ×128 — and nothing else, ever.
The CC dispatch table is at EE `0x0069dea8` (128 entries; CC100/CC101 both land
on the `0x003fa940` bare-`jr ra` stub shared by 102 unassigned CCs, which is what
identifies the table).

CC1 (modulation) → `0x003f9e40`: stores the value at `chan[ch*0x70 + 772]`, then
walks every **active** voice on the channel calling `FUN_003feea8(voice, value)`:

| function | writes | sets `cmd |= 0x400` only if |
|---|---|---|
| `FUN_003fedf8(v, p)` | **rate** = `240/(60 - p*58/127)` → `cmd+0x80` | **depth** (`cmd+0x84`) ≠ 0 |
| `FUN_003feea8(v, p)` | **depth** → `cmd+0x84` | **rate** (`cmd+0x80`) ≠ 0 |

The guard is symmetric: modulation needs **both** rate and depth. Only the
note-on sets the rate, and only for `flags & 0x20` (passing `p=10` → rate 4,
depth `0x7f`). **Exactly 2 of 3479 tones set flag 0x20**, and only ONE channel in
the game (`s_20_park` ch4 — the one bank with an LFO table) ever has CC1 reach
one. On the other 7 CC1 channels the modulation wheel does nothing on hardware.

### Reverb

Reverb is standing state, not a per-song knob. At EE `0x0035fa1c` the game sets
`type = 0x104` (CLEAR_WA | 4 → `SD_REV_MODE_STUDIO_C`) and `depth = 0x1e` (30) on
**both** cores and never touches them again; coefficients come out of Sony's
`libsd.irx` at runtime. Across all 68 sequences **CC91 (reverb send) = 0 and CC98
= 0**; the only CCs the game ever sends are 1, 2, 6, 7, 8, 9, 10, 11, 66, 99. The
one per-note reverb variable is tone flag `0x80` (reverb send on/off).

### LFO table

Parsed but only present on `s_20_park`. `flags & 0x40` selects `prog[5]` over
`tone[14]` as the LFO index; both are `0x7f` (= none) in nearly all tones.

### Open: tone flags bit 0

Bit 0 of tone byte 15 is set by 1196 of 3479 tones (34%) and reaches EE
`0x003fed10`. **It is not used by the BGM path:** the `flags & 1 → FUN_003fed10`
check exists only in the SE note-on path (`FUN_003f8690`); the BGM note-on
(`FUN_003facb8`) never reads bit 0. `FUN_003fed10(voice)` takes no parameter and
does exactly one thing — `cmd[0] |= 0x04`; its sibling `FUN_003fed58(voice)` sets
`0x08` from a per-channel state at `chan+0xb9`. These are the only two bits set by
a lone function each; every other command bit is a shared dirty-marker for a
field group. The flush at `0x00400020`+ tests `0x100`, `0x1000`, `0x2000`,
`0x4000`, `0x400`, `0x80` — and never `0x04`/`0x08`, so the consumer is elsewhere
and unidentified. "Noise" is a tempting but unconfirmed guess: the IOP does
export `ev_set_noise` (`0x10b8` → `sceSdSetSwitch(0x1400/0x1401)` = `SD_S_NON`),
but 34% of tones is far too many for a noise voice, and GT4SoundTool does not
name this bit either. Whatever it does, sequenced music cannot trigger it.

### Open: Dolby matrix encoding

`sound_output_method = dolby` is selectable. Whether the BGM path is
matrix-encoded is untested; the hard-L/hard-R tone pairs (byte 12 = `0x00`/`0x7f`)
are a plausible place to look.

### Authored artifacts (not rendering bugs)

Two audible traits are in Sony's data, verified against the original game audio,
and must not be "fixed":

- **`b_8`'s intro→loop pause.** A ~0.6 s gap at 0.84–1.46 s: the voice plays a
  one-shot 0.721 s sample held for 1.34 s with no loop point, so it simply runs
  out of sample data.
- **`s_9`'s high-frequency hiss.** An authored voice — program 0's key-44
  noise-burst percussion (addr 45764, vol 57, flags 0), ~200 ms speckled noise
  columns to ~18 kHz every ~322 ms. Present in the original rip; nothing gates
  it.

## 9. Provenance

Everything here came from the files, in this order: chunk tags → size-prefix
check → program-table arithmetic → MIDI cross-check (the off-by-one) → end-flag
address test → field statistics over all 3537 tones. Web research (GT4SoundTool,
psx-spx, VGMTrans) came *after* and served only to name fields and supply the
SPU2 bit layout and the 48 kHz clock; where it conflicted with the files, the
files won (§2).

Facts about hardware and driver behavior are cited inline to their source: EE and
IOP addresses (`FUN_00xxxxxx`, magic constants) are this project's own
disassembly of the game's stripped R5900 ELF and the IOP module `sg2iopm1.irx`
(which ships a symbol table); SPU2 register and interpolation behavior is from
psx-spx and VGMTrans.
