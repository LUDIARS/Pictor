// Web mesh module input contract (SPEC-PC-WEB-MESH): validation, vertex packing, sRGB
// conversion and lighting/surface checks. Pure CPU code, built natively.

#include "pictor/webgl/web_mesh_input.h"
#include "test_common.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace pictor;

namespace {

struct Triangle {
    std::vector<float>    positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float>    normals{0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<float>    colors{1, 0, 0, 0, 1, 0, 0, 0, 0.5f};
    std::vector<uint32_t> indices{0, 1, 2};

    WebMeshInput input() const {
        WebMeshInput in;
        in.positions = positions.data();
        in.normals = normals.data();
        in.colors = colors.data();
        in.vertex_count = positions.size() / 3;
        in.indices = indices.data();
        in.index_count = indices.size();
        return in;
    }
};

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void test_valid_mesh() {
    Triangle t;
    PT_ASSERT(validate_web_mesh_input(t.input()) == WebMeshInputError::NONE, "a well-formed triangle is accepted");
}

void test_rejections() {
    Triangle t;
    WebMeshInput in = t.input();
    in.normals = nullptr;
    PT_ASSERT(validate_web_mesh_input(in) == WebMeshInputError::MISSING_ARRAY, "missing normals");

    in = t.input();
    in.index_count = 0;
    PT_ASSERT(validate_web_mesh_input(in) == WebMeshInputError::EMPTY, "no indices");

    in = t.input();
    in.index_count = 2;
    PT_ASSERT(validate_web_mesh_input(in) == WebMeshInputError::NOT_TRIANGLES, "index count not a multiple of 3");

    Triangle bad_index;
    bad_index.indices[2] = 3;
    PT_ASSERT(validate_web_mesh_input(bad_index.input()) == WebMeshInputError::INDEX_OUT_OF_RANGE, "index past the last vertex");

    Triangle nan_position;
    nan_position.positions[4] = std::numeric_limits<float>::quiet_NaN();
    PT_ASSERT(validate_web_mesh_input(nan_position.input()) == WebMeshInputError::NON_FINITE_VALUE, "NaN position");

    Triangle inf_normal;
    inf_normal.normals[0] = std::numeric_limits<float>::infinity();
    PT_ASSERT(validate_web_mesh_input(inf_normal.input()) == WebMeshInputError::NON_FINITE_VALUE, "infinite normal");

    Triangle bright;
    bright.colors[0] = 1.5f;
    PT_ASSERT(validate_web_mesh_input(bright.input()) == WebMeshInputError::COLOR_OUT_OF_RANGE, "colour above 1");
}

void test_srgb_conversion() {
    PT_ASSERT(near(srgb_to_linear(0.0f), 0.0f), "black stays black");
    PT_ASSERT(near(srgb_to_linear(1.0f), 1.0f), "white stays white");
    PT_ASSERT(near(srgb_to_linear(0.04045f), 0.04045f / 12.92f), "linear segment");
    PT_ASSERT(near(srgb_to_linear(0.5f), 0.2140411f), "mid grey follows the power segment");
}

void test_packing() {
    Triangle t;
    std::vector<float> packed;
    pack_web_mesh_vertices(t.input(), WebColorSpace::SRGB, packed);
    PT_ASSERT_OP(packed.size(), ==, 3 * WEB_MESH_VERTEX_FLOATS, "9 floats per vertex");
    // vertex 1: position (1,0,0), normal (0,0,1), colour (0,1,0)
    const float* v1 = packed.data() + WEB_MESH_VERTEX_FLOATS;
    PT_ASSERT(near(v1[0], 1.0f) && near(v1[1], 0.0f), "position is copied");
    PT_ASSERT(near(v1[5], 1.0f), "normal is copied");
    PT_ASSERT(near(v1[7], 1.0f), "green stays 1 after sRGB decode");
    const float* v2 = packed.data() + 2 * WEB_MESH_VERTEX_FLOATS;
    PT_ASSERT(near(v2[8], srgb_to_linear(0.5f)), "sRGB colour is decoded to linear");

    pack_web_mesh_vertices(t.input(), WebColorSpace::LINEAR, packed);
    PT_ASSERT(near(packed[2 * WEB_MESH_VERTEX_FLOATS + 8], 0.5f), "linear colour is kept as given");
}

void test_lighting() {
    WebLighting lighting;
    lighting.directional_count = 2;
    PT_ASSERT(validate_web_lighting(lighting) == WebLightingError::NONE, "defaults with two lights are valid");

    WebLighting too_many = lighting;
    too_many.directional_count = WEB_MAX_DIRECTIONAL_LIGHTS + 1;
    PT_ASSERT(validate_web_lighting(too_many) == WebLightingError::TOO_MANY_LIGHTS, "light count cap");

    WebLighting zero = lighting;
    zero.directional[1].direction[0] = zero.directional[1].direction[1] = zero.directional[1].direction[2] = 0.0f;
    PT_ASSERT(validate_web_lighting(zero) == WebLightingError::ZERO_DIRECTION, "zero direction");

    WebLighting negative = lighting;
    negative.hemisphere_intensity = -1.0f;
    PT_ASSERT(validate_web_lighting(negative) == WebLightingError::OUT_OF_RANGE, "negative intensity");

    WebLighting unused = lighting;
    unused.directional_count = 1;
    unused.directional[1].intensity = std::numeric_limits<float>::quiet_NaN();
    PT_ASSERT(validate_web_lighting(unused) == WebLightingError::NONE, "slots past the count are ignored");

    WebSurface surface;
    PT_ASSERT(validate_web_surface(surface) == WebLightingError::NONE, "default surface");
    surface.roughness = 1.5f;
    PT_ASSERT(validate_web_surface(surface) == WebLightingError::OUT_OF_RANGE, "roughness above 1");
}

} // namespace

int main() {
    test_valid_mesh();
    test_rejections();
    test_srgb_conversion();
    test_packing();
    test_lighting();
    return pictor_test::report("unit_web_mesh_input_test");
}
