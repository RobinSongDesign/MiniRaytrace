// BVH correctness: traversal through the built tree must find exactly the
// same closest hit as brute force over all triangles (PRD §12: CPU reference
// intersector cross-validation).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "accel/Bvh.hpp"

#include <random>

using namespace mrt;

namespace {

struct Tri { vec3 a, b, c; };

// Moeller-Trumbore, mirrors intersect.glsl.
float intersectTri(const Tri& tri, const vec3& o, const vec3& d, float tMax) {
    const vec3 e1 = tri.b - tri.a;
    const vec3 e2 = tri.c - tri.a;
    const vec3 p = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return -1.0f;
    const float invDet = 1.0f / det;
    const vec3 s = o - tri.a;
    const float u = glm::dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f) return -1.0f;
    const vec3 q = glm::cross(s, e1);
    const float v = glm::dot(d, q) * invDet;
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    const float t = glm::dot(e2, q) * invDet;
    return (t > 1e-4f && t < tMax) ? t : -1.0f;
}

bool slabTest(const BvhNode& n, const vec3& o, const vec3& invD, float tMax) {
    const vec3 lo(n.lo[0], n.lo[1], n.lo[2]);
    const vec3 hi(n.hi[0], n.hi[1], n.hi[2]);
    const vec3 t0 = (lo - o) * invD;
    const vec3 t1 = (hi - o) * invD;
    const vec3 tmin = glm::min(t0, t1);
    const vec3 tmax = glm::max(t0, t1);
    const float enter = std::max({ tmin.x, tmin.y, tmin.z, 0.0f });
    const float exit = std::min({ tmax.x, tmax.y, tmax.z, tMax });
    return enter <= exit;
}

// CPU traversal replicating the shader's BLAS walk.
int traverse(const BvhBuildResult& bvh, const std::vector<Tri>& trisInLeafOrder,
             const vec3& o, const vec3& d, float& tOut) {
    const vec3 invD = 1.0f / d;
    float tMax = 1e30f;
    int hitTri = -1;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const BvhNode& n = bvh.nodes[stack[--sp]];
        if (!slabTest(n, o, invD, tMax)) continue;
        const uint32_t count = n.packed >> 2;
        if (count == 0) {
            stack[sp++] = n.leftOrPrim;
            stack[sp++] = n.leftOrPrim + 1;
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                const float t = intersectTri(trisInLeafOrder[n.leftOrPrim + i], o, d, tMax);
                if (t > 0.0f) { tMax = t; hitTri = int(n.leftOrPrim + i); }
            }
        }
    }
    tOut = tMax;
    return hitTri;
}

std::vector<Tri> randomTriangles(size_t count, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> pos(-10.0f, 10.0f);
    std::uniform_real_distribution<float> size(0.05f, 0.8f);
    std::vector<Tri> tris(count);
    for (auto& t : tris) {
        const vec3 base(pos(rng), pos(rng), pos(rng));
        t.a = base;
        t.b = base + vec3(size(rng), size(rng), 0.0f);
        t.c = base + vec3(0.0f, size(rng), size(rng));
    }
    return tris;
}

} // namespace

TEST_CASE("BVH build produces valid topology", "[bvh]") {
    const auto tris = randomTriangles(5000, 42);
    std::vector<Aabb> bounds(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        bounds[i].expand(tris[i].a);
        bounds[i].expand(tris[i].b);
        bounds[i].expand(tris[i].c);
    }
    const auto bvh = BvhBuilder::build(bounds, 4);

    REQUIRE(bvh.primOrder.size() == tris.size());
    REQUIRE(!bvh.nodes.empty());

    // Every primitive appears exactly once across the leaves.
    std::vector<int> seen(tris.size(), 0);
    size_t leafPrims = 0;
    for (const auto& n : bvh.nodes) {
        const uint32_t count = n.packed >> 2;
        if (count == 0) {
            REQUIRE(n.leftOrPrim + 1 < bvh.nodes.size());
        } else {
            leafPrims += count;
            for (uint32_t i = 0; i < count; ++i)
                seen[bvh.primOrder[n.leftOrPrim + i]]++;
        }
    }
    REQUIRE(leafPrims == tris.size());
    for (int s : seen) REQUIRE(s == 1);
}

TEST_CASE("BVH traversal matches brute force", "[bvh]") {
    const auto tris = randomTriangles(2000, 7);
    std::vector<Aabb> bounds(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        bounds[i].expand(tris[i].a);
        bounds[i].expand(tris[i].b);
        bounds[i].expand(tris[i].c);
    }
    const auto bvh = BvhBuilder::build(bounds, 4);

    // Triangles in leaf order, as the renderer packs them.
    std::vector<Tri> leafOrder(tris.size());
    for (size_t i = 0; i < tris.size(); ++i)
        leafOrder[i] = tris[bvh.primOrder[i]];

    std::mt19937 rng(123);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int hits = 0;
    for (int i = 0; i < 2000; ++i) {
        const vec3 o(u(rng) * 15.0f, u(rng) * 15.0f, u(rng) * 15.0f);
        vec3 d(u(rng), u(rng), u(rng));
        if (glm::length(d) < 1e-3f) d = vec3(1, 0, 0);
        d = glm::normalize(d);

        // Brute force reference over the original triangle order.
        float tRef = 1e30f;
        bool hitRef = false;
        for (const auto& tri : tris) {
            const float t = intersectTri(tri, o, d, tRef);
            if (t > 0.0f) { tRef = t; hitRef = true; }
        }

        float tBvh = 1e30f;
        const int hitBvh = traverse(bvh, leafOrder, o, d, tBvh);

        REQUIRE((hitBvh >= 0) == hitRef);
        if (hitRef) {
            REQUIRE(tBvh == Catch::Approx(tRef).epsilon(1e-4));
            ++hits;
        }
    }
    REQUIRE(hits > 50); // sanity: the scene actually got hit a meaningful number of times
}

TEST_CASE("BVH handles degenerate inputs", "[bvh]") {
    SECTION("empty input") {
        const auto bvh = BvhBuilder::build({}, 4);
        REQUIRE(bvh.nodes.empty());
        REQUIRE(bvh.primOrder.empty());
    }
    SECTION("single primitive") {
        Aabb b;
        b.expand(vec3(0));
        b.expand(vec3(1));
        const auto bvh = BvhBuilder::build({ b }, 4);
        REQUIRE(bvh.nodes.size() == 1);
        REQUIRE((bvh.nodes[0].packed >> 2) == 1);
    }
    SECTION("all primitives at the same point") {
        Aabb b;
        b.expand(vec3(1, 2, 3));
        const std::vector<Aabb> bounds(100, b);
        const auto bvh = BvhBuilder::build(bounds, 4);
        size_t leafPrims = 0;
        for (const auto& n : bvh.nodes) leafPrims += n.packed >> 2;
        REQUIRE(leafPrims == 100);
    }
}
