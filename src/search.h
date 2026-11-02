<<<<<<< HEAD
// NARC Genesys - search.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "history.h"
#include "nnue.h"
#include "position.h"
#include "tt.h"

namespace narc {


// Search constants tuned by SPSA (UCI options named as the fields). Defaults: the 0.12 SPSA values (14,000
// iterations on 0.11 kb16); the hand-set values of 0.5-0.11 are in the comments.
// The following SPSA finetuned values, Only performs +8 elo on 0.11 kb16, but the SPSA values are more robust across time controls and hardware.
struct TuneParams {             // SPSA value; hand-set value (0.5-0.11) in brackets
    int lmr_base = 76;          // (70)   LMR table: base / 100 + ln d * ln m / (lmr_div / 100)
    int lmr_div = 218;          // (230)
    int fut_base = 110;         // (110)  quiet futility: eval + fut_base + fut_mul * depth <= alpha
    int fut_mul = 93;           // (95)
    int rfp_mul = 69;           // (80)   reverse futility: eval - (rfp_mul * depth - rfp_imp * improving) >= beta
    int rfp_imp = 63;           // (60)
    int nmp_div = 200;          // (200)  null move: R += min((eval - beta) / nmp_div, 3)
    int see_quiet = 56;         // (55)   quiet SEE pruning: see_ge(m, -see_quiet * depth)
    int see_cap = 28;           // (28)   capture SEE pruning: see_ge(m, -see_cap * depth * depth)
    int hist_prune = 2043;      // (2200) history pruning: hist < -hist_prune * depth
    int sing_margin = 24;       // (24)   double extension when the singular search fails this far below
    int lmr_hist = 6859;        // (7000) LMR: r -= clamp(hist / lmr_hist, -2, 2)
};
extern TuneParams TUNE;
void init_lmr();                                    // rebuilds the LMR table from TUNE
int* tune_param(const std::string& lower_name);     // the field named so, or nullptr

struct SearchLimits {
    int depth = 0;          // 0 = no depth limit
    u64 nodes = 0;          // 0 = no node limit
    i64 movetime = 0;       // ms; 0 = not set
    i64 time[2] = {-1, -1}; // ms left on each clock; -1 = not set
    i64 inc[2] = {0, 0};
    int movestogo = 0;
    bool infinite = false;
};

// Search switches (hidden UCI options, for testing each technique in games).
struct SearchFeatures {
    bool pruning = true;      // reverse futility, null move, razoring, move-count/futility/SEE/history pruning
    bool reductions = true;   // late move reductions and internal iterative reduction
    bool singular = true;     // singular / multi-cut / negative extensions
    bool correction = true;   // pawn-structure correction history
    bool smart_time = true;   // best-move stability, score trend and node share scale the soft limit
    // 0.5 features, each a UCI option. CorrPlus is on (+46 +- 19 in 400 games); TTPV and ProbCut stay off
    // (-5 +- 31 and -2 +- 30 in 200 games each).
    bool corr_plus = true;    // non-pawn, minor-piece and continuation correction histories
    bool tt_pv = false;       // TT "was on the PV" bit, TT-capture and killer/counter terms in late move reductions
    bool probcut = false;     // ProbCut: a capture that beats beta by a margin at reduced depth cuts the node
    // Batch 2 (under test, off by default)
    bool threat_hist = false;      // quiet history split by whether the from / to squares are attacked
    bool cont_hist4 = false;       // continuation history from the move 4 plies back
    bool fifty_damp = false;       // static eval shrinks towards 0 as the fifty-move counter rises
    bool qs_evasion = false;       // quiescence: after one evasion that isn't mated, skip quiet evasions
    bool double_ext_cap = false;   // at most 5 double extensions along one line
    // Batch 3 (under test, off by default)
    bool doubt = false;        // the size of the correction-history adjustment widens pruning margins and trims LMR
    bool root_watch = false;   // root fail-lows lengthen the think; an interrupted iteration keeps a proven better move
};

struct Options {
    int hash_mb = 16;
    int threads = 1;
    int move_overhead = 10;
    SearchFeatures features;
};

// Soft limit: don't start another iteration after it. Hard limit: abort the search.
struct TimeManager {
    i64 soft_ms = 0, hard_ms = 0;
    bool timed = false;
    std::chrono::steady_clock::time_point start;
    void init(const SearchLimits& limits, Color us, int overhead);
    i64 elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    }
};

struct StackEntry {
    Move move = NO_MOVE;
    Piece piece = NO_PIECE;
    int eval = VALUE_NONE;
    Move killers[2] = {NO_MOVE, NO_MOVE};
    ContinuationRow* cont = nullptr;   // continuation-history row for this ply's move
    Move excluded = NO_MOVE;
    int double_ext = 0;   // double extensions on the path from the root to this node
};

class ThreadPool;

// State every search thread shares for one "go".
struct SharedSearch {
    std::atomic<bool> stop{false};
    SearchLimits limits;
    Options options;
    TimeManager tm;
    ThreadPool* pool = nullptr;
};

// One search thread (Lazy SMP). Thread 0 is the main thread: it manages time, reports and picks the move;
// helpers search the same root independently and share only the transposition table.
class Searcher {
public:
    Searcher(int id, SharedSearch& shared);
    ~Searcher();
    void start(const Position& pos);   // wake the thread on this root position
    void wait();                       // until this thread's search has finished
    void new_game() { history_.clear(); }
    u64 nodes() const { return nodes_.load(std::memory_order_relaxed); }

