# mrt_core: the embeddable renderer library (no window / IO dependencies).

set(MRT_SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pathtrace.comp
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/resolve.comp)
set(MRT_SHADER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/common.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/random.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/intersect.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sampling.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bsdf.glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/lights.glsl)
# sampling.glsl is optional; prune missing entries so DEPENDS stays valid.
set(_includes "")
foreach(f ${MRT_SHADER_INCLUDES})
    if(EXISTS ${f})
        list(APPEND _includes ${f})
    endif()
endforeach()

mrt_compile_shaders(MRT_GENERATED_HEADERS
    SHADERS  ${MRT_SHADER_SOURCES}
    INCLUDES ${_includes})

add_library(mrt_core STATIC
    src/gpu/GpuContext.cpp
    src/accel/Bvh.cpp
    src/render/Renderer.cpp
    src/post/Denoiser.cpp
    src/scene/Scene.cpp
    src/Engine.cpp
    ${MRT_GENERATED_HEADERS})

target_include_directories(mrt_core
    PUBLIC  include
    PRIVATE src ${MRT_SHADER_HEADER_DIR})

find_package(Threads REQUIRED)
target_link_libraries(mrt_core
    PUBLIC  glm::glm
    PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator Threads::Threads)

if(MRT_ENABLE_OIDN)
    target_link_libraries(mrt_core PRIVATE OpenImageDenoise)
    target_compile_definitions(mrt_core PRIVATE MRT_ENABLE_OIDN)
endif()

if(MRT_DEV_SHADER_RELOAD)
    target_compile_definitions(mrt_core PRIVATE
        MRT_DEV_SHADER_RELOAD
        MRT_SHADER_DIR="${MRT_SHADER_BIN_DIR}")
endif()
