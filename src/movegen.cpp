// NARC Genesys - strictly legal move generation.
//
// The king never steps onto an attacked square (attacks computed with the king removed). With two checkers
// only king moves are generated. With one checker every other move must land on the check mask (the checker
// or a square between it and the king). Pinned pieces move only along the line through their king. En passant
// is verified by recomputing the king's attackers on the board after the capture.
#include "movegen.h"

namespace narc {

namespace {

template <GenType T>
inline void add_promotions(MoveList& list, int from, int to, bool capture) {
    const int base = capture ? PROMO_CAPTURE : PROMO;
    if constexpr (T == GEN_ALL) {
        for (int k = 3; k >= 0; k--) list.push(make_move(from, to, base + k));
    } else if constexpr (T == GEN_NOISY) {
        if (capture)
            for (int k = 3; k >= 0; k--) list.push(make_move(from, to, base + k));
        else
            list.push(make_move(from, to, base + 3));
    } else {
        if (!capture)
            for (int k = 2; k >= 0; k--) list.push(make_move(from, to, base + k));
    }
}

}  // namespace

template <GenType T>
void generate(const Position& pos, MoveList& list) {
    const Color us = pos.side_to_move(), them = ~us;
    const int ksq = pos.king_square(us);
    const Bitboard occ = pos.pieces(), ours = pos.pieces(us), theirs = pos.pieces(them);
    const Bitboard empty = ~occ;
    const Bitboard checkers = pos.checkers(), pinned = pos.state().pinned;

    // Destination filter for non-pawn moves.
    Bitboard kind = T == GEN_ALL ? ~ours : T == GEN_NOISY ? theirs : empty;

    // King
    {
        Bitboard targets = KING_ATTACKS[ksq] & kind;
        const Bitboard occ_no_king = occ ^ bb(ksq);
        while (targets) {
            int to = pop_lsb(targets);
            if (!pos.attacked_by(them, to, occ_no_king))
                list.push(make_move(ksq, to, (theirs & bb(to)) ? CAPTURE : QUIET));
        }
    }
    if (more_than_one(checkers)) return;

    const Bitboard check_mask = checkers ? BETWEEN[ksq][lsb(checkers)] | checkers : ~0ULL;

    // Castling
    if constexpr (T != GEN_NOISY) {
        const int rights = pos.state().castling;
        if (!checkers && rights) {
            if (us == WHITE) {
                if ((rights & WHITE_OO) && !(occ & (bb(F1) | bb(G1))) && !pos.attacked_by(them, F1, occ) &&
                    !pos.attacked_by(them, G1, occ))
                    list.push(make_move(E1, G1, CASTLE_KING));
                if ((rights & WHITE_OOO) && !(occ & (bb(B1) | bb(C1) | bb(D1))) && !pos.attacked_by(them, D1, occ) &&
                    !pos.attacked_by(them, C1, occ))
                    list.push(make_move(E1, C1, CASTLE_QUEEN));
            } else {
                if ((rights & BLACK_OO) && !(occ & (bb(F8) | bb(G8))) && !pos.attacked_by(them, F8, occ) &&
                    !pos.attacked_by(them, G8, occ))
                    list.push(make_move(E8, G8, CASTLE_KING));
                if ((rights & BLACK_OOO) && !(occ & (bb(B8) | bb(C8) | bb(D8))) && !pos.attacked_by(them, D8, occ) &&
                    !pos.attacked_by(them, C8, occ))
                    list.push(make_move(E8, C8, CASTLE_QUEEN));
            }
        }
    }

    // Knights, bishops, rooks, queens
    for (PieceType t : {KNIGHT, BISHOP, ROOK, QUEEN}) {
        Bitboard from_set = pos.pieces(us, t);
        if (t == KNIGHT) from_set &= ~pinned;   // a pinned knight can never move
        while (from_set) {
            int from = pop_lsb(from_set);
            Bitboard targets = attacks_of(t, from, occ) & kind & check_mask;
            if (pinned & bb(from)) targets &= LINE[ksq][from];
            while (targets) {
                int to = pop_lsb(targets);
                list.push(make_move(from, to, (theirs & bb(to)) ? CAPTURE : QUIET));
            }
        }
    }

    // Pawns
    const Bitboard pawns = pos.pieces(us, PAWN);
    const Bitboard rank7 = us == WHITE ? RANK_7 : RANK_2;
    const Bitboard rank3 = us == WHITE ? RANK_3 : RANK_6;
    const int up = pawn_push(us);
    const int up_left = us == WHITE ? 7 : -9;    // towards the a-file
    const int up_right = us == WHITE ? 9 : -7;   // towards the h-file
    auto fwd = [us](Bitboard b) { return us == WHITE ? shift<8>(b) : shift<-8>(b); };
    auto cap_left = [us](Bitboard b) { return us == WHITE ? shift<7>(b) : shift<-9>(b); };
    auto cap_right = [us](Bitboard b) { return us == WHITE ? shift<9>(b) : shift<-7>(b); };
    auto legal_for_pin = [&](int from, int to) { return !(pinned & bb(from)) || (LINE[ksq][from] & bb(to)); };

    const Bitboard normal = pawns & ~rank7, promoting = pawns & rank7;

    if constexpr (T != GEN_NOISY) {
        Bitboard single = fwd(normal) & empty;
        Bitboard dbl = fwd(single & rank3) & empty & check_mask;
        single &= check_mask;
        while (single) {
            int to = pop_lsb(single), from = to - up;
            if (legal_for_pin(from, to)) list.push(make_move(from, to, QUIET));
        }
        while (dbl) {
            int to = pop_lsb(dbl), from = to - 2 * up;
            if (legal_for_pin(from, to)) list.push(make_move(from, to, DOUBLE_PUSH));
        }
    }
    if constexpr (T != GEN_QUIET) {
        Bitboard left = cap_left(normal) & theirs & check_mask;
        Bitboard right = cap_right(normal) & theirs & check_mask;
        while (left) {
            int to = pop_lsb(left), from = to - up_left;
            if (legal_for_pin(from, to)) list.push(make_move(from, to, CAPTURE));
        }
        while (right) {
            int to = pop_lsb(right), from = to - up_right;
            if (legal_for_pin(from, to)) list.push(make_move(from, to, CAPTURE));
        }
        const int ep = pos.state().ep_square;
        if (ep != NO_SQUARE) {
            const int cap = ep - up;
            Bitboard candidates = PAWN_ATTACKS[them][ep] & normal;
            while (candidates) {
                int from = pop_lsb(candidates);
                Bitboard after = (occ ^ bb(from) ^ bb(cap)) | bb(ep);
                Bitboard attackers = pos.attackers_to(ksq, after) & theirs & ~bb(cap);
                // the capturing pawn leaves `from`; enemy pieces on `from` cannot exist, so only sliders matter
                if (!attackers) list.push(make_move(from, ep, EN_PASSANT));
            }
        }
    }
    if (promoting) {
        Bitboard push = fwd(promoting) & empty & check_mask;
        Bitboard left = cap_left(promoting) & theirs & check_mask;
        Bitboard right = cap_right(promoting) & theirs & check_mask;
        while (push) {
            int to = pop_lsb(push), from = to - up;
            if (legal_for_pin(from, to)) add_promotions<T>(list, from, to, false);
        }
        while (left) {
            int to = pop_lsb(left), from = to - up_left;
            if (legal_for_pin(from, to)) add_promotions<T>(list, from, to, true);
        }
        while (right) {
            int to = pop_lsb(right), from = to - up_right;
            if (legal_for_pin(from, to)) add_promotions<T>(list, from, to, true);
        }
    }
}

template void generate<GEN_ALL>(const Position&, MoveList&);
template void generate<GEN_NOISY>(const Position&, MoveList&);
template void generate<GEN_QUIET>(const Position&, MoveList&);

}  // namespace narc
