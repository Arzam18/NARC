// NARC Genesys - NNUE evaluation.
#include "nnue.h"

#include <cstring>
#include <fstream>
#include <new>
#include <type_traits>
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

constexpr bool supported_width(u32 h) { return h == 256 || h == 512 || h == 768 || h == 1024; }

// Calls f with the network width as a compile-time constant (std::integral_constant).
template <class F>
decltype(auto) by_width(int hidden, F&& f) {
    switch (hidden) {
    case 512: return f(std::integral_constant<int, 512>{});
    case 768: return f(std::integral_constant<int, 768>{});
    case 1024: return f(std::integral_constant<int, 1024>{});
    default: return f(std::integral_constant<int, 256>{});
    }
}

bool parse(const unsigned char* data, size_t size, Network& net, std::string& error) {
    auto u32_at = [&](size_t off) { u32 v; std::memcpy(&v, data + off, 4); return v; };
    if (size < 36 || std::memcmp(data, "DRNNUE03", 8) != 0) {
        error = "not a DRNNUE03 network";
        return false;
    }
    u32 inputs = u32_at(8), hidden = u32_at(12), buckets = u32_at(16), kb = u32_at(20);
    u32 qa = u32_at(24), qb = u32_at(28), scale = u32_at(32);
    if ((kb != 4 && kb != 16 && kb != 64) || inputs != 768 * kb || !supported_width(hidden) ||
        buckets != OUTPUT_BUCKETS || qa != QA || qb != QB || scale != SCALE) {
        error = "unsupported network shape";
        return false;
    }
    const size_t h = hidden;
    size_t expect = 36 + 2 * size_t(inputs) * h + 2 * h + 2 * OUTPUT_BUCKETS * 2 * h + 4 * OUTPUT_BUCKETS;
    if (size < expect) {
        error = "network file is truncated";
        return false;
    }
    if (net.ft) ::operator delete[](net.ft, std::align_val_t(64));
    net.king_buckets = int(kb);
    net.inputs = int(inputs);
    net.hidden = int(hidden);
    net.ft = static_cast<i16*>(::operator new[](sizeof(i16) * size_t(inputs) * h, std::align_val_t(64)));
    size_t off = 36;
    std::memcpy(net.ft, data + off, 2 * size_t(inputs) * h);
    off += 2 * size_t(inputs) * h;
    std::memset(net.ft_bias, 0, sizeof(net.ft_bias));
    std::memcpy(net.ft_bias, data + off, 2 * h);
    off += 2 * h;
    std::memset(net.out_w, 0, sizeof(net.out_w));
    for (int b = 0; b < OUTPUT_BUCKETS; b++)
        for (int side = 0; side < 2; side++, off += 2 * h) std::memcpy(net.out_w[b][side], data + off, 2 * h);
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

template <int H>
inline void add_row(i16* acc, const i16* row) {
    for (int i = 0; i < H; i++) acc[i] += row[i];
}

// dst = src + sum(adds) - sum(subs) in one pass (and the same into dst2 when given). With AVX2 the vector is
// processed in tiles of 256 values held in the 16 ymm registers, so src is read and dst written once whatever the
// number of rows. i16 arithmetic wraps the same way in both paths, so the results are identical.
template <int H>
inline void update(i16* dst, i16* dst2, const i16* src, const i16* const* adds, int na, const i16* const* subs,
                   int ns) {
#if defined(__AVX2__)
    constexpr int REGS = 16, TILE = REGS * 16;
    static_assert(H % TILE == 0, "the width must be a multiple of the register tile");
    for (int t = 0; t < H; t += TILE) {
        __m256i r[REGS];
        for (int k = 0; k < REGS; k++) r[k] = _mm256_load_si256(reinterpret_cast<const __m256i*>(src + t) + k);
        for (int a = 0; a < na; a++) {
            const __m256i* row = reinterpret_cast<const __m256i*>(adds[a] + t);
            for (int k = 0; k < REGS; k++) r[k] = _mm256_add_epi16(r[k], _mm256_load_si256(row + k));
        }
        for (int s = 0; s < ns; s++) {
            const __m256i* row = reinterpret_cast<const __m256i*>(subs[s] + t);
            for (int k = 0; k < REGS; k++) r[k] = _mm256_sub_epi16(r[k], _mm256_load_si256(row + k));
        }
        for (int k = 0; k < REGS; k++) _mm256_store_si256(reinterpret_cast<__m256i*>(dst + t) + k, r[k]);
        if (dst2)
            for (int k = 0; k < REGS; k++) _mm256_store_si256(reinterpret_cast<__m256i*>(dst2 + t) + k, r[k]);
    }
#else
    if (dst != src) std::memcpy(dst, src, sizeof(i16) * H);
    for (int a = 0; a < na; a++)
        for (int i = 0; i < H; i++) dst[i] += adds[a][i];
    for (int s = 0; s < ns; s++)
        for (int i = 0; i < H; i++) dst[i] -= subs[s][i];
    if (dst2) std::memcpy(dst2, dst, sizeof(i16) * H);
#endif
}

// Sum over both perspectives of clamp(acc, 0, QA)^2 * w, exactly. Per-lane i32 sums are bounded: within one
// perspective each lane adds H / 8 products of at most 255 * 255 * 127 (128 products, 1.06e9, at H = 1024), below
// 2^31; each perspective's lanes are folded into the i64 total separately.
template <int H>
inline i64 screlu_dot(const i16* us, const i16* them, const i16* w_us, const i16* w_them) {
    static_assert(H % 16 == 0 && H <= 1024, "the i32 lane sums are proven only up to 1024");
    i64 total = 0;
#if defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256(), qa = _mm256_set1_epi16(QA);
    for (int side = 0; side < 2; side++) {
        const i16* a = side ? them : us;
        const i16* w = side ? w_them : w_us;
        __m256i sum = _mm256_setzero_si256(), sum2 = _mm256_setzero_si256();   // two chains, 32 values a step
        for (int i = 0; i < H; i += 32) {
            __m256i x = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i));
            __m256i y = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i + 16));
            x = _mm256_min_epi16(_mm256_max_epi16(x, zero), qa);
            y = _mm256_min_epi16(_mm256_max_epi16(y, zero), qa);
            __m256i wx = _mm256_mullo_epi16(x, _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i)));
            __m256i wy = _mm256_mullo_epi16(y, _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i + 16)));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(wx, x));
            sum2 = _mm256_add_epi32(sum2, _mm256_madd_epi16(wy, y));
        }
        sum = _mm256_add_epi32(sum, sum2);
        alignas(32) i32 lanes[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sum);
        for (i32 l : lanes) total += l;
    }
