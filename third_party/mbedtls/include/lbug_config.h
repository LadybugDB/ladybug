/**
 * Minimal mbedtls configuration for ladybug.
 * Only SHA-256 and HMAC-SHA256 are needed (used by the httpfs/llm extensions).
 */

#ifndef MBEDTLS_LBUG_CONFIG_H
#define MBEDTLS_LBUG_CONFIG_H

/* SHA-256 (and SHA-224 via the same translation unit) */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C

/* Generic message-digest / HMAC layer, restricted to SHA-256 */
#define MBEDTLS_MD_C

/* Platform abstraction (mbedtls_platform_zeroize, etc.) */
#define MBEDTLS_PLATFORM_C

#endif /* MBEDTLS_LBUG_CONFIG_H */
