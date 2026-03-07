set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

include_directories(include)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Use a minimal config that enables only SHA-256 and HMAC-SHA256, which is
# all that the httpfs/llm extensions require.  This avoids pulling in AES,
# GCM, RSA, ECC and the rest of the mbedtls universe (and the associated
# hardware-acceleration symbols such as mbedtls_aesni_gcm_mult).
add_compile_definitions(MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_SOURCE_DIR}/include/lbug_config.h")

add_library(mbedtls STATIC
        library/platform.cpp
        library/platform_util.cpp
        library/md.cpp
        library/sha256.cpp)

set_property(TARGET mbedtls PROPERTY C_STANDARD 99)
