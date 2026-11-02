// NARC Genesys - staged move picker.
#include "movepick.h"

namespace narc {

namespace {
constexpr int ORDER_VALUE[7] = {100, 320, 330, 500, 950, 0, 0};

inline bool is_noisy(Move m) { return is_capture(m) || (is_promotion(m) && promo_type(m) == QUEEN); }

inline PieceType captured_type(const Position& pos, Move m) {
    return move_flag(m) == EN_PASSANT ? PAWN : type_of(pos.piece_on(move_to(m)));
}
}  // namespace

MovePicker::MovePicker(const Position& pos, Move tt_move, const History& hist, const Move killers[2], Move counter,
                       const ContinuationRow* cont1, const ContinuationRow* cont2, const ContinuationRow* cont4,
                       bool use_threats, Bitboard threats)
    : pos_(pos), hist_(hist), cont1_(cont1), cont2_(cont2), cont4_(cont4), use_threats_(use_threats),
      threats_(threats) {
    tt_move_ = pos.is_legal(tt_move) ? tt_move : NO_MOVE;
    auto usable = [&](Move m) { return m != NO_MOVE && m != tt_move_ && !is_noisy(m) && pos.is_legal(m); };
    if (usable(killers[0])) killers_[0] = killers[0];
    if (usable(killers[1]) && killers[1] != killers_[0]) killers_[1] = killers[1];
    if (usable(counter) && counter != killers_[0] && counter != killers_[1]) counter_ = counter;
    stage_ = tt_move_ ? TT_MOVE : GEN_NOISY;
}

MovePicker::MovePicker(const Position& pos, Move tt_move, const History& hist) : pos_(pos), hist_(hist) {
    in_check_ = pos.in_check();
    if (in_check_) {   // every evasion, ordered like the main search but without killers
        tt_move_ = pos.is_legal(tt_move) ? tt_move : NO_MOVE;
        stage_ = tt_move_ ? TT_MOVE : GEN_NOISY;
    } else {
        tt_move_ = is_noisy(tt_move) && pos.is_legal(tt_move) ? tt_move : NO_MOVE;
        stage_ = tt_move_ ? Q_TT_MOVE : Q_GEN;
    }
}

void MovePicker::score_noisy(int first, int last) {
    for (int i = first; i < last; i++) {
        Move m = list_.moves[i];
        Piece p = pos_.moved_piece(m);
        PieceType victim = is_capture(m) ? captured_type(pos_, m) : PAWN;
        int s = is_capture(m) ? ORDER_VALUE[victim] * 10 + hist_.capture[piece_index(p)][move_to(m)][victim] / 32 : 0;
        if (is_promotion(m)) s += ORDER_VALUE[promo_type(m)] * 10;
        scores_[i] = s;
    }
}

void MovePicker::score_quiets(int first, int last) {
    const int side = pos_.side_to_move();
    for (int i = first; i < last; i++) {
        Move m = list_.moves[i];
        const int pi = piece_index(pos_.moved_piece(m)), to = move_to(m);
        const int from = move_from(m);
        int s = use_threats_ ? hist_.quiet_threat[side][(threats_ >> from) & 1][(threats_ >> to) & 1][from][to]
                             : hist_.quiet[side][from][to];
        if (cont1_) s += (*cont1_)[pi][to];
        if (cont2_) s += (*cont2_)[pi][to];
        if (cont4_) s += (*cont4_)[pi][to];
        scores_[i] = s;
    }
}

int MovePicker::pick_best(int first, int last) {
    int best = first;
    for (int i = first + 1; i < last; i++)
        if (scores_[i] > scores_[best]) best = i;
    std::swap(list_.moves[first], list_.moves[best]);
    std::swap(scores_[first], scores_[best]);
    return first;
}

Move MovePicker::next() {
    for (;;) {
        switch (stage_) {
        case TT_MOVE:
            stage_ = GEN_NOISY;
            last_score_ = 1 << 20;
            return tt_move_;

        case GEN_NOISY:
            list_.count = 0;
            generate<narc::GEN_NOISY>(pos_, list_);
            end_noisy_ = list_.count;
            score_noisy(0, end_noisy_);
            cur_ = 0;
            stage_ = GOOD_NOISY;
            [[fallthrough]];

        case GOOD_NOISY:
            while (cur_ < end_noisy_) {
                int i = pick_best(cur_, end_noisy_);
                cur_++;
                Move m = list_.moves[i];
                if (m == tt_move_) continue;
                if (!pos_.see_ge(m, 0)) {
                    bad_[bad_count_++] = m;
                    continue;
                }
                last_score_ = scores_[i];
                return m;
            }
            stage_ = in_check_ ? GEN_QUIETS : KILLER1;
            continue;

        case KILLER1:
            stage_ = KILLER2;
            if (killers_[0] && !skip_quiets_) return killers_[0];
            continue;

        case KILLER2:
            stage_ = COUNTER;
            if (killers_[1] && !skip_quiets_) return killers_[1];
            continue;

        case COUNTER:
            stage_ = GEN_QUIETS;
            if (counter_ && !skip_quiets_) return counter_;
            continue;

        case GEN_QUIETS:
            if (skip_quiets_) {
                stage_ = BAD_NOISY;
                cur_ = 0;
                continue;
            }
            list_.count = end_noisy_;
            generate<narc::GEN_QUIET>(pos_, list_);
            score_quiets(end_noisy_, list_.count);
            cur_ = end_noisy_;
            stage_ = QUIETS;
            [[fallthrough]];

        case QUIETS:
            while (!skip_quiets_ && cur_ < list_.count) {
                int i = pick_best(cur_, list_.count);
                cur_++;
                Move m = list_.moves[i];
                if (is_special(m)) continue;
                last_score_ = scores_[i];
                return m;
            }
            stage_ = BAD_NOISY;
            cur_ = 0;
            [[fallthrough]];

        case BAD_NOISY:
            if (cur_ < bad_count_) {
                last_score_ = -(1 << 20);
                return bad_[cur_++];
            }
            stage_ = DONE;
            [[fallthrough]];

        case DONE:
            return NO_MOVE;

        case Q_TT_MOVE:
            stage_ = Q_GEN;
            return tt_move_;

        case Q_GEN:
            list_.count = 0;
            generate<narc::GEN_NOISY>(pos_, list_);
            end_noisy_ = list_.count;
            score_noisy(0, end_noisy_);
            cur_ = 0;
            stage_ = Q_NOISY;
            [[fallthrough]];

        case Q_NOISY:
            while (cur_ < end_noisy_) {
                int i = pick_best(cur_, end_noisy_);
                cur_++;
                if (list_.moves[i] == tt_move_) continue;
                last_score_ = scores_[i];
                return list_.moves[i];
            }
            stage_ = Q_DONE;
            [[fallthrough]];

        case Q_DONE:
            return NO_MOVE;
        }
    }
}

}  // namespace narc
