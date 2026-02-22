cmake_minimum_required(VERSION 3.11)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

project(zstd CXX)

set(ZSTD_SOURCES
        common/entropy_common.cpp
        common/error_private.cpp
        common/fse_decompress.cpp
        common/xxhash.cpp
        common/zstd_common.cpp
        compress/fse_compress.cpp
        compress/hist.cpp
        compress/huf_compress.cpp
        compress/zstd_compress.cpp
        compress/zstd_compress_literals.cpp
        compress/zstd_compress_sequences.cpp
        compress/zstd_compress_superblock.cpp
        compress/zstd_double_fast.cpp
        compress/zstd_fast.cpp
        compress/zstd_lazy.cpp
        compress/zstd_ldm.cpp
        compress/zstd_opt.cpp
        compress/zstd_preSplit.cpp
        decompress/huf_decompress.cpp
        decompress/zstd_ddict.cpp
        decompress/zstd_decompress.cpp
        decompress/zstd_decompress_block.cpp)

add_library(zstd STATIC ${ZSTD_SOURCES})

# Hide all zstd symbols, since we are importing the library.
add_compile_definitions(zstd ZSTDLIB_VISIBILITY= ZSTDERRORLIB_VISIBILITY=)

# Disable x86-64 ASM optimizations to avoid hidden symbol issues with shared libraries
# The assembly functions are marked as hidden (.hidden directive) which causes
# "relocation R_X86_64_PC32 against undefined hidden symbol" errors when building shared libs
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)

target_include_directories(
        zstd
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/zstd/common>
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/zstd/compress>
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/zstd/decompress>
        PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common>)
