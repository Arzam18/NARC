// NARC Genesys - NNUE evaluation.
//
// Network format (little-endian): "DRNNUE03"(Which I created for a private engine), u32 inputs, hidden, output buckets, king buckets, QA, QB, scale;
// then i16 feature weights [inputs][hidden], i16 feature bias [hidden], i16 output weights [buckets][2][hidden]
// (perspective 0 = side to move), i32 output bias [buckets]. Architecture: (768 x king buckets) -> 256, both
// perspectives -> 8 output buckets by piece count, SCReLU. Supports 4 and 16 king buckets (files e-h mirrored) and 64.
//
// Accumulators are updated lazily: each search ply only records what changed (StateInfo::dirty); an accumulator
// is brought up to date when an evaluation needs it, from the nearest computed ancestor, or rebuilt from a
// per-king-bucket cache when that perspective's king changed bucket.
#pragma once

#include <string>

#include "position.h"

namespace narc::nnue {

constexpr int HIDDEN = 256;
constexpr int OUTPUT_BUCKETS = 8;
constexpr int QA = 255, QB = 64, SCALE = 400;

struct Network {
    int king_buckets = 0;
    int inputs = 0;
    i16* ft = nullptr;   // [inputs][HIDDEN], 64-byte aligned
    alignas(64) i16 ft_bias[HIDDEN];
    alignas(64) i16 out_w[OUTPUT_BUCKETS][2][HIDDEN];
    i32 out_b[OUTPUT_BUCKETS];
};

extern Network* network;   // the active network (never null after init)

void init();                                  // loads the embedded network
bool load_file(const std::string& path, std::string& error);
void use_embedded();
std::string network_name();

struct Accumulator {
    alignas(64) i16 v[2][HIDDEN];
    bool computed[2];
};

// Per thread: the accumulator stack for one search and the king-bucket refresh cache.
class Evaluator {
public:
    void reset(const Position& pos);          // at the search root (computes both perspectives)
    void push() { top_++; stack_[top_].computed[WHITE] = stack_[top_].computed[BLACK] = false; }
    void pop() { top_--; }
    int units(const Position& pos);           // network output, side-to-move view
    int evaluate(const Position& pos);        // centipawns, side-to-move view

    static int units_full(const Position& pos);   // from scratch, no incremental state (for testing)

private:
    struct CacheEntry {
        alignas(64) i16 v[HIDDEN];
        Bitboard by_color[2];
        Bitboard by_type[6];
    };
    void ensure(const Position& pos, Color persp);
    void refresh(const Position& pos, Color persp, Accumulator& acc);

    Accumulator stack_[MAX_PLY + 16];
    int top_ = 0;
    int root_ply_ = 0;
    CacheEntry cache_[2][128];   // [perspective][bucket key]
    const Network* cache_net_ = nullptr;
};

}  // namespace narc::nnue
