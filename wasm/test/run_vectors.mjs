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
    lfo: {},
    /* M8 cue layer -- mirrors run_vectors.py's --cue/--duck args */
    cue: { cueScale: 0.42, ducks: [{ which: 0, t0: 0.5, t1: 1.5 },
                                   { which: 1, t0: 1.0, t1: 2.2 }] },
};

/* vectors on a bank other than vec.hd (all share vec.bd); mirrors BANKS */
const BANKS = { lfo: "vlfo.hd" };

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
    const bankHd = BANKS[name] ? readFileSync(join(VEC, BANKS[name])) : hd;
    const { wav } = await renderWav({ wasmBytes, hd: bankHd, bd, mid, ...RENDERS[name] });
    judge(name, createHash("sha256").update(wav).digest("hex"));
}

/* EXST stream vectors: decode through the AE3Exst binding and frame as WAV --
 * the same golden entries the native exstdump renders hash to, so wasm decode
 * == native decode == the `ae3 exst --decode` framing. The parsed header
 * fields are hard-asserted against the authored synthetic values (this is the
 * ae3w_exst_parse flattener's gate). */
{
    const { AE3Exst } = await import("../js/ae3synth.mjs");
    const { wavHeader } = await import("../js/wav.mjs");
    const exst = await AE3Exst.instantiate(wasmBytes);
    const EXSTS = {
        exst_mono: { file: "vec_mono.x", trimPad: false },
        exst_mono_trim: { file: "vec_mono.x", trimPad: true },
        exst_stereo: { file: "vec_stereo.x", trimPad: false },
    };
    for (const [name, cfg] of Object.entries(EXSTS).sort()) {
        const data = readFileSync(join(VEC, cfg.file));
        const { header, pcm } = exst.decodeFile(data, { trimPad: cfg.trimPad });
        const wav = Buffer.concat([
            wavHeader(pcm.byteLength, header.channels, header.rate),
            new Uint8Array(pcm.buffer, pcm.byteOffset, pcm.byteLength),
        ]);
        judge(name, createHash("sha256").update(wav).digest("hex"));
    }
    const expect = (cond, what) => {
        if (!cond) { console.log(`FAIL exst_header ${what}`); fail++; }
    };
    const hm = exst.parseHeader(readFileSync(join(VEC, "vec_mono.x")));
    const hs = exst.parseHeader(readFileSync(join(VEC, "vec_stereo.x")));
    expect(hm.channels === 1 && hm.rate === 24000 && hm.length === 2
        && hm.loop === 0 && hm.loop_start === 0, "mono fields");
    expect(hm.vol_l[0] === 0x407F && hm.vol_r[0] === 0x407F, "mono volumes");
    expect(hs.channels === 2 && hs.rate === 48000 && hs.length === 4,
        "stereo fields (authored overstated length)");
    expect(hs.vol_l[0] === 0x407F && hs.vol_l[1] === 0 && hs.vol_r[0] === 0
        && hs.vol_r[1] === 0x407F && hs.reverb.every(x => x === 0),
        "stereo hard-pan + reverb clear");
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
