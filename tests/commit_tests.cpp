// mrtCommitScene / Engine::commitScene correctness (PRD §8 A1). Needs a real
// Vulkan device — same requirement as the rest of the GPU-backed test plan
// (PRD §12: golden-image regression).

#include <catch2/catch_test_macros.hpp>

#include "mrt/Engine.hpp"

#include <vector>

using namespace mrt;

namespace {

EngineDesc smallEngineDesc() {
    EngineDesc d;
    d.enableValidation = true;
    d.settings.width = 64;
    d.settings.height = 64;
    d.settings.sppLimit = 4;
    d.settings.denoise = false;
    return d;
}

// Single triangle, default material, identity transform.
void addTriangle(Engine& e) {
    static const float positions[] = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    };
    static const uint32_t indices[] = { 0, 1, 2 };
    MeshDesc md;
    md.vertexCount = 3;
    md.indexCount = 3;
    md.positions = positions;
    md.indices = indices;
    const MeshId mesh = e.scene().addMesh(md);
    e.scene().addInstance(mesh, nullptr, kInvalidId);
}

} // namespace

TEST_CASE("commitScene is a no-op with nothing dirty", "[commit]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);

    CommitStats stats{};
    REQUIRE(engine->commitScene(&stats) == Result::Success);
    CHECK(stats.blasRebuilt == 0);
    CHECK(stats.tlasRebuilt == false);
}

TEST_CASE("commitScene rebuilds BLAS/TLAS exactly once per geometry change", "[commit]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);

    addTriangle(*engine);

    CommitStats stats{};
    REQUIRE(engine->commitScene(&stats) == Result::Success);
    CHECK(stats.blasRebuilt == 1);
    CHECK(stats.tlasRebuilt == true);

    // Second call, nothing changed since: must be a cheap no-op.
    CommitStats stats2{};
    REQUIRE(engine->commitScene(&stats2) == Result::Success);
    CHECK(stats2.blasRebuilt == 0);
    CHECK(stats2.tlasRebuilt == false);
}

TEST_CASE("camera-only change is a lightweight commit (no BLAS/TLAS rebuild)", "[commit]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);
    addTriangle(*engine);
    REQUIRE(engine->commitScene(nullptr) == Result::Success);

    CameraDesc cam;
    cam.position[2] = 6.0f;
    engine->scene().setCamera(cam);

    CommitStats stats{};
    REQUIRE(engine->commitScene(&stats) == Result::Success);
    CHECK(stats.blasRebuilt == 0);
    CHECK(stats.tlasRebuilt == false);
}

TEST_CASE("explicit commit + render matches implicit commit via renderFrame", "[commit]") {
    // Engine A: explicit mrtCommitScene() then renderFrame().
    std::unique_ptr<Engine> a;
    REQUIRE(Engine::create(smallEngineDesc(), a) == Result::Success);
    addTriangle(*a);
    REQUIRE(a->commitScene(nullptr) == Result::Success);
    FrameInfo infoA{};
    REQUIRE(a->renderFrame(infoA) == Result::Success);

    // Engine B: renderFrame() alone, relying on its implicit commit.
    std::unique_ptr<Engine> b;
    REQUIRE(Engine::create(smallEngineDesc(), b) == Result::Success);
    addTriangle(*b);
    FrameInfo infoB{};
    REQUIRE(b->renderFrame(infoB) == Result::Success);

    CHECK(infoA.spp == infoB.spp);

    const size_t pixels = 64 * 64;
    std::vector<float> bufA(pixels * 4), bufB(pixels * 4);
    REQUIRE(a->readFramebuffer(ReadbackFormat::RGBA32F_LINEAR, bufA.data(), bufA.size() * sizeof(float)) == Result::Success);
    REQUIRE(b->readFramebuffer(ReadbackFormat::RGBA32F_LINEAR, bufB.data(), bufB.size() * sizeof(float)) == Result::Success);
    CHECK(bufA == bufB);
}
