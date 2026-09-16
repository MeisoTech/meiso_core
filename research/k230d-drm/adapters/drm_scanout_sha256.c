#include "meiso_drm_scanout_private.h"
#include "../../../src/core/meiso_sha256_private.h"

#include <string.h>

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

void meiso_k230d_drm_private_sha256(
    const uint8_t *bytes,
    size_t size,
    uint8_t digest[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    MeisoSha256 sha;
    meiso_sha256_init(&sha);
    if (size != 0U) {
        meiso_sha256_update(&sha, bytes, size);
    }
    meiso_sha256_final(&sha, digest);
}

static void hash_u8(MeisoSha256 *sha, uint8_t value)
{
    meiso_sha256_update(sha, &value, 1U);
}

static void hash_u16(MeisoSha256 *sha, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
    meiso_sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_u32(MeisoSha256 *sha, uint32_t value)
{
    uint8_t bytes[4];
    write_be32(bytes, value);
    meiso_sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_u64(MeisoSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];
    write_be64(bytes, value);
    meiso_sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_mode(MeisoSha256 *sha, const MeisoK230dDrmMode *mode)
{
    meiso_sha256_update(sha, (const uint8_t *)mode->name, sizeof(mode->name));
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
    MeisoSha256 sha;
    uint32_t index;
    uint32_t property_index;

    if (topology == NULL || digest == NULL ||
        !meiso_k230d_drm_private_topology_valid(topology)) {
        return false;
    }
    meiso_sha256_init(&sha);
    meiso_sha256_update(&sha, prefix, sizeof(prefix));
    meiso_sha256_update(
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
    meiso_sha256_final(&sha, digest);
    return true;
}
