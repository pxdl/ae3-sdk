# wasm/ — the core's WebAssembly target (`@ae3/synth`)

The same C files as `core/`, compiled by Emscripten to ONE standalone
`dist/ae3synth.wasm` — no JS glue, no toolchain-generated loader. The module's
entire import surface is `env.emscripten_notify_memory_growth`, so it
instantiates anywhere a `WebAssembly.Module` does: an AudioWorkletGlobalScope
(the player's audio path), a plain Worker (the player's WAV export), Node (the
gates below).

```
make            # -> dist/ae3synth.wasm  (needs emcc; brew install emscripten)
```

## Binding

`js/ae3synth.mjs` is the thin, environment-agnostic ESM binding — the package's
only JS. One wasm instance per synth (isolated heaps; the export path's second
synth can never touch the audio path's). It mirrors `core/ae3synth.h`
one-to-one; struct returns cross the boundary through the flatteners in
`abi.c`, written field-by-field so no JS code ever depends on C struct layout.

```js
import { AE3Synth } from "@ae3/synth";
const s = await AE3Synth.instantiate(wasmBytesOrModule);
s.loadBank(hd, bd);          // Uint8Arrays from @ae3/extract
s.loadSeq(mid);
s.loadPitchIrx(irx);         // optional donors, user's own disc
s.loadReverbIrx(libsd);
const buf = new Float32Array(128 * 2);
const n = s.render(buf);     // interleaved stereo at 48 kHz
```

No `TextDecoder`, no `fetch`, no `fs` — none of those exist in every target
scope. The caller supplies wasm bytes or a precompiled `WebAssembly.Module`
(worklet case: compile on the main thread, `postMessage` the Module).

`AE3Exst` (same module) is the standalone EXST (`.x`) stream decoder over
`ae3_exst_*`: `parseHeader` / `decodeFile({trimPad})` for whole files,
`reset` / `decodeSector` for chunked decode of the multi-minute tracks.

## Bit-exactness

The WASM render is byte-identical to the native one — the same discipline as
`make check`, extended to the second compilation target:

- **Public gate** (CI, no game data): `node test/run_vectors.mjs` renders the
  synthetic vectors and compares SHA-256 against `tests/golden.sha256` — the
  same golden file the native gate checks, so a pass proves WASM == native.
- **Private gate** (the W2 exit): the full-corpus A/B in the research repo
  (`checks/wasm_ab.py`) — all 68 songs in shipping config plus dial-coverage
  runs, native wavdump WAV vs `test/render.mjs` WAV, hash-identical.
  Green 2026-07-17: 82/82 exact.

Why it stays exact: WASM MVP has no FMA (the `-ffp-contract=off` discipline is
the platform default), the core is integer DSP plus IEEE doubles, and the one
libc dependency that could diverge (libm `pow` in the fallback ET pitch table)
is covered by both gates. Never enable relaxed SIMD on this target.

`js/wav.mjs` (shipped: the player's WAV export uses it) documents the one
subtlety worth knowing: dry renders emit
floats exactly on the s16/32768 grid, but reverb renders are fractional and the
WAV writers' `lrintf` round-half-to-even is load-bearing — the JS writer
implements it explicitly.
