#include "composition_sha256_private.h"

#include <string.h>

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t read_be_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static void write_be_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void sha256_transform(
    MeisoCompositionSha256 *sha,
    const uint8_t block[64]
)
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = read_be_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
            rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
            rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
            rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice +
            constants[index] + words[index];
        const uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
            rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

void meiso_composition_sha256_init(MeisoCompositionSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

void meiso_composition_sha256_update(
    MeisoCompositionSha256 *sha,
    const uint8_t *data,
    size_t size
)
{
    while (size != 0U) {
        const size_t available = sizeof(sha->block) - sha->block_size;
        const size_t take = size < available ? size : available;

        memcpy(sha->block + sha->block_size, data, take);
        sha->block_size += take;
        sha->total_bytes += take;
        data += take;
        size -= take;
        if (sha->block_size == sizeof(sha->block)) {
            sha256_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

void meiso_composition_sha256_final(
    MeisoCompositionSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    const uint64_t total_bits = sha->total_bytes * 8U;
    size_t index;

    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        sha256_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (index = 0U; index < 8U; ++index) {
        sha->block[56U + index] =
            (uint8_t)(total_bits >> (56U - index * 8U));
    }
    sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        write_be_u32(digest + index * 4U, sha->state[index]);
    }
}