    // Fixed-depth searches of the bench positions; prints the node total (the search signature) and speed.
    void bench(int depth);
    // Synchronous silent search of `pos` limited to `nodes` (for labelling training data); returns the score of
    // the last completed iteration (side to move, centipawns) and its best move.
    int label(const Position& pos, u64 nodes, Move& best);

private:
    void run();
    void worker_loop();   // the persistent search thread: waits for start(), runs, repeats
    int search(int alpha, int beta, int depth, int ply, bool cutnode);
    int qsearch(int alpha, int beta, int ply);
    bool should_stop();
    void report(int depth, int score, int alpha, int beta) const;
    void update_quiet_stats(int ply, Move best, int bonus, const Move* tried, int n_tried, Bitboard threats);
    void update_capture_stats(Move best, int bonus, const Move* tried, int n_tried);
    struct CorrIndex {   // where this node's correction-history entries live
        int pawn, nonpawn[2], minor;
        i16* cont = nullptr;
    };
    CorrIndex corr_index(int ply) const;
    int correct_eval(int raw, const CorrIndex& ci) const;
    void update_correction(int raw_eval, int best, int depth, const CorrIndex& ci);

    void count_node() { nodes_.store(nodes_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

    const int id_;
    SharedSearch& shared_;
    std::atomic<bool>& stop_flag_;
    SearchLimits& limits_;
    Options& options_;
    TimeManager& tm_;
    Position pos_;
    nnue::Evaluator eval_;
    History history_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool searching_ = false, quit_ = false;
    std::atomic<u64> nodes_{0};
    int seldepth_ = 0;
    int root_depth_ = 0;
    bool silent_ = false;
    StackEntry stack_[MAX_PLY + 8];   // stack_[ply + 4]: four sentinel entries below the root
    Move pv_[MAX_PLY + 1][MAX_PLY + 1];
    int pv_len_[MAX_PLY + 1];
    Move best_move_ = NO_MOVE;
    int best_score_ = 0;
    u64 root_move_nodes_[64][64];     // nodes spent below each root move this iteration
};

// The search threads. Same interface as a single searcher for the UCI layer.
class ThreadPool {
public:
    ThreadPool();
    ~ThreadPool();
    void set_threads(int n);   // only while idle
    void start(const Position& pos, const SearchLimits& limits, const Options& options);
    void stop() { shared_.stop = true; }
    void wait();               // until the main thread has printed its best move
    void new_game();
    void bench(int depth) { workers_[0]->bench(depth); }
    int label(const Position& pos, u64 nodes, Move& best) { return workers_[0]->label(pos, nodes, best); }
    u64 total_nodes() const;
    void stop_helpers();       // called by the main thread when it is done: stop and join every helper's search

private:
    SharedSearch shared_;
    std::vector<std::unique_ptr<Searcher>> workers_;
};

}  // namespace narc
=======
// SPDX-License-Identifier: GPL-3.0-or-later
// NARC Engine — Copyright (C) 2026 Madushan Dissanayake. GNU GPL v3+; see LICENSE.
//
// NARC Engine — transposition table, search (Lazy SMP), time management
#pragma once
#include "nnue.h"
#include <memory>
#include <thread>

// ----------------------------- Transposition table ---------------------------
// Shared across threads. Entries store key ^ data so a torn read/write under a
// data race is detected as a key mismatch (a miss) rather than corrupting play.
enum Bound : u8 { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

struct TTEntry {
    u64 key;  // zobrist ^ data
    u64 data; // packed: move(16) | score(16) | eval(16) | depth(8) | bound(8)
};

struct TT {
    static constexpr size_t CLUSTER_SIZE = 4;
    std::vector<TTEntry> table;
    u64 mask = 0;

    void resize(size_t mb) {
        size_t entries = mb * 1024 * 1024 / sizeof(TTEntry);
        size_t buckets = 1;
        while ((buckets * 2) * CLUSTER_SIZE <= entries) buckets *= 2;
        table.assign(buckets * CLUSTER_SIZE, TTEntry{});
        mask = buckets - 1;
    }
    void clear() { std::fill(table.begin(), table.end(), TTEntry{}); }

