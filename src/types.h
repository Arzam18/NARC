<<<<<<< HEAD
// NARC Genesys chess engine - basic types.
#pragma once

#include <cstdint>
#include <string>

namespace narc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using Bitboard = u64;

constexpr int MAX_PLY = 128;          // deepest search ply
constexpr int MAX_MOVES = 256;        // upper bound on legal moves in a position
constexpr int MAX_GAME_PLY = 2048;    // plies of game history kept for repetition checks

enum Color : int { WHITE = 0, BLACK = 1 };
constexpr Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int { PAWN = 0, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_TYPE };

// Piece code = colour * 8 + type, the same nibble code as the training records.
enum Piece : int { NO_PIECE = 15 };
constexpr Piece make_piece(Color c, PieceType t) { return Piece(c * 8 + t); }
constexpr PieceType type_of(Piece p) { return PieceType(p & 7); }
constexpr Color color_of(Piece p) { return Color(p >> 3); }

// Squares: a1 = 0, b1 = 1, ..., h8 = 63.
enum Square : int {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    NO_SQUARE = 64
};
constexpr int file_of(int s) { return s & 7; }
constexpr int rank_of(int s) { return s >> 3; }
constexpr int relative_rank(Color c, int s) { return c == WHITE ? rank_of(s) : 7 - rank_of(s); }
constexpr int pawn_push(Color c) { return c == WHITE ? 8 : -8; }

// Castling rights: one bit each.
enum Castling : int { WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8, ALL_CASTLING = 15 };

// Move = 16 bits: from (0-5), to (6-11), flag (12-15).
// Flags: 0 quiet, 1 double pawn push, 2 king-side castle, 3 queen-side castle, 4 capture,
// 5 en passant, 8-11 promotion to N/B/R/Q, 12-15 capture-promotion to N/B/R/Q.
// Castling moves are encoded king-from -> king-to.
using Move = u16;
constexpr Move NO_MOVE = 0;
enum MoveFlag : int {
    QUIET = 0, DOUBLE_PUSH = 1, CASTLE_KING = 2, CASTLE_QUEEN = 3, CAPTURE = 4, EN_PASSANT = 5,
    PROMO = 8, PROMO_CAPTURE = 12
};
constexpr Move make_move(int from, int to, int flag = QUIET) { return Move(from | (to << 6) | (flag << 12)); }
constexpr int move_from(Move m) { return m & 63; }
constexpr int move_to(Move m) { return (m >> 6) & 63; }
constexpr int move_flag(Move m) { return m >> 12; }
constexpr bool is_capture(Move m) { return (m >> 14) & 1; }       // flags 4, 5, 12-15
constexpr bool is_promotion(Move m) { return (m >> 15) & 1; }     // flags 8-15
constexpr bool is_castle(Move m) { return move_flag(m) == CASTLE_KING || move_flag(m) == CASTLE_QUEEN; }
constexpr PieceType promo_type(Move m) { return PieceType(KNIGHT + (move_flag(m) & 3)); }

// Scores.
constexpr int VALUE_INF = 32001;
constexpr int VALUE_MATE = 32000;
constexpr int VALUE_MATE_IN_MAX = VALUE_MATE - MAX_PLY;
constexpr int VALUE_NONE = 32002;

std::string square_name(int s);
std::string move_to_uci(Move m);

}  // namespace narc
=======
// SPDX-License-Identifier: GPL-3.0-or-later
// NARC Engine — Copyright (C) 2026 Madushan Dissanayake.
// Licensed under the GNU GPL v3 or later; see LICENSE.
//
// NARC Engine — basic types, bitboards, attack tables, zobrist
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using u64 = uint64_t;
using u32 = uint32_t;
using u16 = uint16_t;
using u8  = uint8_t;
using s64 = int64_t;

enum Color { WHITE, BLACK };
enum PieceType { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE };

constexpr int MAX_PLY = 128;
constexpr int MATE    = 30000;
constexpr int MATE_BOUND = 29000; // scores beyond this are mate scores
constexpr int INF     = 31000;
constexpr int VALUE_DRAW = 0;

constexpr u64 FILE_A = 0x0101010101010101ULL;
constexpr u64 FILE_H = 0x8080808080808080ULL;
constexpr u64 RANK_1 = 0x00000000000000FFULL;
constexpr u64 RANK_2 = 0x000000000000FF00ULL;
constexpr u64 RANK_3 = 0x0000000000FF0000ULL;
constexpr u64 RANK_6 = 0x0000FF0000000000ULL;
constexpr u64 RANK_7 = 0x00FF000000000000ULL;
constexpr u64 RANK_8 = 0xFF00000000000000ULL;

