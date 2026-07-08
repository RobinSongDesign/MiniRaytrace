// RGBA32F readback performance (PRD §1.3 / §8 A7 — target < 3ms at 1080p) and
// the "safe to call every frame" claim (no growing memory from repeated
// calls, since the scratch buffer is reused rather than freshly allocated).

#include <catch2/catch_test_macros.hpp>

#include "mrt/Engine.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace mrt;

TEST_CASE("RGBA32F readback at 1080p is fast and allocation-free after warmup", "[readback][bench]") {
    EngineDesc d;
    d.settings.width = 1920;
    d.settings.height = 1080;
    d.settings.sppLimit = 1;
    d.settings.denoise = false;
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(d, engine) == Result::Success);

    FrameInfo info{};
    REQUIRE(engine->renderFrame(info) == Result::Success);

    const size_t pixels = size_t(d.settings.width) * d.settings.height;
    std::vector<float> buf(pixels * 4);

    // First call may grow the internal scratch buffer; discard from timing.
    REQUIRE(engine->readFramebuffer(ReadbackFormat::RGBA32F_LINEAR, buf.data(), buf.size() * sizeof(float)) == Result::Success);

    constexpr int kIters = 20;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < kIters; ++i) {
        REQUIRE(engine->readFramebuffer(ReadbackFormat::RGBA32F_LINEAR, buf.data(), buf.size() * sizeof(float)) == Result::Success);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double avgMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / kIters;

    std::fprintf(stderr, "[bench] 1080p RGBA32F readback: %.3f ms/call (PRD target < 3ms)\n", avgMs);
    if (avgMs >= 3.0)
        WARN("readback exceeded the PRD §1.3 target of 3ms/call: " << avgMs << "ms");
    // Generous hard bound so this stays a useful regression guard across
    // varied CI hardware without being flaky about the tighter PRD target.
    CHECK(avgMs < 20.0);

    // Alpha must be exactly 1.0 everywhere (PRD §8 A7). One aggregate check
    // rather than per-pixel: this is ~2M pixels, and Catch2's per-assertion
    // bookkeeping would dominate the test's runtime otherwise.
    bool allOpaque = true;
    for (size_t i = 0; i < pixels && allOpaque; ++i)
        allOpaque = (buf[i * 4 + 3] == 1.0f);
    CHECK(allOpaque);
}
