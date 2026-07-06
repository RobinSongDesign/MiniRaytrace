// Binned SAH builder. Single-threaded per BVH; Renderer builds multiple
// BLASes concurrently via std::async, which is where the parallelism pays off.

#include "Bvh.hpp"

#include <algorithm>
#include <numeric>

namespace mrt {

namespace {

constexpr uint32_t kNumBins = 32;
constexpr float    kTraversalCost = 1.0f;
constexpr float    kIntersectCost = 1.5f;

struct PrimRef {
    Aabb bounds;
    vec3 centroid;
    uint32_t index;
};

struct BuildContext {
    std::vector<PrimRef>& prims;
    std::vector<BvhNode>& nodes;
    uint32_t maxLeafSize;
};

Aabb boundsOf(const std::vector<PrimRef>& prims, uint32_t begin, uint32_t end) {
    Aabb b;
    for (uint32_t i = begin; i < end; ++i) b.expand(prims[i].bounds);
    return b;
}

void makeLeaf(BvhNode& node, uint32_t begin, uint32_t count, uint32_t axis) {
    node.leftOrPrim = begin;
    node.packed = (count << 2) | axis;
}

// Recursively build [begin, end) into nodes[nodeIndex].
void buildRange(BuildContext& ctx, uint32_t nodeIndex, uint32_t begin, uint32_t end) {
    auto& prims = ctx.prims;
    const uint32_t count = end - begin;

    const Aabb bounds = boundsOf(prims, begin, end);
    ctx.nodes[nodeIndex].setBounds(bounds);

    Aabb centroidBounds;
    for (uint32_t i = begin; i < end; ++i) centroidBounds.expand(prims[i].centroid);
    const vec3 cExtent = centroidBounds.extent();

    // Pick the widest centroid axis.
    uint32_t axis = 0;
    if (cExtent.y > cExtent.x) axis = 1;
    if (cExtent.z > cExtent[axis]) axis = 2;

    if (count <= ctx.maxLeafSize || cExtent[axis] < 1e-8f) {
        makeLeaf(ctx.nodes[nodeIndex], begin, count, axis);
        return;
    }

    // --- binned SAH along `axis` ---
    struct Bin { Aabb bounds; uint32_t count = 0; };
    Bin bins[kNumBins];
    const float k = kNumBins * (1.0f - 1e-6f) / cExtent[axis];
    const float origin = centroidBounds.lo[axis];

    for (uint32_t i = begin; i < end; ++i) {
        uint32_t b = static_cast<uint32_t>(k * (prims[i].centroid[axis] - origin));
        bins[b].bounds.expand(prims[i].bounds);
        bins[b].count++;
    }

    // Sweep: suffix areas/counts right-to-left, then prefix left-to-right.
    float rightArea[kNumBins];
    uint32_t rightCount[kNumBins];
    {
        Aabb acc; uint32_t cnt = 0;
        for (int i = kNumBins - 1; i > 0; --i) {
            acc.expand(bins[i].bounds);
            cnt += bins[i].count;
            rightArea[i]  = acc.surfaceArea();
            rightCount[i] = cnt;
        }
    }

    float bestCost = kIntersectCost * count; // cost of making this a leaf
    int bestSplit = -1;
    {
        Aabb acc; uint32_t cnt = 0;
        const float invRootArea = 1.0f / std::max(bounds.surfaceArea(), 1e-12f);
        for (uint32_t i = 0; i < kNumBins - 1; ++i) {
            acc.expand(bins[i].bounds);
            cnt += bins[i].count;
            if (cnt == 0 || rightCount[i + 1] == 0) continue;
            const float cost = kTraversalCost + kIntersectCost * invRootArea *
                (acc.surfaceArea() * cnt + rightArea[i + 1] * rightCount[i + 1]);
            if (cost < bestCost) { bestCost = cost; bestSplit = static_cast<int>(i); }
        }
    }

    uint32_t mid;
    if (bestSplit < 0) {
        if (count <= ctx.maxLeafSize * 2) { // SAH says leaf and it's small enough
            makeLeaf(ctx.nodes[nodeIndex], begin, count, axis);
            return;
        }
        // Forced median split to bound leaf size.
        mid = begin + count / 2;
        std::nth_element(prims.begin() + begin, prims.begin() + mid, prims.begin() + end,
                         [axis](const PrimRef& a, const PrimRef& b) {
                             return a.centroid[axis] < b.centroid[axis];
                         });
    } else {
        auto it = std::partition(prims.begin() + begin, prims.begin() + end,
                                 [&](const PrimRef& p) {
                                     uint32_t b = static_cast<uint32_t>(k * (p.centroid[axis] - origin));
                                     return b <= static_cast<uint32_t>(bestSplit);
                                 });
        mid = static_cast<uint32_t>(it - prims.begin());
        if (mid == begin || mid == end) mid = begin + count / 2; // degenerate partition guard
    }

    // Children are allocated as a pair; traversal computes right = left + 1.
    const uint32_t left = static_cast<uint32_t>(ctx.nodes.size());
    ctx.nodes.emplace_back();
    ctx.nodes.emplace_back();
    ctx.nodes[nodeIndex].leftOrPrim = left;
    ctx.nodes[nodeIndex].packed = axis; // count == 0 -> internal

    buildRange(ctx, left,     begin, mid);
    buildRange(ctx, left + 1, mid,   end);
}

} // namespace

BvhBuildResult BvhBuilder::build(const std::vector<Aabb>& primBounds, uint32_t maxLeafSize) {
    BvhBuildResult out;
    const uint32_t count = static_cast<uint32_t>(primBounds.size());
    if (count == 0) return out;

    std::vector<PrimRef> prims(count);
    for (uint32_t i = 0; i < count; ++i)
        prims[i] = { primBounds[i], primBounds[i].center(), i };

    out.nodes.reserve(2 * count);
    out.nodes.emplace_back();

    BuildContext ctx{ prims, out.nodes, std::max(1u, maxLeafSize) };
    buildRange(ctx, 0, 0, count);

    out.primOrder.resize(count);
    for (uint32_t i = 0; i < count; ++i) out.primOrder[i] = prims[i].index;
    out.rootBounds = boundsOf(prims, 0, count);
    return out;
}

} // namespace mrt
