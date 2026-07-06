# libmrt: shared library exposing the stable C ABI (consumed by the Rhino
# plugin via P/Invoke).

add_library(mrt SHARED src/capi.cpp)

target_include_directories(mrt PUBLIC include)
target_link_libraries(mrt PRIVATE mrt_core)

set_target_properties(mrt PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME mrt)
