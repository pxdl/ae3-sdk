#!/usr/bin/env python3
"""Author the synthetic golden-vector assets (hand-made, zero Sony bytes).

Emits into tests/vectors/: one micro bank (vec.hd + vec.bd) and five format-0
MIDIs exercising the seams the corpus gates cover privately: loop seam, drum
kit, pitch bend, reverb flag, ADSR edges. Deterministic output: same script,
same bytes, forever — the golden hashes in golden.sha256 pin the renders.

Renders run with no IRX donors: nearest-ET pitch table, pure-dry mix.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vectors")

# ---------------------------------------------------------------- BD (ADPCM)
# 16-byte frames: byte0 = (filter<<4)|shift, byte1 = flags, 14 payload bytes
# (2 nibbles each, low nibble first). Flags: 0x04 LOOPSTART, 0x01 END,
# 0x02 REPEAT. Nibble pattern is an index-based ramp — deterministic, nonzero.

F_END, F_REPEAT, F_LOOPSTART = 0x01, 0x02, 0x04


def frame(idx, shift, filt, flags):
    payload = bytes(((2 * idx + i) * 5 + 3) % 251 for i in range(14))
    return bytes(((filt << 4) | shift, flags)) + payload


def waveform(nframes, loopstart=None, looped=False):
    out = b""
    for i in range(nframes):
        flags = 0
        if loopstart is not None and i == loopstart:
            flags |= F_LOOPSTART
        if i == nframes - 1:
            flags |= F_END | (F_REPEAT if looped else 0)
        out += frame(i, shift=3, filt=(i % 3), flags=flags)
    return out


W_ONESHOT = waveform(8)                                  # BD offset 0, addr 0
W_LOOPED = waveform(8, loopstart=2, looped=True)         # BD offset 128, addr 16
BD = W_ONESHOT + W_LOOPED
ADDR_ONESHOT, ADDR_LOOPED = 0, len(W_ONESHOT) // 8

# ---------------------------------------------------------------- HD (bank)
# Layout per the core parser (core/bank.c): 0x80-byte header, program chunk at
# 0x80 (u16 last-index + u16 offsets, first == table size), programs = 8-byte
# header + N*16-byte tones, velocity chunk (u16 count + 128 bytes) at the end.


def tone(lo, hi, root, addr, adsr1, adsr2, vol=127, pan=64, bend=0, lfo=0,
         flags=0, tune=0):
    return struct.pack("<BBBbHHH", lo, hi, root, tune, addr, adsr1, adsr2) + \
        bytes((0, vol, pan, bend, lfo, flags))


def prog(h0, tones, vol=127, bend=2, lfo=0, key0=0, key1=127):
    return bytes((h0, vol, 0, 0, bend, lfo, key0, key1)) + b"".join(tones)


A1_FAST_FULL = 0x000F            # instant linear attack, sustain level max
A2_LIN_HOLD = 0x0000             # sustain hold, instant linear release
A1_EXP_ATT = (1 << 15) | (10 << 10) | 0x000F   # exponential attack, shift 10
A2_SUS_DECR = (1 << 15) | (1 << 14) | (10 << 8) | (2 << 6)  # exp decreasing sustain
A1_SL_ZERO = (0x0F << 4) | 0x0000              # decay shift max, sustain level 0
A2_EXP_REL = (1 << 5) | 13                     # exponential release, shift 13

PROGS = [
    prog(0x00, [tone(0, 127, 60, ADDR_ONESHOT, A1_FAST_FULL, A2_LIN_HOLD)]),
    prog(0x00, [tone(0, 127, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD)]),
    prog(0xFF, [tone(0, 0, 60, ADDR_ONESHOT, A1_FAST_FULL, A2_LIN_HOLD, pan=32),
                tone(0, 0, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD, pan=64),
                tone(0, 0, 48, ADDR_ONESHOT, A1_FAST_FULL, A2_LIN_HOLD, pan=96)],
         key0=36, key1=38),
    prog(0x00, [tone(0, 127, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD,
                     flags=0x80)]),
    prog(0x03, [tone(0, 31, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD),
                tone(32, 63, 60, ADDR_LOOPED, A1_EXP_ATT, A2_LIN_HOLD),
                tone(64, 95, 60, ADDR_LOOPED, A1_FAST_FULL, A2_SUS_DECR),
                tone(96, 127, 60, ADDR_LOOPED, A1_SL_ZERO, A2_EXP_REL)]),
]


def bank_hd(progs, bd_len, lfo=None):
    nprog = len(progs)
    table_sz = 2 + nprog * 2
    offs, cur = [], table_sz
    for p in progs:
        offs.append(cur)
        cur += len(p)
    vel_off = 0x80 + cur
    lfo_off = vel_off + 2 + 128 if lfo else -1     # contiguous, like s_20_park
    lfo_chunk = b""
    if lfo:
        # Same chunk convention as programs: s16 last-index + u16 offsets from
        # the chunk start. Entries are 120 B: 60 B pitch + 60 B amplitude.
        lt = 2 + len(lfo) * 2
        lfo_chunk = struct.pack(f"<h{len(lfo)}H", len(lfo) - 1,
                                *(lt + 120 * i for i in range(len(lfo))))
        for e in lfo:
            assert len(e) == 120
            lfo_chunk += e
    hd_len = vel_off + 2 + 128 + len(lfo_chunk)
    hd = struct.pack("<III", hd_len, bd_len, 0) + b"SShd"
    hd += struct.pack("<iiiiii", 0x80, vel_off, lfo_off, -1, -1, -1)
    hd += b"\x00" * (0x80 - len(hd))
    hd += struct.pack("<H", nprog - 1)
    hd += b"".join(struct.pack("<H", o) for o in offs)
    hd += b"".join(progs)
    hd += struct.pack("<H", 127) + bytes(range(128))
    hd += lfo_chunk
    assert len(hd) == hd_len
    return hd


# ------------------------------------------------------------- LFO bank (M9)
# A second bank carrying an LFO chunk (docs/formats/BGM.md "LFO"): two 120-byte
# entries, each a 60-byte pitch waveform + a 60-byte amplitude half (unread by
# the BGM path -- zeros here, so a regression that starts reading it screams).
# Entry 0 = rising ramp (pitch saw), entry 1 = square. Prog 0 selects entry 0
# via tone byte 14 (+ the authoring-metadata flag 0x20, behaviorally inert);
# prog 1 selects entry 1 via flags 0x40 -> prog header byte 5. Banks WITHOUT a
# chunk get the driver's built-in triangle -- the corpus-shaped default already
# exercised by every other vector the moment CCs arm it (none do).

LFO_RAMP = bytes(range(4, 244, 4)) + bytes(60)
LFO_SQUARE = bytes([0xF0] * 30 + [0x10] * 30) + bytes(60)

PROGS_LFO = [
    prog(0x00, [tone(0, 127, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD,
                     bend=2, lfo=0, flags=0x20)]),
    prog(0x00, [tone(0, 127, 60, ADDR_LOOPED, A1_FAST_FULL, A2_LIN_HOLD,
                     bend=4, lfo=0, flags=0x40)], lfo=1),
]

# ---------------------------------------------------------------- MIDI


def vlq(n):
    out = [n & 0x7F]
    while n > 0x7F:
        n >>= 7
        out.append((n & 0x7F) | 0x80)
    return bytes(reversed(out))


def smf(events, ppqn=480):
    track, last = b"", 0
    for tick, data in events:
        track += vlq(tick - last) + data
        last = tick
    track += vlq(0) + b"\xff\x2f\x00"
    return (b"MThd" + struct.pack(">IHHH", 6, 0, 1, ppqn) +
            b"MTrk" + struct.pack(">I", len(track)) + track)


TEMPO_120 = b"\xff\x51\x03" + (500000).to_bytes(3, "big")


def on(k, v=100):
    return bytes((0x90, k, v))


def off(k):
    return bytes((0x80, k, 0))


MIDIS = {
    # loop machinery: CC99/20 start marker, CC99/30 end marker; rendered both
    # plain and with --loop 2 to exercise the seam
    "loop_seam": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 1))),
        (0, on(60)), (400, off(60)),
        (480, bytes((0xB0, 99, 20))),
        (480, on(64)), (900, off(64)),
        (960, on(67)), (1400, off(67)),
        (1920, bytes((0xB0, 99, 30))),
        (1920, on(72)), (2300, off(72)),
    ]),
    "drum_kit": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 2))),
        (0, on(36)), (200, off(36)),
        (240, on(37)), (440, off(37)),
        (480, on(38)), (680, off(38)),
        (720, on(36)), (760, on(38)), (960, off(36)), (960, off(38)),
    ]),
    "bend": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 0))),
        (0, on(60)),
        (120, bytes((0xE0, 0x00, 0x50))),
        (240, bytes((0xE0, 0x00, 0x60))),
        (360, bytes((0xE0, 0x7F, 0x7F))),
        (480, bytes((0xE0, 0x00, 0x20))),
        (600, bytes((0xE0, 0x00, 0x40))),
        (720, off(60)),
        (840, on(72)), (960, bytes((0xE0, 0x00, 0x00))), (1200, off(72)),
    ]),
    "reverb_flag": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 3))),
        (0, on(60)), (400, off(60)),
        (480, on(67)), (880, off(67)),
    ]),
    "adsr_edges": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 4))),
        (0, on(10)), (400, off(10)),
        (480, on(40)), (1400, off(40)),
        (1440, on(70)), (2300, off(70)),
        (2400, on(110)), (2500, off(110)),
    ]),
    # M9 LFO, on the vlfo bank: CC2 = rate (240/(60-p*58/127); 100 -> 16),
    # CC1 = depth. Covers: arming mid-note via the CC walk, a depth ramp,
    # disarm at CC1=0 (the freeze quirk -- pitch holds its last modulated
    # value), re-arm, then a second note that (a) arms AT note-on from the
    # standing channel stores and (b) resolves its waveform through flags
    # 0x40 -> prog[5] (the square, entry 1).
    "lfo": smf([
        (0, TEMPO_120), (0, bytes((0xC0, 0))),
        (0, on(60)),
        (60, bytes((0xB0, 2, 100))),
        (120, bytes((0xB0, 1, 20))),
        (240, bytes((0xB0, 1, 40))),
        (360, bytes((0xB0, 1, 60))),
        (480, bytes((0xB0, 1, 0))),
        (600, bytes((0xB0, 1, 50))),
        (720, off(60)),
        (760, bytes((0xC0, 1))),
        (780, on(64)),
        (1200, bytes((0xB0, 1, 30))),
        (1440, off(64)),
    ]),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    hd = bank_hd(PROGS, len(BD))
    with open(os.path.join(OUT, "vec.hd"), "wb") as f:
        f.write(hd)
    with open(os.path.join(OUT, "vec.bd"), "wb") as f:
        f.write(BD)
    hd_lfo = bank_hd(PROGS_LFO, len(BD), lfo=[LFO_RAMP, LFO_SQUARE])
    with open(os.path.join(OUT, "vlfo.hd"), "wb") as f:
        f.write(hd_lfo)                        # shares vec.bd
    for name, data in MIDIS.items():
        with open(os.path.join(OUT, name + ".mid"), "wb") as f:
            f.write(data)
    print(f"vec.hd {len(hd)}B  vlfo.hd {len(hd_lfo)}B  vec.bd {len(BD)}B  "
          f"+ {len(MIDIS)} midis -> {OUT}")


if __name__ == "__main__":
    main()
