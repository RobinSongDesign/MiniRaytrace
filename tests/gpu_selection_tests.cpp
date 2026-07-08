// volk runtime loading + GPU preference (PRD §8 A4). Needs a real Vulkan
// device; on a hybrid-graphics machine (confirmed here: NVIDIA discrete +
// AMD integrated) this exercises the actual selection logic end to end
// instead of just compiling it.

#include <catch2/catch_test_macros.hpp>

#include "mrt/Engine.hpp"

// volk is already initialized (global function pointers loaded) by the time
// any Engine exists in this process, so calling straight into the Vulkan API
// via the handle Engine exposes for viewer interop is safe here.
#include <volk.h>

#include <string>
#include <vector>

using namespace mrt;

TEST_CASE("auto GPU selection prefers the discrete adapter", "[gpu]") {
    EngineDesc d;
    d.settings.width = 16;
    d.settings.height = 16;
    // gpuIndex left at its default (-1 = auto).
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(d, engine) == Result::Success);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(reinterpret_cast<VkPhysicalDevice>(engine->vkPhysicalDevice()), &props);
    CHECK(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
}

TEST_CASE("explicit gpuIndex forces device selection", "[gpu]") {
    // Enumerate directly to find how many devices exist and pick the LAST
    // one deliberately (on this dev box: index 1 = AMD iGPU) rather than
    // hardcoding an assumption about ordering.
    EngineDesc probe;
    probe.settings.width = 16;
    probe.settings.height = 16;
    std::unique_ptr<Engine> auto_engine;
    REQUIRE(Engine::create(probe, auto_engine) == Result::Success);
    VkInstance instance = reinterpret_cast<VkInstance>(auto_engine->vkInstance());

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count < 2) {
        WARN("Only one Vulkan device present; skipping explicit-index selection check");
        return;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    const uint32_t targetIndex = count - 1;
    VkPhysicalDeviceProperties expected{};
    // Query properties while `instance` (and thus these VkPhysicalDevice
    // handles) is still alive — they're invalidated once it's destroyed.
    vkGetPhysicalDeviceProperties(devices[targetIndex], &expected);
    auto_engine.reset();

    EngineDesc d;
    d.settings.width = 16;
    d.settings.height = 16;
    d.gpuIndex = static_cast<int32_t>(targetIndex);
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(d, engine) == Result::Success);

    VkPhysicalDeviceProperties actual{};
    vkGetPhysicalDeviceProperties(reinterpret_cast<VkPhysicalDevice>(engine->vkPhysicalDevice()), &actual);
    CHECK(std::string(actual.deviceName) == std::string(expected.deviceName));
}

TEST_CASE("out-of-range gpuIndex fails clearly instead of silently falling back", "[gpu]") {
    EngineDesc d;
    d.settings.width = 16;
    d.settings.height = 16;
    d.gpuIndex = 9999;
    std::unique_ptr<Engine> engine;
    REQUIRE(Engine::create(d, engine) == Result::ErrorVulkanInit);
    CHECK(lastErrorMessage().find("out of range") != std::string::npos);
}