    bool probe(u64 key, Move& move, int& score, int& eval, int& depth, int& bound) const {
        size_t base = (key & mask) * CLUSTER_SIZE;
        for (size_t i = 0; i < CLUSTER_SIZE; i++) {
            const TTEntry& e = table[base + i];
            u64 d = e.data;
            if (d == 0 || (e.key ^ d) != key) continue;
            move  = Move(d & 0xFFFF);
            score = int(int16_t((d >> 16) & 0xFFFF));
            eval  = int(int16_t((d >> 32) & 0xFFFF));
            depth = int((d >> 48) & 0xFF);
            bound = int((d >> 56) & 0xFF);
            return true;
        }
        return false;
    }
    void store(u64 key, Move move, int score, int eval, int depth, int bound) {
        size_t base = (key & mask) * CLUSTER_SIZE;
        TTEntry* replace = &table[base];
        int replaceDepth = 256;
        for (size_t i = 0; i < CLUSTER_SIZE; i++) {
            TTEntry& candidate = table[base + i];
            u64 candidateData = candidate.data;
            if (candidateData == 0) { replace = &candidate; break; }
            if ((candidate.key ^ candidateData) == key) { replace = &candidate; break; }
            int candidateDepth = int((candidateData >> 48) & 0xFF);
            if (candidateDepth < replaceDepth) {
                replaceDepth = candidateDepth;
                replace = &candidate;
            }
        }
        TTEntry& e = *replace;
        u64 old = e.data;
        bool same = old != 0 && (e.key ^ old) == key;
        if (same) {
            int oldDepth = int((old >> 48) & 0xFF);
            if (bound != BOUND_EXACT && oldDepth > depth + 3) return;
            if (move == MOVE_NONE) move = Move(old & 0xFFFF); // keep old move
        }
        u64 d = u64(u16(move))
              | (u64(u16(int16_t(score))) << 16)
              | (u64(u16(int16_t(eval))) << 32)
              | (u64(u8(depth)) << 48)
              | (u64(u8(bound)) << 56);
        e.key = key ^ d;
        e.data = d;
    }
};

inline TT tt;

// ----------------------------- Search globals --------------------------------
struct SearchLimits {
    s64 softLimit = -1, hardLimit = -1; // ms
    int depthLimit = MAX_PLY - 1;
    u64 nodeLimit = 0;
    bool infinite = false;
};

inline std::atomic<bool> Stopped{false};
inline bool SilentSearch = false; // datagen: suppress info output
inline int Threads = 1;           // number of search threads (UCI "Threads")

// shared per-search settings, distributed to each thread at search start
inline SearchLimits searchLimits;
inline std::chrono::steady_clock::time_point searchStart;

// piece-square index helper for continuation history (piece 0-11 = color*6+type)
inline int psIdx(int pc12, int sq) { return pc12 * 64 + sq; }

struct SearchData {
    int  threadId = 0;
    bool isMain = true;
    u64 nodes = 0;
    int selDepth = 0;
    int rootDepth = 0;
    int completedDepth = 0;
    int lastScore = 0; // score of last completed iteration (for datagen)
    Move lastBest = MOVE_NONE; // best move from the last completed iteration

    Move killers[MAX_PLY + 2][2] = {};
    int  history[2][64][64] = {};
    Move counterMove[12][64] = {};
    // continuation history: previous move at 1, 2, and 4 plies back
    int16_t contHist[3][12 * 64][12 * 64] = {};
    // capture history: [attacker pc12][to][victim piece type]
    int16_t capHist[12][64][6] = {};

    // per-ply stack: what moved to where (for contHist/countermove); -1 = none/null
    int  stackPc12[MAX_PLY + 2];
    int  stackTo[MAX_PLY + 2];

    // per-thread copy of the game's position keys (repetition detection)
    u64 keyHist[1024 + MAX_PLY];
    int keyHistLen = 0;

    Move pvTable[MAX_PLY + 1][MAX_PLY + 1];
    int  pvLen[MAX_PLY + 1] = {};
    int  evalStack[MAX_PLY + 2] = {};

    s64 elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - searchStart).count();
    }
    void clearHistories() {
        memset(history, 0, sizeof(history));
        memset(counterMove, 0, sizeof(counterMove));
        memset(contHist, 0, sizeof(contHist));
        memset(capHist, 0, sizeof(capHist));
        memset(killers, 0, sizeof(killers));
    }
};

// thread pool (index 0 = main). Heap-allocated: each SearchData is ~2.4 MB.
inline std::vector<std::unique_ptr<SearchData>> threadData;

inline void initThreadData(int n) {
    while ((int)threadData.size() < n) threadData.push_back(std::make_unique<SearchData>());
    for (int i = 0; i < n; i++) { threadData[i]->threadId = i; threadData[i]->isMain = (i == 0); }
}
inline void clearAllHistories() {
    for (auto& td : threadData) td->clearHistories();
}
inline u64 totalNodes() {
    u64 n = 0;
    for (auto& td : threadData) n += td->nodes;
    return n;
}

// game-level position history (maintained by main.cpp); copied into each thread
inline u64 gameHist[1024 + MAX_PLY];
inline int gameHistLen = 0;

inline int lmrTable[64][64];
inline void initSearch() {
    for (int d = 1; d < 64; d++)
        for (int m = 1; m < 64; m++)
            lmrTable[d][m] = int(0.75 + std::log(double(d)) * std::log(double(m)) / 2.25);
    initThreadData(1);
}

inline void checkTime(SearchData& sd) {
    if (searchLimits.hardLimit >= 0 && sd.elapsed() >= searchLimits.hardLimit) Stopped = true;
    if (sd.isMain && searchLimits.nodeLimit && totalNodes() >= searchLimits.nodeLimit) Stopped = true;
}

inline bool isRepetitionOrFifty(const SearchData& sd, const Position& pos, int ply) {
    if (pos.halfmove >= 100) return true;
    int cur = sd.keyHistLen - 1 + ply;
    int limit = std::max(0, cur - pos.halfmove);
    for (int i = cur - 4; i >= limit; i -= 2)
        if (sd.keyHist[i] == pos.key) return true;
    return false;
}

