# SE — sound-effect banks (`sound/se/*.hd` + `*.bd`)

The sound-effect family of Ape Escape 3: **101 `.hd`/`.bd` bank pairs** (20.1 MB)
under `debug/us/sound/se/` inside DATA.BIN — one bank per area or context
(`arabian`, `bay_a..e`, `boss_*`, `space_a..j`, `toyhouse_a..h`, the character
voice banks `p_N_voice_b/g`, `pcvoice*`, the Metal-Gear crossover `mgs*`, …).

An SE bank is the **same Sony "Jam" soundbank as a BGM bank** — `SShd` at
`0x0C`, the same 16-byte PS-ADPCM tone record, the same `.bd` codec — so this
document is written as **deltas against [`BGM.md`](BGM.md)**; read that first.
The one structural difference is where the *sequence* lives: a BGM song ships a
separate standard `.mid`, whereas an SE bank **carries its own little sequences
inside the `.hd`** (the seseq chunk) and its programs are always positional, so
a cue plays a fixed key rather than a melodic range.

> **Legal.** Extracted data is Sony's copyrighted content. This document
> describes the format only; it does not redistribute any asset.

Every claim below is either read from the shipped EE executable (`SCUS_975.01`,
addresses cited) or measured across the whole 101-bank corpus; the census and
the parser agree everywhere. Facts that hold in the corpus but are not enforced
by the driver are marked *(corpus)*.

---

## 1. Relationship to the BGM bank

Both formats are Sony's "Jam" bank (`SShd` at `0x0C`; [`BGM.md`](BGM.md) §2).
They share one loader: the sg2 library function whose asserts name it
**`vab_set`** (`FUN_00402938`, source `sg2vab.c`). What differs is which of the
header's six chunk slots each fills. The header slot table
([`BGM.md`](BGM.md) §3) has always carried six named `s32` offsets; BGM leaves
three of them −1 and this format is what fills them:

| slot | `+off` | BGM bank | SE bank |
|---|---|---|---|
| program | `+0x10` | `0x80` (melodic) | **−1** (100/101) |
| velocity | `+0x14` | varies | `0x80` |
| LFO | `+0x18` | −1 (only `s_20_park`) | present in **43/101** |
| **SE sequence** | `+0x1C` | −1 (all 62) | **present** (all 101) |
| **unknown** | `+0x20` | −1 (all 62) | **present** (all 101) |
| **SE program** | `+0x24` | −1 (all 62) | **present** (all 101) |

So an SE bank is the mirror image of a BGM bank: it empties BGM's melodic
**program** slot (`+0x10`) and populates the three **SE** slots BGM leaves
empty. Its instruments live in the **SE program chunk (`+0x24`)**, not the
melodic program chunk. (One bank, `mgs_saru`, is a **hybrid** — it carries
*both* a melodic program chunk at `+0x10=0x80` and an SE program chunk — because
that Metal-Gear cutscene bank is driven both ways.)

## 2. Header

Identical to BGM ([`BGM.md`](BGM.md) §3) except the slot fill above and the two
size prefixes:

```
0x00  u32  hd_size   == filesize - 0x180  (100/101; space_c: -0x300, see §7)   -- IGNORED by the driver
0x04  u32  bd_size   == the .bd file size (101/101)                            -- IGNORED by the driver
0x08  u32  0                                                                   (101/101)
0x0C  char[4] "SShd"                                                           -- the ONLY field validated
0x10  s32[6]  chunk offsets (table above); -1 = absent
0x28..0x80    -1 filler (101/101)
```

**Only `+0x0C` is validated.** `vab_set` (`FUN_00402938`) checks
`*(u32*)(bank+0x0C) == 0x64685353 'SShd'` and reads nothing at `+0x00` or
`+0x04`. So `hd_size` and `bd_size` are informational — an authoring-tool
artifact the driver never consults. `hd_size` is a genuine under-count: the real
chunk data runs all the way to EOF (proven by computing the last program's end
from its own header — it lands at filesize exactly, gap 0, on all 58 banks whose
last chunk is the SE program chunk), so `filesize − hd_size` is a fixed `0x180`
that corresponds to no boundary. Our parser still checks both prefixes as a
sanity gate; neither is load-bearing.

