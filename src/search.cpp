// NARC Genesys - search: iterative deepening with aspiration windows, principal variation search, quiescence search.
//
// Stage 1 (always on): transposition table, staged move ordering with killers, counter moves and
// butterfly / continuation / capture histories, check extension, mate-distance pruning.
// Stage 2 (SearchFeatures): reverse futility pruning, null-move pruning, razoring, late-move, futility, SEE and
// history pruning, late-move reductions, internal iterative reduction.
#include "search.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include "io.h"
#include "movegen.h"
#include "movepick.h"

namespace narc {

namespace {

int LMR[64][64];

}  // namespace

TuneParams TUNE;

void init_lmr() {
    const double base = TUNE.lmr_base / 100.0, div = std::max(TUNE.lmr_div, 1) / 100.0;
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++) LMR[d][m] = int(base + std::log(d) * std::log(m) / div);
}

int* tune_param(const std::string& n) {
    if (n == "lmr_base") return &TUNE.lmr_base;
    if (n == "lmr_div") return &TUNE.lmr_div;
    if (n == "fut_base") return &TUNE.fut_base;
    if (n == "fut_mul") return &TUNE.fut_mul;
    if (n == "rfp_mul") return &TUNE.rfp_mul;
    if (n == "rfp_imp") return &TUNE.rfp_imp;
    if (n == "nmp_div") return &TUNE.nmp_div;
    if (n == "see_quiet") return &TUNE.see_quiet;
    if (n == "see_cap") return &TUNE.see_cap;
    if (n == "hist_prune") return &TUNE.hist_prune;
    if (n == "sing_margin") return &TUNE.sing_margin;
    if (n == "lmr_hist") return &TUNE.lmr_hist;
    return nullptr;
}

namespace {

const bool lmr_ready = [] {
    init_lmr();
    return true;
}();

inline int stat_bonus(int depth) { return std::min(170 * depth - 80, 1700); }

// Doubt (batch 3): the correction histories' adjustment at a node, in cp, is read as uncertainty about the eval.
// On the bench positions about 2/3 of nodes get under 8 cp of correction and about 4% get 40 cp or more.
constexpr int DOUBT_CAP = 160;   // at most +120 cp on a pruning margin
constexpr int DOUBT_LMR = 40;    // one ply less reduction from this much doubt

inline bool is_noisy(Move m) { return is_capture(m) || (is_promotion(m) && promo_type(m) == QUEEN); }

inline PieceType captured_type(const Position& pos, Move m) {
    return move_flag(m) == EN_PASSANT ? PAWN : type_of(pos.piece_on(move_to(m)));
}

// Squares the side not to move attacks (ThreatHist).
Bitboard threats_against_us(const Position& pos) {
    const Color them = ~pos.side_to_move();
    const Bitboard occ = pos.pieces();
    Bitboard t = pawn_attacks_bb(them, pos.pieces(them, PAWN));
    for (PieceType pt : {KNIGHT, BISHOP, ROOK, QUEEN, KING}) {
        Bitboard b = pos.pieces(them, pt);
        while (b) t |= attacks_of(pt, pop_lsb(b), occ);
    }
    return t;
}

const char* BENCH_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P4/2PBPN2/PP1N1PPP/R1BQ1RK1 w - - 0 9",
    "2rq1rk1/pb2bppp/1pn1pn2/2pp4/3P4/1PN1PN2/PBQ2PPP/2RR1BK1 w - - 4 13",
    "r2qr1k1/1p1n1pbp/p2p1np1/2pP4/P3P3/2N2N2/1P2BPPP/R1BQR1K1 w - - 1 14",
    "3r2k1/pp3ppp/2p5/4P3/2P1n3/1P3N2/P4PPP/3R2K1 b - - 0 24",
    "8/5pk1/3p2p1/2pP3p/2P2P1P/6P1/5K2/8 w - - 0 45",
    "6k1/5p2/6p1/8/3B4/1P3KP1/5P2/2b5 w - - 3 50",
};
}  // namespace

