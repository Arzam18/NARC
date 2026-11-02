// NARC Genesys - perft (move generation test) with bulk counting at the last ply.
#include "perft.h"

#include <chrono>
#include <iostream>

#include "movegen.h"

namespace narc {

u64 perft(Position& pos, int depth) {
    MoveList list;
    generate_legal(pos, list);
    if (depth <= 1) return depth == 1 ? u64(list.size()) : 1;
    u64 nodes = 0;
    for (Move m : list) {
        pos.make_move(m);
        nodes += perft(pos, depth - 1);
        pos.unmake_move(m);
    }
    return nodes;
}

u64 perft_divide(Position& pos, int depth) {
    auto t0 = std::chrono::steady_clock::now();
    MoveList list;
    generate_legal(pos, list);
    u64 total = 0;
    for (Move m : list) {
        pos.make_move(m);
        u64 n = depth > 1 ? perft(pos, depth - 1) : 1;
        pos.unmake_move(m);
        std::cout << move_to_uci(m) << ": " << n << '\n';
        total += n;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\nNodes searched: " << total << " (" << ms << " ms)" << std::endl;
    return total;
}

namespace {
struct PerftCase {
    const char* fen;
    int depth;
    u64 nodes;
};
// Standard reference positions and counts.
constexpr PerftCase SUITE[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6, 119060324ULL},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 5, 193690690ULL},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083ULL},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, 15833292ULL},
    {"r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1", 5, 15833292ULL},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 5, 89941194ULL},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 5, 164075551ULL},
};
}  // namespace

bool perft_suite() {
    bool all_ok = true;
    u64 total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : SUITE) {
        Position* pos = new Position;
        pos->set_fen(c.fen);
        u64 n = perft(*pos, c.depth);
        delete pos;
        total += n;
        bool ok = n == c.nodes;
        all_ok &= ok;
        std::cout << (ok ? "ok   " : "FAIL ") << "depth " << c.depth << " " << n << " (expected " << c.nodes << ")  "
                  << c.fen << std::endl;
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << (all_ok ? "perft suite passed" : "perft suite FAILED") << ": " << total << " nodes in " << s
              << " s (" << u64(total / std::max(s, 1e-9) / 1e6) << " Mnps)" << std::endl;
    return all_ok;
}

}  // namespace narc
