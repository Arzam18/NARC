// NARC Genesys - UCI protocol. The search runs on its own thread so "stop", "isready" and "quit" are handled at once.
#include "uci.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "io.h"
#include "movegen.h"
#include "nnue.h"
#include "perft.h"
#include "search.h"
#include "tt.h"

namespace narc {

namespace {

#ifndef NARC_TAG
#define NARC_TAG ""   // set by build.sh from TAG (e.g. the network: kb4, kb64)
#endif
constexpr const char* NAME = "NARC Genesys 5.1";
constexpr const char* AUTHOR = "Madushan Dissanayake";
constexpr const char* COUNTRY = "Sri Lanka";
const std::string FULL_NAME = std::string(NAME) + (NARC_TAG[0] ? std::string(" ") + NARC_TAG : std::string());

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void set_position(Position& pos, std::istringstream& in) {
    std::string token, fen;
    in >> token;
    if (token == "startpos") {
        fen = Position::START_FEN;
        in >> token;   // "moves", if present
    } else if (token == "fen") {
        while (in >> token && token != "moves") fen += token + " ";
    } else {
        return;
    }
    Position next;
    if (!next.set_fen(fen)) {
        out_line("info string invalid FEN: " + fen);
        return;
    }
    while (in >> token) {
        Move m = next.parse_uci_move(token);
        if (m == NO_MOVE) {
            out_line("info string illegal move " + token);
            break;
        }
        if (next.game_ply() >= MAX_GAME_PLY - 1) next.set_fen(next.fen());   // keep the history array bounded
        next.make_move(m);
    }
    pos = next;
}

SearchLimits parse_go(std::istringstream& in) {
    SearchLimits l;
    std::string token;
    while (in >> token) {
        if (token == "wtime") in >> l.time[WHITE];
        else if (token == "btime") in >> l.time[BLACK];
        else if (token == "winc") in >> l.inc[WHITE];
        else if (token == "binc") in >> l.inc[BLACK];
        else if (token == "movestogo") in >> l.movestogo;
        else if (token == "depth") in >> l.depth;
        else if (token == "nodes") in >> l.nodes;
        else if (token == "movetime") in >> l.movetime;
        else if (token == "infinite") l.infinite = true;
    }
    return l;
}

void set_option(Options& opt, std::istringstream& in) {
    std::string token, name, value;
    in >> token;   // "name"
    while (in >> token && token != "value") name += (name.empty() ? "" : " ") + token;
    while (in >> token) value += (value.empty() ? "" : " ") + token;
    name = lower(name);
    try {
        auto truthy = [&]() { std::string v = lower(value); return v == "true" || v == "1" || v == "on"; };
        if (name == "hash") {
            opt.hash_mb = std::clamp(std::stoi(value), 1, 65536);
            TT.resize(size_t(opt.hash_mb));
        }
        else if (name == "pruning") opt.features.pruning = truthy();         // hidden: for testing
        else if (name == "reductions") opt.features.reductions = truthy();   // hidden: for testing
        else if (name == "singular") opt.features.singular = truthy();
        else if (name == "correction") opt.features.correction = truthy();
        else if (name == "smarttime") opt.features.smart_time = truthy();
        else if (name == "corrplus") opt.features.corr_plus = truthy();     // 0.5-dev features under test
        else if (name == "ttpv") opt.features.tt_pv = truthy();
        else if (name == "probcut") opt.features.probcut = truthy();
        else if (name == "ttagefix") TT.age_fix = truthy();
        else if (name == "threathist") opt.features.threat_hist = truthy();   // batch 2 under test
        else if (name == "conthist4") opt.features.cont_hist4 = truthy();
        else if (name == "fiftydamp") opt.features.fifty_damp = truthy();
        else if (name == "qsevasion") opt.features.qs_evasion = truthy();
        else if (name == "doubleextcap") opt.features.double_ext_cap = truthy();
        else if (name == "doubt") opt.features.doubt = truthy();             // batch 3 under test
        else if (name == "rootwatch") opt.features.root_watch = truthy();
        else if (int* tp = tune_param(name)) {                              // SPSA-tuned search constants
            *tp = std::stoi(value);
            if (name == "lmr_base" || name == "lmr_div") init_lmr();
        }
        else if (name == "threads") opt.threads = std::clamp(std::stoi(value), 1, 256);   // applied by the caller
        else if (name == "move overhead") opt.move_overhead = std::clamp(std::stoi(value), 0, 5000);
        else if (name == "evalfile") {
            std::string error;
            if (value.empty() || value == "<embedded>") nnue::use_embedded();
            else if (!nnue::load_file(value, error)) out_line("info string EvalFile: " + error);
            out_line("info string network " + nnue::network_name());
        }
        else out_line("info string unknown option " + name);
    } catch (...) {
        out_line("info string bad value for " + name);
    }
}

// Random walks with make/unmake, evaluating at random points: the lazy incremental accumulators must always
// equal a full refresh. Covers captures, promotions, castling and king-bucket changes over many positions.
void nnue_check(int steps) {
    u64 rng = 0x9E3779B97F4A7C15ULL;
    auto next = [&rng]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    auto pos = std::make_unique<Position>();
    auto ev = std::make_unique<nnue::Evaluator>();
    const char* starts[] = {Position::START_FEN,
                            "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                            "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                            "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"};
    long checks = 0, bad = 0;
    for (int s = 0; s < steps;) {
        pos->set_fen(starts[next() % 4]);
        ev->reset(*pos);
        Move line[MAX_PLY];
        int depth = 0;
        for (int k = 0; k < 400 && s < steps; k++, s++) {
            MoveList list;
            generate_legal(*pos, list);
            bool back = depth > 0 && (list.size() == 0 || depth >= MAX_PLY - 2 || next() % 100 < 35);
            if (back) {
                ev->pop();
                pos->unmake_move(line[--depth]);
            } else if (list.size() > 0) {
                Move m = list[int(next() % u64(list.size()))];
                pos->make_move(m);
                ev->push();
                line[depth++] = m;
            } else {
                break;
            }
            if (next() % 100 < 40) {
                checks++;
                int inc = ev->units(*pos), full = nnue::Evaluator::units_full(*pos);
                if (inc != full && bad++ < 5)
                    std::cout << "mismatch " << inc << " vs " << full << ": " << pos->fen() << std::endl;
            }
        }
    }
    std::cout << "nnuecheck: " << checks << " evaluations, " << bad << " mismatches" << std::endl;
}

// Position::is_legal must accept exactly the generated moves: every 16-bit code on positions from random games.
void legal_check(int positions) {
    u64 rng = 0x2545F4914F6CDD1DULL;
    auto next = [&rng]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    auto pos = std::make_unique<Position>();
    long bad = 0, legal_total = 0;
    for (int n = 0; n < positions; n++) {
        pos->set_fen(Position::START_FEN);
        int plies = int(next() % 120);
        for (int k = 0; k < plies; k++) {
            MoveList list;
            generate_legal(*pos, list);
            if (list.size() == 0 || pos->game_ply() >= MAX_GAME_PLY - 2) break;
            pos->make_move(list[int(next() % u64(list.size()))]);
        }
        MoveList list;
        generate_legal(*pos, list);
        static bool in_list[65536];
        std::fill(std::begin(in_list), std::end(in_list), false);
        for (Move m : list) in_list[m] = true;
        legal_total += list.size();
        for (int code = 0; code < 65536; code++)
            if (pos->is_legal(Move(code)) != in_list[code] && bad++ < 5)
                std::cout << "is_legal mismatch " << move_to_uci(Move(code)) << " flag " << (code >> 12) << ": " << pos->fen()
                          << std::endl;
    }
    std::cout << "legalcheck: " << positions << " positions, " << legal_total << " legal moves, " << bad << " mismatches"
              << std::endl;
}

// narc relabel <in.bin> <out.bin> <nodes> [seconds]
// Re-scores 32-byte training records (colour * 8 + type nibbles, the training-record layout) with a
// node-limited search. Every byte except the score is copied. Dropped: positions in check, a best move that is a
// capture or promotion, |score| >= 2000 cp. TT and histories are cleared at each game start (byte 28 == 0).
// With [seconds], stops cleanly once that much time has passed (for machines with a hard session limit).
int relabel(ThreadPool& pool, const std::string& in_path, const std::string& out_path, u64 nodes, double limit_s) {
    std::FILE* in = std::fopen(in_path.c_str(), "rb");
    std::FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!in || !out) {
        std::printf("relabel: cannot open files\n");
        return 1;
    }
    TT.resize(4);   // positions are searched shallowly and the table is cleared per game: keep it small
    auto pos = std::make_unique<Position>();
    u8 rec[32];
    long read = 0, kept = 0, in_check = 0, tactical = 0, big = 0;
    const auto t0 = std::chrono::steady_clock::now();
    bool timed_out = false;
    while (std::fread(rec, 1, 32, in) == 32) {
        if (limit_s > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= limit_s) {
            timed_out = true;
            break;
        }
        read++;
        if (rec[28] == 0 || read == 1) {
            TT.clear();
            pool.new_game();
        }
        // record -> FEN
        u64 occ = 0;
        for (int i = 0; i < 8; i++) occ |= u64(rec[i]) << (8 * i);
        int board[64];
        for (int& b : board) b = -1;
        int k = 0;
        for (int sq = 0; sq < 64; sq++)
            if (occ >> sq & 1) {
                int code = k & 1 ? rec[8 + k / 2] >> 4 : rec[8 + k / 2] & 15;
                board[sq] = code;
                k++;
            }
        std::string fen;
        for (int r = 7; r >= 0; r--) {
            int empty = 0;
            for (int f = 0; f < 8; f++) {
                int code = board[r * 8 + f];
                if (code < 0) {
                    empty++;
                    continue;
                }
                if (empty) fen += char('0' + empty);
                empty = 0;
                fen += "PNBRQK??pnbrqk??"[code];
            }
            if (empty) fen += char('0' + empty);
            if (r) fen += '/';
        }
        fen += rec[27] ? " b - - 0 1" : " w - - 0 1";
        if (!pos->set_fen(fen)) {
            std::printf("relabel: bad record %ld\n", read);
            return 1;
        }
        if (pos->in_check()) {
            in_check++;
            continue;
        }
        Move best = NO_MOVE;
        int score = pool.label(*pos, nodes, best);
        if (is_capture(best) || is_promotion(best)) {
            tactical++;
            continue;
        }
        if (std::abs(score) >= 2000) {
            big++;
            continue;
        }
        const int white = rec[27] ? -score : score;
        const u16 bits = u16(i16(white));
        rec[24] = u8(bits & 0xFF);
        rec[25] = u8(bits >> 8);
        std::fwrite(rec, 1, 32, out);
        kept++;
        if (read % 5000 == 0) {
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("relabel: read %ld kept %ld pos/s %ld\n", read, kept, long(read / std::max(s, 1e-9)));
            std::fflush(stdout);
            std::fflush(out);
        }
    }
    std::fclose(in);
    std::fclose(out);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("relabel done: read %ld kept %ld dropped in_check %ld tactical %ld big_score %ld seconds %ld%s\n",
                read, kept, in_check, tactical, big, long(s), timed_out ? " (time limit)" : "");
    return 0;
}

// narc datagen <out.bin> <seed> <seconds> [play_nodes] [label_nodes] [keep_pct]
// Self-play training records (the relabel layout; byte 26 = game result, 28 = record ordinal in the game, 29 = ply).
// Diversity: every game opens with 8-9 random legal plies (kept only if a 1,000-node search scores it within
// 200 cp), each game plays at play_nodes +-25%, and before ply 40 a move is random with probability 5% (one of up to
// 3 random tries that a 1,000-node search scores within 100 cp of the best move; else the best move).
// Labels: from ply 10, keep_pct% of the positions are searched with label_nodes; that score is the label and that
// move is played. Dropped as in relabel (in check, capture or promotion best move, |score| >= 2000) and repeats.
// Games end on mate, stalemate, repetition, fifty moves, insufficient material or ply 400; adjudicated as a win
// after |score| >= 1000 for 4 plies, as a draw from ply 120 after |score| <= 8 for 12 plies.
// Starts no new game once <seconds> have passed.
int datagen(ThreadPool& pool, const std::string& out_path, u64 seed, double limit_s, u64 play_nodes,
            u64 label_nodes, int keep_pct) {
    std::FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) {
        std::printf("datagen: cannot open %s\n", out_path.c_str());
        return 1;
    }
    TT.resize(16);
    u64 rng = seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
    auto next = [&rng]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    auto pos = std::make_unique<Position>();
    std::unordered_set<u64> seen;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&t0]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    long games = 0, positions = 0, wins = 0, draws = 0, losses = 0;
    std::vector<std::array<u8, 32>> recs;
    while (elapsed() < limit_s) {
        TT.clear();
        pool.new_game();
        // random opening, balanced by a short search
        int ply = 0;
        for (;;) {
            pos->set_fen(Position::START_FEN);
            ply = 0;
            const int n = 8 + int(next() % 2);
            bool ok = true;
            for (; ply < n && ok; ply++) {
                MoveList list;
                generate_legal(*pos, list);
                if (list.size() == 0) ok = false;
                else pos->make_move(list[int(next() % u64(list.size()))]);
            }
            MoveList list;
            generate_legal(*pos, list);
            if (!ok || list.size() == 0) continue;
            Move b = NO_MOVE;
            if (std::abs(pool.label(*pos, 1000, b)) <= 200) break;
        }
        const u64 play = play_nodes * (75 + next() % 51) / 100;
        recs.clear();
        int result = 1, win_run = 0, draw_run = 0;
        for (;; ply++) {
            MoveList list;
            generate_legal(*pos, list);
            if (list.size() == 0) {
                result = pos->in_check() ? (pos->side_to_move() == WHITE ? 0 : 2) : 1;
                break;
            }
            if (pos->is_repetition() || pos->is_fifty_move_draw() || pos->insufficient_material() || ply >= 400) {
                result = 1;
                break;
            }
            const bool labelled = ply >= 10 && !pos->in_check() && int(next() % 100) < keep_pct;
            Move best = NO_MOVE;
            const int score = pool.label(*pos, labelled ? label_nodes : play, best);
            const int white = pos->side_to_move() == WHITE ? score : -score;
            if (labelled && best != NO_MOVE && !is_capture(best) && !is_promotion(best) && std::abs(score) < 2000 &&
                seen.insert(pos->key()).second) {
                std::array<u8, 32> r{};
                const u64 occ = pos->pieces();
                for (int i = 0; i < 8; i++) r[i] = u8(occ >> (8 * i));
                int k = 0;
                for (Bitboard b = occ; b; k++) {
                    const int code = pos->piece_on(pop_lsb(b));
                    r[8 + k / 2] |= u8(k & 1 ? code << 4 : code);
                }
                const u16 bits = u16(i16(white));
                r[24] = u8(bits & 0xFF);
                r[25] = u8(bits >> 8);
                r[27] = pos->side_to_move() == WHITE ? 0 : 1;
                r[28] = u8(std::min<size_t>(recs.size(), 255));
                r[29] = u8(std::min(ply, 255));
                recs.push_back(r);
            }
            win_run = std::abs(white) >= 1000 ? win_run + 1 : 0;
            if (win_run >= 4) {
                result = white > 0 ? 2 : 0;
                break;
            }
            draw_run = ply >= 120 && std::abs(white) <= 8 ? draw_run + 1 : 0;
            if (draw_run >= 12) {
                result = 1;
                break;
            }
            Move m = best;
            if (m == NO_MOVE) m = list[int(next() % u64(list.size()))];
            else if (ply < 40 && next() % 100 < 5) {
                // a random move for variety, but not a blunder: within 100 cp of the best move by a short search
                for (int t = 0; t < 3; t++) {
                    const Move r = list[int(next() % u64(list.size()))];
                    if (r == best) continue;
                    pos->make_move(r);
                    Move reply = NO_MOVE;
                    const int after = -pool.label(*pos, 1000, reply);
                    pos->unmake_move(r);
                    if (after >= score - 100) {
                        m = r;
                        break;
                    }
                }
            }
            pos->make_move(m);
        }
        for (auto& r : recs) {
            r[26] = u8(result);
            std::fwrite(r.data(), 1, 32, out);
        }
        games++;
        positions += long(recs.size());
        (result == 2 ? wins : result == 1 ? draws : losses)++;
        if (games % 50 == 0) {
            std::printf("datagen: games %ld positions %ld (%.1f/s)\n", games, positions, positions / elapsed());
            std::fflush(stdout);
            std::fflush(out);
        }
    }
    std::fclose(out);
    std::printf("datagen done: games %ld positions %ld white %ld draw %ld black %ld seconds %ld\n", games, positions,
                wins, draws, losses, long(elapsed()));
    return 0;
}

}  // namespace

