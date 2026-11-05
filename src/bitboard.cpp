// NARC Genesys - attack tables. Slider attacks use magic multiplication, or PEXT on BMI2 builds;
// the magics are built in (found once by the seeded search below), so startup is instant; the search runs only if a
// built-in magic fails its check, which would take about 1.5 s.
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

// Magics found by build_slider's seeded search (seeds below), checked again at startup.
constexpr u64 ROOK_MAGICS[64] = {
    0x018000604000811BULL, 0x1080168220004000ULL, 0x8880100208802000ULL, 0x0200084200042011ULL,
    0x12000810A0020084ULL, 0x4900010002040008ULL, 0x2080410000800200ULL, 0x4100008220421500ULL,
    0x8401800080400120ULL, 0x0004400040201000ULL, 0x0480801000802005ULL, 0x0800800800100080ULL,
    0x0000800400800800ULL, 0x4008800400800200ULL, 0x1201000100020004ULL, 0x0839000082084100ULL,
    0x0108288000400082ULL, 0x0041050020400882ULL, 0x0000808010002000ULL, 0x000A020010402008ULL,
    0x0C04808004000800ULL, 0x0090808002000400ULL, 0x0019040002289001ULL, 0x00280A000040810CULL,
    0x0010802880004000ULL, 0x00005000C0092000ULL, 0x045B021500402000ULL, 0x0108000880801000ULL,
    0x8008000500108900ULL, 0x0800020080040080ULL, 0x0000020400104108ULL, 0x3900892200084484ULL,
    0x0000400020800080ULL, 0x110081002A004200ULL, 0x8440150041002002ULL, 0x1042001022004008ULL,
    0x0020080080800400ULL, 0x4800040080800200ULL, 0x0080080204000110ULL, 0x080404058A000849ULL,
    0x1000802040008004ULL, 0x8010002000424000ULL, 0x0024804200220010ULL, 0x0055090010010020ULL,
    0x0000080004008080ULL, 0x0024040002008080ULL, 0x2200020001008080ULL, 0x00801441108E0004ULL,
    0x0080002041009100ULL, 0x000A228110400300ULL, 0x0010200100401100ULL, 0xA400080080100080ULL,
    0x0900880100100500ULL, 0x2604008002000480ULL, 0x00A0029001280400ULL, 0x0030004104008200ULL,
    0x0000201100800041ULL, 0x8005204A00108102ULL, 0x9900082080420012ULL, 0x208010002038C501ULL,
    0x0001000290080005ULL, 0x4042000190040802ULL, 0x8110100801008204ULL, 0x0100108C00210442ULL};
constexpr u64 BISHOP_MAGICS[64] = {
    0x4002204911030105ULL, 0x5A20280A40404008ULL, 0x0004A80201422000ULL, 0x8011040083100022ULL,
    0x0001104040000000ULL, 0x05010402C0080000ULL, 0x0010880110118000ULL, 0x0000808801012008ULL,
    0x0400408202040101ULL, 0x1006040808404081ULL, 0x0082488800408221ULL, 0x0040110400842000ULL,
    0x4200042420000504ULL, 0x0400010420050100ULL, 0x0210808208218404ULL, 0x4002004404442248ULL,
    0x4020009002100120ULL, 0x81424104100A0600ULL, 0x00100001070AA101ULL, 0x0004300802002010ULL,
    0x0101000820082800ULL, 0x0080209210100803ULL, 0x60160008D8040CE2ULL, 0x04002011040104C4ULL,
    0x000620207908B036ULL, 0x00082024080200A8ULL, 0x0002020401080200ULL, 0x0812080004040408ULL,
    0x0041010000104000ULL, 0x2088020080412880ULL, 0x0480842001042204ULL, 0x446410200041042AULL,
    0x8070446088448800ULL, 0x2244304202080200ULL, 0x0884004100082201ULL, 0x2008020080180080ULL,
    0x8C90020200422008ULL, 0x0010008201828A08ULL, 0x0004040048042100ULL, 0x1184408188042400ULL,
    0x4202022004202032ULL, 0x4219010120221000ULL, 0x0002022208020100ULL, 0x8284082124020800ULL,
    0x041409612200C400ULL, 0x004176080A000040ULL, 0x04880208CC000202ULL, 0x0008022410200040ULL,
    0x1001280104600000ULL, 0x0008208804B00000ULL, 0x8621420101988800ULL, 0x0000000020880201ULL,
    0x8820803021024200ULL, 0x2200092108008000ULL, 0x0050200101222122ULL, 0x092324080085000AULL,
    0x4640104104202010ULL, 0x1080010411010808ULL, 0x010042808400C800ULL, 0x8000100081940414ULL,
    0x1404224030021204ULL, 0x0400004405380200ULL, 0x2204203404084048ULL, 0x0582040808210020ULL};

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

void build_slider(SliderTable* table, Bitboard* storage, const int (*dirs)[2], u64 seed, const u64* known) {
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
        (void)known;
        (void)rng;
        for (size_t i = 0; i < occupancy.size(); i++) t.attacks[t.index(occupancy[i])] = reference[i];
#else
        stamp.assign(size, 0);
        for (int attempt = 1;; attempt++) {
            t.magic = attempt == 1 ? known[s] : rng.sparse();   // the built-in magic first
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
    build_slider(ROOK_TABLE, rook_storage, ROOK_DIRS, 0x6D616B617261ULL, ROOK_MAGICS);     // fixed magic-search seed
    build_slider(BISHOP_TABLE, bishop_storage, BISHOP_DIRS, 0x4D414B415241ULL, BISHOP_MAGICS);

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
