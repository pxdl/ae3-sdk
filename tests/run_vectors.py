#!/usr/bin/env python3
"""Golden-vector gate: render the synthetic vectors, compare SHA-256 to golden.

Usage: python3 tests/run_vectors.py [--update]
Builds the harness (make), regenerates the vector assets from make_vectors.py
(they are deterministic), renders each through wavdump with no IRX donors
(ET pitch table, dry mix), and byte-hash-compares against golden.sha256.
--update rewrites golden.sha256 from the current renders instead.
"""
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VEC = os.path.join(HERE, "vectors")
WAVDUMP = os.path.join(ROOT, "harness", "wavdump")
EXSTDUMP = os.path.join(ROOT, "harness", "exstdump")
SERENDER = os.path.join(ROOT, "harness", "serender")
GOLDEN = os.path.join(HERE, "golden.sha256")

# name -> extra wavdump args
RENDERS = {
    "loop_seam": [],
    "loop_seam_x2": ["--loop", "2"],
    "drum_kit": [],
    "bend": [],
    "reverb_flag": [],
    "adsr_edges": [],
    "lfo": [],
    # M8 cue layer: authored scale + both duck groups, overlapping windows
    # (x0.49), ramps in both directions, edges on tick boundaries
    "cue": ["--cue", "0.42", "--duck", "0:0.5:1.5", "--duck", "1:1.0:2.2"],
}

# vectors rendered against a bank other than vec.hd (all share vec.bd)
BANKS = {"lfo": "vlfo.hd"}

# Embedded-SE sequencer: exact 480 Hz and console 60 Hz dispatch, plus host
# play-once/forever control of an authored count=0 jump.
# name -> (bank vector, extra serender args)
SE_RENDERS = {
    "se_exact": ("vec_se.hd", []),
    "se_tick": ("vec_se.hd", ["--tick-events"]),
    "se_loop_once": ("vec_se_inf.hd", ["--loop", "0"]),
    "se_loop_forever": ("vec_se_inf.hd", ["--loop", "127"]),
}

# EXST stream vectors through exstdump --decode, hashed as whole WAVs. The
# wasm gate rebuilds the identical files through the AE3Exst binding, so the
# shared golden entries prove WASM decode == native decode == the framing
# `ae3 exst --decode` writes. name -> (vector, extra args)
EXSTS = {
    "exst_mono": ("vec_mono.x", []),
    "exst_mono_trim": ("vec_mono.x", ["--trim-pad"]),
    "exst_stereo": ("vec_stereo.x", []),   # header overstates length: warns
}

# stdout modes, hashed as text. "decode" gates the bank-introspection API:
# wavdump --decode enumerates waveforms through it, cross-checks the table and
# PCM against the raw decoder stream internally, and prints per-waveform
# sample counts / loop points / FNV hashes / naming refs.
DUMPS = {
    "decode": ["--decode"],
}


def source_states(text):
    states = {}
    for line in text.decode().splitlines():
        fields = line.split()
        if not fields or "=" in fields[0]:
            continue
        states[fields[0]] = {
            key: value for key, value in (field.split("=", 1) for field in fields[1:])
        }
    return states


def validate_source_state(text):
    state = source_states(text)

    def number(tag, field):
        return int(state[tag][field])

    def need(condition, message):
        if not condition:
            raise RuntimeError(f"source-state assertion failed: {message}")

    need(number("loop_unprimed", "samples") == 224, "source length")
    need(number("loop_unprimed", "loop_start") == 56, "loop start")
    need(number("loop_unprimed", "phase_q12") == 0, "unprimed phase")
    need(number("loop_primed", "phase_q12") > 0, "primed phase")
    need(number("loop1_before", "phase_q12") > number("loop1_after", "phase_q12"),
         "first phase wrap")
    need(number("loop1_before", "loops") == 0
         and number("loop1_after", "loops") == 1
         and number("loop2_after", "loops") == 2, "seam counts")
    need(number("loop1_before", "env") == number("loop1_after", "env")
         and state["loop1_before"]["env_phase"] == state["loop1_after"]["env_phase"],
         "envelope seam continuity")
    need(state["loop_release"]["env_phase"] == "RELEASE"
         and number("loop_release", "loops") == 2, "release continuity")
    need(state["noise"]["source"] == "NOISE"
         and number("noise", "waveform") == -1
         and number("noise", "phase_q12") == -1, "noise sentinel")
    need(state["oneshot"]["source"] == "ONESHOT"
         and number("oneshot", "loop_start") == -1
         and number("oneshot", "phase_q12") == 0, "one-shot sentinel")
    for tag in ("idle", "ended"):
        need(state[tag]["source"] == "NONE"
             and state[tag]["env_phase"] == "OFF"
             and number(tag, "waveform") == -1
             and number(tag, "phase_q12") == -1, f"{tag} sentinel")
    need(b"out_of_range=0\n" in text, "out-of-range query")


