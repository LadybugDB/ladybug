add_subdirectory(aggregate)
add_subdirectory(arithmetic)
add_subdirectory(array)
add_subdirectory(cast)
add_subdirectory(date)
add_subdirectory(gds)
add_subdirectory(list)
add_subdirectory(map)
add_subdirectory(path)
add_subdirectory(pattern)
add_subdirectory(sequence)
add_subdirectory(struct)
add_subdirectory(table)
add_subdirectory(union)
add_subdirectory(utility)
add_subdirectory(uuid)
add_subdirectory(string)
add_subdirectory(export)
add_subdirectory(internal_id)
add_subdirectory(timestamp)

set(LBUG_FUNCTION_SIMD_SOURCES simd_filter.cpp)
set(LBUG_BUILD_FILTER_AVX2 FALSE)
set(LBUG_BUILD_FILTER_NEON FALSE)
if(LBUG_ENABLE_SIMD AND NOT BUILD_WASM)
    if(OS_ARCH STREQUAL "amd64")
        if(MSVC)
            set(LBUG_BUILD_FILTER_AVX2 TRUE)
            set_source_files_properties(simd_filter_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            include(CheckCXXCompilerFlag)
            check_cxx_compiler_flag("-mavx2" LBUG_COMPILER_SUPPORTS_MAVX2)
            if(LBUG_COMPILER_SUPPORTS_MAVX2)
                set(LBUG_BUILD_FILTER_AVX2 TRUE)
                set_source_files_properties(simd_filter_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
            endif()
        endif()
    elseif(OS_ARCH STREQUAL "arm64")
        if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            set(LBUG_BUILD_FILTER_NEON TRUE)
        endif()
    endif()
endif()

if(LBUG_BUILD_FILTER_AVX2)
    list(APPEND LBUG_FUNCTION_SIMD_SOURCES simd_filter_avx2.cpp)
    set_source_files_properties(simd_filter.cpp
        PROPERTIES COMPILE_DEFINITIONS LBUG_SIMD_COMPILED_AVX2)
    message(STATUS "Query processor SIMD: AVX2 filter kernels enabled")
elseif(LBUG_BUILD_FILTER_NEON)
    list(APPEND LBUG_FUNCTION_SIMD_SOURCES simd_filter_neon.cpp)
    set_source_files_properties(simd_filter.cpp
        PROPERTIES COMPILE_DEFINITIONS LBUG_SIMD_COMPILED_NEON)
    message(STATUS "Query processor SIMD: NEON filter kernels enabled")
else()
    message(STATUS "Query processor SIMD: scalar filter path only")
endif()

add_library(lbug_function
        OBJECT
        aggregate_function.cpp
        base_lower_upper_operation.cpp
        built_in_function_utils.cpp
        cast_string_non_nested_functions.cpp
        cast_from_string_functions.cpp
        comparison_functions.cpp
        find_function.cpp
        function.cpp
        function_collection.cpp
        scalar_macro_function.cpp
        vector_arithmetic_functions.cpp
        vector_boolean_functions.cpp
        vector_cast_functions.cpp
        vector_date_functions.cpp
        vector_hash_functions.cpp
        vector_null_functions.cpp
        vector_node_rel_functions.cpp
        vector_string_functions.cpp
        vector_timestamp_functions.cpp
        vector_blob_functions.cpp
        vector_uuid_functions.cpp
        ${LBUG_FUNCTION_SIMD_SOURCES})

set(ALL_OBJECT_FILES
        ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:lbug_function>
        PARENT_SCOPE)
