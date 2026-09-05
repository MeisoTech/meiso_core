#include "meiso_drm_scanout_private.h"

#include <limits.h>
#include <string.h>

static bool plane_unused(const MeisoK230dDrmPlane *plane)
{
    return plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value == 0U &&
        plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value == 0U;
}

static bool find_active_route(
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmTopologyAdmission *admission
)
{
    uint32_t route_count = 0U;
    uint32_t connector_index;

    for (connector_index = 0U;
         connector_index < topology->connector_count;
         ++connector_index) {
        const MeisoK230dDrmConnector *connector =
            &topology->connectors[connector_index];
        uint32_t crtc_index;
        if (!connector->connected || connector->encoder_id == 0U ||
            connector->crtc_id == 0U) {
            continue;
        }
        for (crtc_index = 0U; crtc_index < topology->crtc_count;
             ++crtc_index) {
            if (topology->crtcs[crtc_index].crtc_id ==
                connector->crtc_id) {
                break;
            }
        }
        if (crtc_index == topology->crtc_count ||
            !topology->crtcs[crtc_index].active ||
            !topology->crtcs[crtc_index].mode_valid) {
            continue;
        }
        ++route_count;
        admission->connector_index = connector_index;
        admission->crtc_index = crtc_index;
    }
    if (route_count == 0U) {
        admission->reason = MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE;
        return false;
    }
    if (route_count != 1U) {
        admission->reason =
            MEISO_K230D_DRM_ADMISSION_MULTIPLE_ACTIVE_ROUTES;
        return false;
    }
    admission->route_selected = true;
    return true;
}

static bool find_overlay_plane(
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmTopologyAdmission *admission
)
{
    const uint32_t crtc_id = topology->crtcs[
        admission->crtc_index
    ].crtc_id;
    bool active_present = false;
    bool active_zpos_valid = true;
    bool candidate_shape_present = false;
    bool candidate_selected = false;
    int64_t maximum_active_zpos = INT64_MIN;
    int64_t selected_zpos = 0;
    uint32_t selected_plane_id = 0U;
    uint32_t index;

    for (index = 0U; index < topology->plane_count; ++index) {
        const MeisoK230dDrmPlane *plane = &topology->planes[index];
        const uint64_t attached_crtc =
            plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value;
        if (attached_crtc == crtc_id) {
            active_present = true;
            if (!plane->type_immutable || !plane->zpos_present ||
                !plane->zpos_immutable) {
                active_zpos_valid = false;
            } else if (plane->zpos > maximum_active_zpos) {
                maximum_active_zpos = plane->zpos;
            }
        }
    }
    for (index = 0U; index < topology->plane_count; ++index) {
        const MeisoK230dDrmPlane *plane = &topology->planes[index];
        if (plane->type != MEISO_K230D_DRM_PLANE_TYPE_OVERLAY ||
            !plane->supports_rgb565 || !plane_unused(plane) ||
            (plane->possible_crtcs &
             (UINT32_C(1) << admission->crtc_index)) == 0U) {
            continue;
        }
        candidate_shape_present = true;
        if (!plane->type_immutable || !plane->zpos_present ||
            !plane->zpos_immutable || !active_zpos_valid ||
            (active_present && plane->zpos <= maximum_active_zpos)) {
            continue;
        }
        if (!candidate_selected || plane->zpos < selected_zpos ||
            (plane->zpos == selected_zpos &&
             plane->plane_id < selected_plane_id)) {
            admission->plane_index = index;
            selected_zpos = plane->zpos;
            selected_plane_id = plane->plane_id;
            candidate_selected = true;
        }
    }
    if (!candidate_selected) {
        admission->reason = candidate_shape_present ?
            MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE :
            MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY;
        return false;
    }
    admission->plane_selected = true;
    return true;
}

