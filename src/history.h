// NARC Genesys - move-ordering statistics, one set per search thread.
#pragma once

#include <cstdlib>
#include <cstring>

#include "types.h"

namespace narc {

constexpr int HISTORY_LIMIT = 16384;
constexpr int CORR_SIZE = 16384;       // correction-history entries per side (power of two)
constexpr int CORR_LIMIT = 16 * 400;   // corrections are capped at +-400 cp

// Moves the value towards the bonus's sign, saturating smoothly at +-HISTORY_LIMIT.
inline void history_update(i16& entry, int bonus) {
    int clamped = bonus > HISTORY_LIMIT ? HISTORY_LIMIT : bonus < -HISTORY_LIMIT ? -HISTORY_LIMIT : bonus;
    entry = i16(entry + clamped - entry * std::abs(clamped) / HISTORY_LIMIT);
}

// Piece index 0-11 (colour * 6 + type) for compact tables.
inline int piece_index(Piece p) { return color_of(p) * 6 + type_of(p); }

using ContinuationRow = i16[12][64];   // indexed by [piece index][to] of the current move

struct History {
    i16 quiet[2][64][64];             // [side][from][to]
    i16 quiet_threat[2][2][2][64][64];   // ThreatHist: [side][from attacked][to attacked][from][to]
    i16 capture[12][64][6];           // [moving piece][to][captured type]
    ContinuationRow cont[12][64];     // [previous piece][previous to] -> row for the current move
    Move counter[12][64];             // refutation of [previous piece][previous to]
    i16 pawn_corr[2][CORR_SIZE];      // [side][pawn key]: learned eval error, in 1/16 cp
    // CorrPlus: further correction tables, same units
    i16 nonpawn_corr[2][2][CORR_SIZE];   // [side to move][piece colour][that colour's non-pawn key]
    i16 minor_corr[2][CORR_SIZE];        // [side][knight, bishop and king key]
    i16 cont_corr[12][64][12][64];       // [piece][to] two plies back -> [piece][to] one ply back

    void clear() { std::memset(this, 0, sizeof(*this)); }
};

}  // namespace narc
