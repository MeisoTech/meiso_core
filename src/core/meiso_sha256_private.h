#ifndef MEISO_SHA256_PRIVATE_H
#define MEISO_SHA256_PRIVATE_H

#include <meiso/core/resource.h>

#include <stddef.h>
#include <stdint.h>

/* Internal, non-installed primitive. No allocation: 368 bytes on 64-bit
 * targets, including the 256-byte transform schedule. Keeping scratch in the
 * caller context preserves the camera/offscreen runner workspace budget.
 * Callers own validation and the byte sequence; update accepts size == 0. */
typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
    uint32_t words[64];
} MeisoSha256;

void meiso_sha256_init(MeisoSha256 *sha);
void meiso_sha256_update(
    MeisoSha256 *sha,
    const uint8_t *data,
    size_t size
);
void meiso_sha256_final(
    MeisoSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
);

#endif
