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

function expect(cond, what) {
    if (!cond) {
        console.log(`FAIL ${what}`);
        fail++;
    }
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

/* source_state: the live per-voice source coordinate through the public JS
 * binding. The text is byte-identical to serender --source-state and shares
 * its native golden entry. */
{
    const {
        AE3Synth, VOICE_STATE_SIZE,
    } = await import("../js/ae3synth.mjs");
    const s = await AE3Synth.instantiate(wasmBytes);
    s.loadBank(readFileSync(join(VEC, "vec_se.hd")), bd);
    const envNames = ["ATTACK", "DECAY", "SUSTAIN", "RELEASE", "OFF"];
    const sourceNames = ["NONE", "ONESHOT", "LOOPED", "NOISE"];
    const mix = new Float32Array(2);
    let text = "", frame = 0;
    const line = (tag, slot, state, at = frame) => {
        text += `${tag} slot=${slot} frame=${at} in_use=${Number(state.in_use)}`
              + ` active=${Number(state.active)} released=${Number(state.released)}`
              + ` key=${state.key} env=${state.env} env_phase=${envNames[state.env_phase]}`
              + ` se_prog=${state.se_prog} source=${sourceNames[state.source_kind]}`
              + ` waveform=${state.waveform} samples=${state.source_samples}`
              + ` loop_start=${state.source_loop_start}`
              + ` phase_q12=${state.source_phase_q12} loops=${state.source_loops}\n`;
    };
    const activeSlot = key => {
        for (let slot = 0; slot < 48; slot++) {
            const state = s.voice(slot);
            if (state.active && state.key === key) return slot;
        }
        return -1;
    };

    line("idle", 0, s.voice(0));
    text += `out_of_range=${Number(s.voice(48) !== null)}\n`;
    expect(VOICE_STATE_SIZE === 12, "source_state voice stride");

    s.noteOn(0, 60, 100);
    const loopSlot = activeSlot(60);
    let current = s.voice(loopSlot);
    line("loop_unprimed", loopSlot, current);
    const packed = new Float64Array(VOICE_STATE_SIZE);
    expect(s.voiceInto(loopSlot, packed)
        && packed[0] === 3 && packed[8] === 224 && packed[9] === 56
        && packed[10] === 0, "source_state packed layout");
    s.render(mix, 1);
    frame++;
    current = s.voice(loopSlot);
    line("loop_primed", loopSlot, current);

    let previous = current;
    while (current.source_loops < 2 && frame < 2000) {
        s.render(mix, 1);
        frame++;
        current = s.voice(loopSlot);
        if (current.source_loops !== previous.source_loops) {
            line(current.source_loops === 1 ? "loop1_before" : "loop2_before",
                 loopSlot, previous, frame - 1);
            line(current.source_loops === 1 ? "loop1_after" : "loop2_after",
                 loopSlot, current);
        }
        previous = current;
    }
    expect(current.source_loops === 2, "source_state second seam");

    s.noteOff(0, 60);
    current = s.voice(loopSlot);
    line("loop_release", loopSlot, current);

    s.noteOn(0, 61, 100);
    const noiseSlot = activeSlot(61);
    line("noise", noiseSlot, s.voice(noiseSlot));

    s.noteOn(0, 62, 100);
    const oneSlot = activeSlot(62);
    current = s.voice(oneSlot);
    line("oneshot", oneSlot, current);
    while (current.active && frame < 5000) {
        s.render(mix, 1);
        frame++;
        current = s.voice(oneSlot);
    }
    expect(!current.active, "source_state one-shot end");
    line("ended", oneSlot, current);
    s.dispose();

    const parsed = Object.fromEntries(text.trim().split("\n")
        .filter(row => !row.startsWith("out_of_range="))
        .map(row => {
            const fields = row.split(" ");
            return [fields[0], Object.fromEntries(fields.slice(1)
                .map(field => field.split("=", 2)))];
        }));
    expect(parsed.loop_unprimed.samples === "224"
        && parsed.loop_unprimed.loop_start === "56", "source_state source metadata");
    expect(Number(parsed.loop1_before.phase_q12) > Number(parsed.loop1_after.phase_q12)
        && parsed.loop1_after.loops === "1" && parsed.loop2_after.loops === "2",
        "source_state phase wraps");
    expect(parsed.loop1_before.env === parsed.loop1_after.env
        && parsed.loop1_before.env_phase === parsed.loop1_after.env_phase,
        "source_state envelope continuity");
    expect(parsed.noise.source === "NOISE" && parsed.noise.phase_q12 === "-1",
        "source_state noise sentinel");
    expect(parsed.oneshot.source === "ONESHOT" && parsed.oneshot.loop_start === "-1",
        "source_state one-shot sentinel");
    expect(parsed.idle.source === "NONE" && parsed.ended.env_phase === "OFF",
        "source_state inactive sentinels");
    expect(text.includes("out_of_range=0\n"), "source_state out-of-range query");
    judge("source_state", createHash("sha256").update(text).digest("hex"));
}

process.exit(fail ? 1 : 0);
