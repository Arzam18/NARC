// NARC Genesys - NNUE evaluation.
#include "nnue.h"

#include <cstring>
#include <fstream>
#include <new>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

// The default network is embedded in the executable (NARC_NET is set by build.sh).
#ifndef NARC_NET
#define NARC_NET "networks/default.nnue"
#endif
#if defined(_WIN32)
#define NARC_RODATA ".section .rdata,\"dr\"\n"
#else
#define NARC_RODATA ".section .rodata\n"
#endif
asm(NARC_RODATA ".balign 64\n.global narc_net_data\nnarc_net_data:\n.incbin \"" NARC_NET
    "\"\n.global narc_net_end\nnarc_net_end:\n.byte 0\n.text\n");
extern "C" const unsigned char narc_net_data[];
extern "C" const unsigned char narc_net_end[];

namespace narc::nnue {

Network* network = nullptr;

namespace {

Network embedded_net, file_net;
std::string active_name = "<embedded>";

// Four king buckets over the perspective king square after mirroring files e-h onto a-d:
// rank 1 a/b and g/h corners, rank 1 centre, rank 2, ranks 3-8.
constexpr int KB4[64] = {0, 0, 1, 1, 1, 1, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2,
                         3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                         3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                         3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};

// Sixteen king buckets over the mirrored king square: every square of ranks 1 and 2, then file pairs (a-b, c-d)
// on rank 3, rank 4, ranks 5-6 and ranks 7-8.
constexpr int KB16[64] = { 0,  1,  2,  3,  3,  2,  1,  0,   4,  5,  6,  7,  7,  6,  5,  4,
                           8,  8,  9,  9,  9,  9,  8,  8,  10, 10, 11, 11, 11, 11, 10, 10,
                          12, 12, 13, 13, 13, 13, 12, 12,  12, 12, 13, 13, 13, 13, 12, 12,
                          14, 14, 15, 15, 15, 15, 14, 14,  14, 14, 15, 15, 15, 15, 14, 14};

bool parse(const unsigned char* data, size_t size, Network& net, std::string& error) {
    auto u32_at = [&](size_t off) { u32 v; std::memcpy(&v, data + off, 4); return v; };
    if (size < 36 || std::memcmp(data, "DRNNUE03", 8) != 0) {
        error = "not a DRNNUE03 network";
        return false;
    }
    u32 inputs = u32_at(8), hidden = u32_at(12), buckets = u32_at(16), kb = u32_at(20);
    u32 qa = u32_at(24), qb = u32_at(28), scale = u32_at(32);
    if ((kb != 4 && kb != 16 && kb != 64) || inputs != 768 * kb || hidden != HIDDEN || buckets != OUTPUT_BUCKETS || qa != QA ||
        qb != QB || scale != SCALE) {
        error = "unsupported network shape";
        return false;
    }
    size_t expect = 36 + 2 * size_t(inputs) * HIDDEN + 2 * HIDDEN + 2 * OUTPUT_BUCKETS * 2 * HIDDEN + 4 * OUTPUT_BUCKETS;
    if (size < expect) {
        error = "network file is truncated";
        return false;
    }
    if (net.ft) ::operator delete[](net.ft, std::align_val_t(64));
    net.king_buckets = int(kb);
    net.inputs = int(inputs);
    net.ft = static_cast<i16*>(::operator new[](sizeof(i16) * size_t(inputs) * HIDDEN, std::align_val_t(64)));
    size_t off = 36;
    std::memcpy(net.ft, data + off, 2 * size_t(inputs) * HIDDEN);
    off += 2 * size_t(inputs) * HIDDEN;
    std::memcpy(net.ft_bias, data + off, 2 * HIDDEN);
    off += 2 * HIDDEN;
    std::memcpy(net.out_w, data + off, 2 * OUTPUT_BUCKETS * 2 * HIDDEN);
    off += 2 * OUTPUT_BUCKETS * 2 * HIDDEN;
    std::memcpy(net.out_b, data + off, 4 * OUTPUT_BUCKETS);
    return true;
}

// Bucket key of a perspective: which feature block and whether files are mirrored.
struct View {
    int key;     // cache slot
    int base;    // first feature of the block
    int flip;    // square transform: xor 56 for Black, xor 7 when mirrored
};

inline View view_of(const Network& net, Color persp, int king_sq) {
    const int flip = 56 * persp;
    const int rel = king_sq ^ flip;
    if (net.king_buckets == 64) return {rel, rel * 768, flip};
    const int mirror = (rel & 7) >= 4 ? 7 : 0;
    const int bucket = (net.king_buckets == 4 ? KB4 : KB16)[rel ^ mirror];
    return {bucket * 2 + (mirror ? 1 : 0), bucket * 768, flip ^ mirror};
}

inline int feature(const View& v, Color persp, Piece p, int sq) {
    return v.base + (color_of(p) != persp ? 384 : 0) + type_of(p) * 64 + (sq ^ v.flip);
}

inline void add_row(i16* acc, const i16* row) {
    for (int i = 0; i < HIDDEN; i++) acc[i] += row[i];
}
inline void sub_row(i16* acc, const i16* row) {
    for (int i = 0; i < HIDDEN; i++) acc[i] -= row[i];
}
// dst = src + a1 - s1 (+ a2 - s2): the common quiet move and capture updates in one pass
inline void add_sub(i16* dst, const i16* src, const i16* a1, const i16* s1) {
    for (int i = 0; i < HIDDEN; i++) dst[i] = i16(src[i] + a1[i] - s1[i]);
}
inline void add_sub_sub(i16* dst, const i16* src, const i16* a1, const i16* s1, const i16* s2) {
    for (int i = 0; i < HIDDEN; i++) dst[i] = i16(src[i] + a1[i] - s1[i] - s2[i]);
}

// Sum over both perspectives of clamp(acc, 0, QA)^2 * w, exactly (per-lane i32 sums are bounded:
// each lane accumulates 32 products of at most 255 * 255 * 127, far below 2^31).
inline i64 screlu_dot(const i16* us, const i16* them, const i16* w_us, const i16* w_them) {
#if defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(QA);
    __m256i sum = _mm256_setzero_si256();
    for (int side = 0; side < 2; side++) {
        const i16* a = side ? them : us;
        const i16* w = side ? w_them : w_us;
        for (int i = 0; i < HIDDEN; i += 16) {
            __m256i x = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i));
            x = _mm256_min_epi16(_mm256_max_epi16(x, zero), qa);
            __m256i wx = _mm256_mullo_epi16(x, _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i)));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(wx, x));
        }
    }
    alignas(32) i32 lanes[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sum);
    i64 total = 0;
    for (i32 l : lanes) total += l;
    return total;
