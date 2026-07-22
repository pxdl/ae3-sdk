# ae3-sdk

ae3-sdk contains the reusable code and format notes behind Ape Escape 3 asset
tools. It can extract the game's files, inspect and convert its data formats,
and reproduce its music and embedded sound effects without bundling any game
data. Callers supply files from their own disc image.

The repository has three implementations for different jobs: a C11 audio
library, browser packages written in TypeScript and JavaScript, and a Python
command-line toolkit. They share the format descriptions in `docs/formats/` and
are checked against the same reference outputs where their behavior overlaps.

## Audio library

`core/` builds `libae3synth.a`. Its public API is
[`core/ae3synth.h`](core/ae3synth.h), and the implementation has no operating
system layer.

The synth loads the game's `.hd` and `.bd` instrument banks, standard MIDI
sequences, and embedded sound-effect requests. It models the shared 48-voice
pool, PS-ADPCM and noise voices, envelopes, pitch and volume math, the SPU2
Gaussian resampler, and the hardware reverb path. Rendering is stereo at 48 kHz.
The API also exposes exact or 60 Hz event timing, sequence loops, authored
volume and ducking controls, live voice state, timeline events, waveform decode,
and playback clock information.

The same library contains the EXST `.x` stream parser and sector decoder. It
supports the channel layout used by the game, carries ADPCM history across
sectors, and can identify trailing silent padding.

Build the library with:

```sh
make -C core
```

`harness/` contains small native consumers of the public API:

- `wavdump` renders BGM and exposes state used by the corpus checks.
- `serender` renders one embedded sound-effect request.
- `exstdump` inspects or decodes EXST streams.
- `bgmplay` is an SDL2 player with a piano roll, voice slots, and waveform view.

The first three can be built without SDL2:

```sh
make -C harness wavdump serender exstdump
```

`bgmplay` uses SDL2 through `pkg-config`:

```sh
make -C harness bgmplay
```

## Browser packages

`wasm/` compiles the C audio core into one standalone WebAssembly module. The
module has no generated JavaScript loader. The thin `@ae3/synth` ESM binding can
run in an AudioWorklet, a Worker, or Node, and it accepts either wasm bytes or a
precompiled `WebAssembly.Module`. The same package exposes `AE3Exst` for whole
file or sector-by-sector stream decoding. See
[`wasm/README.md`](wasm/README.md) for the binding API and build details.

`extract-web/` is the TypeScript `@ae3/extract` package. It reads a plain
2048-byte-sector ISO, walks ISO9660 and DATA.BIN, expands VFI, raw deflate, and
PCK containers, and can cache extracted assets in OPFS. It also parses the BGM
database, FMV containers, subtitle sidecars, display metadata, and MPEG-2 seek
points. Catalog inspection can read movie metadata without loading the whole
movie. See [`extract-web/README.md`](extract-web/README.md) for examples.

Both JavaScript package manifests are private. They are source packages for
consumers that vendor or bundle them from this repository rather than published
npm packages.

Build the WebAssembly target with Emscripten:

```sh
make -C wasm
```

Typecheck and test the browser extractor with Node:

```sh
cd extract-web
npm ci
npm run typecheck
npm test
```

## Python tools

`tools/` installs the `ae3tools` package and the `ae3` command. Pillow is optional
and is only needed for image conversion and software rendering.

```sh
python3 -m pip install "./tools[images]"
ae3 --help
```

The current command set covers:

| Area | Commands |
|---|---|
| DATA.BIN and design tables | `vfiparse`, `extract`, `databin-scan`, `exdb`, `songvol` |
| Models, animation, collision, and textures | `i3d`, `i3c`, `i3dmesh`, `gltf`, `i3manim`, `render`, `tm2` |
| Audio | `exst`, `se` |
| Movies and subtitles | `strextract`, `sbt2srt`, `fmv2mp4` |

Run `ae3 <command> --help` for the options accepted by each tool. The modules are
also importable from Python for custom scripts. More examples are in
[`tools/README.md`](tools/README.md).

## Format notes

[`docs/formats/`](docs/formats/) contains the format descriptions used by the
implementations:

- DATA.BIN, VFI, raw deflate, and PCK extraction
- BGM banks and sequences
- embedded sound-effect banks and request bytecode
- EXST streamed audio
- I3D models, animation, and collision
- FMV video, audio, subtitles, and display metadata

These documents describe the fields the code relies on and identify boundaries
where metadata is intentionally left uninterpreted.

## Verification

Public CI uses synthetic data and runs on clean checkouts:

- `python3 tests/run_vectors.py` builds the native core and compares its renders
  with the hashes in `tests/golden.sha256` on Linux and macOS.
- `node wasm/test/run_vectors.mjs` renders the same vectors through WebAssembly
  and compares them with the native golden hashes.
- `extract-web` builds a miniature ISO, VFI, PCK, Exdb, and STR fixture to test
  its parsers and the complete `openDisc` path.
- The Python workflow imports every `ae3tools` module and checks the CLI entry
  points.

Private checks use files extracted from a user-owned disc. They compare native
and WebAssembly audio across the full music corpus, and compare browser
extraction and FMV output with the Python tools. No game files or derived media
are stored in this repository or its public tests.

## Data policy and license

This repository ships no Sony code or game data. Tools read the user's own disc
at runtime, and public fixtures are synthetic. [`NOTICE.md`](NOTICE.md) records
the data policy and third-party provenance.

The code is licensed under the MIT License. See [`LICENSE`](LICENSE).
