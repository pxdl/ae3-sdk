#!/usr/bin/env node
/* render.mjs -- wavdump's render mode through the WASM core, for the hash
 * gates: same argument surface, same setup order, same output bytes.
 *
 * usage: node render.mjs [--tail SEC] [--songvol 0..127] [--libsd PATH]
 *                        [--rev-depth 0..127] [--tick-events] [--exact-events]
 *                        [--bright] [--loop N] -o OUT.wav FILES...
 * FILES classified by extension like wavdump: .hd + .bd (bank), .mid, and
 * .irx = the pitch table donor (the reverb's libsd.irx goes via --libsd).
 * Only the render mode exists here; the dump modes stay native (they are
 * white-box, internal.h harness territory). */
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { AE3Synth } from "../js/ae3synth.mjs";
import { RATE, wavHeader, floatToS16 } from "../js/wav.mjs";

const WASM = join(dirname(fileURLToPath(import.meta.url)), "../dist/ae3synth.wasm");

function die(msg) { console.error(msg); process.exit(1); }

export async function renderWav(opts) {
    const s = await AE3Synth.instantiate(opts.wasmBytes ?? readFileSync(WASM));
    /* Setup order mirrors wavdump main: dials, pitch irx, bank, seq, libsd,
     * rev-depth, song volume. */
    s.setEventTiming(opts.exactEvents ?? true);
    s.setGaussian(!(opts.bright ?? false));
    s.setLoop(opts.loop ?? 0);
    if (opts.irx) s.loadPitchIrx(opts.irx);
    if (opts.hd) s.loadBank(opts.hd, opts.bd);
    if (opts.mid) s.loadSeq(opts.mid);
    if (opts.libsd) s.loadReverbIrx(opts.libsd);
    if (opts.revDepth != null) s.setReverbDepth(opts.revDepth);
    const vol = opts.songvol ?? 127;
    s.setSongVolume(vol, vol);

    const BLOCK = 4096;
    const buf = new Float32Array(BLOCK * 2);
    const chunks = [];
    let frames = 0;
    for (;;) {
        const n = s.render(buf, BLOCK);
        if (n < 0) die("render: nothing loaded");
        if (n === 0) break;
        chunks.push(floatToS16(buf, n * 2));
        frames += n;
    }
    const tailFrames = Math.round((opts.tail ?? 1.0) * RATE);
    const dataBytes = (frames + tailFrames) * 4;
    const out = new Uint8Array(44 + dataBytes);
    out.set(wavHeader(dataBytes), 0);
    let off = 44;
    for (const c of chunks) {
        out.set(new Uint8Array(c.buffer, 0, c.length * 2), off);
        off += c.length * 2;
    }
    /* tail is already zero-initialized */
    s.dispose();
    return { wav: out, frames };
}

async function main() {
    const o = { exactEvents: true };
    let outPath = null;
    const argv = process.argv.slice(2);
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === "--tail") o.tail = parseFloat(argv[++i]);
        else if (a === "--songvol") o.songvol = parseInt(argv[++i]);
        else if (a === "--libsd") o.libsd = readFileSync(argv[++i]);
        else if (a === "--rev-depth") o.revDepth = parseInt(argv[++i]);
        else if (a === "--loop") o.loop = parseInt(argv[++i]);
        else if (a === "--tick-events") o.exactEvents = false;
        else if (a === "--exact-events") o.exactEvents = true;
        else if (a === "--bright") o.bright = true;
        else if (a === "-o") outPath = argv[++i];
        else if (a.endsWith(".hd")) o.hd = readFileSync(a);
        else if (a.endsWith(".bd")) o.bd = readFileSync(a);
        else if (a.endsWith(".mid")) o.mid = readFileSync(a);
        else if (a.endsWith(".irx")) o.irx = readFileSync(a);
        else die(`unrecognized argument: ${a}`);
    }
    if (!o.hd !== !o.bd) die(".hd and .bd must be given together");
    if (!o.hd || !o.mid || !outPath) die("usage: render.mjs NAME.hd NAME.bd NAME.mid [PITCH.irx] [--libsd F] [flags] -o OUT.wav");
    const { wav, frames } = await renderWav(o);
    writeFileSync(outPath, wav);
    console.error(`${(frames / RATE).toFixed(2)} s (${frames} samples) -> ${outPath}`);
}

if (process.argv[1] === fileURLToPath(import.meta.url))
    await main();
