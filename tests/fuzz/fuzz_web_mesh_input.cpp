// Fuzz entry: the Web mesh module's input contract (validate_web_mesh_input and
// pack_web_mesh_vertices), as the wasm C ABI receives it from JavaScript: the first
// 8 bytes give the vertex and index counts, the rest is the vertex and index data.
#include "fuzz_entry.h"

#include "pictor/webgl/web_mesh_input.h"

#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 8) return 0;
    uint32_t vertices = 0, indices = 0;
    std::memcpy(&vertices, data, 4);
    std::memcpy(&indices, data + 4, 4);
    const size_t payload = size - 8;
    // The C ABI cannot check that JavaScript passed arrays this long; here the counts
    // are clamped to the bytes the fuzzer supplied so every pointer stays valid.
    const size_t max_vertices = payload / (9 * sizeof(float));
    if (vertices > max_vertices) vertices = static_cast<uint32_t>(max_vertices);
    const size_t vertex_floats = size_t{vertices} * 9;
    const size_t max_indices = (payload - vertex_floats * sizeof(float)) / sizeof(uint32_t);
    if (indices > max_indices) indices = static_cast<uint32_t>(max_indices);

    std::vector<float> floats(vertex_floats);
    if (vertex_floats) std::memcpy(floats.data(), data + 8, vertex_floats * sizeof(float));
    std::vector<uint32_t> index_data(indices);
    if (indices) {
        std::memcpy(index_data.data(), data + 8 + vertex_floats * sizeof(float), size_t{indices} * sizeof(uint32_t));
    }

    pictor::WebMeshInput input;
    input.positions = floats.data();
    input.normals = floats.data() + size_t{vertices} * 3;
    input.colors = floats.data() + size_t{vertices} * 6;
    input.vertex_count = vertices;
    input.indices = index_data.data();
    input.index_count = indices;
    if (pictor::validate_web_mesh_input(input) == pictor::WebMeshInputError::NONE) {
        std::vector<float> packed;
        pictor::pack_web_mesh_vertices(input, pictor::WebColorSpace::SRGB, packed);
    }
    return 0;
}

std::vector<std::vector<uint8_t>> pictor_fuzz_seeds() {
    // One triangle: positions, normals, colours (0.5), indices 0 1 2.
    std::vector<uint8_t> seed(8);
    const uint32_t vertices = 3, indices = 3;
    std::memcpy(seed.data(), &vertices, 4);
    std::memcpy(seed.data() + 4, &indices, 4);
    const float values[27] = {0, 0, 0, 1, 0, 0, 0, 1, 0,
                              0, 0, 1, 0, 0, 1, 0, 0, 1,
                              .5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f};
    const auto* bytes = reinterpret_cast<const uint8_t*>(values);
    seed.insert(seed.end(), bytes, bytes + sizeof(values));
    for (uint32_t i : {0u, 1u, 2u}) {
        const auto* b = reinterpret_cast<const uint8_t*>(&i);
        seed.insert(seed.end(), b, b + 4);
    }
    return {seed};
}
