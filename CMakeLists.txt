cmake_minimum_required(VERSION 3.24)
project(MiniRaytrace VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MRT_BUILD_VIEWER      "Build the standalone GLFW viewer"                       ON)
option(MRT_BUILD_TESTS       "Build unit tests"                                       OFF)
option(MRT_ENABLE_OIDN       "Enable Intel Open Image Denoise (needs OIDN findable)"  OFF)
option(MRT_DEV_SHADER_RELOAD "Load SPIR-V from build dir at runtime (dev iteration)"  OFF)

# Vulkan SDK is the only environment prerequisite (ships MoltenVK + glslangValidator).
find_package(Vulkan REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0)
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG        v2.0.0rc13)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5)
# volk: runtime Vulkan loader (PRD §8 A4). mrt_core links this instead of the
# loader import library, so libmrt has zero load-time Vulkan dependency —
# see docs/rhino-integration-design.md §6.
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        1.3.270)
FetchContent_MakeAvailable(glm vma tinyobjloader stb volk)

# stb has no CMake build; expose it as an interface target.
add_library(stb_image INTERFACE)
target_include_directories(stb_image INTERFACE ${stb_SOURCE_DIR})

if(MRT_BUILD_VIEWER)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4)
    FetchContent_MakeAvailable(glfw)
endif()

if(MRT_ENABLE_OIDN)
    # Expects an installed OIDN (e.g. `brew install open-image-denoise` or the
    # official prebuilt package with OpenImageDenoiseConfig.cmake on CMAKE_PREFIX_PATH).
    find_package(OpenImageDenoise REQUIRED)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShaderTools.cmake)

add_subdirectory(core)
add_subdirectory(capi)
if(MRT_BUILD_VIEWER)
    add_subdirectory(viewer)
endif()
if(MRT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
