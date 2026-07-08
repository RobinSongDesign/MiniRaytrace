# Dear ImGui (no upstream CMake; build as a small static lib).
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8)
FetchContent_MakeAvailable(imgui)

add_library(imgui_vk STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_vk PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
# volk, not Vulkan::Vulkan: mrt_core (which mrt_viewer links) already brings
# in volk's global vk* symbols, and those collide with the loader import
# library's same-named symbols if both are linked into one binary. imgui's
# Vulkan backend has first-party volk support via this define.
target_compile_definitions(imgui_vk PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui_vk PUBLIC glfw volk)

add_executable(mrt_viewer
    src/main.cpp
    src/Window.cpp
    src/ObjLoader.cpp
    src/Gui.cpp)

target_link_libraries(mrt_viewer
    PRIVATE mrt_core glfw volk tinyobjloader stb_image imgui_vk)

# ---- self-contained macOS .app (no Vulkan SDK needed at runtime) ------------
# Bundles the Vulkan loader + MoltenVK; the loader discovers the ICD via the
# bundle's Resources/vulkan/icd.d directory (standard macOS loader search path).
option(MRT_MACOS_BUNDLE "Build mrt_viewer as a self-contained macOS .app" OFF)

if(APPLE AND MRT_MACOS_BUNDLE)
    set_target_properties(mrt_viewer PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "MiniRaytrace"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "MiniRaytrace"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.robinsong.miniraytrace"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        BUILD_RPATH "@executable_path/../Frameworks")

    get_filename_component(_vk_libdir ${Vulkan_LIBRARY} DIRECTORY)
    set(_app $<TARGET_BUNDLE_DIR:mrt_viewer>)

    add_custom_command(TARGET mrt_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${_app}/Contents/Frameworks
                ${_app}/Contents/Resources/vulkan/icd.d
        COMMAND ${CMAKE_COMMAND} -E copy
                ${_vk_libdir}/libvulkan.1.dylib ${_app}/Contents/Frameworks/
        COMMAND ${CMAKE_COMMAND} -E copy
                ${_vk_libdir}/libMoltenVK.dylib ${_app}/Contents/Frameworks/
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/packaging/MoltenVK_icd.json
                ${_app}/Contents/Resources/vulkan/icd.d/
        # Ad-hoc signature: required on Apple Silicon for local launch.
        COMMAND codesign --force --deep --sign - ${_app}
        COMMENT "Packaging self-contained MiniRaytrace.app"
        VERBATIM)
endif()
