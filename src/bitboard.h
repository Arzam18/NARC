// NARC Genesys - bitboard helpers and attack tables.
#pragma once

#include <bit>

#include "types.h"

#if defined(USE_PEXT)
#include <immintrin.h>
#endif

namespace narc {

constexpr Bitboard FILE_A = 0x0101010101010101ULL;
constexpr Bitboard FILE_H = FILE_A << 7;
constexpr Bitboard RANK_1 = 0xFFULL;
constexpr Bitboard RANK_2 = RANK_1 << 8;
constexpr Bitboard RANK_3 = RANK_1 << 16;
constexpr Bitboard RANK_6 = RANK_1 << 40;
constexpr Bitboard RANK_7 = RANK_1 << 48;
constexpr Bitboard RANK_8 = RANK_1 << 56;

constexpr Bitboard bb(int s) { return 1ULL << s; }
inline int popcount(Bitboard b) { return std::popcount(b); }
inline int lsb(Bitboard b) { return std::countr_zero(b); }
inline int pop_lsb(Bitboard& b) {
    int s = lsb(b);
    b &= b - 1;
    return s;
}
constexpr bool more_than_one(Bitboard b) { return b & (b - 1); }

template <int D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == 8) return b << 8;
    else if constexpr (D == -8) return b >> 8;
    else if constexpr (D == 1) return (b & ~FILE_H) << 1;
    else if constexpr (D == -1) return (b & ~FILE_A) >> 1;
    else if constexpr (D == 9) return (b & ~FILE_H) << 9;
    else if constexpr (D == 7) return (b & ~FILE_A) << 7;
    else if constexpr (D == -7) return (b & ~FILE_H) >> 7;
    else if constexpr (D == -9) return (b & ~FILE_A) >> 9;
    else return 0;
}

// Squares attacked by the pawns in `b` of colour `c`.
constexpr Bitboard pawn_attacks_bb(Color c, Bitboard b) {
    return c == WHITE ? shift<7>(b) | shift<9>(b) : shift<-7>(b) | shift<-9>(b);
}

struct SliderTable {
    Bitboard mask;
    u64 magic;
    Bitboard* attacks;
    unsigned shift;
    unsigned index(Bitboard occ) const {
#if defined(USE_PEXT)
        return unsigned(_pext_u64(occ, mask));
#else
        return unsigned(((occ & mask) * magic) >> shift);
#endif
    }
};

extern Bitboard PAWN_ATTACKS[2][64];
extern Bitboard KNIGHT_ATTACKS[64];
extern Bitboard KING_ATTACKS[64];
extern Bitboard BETWEEN[64][64];   // squares strictly between a and b on a shared line, else 0
extern Bitboard LINE[64][64];      // the full line through a and b (both included), else 0
extern SliderTable ROOK_TABLE[64];
extern SliderTable BISHOP_TABLE[64];

void init_bitboards();

inline Bitboard rook_attacks(int s, Bitboard occ) { return ROOK_TABLE[s].attacks[ROOK_TABLE[s].index(occ)]; }
inline Bitboard bishop_attacks(int s, Bitboard occ) { return BISHOP_TABLE[s].attacks[BISHOP_TABLE[s].index(occ)]; }
inline Bitboard queen_attacks(int s, Bitboard occ) { return rook_attacks(s, occ) | bishop_attacks(s, occ); }

inline Bitboard attacks_of(PieceType t, int s, Bitboard occ) {
    switch (t) {
    case KNIGHT: return KNIGHT_ATTACKS[s];
    case BISHOP: return bishop_attacks(s, occ);
    case ROOK: return rook_attacks(s, occ);
    case QUEEN: return queen_attacks(s, occ);
    case KING: return KING_ATTACKS[s];
    default: return 0;
    }
}

}  // namespace narc
