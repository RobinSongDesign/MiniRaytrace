// Material/texture removal + slot recycling (PRD §8 A3): removed textures
// must not leak GPU slots, and instances/materials referencing removed
// handles must render safely instead of crashing or reading stale memory.

#include <catch2/catch_test_macros.hpp>

#include "mrt/Engine.hpp"

#include <vector>

using namespace mrt;

namespace {

EngineDesc smallEngineDesc() {
    EngineDesc d;
    d.enableValidation = true;
    d.settings.width = 32;
    d.settings.height = 32;
    d.settings.sppLimit = 2;
    d.settings.denoise = false;
    return d;
}

TextureDesc onePixelTexture(uint8_t r, uint8_t g, uint8_t b) {
    static thread_local uint8_t px[4];
    px[0] = r; px[1] = g; px[2] = b; px[3] = 255;
    TextureDesc t;
    t.width = 1;
    t.height = 1;
    t.format = TextureFormat::RGBA8_UNORM;
    t.pixels = px;
    t.generateMips = false;
    return t;
}

void addTriangleWithMaterial(Engine& e, MaterialId mat) {
    static const float positions[] = {
        -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f,
    };
    static const uint32_t indices[] = { 0, 1, 2 };
    MeshDesc md;
    md.vertexCount = 3;
    md.indexCount = 3;
    md.positions = positions;
    md.indices = indices;
    const MeshId mesh = e.scene().addMesh(md);
    e.scene().addInstance(mesh, nullptr, mat);
}

} // namespace

TEST_CASE("removed material falls back to default without crashing", "[material][texture]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);

    MaterialDesc md;
    md.baseColor[0] = 1.0f; md.baseColor[1] = 0.0f; md.baseColor[2] = 0.0f;
    const MaterialId mat = engine->scene().addMaterial(md);
    addTriangleWithMaterial(*engine, mat);
    REQUIRE(engine->commitScene(nullptr) == Result::Success);

    engine->scene().removeMaterial(mat);

    FrameInfo info{};
    REQUIRE(engine->renderFrame(info) == Result::Success);
}

TEST_CASE("removed texture slot is recycled and material falls back safely", "[material][texture]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);

    const TextureId tex1 = engine->scene().addTexture(onePixelTexture(255, 0, 0));
    REQUIRE(tex1 != kInvalidId);
    const uint32_t slot1 = engine->scene().textureSlot(tex1);

    engine->scene().removeTexture(tex1);
    CHECK(engine->scene().textureSlot(tex1) == ~0u);

    const TextureId tex2 = engine->scene().addTexture(onePixelTexture(0, 255, 0));
    const uint32_t slot2 = engine->scene().textureSlot(tex2);
    CHECK(slot2 == slot1); // slot recycled, not grown

    MaterialDesc md;
    md.baseColorTex = tex2;
    const MaterialId mat = engine->scene().addMaterial(md);
    addTriangleWithMaterial(*engine, mat);

    FrameInfo info{};
    REQUIRE(engine->commitScene(nullptr) == Result::Success);
    REQUIRE(engine->renderFrame(info) == Result::Success);

    // Now remove tex2 too, while a material still references it: must still
    // render (falls back to the dummy/constant texture at that slot).
    engine->scene().removeTexture(tex2);
    REQUIRE(engine->commitScene(nullptr) == Result::Success);
    REQUIRE(engine->renderFrame(info) == Result::Success);
}

TEST_CASE("repeated add/remove keeps texture slot count bounded", "[material][texture]") {
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(smallEngineDesc(), engine) == Result::Success);

    // Prime one texture so slot 0 exists, then thrash add/remove: the slot
    // count (scene.textures().size()) must never grow past the high-water
    // mark of concurrently-alive textures (here: 1).
    TextureId prev = engine->scene().addTexture(onePixelTexture(10, 20, 30));
    REQUIRE(engine->commitScene(nullptr) == Result::Success);
    const size_t initialSlotCount = engine->scene().textures().size();

    for (int i = 0; i < 300; ++i) {
        engine->scene().removeTexture(prev);
        prev = engine->scene().addTexture(onePixelTexture(uint8_t(i), 0, 0));
        REQUIRE(prev != kInvalidId);
        if (i % 37 == 0) REQUIRE(engine->commitScene(nullptr) == Result::Success);
    }
    REQUIRE(engine->commitScene(nullptr) == Result::Success);

    CHECK(engine->scene().textures().size() == initialSlotCount);

    FrameInfo info{};
    REQUIRE(engine->renderFrame(info) == Result::Success);
}