void TimeManager::init(const SearchLimits& limits, Color us, int overhead) {
    start = std::chrono::steady_clock::now();
    timed = false;
    if (limits.movetime > 0) {
        timed = true;
        soft_ms = hard_ms = std::max<i64>(1, limits.movetime - overhead);
    } else if (limits.time[us] >= 0 && !limits.infinite) {
        timed = true;
        i64 left = std::max<i64>(1, limits.time[us] - overhead);
        int mtg = limits.movestogo > 0 ? std::min(limits.movestogo, 40) : 30;
        // Spend the increment only in proportion to the reserve: below 20 increments left, less of it is used, so
        // a low clock rebuilds instead of hovering near zero (where any scheduling hiccup loses on time).
        const i64 inc = limits.inc[us];
        const double inc_share = inc > 0 ? std::clamp(double(left) / double(20 * inc), 0.0, 0.75) : 0.0;
        // The hard cap is a fixed share of the clock and is never raised to fit the soft target.
        hard_ms = std::max<i64>(1, std::min(left * 2 / 5, (left / mtg + inc) * 4));
        soft_ms = std::clamp<i64>(left / mtg + i64(double(inc) * inc_share), 1, hard_ms);
    }
}

Searcher::Searcher(int id, SharedSearch& shared)
    : id_(id), shared_(shared), stop_flag_(shared.stop), limits_(shared.limits), options_(shared.options),
      tm_(shared.tm) {
    history_.clear();
    thread_ = std::thread([this] { worker_loop(); });
}

Searcher::~Searcher() {
    stop_flag_ = true;
    wait();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Searcher::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return searching_ || quit_; });
        if (quit_) return;
        lock.unlock();
        run();
        lock.lock();
        searching_ = false;
        cv_.notify_all();
    }
}

// Table indices for this node's correction-history entries. CorrPlus adds keys for each side's non-pawn pieces,
// for the minor pieces and kings, and the previous two moves (continuation correction).
Searcher::CorrIndex Searcher::corr_index(int ply) const {
    CorrIndex ci;
    ci.pawn = int(pos_.state().pawn_key & (CORR_SIZE - 1));
    if (!options_.features.corr_plus) return ci;
    auto key_of = [&](Bitboard b) {
        u64 k = 0;
        while (b) {
            const int s = pop_lsb(b);
            k ^= zobrist::PIECE_SQUARE[pos_.piece_on(s)][s];
        }
        return int((k ^ (k >> 32)) & (CORR_SIZE - 1));
    };
    for (Color c : {WHITE, BLACK}) ci.nonpawn[c] = key_of(pos_.pieces(c) & ~pos_.pieces(PAWN));
    ci.minor = key_of(pos_.pieces(KNIGHT, BISHOP) | pos_.pieces(KING));
    const StackEntry& p1 = stack_[ply + 3];
    const StackEntry& p2 = stack_[ply + 2];
    if (p1.move && p1.piece != NO_PIECE && p2.move && p2.piece != NO_PIECE)
        ci.cont = const_cast<i16*>(
            &history_.cont_corr[piece_index(p2.piece)][move_to(p2.move)][piece_index(p1.piece)][move_to(p1.move)]);
    return ci;
}

int Searcher::correct_eval(int raw, const CorrIndex& ci) const {
    if (!options_.features.correction) return raw;
    const int stm = pos_.side_to_move();
    const int pawn = history_.pawn_corr[stm][ci.pawn];
    int c;
    if (!options_.features.corr_plus) {
        c = pawn / 16;
    } else {   // weighted sum, weights in 1/128 (they add up to 1.25: the tables share the error between them)
        const int sum = 48 * pawn + 32 * history_.nonpawn_corr[stm][WHITE][ci.nonpawn[WHITE]] +
                        32 * history_.nonpawn_corr[stm][BLACK][ci.nonpawn[BLACK]] +
                        24 * history_.minor_corr[stm][ci.minor] + (ci.cont ? 24 * *ci.cont : 0);
        c = sum / (128 * 16);
    }
    return std::clamp(raw + c, -VALUE_MATE_IN_MAX + 1, VALUE_MATE_IN_MAX - 1);
}

// Moves the correction entries towards the search result's difference from the raw evaluation.
void Searcher::update_correction(int raw_eval, int best, int depth, const CorrIndex& ci) {
    const int target = std::clamp((best - raw_eval) * 16, -CORR_LIMIT, CORR_LIMIT);
    const int w = std::min(depth * depth + 2 * depth + 1, 128);
    auto move = [&](i16& e) { e = i16(std::clamp((e * (512 - w) + target * w) / 512, -CORR_LIMIT, CORR_LIMIT)); };
    const int stm = pos_.side_to_move();
    move(history_.pawn_corr[stm][ci.pawn]);
    if (!options_.features.corr_plus) return;
    move(history_.nonpawn_corr[stm][WHITE][ci.nonpawn[WHITE]]);
    move(history_.nonpawn_corr[stm][BLACK][ci.nonpawn[BLACK]]);
    move(history_.minor_corr[stm][ci.minor]);
    if (ci.cont) move(*ci.cont);
}