inline bool insufficientMaterial(const Position& pos) {
    if (pos.byPiece[PAWN] | pos.byPiece[ROOK] | pos.byPiece[QUEEN]) return false;
    return popcount(pos.byPiece[KNIGHT] | pos.byPiece[BISHOP]) <= 1;
}

// ----------------------------- Move ordering ---------------------------------
constexpr int SCORE_TT      = 2000000000;
constexpr int SCORE_GOODCAP = 1000000000;
constexpr int SCORE_KILLER1 =  900000000;
constexpr int SCORE_KILLER2 =  899000000;
constexpr int SCORE_COUNTER =  898000000;
constexpr int SCORE_QUIET   =  500000000; // + combined history
constexpr int SCORE_BADCAP  =  100000000;

// combined quiet history: main + continuation (1 and 2 plies back)
inline int quietHistScore(const SearchData& sd, const Position& pos, Move m, int ply) {
    int from = fromSq(m), to = toSq(m);
    int h = sd.history[pos.stm][from][to];
    int cur = psIdx(pos.stm * 6 + pos.board[from], to);
    if (ply >= 1 && sd.stackPc12[ply - 1] >= 0)
        h += sd.contHist[0][psIdx(sd.stackPc12[ply - 1], sd.stackTo[ply - 1])][cur];
    if (ply >= 2 && sd.stackPc12[ply - 2] >= 0)
        h += sd.contHist[1][psIdx(sd.stackPc12[ply - 2], sd.stackTo[ply - 2])][cur];
    if (ply >= 4 && sd.stackPc12[ply - 4] >= 0)
        h += sd.contHist[2][psIdx(sd.stackPc12[ply - 4], sd.stackTo[ply - 4])][cur] / 2;
    return h;
}

inline void scoreMoves(const SearchData& sd, const Position& pos, MoveList& ml,
                       Move ttMove, int ply) {
    Move counter = MOVE_NONE;
    if (ply >= 1 && sd.stackPc12[ply - 1] >= 0)
        counter = sd.counterMove[sd.stackPc12[ply - 1]][sd.stackTo[ply - 1]];

    for (int i = 0; i < ml.count; i++) {
        Move m = ml.list[i].move;
        if (m == ttMove) { ml.list[i].score = SCORE_TT; continue; }
        if (pos.isCapture(m)) {
            int victim = (flagOf(m) == ENPASSANT) ? PAWN : pos.board[toSq(m)];
            int att12 = pos.stm * 6 + pos.board[fromSq(m)];
            int mvvlva = 100 * seeValue[victim] - seeValue[pos.board[fromSq(m)]];
            int ch = sd.capHist[att12][toSq(m)][victim];
            int base = (see(pos, m) >= 0) ? SCORE_GOODCAP : SCORE_BADCAP;
            ml.list[i].score = base + mvvlva + ch * 4 + (flagOf(m) == PROMOTION ? 50000 : 0);
        } else if (flagOf(m) == PROMOTION) {
            ml.list[i].score = (promoOf(m) == QUEEN ? SCORE_GOODCAP - 1 : SCORE_BADCAP - 1);
        } else if (m == sd.killers[ply][0]) {
            ml.list[i].score = SCORE_KILLER1;
        } else if (m == sd.killers[ply][1]) {
            ml.list[i].score = SCORE_KILLER2;
        } else if (m == counter) {
            ml.list[i].score = SCORE_COUNTER;
        } else {
            ml.list[i].score = SCORE_QUIET + quietHistScore(sd, pos, m, ply);
        }
        if (ply == 0 && policy::loaded && policy::enabled)
            ml.list[i].score += 64 * policy::score(pos, m);
    }
}

inline Move pickMove(MoveList& ml, int idx) {
    int best = idx;
    for (int i = idx + 1; i < ml.count; i++)
        if (ml.list[i].score > ml.list[best].score) best = i;
    std::swap(ml.list[idx], ml.list[best]);
    return ml.list[idx].move;
}

inline void gravity(int& h, int bonus) {
    h += bonus - h * std::abs(bonus) / 16384;
}
inline void gravity16(int16_t& h, int bonus) {
    int v = h;
    v += bonus - v * std::abs(bonus) / 16384;
    h = int16_t(std::clamp(v, -30000, 30000));
}

// update all quiet-history tables for one move
inline void updateQuietHists(SearchData& sd, const Position& pos, Move m, int bonus, int ply) {
    gravity(sd.history[pos.stm][fromSq(m)][toSq(m)], bonus);
    int cur = psIdx(pos.stm * 6 + pos.board[fromSq(m)], toSq(m));
    if (ply >= 1 && sd.stackPc12[ply - 1] >= 0)
        gravity16(sd.contHist[0][psIdx(sd.stackPc12[ply - 1], sd.stackTo[ply - 1])][cur], bonus);
    if (ply >= 2 && sd.stackPc12[ply - 2] >= 0)
        gravity16(sd.contHist[1][psIdx(sd.stackPc12[ply - 2], sd.stackTo[ply - 2])][cur], bonus);
    if (ply >= 4 && sd.stackPc12[ply - 4] >= 0)
        gravity16(sd.contHist[2][psIdx(sd.stackPc12[ply - 4], sd.stackTo[ply - 4])][cur], bonus / 2);
}

