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
for (const name of Object.keys(RENDERS).sort()) {
    const mid = readFileSync(join(VEC, (name.endsWith("_x2") ? name.slice(0, -3) : name) + ".mid"));
    const { wav } = await renderWav({ wasmBytes, hd, bd, mid, ...RENDERS[name] });
    const got = createHash("sha256").update(wav).digest("hex");
    const want = golden[name];
    const ok = got === want;
    console.log(`${ok ? "PASS" : "FAIL"} ${name}`
        + (ok ? "" : `  got ${got.slice(0, 16)}… want ${(want ?? "MISSING").slice(0, 16)}…`));
    fail += !ok;
}
process.exit(fail ? 1 : 0);