void Searcher::start(const Position& pos) {
    wait();
    pos_ = pos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        searching_ = true;
    }
    cv_.notify_all();
}

void Searcher::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !searching_; });
}

bool Searcher::should_stop() {
    if (stop_flag_) return true;
    const u64 n = nodes();
    if (id_ == 0 && (n & 1023) == 0) {   // only the main thread watches the clock and the node limit
        if (limits_.nodes && n >= limits_.nodes) stop_flag_ = true;
        if (tm_.timed && tm_.elapsed() >= tm_.hard_ms) stop_flag_ = true;
    }
    return stop_flag_;
}

void Searcher::report(int depth, int score, int alpha, int beta) const {
    if (silent_) return;
    i64 ms = std::max<i64>(tm_.elapsed(), 1);
    std::ostringstream out;
    out << "info depth " << depth << " seldepth " << seldepth_ << " score ";
    if (std::abs(score) >= VALUE_MATE_IN_MAX)
        out << "mate " << (score > 0 ? (VALUE_MATE - score + 1) / 2 : -(VALUE_MATE + score) / 2);
    else
        out << "cp " << score;
    if (score >= beta) out << " lowerbound";
    else if (score <= alpha) out << " upperbound";
    const u64 total = shared_.pool ? shared_.pool->total_nodes() : nodes();
    out << " nodes " << total << " nps " << total * 1000 / ms << " hashfull " << TT.hashfull() << " time " << ms
        << " pv";
    for (int i = 0; i < pv_len_[0]; i++) out << ' ' << move_to_uci(pv_[0][i]);
    out_line(out.str());
}

