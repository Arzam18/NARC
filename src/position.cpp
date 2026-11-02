// NARC Genesys - board representation, FEN, make/unmake.
#include "position.h"

#include <cstring>
#include <sstream>

#include "movegen.h"

namespace narc {

namespace zobrist {
u64 PIECE_SQUARE[16][64];
u64 CASTLING[16];
u64 EP_FILE[8];
u64 SIDE;

void init() {
    u64 s = 0x1B873593A5A5A5A5ULL;   // splitmix64 stream
    auto next = [&]() {
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    };
    for (auto& row : PIECE_SQUARE)
        for (auto& k : row) k = next();
    for (auto& k : CASTLING) k = next();
    for (auto& k : EP_FILE) k = next();
    SIDE = next();
}
}  // namespace zobrist

void init_tables() {
    init_bitboards();
    zobrist::init();
}

namespace {

// Castling rights kept after a move touches a square.
int castling_mask(int s) {
    switch (s) {
    case A1: return ALL_CASTLING & ~WHITE_OOO;
    case H1: return ALL_CASTLING & ~WHITE_OO;
    case E1: return ALL_CASTLING & ~(WHITE_OO | WHITE_OOO);
    case A8: return ALL_CASTLING & ~BLACK_OOO;
    case H8: return ALL_CASTLING & ~BLACK_OO;
    case E8: return ALL_CASTLING & ~(BLACK_OO | BLACK_OOO);
    default: return ALL_CASTLING;
    }
}

int CASTLING_MASK[64];
const bool castling_mask_ready = [] {
    for (int s = 0; s < 64; s++) CASTLING_MASK[s] = castling_mask(s);
    return true;
}();

constexpr const char* PIECE_CHARS = "PNBRQK??pnbrqk?.";

}  // namespace

std::string square_name(int s) {
    if (s < 0 || s >= 64) return "-";
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string move_to_uci(Move m) {
    if (m == NO_MOVE) return "0000";
    std::string s = square_name(move_from(m)) + square_name(move_to(m));
    if (is_promotion(m)) s += "nbrq"[promo_type(m) - KNIGHT];
    return s;
}

void Position::put_piece(Piece p, int s) {
    board_[s] = p;
    by_type_[type_of(p)] |= bb(s);
    by_color_[color_of(p)] |= bb(s);
}

void Position::remove_piece(int s) {
    Piece p = board_[s];
    by_type_[type_of(p)] &= ~bb(s);
    by_color_[color_of(p)] &= ~bb(s);
    board_[s] = NO_PIECE;
}

void Position::move_piece(int from, int to) {
    Piece p = board_[from];
    Bitboard ft = bb(from) | bb(to);
    by_type_[type_of(p)] ^= ft;
    by_color_[color_of(p)] ^= ft;
    board_[from] = NO_PIECE;
    board_[to] = p;
}

Bitboard Position::attackers_to(int s, Bitboard occ) const {
    return (PAWN_ATTACKS[BLACK][s] & pieces(WHITE, PAWN)) | (PAWN_ATTACKS[WHITE][s] & pieces(BLACK, PAWN)) |
           (KNIGHT_ATTACKS[s] & by_type_[KNIGHT]) | (KING_ATTACKS[s] & by_type_[KING]) |
           (rook_attacks(s, occ) & (by_type_[ROOK] | by_type_[QUEEN])) |
           (bishop_attacks(s, occ) & (by_type_[BISHOP] | by_type_[QUEEN]));
}

bool Position::attacked_by(Color by, int s, Bitboard occ) const {
    Bitboard them = by_color_[by];
    return (PAWN_ATTACKS[~by][s] & by_type_[PAWN] & them) || (KNIGHT_ATTACKS[s] & by_type_[KNIGHT] & them) ||
           (KING_ATTACKS[s] & by_type_[KING] & them) ||
           (rook_attacks(s, occ) & (by_type_[ROOK] | by_type_[QUEEN]) & them) ||
           (bishop_attacks(s, occ) & (by_type_[BISHOP] | by_type_[QUEEN]) & them);
}

bool Position::ep_capture_possible(int ep_square, Color capturer) const {
    return PAWN_ATTACKS[~capturer][ep_square] & pieces(capturer, PAWN);
}

void Position::update_derived() {
    StateInfo& st = states_[ply_];
    Color us = stm_, them = ~stm_;
    int k = king_square(us);
    Bitboard occ = pieces();
    st.checkers = attackers_to(k, occ) & by_color_[them];
    st.pinned = 0;
    Bitboard snipers = (rook_attacks(k, 0) & pieces(ROOK, QUEEN) & by_color_[them]) |
                       (bishop_attacks(k, 0) & pieces(BISHOP, QUEEN) & by_color_[them]);
    while (snipers) {
        int s = pop_lsb(snipers);
        Bitboard between = BETWEEN[k][s] & occ;
        if (between && !more_than_one(between) && (between & by_color_[us])) st.pinned |= between;
    }
}

bool Position::set_fen(const std::string& fen) {
    std::istringstream in(fen);
    std::string placement, side, castle, ep;
    int halfmove = 0, fullmove = 1;
    if (!(in >> placement >> side)) return false;
    if (!(in >> castle)) castle = "-";
    if (!(in >> ep)) ep = "-";
    if (!(in >> halfmove)) halfmove = 0;
    if (!(in >> fullmove)) fullmove = 1;

    for (auto& p : board_) p = NO_PIECE;
    std::memset(by_type_, 0, sizeof(by_type_));
    std::memset(by_color_, 0, sizeof(by_color_));
    int rank = 7, file = 0;
    for (char c : placement) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            const char* found = std::strchr(PIECE_CHARS, c);
            if (!found || c == '?' || c == '.' || rank < 0 || file > 7) return false;
            put_piece(Piece(found - PIECE_CHARS), rank * 8 + file);
            file++;
        }
    }
    if (popcount(pieces(WHITE, KING)) != 1 || popcount(pieces(BLACK, KING)) != 1) return false;
    if (side != "w" && side != "b") return false;
    stm_ = side == "w" ? WHITE : BLACK;
    fullmove_ = std::max(1, fullmove);
    ply_ = 0;

