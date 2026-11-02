// NARC Genesys - transposition table.
#include "tt.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace narc {

TranspositionTable TT;

TranspositionTable::~TranspositionTable() {
    if (buckets_) ::operator delete[](buckets_, std::align_val_t(64));
}

void TranspositionTable::resize(size_t mb) {
    if (buckets_) ::operator delete[](buckets_, std::align_val_t(64));
    count_ = std::max<size_t>(1, mb * 1024 * 1024 / sizeof(TTBucket));
    buckets_ = static_cast<TTBucket*>(::operator new[](count_ * sizeof(TTBucket), std::align_val_t(64)));
    clear();
}

void TranspositionTable::clear() {
    // zero in parallel for large tables
    const size_t threads = std::max<size_t>(1, std::min<size_t>(8, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t t = 0; t < threads; t++)
        pool.emplace_back([this, t, threads] {
            size_t begin = count_ * t / threads, end = count_ * (t + 1) / threads;
            std::memset(static_cast<void*>(buckets_ + begin), 0, (end - begin) * sizeof(TTBucket));
        });
    for (auto& th : pool) th.join();
    generation_ = 0;
}

TTProbe TranspositionTable::probe(u64 key, int ply) const {
    TTProbe r;
    const u16 k16 = u16(key);
    const TTBucket* b = bucket(key);
    for (const TTEntry& e : b->entries)
        if (e.key16 == k16 && e.depth8) {
            r.hit = true;
            r.move = e.move;
            r.score = score_from_tt(e.score, ply);
            r.eval = e.eval;
            r.depth = e.depth();
            r.bound = e.bound();
            r.pv = e.gen_bound & 4;
            break;
        }
    return r;
}

void TranspositionTable::store(u64 key, int ply, Move move, int score, int eval, int depth, Bound bound, bool pv,
                               bool pv_flag) {
    const u16 k16 = u16(key);
    TTBucket* b = bucket(key);
    TTEntry* replace = &b->entries[0];
    for (TTEntry& e : b->entries) {
        if (e.key16 == k16 || !e.depth8) {
            replace = &e;
            break;
        }
        // prefer to overwrite old, shallow entries. The original age term subtracts the whole gen_bound byte, so
        // its low (bound) bits borrow: an entry from this search reads as the oldest possible and one from the
        // previous search as new. age_fix adds the low bits back first (8 + 7 = 263 - 256), giving the true age.
        auto worth = [this](const TTEntry& x) {
            const int age = age_fix ? ((263 + generation_ - x.gen_bound) & 0xF8) / 8
                                    : ((generation_ - x.gen_bound) & 0xF8) / 8;
            return x.depth8 - 2 * age;
        };
        if (worth(e) < worth(*replace)) replace = &e;
    }
    // keep the old move if the new result has none for the same position
    if (move != NO_MOVE || replace->key16 != k16) replace->move = move;
    // don't let a shallow non-exact result overwrite a deeper one for the same position
    if (replace->key16 == k16 && bound != BOUND_EXACT && depth + 3 + 2 * pv < replace->depth() &&
        ((replace->gen_bound & 0xF8) == generation_))
        return;
    replace->key16 = k16;
    replace->score = i16(score_to_tt(score, ply));
    replace->eval = i16(eval);
    replace->depth8 = u8(std::clamp(depth + TTEntry::DEPTH_OFFSET, 1, 255));
    replace->gen_bound = u8(generation_ | (pv_flag ? 4 : 0) | bound);
}

int TranspositionTable::hashfull() const {
    int used = 0;
    for (size_t i = 0; i < std::min<size_t>(1000, count_); i++)
        for (const TTEntry& e : buckets_[i].entries)
            used += e.depth8 && (e.gen_bound & 0xF8) == generation_;
    return used / 3;
}

}  // namespace narc
