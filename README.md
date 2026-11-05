# NARC Genesys

<p align="center"><img src="assets/narc-genesys-banner.png" alt="NARC Genesys" width="640"></p>

<p align="center">
  <img src="https://img.shields.io/badge/Strength-3600%2B-brightgreen" alt="Strength: 3600+ blitz">
  <img src="https://img.shields.io/badge/Homemade-orange" alt="Homemade">
  <a href="https://github.com/Madushan996/NARC/releases/tag/v5.1"><img src="https://img.shields.io/github/v/release/Madushan996/NARC" alt="Release"></a>
 <a href="https://github.com/Madushan996/NARC/releases"><img src="https://img.shields.io/github/downloads/Madushan996/NARC/total" alt="Downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License: GPL v3"></a>
</p>

## NARC Genesys 5.1 Fix

During long time control testing, I have found that version 5.0 misjudges some positions.
This quick update is mainly to address that issue. More information about the fix which includes retraining a bigger 
network given below 


UCI chess engine by Madushan Dissanayake, Sri Lanka

NARC Genesys is the next major public release of my NARC engine project (the previous release was NARC NEXT
4.1). The purpose of this project is to create a highly optimized and efficient chess engine from scratch, with
some AI help. It was carried out as an individual project, and it helped me get into chess engine development.
During development I gathered a lot of knowledge about engines and gained a good understanding of the factors
that contribute to a chess engine's strength. With AI assistance I was able to test hundreds of ideas very
quickly, within a span of just three months. Every idea was tested with SPRT matches, and most of them had to be
rejected. Many of the early tests were done without a proper understanding of what I was doing, and they wasted
a lot of compute in the early stages of the project. In my tests, the main strength improvements came from
choosing high-quality training data and from optimizing the NNUE architecture.

NARC Genesys is completely rewritten and can be considered a full re-implementation of the original NARC engine.
It went through a process of iterative self-improvement, starting from a hand-crafted evaluation (HCE). The
final NNUE was trained on a mixture of high-quality, filtered training positions: positions from the engine's own
self-play, and positions taken from licensed data sources. To prevent any knowledge distillation from other
authors' engines, the positions were relabelled by my own engines, mostly by NARC Genesys itself during its
development. Obtaining high-quality and diverse data was one of the primary objectives of this project. Human
games from Lichess were also used, but only filtered, high-quality positions went into the final corpus, and
they were relabelled by the best version of the engine at that point.

This is an individual project carried out in a compute-constrained environment, so using compute resources
efficiently was one of the biggest challenges. Development followed an iterative cycle. The first NNUE was
trained on only 1M self-play positions, and it outperformed the HCE version by roughly 100 Elo. The same process
was then repeated with the dataset growing step by step. The label quality of each new corpus also improved,
because each time a newer, stronger version was used to produce self-play data or to relabel the data.

The networks of most other 3500+ engines are trained on billions, or even hundreds of billions, of positions.
NARC Genesys was trained on only about 249M high-quality positions, and yet it competes with those engines:
its estimated strength is about 3600 on the CCRL Blitz scale. The engine has not yet hit a strength wall;
further development is currently limited by access to compute resources and by my own understanding.

Estimated strength: about 3608 CCRL Blitz (gauntlet against nine CCRL-rated engines at 8+0.08, 1,980 games).
Against NARC Genesys 5.0: +29.9 +- 13.8 Elo (SPRT, 756 games at 8+0.08).

## What's new in 5.1

NARC Genesys 5.1 is a small update, released two days after 5.0. During long time control testing after the 5.0
release, I found that the engine misjudged some positions and lost games because of it, mostly in longer games.
5.1 aims to fix this with a more accurate evaluation and better move ordering. The Elo figures below were
measured at a short time control (8+0.08); results at longer time controls are still being collected.

