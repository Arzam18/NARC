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
