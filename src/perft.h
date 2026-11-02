// NARC Genesys - perft.
#pragma once

#include "position.h"

namespace narc {

u64 perft(Position& pos, int depth);
u64 perft_divide(Position& pos, int depth);
bool perft_suite();   // standard positions with known counts; prints results, returns true if all match

}  // namespace narc
