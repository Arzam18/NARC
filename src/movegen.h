// NARC Genesys - strictly legal move generation (check and pin masks, no make/unmake test).
#pragma once

#include "position.h"

namespace narc {

struct MoveList {
    Move moves[MAX_MOVES];
    int count = 0;
    void push(Move m) { moves[count++] = m; }
    Move* begin() { return moves; }
    Move* end() { return moves + count; }
    const Move* begin() const { return moves; }
    const Move* end() const { return moves + count; }
    int size() const { return count; }
    Move operator[](int i) const { return moves[i]; }
};

// NOISY: every capture (en passant and capture-promotions included) and queen promotions.
// QUIET: everything else. NOISY and QUIET partition ALL.
enum GenType { GEN_ALL, GEN_NOISY, GEN_QUIET };

template <GenType T>
void generate(const Position& pos, MoveList& list);

inline void generate_legal(const Position& pos, MoveList& list) { generate<GEN_ALL>(pos, list); }

}  // namespace narc