void Searcher::run() {
    const i64 wake_ms = tm_.elapsed();   // time from "go" until this thread started working
    nodes_.store(0, std::memory_order_relaxed);
    best_move_ = NO_MOVE;
    for (auto& e : stack_) e = StackEntry{};
    MoveList root;
    generate_legal(pos_, root);
    if (root.size() > 0) best_move_ = root[0];   // always have a legal move to play
    eval_.reset(pos_);
    int max_depth = limits_.depth > 0 ? std::min(limits_.depth, MAX_PLY - 1) : MAX_PLY - 1;
    const bool watch = options_.features.root_watch;
    int prev_score = 0, stability = 0;
    double trouble = 0;   // RootWatch: recent root fail-lows, each iteration's count added to half the old value
    Move prev_best = NO_MOVE;
    for (int depth = 1; depth <= max_depth && root.size() > 0; depth++) {
        root_depth_ = depth;
        std::memset(root_move_nodes_, 0, sizeof(root_move_nodes_));
        seldepth_ = 0;
        // Aspiration window around the previous score, widened on each fail.
        int delta = 18 + 3 * (id_ % 4), alpha = -VALUE_INF, beta = VALUE_INF;   // helpers vary the window a little
        if (depth >= 4) {
            alpha = std::max(prev_score - delta, -VALUE_INF);
            beta = std::min(prev_score + delta, VALUE_INF);
        }
        int score, fail_lows = 0;
        for (;;) {
            pv_len_[0] = 0;
            score = search(alpha, beta, depth, 0, false);
            if (stop_flag_) break;
            if (score <= alpha) {
                fail_lows++;
                beta = (alpha + beta) / 2;
                alpha = std::max(score - delta, -VALUE_INF);
            } else if (score >= beta) {
                beta = std::min(score + delta, VALUE_INF);
            } else {
                break;
            }
            delta += delta / 2;
        }
        if (stop_flag_ && depth > 1) {   // an interrupted iteration is discarded...
            // ...unless (RootWatch) it had already found a different root move that beat the window with a
            // completed full-window search: that move is proven better at this depth than the one about to be played.
            if (watch && id_ == 0 && pv_len_[0] > 0 && pv_[0][0] != best_move_) {
                best_move_ = pv_[0][0];
                if (!silent_) out_line("info string RootWatch: unfinished depth " + std::to_string(depth) +
                                       " replaced the move with " + move_to_uci(best_move_));
            }
            break;
        }
        if (pv_len_[0] > 0) best_move_ = pv_[0][0];
        if (id_ != 0) continue;   // helpers just keep deepening until the main thread stops them
        report(depth, score, -VALUE_INF, VALUE_INF);
        trouble = trouble / 2 + fail_lows;
        // Soft limit, scaled by how settled the search looks
        double scale = 1.0;
        if (options_.features.smart_time && depth >= 6) {
            stability = best_move_ == prev_best ? std::min(stability + 1, 8) : 0;
            const double stab = 1.6 - 0.11 * stability;                                     // 1.6 .. 0.72
            const double drop = std::clamp(1.0 + (prev_score - score) / 120.0, 0.85, 1.5);    // falling score: think more
            const double share =
                double(root_move_nodes_[move_from(best_move_)][move_to(best_move_)]) / double(std::max<u64>(nodes(), 1));
            const double focus = std::clamp(1.55 - share, 0.6, 1.4);                      // one dominant move: think less
            scale = stab * drop * focus * 0.75;
            // RootWatch: each recent fail-low means the previous answer was wrong; keep looking (hard limit still caps)
            if (watch) scale *= std::min(1.0 + 0.15 * trouble, 1.6);
        }
        prev_best = best_move_;
        prev_score = best_score_ = score;
        if (tm_.timed && tm_.elapsed() >= i64(double(tm_.soft_ms) * scale)) break;
        if (stop_flag_) break;
    }
    if (id_ != 0) return;
    // In infinite/ponder mode the GUI expects bestmove only after "stop".
    while (limits_.infinite && !stop_flag_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (shared_.pool) shared_.pool->stop_helpers();
    if (!silent_) out_line("bestmove " + move_to_uci(best_move_));
    // Diagnostics: with NARC_TIMELOG=<file>, every move is logged (wake-up delay, total time, limits, clock).
    static const char* timelog = std::getenv("NARC_TIMELOG");
    if (timelog && tm_.timed && !silent_) {
        if (FILE* f = std::fopen(timelog, "a")) {
            const std::string line = "wake " + std::to_string(wake_ms) + " total " + std::to_string(tm_.elapsed()) +
                                     " soft " + std::to_string(tm_.soft_ms) + " hard " + std::to_string(tm_.hard_ms) +
                                     " clock " + std::to_string(limits_.time[pos_.side_to_move()]) + " inc " +
                                     std::to_string(limits_.inc[pos_.side_to_move()]) + " nodes " +
                                     std::to_string(nodes()) + " depth " + std::to_string(root_depth_) + "\n";
            std::fputs(line.c_str(), f);
            std::fclose(f);
        }
    }
}

void Searcher::update_quiet_stats(int ply, Move best, int bonus, const Move* tried, int n_tried, Bitboard threats) {
    StackEntry& ss = stack_[ply + 4];
    const int side = pos_.side_to_move();
    if (ss.killers[0] != best) {
        ss.killers[1] = ss.killers[0];
        ss.killers[0] = best;
    }
    const StackEntry& prev = stack_[ply + 3];
    if (prev.move != NO_MOVE && prev.piece != NO_PIECE)
        history_.counter[piece_index(prev.piece)][move_to(prev.move)] = best;
    auto apply = [&](Move m, int b) {
        const int pi = piece_index(pos_.moved_piece(m)), to = move_to(m), from = move_from(m);
        if (options_.features.threat_hist)
            history_update(history_.quiet_threat[side][(threats >> from) & 1][(threats >> to) & 1][from][to], b);
        else
            history_update(history_.quiet[side][from][to], b);
        if (stack_[ply + 3].cont) history_update((*stack_[ply + 3].cont)[pi][to], b);
        if (stack_[ply + 2].cont) history_update((*stack_[ply + 2].cont)[pi][to], b);
        if (options_.features.cont_hist4 && stack_[ply].cont) history_update((*stack_[ply].cont)[pi][to], b);
    };
    apply(best, bonus);
    for (int i = 0; i < n_tried; i++)
        if (tried[i] != best) apply(tried[i], -bonus);
}

void Searcher::update_capture_stats(Move best, int bonus, const Move* tried, int n_tried) {
    auto apply = [&](Move m, int b) {
        if (!is_capture(m)) return;
        history_update(history_.capture[piece_index(pos_.moved_piece(m))][move_to(m)][captured_type(pos_, m)], b);
    };
    apply(best, bonus);
    for (int i = 0; i < n_tried; i++)
        if (tried[i] != best) apply(tried[i], -bonus);
}

int Searcher::search(int alpha, int beta, int depth, int ply, bool cutnode) {
    const bool pv = beta - alpha > 1;
    const bool root = ply == 0;
    if (depth <= 0) return qsearch(alpha, beta, ply);

    count_node();
    pv_len_[ply] = 0;
    seldepth_ = std::max(seldepth_, ply);
    StackEntry& ss = stack_[ply + 4];
    const Move excluded = ss.excluded;
    stack_[ply + 5].double_ext = ss.double_ext;

    if (!root) {
        if (should_stop()) return 0;
        if (pos_.is_repetition() || pos_.is_fifty_move_draw() || pos_.insufficient_material()) return 0;
        if (ply >= MAX_PLY - 1) return pos_.in_check() ? 0 : eval_.evaluate(pos_);
        // mate-distance pruning
        alpha = std::max(alpha, -VALUE_MATE + ply);
        beta = std::min(beta, VALUE_MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    // Transposition table
    const u64 key = pos_.key();
    TTProbe tt = excluded ? TTProbe{} : TT.probe(key, ply);
    const Move tt_move = root && best_move_ ? best_move_ : tt.move;
    if (!pv && tt.hit && tt.depth >= depth && tt.score != VALUE_NONE &&
        (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && tt.score >= beta) ||
         (tt.bound == BOUND_UPPER && tt.score <= alpha)))
        return tt.score;

    const SearchFeatures& f = options_.features;
    const bool tt_pv = pv || (f.tt_pv && tt.hit && tt.pv);   // this position is or was on a principal line

    // Static evaluation
    const bool in_check = pos_.in_check();
    const CorrIndex ci = corr_index(ply);
    int raw_eval = VALUE_NONE, eval = VALUE_NONE;
    int doubt = 0;   // Doubt: how far the correction histories moved the network's eval (cp); large = unreliable here
    if (!in_check) {
        raw_eval = tt.hit && tt.eval != VALUE_NONE ? tt.eval : eval_.evaluate(pos_);
        eval = correct_eval(raw_eval, ci);
        if (f.doubt) doubt = std::min(std::abs(eval - raw_eval), DOUBT_CAP);
        if (f.fifty_damp) eval = eval * (200 - pos_.state().halfmove) / 200;
        // a bounded search score is a better estimate than the static eval when it points the right way
        if (tt.hit && tt.score != VALUE_NONE &&
            (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && tt.score > eval) ||
             (tt.bound == BOUND_UPPER && tt.score < eval)))
            eval = tt.score;
    }
    ss.eval = in_check ? VALUE_NONE : eval;
    const int past = stack_[ply + 2].eval != VALUE_NONE ? stack_[ply + 2].eval : stack_[ply].eval;
    const bool improving = !in_check && past != VALUE_NONE && eval > past;
    stack_[ply + 5].killers[0] = stack_[ply + 5].killers[1] = NO_MOVE;

    // Doubt: where the eval is known to be off, every "the eval is far enough from the window" test asks for more
    const int doubt_margin = doubt * 3 / 4;

    if (f.pruning && !pv && !in_check && !excluded) {
        // Reverse futility pruning: far above beta with little depth left
        if (depth <= 8 && eval < VALUE_MATE_IN_MAX && eval - (TUNE.rfp_mul * depth - TUNE.rfp_imp * improving) - doubt_margin >= beta)
            return (eval + beta) / 2;

        // Razoring: hopeless at low depth unless captures change it
        if (depth <= 3 && eval + 220 + 230 * depth + doubt_margin < alpha) {
            int q = qsearch(alpha - 1, alpha, ply);
            if (q < alpha) return q;
        }

        // Null-move pruning
        if (depth >= 3 && eval >= beta && stack_[ply + 3].move != NO_MOVE &&
            pos_.has_non_pawn_material(pos_.side_to_move())) {
            int r = 3 + depth / 3 + std::min((eval - beta) / std::max(TUNE.nmp_div, 1), 3);
            ss.move = NO_MOVE;
            ss.piece = NO_PIECE;
            ss.cont = nullptr;
            pos_.make_null();
            eval_.push();
            int score = -search(-beta, -beta + 1, depth - r, ply + 1, !cutnode);
            eval_.pop();
            pos_.unmake_null();
            if (stop_flag_) return 0;
            if (score >= beta) return score >= VALUE_MATE_IN_MAX ? beta : score;
        }
    }

    // ProbCut: a capture that beats beta by a margin in a shallow search almost surely beats beta at full depth
    if (f.probcut && !pv && !in_check && !excluded && depth >= 5 && std::abs(beta) < VALUE_MATE_IN_MAX) {
        const int pc_beta = beta + 190 - 60 * improving;
        if (!(tt.hit && tt.depth >= depth - 3 && tt.score != VALUE_NONE && tt.score < pc_beta)) {
            MovePicker pc(pos_, tt.move, history_);   // captures and queen promotions only
            while (Move m = pc.next()) {
                if (!pos_.see_ge(m, pc_beta - eval)) continue;
                const Piece moved = pos_.moved_piece(m);
                ss.move = m;
                ss.piece = moved;
                ss.cont = &history_.cont[piece_index(moved)][move_to(m)];
                pos_.make_move(m);
                eval_.push();
                TT.prefetch(pos_.key());
                int score = -qsearch(-pc_beta, -pc_beta + 1, ply + 1);
                if (score >= pc_beta) score = -search(-pc_beta, -pc_beta + 1, depth - 4, ply + 1, !cutnode);
                eval_.pop();
                pos_.unmake_move(m);
                if (stop_flag_) return 0;
                if (score >= pc_beta) {
                    TT.store(key, ply, m, score, raw_eval, depth - 3, BOUND_LOWER, false, tt_pv);
                    return score;
                }
            }
        }
    }

    // Internal iterative reduction: without a hash move this node is probably not worth the full depth
    if (f.reductions && (pv || cutnode) && depth >= 4 && !tt_move) depth--;

    const StackEntry& prev = stack_[ply + 3];
    const Move counter =
        prev.move && prev.piece != NO_PIECE ? history_.counter[piece_index(prev.piece)][move_to(prev.move)] : NO_MOVE;
    const Bitboard threats = f.threat_hist ? threats_against_us(pos_) : 0;
    MovePicker picker(pos_, tt_move, history_, ss.killers, counter, stack_[ply + 3].cont, stack_[ply + 2].cont,
                      f.cont_hist4 ? stack_[ply].cont : nullptr, f.threat_hist, threats);

    int best = -VALUE_INF, moves_played = 0;
    Move best_move = NO_MOVE;
    Move quiets_tried[64], captures_tried[32];
    int n_quiets = 0, n_captures = 0;
    const int lmp_limit = (3 + depth * depth) / (2 - improving);

    while (Move m = picker.next()) {
        if (m == excluded) continue;
        const bool quiet = !is_noisy(m);
        const int hist_score = quiet ? picker.last_score() : 0;

        // Move-level pruning (never when every line so far is mated)
        if (f.pruning && !root && best > -VALUE_MATE_IN_MAX && pos_.has_non_pawn_material(pos_.side_to_move())) {
            if (quiet) {
                if (moves_played >= lmp_limit) {
                    picker.skip_quiets();
                    continue;
                }
                if (!in_check && depth <= 8 && eval + TUNE.fut_base + TUNE.fut_mul * depth + doubt_margin <= alpha) {
                    picker.skip_quiets();
                    continue;
                }
                if (depth <= 4 && hist_score < -TUNE.hist_prune * depth) continue;
                if (depth <= 8 && !pos_.see_ge(m, -TUNE.see_quiet * depth)) continue;
            } else if (depth <= 8 && !pos_.see_ge(m, -TUNE.see_cap * depth * depth)) {
                continue;
            }
        }

        // Singular extension: if every alternative to the hash move fails well below its score, the hash move is
        // forced and gets an extra ply; if even the alternatives beat beta, cut here (multi-cut).
        int extension = 0;
        if (f.singular && !root && m == tt_move && depth >= 7 && !excluded && tt.depth >= depth - 3 &&
            (tt.bound & BOUND_LOWER) && std::abs(tt.score) < VALUE_MATE_IN_MAX && ply < 2 * root_depth_) {
            const int s_beta = tt.score - 2 * depth;
            ss.excluded = m;
            const int s = search(s_beta - 1, s_beta, (depth - 1) / 2, ply, cutnode);
            ss.excluded = NO_MOVE;
            if (stop_flag_) return 0;
            if (s < s_beta) extension = !pv && s < s_beta - TUNE.sing_margin ? 2 : 1;
            else if (s_beta >= beta) return s_beta;
            else if (tt.score >= beta) extension = -1;
        }

        moves_played++;
        const Piece moved = pos_.moved_piece(m);
        ss.move = m;
        ss.piece = moved;
        ss.cont = &history_.cont[piece_index(moved)][move_to(m)];
        pos_.make_move(m);
        eval_.push();
        TT.prefetch(pos_.key());

        const bool gives_check = pos_.in_check();
        if (!extension && gives_check && ply < 2 * root_depth_) extension = 1;
        if (f.double_ext_cap && extension >= 2 && ss.double_ext >= 5) extension = 1;
        stack_[ply + 5].double_ext = ss.double_ext + (extension >= 2);
        int new_depth = depth - 1 + extension;
        int score;
        const u64 nodes_before = nodes();

        if (f.reductions && depth >= 3 && moves_played > 1 + 2 * root) {
            int r = LMR[std::min(depth, 63)][std::min(moves_played, 63)];
            r += cutnode;
            if (f.tt_pv) {
                r -= tt_pv;
                if (quiet) {
                    r += tt_move && is_noisy(tt_move);   // the best move here is probably a capture
                    r -= m == ss.killers[0] || m == ss.killers[1] || m == counter;
                }
            } else {
                r -= pv;
            }
            r -= improving;
            r -= gives_check;
            r -= doubt >= DOUBT_LMR;   // Doubt: don't cut a line short on an eval that is known to be off
            if (quiet) r -= std::clamp(hist_score / std::max(TUNE.lmr_hist, 1), -2, 2);
            else r /= 2;
            int reduced = std::clamp(new_depth - r, 1, new_depth);
            score = -search(-alpha - 1, -alpha, reduced, ply + 1, true);
            if (score > alpha && reduced < new_depth) score = -search(-alpha - 1, -alpha, new_depth, ply + 1, !cutnode);
        } else if (!pv || moves_played > 1) {
            score = -search(-alpha - 1, -alpha, new_depth, ply + 1, !cutnode);
        } else {
            score = alpha + 1;   // forces the full-window search below for the first PV move
        }
        if (pv && (moves_played == 1 || score > alpha)) score = -search(-beta, -alpha, new_depth, ply + 1, false);

        eval_.pop();
        pos_.unmake_move(m);
        if (stop_flag_) return 0;
        if (root) root_move_nodes_[move_from(m)][move_to(m)] += nodes() - nodes_before;

        if (score > best) {
            best = score;
            if (score > alpha) {
                best_move = m;
                alpha = score;
                if (pv) {
                    pv_[ply][0] = m;
                    for (int i = 0; i < pv_len_[ply + 1]; i++) pv_[ply][i + 1] = pv_[ply + 1][i];
                    pv_len_[ply] = pv_len_[ply + 1] + 1;
                }
                if (alpha >= beta) break;
            }
        }
        if (m != best_move) {
            if (quiet && n_quiets < 64) quiets_tried[n_quiets++] = m;
            else if (!quiet && n_captures < 32) captures_tried[n_captures++] = m;
        }
    }

    if (moves_played == 0) {
        if (excluded) return alpha;
        // with pruning, "no move played" may also mean every move was pruned: only mate/stalemate if none exist
        MoveList any;
        generate_legal(pos_, any);
        if (any.size() == 0) return in_check ? -VALUE_MATE + ply : 0;
        return alpha;
    }

    if (best >= beta) {
        const int bonus = stat_bonus(depth);
        if (!is_noisy(best_move)) update_quiet_stats(ply, best_move, bonus, quiets_tried, n_quiets, threats);
        update_capture_stats(best_move, bonus, captures_tried, n_captures);
    }

    if (!excluded) {
        Bound bound = best >= beta ? BOUND_LOWER : pv && best_move ? BOUND_EXACT : BOUND_UPPER;
        TT.store(key, ply, best_move, best, raw_eval, depth, bound, pv, tt_pv);
        // learn the evaluation's error, only from results that bound it in a meaningful direction
        if (f.correction && !in_check && (!best_move || !is_noisy(best_move)) &&
            !(bound == BOUND_LOWER && best <= eval) && !(bound == BOUND_UPPER && best >= eval) &&
            std::abs(best) < VALUE_MATE_IN_MAX)
            update_correction(raw_eval, best, depth, ci);
    }
    return best;
}

int Searcher::qsearch(int alpha, int beta, int ply) {
    const bool pv = beta - alpha > 1;
    count_node();
    pv_len_[ply] = 0;
    seldepth_ = std::max(seldepth_, ply);
    if (should_stop()) return 0;
    if (pos_.is_fifty_move_draw() || pos_.insufficient_material()) return 0;
    const bool in_check = pos_.in_check();
    if (ply >= MAX_PLY - 1) return in_check ? 0 : eval_.evaluate(pos_);

    const u64 key = pos_.key();
    TTProbe tt = TT.probe(key, ply);
    if (!pv && tt.hit && tt.score != VALUE_NONE &&
        (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && tt.score >= beta) ||
         (tt.bound == BOUND_UPPER && tt.score <= alpha)))
        return tt.score;

    int best = -VALUE_INF, raw_eval = VALUE_NONE, eval = VALUE_NONE;
    if (!in_check) {
        raw_eval = tt.hit && tt.eval != VALUE_NONE ? tt.eval : eval_.evaluate(pos_);
        eval = correct_eval(raw_eval, corr_index(ply));
        if (options_.features.fifty_damp) eval = eval * (200 - pos_.state().halfmove) / 200;
        if (tt.hit && tt.score != VALUE_NONE &&
            (tt.bound == BOUND_EXACT || (tt.bound == BOUND_LOWER && tt.score > eval) ||
             (tt.bound == BOUND_UPPER && tt.score < eval)))
            eval = tt.score;
        best = eval;
        if (best >= beta) {
            if (!tt.hit) TT.store(key, ply, NO_MOVE, best, raw_eval, -1, BOUND_LOWER, false);
            return best;
        }
        alpha = std::max(alpha, best);
    }

    MovePicker picker(pos_, tt.move, history_);
    Move best_move = NO_MOVE;
    int moves_played = 0;
    const int futility_base = eval + 180;
    while (Move m = picker.next()) {
        // QSEvasion: one evasion already holds off mate; quiet evasions rarely change the result
        if (in_check && options_.features.qs_evasion && best > -VALUE_MATE_IN_MAX && !is_noisy(m)) continue;
        if (!in_check) {
            if (!pos_.see_ge(m, 0)) continue;   // losing captures can't raise a quiet position's score
            if (!is_promotion(m) && futility_base <= alpha && !pos_.see_ge(m, 1)) {
                best = std::max(best, futility_base);
                continue;
            }
        }
        moves_played++;
        pos_.make_move(m);
        eval_.push();
        TT.prefetch(pos_.key());
        int score = -qsearch(-beta, -alpha, ply + 1);
        eval_.pop();
        pos_.unmake_move(m);
        if (stop_flag_) return 0;
        if (score > best) {
            best = score;
            if (score > alpha) {
                best_move = m;
                alpha = score;
                if (pv) {
                    pv_[ply][0] = m;
                    for (int i = 0; i < pv_len_[ply + 1]; i++) pv_[ply][i + 1] = pv_[ply + 1][i];
                    pv_len_[ply] = pv_len_[ply + 1] + 1;
                }
                if (alpha >= beta) break;
            }
        }
    }
    if (in_check && moves_played == 0) return -VALUE_MATE + ply;

    TT.store(key, ply, best_move, best, raw_eval, 0, best >= beta ? BOUND_LOWER : BOUND_UPPER, false);
    return best;
}

