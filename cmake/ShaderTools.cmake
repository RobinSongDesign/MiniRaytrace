# Offline GLSL -> SPIR-V compilation, embedded into the binary as C headers.
# Uses glslangValidator from the Vulkan SDK; compiled .spv files are also kept
# on disk so MRT_DEV_SHADER_RELOAD can hot-load them during development.

find_program(MRT_GLSLANG glslangValidator
    HINTS $ENV{VULKAN_SDK}
    PATH_SUFFIXES bin)
if(NOT MRT_GLSLANG)
    message(FATAL_ERROR "glslangValidator not found. Install the Vulkan SDK and set VULKAN_SDK.")
endif()

# mrt_compile_shaders(<out_var> SHADERS <files...> INCLUDES <glsl includes...>)
# Produces <out_var> = list of generated headers; also defines MRT_SHADER_BIN_DIR.
function(mrt_compile_shaders OUT_HEADERS)
    cmake_parse_arguments(ARG "" "" "SHADERS;INCLUDES" ${ARGN})
    set(spv_dir     ${CMAKE_BINARY_DIR}/shaders)
    set(header_dir  ${CMAKE_BINARY_DIR}/shaders/include)
    file(MAKE_DIRECTORY ${spv_dir} ${header_dir})

    set(headers "")
    foreach(src ${ARG_SHADERS})
        get_filename_component(name ${src} NAME_WE)
        get_filename_component(dir  ${src} DIRECTORY)
        set(spv    ${spv_dir}/${name}.spv)
        set(header ${header_dir}/${name}_spv.h)

        add_custom_command(
            OUTPUT  ${spv}
            COMMAND ${MRT_GLSLANG} -V --target-env vulkan1.2 -I${dir} -o ${spv} ${src}
            DEPENDS ${src} ${ARG_INCLUDES}
            COMMENT "GLSL -> SPIR-V: ${name}"
            VERBATIM)
        add_custom_command(
            OUTPUT  ${header}
            COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${header} -DVAR=g_${name}_spv
                    -P ${CMAKE_SOURCE_DIR}/cmake/Spv2Header.cmake
            DEPENDS ${spv} ${CMAKE_SOURCE_DIR}/cmake/Spv2Header.cmake
            COMMENT "Embedding ${name}.spv"
            VERBATIM)
        list(APPEND headers ${header})
    endforeach()

    set(${OUT_HEADERS} ${headers} PARENT_SCOPE)
    set(MRT_SHADER_HEADER_DIR ${header_dir} PARENT_SCOPE)
    set(MRT_SHADER_BIN_DIR    ${spv_dir}    PARENT_SCOPE)
endfunction()
