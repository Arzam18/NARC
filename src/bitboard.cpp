// NARC Genesys - attack tables. Slider attacks use magic multiplication, or PEXT on BMI2 builds;
// magics are found at startup with a seeded random search (a few milliseconds).
#include "bitboard.h"

#include <vector>

namespace narc {

Bitboard PAWN_ATTACKS[2][64];
Bitboard KNIGHT_ATTACKS[64];
Bitboard KING_ATTACKS[64];
Bitboard BETWEEN[64][64];
Bitboard LINE[64][64];
SliderTable ROOK_TABLE[64];
SliderTable BISHOP_TABLE[64];

namespace {

Bitboard rook_storage[0x19000];   // 102,400 entries: sum of 2^bits over all squares
Bitboard bishop_storage[0x1480];  // 5,248 entries

constexpr int ROOK_DIRS[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int BISHOP_DIRS[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

// Attacks found by walking rays; used only to build the tables.
Bitboard ray_attacks(int s, Bitboard occ, const int (*dirs)[2]) {
    Bitboard result = 0;
    for (int d = 0; d < 4; d++) {
        int f = file_of(s) + dirs[d][0], r = rank_of(s) + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            int t = r * 8 + f;
            result |= bb(t);
            if (occ & bb(t)) break;
            f += dirs[d][0];
            r += dirs[d][1];
        }
    }
    return result;
}

// Relevant occupancy: the rays without their final square (edge squares never block anything beyond).
Bitboard relevant_mask(int s, const int (*dirs)[2]) {
    Bitboard result = 0;
    for (int d = 0; d < 4; d++) {
        int f = file_of(s) + dirs[d][0], r = rank_of(s) + dirs[d][1];
        while (f + dirs[d][0] >= 0 && f + dirs[d][0] < 8 && r + dirs[d][1] >= 0 && r + dirs[d][1] < 8 &&
               f >= 0 && f < 8 && r >= 0 && r < 8) {
            result |= bb(r * 8 + f);
            f += dirs[d][0];
            r += dirs[d][1];
        }
    }
    return result;
}

struct Rng {
    u64 s;
    u64 next() {   // xorshift64*
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    u64 sparse() { return next() & next() & next(); }
};

void build_slider(SliderTable* table, Bitboard* storage, const int (*dirs)[2], u64 seed) {
    Rng rng{seed};
    Bitboard* next_free = storage;
    std::vector<Bitboard> occupancy, reference;
    std::vector<int> stamp;
    for (int s = 0; s < 64; s++) {
        SliderTable& t = table[s];
        t.mask = relevant_mask(s, dirs);
        int bits = popcount(t.mask);
        t.shift = 64 - bits;
        t.attacks = next_free;
        size_t size = size_t(1) << bits;
        next_free += size;

        // every subset of the mask (carry-rippler enumeration) and its attack set
        occupancy.clear();
        reference.clear();
        Bitboard sub = 0;
        do {
            occupancy.push_back(sub);
            reference.push_back(ray_attacks(s, sub, dirs));
            sub = (sub - t.mask) & t.mask;
        } while (sub);

#if defined(USE_PEXT)
        t.magic = 0;
        for (size_t i = 0; i < occupancy.size(); i++) t.attacks[t.index(occupancy[i])] = reference[i];
#else
        stamp.assign(size, 0);
        for (int attempt = 1;; attempt++) {
            t.magic = rng.sparse();
            if (popcount((t.mask * t.magic) >> 56) < 6) continue;
            bool ok = true;
            for (size_t i = 0; i < occupancy.size() && ok; i++) {
                unsigned idx = t.index(occupancy[i]);
                if (stamp[idx] != attempt) {
                    stamp[idx] = attempt;
                    t.attacks[idx] = reference[i];
                } else if (t.attacks[idx] != reference[i]) {
                    ok = false;
                }
            }
            if (ok) break;
        }
#endif
    }
}

}  // namespace

void init_bitboards() {
    for (int s = 0; s < 64; s++) {
        Bitboard b = bb(s);
        PAWN_ATTACKS[WHITE][s] = pawn_attacks_bb(WHITE, b);
        PAWN_ATTACKS[BLACK][s] = pawn_attacks_bb(BLACK, b);
        KNIGHT_ATTACKS[s] = 0;
        KING_ATTACKS[s] = 0;
        for (int df = -2; df <= 2; df++)
            for (int dr = -2; dr <= 2; dr++) {
                int f = file_of(s) + df, r = rank_of(s) + dr;
                if (f < 0 || f > 7 || r < 0 || r > 7) continue;
                if (df * df + dr * dr == 5) KNIGHT_ATTACKS[s] |= bb(r * 8 + f);
                if ((df || dr) && df >= -1 && df <= 1 && dr >= -1 && dr <= 1) KING_ATTACKS[s] |= bb(r * 8 + f);
            }
    }
    build_slider(ROOK_TABLE, rook_storage, ROOK_DIRS, 0x6D616B617261ULL);     // fixed magic-search seed
    build_slider(BISHOP_TABLE, bishop_storage, BISHOP_DIRS, 0x4D414B415241ULL);

    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++) {
            BETWEEN[a][b] = LINE[a][b] = 0;
            if (a == b) continue;
            if (ray_attacks(a, 0, ROOK_DIRS) & bb(b)) {
                BETWEEN[a][b] = ray_attacks(a, bb(b), ROOK_DIRS) & ray_attacks(b, bb(a), ROOK_DIRS);
                LINE[a][b] = (ray_attacks(a, 0, ROOK_DIRS) & ray_attacks(b, 0, ROOK_DIRS)) | bb(a) | bb(b);
            } else if (ray_attacks(a, 0, BISHOP_DIRS) & bb(b)) {
                BETWEEN[a][b] = ray_attacks(a, bb(b), BISHOP_DIRS) & ray_attacks(b, bb(a), BISHOP_DIRS);
                LINE[a][b] = (ray_attacks(a, 0, BISHOP_DIRS) & ray_attacks(b, 0, BISHOP_DIRS)) | bb(a) | bb(b);
            }
        }
}

}  // namespace narc