#else
    i64 total = 0;
    for (int side = 0; side < 2; side++) {
        const i16* a = side ? them : us;
        const i16* w = side ? w_them : w_us;
        for (int i = 0; i < HIDDEN; i++) {
            i32 x = a[i] < 0 ? 0 : a[i] > QA ? QA : a[i];
            total += i64(x * w[i]) * x;
        }
    }
    return total;
#endif
}

inline int output(const Network& net, const Position& pos, const i16* us, const i16* them) {
    const int pieces = popcount(pos.pieces());
    const int bucket = std::min(std::max(pieces - 2, 0) / 4, OUTPUT_BUCKETS - 1);
    i64 total = screlu_dot(us, them, net.out_w[bucket][0], net.out_w[bucket][1]);
    i64 hidden = total / QA + net.out_b[bucket];     // C++ division truncates toward zero, as the format expects
    return int(hidden * SCALE / (QA * QB));
}

inline int to_centipawns(int units) { return int(i64(units) * 1000 / 2303); }   // units are ln(10) ~ 2.303 x cp

}  // namespace

void init() { use_embedded(); }

void use_embedded() {
    std::string error;
    if (!embedded_net.ft) parse(narc_net_data, size_t(narc_net_end - narc_net_data), embedded_net, error);
    network = &embedded_net;
    active_name = "<embedded>";
}

bool load_file(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parse(data.data(), data.size(), file_net, error)) return false;
    network = &file_net;
    active_name = path;
    return true;
}

std::string network_name() {
    return active_name + " (" + std::to_string(network->king_buckets) + " king buckets)";
}

int Evaluator::units_full(const Position& pos) {
    const Network& net = *network;
    alignas(64) i16 acc[2][HIDDEN];
    for (Color persp : {WHITE, BLACK}) {
        View v = view_of(net, persp, pos.king_square(persp));
        std::memcpy(acc[persp], net.ft_bias, sizeof(acc[persp]));
        Bitboard occ = pos.pieces();
        while (occ) {
            int sq = pop_lsb(occ);
            add_row(acc[persp], net.ft + size_t(feature(v, persp, pos.piece_on(sq), sq)) * HIDDEN);
        }
    }
    Color us = pos.side_to_move();
    return output(net, pos, acc[us], acc[~us]);
}