    StateInfo& st = states_[0];
    st = StateInfo{};
    st.castling = 0;
    for (char c : castle) {
        if (c == 'K' && piece_on(E1) == make_piece(WHITE, KING) && piece_on(H1) == make_piece(WHITE, ROOK)) st.castling |= WHITE_OO;
        if (c == 'Q' && piece_on(E1) == make_piece(WHITE, KING) && piece_on(A1) == make_piece(WHITE, ROOK)) st.castling |= WHITE_OOO;
        if (c == 'k' && piece_on(E8) == make_piece(BLACK, KING) && piece_on(H8) == make_piece(BLACK, ROOK)) st.castling |= BLACK_OO;
        if (c == 'q' && piece_on(E8) == make_piece(BLACK, KING) && piece_on(A8) == make_piece(BLACK, ROOK)) st.castling |= BLACK_OOO;
    }
    st.ep_square = NO_SQUARE;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1] == '3' || ep[1] == '6')) {
        int s = (ep[1] - '1') * 8 + (ep[0] - 'a');
        if (ep_capture_possible(s, stm_)) st.ep_square = s;
    }
    st.halfmove = halfmove;
    st.plies_from_null = 0;
    st.captured = NO_PIECE;
    st.move = NO_MOVE;
    st.dirty.count = 0;
    st.key = st.pawn_key = 0;
    for (int s = 0; s < 64; s++)
        if (board_[s] != NO_PIECE) {
            st.key ^= zobrist::PIECE_SQUARE[board_[s]][s];
            if (type_of(board_[s]) == PAWN) st.pawn_key ^= zobrist::PIECE_SQUARE[board_[s]][s];
        }
    st.key ^= zobrist::CASTLING[st.castling];
    if (st.ep_square != NO_SQUARE) st.key ^= zobrist::EP_FILE[file_of(st.ep_square)];
    if (stm_ == BLACK) st.key ^= zobrist::SIDE;
    update_derived();
    // the side not to move must not be in check
    return !attacked_by(stm_, king_square(~stm_), pieces());
}

std::string Position::fen() const {
    std::string out;
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            Piece p = board_[rank * 8 + file];
            if (p == NO_PIECE) {
                empty++;
                continue;
            }
            if (empty) out += char('0' + empty);
            empty = 0;
            out += PIECE_CHARS[p];
        }
        if (empty) out += char('0' + empty);
        if (rank) out += '/';
    }
    const StateInfo& st = state();
    out += stm_ == WHITE ? " w " : " b ";
    std::string c;
    if (st.castling & WHITE_OO) c += 'K';
    if (st.castling & WHITE_OOO) c += 'Q';
    if (st.castling & BLACK_OO) c += 'k';
    if (st.castling & BLACK_OOO) c += 'q';
    out += c.empty() ? "-" : c;
    out += " " + square_name(st.ep_square) + " " + std::to_string(st.halfmove) + " " + std::to_string(fullmove_);
    return out;
}

