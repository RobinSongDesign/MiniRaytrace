include(FetchContent)
FetchContent_Declare(catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4)
FetchContent_MakeAvailable(catch2)

add_executable(mrt_tests
    bvh_tests.cpp)

# Tests reach into core internals (BVH builder), hence the src include.
target_include_directories(mrt_tests PRIVATE ${CMAKE_SOURCE_DIR}/core/src)
target_link_libraries(mrt_tests PRIVATE mrt_core Catch2::Catch2WithMain)

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(mrt_tests)