void Evaluator::refresh(const Position& pos, Color persp, Accumulator& acc) {
    const Network& net = *network;
    View v = view_of(net, persp, pos.king_square(persp));
    CacheEntry& e = cache_[persp][v.key];
    for (Color c : {WHITE, BLACK})
        for (int t = PAWN; t <= KING; t++) {
            Bitboard now = pos.pieces(c, PieceType(t));
            Bitboard was = e.by_color[c] & e.by_type[t];
            Piece p = make_piece(c, PieceType(t));
            for (Bitboard add = now & ~was; add;) {
                int sq = pop_lsb(add);
                add_row(e.v, net.ft + size_t(feature(v, persp, p, sq)) * HIDDEN);
            }
            for (Bitboard sub = was & ~now; sub;) {
                int sq = pop_lsb(sub);
                sub_row(e.v, net.ft + size_t(feature(v, persp, p, sq)) * HIDDEN);
            }
        }
    for (Color c : {WHITE, BLACK}) e.by_color[c] = pos.pieces(c);
    for (int t = PAWN; t <= KING; t++) e.by_type[t] = pos.pieces(PieceType(t));
    std::memcpy(acc.v[persp], e.v, sizeof(e.v));
    acc.computed[persp] = true;
}

void Evaluator::reset(const Position& pos) {
    if (cache_net_ != network) {   // new network: empty caches holding only the bias
        for (auto& side : cache_)
            for (auto& e : side) {
                std::memcpy(e.v, network->ft_bias, sizeof(e.v));
                std::memset(e.by_color, 0, sizeof(e.by_color));
                std::memset(e.by_type, 0, sizeof(e.by_type));
            }
        cache_net_ = network;
    }
    top_ = 0;
    root_ply_ = pos.game_ply();
    refresh(pos, WHITE, stack_[0]);
    refresh(pos, BLACK, stack_[0]);
}

void Evaluator::ensure(const Position& pos, Color persp) {
    if (stack_[top_].computed[persp]) return;
    const Network& net = *network;
    const Piece king = make_piece(persp, KING);
    // Walk back to the nearest computed accumulator; if this perspective's king changed bucket key on the way,
    // rebuild from the cache instead.
    int j = top_;
    int king_sq = pos.king_square(persp);
    const int key_now = view_of(net, persp, king_sq).key;
    while (!stack_[j].computed[persp]) {
        const DirtyPieces& d = pos.state_at(root_ply_ + j).dirty;
        // every king move on the path must stay inside the current bucket key, or intermediate accumulators
        // would be built with the wrong feature block
        if (d.count && d.piece[0] == king &&
            (view_of(net, persp, d.from[0]).key != key_now || view_of(net, persp, d.to[0]).key != key_now)) {
            refresh(pos, persp, stack_[top_]);
            return;
        }
        j--;
    }
    const View v = view_of(net, persp, king_sq);
    const i16* ft = net.ft;
    for (int i = j + 1; i <= top_; i++) {
        const DirtyPieces& d = pos.state_at(root_ply_ + i).dirty;
        const i16* src = stack_[i - 1].v[persp];
        i16* dst = stack_[i].v[persp];
        int adds[3], subs[3], na = 0, ns = 0;
        for (int k = 0; k < d.count; k++) {
            if (d.from[k] != NO_SQUARE) subs[ns++] = feature(v, persp, d.piece[k], d.from[k]);
            if (d.to[k] != NO_SQUARE) adds[na++] = feature(v, persp, d.piece[k], d.to[k]);
        }
        auto row = [ft](int f) { return ft + size_t(f) * HIDDEN; };
        if (na == 1 && ns == 1) {
            add_sub(dst, src, row(adds[0]), row(subs[0]));
        } else if (na == 1 && ns == 2) {
            add_sub_sub(dst, src, row(adds[0]), row(subs[0]), row(subs[1]));
        } else {
            std::memcpy(dst, src, sizeof(i16) * HIDDEN);
            for (int k = 0; k < na; k++) add_row(dst, row(adds[k]));
            for (int k = 0; k < ns; k++) sub_row(dst, row(subs[k]));
        }
        stack_[i].computed[persp] = true;
    }
}

int Evaluator::units(const Position& pos) {
    ensure(pos, WHITE);
    ensure(pos, BLACK);
    Color us = pos.side_to_move();
    return output(*network, pos, stack_[top_].v[us], stack_[top_].v[~us]);
}

int Evaluator::evaluate(const Position& pos) { return to_centipawns(units(pos)); }

}  // namespace narc::nnue
