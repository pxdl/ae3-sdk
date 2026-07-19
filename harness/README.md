# harness/ — native dev harnesses

wavdump is white-box (includes `internal.h` for its trace hooks); bgmplay
consumes only the public API in `ae3synth.h` and is its reference consumer.

## wavdump — headless render / inspect

```bash
make wavdump
./wavdump SONG.hd SONG.bd SONG.mid [sg2iopm1.irx] [--libsd libsd.irx] \
          [--songvol N] [--loop N] [--tick-events] [--bright] -o OUT.wav
```

Renders a song to WAV, or dumps parsed state: `--dump` (bank/sequence),
`--decode` (waveforms), `--envdump` (ADSR), `--pitchdump`, `--voldump`,
`--slotdump` (voice-pool trace), `--eventdump` (dispatch trace), `--stems`
(dry/wet/combined buses), `--tone` (single-tone render). Without the IRX
donors: computed equal-tempered pitch table, pure-dry mix.

## exstdump — EXST (.x) stream inspect / decode

```bash
make exstdump
./exstdump FILE.x...                                   # header info lines
./exstdump --decode [--trim-pad] [-o OUT.wav] FILE.x   # decode to WAV
./exstdump --pcm [-o OUT.pcm] FILE.x                   # raw interleaved s16
```

Public-API consumer of `ae3_exst_*` (like bgmplay is of the synth). Decodes
the actual whole-sector payload — never the header's length field, which 16
shipped files overstate (`../docs/formats/EXST.md` §4, warned on stderr). WAV
framing is byte-identical to `ae3 exst --decode`, so the private corpus gate
diffs whole files.

## bgmplay — SDL2 player / visualizer

```bash
make bgmplay && ./bgmplay [REPO_ROOT] [--song NAME] [--play]
```

Real-time playback with piano roll, the 48-slot voice pool, waveform strip,
help overlay, and 3-mode WAV export. Song list, bank pairing and authored
per-song volumes come from the game's own mastering DB (`bgm_desc.exdb`,
extracted tree under REPO_ROOT; it ships no data). `--songs` dumps that table
for diffing against the `ae3 songvol` oracle.

Keys: SPACE play/pause · ENTER play selected · ↑/↓ select · L loop · T
exact/tick clock · R reverb · ,/. depth · -/= volume · A authored volume ·
Z/X zoom · ESC quit.
