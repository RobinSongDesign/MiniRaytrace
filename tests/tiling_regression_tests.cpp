// Regression test for a real tiled-dispatch bug (PRD §8 A6): the first
// vkCmdDispatchBase-based implementation silently produced a black image
// for any geometry that didn't fill the entire frame, because non-zero
// dispatch bases didn't behave as documented on tested hardware despite
// VK_PIPELINE_CREATE_DISPATCH_BASE_BIT being set correctly. Fixed by driving
// the tile row offset through a push constant instead (see pathtrace.comp
// and Renderer::renderFrame). Earlier tests missed this because they either
// used maxBounces=0 with a frame-filling quad (camera_tests.cpp) or never
// checked pixel values at all (commit_tests.cpp, material_texture_tests.cpp).
#include <catch2/catch_test_macros.hpp>
#include "mrt/Engine.hpp"
#include <algorithm>
#include <vector>

using namespace mrt;

TEST_CASE("a small emissive triangle not filling the frame is still visible", "[regression][tiling]") {
    EngineDesc d;
    d.enableValidation = true;
    d.settings.width = 64;
    d.settings.height = 64;
    d.settings.sppLimit = 4;
    d.settings.denoise = false;
    // maxBounces intentionally left at its default (8, not 0): the bug only
    // manifested with the real megakernel loop, not the maxBounces=0
    // short-circuit used by camera_tests.cpp's frame-filling-quad helpers.
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(d, engine) == Result::Success);

    static const float positions[] = {
        -0.6f, -0.6f, 0.0f,
         0.6f, -0.6f, 0.0f,
         0.0f,  0.6f, 0.0f,
    };
    static const uint32_t indices[] = { 0, 1, 2 };
    MeshDesc md;
    md.vertexCount = 3;
    md.indexCount = 3;
    md.positions = positions;
    md.indices = indices;
    MeshId mesh = engine->scene().addMesh(md);

    MaterialDesc mat;
    mat.baseColor[0] = 1.0f; mat.baseColor[1] = 0.2f; mat.baseColor[2] = 0.1f;
    mat.emission[0] = 2.0f; mat.emission[1] = 0.4f; mat.emission[2] = 0.2f;
    MaterialId matId = engine->scene().addMaterial(mat);
    engine->scene().addInstance(mesh, nullptr, matId);

    CameraDesc cam;
    cam.position[0] = 0; cam.position[1] = 0; cam.position[2] = 3;
    cam.target[0] = 0; cam.target[1] = 0; cam.target[2] = 0;
    cam.up[0] = 0; cam.up[1] = 1; cam.up[2] = 0;
    cam.fovYDeg = 45.0f;
    engine->scene().setCamera(cam);

    REQUIRE(engine->commitScene(nullptr) == Result::Success);

    FrameInfo info{};
    for (int i = 0; i < 4; ++i) REQUIRE(engine->renderFrame(info) == Result::Success);

    std::vector<uint8_t> rgba8(64 * 64 * 4);
    REQUIRE(engine->readFramebuffer(ReadbackFormat::RGBA8_SRGB, rgba8.data(), rgba8.size()) == Result::Success);

    // The triangle covers only the middle rows of the frame, spanning at
    // least two dispatch tiles (kTileCount=4, 8 workgroup-rows total) — this
    // is exactly the configuration the original bug produced all-black for.
    int maxR = 0;
    for (size_t i = 0; i < rgba8.size(); i += 4) maxR = std::max(maxR, int(rgba8[i]));
    CHECK(maxR > 10);
}