inline int popcount(u64 b) { return __builtin_popcountll(b); }
inline int lsb(u64 b)      { return __builtin_ctzll(b); }
inline int poplsb(u64& b)  { int s = lsb(b); b &= b - 1; return s; }
inline u64 bit(int sq)     { return 1ULL << sq; }
inline int fileOf(int sq)  { return sq & 7; }
inline int rankOf(int sq)  { return sq >> 3; }

// ----------------------------- Moves ---------------------------------------
// 16-bit move: bits 0-5 from, 6-11 to, 12-13 promo piece (0=N..3=Q), 14-15 flag
enum MoveFlag { NORMAL = 0, PROMOTION = 1, ENPASSANT = 2, CASTLING = 3 };
using Move = u16;
constexpr Move MOVE_NONE = 0;

inline Move makeMove(int from, int to, int flag = NORMAL, int promo = 0) {
    return Move(from | (to << 6) | (promo << 12) | (flag << 14));
}
inline int fromSq(Move m)  { return m & 63; }
inline int toSq(Move m)    { return (m >> 6) & 63; }
inline int promoOf(Move m) { return ((m >> 12) & 3) + KNIGHT; }
inline int flagOf(Move m)  { return (m >> 14) & 3; }

inline std::string sqStr(int sq) {
    return { char('a' + fileOf(sq)), char('1' + rankOf(sq)) };
}
inline std::string moveStr(Move m) {
    if (m == MOVE_NONE) return "0000";
    std::string s = sqStr(fromSq(m)) + sqStr(toSq(m));
    if (flagOf(m) == PROMOTION) s += "nbrq"[promoOf(m) - KNIGHT];
    return s;
}

