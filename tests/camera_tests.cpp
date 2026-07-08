// Extended camera model (PRD §8 A2): mrtCameraDescEx must reproduce the
// legacy symmetric-perspective path exactly, support orthographic (parallel)
// rays, and support asymmetric frustum slices consistent with a symmetric
// crop of the same scene.

#include <catch2/catch_test_macros.hpp>

#include "mrt/Engine.hpp"

#include <cmath>
#include <vector>

using namespace mrt;

namespace {

EngineDesc engineDesc(uint32_t w, uint32_t h) {
    EngineDesc d;
    d.enableValidation = true;
    d.settings.width = w;
    d.settings.height = h;
    d.settings.sppLimit = 1;
    d.settings.denoise = false;
    d.settings.maxBounces = 0; // emissive quads only: no GI needed, keeps this deterministic
    return d;
}

// A single quad, large enough to fill any frustum extent used below, facing
// +Z (towards a camera looking down -Z), purely emissive so every pixel that
// hits it reads back as exactly the emission color regardless of subpixel
// jitter or hit position (no lights, no NEE contribution, maxBounces == 0).
void addFullFrameEmissiveQuad(Engine& e, float z) {
    const float s = 1000.0f;
    const float positions[] = {
        -s, -s, z,   s, -s, z,   s, s, z,
        -s, -s, z,   s, s, z,   -s, s, z,
    };
    const uint32_t indices[] = { 0, 1, 2, 3, 4, 5 };
    MeshDesc md;
    md.vertexCount = 6;
    md.indexCount = 6;
    md.positions = positions;
    md.indices = indices;
    const MeshId mesh = e.scene().addMesh(md);

    MaterialDesc mat;
    mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = 0.0f;
    mat.emission[0] = 1.0f; mat.emission[1] = 0.5f; mat.emission[2] = 0.25f;
    const MaterialId matId = e.scene().addMaterial(mat);
    e.scene().addInstance(mesh, nullptr, matId);
}

std::vector<float> renderRgba32f(Engine& e, uint32_t w, uint32_t h) {
    FrameInfo info{};
    if (e.renderFrame(info) != Result::Success) return {};
    std::vector<float> buf(size_t(w) * h * 4);
    if (e.readFramebuffer(ReadbackFormat::RGBA32F_LINEAR, buf.data(), buf.size() * sizeof(float)) != Result::Success)
        return {};
    return buf;
}

} // namespace

TEST_CASE("symmetric CameraDescEx matches the legacy look-at API pixel-for-pixel", "[camera]") {
    constexpr uint32_t W = 48, H = 48;

    std::unique_ptr<Engine> legacy;
    REQUIRE(Engine::create(engineDesc(W, H), legacy) == Result::Success);
    addFullFrameEmissiveQuad(*legacy, -20.0f);
    CameraDesc camLegacy;
    camLegacy.position[2] = 0.0f;
    camLegacy.target[2] = -1.0f;
    camLegacy.fovYDeg = 90.0f; // tan(45deg) == 1 -> matches left/right/bottom/top below at aspect 1
    legacy->scene().setCamera(camLegacy);
    auto bufLegacy = renderRgba32f(*legacy, W, H);
    REQUIRE(!bufLegacy.empty());

    std::unique_ptr<Engine> ex;
    REQUIRE(Engine::create(engineDesc(W, H), ex) == Result::Success);
    addFullFrameEmissiveQuad(*ex, -20.0f);
    CameraDescEx camEx;
    camEx.projection = CameraProjection::Perspective;
    camEx.position[0] = 0; camEx.position[1] = 0; camEx.position[2] = 0;
    camEx.forward[0] = 0; camEx.forward[1] = 0; camEx.forward[2] = -1;
    camEx.up[0] = 0; camEx.up[1] = 1; camEx.up[2] = 0;
    camEx.left = -1.0f; camEx.right = 1.0f; camEx.bottom = -1.0f; camEx.top = 1.0f;
    ex->scene().setCameraEx(camEx);
    auto bufEx = renderRgba32f(*ex, W, H);
    REQUIRE(!bufEx.empty());

    CHECK(bufLegacy == bufEx);
}

TEST_CASE("orthographic camera renders identically regardless of eye distance", "[camera]") {
    constexpr uint32_t W = 32, H = 32;

    auto renderAt = [&](float eyeZ) {
        std::unique_ptr<Engine> e;
        REQUIRE(Engine::create(engineDesc(W, H), e) == Result::Success);
        addFullFrameEmissiveQuad(*e, -50.0f);
        CameraDescEx cam;
        cam.projection = CameraProjection::Orthographic;
        cam.position[0] = 0; cam.position[1] = 0; cam.position[2] = eyeZ;
        cam.forward[0] = 0; cam.forward[1] = 0; cam.forward[2] = -1;
        cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
        cam.left = -1.0f; cam.right = 1.0f; cam.bottom = -1.0f; cam.top = 1.0f;
        e->scene().setCameraEx(cam);
        return renderRgba32f(*e, W, H);
    };

    auto near = renderAt(0.0f);
    auto far = renderAt(30.0f); // moved back 30 units; a perspective camera would shrink the quad's coverage
    REQUIRE(!near.empty());
    REQUIRE(!far.empty());
    CHECK(near == far); // orthographic: parallel rays, no foreshortening with distance
}

TEST_CASE("asymmetric frustum crop matches the corresponding region of a symmetric render", "[camera]") {
    constexpr uint32_t Wbig = 64, H = 32;
    constexpr uint32_t Wcrop = 32;

    std::unique_ptr<Engine> big;
    REQUIRE(Engine::create(engineDesc(Wbig, H), big) == Result::Success);
    addFullFrameEmissiveQuad(*big, -20.0f);
    CameraDescEx camBig;
    camBig.projection = CameraProjection::Perspective;
    camBig.forward[2] = -1.0f; camBig.up[1] = 1.0f;
    camBig.left = -1.0f; camBig.right = 1.0f; camBig.bottom = -1.0f; camBig.top = 1.0f;
    big->scene().setCameraEx(camBig);
    auto bufBig = renderRgba32f(*big, Wbig, H);
    REQUIRE(!bufBig.empty());

    // Right half only: left = 0 instead of -1, same right/bottom/top.
    std::unique_ptr<Engine> crop;
    REQUIRE(Engine::create(engineDesc(Wcrop, H), crop) == Result::Success);
    addFullFrameEmissiveQuad(*crop, -20.0f);
    CameraDescEx camCrop = camBig;
    camCrop.left = 0.0f;
    crop->scene().setCameraEx(camCrop);
    auto bufCrop = renderRgba32f(*crop, Wcrop, H);
    REQUIRE(!bufCrop.empty());

    // bufBig's right half (columns [Wbig/2, Wbig)) should match bufCrop exactly:
    // the emissive quad fills the whole frame either way, so every sampled
    // pixel reads back as the constant emission color regardless of exactly
    // which world-space ray produced it.
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < Wcrop; ++x) {
            const size_t bigIdx = (size_t(y) * Wbig + (Wbig / 2 + x)) * 4;
            const size_t cropIdx = (size_t(y) * Wcrop + x) * 4;
            for (int c = 0; c < 3; ++c)
                CHECK(bufBig[bigIdx + c] == bufCrop[cropIdx + c]);
        }
    }
}