#else
    for (int side = 0; side < 2; side++) {
        const i16* a = side ? them : us;
        const i16* w = side ? w_them : w_us;
        for (int i = 0; i < H; i++) {
            i32 x = a[i] < 0 ? 0 : a[i] > QA ? QA : a[i];
            total += i64(x * w[i]) * x;
        }
    }
#endif
    return total;
}

template <int H>
inline int output(const Network& net, const Position& pos, const i16* us, const i16* them) {
    const int pieces = popcount(pos.pieces());
    const int bucket = std::min(std::max(pieces - 2, 0) / 4, OUTPUT_BUCKETS - 1);
    i64 total = screlu_dot<H>(us, them, net.out_w[bucket][0], net.out_w[bucket][1]);
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
    return active_name + " (" + std::to_string(network->king_buckets) + " king buckets, " +
           std::to_string(network->hidden) + " hidden)";
}

int Evaluator::units_full(const Position& pos) {
    const Network& net = *network;
    return by_width(net.hidden, [&](auto width) {
        constexpr int H = decltype(width)::value;
        alignas(64) i16 acc[2][H];
        for (Color persp : {WHITE, BLACK}) {
            View v = view_of(net, persp, pos.king_square(persp));
            std::memcpy(acc[persp], net.ft_bias, sizeof(acc[persp]));
            Bitboard occ = pos.pieces();
            while (occ) {
                int sq = pop_lsb(occ);
                add_row<H>(acc[persp], net.ft + size_t(feature(v, persp, pos.piece_on(sq), sq)) * H);
            }
        }
        Color us = pos.side_to_move();
        return output<H>(net, pos, acc[us], acc[~us]);
    });
}

Evaluator::~Evaluator() {
    if (acc_) ::operator delete[](acc_, std::align_val_t(64));
    if (cache_) ::operator delete[](cache_, std::align_val_t(64));
}

void Evaluator::allocate(int hidden) {
    if (hidden == hidden_) return;
    if (acc_) ::operator delete[](acc_, std::align_val_t(64));
    if (cache_) ::operator delete[](cache_, std::align_val_t(64));
    hidden_ = hidden;
    acc_ = static_cast<i16*>(::operator new[](sizeof(i16) * STACK * 2 * hidden, std::align_val_t(64)));
    cache_ = static_cast<i16*>(::operator new[](sizeof(i16) * 2 * CACHE_KEYS * hidden, std::align_val_t(64)));
}

