#ifndef MEISO_COMPOSITION_SHA256_PRIVATE_H
#define MEISO_COMPOSITION_SHA256_PRIVATE_H

#include <meiso/core/resource.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} MeisoCompositionSha256;

void meiso_composition_sha256_init(MeisoCompositionSha256 *sha);
void meiso_composition_sha256_update(
    MeisoCompositionSha256 *sha,
    const uint8_t *data,
    size_t size
);
void meiso_composition_sha256_final(
    MeisoCompositionSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
);

#endif
