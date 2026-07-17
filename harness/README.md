# harness/ — native dev harnesses

Both consume the core directly (white-box: they include `internal.h`).

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

## bgmplay — SDL2 player / visualizer

```bash
make bgmplay && ./bgmplay [REPO_ROOT] [--song NAME] [--play]
```

Real-time playback with piano roll, the 48-slot voice pool, waveform strip,
help overlay, and 3-mode WAV export. Reads the song table and IRX donors from
the research-repo layout it is launched from (it ships no data); W1 migrates it
onto the public introspection API.

Keys: SPACE play/pause · ENTER play selected · ↑/↓ select · L loop · T
exact/tick clock · R reverb · ,/. depth · -/= volume · A authored volume ·
Z/X zoom · ESC quit.