int Searcher::label(const Position& pos, u64 nodes, Move& best) {
    pos_ = pos;
    limits_ = SearchLimits{};
    limits_.nodes = nodes;
    stop_flag_ = false;
    tm_.init(limits_, pos.side_to_move(), 0);
    silent_ = true;
    run();
    silent_ = false;
    best = best_move_;
    return best_score_;
}

void Searcher::bench(int depth) {
    u64 total = 0;
    auto t0 = std::chrono::steady_clock::now();
    silent_ = true;
    TT.clear();
    history_.clear();
    for (const char* fen : BENCH_FENS) {
        pos_.set_fen(fen);
        limits_ = SearchLimits{};
        limits_.depth = depth;
        stop_flag_ = false;
        tm_.init(limits_, pos_.side_to_move(), 0);
        run();
        total += nodes();
        TT.clear();
        history_.clear();
    }
    silent_ = false;
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << total << " nodes " << u64(total / std::max(s, 1e-9)) << " nps" << std::endl;
}

}  // namespace narc

namespace narc {

ThreadPool::ThreadPool() {
    shared_.pool = this;
    set_threads(1);
}

ThreadPool::~ThreadPool() {
    stop();
    wait();
    workers_.clear();
}

void ThreadPool::set_threads(int n) {
    n = std::clamp(n, 1, 256);
    if (int(workers_.size()) == n) return;
    workers_.clear();   // joins the old threads
    for (int i = 0; i < n; i++) workers_.push_back(std::make_unique<Searcher>(i, shared_));
}

void ThreadPool::start(const Position& pos, const SearchLimits& limits, const Options& options) {
    wait();
    shared_.tm.init(limits, pos.side_to_move(), options.move_overhead);   // the clock starts when "go" arrives
    shared_.limits = limits;
    shared_.options = options;
    shared_.stop = false;
    TT.new_search();
    for (size_t i = 1; i < workers_.size(); i++) workers_[i]->start(pos);
    workers_[0]->start(pos);
}

void ThreadPool::wait() { workers_[0]->wait(); }

void ThreadPool::stop_helpers() {
    shared_.stop = true;
    for (size_t i = 1; i < workers_.size(); i++) workers_[i]->wait();
}

void ThreadPool::new_game() {
    for (auto& w : workers_) w->new_game();
}

u64 ThreadPool::total_nodes() const {
    u64 n = 0;
    for (const auto& w : workers_) n += w->nodes();
    return n;
}

}  // namespace narc

// By Madushan Dissanayake
