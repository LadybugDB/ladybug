set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(POLICY CMP0063)
    cmake_policy(SET CMP0063 NEW)
endif()

include_directories(include)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)

add_library(mbedtls STATIC
        library/aesce.cpp
        library/aes.cpp
        library/aesni.cpp
        library/aria.cpp
        library/asn1parse.cpp
        library/base64.cpp
        library/bignum.cpp
        library/bignum_core.cpp
        library/bignum_mod.cpp
        library/bignum_mod_raw.cpp
        library/block_cipher.cpp
        library/camellia.cpp
        library/ccm.cpp
        library/chacha20.cpp
        library/chachapoly.cpp
        library/cipher.cpp
        library/cipher_wrap.cpp
        library/cmac.cpp
        library/constant_time.cpp
        library/ctr_drbg.cpp
        library/des.cpp
        library/ecdh.cpp
        library/ecdsa.cpp
        library/ecjpake.cpp
        library/ecp.cpp
        library/ecp_curves.cpp
        library/ecp_curves_new.cpp
        library/entropy.cpp
        library/entropy_poll.cpp
        library/gcm.cpp
        library/hkdf.cpp
        library/hmac_drbg.cpp
        library/md.cpp
        library/md5.cpp
        library/nist_kw.cpp
        library/oid.cpp
        library/pem.cpp
        library/pk.cpp
        library/pk_ecc.cpp
        library/pk_wrap.cpp
        library/pkcs12.cpp
        library/pkcs5.cpp
        library/pkparse.cpp
        library/platform_util.cpp
        library/poly1305.cpp
        library/psa_crypto.cpp
        library/psa_crypto_aead.cpp
        library/psa_crypto_cipher.cpp
        library/psa_crypto_client.cpp
        library/psa_crypto_driver_wrappers_no_static.cpp
        library/psa_crypto_ecp.cpp
        library/psa_crypto_ffdh.cpp
        library/psa_crypto_hash.cpp
        library/psa_crypto_mac.cpp
        library/psa_crypto_pake.cpp
        library/psa_crypto_rsa.cpp
        library/psa_crypto_se.cpp
        library/psa_crypto_slot_management.cpp
        library/psa_crypto_storage.cpp
        library/psa_its_file.cpp
        library/psa_util.cpp
        library/ripemd160.cpp
        library/rsa.cpp
        library/rsa_alt_helpers.cpp
        library/sha1.cpp
        library/sha256.cpp
        library/sha3.cpp
        library/sha512.cpp)

set_property(TARGET mbedtls PROPERTY C_STANDARD 99)