// scores to/from TT need mate-distance adjustment
inline int scoreToTT(int s, int ply)   { return s >  MATE_BOUND ? s + ply : s < -MATE_BOUND ? s - ply : s; }
inline int scoreFromTT(int s, int ply) { return s >  MATE_BOUND ? s - ply : s < -MATE_BOUND ? s + ply : s; }

// ----------------------------- Quiescence ------------------------------------
inline int qsearch(SearchData& sd, Position& pos, int alpha, int beta, int ply) {
    const u64 timeMask = Threads > 1 ? 255 : 2047;
    if ((++sd.nodes & timeMask) == 0) checkTime(sd);
    if (Stopped) return 0;
    if (ply > sd.selDepth) sd.selDepth = ply;
    if (ply >= MAX_PLY - 1) return evaluate(pos);

    // Quiescence can contain long checked capture/evasion sequences. Record
    // those positions too, so repetitions and rule draws terminate with the
    // same semantics as the full-width search.
    sd.keyHist[sd.keyHistLen - 1 + ply] = pos.key;
    if (isRepetitionOrFifty(sd, pos, ply) || insufficientMaterial(pos))
        return VALUE_DRAW;

    bool isPV = (beta - alpha) > 1;

    Move ttMove = MOVE_NONE;
    int ttScore = 0, ttEval = INF, ttDepth = -1, ttBound = BOUND_NONE;
    bool ttHit = tt.probe(pos.key, ttMove, ttScore, ttEval, ttDepth, ttBound);
    if (ttHit) {
        ttScore = scoreFromTT(ttScore, ply);
        if (!isPV
            && (ttBound == BOUND_EXACT
                || (ttBound == BOUND_LOWER && ttScore >= beta)
                || (ttBound == BOUND_UPPER && ttScore <= alpha)))
            return ttScore;
    }

    bool inCheck = pos.inCheck();
    int best, rawEval = INF;

    if (inCheck) {
        best = -MATE + ply;
    } else {
        rawEval = (ttHit && ttEval != INF) ? ttEval : evaluate(pos);
        best = rawEval;
        if (best >= beta) return best;
        if (best > alpha) alpha = best;
    }

    MoveList ml;
    if (inCheck) genMoves<false>(pos, ml);
    else         genMoves<true>(pos, ml);
    scoreMoves(sd, pos, ml, ttMove, ply);

    int alphaOrig = alpha;
    Move bestMove = MOVE_NONE;
    Undo u;
    for (int i = 0; i < ml.count; i++) {
        Move m = pickMove(ml, i);
        if (!inCheck) {
            if (pos.isCapture(m) && see(pos, m) < 0) continue;
            int victim = (flagOf(m) == ENPASSANT) ? PAWN : pos.board[toSq(m)];
            int gain = (victim == NO_PIECE ? 0 : seeValue[victim])
                     + (flagOf(m) == PROMOTION ? seeValue[QUEEN] - seeValue[PAWN] : 0);
            if (best + gain + 150 <= alpha) continue;
        }
        int pc12 = pos.stm * 6 + pos.board[fromSq(m)];
        if (!pos.make(m, u)) continue;
        sd.stackPc12[ply] = pc12;
        sd.stackTo[ply] = toSq(m);
        int score = -qsearch(sd, pos, -beta, -alpha, ply + 1);
        pos.unmake(m, u);
        if (Stopped) return 0;
        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                bestMove = m;
                if (alpha >= beta) break;
            }
        }
    }

    int bound = best >= beta ? BOUND_LOWER
              : alpha != alphaOrig ? BOUND_EXACT : BOUND_UPPER;
    tt.store(pos.key, bestMove, scoreToTT(best, ply), rawEval, 0, bound);
    return best;
}