**Mode.** `vab_set` sets the VabDesc kind from `bank+0x7c`:
`(bank+0x7c == -1) ? 3 : 4`. `+0x7c` is inside the −1 filler on the whole corpus
(BGM and SE), so the kind is **always 3**; the `4` branch is unreached here.

## 3. The parser relocates the chunk table in place

`vab_set` (`FUN_00402938`, called by `FUN_00402e38` after it allocates a free
VabDesc slot) resolves each of the six offsets to an **absolute pointer written
back into the bank's own `−1` filler region**, at `bank+0x30..+0x44`:

```
for slot i in 0..5:   *(bank + 0x30 + 4*i) = (bank[+0x10+4*i] == -1) ? 0 : bank + bank[+0x10+4*i]
```

so `bank+0x30`=prog, `+0x34`=vel, `+0x38`=LFO, `+0x3c`=seseq, `+0x40`=unk,
`+0x44`=SE-prog. The VabDesc entry (`DAT_0074f840 + id*0x2c`, 32 slots) then
caches the bank base; the accessors index off these resolved pointers:

| accessor | reads | returns |
|---|---|---|
| `vab_get_prog` `FUN_00402cf8` | `bank+0x30` | melodic program `idx` (BGM; `0xFFFF`=empty) |
| **`vab_get_seprog`** `FUN_00402c68` | `bank+0x44` | SE program `idx` |
| **`vab_get_seseq`** `FUN_00402b18` | `bank+0x3c` | one sequence event stream (§6) |

Each is the standard Jam offset table ([`BGM.md`](BGM.md) §3): `s16 count`
(= **last index**, not the count) then `count+1` u16 byte-offsets, `base +
offset[idx]`.

## 4. SE program chunk (`+0x24`)

The bank's instruments. Same Jam chunk convention and same 8-byte program
record + inline 16-byte tones as BGM, but **every SE program is "drum-form"**:

- **program header byte 0 == `0xFF`** in all 156 SE programs across the corpus.
  So a program is always the drum-kit layout ([`BGM.md`](BGM.md) §3): byte 6 =
  `key0` (low key), byte 7 = `keyN` (high key), **tone count = `keyN − key0 + 1`**,
  one tone per key.
- The note-on selects a tone **positionally**: `tone = tones[note − key0]`
  (`FUN_003f8690`, `(note − key0) * 0x10`), guarded by `note >= key0`. There is
  **no key-range scan and no stack flag** — the melodic machinery BGM uses
  (`BGM.md` §8) does not apply.

Census (100 clean banks + `space_c` via its corrected slot): **156 programs,
5923 tones, and every tone sample address passes the end-flag test** — each is
frame-aligned in the `.bd` and immediately preceded by an end-flagged frame,
exactly the test that validates BGM ([`BGM.md`](BGM.md) §4). The `.bd` codec is
byte-for-byte the BGM decoder (gate below).

## 5. Tone record — deltas vs BGM

The 16-byte tone record ([`BGM.md`](BGM.md) §3) is **field-for-field identical**
where it matters to synthesis — root `+2`, fine `+3`, sample addr `+4..5`,
ADSR1/2 `+6`/`+8`, volume `+11`, pan `+12` (clamped 1..127), bend `+13`/prog[4],
LFO `+14`/prog[5], flags `+15` — all read the same way by the SE note-on. Three
deltas, all read from `FUN_003f8690`:

- **byte 0 = voice cut-group** (BGM: key-low, unused here because indexing is
  positional). If nonzero, the note-on calls `FUN_003ff7f8(module, byte0)`,
  which finds active voices tagged with the same module and cut-group, overwrites
  their ADSR words with `(0, 8)`, then issues key-off.
  This is the standard "a sound must not overlap itself" mechanism — looping and
  one-shot cues that should be monophonic share a nonzero group; `0` = no cut.
- **byte 1 = voice metadata.** `FUN_003f8690` passes it as the final argument to
  `FUN_003fe360`, which stores it at live-voice word `+0x18`. It does not enter
  the traced volume, pan, pitch, ADSR, LFO, noise, key-on, or key-off calculations.
  The clean-bank census is 5,856 tones: `0x0A` occurs 5,362 times; the other
  values are `0x7E`×299, `0x14`×140, `0x7F`×36, `0`×17, `0x5A`×1, `0x64`×1.