MeisoK230dDrmTopologyAdmissionStatus
meiso_k230d_drm_private_evaluate_topology(
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmTopologyAdmission *out_admission
)
{
    MeisoK230dDrmTopologyAdmission candidate;
    const MeisoK230dDrmMode *mode;

    if (topology == NULL || out_admission == NULL ||
        meiso_k230d_drm_private_ranges_overlap(
            topology, sizeof(*topology), out_admission,
            sizeof(*out_admission)
        )) {
        return MEISO_K230D_DRM_TOPOLOGY_INVALID_ARGUMENT;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (topology->connector_count > MEISO_K230D_DRM_MAX_CONNECTORS ||
        topology->crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
        topology->crtc_count > 32U ||
        topology->plane_count > MEISO_K230D_DRM_MAX_PLANES) {
        candidate.reason = MEISO_K230D_DRM_ADMISSION_TOPOLOGY_CAPACITY;
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    if (!meiso_k230d_drm_private_topology_valid(topology) ||
        !meiso_k230d_drm_private_topology_sha256(
            topology, candidate.topology_sha256
        )) {
        candidate.reason =
            MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    candidate.topology_hash_valid = true;
    if (!topology->page_flip_timestamps_monotonic) {
        candidate.reason =
            MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE;
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    if (!find_active_route(topology, &candidate) ||
        !find_overlay_plane(topology, &candidate)) {
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    mode = &topology->crtcs[candidate.crtc_index].mode;
    if (mode->hdisplay > MEISO_K230D_DRM_MAX_DIMENSION ||
        mode->vdisplay > MEISO_K230D_DRM_MAX_DIMENSION) {
        candidate.reason = MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE;
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    candidate.minimum_output_bytes =
        (uint64_t)mode->hdisplay * mode->vdisplay * UINT64_C(2);
    if (candidate.minimum_output_bytes == 0U ||
        candidate.minimum_output_bytes > MEISO_K230D_DRM_MAX_DUMB_BYTES) {
        candidate.reason = MEISO_K230D_DRM_ADMISSION_BUFFER_ENVELOPE;
        *out_admission = candidate;
        return MEISO_K230D_DRM_TOPOLOGY_BLOCKED;
    }
    candidate.reason = MEISO_K230D_DRM_ADMISSION_NONE;
    *out_admission = candidate;
    return MEISO_K230D_DRM_TOPOLOGY_ELIGIBLE;
}

bool meiso_k230d_drm_private_remap_crtc_mask(
    const uint32_t *resource_order_ids,
    uint32_t resource_count,
    const uint32_t *canonical_ids,
    uint32_t canonical_count,
    uint32_t resource_order_mask,
    uint32_t *out_mask
)
{
    uint32_t candidate = 0U;
    uint32_t resource_index;
    uint32_t allowed_mask;

    if (out_mask == NULL || resource_count != canonical_count ||
        resource_count > MEISO_K230D_DRM_MAX_CRTCS ||
        resource_count > 32U ||
        (resource_count != 0U &&
         (resource_order_ids == NULL || canonical_ids == NULL)) ||
        meiso_k230d_drm_private_ranges_overlap(
            resource_order_ids,
            (size_t)resource_count * sizeof(resource_order_ids[0]),
            out_mask, sizeof(*out_mask)
        ) || meiso_k230d_drm_private_ranges_overlap(
            canonical_ids,
            (size_t)canonical_count * sizeof(canonical_ids[0]),
            out_mask, sizeof(*out_mask)
        )) {
        return false;
    }
    allowed_mask = resource_count == 32U ? UINT32_MAX :
        (resource_count == 0U ? 0U :
         (UINT32_C(1) << resource_count) - 1U);
    if ((resource_order_mask & ~allowed_mask) != 0U) {
        return false;
    }
    for (resource_index = 0U; resource_index < resource_count;
         ++resource_index) {
        uint32_t canonical_index;
        uint32_t other;
        if (resource_order_ids[resource_index] == 0U ||
            canonical_ids[resource_index] == 0U ||
            (resource_index != 0U &&
             canonical_ids[resource_index - 1U] >=
                 canonical_ids[resource_index])) {
            return false;
        }
        for (other = 0U; other < resource_index; ++other) {
            if (resource_order_ids[other] ==
                resource_order_ids[resource_index]) {
                return false;
            }
        }
        for (canonical_index = 0U;
             canonical_index < canonical_count;
             ++canonical_index) {
            if (canonical_ids[canonical_index] ==
                resource_order_ids[resource_index]) {
                break;
            }
        }
        if (canonical_index == canonical_count) {
            return false;
        }
        if ((resource_order_mask & (UINT32_C(1) << resource_index)) != 0U) {
            candidate |= UINT32_C(1) << canonical_index;
        }
    }
    *out_mask = candidate;
    return true;
}