def main():
    update = "--update" in sys.argv
    subprocess.run(["make", "-C", os.path.join(ROOT, "core")],
                   check=True, capture_output=True)
    subprocess.run(["make", "-C", os.path.join(ROOT, "harness"),
                    "wavdump", "exstdump", "serender"],
                   check=True, capture_output=True)
    subprocess.run([sys.executable, os.path.join(HERE, "make_vectors.py")],
                   check=True, capture_output=True)

    hashes = {}
    with tempfile.TemporaryDirectory() as td:
        for name, extra in RENDERS.items():
            mid = name[:-3] if name.endswith("_x2") else name
            out = os.path.join(td, name + ".wav")
            subprocess.run(
                [WAVDUMP, os.path.join(VEC, BANKS.get(name, "vec.hd")),
                 os.path.join(VEC, "vec.bd"),
                 os.path.join(VEC, mid + ".mid"), *extra, "-o", out],
                check=True, capture_output=True)
            with open(out, "rb") as f:
                hashes[name] = hashlib.sha256(f.read()).hexdigest()

        for name, (bank, extra) in SE_RENDERS.items():
            out = os.path.join(td, name + ".wav")
            subprocess.run(
                [SERENDER, "--seconds", "3", *extra, "-o", out,
                 os.path.join(VEC, bank), os.path.join(VEC, "vec.bd"),
                 "0", "0"],
                check=True, capture_output=True)
            with open(out, "rb") as f:
                hashes[name] = hashlib.sha256(f.read()).hexdigest()

        for name, (vec, extra) in EXSTS.items():
            out = os.path.join(td, name + ".wav")
            subprocess.run(
                [EXSTDUMP, "--decode", *extra, os.path.join(VEC, vec),
                 "-o", out],
                check=True, capture_output=True)
            with open(out, "rb") as f:
                hashes[name] = hashlib.sha256(f.read()).hexdigest()

    for name, flags in DUMPS.items():
        r = subprocess.run(
            [WAVDUMP, *flags, os.path.join(VEC, "vec.hd"), os.path.join(VEC, "vec.bd")],
            check=True, capture_output=True)
        hashes[name] = hashlib.sha256(r.stdout).hexdigest()
        if name == "decode":
            # API-visible projection (no n2=/hash2= seam fields): the wasm gate
            # reconstructs this exact text through the JS binding, so the shared
            # golden entry proves WASM API == native API on the vectors.
            api = "".join(
                " ".join(t for t in line.split()
                         if not t.startswith(("n2=", "hash2="))) + "\n"
                for line in r.stdout.decode().splitlines())
            hashes["decode_api"] = hashlib.sha256(api.encode()).hexdigest()

    source = subprocess.run(
        [SERENDER, "--source-state", os.path.join(VEC, "vec_se.hd"),
         os.path.join(VEC, "vec.bd")],
        check=True, capture_output=True).stdout
    validate_source_state(source)
    hashes["source_state"] = hashlib.sha256(source).hexdigest()
    if update:
        with open(GOLDEN, "w") as f:
            for name, h in sorted(hashes.items()):
                f.write(f"{h}  {name}\n")
        print(f"golden.sha256 updated ({len(hashes)} renders)")
        return 0

    golden = {}
    with open(GOLDEN) as f:
        for line in f:
            h, name = line.split()
            golden[name] = h
    fail = 0
    for name in sorted(hashes):
        got, want = hashes[name], golden.get(name)
        ok = got == want
        print(f"{'PASS' if ok else 'FAIL'} {name}"
              + ("" if ok else f"  got {got[:16]}… want {(want or 'MISSING')[:16]}…"))
        fail += not ok
    extra = set(golden) - set(hashes)
    if extra:
        print(f"FAIL stale golden entries: {sorted(extra)}")
        fail += 1
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