// ----------------------------- Main search -----------------------------------
inline int search(SearchData& sd, Position& pos, int alpha, int beta, int depth,
                  int ply, bool allowNull, Move excluded = MOVE_NONE) {
    bool isPV = (beta - alpha) > 1;
    bool rootNode = (ply == 0);
    sd.pvLen[ply] = 0;

    if (depth <= 0) return qsearch(sd, pos, alpha, beta, ply);

    const u64 timeMask = Threads > 1 ? 255 : 2047;
    if ((++sd.nodes & timeMask) == 0) checkTime(sd);
    if (Stopped) return 0;
    if (ply >= MAX_PLY - 1) return evaluate(pos);

    sd.keyHist[sd.keyHistLen - 1 + ply] = pos.key;

    if (!rootNode) {
        if (isRepetitionOrFifty(sd, pos, ply) || insufficientMaterial(pos)) return VALUE_DRAW;
        alpha = std::max(alpha, -MATE + ply);
        beta  = std::min(beta, MATE - ply - 1);
        if (alpha >= beta) return alpha;
    }

    Move ttMove = MOVE_NONE;
    int ttScore = 0, ttEval = INF, ttDepth = -1, ttBound = BOUND_NONE;
    bool ttHit = tt.probe(pos.key, ttMove, ttScore, ttEval, ttDepth, ttBound);
    if (ttHit) ttScore = scoreFromTT(ttScore, ply);
    if (ttHit && !isPV && excluded == MOVE_NONE && ttDepth >= depth) {
        if (ttBound == BOUND_EXACT
            || (ttBound == BOUND_LOWER && ttScore >= beta)
            || (ttBound == BOUND_UPPER && ttScore <= alpha))
            return ttScore;
    }

    bool inCheck = pos.inCheck();
    int staticEval;
    if (inCheck) staticEval = -INF;
    else if (ttHit && ttEval != INF) staticEval = ttEval;
    else staticEval = evaluate(pos);
    sd.evalStack[ply] = staticEval;
    bool improving = !inCheck && ply >= 2 && staticEval > sd.evalStack[ply - 2];

    if (!isPV && !inCheck && excluded == MOVE_NONE) {
        if (depth <= 3 && staticEval + 200 + 150 * depth <= alpha) {
            int rs = qsearch(sd, pos, alpha, alpha + 1, ply);
            if (rs <= alpha) return rs;
        }
        if (depth <= 8 && staticEval - 80 * depth + (improving ? 60 : 0) >= beta
            && std::abs(beta) < MATE_BOUND)
            return staticEval;

        if (allowNull && depth >= 3 && staticEval >= beta && pos.hasNonPawnMaterial()) {
            int R = 3 + depth / 3 + std::min(3, (staticEval - beta) / 200);
            Undo u;
            pos.makeNull(u);
            sd.stackPc12[ply] = -1;
            sd.stackTo[ply] = -1;
            int score = -search(sd, pos, -beta, -beta + 1, depth - 1 - R, ply + 1, false);
            pos.unmakeNull(u);
            if (Stopped) return 0;
            if (score >= beta) {
                // At high depth, verify the null-move cutoff from the real
                // position. This prevents a single optimistic null search
                // from pruning the node in zugzwang-like or tactically
                // unstable positions while keeping the common shallow path
                // unchanged.
                if (depth < 12)
                    return score > MATE_BOUND ? beta : score;
                else {
                    int verify = search(sd, pos, beta - 1, beta,
                                        depth - 1 - R, ply, false);
                    if (Stopped) return 0;
                    if (verify >= beta)
                        return score > MATE_BOUND ? beta : score;
                }
            }
        }

        if (depth >= 5 && std::abs(beta) < MATE_BOUND) {
            int rBeta = std::min(beta + 180, MATE_BOUND - 1);
            MoveList pcml;
            genMoves<true>(pos, pcml);
            scoreMoves(sd, pos, pcml, ttMove, ply);
            Undo u;
            int tried = 0;
            for (int i = 0; i < pcml.count && tried < 6; i++) {
                Move m = pickMove(pcml, i);
                if (!pos.isCapture(m) || see(pos, m) < rBeta - staticEval) continue;
                int pc12 = pos.stm * 6 + pos.board[fromSq(m)];
                if (!pos.make(m, u)) continue;
                tried++;
                sd.stackPc12[ply] = pc12;
                sd.stackTo[ply] = toSq(m);
                int s = -qsearch(sd, pos, -rBeta, -rBeta + 1, ply + 1);
                if (s >= rBeta)
                    s = -search(sd, pos, -rBeta, -rBeta + 1, depth - 4, ply + 1, true);
                pos.unmake(m, u);
                if (Stopped) return 0;
                if (s >= rBeta) return s;
            }
        }
    }

    if (depth >= 4 && ttMove == MOVE_NONE) depth--;

    int singularExt = 0;
    if (!rootNode && excluded == MOVE_NONE && depth >= 8
        && ttMove != MOVE_NONE && ttHit && (ttBound & BOUND_LOWER)
        && ttDepth >= depth - 3 && std::abs(ttScore) < MATE_BOUND) {
        int sBeta = ttScore - 2 * depth;
        int sScore = search(sd, pos, sBeta - 1, sBeta, (depth - 1) / 2, ply, false, ttMove);
        if (Stopped) return 0;
        if (sScore < sBeta) singularExt = 1;
        else if (sBeta >= beta) return sBeta;
    }

    MoveList ml;
    genMoves<false>(pos, ml);
    scoreMoves(sd, pos, ml, ttMove, ply);

    int best = -INF;
    Move bestMove = MOVE_NONE;
    int legalMoves = 0, quietsTried = 0;
    Move quietList[128], capList[64];
    int quietCount = 0, capCount = 0;
    Undo u;

    for (int i = 0; i < ml.count; i++) {
        Move m = pickMove(ml, i);
        if (m == excluded) continue;
        bool isQuiet = !pos.isCapture(m) && flagOf(m) != PROMOTION;

        if (!rootNode && best > -MATE_BOUND) {
            if (isQuiet) {
                int lmpLimit = improving ? 4 + depth * depth : 2 + depth * depth / 2;
                if (depth <= 8 && quietsTried >= lmpLimit) continue;
                if (depth <= 4 && !inCheck && staticEval + 150 + 120 * depth <= alpha) continue;
                if (depth <= 4 && quietHistScore(sd, pos, m, ply) < -4000 * depth) continue;
            } else if (depth <= 8 && pos.isCapture(m) && see(pos, m) < -150 * depth) {
                continue;
            }
        }

        int histScore = isQuiet ? quietHistScore(sd, pos, m, ply) : 0;
        int pc12 = pos.stm * 6 + pos.board[fromSq(m)];
        if (!pos.make(m, u)) continue;
        legalMoves++;
        if (isQuiet) {
            quietsTried++;
            if (quietCount < 128) quietList[quietCount++] = m;
        } else if (pos.isCapture(m) || flagOf(m) == ENPASSANT) {
            if (capCount < 64) capList[capCount++] = m;
        }
        sd.stackPc12[ply] = pc12;
        sd.stackTo[ply] = toSq(m);

        bool givesCheck = pos.inCheck();
        int ext = (m == ttMove) ? singularExt : 0;
        int newDepth = depth - 1 + ext;
        int score;

        if (legalMoves == 1) {
            score = -search(sd, pos, -beta, -alpha, newDepth, ply + 1, true);
        } else {
            int r = 0;
            if (depth >= 3 && legalMoves > 2 + 2 * isPV && isQuiet && !givesCheck) {
                r = lmrTable[std::min(depth, 63)][std::min(legalMoves, 63)];
                r -= isPV;
                r -= improving;
                if (m == sd.killers[ply][0] || m == sd.killers[ply][1]) r--;
                r -= std::clamp(histScore / 8192, -2, 2);
                // In materially decisive low-piece endings, quiet king and
                // pawn moves often carry the conversion plan. Search them one
                // ply less aggressively reduced; the small branching factor
                // keeps the cost bounded while reducing horizon-driven draws.
                if (!inCheck && popcount(pos.occupied()) <= 10
                    && std::abs(staticEval) >= 400)
                    r--;
                r = std::clamp(r, 0, newDepth - 1);
            }
            score = -search(sd, pos, -alpha - 1, -alpha, newDepth - r, ply + 1, true);
            if (score > alpha && r > 0)
                score = -search(sd, pos, -alpha - 1, -alpha, newDepth, ply + 1, true);
            if (score > alpha && score < beta)
                score = -search(sd, pos, -beta, -alpha, newDepth, ply + 1, true);
        }

        pos.unmake(m, u);
        if (Stopped) return 0;

        if (score > best) {
            best = score;
            if (score > alpha) {
                alpha = score;
                bestMove = m;
                if (isPV) {
                    sd.pvTable[ply][0] = m;
                    memcpy(&sd.pvTable[ply][1], &sd.pvTable[ply + 1][0],
                           sd.pvLen[ply + 1] * sizeof(Move));
                    sd.pvLen[ply] = sd.pvLen[ply + 1] + 1;
                }
                if (alpha >= beta) {
                    int bonus = std::min(2000, 16 * depth * depth);
                    if (isQuiet) {
                        if (sd.killers[ply][0] != m) {
                            sd.killers[ply][1] = sd.killers[ply][0];
                            sd.killers[ply][0] = m;
                        }
                        if (ply >= 1 && sd.stackPc12[ply - 1] >= 0)
                            sd.counterMove[sd.stackPc12[ply - 1]][sd.stackTo[ply - 1]] = m;
                        updateQuietHists(sd, pos, m, bonus, ply);
                        for (int q = 0; q < quietCount - 1; q++)
                            updateQuietHists(sd, pos, quietList[q], -bonus, ply);
                    } else if (pos.isCapture(m)) {
                        int victim = (flagOf(m) == ENPASSANT) ? PAWN : pos.board[toSq(m)];
                        gravity16(sd.capHist[pc12][toSq(m)][victim], bonus);
                    }
                    for (int q = 0; q < capCount; q++) {
                        Move cm = capList[q];
                        if (cm == m) continue;
                        int v = (flagOf(cm) == ENPASSANT) ? PAWN : pos.board[toSq(cm)];
                        if (v == NO_PIECE) continue;
                        int a12 = pos.stm * 6 + pos.board[fromSq(cm)];
                        gravity16(sd.capHist[a12][toSq(cm)][v], -bonus);
                    }
                    break;
                }
            }
        }
    }

    if (legalMoves == 0) {
        if (excluded != MOVE_NONE) return alpha;
        return inCheck ? -MATE + ply : VALUE_DRAW;
    }

    if (excluded == MOVE_NONE) {
        int bound = best >= beta ? BOUND_LOWER
                  : bestMove != MOVE_NONE ? BOUND_EXACT
                  : BOUND_UPPER;
        tt.store(pos.key, bestMove, scoreToTT(best, ply),
                 inCheck ? INF : sd.evalStack[ply], depth, bound);
    }

    return best;
}

