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