- **A wider network:** 1024 neurons per perspective instead of 256, trained from scratch on the same 249M positions
  as 5.0, with the same labels from my own engines. It evaluates positions more accurately (validation loss
  0.014254 against 5.0's 0.014808) but runs 25-40% slower; CPUs with a large cache lose the least speed.
- **ThreatHist:** the history that orders quiet moves is kept separately depending on whether the moving piece's
  square and its destination square are attacked, so better moves are tried first (+13.2 +- 12.7 Elo on its own,
  892 games). `setoption name ThreatHist value false` gives the 5.0 search back.

## Build

```bash
./build.sh native build/narc-genesys.exe   # targets: native, avx512, bmi2 (PEXT), avx2, sse42, generic, debug
./release.sh                               # every target into release/NARC-Genesys-5.1/, checked and zipped
```

Requires g++ 12 or newer (MSYS2 UCRT64 on Windows). The network `networks/narc-genesys.nnue` (DRNNUE03,
768 x 16 king buckets -> 1024 x 2 -> 8 output buckets, trained on 249M positions) is embedded in the executable;
`NET=<file>` picks another. Networks of any hidden width of 256, 512, 768 or 1024 load with `EvalFile`, so 5.0's
network still works.


## Versions

| Engine | Rating (CCRL Blitz) | Official CCRL (40/15) rating |
|---|---|---| 
| `NARC Engine` | 2950 (approx)  | Not published for Testing |
| `NARC NEXT 4.1 ` | 3400 (approx) | 3320 (rank #168 )| 
| `NARC Atlas 4.5` | 3500 (approx) | Private | 
| `NARC Genesys 5.0 ` | 3600 (approx)| Pending | 
| `NARC Genesys 5.1 ` | 3608 (approx)| Pending | 




## Commands

| Command | Purpose |
|---|---|
| `narc-genesys bench [depth]` | fixed-depth search over 12 positions; the node total is the search signature (1,146,093 at depth 12) |
| `narc-genesys perftsuite` | perft on 7 standard positions with known counts (exit code 1 on any mismatch) |
| `narc-genesys perft <depth>` | perft divide from the start position |
| UCI: `d`, `eval`, `perft <depth>`, `bench [depth]` | board and key, evaluation, divide, bench |

## Originality

NARC Genesys is an original engine. Its source code was written from scratch for this project. It is not a fork
or clone of another engine, and it contains no code copied from other engines. Its network was trained only on
positions labelled by my own engines (see Credits below). Like nearly every modern engine, it uses well-known,
published techniques: alpha-beta search with aspiration windows, late move reductions, null-move and futility
pruning, singular extensions, correction history, history-based move ordering (including history split by
threatened squares), Lazy SMP, magic bitboards, and an NNUE evaluation with king and material buckets. These are
written in my own code, and the network uses its own file format (DRNNUE03). Two parts follow Stockfish's design
closely; they are credited below.

## Credits

- **Stockfish** ([official-stockfish/Stockfish](https://github.com/official-stockfish/Stockfish), GPL-3.0): the
  transposition-table replacement scheme (three 10-byte entries per 32-byte bucket, generation ageing) and the
  static exchange evaluation (`see_ge`) follow Stockfish's design. They are re-implemented in my own code. No
  Stockfish source code, network or evaluation scores are used.
- **Training positions.** Every score in the training data was produced by my own engines (mostly NARC Genesys
  during its development, and my earlier engines NEXOR, CeylonDemon and DarkReality). The board positions come
  from these sources:
  - self-play games of NARC Genesys and of my earlier engines;
  - **Lichess open database** ([database.lichess.org](https://database.lichess.org), CC0): filtered, high-quality
    positions from human games. Only the board positions and game results were taken;
  - **Stockfish self-play games published by LAION** (CC-BY-4.0): board positions and the games' results. The
    positions were rescored by my own engine; no Stockfish evaluations are used;
  - **Stoofvlees self-play games** (CC-BY-4.0): board positions only, scored by my own engine.
- The perft reference positions used by `perftsuite` (and by part of `bench`) are from the
  [Chess Programming Wiki](https://www.chessprogramming.org/Perft_Results).
