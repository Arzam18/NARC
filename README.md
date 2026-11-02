# NARC Genesys

<p align="center"><img src="assets/narc-genesys-banner.png" alt="NARC Genesys" width="640"></p>

A UCI chess engine in C++20 by Madushan Dissanayake (Sri Lanka). NARC Genesys is version 5.0 of the NARC
engines; the previous release was NARC NEXT 4.1. It is a complete rewrite, developed under the working name
Makara (Makara 0.12 kb16 is the same engine), with an NNUE network trained only on positions labelled by the
engine itself. See `ReadMe.txt` for the story of the project.

Estimated strength: about 3600 CCRL Blitz (gauntlet against nine CCRL-rated engines at 8+0.08, 1,736 games).

## Build

```bash
./build.sh native build/narc-genesys.exe   # targets: native, avx512, bmi2 (PEXT), avx2, sse42, generic, debug
./release.sh                               # every target into release/NARC-Genesys-5.0/, checked and zipped
```

Requires g++ 12 or newer (MSYS2 UCRT64 on Windows). The network `networks/narc-genesys.nnue`
(DRNNUE03, 16 king buckets, trained on 249M positions) is embedded in the executable; `NET=<file>` picks another.

## Commands

| Command | Purpose |
|---|---|
| `narc-genesys bench [depth]` | fixed-depth search over 12 positions; the node total is the search signature (1,405,722 at depth 12) |
| `narc-genesys perftsuite` | perft on 7 standard positions with known counts (exit code 1 on any mismatch) |
| `narc-genesys perft <depth>` | perft divide from the start position |
| UCI: `d`, `eval`, `perft <depth>`, `bench [depth]` | board and key, evaluation, divide, bench |