// ----------------------------- Iterative deepening ---------------------------
inline std::string scoreString(int score) {
    std::ostringstream os;
    if (std::abs(score) > MATE_BOUND) {
        int mateIn = (MATE - std::abs(score) + 1) / 2;
        os << "mate " << (score > 0 ? mateIn : -mateIn);
    } else {
        os << "cp " << score;
    }
    return os.str();
}

// One thread's iterative-deepening loop. Helpers (isMain == false) neither print
// nor manage time; they desync via a per-thread start-depth offset and share the
// TT, which is the whole Lazy SMP mechanism.
inline void iterativeDeepening(SearchData& sd, Position pos) {
    sd.nodes = 0;
    sd.selDepth = 0;
    sd.completedDepth = 0;
    sd.lastBest = MOVE_NONE;
    memset(sd.killers, 0, sizeof(sd.killers));
    memset(sd.pvLen, 0, sizeof(sd.pvLen));
    for (int i = 0; i < MAX_PLY + 2; i++) { sd.stackPc12[i] = -1; sd.stackTo[i] = -1; }

    Move bestMove = MOVE_NONE, prevBest = MOVE_NONE;
    int prevScore = 0, stability = 0;
    int scoreChange = 0;
    int startDepth = 1 + (sd.threadId & 1); // helpers on odd ids start one deeper

    for (int depth = startDepth; depth <= searchLimits.depthLimit; depth++) {
        sd.selDepth = 0;
        sd.rootDepth = depth;
        int alpha = -INF, beta = INF, delta = 25;
        if (depth >= 4) { alpha = prevScore - delta; beta = prevScore + delta; }

        int score;
        while (true) {
            score = search(sd, pos, alpha, beta, depth, 0, true);
            if (Stopped) break;
            if (score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-INF, score - delta);
            } else if (score >= beta) {
                beta = std::min(INF, score + delta);
            } else break;
            delta += delta / 2;
        }
        if (Stopped) break;

        scoreChange = score - prevScore;
        prevScore = score;
        sd.lastScore = score;
        sd.completedDepth = depth;
        if (sd.pvLen[0] > 0) {
            bestMove = sd.pvTable[0][0];
            // A later iteration may be interrupted after search() has reset
            // pvLen[0]. Preserve this move separately so time expiration never
            // falls back to the first pseudo-legal move.
            sd.lastBest = bestMove;
        }

        if (!sd.isMain) continue; // helpers don't print or manage the clock

        if (bestMove == prevBest) stability = std::min(stability + 1, 8);
        else stability = 0;
        prevBest = bestMove;

        s64 ms = sd.elapsed();
        if (!SilentSearch) {
            u64 nodes = totalNodes();
            u64 nps = ms > 0 ? nodes * 1000 / ms : 0;
            std::cout << "info depth " << depth
                      << " seldepth " << sd.selDepth
                      << " score " << scoreString(score)
                      << " nodes " << nodes
                      << " nps " << nps
                      << " time " << ms
                      << " pv";
            for (int i = 0; i < sd.pvLen[0]; i++) std::cout << " " << moveStr(sd.pvTable[0][i]);
            std::cout << std::endl;
        }

        if (searchLimits.softLimit >= 0) {
            // Spend longer when a new iteration reveals unexpected danger;
            // save a little time when the position improves. The hard clock
            // limit remains unchanged, preserving the game reserve.
            double scorePressure = std::clamp(-scoreChange / 100.0, -0.15, 0.50);
            double factor = 1.35 - 0.07 * stability + scorePressure;
            if (ms >= s64(searchLimits.softLimit * factor)) break;
        }
        if (std::abs(score) > MATE_BOUND && depth >= (MATE - std::abs(score)) + 10) break;
    }

    if (sd.isMain) Stopped = true; // main is done -> tell helpers to wrap up
}

