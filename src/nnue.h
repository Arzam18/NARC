// NARC Genesys - NNUE evaluation.
//
// Network format (little-endian): "DRNNUE03"(Which I created for a private engine), u32 inputs, hidden, output buckets, king buckets, QA, QB, scale;
// then i16 feature weights [inputs][hidden], i16 feature bias [hidden], i16 output weights [buckets][2][hidden]
// (perspective 0 = side to move), i32 output bias [buckets]. Architecture: (768 x king buckets) -> hidden, both
// perspectives -> 8 output buckets by piece count, SCReLU. Supports 4 and 16 king buckets (files e-h mirrored) and 64,
// and hidden widths 256, 512, 768 and 1024 (the kernels are compiled once per width).
//
// Accumulators are updated lazily: each search ply only records what changed (StateInfo::dirty); an accumulator
// is brought up to date when an evaluation needs it, from the nearest computed ancestor, or rebuilt from a
// per-king-bucket cache when that perspective's king changed bucket.
#pragma once

#include <string>

#include "position.h"

namespace narc::nnue {

constexpr int MAX_HIDDEN = 1024;
constexpr int OUTPUT_BUCKETS = 8;
constexpr int QA = 255, QB = 64, SCALE = 400;

struct Network {
    int king_buckets = 0;
    int inputs = 0;
    int hidden = 0;
    i16* ft = nullptr;   // [inputs][hidden], 64-byte aligned
    alignas(64) i16 ft_bias[MAX_HIDDEN];
    alignas(64) i16 out_w[OUTPUT_BUCKETS][2][MAX_HIDDEN];   // the first `hidden` entries of each row are used
    i32 out_b[OUTPUT_BUCKETS];
};

extern Network* network;   // the active network (never null after init)

void init();                                  // loads the embedded network
bool load_file(const std::string& path, std::string& error);
void use_embedded();
std::string network_name();

// Per thread: the accumulator stack for one search and the king-bucket refresh cache. Their storage is sized for
// the active network's width when reset() sees a new network.
class Evaluator {
public:
    Evaluator() = default;
    ~Evaluator();
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    void reset(const Position& pos);          // at the search root (computes both perspectives)
    void push() { top_++; computed_[top_][WHITE] = computed_[top_][BLACK] = false; }
    void pop() { top_--; }
    int units(const Position& pos);           // network output, side-to-move view
    int evaluate(const Position& pos);        // centipawns, side-to-move view

    static int units_full(const Position& pos);   // from scratch, no incremental state (for testing)

private:
    struct CacheBoards {
        Bitboard by_color[2];
        Bitboard by_type[6];
    };
    static constexpr int STACK = MAX_PLY + 16;
    static constexpr int CACHE_KEYS = 128;

    template <int H> void ensure(const Position& pos, Color persp);
    template <int H> void refresh(const Position& pos, Color persp, int ply);
    template <int H> int units_t(const Position& pos);
    void allocate(int hidden);

    i16* acc(int ply, Color persp) { return acc_ + (size_t(ply) * 2 + persp) * hidden_; }
    i16* cache_v(Color persp, int key) { return cache_ + (size_t(persp) * CACHE_KEYS + key) * hidden_; }

    i16* acc_ = nullptr;     // [STACK][2][hidden_]
    i16* cache_ = nullptr;   // [2][CACHE_KEYS][hidden_]: per perspective and bucket key
    int hidden_ = 0;
    bool computed_[STACK][2] = {};
    CacheBoards cache_boards_[2][CACHE_KEYS] = {};
    int top_ = 0;
    int root_ply_ = 0;
    const Network* cache_net_ = nullptr;
};

}  // namespace narc::nnue
