// NARC Genesys - staged move picker. Every move it returns is legal.
//
// Main search: hash move, good captures (static exchange >= 0, by victim value and capture history), killers,
// counter move, quiet moves by history, then losing captures. Quiescence: hash move and good captures only, or
// every evasion when in check.
#pragma once

#include "history.h"
#include "movegen.h"
#include "position.h"

namespace narc {

class MovePicker {
public:
    // main search
    MovePicker(const Position& pos, Move tt_move, const History& hist, const Move killers[2], Move counter,
               const ContinuationRow* cont1, const ContinuationRow* cont2, const ContinuationRow* cont4 = nullptr,
               bool use_threats = false, Bitboard threats = 0);
    // quiescence search
    MovePicker(const Position& pos, Move tt_move, const History& hist);

    Move next();
    void skip_quiets() { skip_quiets_ = true; }
    int stage() const { return stage_; }
    int last_score() const { return last_score_; }   // ordering score of the move just returned

private:
    enum Stage {
        TT_MOVE, GEN_NOISY, GOOD_NOISY, KILLER1, KILLER2, COUNTER, GEN_QUIETS, QUIETS, BAD_NOISY, DONE,
        Q_TT_MOVE, Q_GEN, Q_NOISY, Q_DONE
    };
    void score_noisy(int first, int last);
    void score_quiets(int first, int last);
    int pick_best(int first, int last);
    bool is_special(Move m) const { return m == tt_move_ || m == killers_[0] || m == killers_[1] || m == counter_; }

    const Position& pos_;
    const History& hist_;
    const ContinuationRow* cont1_ = nullptr;
    const ContinuationRow* cont2_ = nullptr;
    const ContinuationRow* cont4_ = nullptr;
    bool use_threats_ = false;
    Bitboard threats_ = 0;
    Move tt_move_, killers_[2] = {NO_MOVE, NO_MOVE}, counter_ = NO_MOVE;
    int stage_;
    bool skip_quiets_ = false;
    bool in_check_ = false;
    MoveList list_;
    int scores_[MAX_MOVES];
    int cur_ = 0, end_noisy_ = 0, bad_count_ = 0;
    Move bad_[MAX_MOVES];
    int last_score_ = 0;
};

}  // namespace narc
