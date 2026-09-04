#include "meiso_drm_scanout_private.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool bytes_zero(const void *value, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool meiso_k230d_drm_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    uintptr_t lhs_begin;
    uintptr_t rhs_begin;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs == NULL || rhs == NULL) {
        return true;
    }
    lhs_begin = (uintptr_t)lhs;
    rhs_begin = (uintptr_t)rhs;
    if (lhs_size - 1U > UINTPTR_MAX - lhs_begin ||
        rhs_size - 1U > UINTPTR_MAX - rhs_begin) {
        return true;
    }
    lhs_end = lhs_begin + lhs_size - 1U;
    rhs_end = rhs_begin + rhs_size - 1U;
    return lhs_begin <= rhs_end && rhs_begin <= lhs_end;
}

bool meiso_k230d_drm_private_path_size(
    const char *path,
    size_t *out_size
)
{
    size_t size;
    if (path == NULL || out_size == NULL || path[0] != '/') {
        return false;
    }
    for (size = 1U; size < MEISO_K230D_DRM_DEVICE_PATH_CAPACITY; ++size) {
        if (path[size] == '\0') {
            if (size == 1U) {
                return false;
            }
            *out_size = size;
            return true;
        }
    }
    return false;
}

bool meiso_k230d_drm_private_ops_valid(const MeisoK230dDrmOps *ops)
{
    return ops != NULL && ops->open_device != NULL &&
        ops->set_client_capability != NULL &&
        ops->acquire_master != NULL && ops->read_topology != NULL &&
        ops->create_dumb_buffer != NULL && ops->map_dumb_buffer != NULL &&
        ops->add_framebuffer != NULL && ops->atomic_commit != NULL &&
        ops->poll_event != NULL && ops->read_plane != NULL &&
        ops->remove_framebuffer != NULL &&
        ops->unmap_dumb_buffer != NULL &&
        ops->destroy_dumb_buffer != NULL && ops->drop_master != NULL &&
        ops->close_device != NULL;
}

static bool canonical_string(const char *text, size_t capacity, bool nonempty)
{
    size_t end;
    if (text == NULL || capacity == 0U) {
        return false;
    }
    for (end = 0U; end < capacity && text[end] != '\0'; ++end) {
    }
    if (end == capacity || (nonempty && end == 0U)) {
        return false;
    }
    return bytes_zero(text + end + 1U, capacity - end - 1U);
}

static bool mode_valid(const MeisoK230dDrmMode *mode, bool present)
{
    if (!present) {
        return bytes_zero(mode, sizeof(*mode));
    }
    return canonical_string(
            mode->name, sizeof(mode->name), true
        ) && mode->hdisplay != 0U && mode->vdisplay != 0U &&
        mode->hdisplay <= mode->hsync_start &&
        mode->hsync_start <= mode->hsync_end &&
        mode->hsync_end <= mode->htotal &&
        mode->vdisplay <= mode->vsync_start &&
        mode->vsync_start <= mode->vsync_end &&
        mode->vsync_end <= mode->vtotal;
}

static bool crtc_id_present(
    const MeisoK230dDrmTopology *topology,
    uint32_t crtc_id
)
{
    uint32_t index;
    for (index = 0U; index < topology->crtc_count; ++index) {
        if (topology->crtcs[index].crtc_id == crtc_id) {
            return true;
        }
    }
    return false;
}

static bool crtc_index_for_id(
    const MeisoK230dDrmTopology *topology,
    uint32_t crtc_id,
    uint32_t *out_index
)
{
    uint32_t index;
    for (index = 0U; index < topology->crtc_count; ++index) {
        if (topology->crtcs[index].crtc_id == crtc_id) {
            if (out_index != NULL) {
                *out_index = index;
            }
            return true;
        }
    }
    return false;
}

static bool plane_valid(
    const MeisoK230dDrmPlane *plane,
    uint32_t allowed_crtc_mask
)
{
    uint32_t index;
    uint32_t other;
    const bool fb_present =
        plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value != 0U;
    const bool crtc_present =
        plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value != 0U;

    if (plane->plane_id == 0U ||
        (plane->possible_crtcs & ~allowed_crtc_mask) != 0U ||
        plane->type > MEISO_K230D_DRM_PLANE_TYPE_CURSOR ||
        (plane->type == MEISO_K230D_DRM_PLANE_TYPE_UNKNOWN &&
         plane->type_immutable) || fb_present != crtc_present ||
        (!plane->zpos_present &&
         (plane->zpos_immutable || plane->zpos != 0))) {
        return false;
    }
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        if (plane->properties[index].property_id == 0U) {
            return false;
        }
        for (other = 0U; other < index; ++other) {
            if (plane->properties[other].property_id ==
                plane->properties[index].property_id) {
                return false;
            }
        }
    }
    return true;
}