template <int H>
void Evaluator::refresh(const Position& pos, Color persp, int ply) {
    const Network& net = *network;
    View v = view_of(net, persp, pos.king_square(persp));
    i16* e = cache_v(persp, v.key);
    CacheBoards& boards = cache_boards_[persp][v.key];
    const i16* adds[64];
    const i16* subs[64];
    int na = 0, ns = 0;
    for (Color c : {WHITE, BLACK})
        for (int t = PAWN; t <= KING; t++) {
            Bitboard now = pos.pieces(c, PieceType(t));
            Bitboard was = boards.by_color[c] & boards.by_type[t];
            Piece p = make_piece(c, PieceType(t));
            for (Bitboard add = now & ~was; add;) adds[na++] = net.ft + size_t(feature(v, persp, p, pop_lsb(add))) * H;
            for (Bitboard sub = was & ~now; sub;) subs[ns++] = net.ft + size_t(feature(v, persp, p, pop_lsb(sub))) * H;
        }
    for (Color c : {WHITE, BLACK}) boards.by_color[c] = pos.pieces(c);
    for (int t = PAWN; t <= KING; t++) boards.by_type[t] = pos.pieces(PieceType(t));
    update<H>(e, acc(ply, persp), e, adds, na, subs, ns);   // the cache entry and the accumulator in one pass
    computed_[ply][persp] = true;
}

void Evaluator::reset(const Position& pos) {
    if (cache_net_ != network) {   // new network: storage for its width, caches holding only the bias
        allocate(network->hidden);
        for (Color persp : {WHITE, BLACK})
            for (int key = 0; key < CACHE_KEYS; key++) {
                std::memcpy(cache_v(persp, key), network->ft_bias, sizeof(i16) * hidden_);
                cache_boards_[persp][key] = {};
            }
        cache_net_ = network;
    }
    top_ = 0;
    root_ply_ = pos.game_ply();
    by_width(hidden_, [&](auto width) {
        constexpr int H = decltype(width)::value;
        refresh<H>(pos, WHITE, 0);
        refresh<H>(pos, BLACK, 0);
    });
}

template <int H>
void Evaluator::ensure(const Position& pos, Color persp) {
    if (computed_[top_][persp]) return;
    const Network& net = *network;
    const Piece king = make_piece(persp, KING);
    // Walk back to the nearest computed accumulator; if this perspective's king changed bucket key on the way,
    // rebuild from the cache instead.
    int j = top_;
    int king_sq = pos.king_square(persp);
    const int key_now = view_of(net, persp, king_sq).key;
    while (!computed_[j][persp]) {
        const DirtyPieces& d = pos.state_at(root_ply_ + j).dirty;
        // every king move on the path must stay inside the current bucket key, or intermediate accumulators
        // would be built with the wrong feature block
        if (d.count && d.piece[0] == king &&
            (view_of(net, persp, d.from[0]).key != key_now || view_of(net, persp, d.to[0]).key != key_now)) {
            refresh<H>(pos, persp, top_);
            return;
        }
        j--;
    }
    const View v = view_of(net, persp, king_sq);
    const i16* ft = net.ft;
    for (int i = j + 1; i <= top_; i++) {
        const DirtyPieces& d = pos.state_at(root_ply_ + i).dirty;
        const i16* adds[3];
        const i16* subs[3];
        int na = 0, ns = 0;
        for (int k = 0; k < d.count; k++) {
            if (d.from[k] != NO_SQUARE) subs[ns++] = ft + size_t(feature(v, persp, d.piece[k], d.from[k])) * H;
            if (d.to[k] != NO_SQUARE) adds[na++] = ft + size_t(feature(v, persp, d.piece[k], d.to[k])) * H;
        }
        update<H>(acc(i, persp), nullptr, acc(i - 1, persp), adds, na, subs, ns);
        computed_[i][persp] = true;
    }
}

template <int H>
int Evaluator::units_t(const Position& pos) {
    ensure<H>(pos, WHITE);
    ensure<H>(pos, BLACK);
    Color us = pos.side_to_move();
    return output<H>(*network, pos, acc(top_, us), acc(top_, ~us));
}

int Evaluator::units(const Position& pos) {
    return by_width(hidden_, [&](auto width) { return units_t<decltype(width)::value>(pos); });
}

int Evaluator::evaluate(const Position& pos) { return to_centipawns(units(pos)); }

}  // namespace narc::nnue
