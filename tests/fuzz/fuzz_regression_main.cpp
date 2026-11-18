// Replays a fuzz entry point (tests/fuzz/fuzz_*.cpp) on fixed inputs: the entry's
// seeds, every prefix of each seed, deterministic byte mutations, and inputs with
// no structure at all. Exits non-zero only when the entry reports a failure; a
// crash, sanitizer report or hardened-library assertion fails the test by itself.
#include "fuzz_entry.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// xorshift32: the same mutations on every run and platform.
uint32_t next(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int run(const std::vector<uint8_t>& input) {
    return LLVMFuzzerTestOneInput(input.empty() ? nullptr : input.data(), input.size());
}

} // namespace

// --write-seeds <dir>: store the seeds as files, the starting corpus of the libFuzzer binary.
static int write_seeds(const char* dir) {
    int index = 0;
    for (const auto& seed : pictor_fuzz_seeds()) {
        std::ofstream out(std::string(dir) + "/seed" + std::to_string(index++), std::ios::binary);
        out.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        if (!out) return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--write-seeds") == 0) return write_seeds(argv[2]);
    std::vector<std::vector<uint8_t>> inputs = pictor_fuzz_seeds();
    inputs.push_back({});
    inputs.push_back(std::vector<uint8_t>(1, 0xFF));
    inputs.push_back(std::vector<uint8_t>(4096, 0xFF));
    inputs.push_back(std::vector<uint8_t>(4096, 0x00));

    size_t runs = 0;
    int failures = 0;
    uint32_t state = 0x9E3779B9u;
    for (const auto& seed : pictor_fuzz_seeds()) {
        for (size_t length = 0; length <= seed.size(); ++length) {
            failures += run(std::vector<uint8_t>(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(length)));
            ++runs;
        }
        for (int round = 0; round < 2000 && !seed.empty(); ++round) {
            std::vector<uint8_t> mutated = seed;
            const uint32_t edits = 1 + next(state) % 8;
            for (uint32_t e = 0; e < edits; ++e) {
                const size_t at = next(state) % mutated.size();
                switch (next(state) % 4) {
                    case 0: mutated[at] = static_cast<uint8_t>(next(state)); break;
                    case 1: mutated[at] = 0xFF; break;
                    case 2: mutated[at] = 0x00; break;
                    default: mutated[at] ^= static_cast<uint8_t>(1u << (next(state) % 8)); break;
                }
            }
            failures += run(mutated);
            ++runs;
        }
    }
    for (const auto& input : inputs) {
        failures += run(input);
        ++runs;
    }
    std::printf("fuzz regression: %zu inputs, %d failures\n", runs, failures);
    return failures == 0 ? 0 : 1;
}
