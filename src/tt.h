// NARC Genesys - transposition table shared by all search threads.
//
// 32-byte buckets of three 10-byte entries. An entry keeps 16 key bits (the bucket index supplies the rest),
// move, score, static eval, depth and generation/bound. Threads read and write without locks; a torn entry can
// only produce a wrong move or score for one node, and moves from the table are always legality-checked.
#pragma once

#include <cstddef>

#include "types.h"

namespace narc {

enum Bound : u8 { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

struct TTEntry {
    u16 key16;
    u16 move;
    i16 score;
    i16 eval;
    u8 depth8;      // depth + DEPTH_OFFSET (0 = empty)
    u8 gen_bound;   // generation (upper 5 bits) | was-PV (bit 2) | bound (2 bits)

    int depth() const { return int(depth8) - DEPTH_OFFSET; }
    Bound bound() const { return Bound(gen_bound & 3); }
    static constexpr int DEPTH_OFFSET = 8;
};

struct TTBucket {
    TTEntry entries[3];
    char pad[2];
};
static_assert(sizeof(TTEntry) == 10 && sizeof(TTBucket) == 32);

struct TTProbe {
    bool hit = false;
    Move move = NO_MOVE;
    int score = VALUE_NONE;   // already converted from "mate from here" to "mate from the root"
    int eval = VALUE_NONE;
    int depth = -99;
    Bound bound = BOUND_NONE;
    bool pv = false;          // the entry was stored from a PV node
};

class TranspositionTable {
public:
    ~TranspositionTable();
    void resize(size_t mb);
    void clear();
    void new_search() { generation_ = u8(generation_ + 8); }   // the low 3 bits hold PV and bound
    bool age_fix = true;    // TTAgeFix: correct entry ages in the replacement decision (+12 +- 20 in 400 games)
    TTProbe probe(u64 key, int ply) const;
    void store(u64 key, int ply, Move move, int score, int eval, int depth, Bound bound, bool pv, bool pv_flag = false);
    void prefetch(u64 key) const { __builtin_prefetch(bucket(key)); }
    int hashfull() const;   // permille of recent entries in the first 1000 buckets

private:
    TTBucket* bucket(u64 key) const { return &buckets_[(unsigned __int128)key * count_ >> 64]; }
    TTBucket* buckets_ = nullptr;
    size_t count_ = 0;
    u8 generation_ = 0;
};

extern TranspositionTable TT;

// Mate scores are stored relative to the node, used relative to the root.
inline int score_to_tt(int s, int ply) {
    return s >= VALUE_MATE_IN_MAX ? s + ply : s <= -VALUE_MATE_IN_MAX ? s - ply : s;
}
inline int score_from_tt(int s, int ply) {
    if (s == VALUE_NONE) return s;
    return s >= VALUE_MATE_IN_MAX ? s - ply : s <= -VALUE_MATE_IN_MAX ? s + ply : s;
}

}  // namespace narc
