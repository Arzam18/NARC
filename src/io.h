// NARC Genesys - thread-safe output: the search thread and the UCI thread both write to stdout.
#pragma once

#include <iostream>
#include <mutex>
#include <string>

namespace narc {

inline std::mutex& output_mutex() {
    static std::mutex m;
    return m;
}

// Writes one complete line and flushes, never interleaved with another thread's line.
inline void out_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(output_mutex());
    std::cout << line << '\n' << std::flush;
}

}  // namespace narc
