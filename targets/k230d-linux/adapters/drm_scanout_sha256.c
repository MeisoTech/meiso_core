#include "meiso_drm_scanout_private.h"

#include <string.h>

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} DrmSha256;

static uint32_t rotate_right(uint32_t value, uint32_t shift)
{
    return (value >> shift) | (value << (32U - shift));
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
        ((uint32_t)bytes[1] << 16U) |
        ((uint32_t)bytes[2] << 8U) |
        (uint32_t)bytes[3];
}

static void write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)(value >> 16U);
    bytes[2] = (uint8_t)(value >> 8U);
    bytes[3] = (uint8_t)value;
}

static void write_be64(uint8_t *bytes, uint64_t value)
{
    write_be32(bytes, (uint32_t)(value >> 32U));
    write_be32(bytes + 4U, (uint32_t)value);
}

static void sha_transform(DrmSha256 *sha, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491),
        UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01),
        UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe),
        UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa),
        UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d),
        UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138),
        UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb),
        UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624),
        UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08),
        UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f),
        UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb),
        UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)
    };
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        schedule[index] = read_be32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t x = schedule[index - 15U];
        const uint32_t y = schedule[index - 2U];
        const uint32_t s0 = rotate_right(x, 7U) ^ rotate_right(x, 18U) ^
            (x >> 3U);
        const uint32_t s1 = rotate_right(y, 17U) ^ rotate_right(y, 19U) ^
            (y >> 10U);
        schedule[index] = schedule[index - 16U] + s0 +
            schedule[index - 7U] + s1;
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
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temporary1 = h + sum1 + choose + constants[index] +
            schedule[index];
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

static void sha_init(DrmSha256 *sha)
{
    static const uint32_t initial[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
    };
    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha_update(DrmSha256 *sha, const uint8_t *bytes, size_t size)
{
    while (size > 0U) {
        size_t available = sizeof(sha->block) - sha->block_size;
        size_t amount = size < available ? size : available;
        memcpy(sha->block + sha->block_size, bytes, amount);
        sha->block_size += amount;
        sha->total_bytes += amount;
        bytes += amount;
        size -= amount;
        if (sha->block_size == sizeof(sha->block)) {
            sha_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

static void sha_final(DrmSha256 *sha, uint8_t digest[32])
{
    uint64_t bit_count = sha->total_bytes * UINT64_C(8);
    uint32_t index;

    sha->block[sha->block_size++] = UINT8_C(0x80);
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0, 64U - sha->block_size);
        sha_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    write_be64(sha->block + 56U, bit_count);
    sha_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        write_be32(digest + index * 4U, sha->state[index]);
    }
}

void meiso_k230d_drm_private_sha256(
    const uint8_t *bytes,
    size_t size,
    uint8_t digest[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    DrmSha256 sha;
    sha_init(&sha);
    if (size != 0U) {
        sha_update(&sha, bytes, size);
    }
    sha_final(&sha, digest);
}

static void hash_u8(DrmSha256 *sha, uint8_t value)
{
    sha_update(sha, &value, 1U);
}

static void hash_u16(DrmSha256 *sha, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
    sha_update(sha, bytes, sizeof(bytes));
}

static void hash_u32(DrmSha256 *sha, uint32_t value)
{
    uint8_t bytes[4];
    write_be32(bytes, value);
    sha_update(sha, bytes, sizeof(bytes));
}

static void hash_u64(DrmSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];
    write_be64(bytes, value);
    sha_update(sha, bytes, sizeof(bytes));
}

static void hash_mode(DrmSha256 *sha, const MeisoK230dDrmMode *mode)
{
    sha_update(sha, (const uint8_t *)mode->name, sizeof(mode->name));
    hash_u32(sha, mode->clock_khz);
    hash_u16(sha, mode->hdisplay);
    hash_u16(sha, mode->hsync_start);
    hash_u16(sha, mode->hsync_end);
    hash_u16(sha, mode->htotal);
    hash_u16(sha, mode->hskew);
    hash_u16(sha, mode->vdisplay);
    hash_u16(sha, mode->vsync_start);
    hash_u16(sha, mode->vsync_end);
    hash_u16(sha, mode->vtotal);
    hash_u16(sha, mode->vscan);
    hash_u32(sha, mode->flags);
    hash_u32(sha, mode->type);
}

bool meiso_k230d_drm_private_topology_sha256(
    const MeisoK230dDrmTopology *topology,
    uint8_t digest[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    static const uint8_t prefix[8] = {
        'K', 'D', 'T', '0', 0, 0, 0, 1
    };
    DrmSha256 sha;
    uint32_t index;
    uint32_t property_index;

    if (topology == NULL || digest == NULL ||
        !meiso_k230d_drm_private_topology_valid(topology)) {
        return false;
    }
    sha_init(&sha);
    sha_update(&sha, prefix, sizeof(prefix));
    sha_update(
        &sha,
        (const uint8_t *)topology->driver_name,
        sizeof(topology->driver_name)
    );
    hash_u32(&sha, topology->driver_version_major);
    hash_u32(&sha, topology->driver_version_minor);
    hash_u32(&sha, topology->driver_version_patch);
    hash_u8(&sha, topology->page_flip_timestamps_monotonic ? 1U : 0U);
    hash_u32(&sha, topology->connector_count);
    hash_u32(&sha, topology->crtc_count);
    hash_u32(&sha, topology->plane_count);
    for (index = 0U; index < topology->connector_count; ++index) {
        const MeisoK230dDrmConnector *connector =
            &topology->connectors[index];
        hash_u32(&sha, connector->connector_id);
        hash_u32(&sha, connector->connector_type);
        hash_u32(&sha, connector->connector_type_id);
        hash_u32(&sha, connector->encoder_id);
        hash_u32(&sha, connector->crtc_id);
        hash_u8(&sha, connector->connected ? 1U : 0U);
    }
    for (index = 0U; index < topology->crtc_count; ++index) {
        const MeisoK230dDrmCrtc *crtc = &topology->crtcs[index];
        hash_u32(&sha, crtc->crtc_id);
        hash_u32(&sha, crtc->framebuffer_id);
        hash_u32(&sha, crtc->x);
        hash_u32(&sha, crtc->y);
        hash_u8(&sha, crtc->active ? 1U : 0U);
        hash_u8(&sha, crtc->mode_valid ? 1U : 0U);
        hash_mode(&sha, &crtc->mode);
    }
    for (index = 0U; index < topology->plane_count; ++index) {
        const MeisoK230dDrmPlane *plane = &topology->planes[index];
        hash_u32(&sha, plane->plane_id);
        hash_u32(&sha, plane->possible_crtcs);
        hash_u8(&sha, plane->type);
        hash_u8(&sha, plane->type_immutable ? 1U : 0U);
        hash_u8(&sha, plane->supports_rgb565 ? 1U : 0U);
        hash_u8(&sha, plane->zpos_present ? 1U : 0U);
        hash_u8(&sha, plane->zpos_immutable ? 1U : 0U);
        hash_u64(&sha, (uint64_t)plane->zpos);
        for (property_index = 0U;
             property_index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
             ++property_index) {
            hash_u32(
                &sha, plane->properties[property_index].property_id
            );
            hash_u64(&sha, plane->properties[property_index].value);
        }
    }
    sha_final(&sha, digest);
    return true;
}
