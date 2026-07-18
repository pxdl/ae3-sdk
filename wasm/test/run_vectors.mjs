#!/usr/bin/env node
/* Golden-vector gate for the WASM target: render the synthetic vectors through
 * ae3synth.wasm + the JS binding and compare SHA-256 against tests/golden.sha256
 * -- the SAME golden file the native gate (tests/run_vectors.py) checks, so a
 * pass here proves WASM == native on the vectors, not merely WASM == itself.
 *
 * Expects dist/ae3synth.wasm built (make -C wasm) and regenerates the vector
 * assets via tests/make_vectors.py (deterministic), like the native gate. */
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { renderWav } from "./render.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, "../..");
const VEC = join(ROOT, "tests/vectors");

/* name -> render options; mirrors RENDERS in tests/run_vectors.py */
const RENDERS = {
    loop_seam: {},
    loop_seam_x2: { loop: 2 },
    drum_kit: {},
    bend: {},
    reverb_flag: {},
    adsr_edges: {},
};

const gen = spawnSync("python3", [join(ROOT, "tests/make_vectors.py")]);
if (gen.status !== 0) {
    console.error(`make_vectors.py failed:\n${gen.stderr}`);
    process.exit(1);
}

const golden = {};
for (const line of readFileSync(join(ROOT, "tests/golden.sha256"), "utf8").trim().split("\n")) {
    const [h, name] = line.split(/\s+/);
    golden[name] = h;
}

const wasmBytes = readFileSync(join(HERE, "../dist/ae3synth.wasm"));
const hd = readFileSync(join(VEC, "vec.hd"));
const bd = readFileSync(join(VEC, "vec.bd"));

let fail = 0;
function judge(name, got) {
    const want = golden[name];
    const ok = got === want;
    console.log(`${ok ? "PASS" : "FAIL"} ${name}`
        + (ok ? "" : `  got ${got.slice(0, 16)}… want ${(want ?? "MISSING").slice(0, 16)}…`));
    fail += !ok;
}

for (const name of Object.keys(RENDERS).sort()) {
    const mid = readFileSync(join(VEC, (name.endsWith("_x2") ? name.slice(0, -3) : name) + ".mid"));
    const { wav } = await renderWav({ wasmBytes, hd, bd, mid, ...RENDERS[name] });
    judge(name, createHash("sha256").update(wav).digest("hex"));
}

/* decode_api: the bank-introspection surface through the binding, in the same
 * projection tests/run_vectors.py derives from wavdump --decode (its W lines
 * minus the seam-only n2=/hash2= fields). Same golden entry on both sides ->
 * WASM enumeration, table fields and PCM bytes == native. */
{
    const s = await (await import("../js/ae3synth.mjs")).AE3Synth.instantiate(wasmBytes);
    s.loadBank(hd, bd);
    const BASIS = 0xcbf29ce484222325n, PRIME = 0x100000001b3n, M64 = (1n << 64n) - 1n;
    let text = "";
    for (let i = 0; i < s.bankWaveforms(); i++) {
        const w = s.bankWaveform(i);
        const pcm = s.bankWaveformPcm(i);
        let h = BASIS;
        for (let k = 0; k < pcm.length; k++) {
            const v = pcm[k] & 0xffff;
            h = ((h ^ BigInt(v & 0xff)) * PRIME) & M64;
            h = ((h ^ BigInt(v >> 8)) * PRIME) & M64;
        }
        text += `W addr=${w.addr} n=${w.samples} loop=${w.loop_start}`
              + ` hash=${h.toString(16).padStart(16, "0")} prog=${w.prog}`
              + ` tone=${w.tone} root=${w.root} tune=${w.tune} refs=${w.refs}\n`;
    }
    s.dispose();
    judge("decode_api", createHash("sha256").update(text).digest("hex"));
}
process.exit(fail ? 1 : 0);
