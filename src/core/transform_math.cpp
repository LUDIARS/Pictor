#include "pictor/core/transform_math.h"

#include <cmath>

namespace pictor::transform_math {

namespace {

// これより小さい行列式は特異とみなす (float の桁落ちで逆行列が発散するため)。
constexpr float kSingularDeterminant = 1e-12f;
constexpr float kZeroQuaternionLengthSq = 1e-16f;

} // namespace

float4x4 multiply(const float4x4& a, const float4x4& b) {
    float4x4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[i][k] * b.m[k][j];
            r.m[i][j] = sum;
        }
    }
    return r;
}

bool inverse(const float4x4& m, float4x4& out) {
    // 2x2 小行列式を先に求める余因子展開 (Laplace expansion)。
    const float* a = &m.m[0][0];
    const float s0 = a[0] * a[5]  - a[4] * a[1];
    const float s1 = a[0] * a[6]  - a[4] * a[2];
    const float s2 = a[0] * a[7]  - a[4] * a[3];
    const float s3 = a[1] * a[6]  - a[5] * a[2];
    const float s4 = a[1] * a[7]  - a[5] * a[3];
    const float s5 = a[2] * a[7]  - a[6] * a[3];
    const float c5 = a[10] * a[15] - a[14] * a[11];
    const float c4 = a[9]  * a[15] - a[13] * a[11];
    const float c3 = a[9]  * a[14] - a[13] * a[10];
    const float c2 = a[8]  * a[15] - a[12] * a[11];
    const float c1 = a[8]  * a[14] - a[12] * a[10];
    const float c0 = a[8]  * a[13] - a[12] * a[9];

    const float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::fabs(det) < kSingularDeterminant) return false;
    const float inv_det = 1.0f / det;

    float4x4 r{};
    float* o = &r.m[0][0];
    o[0]  = ( a[5]  * c5 - a[6]  * c4 + a[7]  * c3) * inv_det;
    o[1]  = (-a[1]  * c5 + a[2]  * c4 - a[3]  * c3) * inv_det;
    o[2]  = ( a[13] * s5 - a[14] * s4 + a[15] * s3) * inv_det;
    o[3]  = (-a[9]  * s5 + a[10] * s4 - a[11] * s3) * inv_det;
    o[4]  = (-a[4]  * c5 + a[6]  * c2 - a[7]  * c1) * inv_det;
    o[5]  = ( a[0]  * c5 - a[2]  * c2 + a[3]  * c1) * inv_det;
    o[6]  = (-a[12] * s5 + a[14] * s2 - a[15] * s1) * inv_det;
    o[7]  = ( a[8]  * s5 - a[10] * s2 + a[11] * s1) * inv_det;
    o[8]  = ( a[4]  * c4 - a[5]  * c2 + a[7]  * c0) * inv_det;
    o[9]  = (-a[0]  * c4 + a[1]  * c2 - a[3]  * c0) * inv_det;
    o[10] = ( a[12] * s4 - a[13] * s2 + a[15] * s0) * inv_det;
    o[11] = (-a[8]  * s4 + a[9]  * s2 - a[11] * s0) * inv_det;
    o[12] = (-a[4]  * c3 + a[5]  * c1 - a[6]  * c0) * inv_det;
    o[13] = ( a[0]  * c3 - a[1]  * c1 + a[2]  * c0) * inv_det;
    o[14] = (-a[12] * s3 + a[13] * s1 - a[14] * s0) * inv_det;
    o[15] = ( a[8]  * s3 - a[9]  * s1 + a[10] * s0) * inv_det;
    out = r;
    return true;
}

float4x4 inverse_rigid(const float4x4& m) {
    // [R 0; t 1] の逆は [R^T 0; -t R^T 1] (行ベクトル規約)。
    float4x4 r = float4x4::identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m.m[j][i];
    for (int j = 0; j < 3; ++j) {
        r.m[3][j] = -(m.m[3][0] * r.m[0][j] +
                      m.m[3][1] * r.m[1][j] +
                      m.m[3][2] * r.m[2][j]);
    }
    return r;
}

float4x4 from_pose(const float4& q_in, const float3& position) {
    float x = q_in.x, y = q_in.y, z = q_in.z, w = q_in.w;
    const float len_sq = x * x + y * y + z * z + w * w;
    if (len_sq < kZeroQuaternionLengthSq) {
        x = y = z = 0.0f;
        w = 1.0f;
    } else {
        const float inv = 1.0f / std::sqrt(len_sq);
        x *= inv; y *= inv; z *= inv; w *= inv;
    }

    // 行 i = ローカル基底 i を回した先のベクトル (行ベクトル規約)。
    float4x4 m = float4x4::identity();
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;
    m.m[0][0] = 1.0f - 2.0f * (yy + zz);
    m.m[0][1] = 2.0f * (xy + wz);
    m.m[0][2] = 2.0f * (xz - wy);
    m.m[1][0] = 2.0f * (xy - wz);
    m.m[1][1] = 1.0f - 2.0f * (xx + zz);
    m.m[1][2] = 2.0f * (yz + wx);
    m.m[2][0] = 2.0f * (xz + wy);
    m.m[2][1] = 2.0f * (yz - wx);
    m.m[2][2] = 1.0f - 2.0f * (xx + yy);
    m.set_translation(position.x, position.y, position.z);
    return m;
}

bool perspective_asymmetric(float l, float r, float u, float d,
                            float near_z, float far_z, float4x4& out) {
    if (!(r > l) || !(u > d) || !(near_z > 0.0f) || !(far_z > near_z)) return false;

    float4x4 p{};
    p.m[0][0] = 2.0f / (r - l);
    p.m[2][0] = (r + l) / (r - l);
    p.m[1][1] = -2.0f / (u - d);          // Vulkan は y 下向き
    p.m[2][1] = -(u + d) / (u - d);
    p.m[2][2] = far_z / (near_z - far_z); // z は [0,1]
    p.m[2][3] = -1.0f;
    p.m[3][2] = (near_z * far_z) / (near_z - far_z);
    out = p;
    return true;
}

float4 transform_point(const float4x4& m, const float3& p) {
    float4 r;
    r.x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    r.y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    r.z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    r.w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    return r;
}

} // namespace pictor::transform_math
