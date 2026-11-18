// Shared declarations of the Pictor fuzz entry points (tests/fuzz/).
//
// Each fuzz_<name>.cpp defines LLVMFuzzerTestOneInput for one parser and
// pictor_fuzz_seeds() with small well-formed inputs. The same translation unit is
// linked either with libFuzzer (PICTOR_BUILD_FUZZERS, Clang only) or with
// fuzz_regression_main.cpp, which replays the seeds and deterministic mutations
// of them under ctest on every compiler.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/// Small valid (or nearly valid) inputs for the parser under test.
std::vector<std::vector<uint8_t>> pictor_fuzz_seeds();
