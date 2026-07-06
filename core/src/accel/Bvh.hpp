#pragma once
// Binned-SAH BVH2 builder (PRD §4). Used for both BLAS (triangles) and
// TLAS (instance AABBs). Node layout matches the GLSL traversal exactly.

#include "mrt/MathTypes.hpp"

#include <cstdint>
#include <vector>

namespace mrt {

// 32 bytes; mirrored in common.glsl. Internal node: leftOrPrim = left child
// index (right = left + 1), count == 0. Leaf: leftOrPrim = first primitive,
// count = primitive count. packed = (count << 2) | splitAxis.
struct BvhNode {
    float    lo[3];
    uint32_t leftOrPrim;
    float    hi[3];
    uint32_t packed;

    void setBounds(const Aabb& b) {
        lo[0] = b.lo.x; lo[1] = b.lo.y; lo[2] = b.lo.z;
        hi[0] = b.hi.x; hi[1] = b.hi.y; hi[2] = b.hi.z;
    }
};
static_assert(sizeof(BvhNode) == 32, "BvhNode must match GLSL layout");

struct BvhBuildResult {
    std::vector<BvhNode>  nodes;     // node 0 is the root
    std::vector<uint32_t> primOrder; // leaf prims reference this reordering
    Aabb                  rootBounds;
};

class BvhBuilder {
public:
    // Generic build over primitive bounds. maxLeafSize: 4 for BLAS, 1 for TLAS.
    static BvhBuildResult build(const std::vector<Aabb>& primBounds, uint32_t maxLeafSize);
};

} // namespace mrt
