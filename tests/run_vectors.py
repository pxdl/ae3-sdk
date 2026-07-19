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


def main():
    update = "--update" in sys.argv
    subprocess.run(["make", "-C", os.path.join(ROOT, "core")],
                   check=True, capture_output=True)
    subprocess.run(["make", "-C", os.path.join(ROOT, "harness"),
                    "wavdump", "exstdump"],
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