- **flag bit 0 (`0x01`) is consumed here.** The SE note-on calls
  `FUN_003fed10(voice)`, which sets live command bit `0x04`; BGM's note-on never
  tests the tone flag. No traced volume, pan, pitch, ADSR, LFO, noise, key, or
  SPU2-register calculation reads that command bit. It is set on 1,599/5,856
  clean-bank tones.
- **flag `0x20` hard-arms the LFO.** BGM treats `0x20` as inert (it needs a live
  CC1+CC2 pair, `BGM.md` §8); the SE note-on instead calls the rate setter with
  a fixed rate and the depth setter with `0x7f` whenever `0x20` is set, so an SE
  tone can carry built-in vibrato with no controller input. Flags `0x80` reverb,
  `0x40` use-prog-LFO, `0x10` use-prog-bend behave as in BGM.

## 6. SE sequence chunk (`+0x1C`) — embedded cue bytecode

Where BGM ships a `.mid`, an SE bank stores its sequences inline as a
**two-level Jam chunk**, proven by `vab_get_seseq(vab, cue, layer)`
(`FUN_00402b18`):

```
seseq = bank + 0x3c                        # resolved base (ushort*)
cue_off   = seseq[1 + cue]                 # u16, byte offset from seseq base; even
inner     = seseq + cue_off                # this cue's sub-table
layer_off = *(u16*)(inner + 2 + 2*layer)   # u16, byte offset from seseq base; byte-granular
return      seseq + layer_off              # -> the layer's event stream
```