// ----------------------------- PRNG ----------------------------------------
struct PRNG {
    u64 s;
    explicit PRNG(u64 seed) : s(seed) {}
    u64 rand64() { // xorshift*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    u64 sparse() { return rand64() & rand64() & rand64(); }
};

// ----------------------------- Attack tables --------------------------------
inline u64 pawnAtt[2][64];
inline u64 knightAtt[64];
inline u64 kingAtt[64];

struct Magic {
    u64  mask;
    u64  magic;
    int  shift;
    u64* attacks;
    int index(u64 occ) const { return int(((occ & mask) * magic) >> shift); }
};
inline Magic rookMagics[64];
inline Magic bishopMagics[64];
inline u64 rookTable[0x19000];   // 102400
inline u64 bishopTable[0x1480];  // 5248

inline u64 slidingAttack(int sq, u64 occ, bool rook) {
    static const int dirs[2][4][2] = {
        {{1,1},{1,-1},{-1,1},{-1,-1}},   // bishop
        {{1,0},{-1,0},{0,1},{0,-1}}      // rook
    };
    u64 att = 0;
    for (int d = 0; d < 4; d++) {
        int r = rankOf(sq) + dirs[rook][d][0];
        int f = fileOf(sq) + dirs[rook][d][1];
        while (r >= 0 && r < 8 && f >= 0 && f < 8) {
            int s = r * 8 + f;
            att |= bit(s);
            if (occ & bit(s)) break;
            r += dirs[rook][d][0];
            f += dirs[rook][d][1];
        }
    }
    return att;
}

inline u64 relevantMask(int sq, bool rook) {
    u64 m = 0;
    int rk = rankOf(sq), fl = fileOf(sq);
    if (rook) {
        for (int r = rk + 1; r <= 6; r++) m |= bit(r * 8 + fl);
        for (int r = rk - 1; r >= 1; r--) m |= bit(r * 8 + fl);
        for (int f = fl + 1; f <= 6; f++) m |= bit(rk * 8 + f);
        for (int f = fl - 1; f >= 1; f--) m |= bit(rk * 8 + f);
    } else {
        for (int r = rk + 1, f = fl + 1; r <= 6 && f <= 6; r++, f++) m |= bit(r * 8 + f);
        for (int r = rk + 1, f = fl - 1; r <= 6 && f >= 1; r++, f--) m |= bit(r * 8 + f);
        for (int r = rk - 1, f = fl + 1; r >= 1 && f <= 6; r--, f++) m |= bit(r * 8 + f);
        for (int r = rk - 1, f = fl - 1; r >= 1 && f >= 1; r--, f--) m |= bit(r * 8 + f);
    }
    return m;
}

inline void initMagics(bool rook) {
    PRNG rng(0x6C078965ULL);
    u64* table = rook ? rookTable : bishopTable;
    int offset = 0;
    for (int sq = 0; sq < 64; sq++) {
        Magic& mg = rook ? rookMagics[sq] : bishopMagics[sq];
        mg.mask  = relevantMask(sq, rook);
        int bits = popcount(mg.mask);
        mg.shift = 64 - bits;
        mg.attacks = table + offset;
        int size = 1 << bits;

        u64 occs[4096], refs[4096];
        u64 b = 0;
        int n = 0;
        do {
            occs[n] = b;
            refs[n] = slidingAttack(sq, b, rook);
            n++;
            b = (b - mg.mask) & mg.mask;
        } while (b);

        // search a working magic
        std::vector<int> epoch(size, -1);
        int tries = 0;
        while (true) {
            mg.magic = rng.sparse();
            if (popcount((mg.mask * mg.magic) >> 56) < 6) continue;
            bool ok = true;
            for (int i = 0; i < n; i++) {
                int idx = mg.index(occs[i]);
                if (epoch[idx] != tries) { epoch[idx] = tries; mg.attacks[idx] = refs[i]; }
                else if (mg.attacks[idx] != refs[i]) { ok = false; break; }
            }
            if (ok) break;
            tries++;
        }
        offset += size;
    }
}

inline u64 bishopAttacks(int sq, u64 occ) { const Magic& m = bishopMagics[sq]; return m.attacks[m.index(occ)]; }
inline u64 rookAttacks(int sq, u64 occ)   { const Magic& m = rookMagics[sq];   return m.attacks[m.index(occ)]; }
inline u64 queenAttacks(int sq, u64 occ)  { return bishopAttacks(sq, occ) | rookAttacks(sq, occ); }

// masks for eval
inline u64 fileMask[8];
inline u64 adjacentFilesMask[8];
inline u64 passedMask[2][64]; // squares that must be free of enemy pawns

inline void initTables() {
    for (int sq = 0; sq < 64; sq++) {
        u64 b = bit(sq);
        pawnAtt[WHITE][sq] = ((b & ~FILE_A) << 7) | ((b & ~FILE_H) << 9);
        pawnAtt[BLACK][sq] = ((b & ~FILE_A) >> 9) | ((b & ~FILE_H) >> 7);

        u64 n = 0;
        const int nd[8][2] = {{2,1},{2,-1},{-2,1},{-2,-1},{1,2},{1,-2},{-1,2},{-1,-2}};
        for (auto& d : nd) {
            int r = rankOf(sq) + d[0], f = fileOf(sq) + d[1];
            if (r >= 0 && r < 8 && f >= 0 && f < 8) n |= bit(r * 8 + f);
        }
        knightAtt[sq] = n;

        u64 k = 0;
        for (int dr = -1; dr <= 1; dr++)
            for (int df = -1; df <= 1; df++) {
                if (!dr && !df) continue;
                int r = rankOf(sq) + dr, f = fileOf(sq) + df;
                if (r >= 0 && r < 8 && f >= 0 && f < 8) k |= bit(r * 8 + f);
            }
        kingAtt[sq] = k;
    }
    for (int f = 0; f < 8; f++) {
        fileMask[f] = FILE_A << f;
        adjacentFilesMask[f] = (f > 0 ? FILE_A << (f - 1) : 0) | (f < 7 ? FILE_A << (f + 1) : 0);
    }
    for (int sq = 0; sq < 64; sq++) {
        u64 span = fileMask[fileOf(sq)] | adjacentFilesMask[fileOf(sq)];
        u64 ahead = rankOf(sq) < 7 ? (~0ULL << (8 * (rankOf(sq) + 1))) : 0;
        u64 behind = rankOf(sq) ? (~0ULL >> (8 * (8 - rankOf(sq)))) : 0;
        passedMask[WHITE][sq] = span & ahead;
        passedMask[BLACK][sq] = span & behind;
    }
    initMagics(false);
    initMagics(true);
}

// ----------------------------- Zobrist --------------------------------------
inline u64 Zpsq[2][6][64];
inline u64 Zcastle[16];
inline u64 ZepFile[8];
inline u64 Zside;

inline void initZobrist() {
    PRNG rng(0x9E3779B97F4A7C15ULL);
    for (int c = 0; c < 2; c++)
        for (int p = 0; p < 6; p++)
            for (int s = 0; s < 64; s++) Zpsq[c][p][s] = rng.rand64();
    for (int i = 0; i < 16; i++) Zcastle[i] = rng.rand64();
    for (int i = 0; i < 8; i++)  ZepFile[i] = rng.rand64();
    Zside = rng.rand64();
}
>>>>>>> 2d83a7878e76fedee5640e667381d4f61c9af89c
