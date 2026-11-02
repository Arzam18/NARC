// NARC Genesys - board representation, FEN, make/unmake.
#pragma once

#include <string>

#include "bitboard.h"
#include "types.h"

namespace narc {

namespace zobrist {
extern u64 PIECE_SQUARE[16][64];
extern u64 CASTLING[16];
extern u64 EP_FILE[8];
extern u64 SIDE;
void init();
}  // namespace zobrist

// Pieces changed by the move that led to a position (for incremental NNUE updates).
// Entry i: piece[i] moved from[i] -> to[i]; from = NO_SQUARE means added, to = NO_SQUARE means removed.
struct DirtyPieces {
    int count;
    Piece piece[3];
    int from[3];
    int to[3];
};

// Everything make_move must restore, plus values derived once per position.
struct StateInfo {
    u64 key;
    u64 pawn_key;
    int castling;
    int ep_square;        // NO_SQUARE unless an en passant capture is pseudo-legal
    int halfmove;
    int plies_from_null;  // plies since the last null move (bounds repetition scans)
    Piece captured;
    Bitboard checkers;    // enemy pieces giving check to the side to move
    Bitboard pinned;      // side-to-move pieces pinned to their own king
    Move move;            // the move that led here
    DirtyPieces dirty;    // what that move changed on the board
};

class Position {
public:
    static constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    bool set_fen(const std::string& fen);
    std::string fen() const;
    std::string pretty() const;

    void make_move(Move m);
    void unmake_move(Move m);
    void make_null();
    void unmake_null();

    // Accessors
    Color side_to_move() const { return stm_; }
    Piece piece_on(int s) const { return board_[s]; }
    Bitboard pieces() const { return by_color_[WHITE] | by_color_[BLACK]; }
    Bitboard pieces(Color c) const { return by_color_[c]; }
    Bitboard pieces(PieceType t) const { return by_type_[t]; }
    Bitboard pieces(Color c, PieceType t) const { return by_color_[c] & by_type_[t]; }
    Bitboard pieces(PieceType a, PieceType b) const { return by_type_[a] | by_type_[b]; }
    Bitboard pieces(Color c, PieceType a, PieceType b) const { return by_color_[c] & (by_type_[a] | by_type_[b]); }
    int king_square(Color c) const { return lsb(pieces(c, KING)); }
    const StateInfo& state() const { return states_[ply_]; }
    const StateInfo& state_at(int game_ply) const { return states_[game_ply]; }
    u64 key() const { return states_[ply_].key; }
    Bitboard checkers() const { return states_[ply_].checkers; }
    bool in_check() const { return states_[ply_].checkers != 0; }
    int game_ply() const { return ply_; }
    int fullmove() const { return fullmove_; }

    // Attack queries
    Bitboard attackers_to(int s, Bitboard occ) const;
    bool attacked_by(Color by, int s, Bitboard occ) const;

    // Draw rules (repetition inside the game history counts once)
    bool is_repetition() const;
    bool is_fifty_move_draw() const { return states_[ply_].halfmove >= 100; }
    bool insufficient_material() const;

    // Move legality for moves from outside (UCI): matches against the legal list.
    Move parse_uci_move(const std::string& s) const;
    // Full legality check of an arbitrary 16-bit move (hash moves, killers): true iff generate<GEN_ALL> would
    // produce exactly this move.
    bool is_legal(Move m) const;
    // Static exchange evaluation: does the capture sequence started by m win at least `threshold` centipawns?
    bool see_ge(Move m, int threshold) const;
    Piece moved_piece(Move m) const { return board_[move_from(m)]; }
    bool has_non_pawn_material(Color c) const { return pieces(c) & ~pieces(PAWN, KING); }

private:
    void put_piece(Piece p, int s);
    void remove_piece(int s);
    void move_piece(int from, int to);
    void update_derived();
    bool ep_capture_possible(int ep_square, Color capturer) const;

    Piece board_[64];
    Bitboard by_type_[6];
    Bitboard by_color_[2];
    Color stm_ = WHITE;
    int fullmove_ = 1;
    int ply_ = 0;
    StateInfo states_[MAX_GAME_PLY + MAX_PLY + 8];
};

void init_tables();   // bitboards and Zobrist keys; call once at startup

}  // namespace narc