bool meiso_k230d_drm_private_topology_valid(
    const MeisoK230dDrmTopology *topology
)
{
    uint32_t allowed_crtc_mask;
    uint32_t index;
    uint32_t previous;

    if (topology == NULL ||
        topology->connector_count > MEISO_K230D_DRM_MAX_CONNECTORS ||
        topology->crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
        topology->crtc_count > 32U ||
        topology->plane_count > MEISO_K230D_DRM_MAX_PLANES ||
        !canonical_string(
            topology->driver_name, sizeof(topology->driver_name), true
        )) {
        return false;
    }
    allowed_crtc_mask = topology->crtc_count == 32U ? UINT32_MAX :
        (topology->crtc_count == 0U ? 0U :
         (UINT32_C(1) << topology->crtc_count) - 1U);

    previous = 0U;
    for (index = 0U; index < topology->connector_count; ++index) {
        const MeisoK230dDrmConnector *connector =
            &topology->connectors[index];
        if (connector->connector_id == 0U ||
            connector->connector_id <= previous ||
            (connector->crtc_id != 0U &&
             !crtc_id_present(topology, connector->crtc_id))) {
            return false;
        }
        previous = connector->connector_id;
    }
    if (!bytes_zero(
            &topology->connectors[topology->connector_count],
            sizeof(topology->connectors[0]) *
                (MEISO_K230D_DRM_MAX_CONNECTORS - topology->connector_count)
        )) {
        return false;
    }

    previous = 0U;
    for (index = 0U; index < topology->crtc_count; ++index) {
        const MeisoK230dDrmCrtc *crtc = &topology->crtcs[index];
        if (crtc->crtc_id == 0U || crtc->crtc_id <= previous ||
            crtc->active != crtc->mode_valid ||
            !mode_valid(&crtc->mode, crtc->mode_valid) ||
            (!crtc->active &&
             (crtc->framebuffer_id != 0U || crtc->x != 0U ||
              crtc->y != 0U))) {
            return false;
        }
        previous = crtc->crtc_id;
    }
    if (!bytes_zero(
            &topology->crtcs[topology->crtc_count],
            sizeof(topology->crtcs[0]) *
                (MEISO_K230D_DRM_MAX_CRTCS - topology->crtc_count)
        )) {
        return false;
    }

    previous = 0U;
    for (index = 0U; index < topology->plane_count; ++index) {
        const MeisoK230dDrmPlane *plane = &topology->planes[index];
        const uint64_t crtc_value =
            plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value;
        uint32_t crtc_index = 0U;
        if (plane->plane_id <= previous ||
            !plane_valid(plane, allowed_crtc_mask) ||
            (crtc_value != 0U &&
             (crtc_value > UINT32_MAX ||
              !crtc_index_for_id(
                  topology, (uint32_t)crtc_value, &crtc_index
              ) || !topology->crtcs[crtc_index].active ||
              (plane->possible_crtcs & (UINT32_C(1) << crtc_index)) == 0U))) {
            return false;
        }
        previous = plane->plane_id;
    }
    return bytes_zero(
        &topology->planes[topology->plane_count],
        sizeof(topology->planes[0]) *
            (MEISO_K230D_DRM_MAX_PLANES - topology->plane_count)
    );
}

bool meiso_k230d_drm_private_plane_readback_equal(
    const MeisoK230dDrmPlane *expected,
    const MeisoK230dDrmPlane *actual
)
{
    uint32_t index;
    if (expected == NULL || actual == NULL ||
        expected->plane_id != actual->plane_id ||
        expected->type != actual->type ||
        !expected->type_immutable || !actual->type_immutable ||
        expected->zpos_present != actual->zpos_present ||
        expected->zpos_immutable != actual->zpos_immutable ||
        expected->zpos != actual->zpos) {
        return false;
    }
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        if (expected->properties[index].property_id !=
                actual->properties[index].property_id ||
            expected->properties[index].value !=
                actual->properties[index].value) {
            return false;
        }
    }
    return true;
}

bool meiso_k230d_drm_private_scanout_valid(
    const MeisoK230dDrmScanout *scanout
)
{
    return scanout != NULL && scanout->magic == MEISO_K230D_DRM_MAGIC &&
        scanout->arena_size >= sizeof(*scanout) &&
        scanout->phase >= MEISO_K230D_DRM_PHASE_STOPPED &&
        scanout->phase <= MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED &&
        meiso_k230d_drm_private_ops_valid(&scanout->ops);
}

size_t meiso_k230d_drm_scanout_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_k230d_drm_scanout_memory_required(void)
{
    return sizeof(MeisoK230dDrmScanout);
}

