include(FetchContent)
FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4)
FetchContent_MakeAvailable(catch2)

add_executable(mrt_tests
    bvh_tests.cpp
    commit_tests.cpp
    material_texture_tests.cpp
    camera_tests.cpp
    gpu_selection_tests.cpp
    readback_bench_tests.cpp
    tiling_regression_tests.cpp)

# Tests reach into core internals (BVH builder, raw Vulkan via volk for the
# GPU-selection tests), hence the src include and direct volk link — the
# same "interop" exception the viewer relies on (PRD §8 A4).
target_include_directories(mrt_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(mrt_tests PRIVATE mrt_core volk Catch2::Catch2WithMain)

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(mrt_tests)
