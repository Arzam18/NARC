// NARC Genesys chess engine, by Madushan Dissanayake.
#include <iostream>

#include "nnue.h"
#include "position.h"
#include "tt.h"
#include "uci.h"

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    narc::init_tables();
    narc::nnue::init();
    narc::TT.resize(16);
    return narc::uci_loop(argc, argv);
}