std::string Position::pretty() const {
    std::ostringstream out;
    for (int rank = 7; rank >= 0; rank--) {
        out << " " << rank + 1 << "  ";
        for (int file = 0; file < 8; file++) out << PIECE_CHARS[board_[rank * 8 + file]] << ' ';
        out << '\n';
    }
    out << "    a b c d e f g h\n\nFEN: " << fen() << "\nKey: " << std::hex << key() << std::dec
        << "\nCheckers: ";
    Bitboard c = checkers();
    while (c) out << square_name(pop_lsb(c)) << ' ';
    out << '\n';
    return out.str();
}

void Position::make_move(Move m) {
    const StateInfo& prev = states_[ply_];
    StateInfo& st = states_[ply_ + 1];
    ply_++;
    st.key = prev.key ^ zobrist::SIDE;
    st.pawn_key = prev.pawn_key;
    st.castling = prev.castling;
    st.halfmove = prev.halfmove + 1;
    st.plies_from_null = prev.plies_from_null + 1;
    st.captured = NO_PIECE;
    st.move = m;
    st.ep_square = NO_SQUARE;
    if (prev.ep_square != NO_SQUARE) st.key ^= zobrist::EP_FILE[file_of(prev.ep_square)];

    const Color us = stm_, them = ~stm_;
    const int from = move_from(m), to = move_to(m), flag = move_flag(m);
    const Piece p = board_[from];

    if (flag == CASTLE_KING || flag == CASTLE_QUEEN) {
        int rook_from = flag == CASTLE_KING ? to + 1 : to - 2;
        int rook_to = flag == CASTLE_KING ? to - 1 : to + 1;
        Piece rook = board_[rook_from];
        move_piece(from, to);
        move_piece(rook_from, rook_to);
        st.key ^= zobrist::PIECE_SQUARE[p][from] ^ zobrist::PIECE_SQUARE[p][to] ^
                  zobrist::PIECE_SQUARE[rook][rook_from] ^ zobrist::PIECE_SQUARE[rook][rook_to];
        st.dirty = {2, {p, rook, NO_PIECE}, {from, rook_from, 0}, {to, rook_to, 0}};
    } else {
        st.dirty.count = 1;
        st.dirty.piece[0] = p;
        st.dirty.from[0] = from;
        st.dirty.to[0] = to;
        if (flag == EN_PASSANT) {
            int cap = to - pawn_push(us);
            st.captured = board_[cap];
            st.dirty.piece[1] = st.captured;
            st.dirty.from[1] = cap;
            st.dirty.to[1] = NO_SQUARE;
            st.dirty.count = 2;
            remove_piece(cap);
            st.key ^= zobrist::PIECE_SQUARE[st.captured][cap];
            st.pawn_key ^= zobrist::PIECE_SQUARE[st.captured][cap];
        } else if (is_capture(m)) {
            st.captured = board_[to];
            st.dirty.piece[1] = st.captured;
            st.dirty.from[1] = to;
            st.dirty.to[1] = NO_SQUARE;
            st.dirty.count = 2;
            remove_piece(to);
            st.key ^= zobrist::PIECE_SQUARE[st.captured][to];
            if (type_of(st.captured) == PAWN) st.pawn_key ^= zobrist::PIECE_SQUARE[st.captured][to];
        }
        if (st.captured != NO_PIECE) st.halfmove = 0;
        move_piece(from, to);
        st.key ^= zobrist::PIECE_SQUARE[p][from] ^ zobrist::PIECE_SQUARE[p][to];
        if (type_of(p) == PAWN) {
            st.halfmove = 0;
            st.pawn_key ^= zobrist::PIECE_SQUARE[p][from];
            if (is_promotion(m)) {
                Piece promoted = make_piece(us, promo_type(m));
                remove_piece(to);
                put_piece(promoted, to);
                st.dirty.to[0] = NO_SQUARE;
                st.dirty.piece[st.dirty.count] = promoted;
                st.dirty.from[st.dirty.count] = NO_SQUARE;
                st.dirty.to[st.dirty.count] = to;
                st.dirty.count++;
                st.key ^= zobrist::PIECE_SQUARE[p][to] ^ zobrist::PIECE_SQUARE[promoted][to];
            } else {
                st.pawn_key ^= zobrist::PIECE_SQUARE[p][to];
                if (flag == DOUBLE_PUSH) {
                    int ep = from + pawn_push(us);
                    if (ep_capture_possible(ep, them)) {
                        st.ep_square = ep;
                        st.key ^= zobrist::EP_FILE[file_of(ep)];
                    }
                }
            }
        }
    }

    int castling = st.castling & CASTLING_MASK[from] & CASTLING_MASK[to];
    if (castling != st.castling) {
        st.key ^= zobrist::CASTLING[st.castling] ^ zobrist::CASTLING[castling];
        st.castling = castling;
    }
    if (us == BLACK) fullmove_++;
    stm_ = them;
    update_derived();
}

