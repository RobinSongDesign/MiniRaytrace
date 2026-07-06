# Convert a SPIR-V binary into a C header containing a uint32_t word array.
# SPIR-V words are little-endian on disk; bytes are swapped into host uint32 literals.
# Usage: cmake -DIN=<file.spv> -DOUT=<file.h> -DVAR=<symbol> -P Spv2Header.cmake

file(READ ${IN} hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR remainder "${hexlen} % 8")
if(NOT remainder EQUAL 0)
    message(FATAL_ERROR "SPIR-V size of ${IN} is not a multiple of 4 bytes")
endif()

# Each 8 hex chars = 4 bytes b0 b1 b2 b3 (little endian) -> 0x{b3}{b2}{b1}{b0}
string(REGEX MATCHALL "(..)(..)(..)(..)" groups "${hex}")
set(body "")
set(col 0)
foreach(g ${groups})
    string(SUBSTRING "${g}" 0 2 b0)
    string(SUBSTRING "${g}" 2 2 b1)
    string(SUBSTRING "${g}" 4 2 b2)
    string(SUBSTRING "${g}" 6 2 b3)
    string(APPEND body "0x${b3}${b2}${b1}${b0},")
    math(EXPR col "${col} + 1")
    if(col EQUAL 8)
        string(APPEND body "\n    ")
        set(col 0)
    endif()
endforeach()

file(WRITE ${OUT} "// Auto-generated from ${IN}. Do not edit.
#pragma once
#include <cstdint>
#include <cstddef>

static const uint32_t ${VAR}[] = {
    ${body}
};
static const size_t ${VAR}_count = sizeof(${VAR}) / sizeof(uint32_t);
")