Both levels use the Jam `[count=lastindex][u16 offsets]` convention, and — the
one subtlety — **every offset value is relative to the seseq base**, not to its
own sub-table (the accessor returns `seseq + off`). Outer offsets are even (they
are `>>1`'d into the u16 array); inner offsets are byte-granular pointers into
a shared event-stream pool that follows the tables.

A gameplay **bank/request pair** selects one stream. Structurally, the outer
table is a bank/group and its inner table contains request streams; these were
provisionally called a cue and parallel layers during the format pass. `SeDesc`
routing (§6.4) proves that the inner entries are individually selected effects,
not layers that are automatically played together.

### 6.1 Walker and timing

The real walker is **`FUN_003fce80`**, registered as sound-thread callback
**slot 1** by `FUN_003fd1a0` (`FUN_003f6250(1, 0x003fce80)`). This corrects the
format-pass handoff's provisional attribution of the walker to `0x003f9dd0`:
`FUN_003f9d40` (which contains that address) is the downstream **A-status
driver handler**, reached through the channel-event table at `0x0069e0a8`.
The callback order is BGM SMF walker slot 0, SE walker slot 1, driver-ring drain
slot 2, voice flush/free slot 3, and the separate four-byte command queue slot 4.

Cue-open `FUN_003fc3c8` initializes one 0x48-byte request module as follows:

| module offset | walker meaning |
|---|---|
| `+0x00` | state: `1` active, `2` ended |
| `+0x1C` | float advance per sound callback |
| `+0x20` | float elapsed time for the current delay |
| `+0x24` | float current delay |
| `+0x2C` | immutable stream base |
| `+0x30` | current read pointer |
| `+0x34` | control-flow handler already consumed the following delta |
| `+0x38` | running status |

The authored clock is **480 ticks/second**: cue-open stores
`advance = 480.0 / boot_rate`, hence `8.0` per NTSC 60 Hz callback and `9.6`
per PAL 50 Hz callback. Each callback first adds `advance` to `elapsed`, then
dispatches every event whose `delay <= elapsed`. After a normal event it reads
the following VLQ delay and resets `elapsed = 0`; a zero-delay event therefore
runs in the same callback. This is a relative-delay clock, unlike the BGM
walker's running absolute MIDI-tick sum. Console dispatch is quantized to the
sound callback grid; an exact renderer uses 100 output samples per SE tick at
48 kHz. As with BGM, a renderer may normalize the arbitrary cue-open-to-callback
phase so the first due event lands at sample zero.

### 6.2 Event grammar

`FUN_003fce80` treats a byte with bit 7 set as a new status and otherwise reuses
the status at module `+0x38` (running status). It dispatches by `status >> 4`
through the 16-entry table at **`0x0069e1a0`**. Only three handlers are live:

| bytes | EE handler | meaning |
|---|---|---|
| `A0 note velocity program` | `FUN_003fd3d8` → driver `FUN_003f9d40` | custom SE note event; nonzero velocity starts `seprog[program]` at the positional `note`, velocity zero stops matching `(bank, program, note)` voices |
| `B0 command ...` | `FUN_003fd488` | SE voice automation / stream control (below) |
| `FF 2F 00` | `FUN_003fd8d0` | end layer: null read pointer, state `2`, emit the driver's `FF 3F 00` completion event |

Every other high-nibble entry is the bare `jr ra` stub at `0x003fd3d0`.
After every non-ending event comes a standard big-endian **VLQ relative delay**
(`FUN_003fd318`: `value = value*128 + (byte&0x7F)` until bit 7 clears).

`B0` always consumes `command` plus three parameter bytes. Commands `07`, `0A`,
and `41` consume a fourth parameter byte:

| command | parameters after command | EE-proven behavior |
|---|---|---|
| `01` | `value, program, note` | set LFO **depth** on active voices of this cue module matching program/note |
| `02` | `value, program, note` | set LFO **rate** on the same selection |
| `07` | `duration, target, program, note` | ramp note-on velocity (`voice+0x50`) on active voices of this VAB matching program/note |
| `0A` | `duration, target, program, note` | ramp tone pan (`voice+0x5C`) on that selection |
| `41` | `duration, signed_target, program, note` | pitch glide on that selection; target is signed tenths of a semitone, converted by `×1.2` into the driver's 1/12-semitone accumulator |
| `60` | `target_lo, target_hi, count` | jump to `stream_base + u16le(target)`; `count=0` loops forever, otherwise jump while the per-layer counter differs from `count`, incrementing it after each jump |

Commands `01/02/07/0A/41` are repacked as the driver's private `F0 00 7F/7E`
messages, then applied by `FUN_003fb818`. The exact ramp formula read there is
`step=(target-current)*(480/rate)/(duration*4)` for `07/0A`; command `41` uses
`total=signed_target*1.2`, `step=total/(duration*4)`. Command `60` consumes its
following VLQ before changing the read pointer, so the main walker does not read
a second delta at the jump seam.

### 6.3 Whole-corpus grammar gate

Measured over all **2699 request streams** (including `space_c`, whose seseq is
intact): **11,408 events** = 7,233 `A0`, 1,476 `B0`, and 2,699 `FF 2F 00`.
The six and only six B commands are `01`×108, `02`×112, `07`×233, `0A`×16,
`41`×915, `60`×92. Every event carries an explicit status even though the
walker supports running status; every non-ending event carries an exactly
two-byte VLQ (8,709/8,709), and zero is canonically encoded `80 00`
(5,422/5,422). Delays span 0..6240 ticks. Of the 92 jumps, 91 use `count=0`
(infinite) and one uses `count=1`; every jump's following delay is zero.

### 6.4 Gameplay routing: `SeDesc` name → `(VAB, bank, req)`

The game does not identify effects by an event-stream offset. Its separate
`SeDesc` exdb tables map a gameplay name to the two indices consumed by
`vab_get_seseq`. `FUN_001bc0bc` materializes each row as:

| runtime offset | exdb field | absent-field default |
|---|---|---|
| `+0x00` | `name[32]` | `"noname"` |
| `+0x20` | `loop[32]` | `"no"` |
| `+0x40` | `reverb[32]` | `"system"` |
| `+0x60` | `MaxDistance` (`f32`) | `500.0` |
| `+0x64` | `MiniDistance` (`f32`) | `20.0` |
| `+0x68` | `VolCurve` (`s32`) | `0` |
| `+0x6C` | `bank` (`s32`) | `0` |
| `+0x70` | `priority` (`s32`) | `10` |
| `+0x74` | `req` (`s32`) | `0` |

`FUN_0035b010` searches the loaded `SeDesc` owners by `name` and retains the
owner of the matching row. `kick3D` `FUN_0035b258` then obtains that owner's
loaded VAB id and calls `FUN_005b514c(vab, row.bank, row.req, ...)`. The runtime
constructor `FUN_0035c7ec` forwards those first three words unchanged to
`FUN_003fc3b0`/`FUN_003fc3c8`, hence:

```
gameplay name -> owner VAB
               -> outer seseq index = SeDesc.bank
               -> inner stream index = SeDesc.req
```

The field name `bank` therefore means the **outer index inside that VAB**, not
the VAB id. `req` selects exactly one inner event stream. Concrete design-data
rows corroborate the code: `arabian` outer 5 has requests 0/1/2 named
`st_brk_rock01`, `st_brk_statue`, and `st_brk_plate`; outer 25 requests 0/1
are `st_cpt_flyingcarpet_start` and `_loop`. The filename of a `SeDesc` table
is not itself the VAB binding; the matched owner supplies the handle.

For 3D triggers, `FUN_00363dc8` consumes the row's distance block. With
`VolCurve == 0`, let `B = clamp(global_effect_volume * 128, 0, 128)` and `d`
be listener distance:

```
d < MiniDistance                    -> B
MiniDistance <= d <= MaxDistance    -> B * (1 - (d-Mini)/(Max-Mini))
d > MaxDistance                     -> 0
```

A nonzero `VolCurve` bypasses that distance ramp and returns `B`. This matters
for the only two nonzero rows in the US corpus: both use `0x7FFFFFFF` and carry
NaN min/max values, which are consequently not evaluated. There is **no
per-effect master-volume field** in `SeDesc`; `VolCurve` is a selector, not a
gain. Direct SE-module volume is the requested byte multiplied by runtime/global
effect gains and clamped to 0..127 (`FUN_0035c378`).

The US corpus has 114 `SeDesc` tables / 4,262 rows. All present `loop` values
are `"no"` (2,353), all present `reverb` values are `"system"` (2,353), and
`priority` is 10 except three rows (100, 11, 12); omitted fields receive the
defaults above. More importantly, all five call sites of `FUN_0035b010` were
traced: the trigger paths read the distance block, `bank`, and `req`, while no
consumer reads the row's `loop`, `reverb`, or `priority`. In this executable
those three columns are retained design metadata, not playback controls.
Sequence repetition is authored explicitly by `B0 60`; module reverb is
controlled through the cue-open `0x10000` flag / system parameter path.

## 7. The other chunks, and the one corrupt bank

- **Velocity chunk (`+0x14`, at `0x80`).** `s16 count` (= 0) then a **128-byte
  identity ramp** `0,1,…,127` (101/101) — velocity passes through unchanged, so
  SE dynamics are not remapped. (The count/curve pair matches BGM
  [`BGM.md`](BGM.md) §3; BGM's is likewise identity.)
- **LFO chunk (`+0x18`).** Present in **43/101** banks. Same Jam convention and
  same 120-byte entries as `s_20_park` ([`BGM.md`](BGM.md) §8): 60-byte pitch
  waveform + 60-byte amplitude waveform, and the **final entry is EOF-truncated
  to 64 bytes** (60 pitch + 4) on all 43 — identical to the BGM case, and for the
  same reason (the amplitude half is never armed).
- **Unknown chunk (`+0x20`).** A fixed **784-byte block, byte-identical across
  all 101 banks** (16-byte header `64 00 …` + 48× 16-byte records with an
  incrementing index and constant `{vol 0x64, pan 0x40, 0x04, 0x64}` fields —
  a default 48-entry parameter table stamped by the authoring tool). `vab_set`
  resolves it to `bank+0x40` but **no traced code reads it**; being constant, it
  carries no per-bank information.
- **`space_c` is shipped corrupt.** Its unknown chunk is bloated by `0x180`
  (24 duplicated records), which shifts the SE program chunk to `0x8d2` while the
  header slot still reads the stale `0x752` and the `.hd` tail is `0x300` instead
  of `0x180`. The driver, reading the stale slot, would misparse it too — the
  file is effectively broken (an unused `space_*` variant). The tool refuses it
  by the tail signature rather than silently correcting it; the gate isolates it
  as the one known-corrupt bank and still codec-checks its `.bd` via the
  corrected slot.

## 8. Runtime and offline playback model

The SE module shares the 48-voice SPU2 pool with BGM (round-robin, never steals;
`sg2slotctrl.c`, `BGM.md` §M5). Its note-on **`FUN_003f8690`** reads the shared
tone fields plus §5's SE fields, then uses the same pitch, ADSR, pan, LFO, reverb,
Gaussian interpolation, and integer SPU2 bus path documented in `BGM.md` §8.
Tone flag `0x02` selects the SPU2 noise source instead of ADPCM and supplies the
six-bit per-core noise clock from tone byte 2; the source is shared by each
24-voice core, exactly as the SPU2 mixer exposes it.

The offline `serender` harness drives this path through the native core:

1. `ae3_synth_load_bank` recognizes the SE slot pattern and parses its positional
   programs, seseq chunk, and optional LFO table.
2. The harness-internal loader resolves exactly the `bank,request` Jam indices
   from §6.4, validates the selected bytecode through `FF 2F 00`, and resets the
   embedded walker.
3. The render loop dispatches `A0`, every `B0` command, and `FF 2F 00` through
   the shared synth. Exact mode uses 100 output samples per authored tick; console
   mode reproduces the EE walker's per-delay 60 Hz quantization. Voice ramps
   still step at the driver's 60 Hz flush cadence in both modes.

The exported runtime API selects one request with `ae3_synth_load_se`; callers
provide their own `.hd`/`.bd` and may optionally provide extracted
`sg2iopm1.irx` pitch data and `libsd.irx` STUDIO_C coefficients. The library and
offline harness contain no game data.

`ae3_synth_set_loop` lets a host control an authored `B0 60` with `count=0`:
`0` falls through after the first pass, `1..126` takes that many jumps, and
`AE3_LOOP_FOREVER` preserves console behavior. A nonzero count embedded in the
request remains authoritative. `serender` selects `AE3_LOOP_FOREVER`, so an
infinite request remains live until its render cap.

## 9. Tooling

`ae3 se` (`ae3tools/se.py`):

```
ae3 se BANK.hd                 # header, chunks, programs, bank/request counts
ae3 se --census DIR            # survey every .hd under DIR
ae3 se BANK.hd --cues          # dump bank -> request -> raw event streams
ae3 se BANK.hd --decode -o DIR # decode every .bd waveform to WAV (44100 Hz mono)
ae3 se BANK.hd --render 5:0 -o cue.wav \
  --pitch-irx extracted/irx/sg2iopm1.irx \
  --libsd extracted/irx/libsd.irx
```

`--render BANK:REQUEST` assembles one effect at 48 kHz stereo through the native
core. Defaults are the faithful Gaussian resampler, exact event timing, caller
volume 96/127, and a 10-second cap for looping bytecode. `--tick-events` selects
the console's 60 Hz burst timing; `--bright` is the deliberately non-hardware
Catmull-Rom A/B option.

Private gate `checks/check_se.py`: all 101 banks, 2,180 referenced waveforms
sample-identical to the independent BGM decoder including loop points; all
structural invariants in §§2–7; and an independent full-bytecode parse pinned to
the §6.3 event/command/VLQ/jump census. Public golden vectors add a hand-authored,
zero-Sony-data SE bank whose render covers `A0`, all six `B0` commands, finite
looping, cut-groups, LFO, noise, and both event-timing modes.

## 10. Provenance

Format facts are read from `SCUS_975.01`: the parser `vab_set` `FUN_00402938`
and the accessors `FUN_00402b18` (`vab_get_seseq`), `FUN_00402c68`
(`vab_get_seprog`), `FUN_00402cf8` (`vab_get_prog`) in `sg2vab.c`; the SE walker
`FUN_003fce80` and handlers `FUN_003fd3d8`/`FUN_003fd488`/`FUN_003fd8d0`; the
SE note-on `FUN_003f8690`, cut-group scan `FUN_003ff7f8`, cue-open
`FUN_003fc3c8`, `kick3D` `FUN_0035b258`, and SeDesc row loader
`FUN_001bc0bc`. Source-file and function names are the sg2 library's own
`.rodata` assert strings (`sg2vab.c` `0x727a60`, `vab_get_seseq` `0x727a70`,
`vab_loadbybin_hd`/`_bd` `0x7140e8`/`0x714118`). Everything else is a census
over the 101-bank US corpus. Function-level notes:
`decomp/functions_bgm/se/NOTES.md` (private repo).
