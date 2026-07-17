#!/usr/bin/env python3
"""I3D_I3M animation evaluator + the I3D_BIN bone NAME table.

## The name table (solves track binding)

Earlier format notes recorded "bone names are not in I3D_BIN at all ... binding
animations will need an index mapping, not a name lookup". That is WRONG. No bone NODE
carries a name (every 0x2a shares one `extra` pointing at an empty string), but the model
has a separate sorted name table hanging off the ROOT payload:

    root+0x14 -> transform table   (nbones x 0x40)
    root+0x18 -> u32 array         (nbones+1)
    root+0x1c -> u16 permutation   (nbones)   <- name order -> BONE INDEX
                 the name blob follows the permutation immediately: nbones
                 NUL-terminated names, sorted ASCII-ascending.

Each region ends exactly where the next begins. Sorted names + a permutation is a
binary-search name->index map, which is precisely what the runtime does:
FUN_003a78f8 (find_track_by_name) strcmps against the track name, and FUN_0050f028 packs
each track name into a 16-byte key (FUN_0035ab88, via qfsrv) to build a binding table.

Verified: on 154/154 models the permutation is a valid permutation of 0..nbones-1 and
the names are sorted and printable. Semantically, on ape_nrmb_body_b all 57 parent links
agree with what the names mean -- jnt_armL2's parent is jnt_armL1, jnt_fingerL11's is
jnt_handL, jnt_toeL's is jnt_footL2, jnt_eyeL's and jnt_zura's are jnt_head. A wrong
permutation cannot produce 57 coherent anatomical links by chance.

## Key components (earlier format notes listed 1..COMPS-1 as unknown)

    comp0  index into pool A (+0x24, f32) -> key TIME               (already known)
    comp1  index into pool A              -> 1/(t[k+1]-t[k])        SOLVED here
    comp2  index into the +0x20 pool, 16-byte elements              (position, f32[4])
    comp3  index into the +0x28 pool,  8-byte elements              -> unit QUATERNION
    comp4  (COMPS==5 only) index into the +0x28 pool                (tagged, see below)

comp1 is the RECIPROCAL of the interval to the next key -- precomputed so the evaluator
lerps with a multiply instead of a divide -- and is exactly 0.0 on the final key, which
has no successor. Verified on 41633 non-final and 2074 final keys across all 106 I3M
files: 100%, zero exceptions.

comp3 on jnt_* tracks decodes as 4 x s16/32768 to a UNIT quaternion on 31844/31844
referenced elements. HANDOFF.md recorded this reading as a dead end ("only ~1% of
elements are unit-length"), but that test scored EVERY pool element in EVERY file --
mixing joint rotations with camera _path POSITIONS and with comp2 -- so it averaged three
unrelated things. Scored per channel and per track kind, comp3 is exact.

The 16-byte comp2 element size is pinned by exact division on the camera paths, where the
pool is large: 1456/91, 6256/391, 2416/151 are all exactly 16.00, while the same files'
comp3 pools give exactly 8.00. (arijigoku1_path has 91 positions but ONE quaternion: a
camera that moves along a path at fixed orientation.)

## Still open -- deliberately not guessed

comp2 on jnt_* tracks is NOT the bone's local translation: matched against the real
skeleton it lands a mean 0.40 units from the nearest bone, so that reading is refuted,
not merely unconfirmed. Its element carries w == -32768 (0x8000) on 89.4% of references
(comp4: 90.2%), where comp3's elements never do -- so the sentinel discriminates
"not a quaternion", but what the other three s16 mean is unknown. Nothing here depends on
it: skeletal animation needs the rotations, and those are exact.

Two facts added later, when "the leg-stretch fidget does not travel" was reported from
play and comp2 was the obvious suspect. Both narrow the question; neither cracks it.

  1. comp2 is ANIMATED ON EXACTLY ONE TRACK. In pl_b_nrm_idl_s_01 (43 tracks, 61 keys on
     the root) the comp2 INDEX is constant per track on 42 of 43 -- jnt_head, jnt_footL2,
     everything -- and varies only on **jnt_root**, taking 42 distinct values. A per-bone
     constant plus one animated root channel is exactly the shape of root motion, which is
     why the reading keeps being tempting.
  2. It still is not translation, by a sharper test than the original one. If comp2 were a
     local translation, each bone's constant comp2 would have to equal that bone's rest
     local translation in the model's own transform table (root+0x14). It does not, and not
     by a scale factor either -- the implied comp2/rest ratio is incoherent per axis
     (jnt_head y=-755 z=57181; jnt_armL1 x=6104 y=-1050 z=94344).

Also corrected here: the "+0x20 pool, 16-byte elements" above is not general. It was
pinned on camera-path files, where +0x20 and +0x28 are different pools. On pl_b_nrm_idl_s_01
**+0x20 == +0x28** (both 0x4d20) and the stride is 8, so comp2 and comp3 index the SAME
4 x s16 pool -- which is what makes the w == -32768 tag necessary in the first place, and
is consistent with comp2's xyz saturating at +/-32767 rather than behaving like a position.

So the sway the fidget does have comes from jnt_root's ROTATION (121 keys), and it IS
applied: measured on the baked rig, the head travels x = -0.73 .. +0.73 over the clip while
jnt_root's pose origin stays pinned at its rest (0, 9.667, 0).

No EE code reads +0x20/+0x28: the I3M module relocates them but never dereferences them,
and FUN_003a7a40/FUN_003a7ad8 -- the accessors the earlier analysis reasoned from -- have
ZERO callers (tools/jalscan.py). The live consumers are only at 0x50ef50/0x50ef68/
0x50f110/0x50f15c, which read the key data directly. The 1/32768 scale appears nowhere in
.text as a constant, consistent with the dequantise happening on the VU (ITOF15 converts
int->float with 15 fractional bits, i.e. exactly /32768).
"""
import struct
import sys