void Position::unmake_move(Move m) {
    const StateInfo& st = states_[ply_];
    stm_ = ~stm_;
    const Color us = stm_;
    const int from = move_from(m), to = move_to(m), flag = move_flag(m);

    if (flag == CASTLE_KING || flag == CASTLE_QUEEN) {
        int rook_from = flag == CASTLE_KING ? to + 1 : to - 2;
        int rook_to = flag == CASTLE_KING ? to - 1 : to + 1;
        move_piece(to, from);
        move_piece(rook_to, rook_from);
    } else {
        if (is_promotion(m)) {
            remove_piece(to);
            put_piece(make_piece(us, PAWN), to);
        }
        move_piece(to, from);
        if (st.captured != NO_PIECE) put_piece(st.captured, flag == EN_PASSANT ? to - pawn_push(us) : to);
    }
    if (us == BLACK) fullmove_--;
    ply_--;
}

void Position::make_null() {
    const StateInfo& prev = states_[ply_];
    StateInfo& st = states_[ply_ + 1];
    ply_++;
    st = prev;
    st.key ^= zobrist::SIDE;
    if (prev.ep_square != NO_SQUARE) st.key ^= zobrist::EP_FILE[file_of(prev.ep_square)];
    st.ep_square = NO_SQUARE;
    st.halfmove = prev.halfmove + 1;
    st.plies_from_null = 0;
    st.captured = NO_PIECE;
    st.move = NO_MOVE;
    st.dirty.count = 0;
    stm_ = ~stm_;
    update_derived();
}

void Position::unmake_null() {
    stm_ = ~stm_;
    ply_--;
}

bool Position::is_repetition() const {
    const StateInfo& st = states_[ply_];
    int limit = std::min(st.halfmove, st.plies_from_null);
    for (int back = 4; back <= limit && back <= ply_; back += 2)
        if (states_[ply_ - back].key == st.key) return true;
    return false;
}

bool Position::insufficient_material() const {
    if (pieces(PAWN) | pieces(ROOK) | pieces(QUEEN)) return false;
    return popcount(pieces(KNIGHT) | pieces(BISHOP)) <= 1;
}

Move Position::parse_uci_move(const std::string& s) const {
    MoveList list;
    generate_legal(*this, list);
    for (Move m : list)
        if (move_to_uci(m) == s) return m;
    return NO_MOVE;
}

}  // namespace narc

