# core/ — the SPU2 BGM synthesizer (pure C11)

Plays Ape Escape 3's sequenced music — `.mid` sequences against `.hd`/`.bd`
banks — faithful to the PS2's SPU2 and the game's own sound driver. Format and
driver ground truth: `../docs/formats/BGM.md`.

Engine-agnostic by construction: no dependency beyond libc/libm, builds and
tests headless, renders ~460× real time. The same core drives the native
harnesses (`../harness`), the WebAssembly build (`../wasm`, W2), and any future
engine embedding.

```bash
make          # builds libae3synth.a
```

`-ffp-contract=off` is pinned in the Makefile: the sequencer's sample-position
doubles must not be contracted into fused multiply-adds, or event timing stops
being bit-identical across compilers and architectures.

| file | role |
|---|---|
| `ae3synth.h` | public API: load bank/seq/pitch-IRX, transport, render f32 stereo @ 48 kHz |
| `bank.c` | Sony "Jam" `.hd` bank parser (hard errors on any layout violation) |
| `seq.c` | format-0 MIDI parser + sample-accurate tempo clock |
| `voice.c` | streaming PS-ADPCM decoder + gaussian resampler + live SPU2 ADSR |
| `pitch.c` | pitch registers per the driver's IOP module; runtime table load, ET fallback |
| `gauss.c` | the SPU2's 512-entry 4-tap interpolation table (psx-spx transcription) |
| `vol.c` | the driver's linear volume chain + the SPU2 pan table (rebuilt from its rule) |
| `reverb.c` | the SPU2 reverb, streaming: half-band FIR down/up + 24 kHz network; preset from the user's `libsd.irx` |
| `synth.c` | instance, note-on tone scan, the 48-slot voice pool, CC dispatch, loop markers, both event clocks, mixer + wet bus |
| `internal.h` | shared internals (harnesses may include; external consumers must not) |

**No game data is embedded** — banks, sequences, the pitch table and the reverb
preset are all read at runtime from the user's own disc. See `../NOTICE.md` for
the provenance of the two hardware tables that *are* compiled in (gaussian
kernel, reverb-bus FIR taps): both are descriptions of SPU2 hardware behavior.

Validation: synthetic golden vectors in `../tests` run in CI; the full
real-corpus bit-exactness gates live in the (private) research repo that
consumes this one as a submodule.