int uci_loop(int argc, char** argv) {
    auto pos = std::make_unique<Position>();
    auto searcher = std::make_unique<ThreadPool>();
    Options options;
    pos->set_fen(Position::START_FEN);

    // Command-line mode: narc bench [depth] | perft <depth> | perftsuite
    if (argc > 1) {
        std::string cmd = argv[1];
        if (cmd == "bench") searcher->bench(argc > 2 ? std::atoi(argv[2]) : 6);
        else if (cmd == "perft") perft_divide(*pos, argc > 2 ? std::atoi(argv[2]) : 5);
        else if (cmd == "perftsuite") return perft_suite() ? 0 : 1;
        else if (cmd == "nnuecheck") nnue_check(argc > 2 ? std::atoi(argv[2]) : 20000);
        else if (cmd == "legalcheck") legal_check(argc > 2 ? std::atoi(argv[2]) : 2000);
        else if (cmd == "relabel" && argc > 4) return relabel(*searcher, argv[2], argv[3], u64(std::atoll(argv[4])),
                                                               argc > 5 ? std::atof(argv[5]) : 0.0);
        else if (cmd == "datagen" && argc > 4)
            return datagen(*searcher, argv[2], u64(std::atoll(argv[3])), std::atof(argv[4]),
                           argc > 5 ? u64(std::atoll(argv[5])) : 2000, argc > 6 ? u64(std::atoll(argv[6])) : 20000,
                           argc > 7 ? std::atoi(argv[7]) : 15);
        else std::cout << "unknown command " << cmd << " (bench [depth], perft <depth>, perftsuite)" << std::endl;
        return 0;
    }

    out_line(FULL_NAME + " by " + AUTHOR + " (" + COUNTRY + ")");   // startup banner; GUIs ignore it
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        if (cmd == "uci") {
            out_line("id name " + FULL_NAME + "\nid author " + AUTHOR +
                     "\noption name Hash type spin default 16 min 1 max 65536"
                     "\noption name Threads type spin default 1 min 1 max 256"
                     "\noption name Move Overhead type spin default 10 min 0 max 5000"
                     "\noption name EvalFile type string default <embedded>"   // dev switches and tune constants: setoption only
                     "\nuciok");
        } else if (cmd == "isready") {
            out_line("readyok");
        } else if (cmd == "ucinewgame") {
            searcher->stop();
            searcher->wait();
            TT.clear();
            searcher->new_game();
        } else if (cmd == "position") {
            searcher->stop();
            searcher->wait();
            set_position(*pos, in);
        } else if (cmd == "go") {
            searcher->stop();
            searcher->wait();
            searcher->start(*pos, parse_go(in), options);
        } else if (cmd == "stop") {
            searcher->stop();
            searcher->wait();
        } else if (cmd == "setoption") {
            searcher->stop();
            searcher->wait();
            set_option(options, in);
            searcher->set_threads(options.threads);
        } else if (cmd == "quit") {
            break;
        } else if (cmd == "d") {
            out_line(pos->pretty());
        } else if (cmd == "perft") {
            int depth = 1;
            in >> depth;
            perft_divide(*pos, depth);
        } else if (cmd == "eval") {
            auto ev = std::make_unique<nnue::Evaluator>();
            ev->reset(*pos);
            out_line("eval " + std::to_string(ev->evaluate(*pos)) + " cp, network units " +
                     std::to_string(ev->units(*pos)) + " (side to move; full refresh " +
                     std::to_string(nnue::Evaluator::units_full(*pos)) + ")");
        } else if (cmd == "nnuecheck") {
            int steps = 20000;
            in >> steps;
            nnue_check(steps);
        } else if (cmd == "bench") {
            int depth = 6;
            in >> depth;
            searcher->bench(depth);
        } else if (!cmd.empty()) {
            out_line("info string unknown command " + cmd);
        }
    }
    searcher->stop();
    searcher->wait();
    return 0;
}

}  // namespace narc