namespace narc {

bool Position::is_legal(Move m) const {
    if (m == NO_MOVE) return false;
    const Color us = stm_, them = ~stm_;
    const int from = move_from(m), to = move_to(m), flag = move_flag(m);
    const Piece p = board_[from];
    if (p == NO_PIECE || color_of(p) != us || from == to || flag == 6 || flag == 7) return false;
    const PieceType pt = type_of(p);
    const Bitboard occ = pieces(), theirs = by_color_[them];
    const int ksq = king_square(us);
    const StateInfo& st = states_[ply_];

    if (flag == CASTLE_KING || flag == CASTLE_QUEEN) {
        if (pt != KING || st.checkers) return false;
        const bool white = us == WHITE;
        const int home = white ? E1 : E8;
        if (from != home) return false;
        if (flag == CASTLE_KING) {
            if (to != home + 2 || !(st.castling & (white ? WHITE_OO : BLACK_OO))) return false;
            if (occ & (bb(home + 1) | bb(home + 2))) return false;
            return !attacked_by(them, home + 1, occ) && !attacked_by(them, home + 2, occ);
        }
        if (to != home - 2 || !(st.castling & (white ? WHITE_OOO : BLACK_OOO))) return false;
        if (occ & (bb(home - 1) | bb(home - 2) | bb(home - 3))) return false;
        return !attacked_by(them, home - 1, occ) && !attacked_by(them, home - 2, occ);
    }

    // Pseudo-legality: the move shape must match the piece and the flag must match the board.
    if (by_color_[us] & bb(to)) return false;
    const bool target_enemy = theirs & bb(to);
    if (pt == PAWN) {
        const int up = pawn_push(us);
        const bool last_rank = relative_rank(us, to) == 7;
        if (is_promotion(m) != last_rank) return false;
        if (flag == EN_PASSANT) {
            if (to != st.ep_square || !(PAWN_ATTACKS[us][from] & bb(to))) return false;
            const int cap = to - up;
            Bitboard after = (occ ^ bb(from) ^ bb(cap)) | bb(to);
            return !(attackers_to(ksq, after) & theirs & ~bb(cap));
        }
        if (is_capture(m)) {
            if (!target_enemy || !(PAWN_ATTACKS[us][from] & bb(to))) return false;
        } else if (flag == DOUBLE_PUSH) {
            if (relative_rank(us, from) != 1 || to != from + 2 * up || (occ & (bb(from + up) | bb(to)))) return false;
        } else {
            if (to != from + up || (occ & bb(to))) return false;
        }
    } else {
        if (is_promotion(m) || flag == DOUBLE_PUSH || flag == EN_PASSANT) return false;
        if (is_capture(m) != target_enemy) return false;
        if (!(attacks_of(pt, from, occ) & bb(to))) return false;
    }

    // Legality.
    if (pt == KING) return !attacked_by(them, to, occ ^ bb(from));
    if (st.checkers) {
        if (more_than_one(st.checkers)) return false;
        const int checker = lsb(st.checkers);
        if (!((BETWEEN[ksq][checker] | st.checkers) & bb(to))) return false;
    }
    return !(st.pinned & bb(from)) || (LINE[ksq][from] & bb(to));
}

namespace {
constexpr int SEE_VALUE[7] = {100, 300, 300, 500, 900, 20000, 0};
}

bool Position::see_ge(Move m, int threshold) const {
    if (is_castle(m)) return 0 >= threshold;
    const int from = move_from(m), to = move_to(m);
    int swap = (move_flag(m) == EN_PASSANT ? SEE_VALUE[PAWN] : board_[to] == NO_PIECE ? 0 : SEE_VALUE[type_of(board_[to])]) - threshold;
    if (is_promotion(m)) swap += SEE_VALUE[promo_type(m)] - SEE_VALUE[PAWN];
    if (swap < 0) return false;
    int on_square = is_promotion(m) ? SEE_VALUE[promo_type(m)] : SEE_VALUE[type_of(board_[from])];
    swap = on_square - swap;
    if (swap <= 0) return true;   // even losing the moved piece keeps us at or above the threshold

    Bitboard occ = pieces() ^ bb(from) ^ bb(to);
    if (move_flag(m) == EN_PASSANT) occ ^= bb(to - pawn_push(stm_));
    Bitboard attackers = attackers_to(to, occ) & occ;
    const Bitboard diag = pieces(BISHOP, QUEEN), straight = pieces(ROOK, QUEEN);
    Color side = stm_;
    bool result = true;   // true: the side that made the last capture is winning the exchange so far
    for (;;) {
        side = ~side;
        Bitboard mine = attackers & by_color_[side];
        if (!mine) break;
        result = !result;
        PieceType pt = PAWN;
        for (; pt <= KING; pt = PieceType(pt + 1))
            if (mine & by_type_[pt]) break;
        if (pt == KING) {
            // the king may capture only if the opponent has no attackers left
            if (attackers & by_color_[~side] & occ) result = !result;
            break;
        }
        swap = SEE_VALUE[pt] - swap;
        if (swap < int(result)) break;
        occ ^= bb(lsb(mine & by_type_[pt]));
        if (pt == PAWN || pt == BISHOP || pt == QUEEN) attackers |= bishop_attacks(to, occ) & diag;
        if (pt == ROOK || pt == QUEEN) attackers |= rook_attacks(to, occ) & straight;
        attackers &= occ;
    }
    return result;
}

}  // namespace narc
