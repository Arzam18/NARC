NARC Genesys @VERSION@
UCI chess engine written in C++ by Madushan Dissanayake, Sri Lanka.
The fifth generation of the NARC engines (previous release: NARC NEXT 4.1).

Which executable?
  NARC-Genesys-@VERSION@-avx512.exe   CPUs with AVX-512 (Intel Ice Lake / Rocket Lake and newer server or
                                      workstation chips, AMD Zen 4 and newer).
  NARC-Genesys-@VERSION@-bmi2.exe     CPUs with AVX2 and fast BMI2 (Intel Haswell 2013 or newer, AMD Zen 3 or
                                      newer). The best choice for most computers.
  NARC-Genesys-@VERSION@-avx2.exe     AVX2 without fast BMI2: AMD Zen 1 / Zen 2 (Ryzen 1000-3000).
  NARC-Genesys-@VERSION@-sse42.exe    older x86-64 CPUs with SSE4.2 + POPCNT (Intel Nehalem 2008 or newer).
  NARC-Genesys-@VERSION@-generic.exe  any x86-64 CPU. Slowest; use it if no other build starts.
If unsure, try bmi2 first; if it closes at once or the GUI reports an error, go down the list.

The NNUE network is embedded in each executable; no other files are needed.
NARC Genesys has no GUI of its own: add it to a UCI chess GUI such as Arena, Cute Chess, Banksia or En Croissant.
The .bmp next to each executable is its logo for Arena; the banner and icon are in logo\, and the icon is
embedded in each .exe.

UCI options
  Hash            transposition table size in MB (default 16)
  Threads         search threads (default 1; more threads use Lazy SMP)
  Move Overhead   milliseconds kept back per move for GUI or network delay (default 10)
  EvalFile        <embedded> (default) or the path of another .nnue network in the same format

Command line
  NARC-Genesys-@VERSION@-bmi2.exe bench [depth]    fixed-depth benchmark (signature: @BENCH@ nodes at depth 12)
  NARC-Genesys-@VERSION@-bmi2.exe perftsuite       move generation self-test on 7 reference positions

Author:  Madushan Dissanayake
Country: Sri Lanka
