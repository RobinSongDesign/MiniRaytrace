#pragma once
// Thin math layer over GLM plus AABB used by the BVH builder.

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <limits>

namespace mrt {

using glm::vec2;
using glm::vec3;
using glm::vec4;
using glm::mat4;
using glm::uvec2;

struct Aabb {
    vec3 lo{ std::numeric_limits<float>::max() };
    vec3 hi{ -std::numeric_limits<float>::max() };

    void expand(const vec3& p) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    void expand(const Aabb& b) { lo = glm::min(lo, b.lo); hi = glm::max(hi, b.hi); }

    vec3 extent() const { return hi - lo; }
    vec3 center() const { return 0.5f * (lo + hi); }
    bool valid()  const { return lo.x <= hi.x; }

    float surfaceArea() const {
        if (!valid()) return 0.0f;
        const vec3 e = extent();
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    // Transform all 8 corners and refit (conservative world-space bounds).
    Aabb transformed(const mat4& m) const {
        Aabb out;
        for (int i = 0; i < 8; ++i) {
            const vec3 c((i & 1) ? hi.x : lo.x,
                         (i & 2) ? hi.y : lo.y,
                         (i & 4) ? hi.z : lo.z);
            out.expand(vec3(m * vec4(c, 1.0f)));
        }
        return out;
    }
};

// Row-major 3x4 (as used by the public API) -> column-major glm::mat4.
inline mat4 mat4FromRows3x4(const float r[12]) {
    mat4 m(1.0f);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            m[col][row] = r[row * 4 + col];
    return m;
}

} // namespace mrt
