/* ===========================================================================
 * sha256.h
 * ---------------------------------------------------------------------------
 * Tiny self-contained SHA-256 implementation (public-domain style).
 *
 * Used to compute a checksum of the update package so the client can verify
 * file integrity after the transfer (one of the optional advanced features:
 * "Checksum validation"). Implemented locally to avoid any external library
 * dependency such as OpenSSL.
 * ===========================================================================
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_LEN   32          /* raw digest bytes      */
#define SHA256_HEX_LEN      65          /* hex string + NUL      */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN]);

/* Convenience: hash an entire file. Writes a NUL-terminated lowercase hex
 * string into `hexout` (must be >= SHA256_HEX_LEN). Returns 0 on success. */
int  sha256_file(const char *path, char hexout[SHA256_HEX_LEN]);

/* Convert a raw digest to lowercase hex. */
void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN],
                   char hexout[SHA256_HEX_LEN]);

#endif /* SHA256_H */