BASE = 0x10


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def bone_names(path):
    """Return (names_in_bone_index_order, {name: bone_index}) for an I3D_BIN model."""
    from .i3d import I3d
    blob = open(path, "rb").read()
    m = I3d(blob, path)
    buf = blob[BASE:]
    root = m.root
    nb = sum(1 for n in root.walk() if n.type == 0x2A)
    if nb == 0:
        return [], {}
    perm_off = _u32(buf, root.data + 0x1C) + root.data
    perm = struct.unpack_from(f"<{nb}H", buf, perm_off)
    o = perm_off + nb * 2
    by_index = [None] * nb
    lookup = {}
    for k in range(nb):
        z = buf.find(b"\0", o)
        name = buf[o:z].decode("ascii", "replace")
        o = z + 1
        if perm[k] >= nb:
            raise ValueError(f"{path}: name index {perm[k]} >= {nb}")
        by_index[perm[k]] = name
        lookup[name] = perm[k]
    return by_index, lookup


def quat(b, pool, i):
    """Pool element -> (x, y, z, w) unit quaternion. 4 x s16 / 32768."""
    x, y, z, w = struct.unpack_from("<4h", b, pool + i * 8)
    return (x / 32768.0, y / 32768.0, z / 32768.0, w / 32768.0)


def quat_to_m3(q):
    x, y, z, w = q
    n = (x * x + y * y + z * z + w * w) ** 0.5
    if n > 1e-12:
        x, y, z, w = x / n, y / n, z / n, w / n
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0.0:                      # take the short way round
        b = tuple(-x for x in b)
        d = -d
    if d > 0.9995:                   # nearly parallel: lerp and renormalise
        r = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        import math
        th = math.acos(max(-1.0, min(1.0, d)))
        s = math.sin(th)
        w0, w1 = math.sin((1 - t) * th) / s, math.sin(t * th) / s
        r = tuple(x * w0 + y * w1 for x, y in zip(a, b))
    n = sum(x * x for x in r) ** 0.5
    return tuple(x / n for x in r) if n > 1e-12 else r


class Track:
    __slots__ = ("name", "times", "rates", "quats")

    def sample(self, t):
        """Rotation at time t. Uses comp1 (1/dt) exactly as the format precomputed it."""
        if len(self.times) == 1 or t <= self.times[0]:
            return self.quats[0]
        if t >= self.times[-1]:
            return self.quats[-1]
        lo = 0
        for k in range(len(self.times) - 1):
            if self.times[k] <= t < self.times[k + 1]:
                lo = k
                break
        # rates[lo] IS 1/(t[lo+1]-t[lo]); this is the multiply the format exists to enable
        u = (t - self.times[lo]) * self.rates[lo]
        return slerp(self.quats[lo], self.quats[lo + 1], max(0.0, min(1.0, u)))


class Anim:
    def __init__(self, path):
        from .i3d import I3m
        self.i3m = I3m(open(path, "rb").read(), path)
        a = self.i3m
        self.duration = a.duration
        self.tracks = {}
        pa = a.pool_a_floats()
        for t in a.tracks:
            rows = a.key_indices(t)
            if not rows:
                continue
            tr = Track()
            tr.name = t.name
            tr.times = [pa[r[0]] for r in rows]
            tr.rates = [pa[r[1]] for r in rows]
            tr.quats = [quat(a.b, a.p28, r[3]) for r in rows]
            self.tracks[t.name] = tr


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for p in sys.argv[1:]:
        head = open(p, "rb").read(8)
        if head == b"I3D_BIN\0":
            names, _ = bone_names(p)
            print(f"=== {p} ===\n  {len(names)} bones: {names[:8]} ...")
        elif head == b"I3D_I3M\0":
            a = Anim(p)
            print(f"=== {p} ===\n  {len(a.tracks)} tracks, {a.duration:.4f}s")
            for n, t in list(a.tracks.items())[:5]:
                print(f"    {n:16s} keys={len(t.times):3d} q0={[round(x,3) for x in t.quats[0]]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