MeisoK230dDrmInitStatus meiso_k230d_drm_scanout_init(
    void *arena,
    size_t arena_size,
    const MeisoK230dDrmConfig *config,
    MeisoK230dDrmScanout **out_scanout
)
{
    MeisoK230dDrmScanout candidate;
    size_t path_size;

    if (arena == NULL || config == NULL || out_scanout == NULL) {
        return MEISO_K230D_DRM_INIT_INVALID_ARGUMENT;
    }
    if (!meiso_k230d_drm_private_path_size(
            config->device_path, &path_size
        ) || !meiso_k230d_drm_private_ops_valid(config->ops)) {
        return MEISO_K230D_DRM_INIT_INVALID_CONFIG;
    }
    if ((uintptr_t)arena % meiso_k230d_drm_scanout_alignment_required() !=
        0U) {
        return MEISO_K230D_DRM_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < meiso_k230d_drm_scanout_memory_required()) {
        return MEISO_K230D_DRM_INIT_ARENA_TOO_SMALL;
    }
    if (meiso_k230d_drm_private_ranges_overlap(
            arena, arena_size, config, sizeof(*config)
        ) || meiso_k230d_drm_private_ranges_overlap(
            arena, arena_size, config->device_path, path_size + 1U
        ) || meiso_k230d_drm_private_ranges_overlap(
            arena, arena_size, config->ops, sizeof(*config->ops)
        ) || meiso_k230d_drm_private_ranges_overlap(
            arena, arena_size, config->ops_context,
            config->ops_context == NULL ? 0U : 1U
        ) || meiso_k230d_drm_private_ranges_overlap(
            arena, arena_size, out_scanout, sizeof(*out_scanout)
        ) || meiso_k230d_drm_private_ranges_overlap(
            out_scanout, sizeof(*out_scanout), config, sizeof(*config)
        ) || meiso_k230d_drm_private_ranges_overlap(
            out_scanout, sizeof(*out_scanout),
            config->device_path, path_size + 1U
        ) || meiso_k230d_drm_private_ranges_overlap(
            out_scanout, sizeof(*out_scanout),
            config->ops, sizeof(*config->ops)
        ) || meiso_k230d_drm_private_ranges_overlap(
            out_scanout, sizeof(*out_scanout), config->ops_context,
            config->ops_context == NULL ? 0U : 1U
        )) {
        return MEISO_K230D_DRM_INIT_INVALID_ARGUMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate.arena_size = arena_size;
    candidate.ops = *config->ops;
    candidate.ops_context = config->ops_context;
    candidate.magic = MEISO_K230D_DRM_MAGIC;
    candidate.fd = -1;
    candidate.phase = MEISO_K230D_DRM_PHASE_STOPPED;
    memcpy(candidate.device_path, config->device_path, path_size + 1U);
    memset(arena, 0, arena_size);
    memcpy(arena, &candidate, sizeof(candidate));
    *out_scanout = (MeisoK230dDrmScanout *)arena;
    return MEISO_K230D_DRM_INIT_OK;
}

bool meiso_k230d_drm_scanout_snapshot(
    const MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmSnapshot *out_snapshot
)
{
    MeisoK230dDrmSnapshot candidate;
    if (!meiso_k230d_drm_private_scanout_valid(scanout) ||
        out_snapshot == NULL || scanout->callback_active ||
        meiso_k230d_drm_private_ranges_overlap(
            scanout, scanout->arena_size, out_snapshot, sizeof(*out_snapshot)
        ) || (scanout->mapping_owned &&
              meiso_k230d_drm_private_ranges_overlap(
                  scanout->mapping, (size_t)scanout->dumb.size,
                  out_snapshot, sizeof(*out_snapshot)
              )) || meiso_k230d_drm_private_ranges_overlap(
            scanout->ops_context,
            scanout->ops_context == NULL ? 0U : 1U,
            out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.phase = scanout->phase;
    candidate.fd_owned = scanout->fd_owned;
    candidate.master_owned = scanout->master_owned;
    candidate.dumb_owned = scanout->dumb_owned;
    candidate.mapping_owned = scanout->mapping_owned;
    candidate.framebuffer_owned = scanout->framebuffer_owned;
    candidate.present_commit_may_be_active =
        scanout->present_commit_may_be_active;
    candidate.restore_commit_may_be_active =
        scanout->restore_commit_may_be_active;
    candidate.cleanup_callbacks_forbidden =
        scanout->cleanup_callbacks_forbidden;
    candidate.scanout = scanout->scanout_receipt;
    candidate.cleanup = scanout->cleanup_receipt;
    candidate.arena_bytes = scanout->arena_size;
    *out_snapshot = candidate;
    return true;
}