// Drive a full search across the thread pool and return the best move.
// Runs the main thread in the caller; spawns Threads-1 helper threads.
inline Move searchPosition(Position& rootPos) {
    // Creating and joining helpers can dominate very short clock budgets and
    // even cause a flag under CPU contention.  Use SMP only when a move has
    // enough wall time for that startup cost to amortize.
    int n = (searchLimits.hardLimit >= 0 && searchLimits.hardLimit < 500)
          ? 1 : std::max(1, Threads);
    initThreadData(n);
    for (int i = 0; i < n; i++) {
        SearchData& td = *threadData[i];
        memcpy(td.keyHist, gameHist, gameHistLen * sizeof(u64));
        td.keyHistLen = gameHistLen;
    }

    std::vector<std::thread> helpers;
    for (int i = 1; i < n; i++)
        helpers.emplace_back([i, &rootPos] { iterativeDeepening(*threadData[i], rootPos); });

    iterativeDeepening(*threadData[0], rootPos); // main runs here; sets Stopped when done
    for (auto& h : helpers) h.join();

    // Helpers deliberately search slightly different trees.  Keep the main
    // result on ties, but do not discard a helper's deeper completed iteration.
    SearchData* winner = threadData[0].get();
    for (int i = 1; i < n; ++i)
        if (threadData[i]->completedDepth > winner->completedDepth
            && threadData[i]->lastBest != MOVE_NONE)
            winner = threadData[i].get();
    Move best = winner->lastBest;
    if (best == MOVE_NONE) {
        MoveList ml;
        genMoves<false>(rootPos, ml);
        Undo u;
        for (int i = 0; i < ml.count; i++)
            if (rootPos.make(ml.list[i].move, u)) { rootPos.unmake(ml.list[i].move, u); best = ml.list[i].move; break; }
    }
    return best;
}

// Single-threaded synchronous search (datagen): always one thread regardless of
// the Threads setting. Leaves the score in threadData[0]->lastScore.
inline Move searchSync(Position& rootPos) {
    initThreadData(1);
    SearchData& td = *threadData[0];
    memcpy(td.keyHist, gameHist, gameHistLen * sizeof(u64));
    td.keyHistLen = gameHistLen;
    iterativeDeepening(td, rootPos); // searches a copy; rootPos unchanged
    Move best = td.lastBest;
    if (best == MOVE_NONE) {
        MoveList ml;
        genMoves<false>(rootPos, ml);
        Undo u;
        for (int i = 0; i < ml.count; i++)
            if (rootPos.make(ml.list[i].move, u)) { rootPos.unmake(ml.list[i].move, u); best = ml.list[i].move; break; }
    }
    return best;
}
>>>>>>> 2d83a7878e76fedee5640e667381d4f61c9af89c
