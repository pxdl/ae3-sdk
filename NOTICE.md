# NOTICE — provenance and data policy

This project is licensed under the MIT License (see `LICENSE`). The notes below
document the provenance of specific hardware-behavior data embedded in the code,
and the project's policy on game data.

## No Sony data

This repository ships **zero** data derived from Ape Escape 3 or any other Sony
product. Everything the synthesizer needs from the game — instrument banks,
sequences, the IOP pitch table (`sg2iopm1.irx`), the SPU2 reverb preset
(`libsd.irx`) — is read at runtime from the **user's own game disc**. Test
fixtures in this repository are synthetic (hand-authored micro banks and
sequences); no test or asset here contains game-derived bytes.

## SPU2 gaussian interpolation table (`core/gauss.c`)

The 512-entry 4-tap gaussian table is the contents of a ROM table in the
PlayStation SPU/SPU2 hardware, transcribed from the psx-spx documentation
(nocash / https://psx-spx.consoledev.net, "4-Point Gaussian Interpolation"),
which documents the values as measured hardware behavior. The table is
reproduced here as a description of what the hardware does. Not derived from
any GPL source.

## SPU2 reverb (`core/reverb.c`)

The reverb algorithm — a per-sample feedback network running at 24000 Hz — is
implemented from the psx-spx SPU reverb documentation, **not** from any
emulator's source code.

The 39-tap half-band FIR used by the hardware to resample the reverb bus
between 48 kHz and 24 kHz is reproduced with tap values as published in PCSX2
(`pcsx2/SPU2/ReverbResample.cpp`, GPL-3.0+). They appear here **as a
description of measured hardware behavior** — the coefficients of a filter
inside the SPU2 — with this credit, not as a license-relevant copy of program
code. The surrounding implementation is original.

*Planned provenance upgrade:* because these taps are facts about the hardware,
an independent re-derivation (from half-band filter design constraints or
direct hardware measurement) must produce identical values. If/when that
re-derivation is done, this section will be updated to cite it; the code and
its bit-exactness guarantees will not change.

## Reverse-engineering provenance

Format knowledge (VFI archive, HD/BD banks, sequence format, Exdb tables) comes
from this project's own reverse engineering of the game's data files,
documented in `docs/formats/`. The synthesizer core is original C code that
reproduces the *behavior* of the PS2's SPU2 hardware (from psx-spx
documentation) and of the game's sound driver (established through the
project's own analysis of the game, for interoperability with the user's own
game data). No program code was copied or translated from any third-party
source; no GPL emulator code is included beyond the hardware-data credit noted
above.
