#include "meiso_drm_scanout_private.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static void set_primary_failure(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmFailureStage stage,
    int error
)
{
    if (scanout->scanout_receipt.failure_stage ==
        MEISO_K230D_DRM_FAILURE_NONE) {
        scanout->scanout_receipt.failure_stage = stage;
        scanout->scanout_receipt.error = error > 0 ? error : EIO;
    }
}

static bool resources_owned(const MeisoK230dDrmScanout *scanout)
{
    return scanout->fd_owned || scanout->master_owned ||
        scanout->dumb_owned || scanout->mapping_owned ||
        scanout->framebuffer_owned;
}

static MeisoK230dDrmBeginStatus finish_begin_failure(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmBeginStatus status
)
{
    if (resources_owned(scanout) &&
        !meiso_k230d_drm_private_release_resources(scanout)) {
        return MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED;
    }
    scanout->phase = MEISO_K230D_DRM_PHASE_STOPPED;
    return status;
}

static bool find_active_route(
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmConnector *out_connector,
    MeisoK230dDrmCrtc *out_crtc,
    uint32_t *out_crtc_index,
    MeisoK230dDrmAdmissionReason *out_reason
)
{
    uint32_t route_count = 0U;
    uint32_t connector_index;
    uint32_t selected_crtc_index = 0U;
    MeisoK230dDrmConnector selected_connector;
    MeisoK230dDrmCrtc selected_crtc;

    memset(&selected_connector, 0, sizeof(selected_connector));
    memset(&selected_crtc, 0, sizeof(selected_crtc));
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
        selected_connector = *connector;
        selected_crtc = topology->crtcs[crtc_index];
        selected_crtc_index = crtc_index;
    }
    if (route_count == 0U) {
        *out_reason = MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE;
        return false;
    }
    if (route_count != 1U) {
        *out_reason = MEISO_K230D_DRM_ADMISSION_MULTIPLE_ACTIVE_ROUTES;
        return false;
    }
    *out_connector = selected_connector;
    *out_crtc = selected_crtc;
    *out_crtc_index = selected_crtc_index;
    return true;
}

static bool plane_unused(const MeisoK230dDrmPlane *plane)
{
    return plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value == 0U &&
        plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value == 0U;
}

static bool find_overlay_plane(
    const MeisoK230dDrmTopology *topology,
    uint32_t crtc_id,
    uint32_t crtc_index,
    MeisoK230dDrmPlane *out_plane,
    MeisoK230dDrmAdmissionReason *out_reason
)
{
    bool active_present = false;
    bool active_zpos_valid = true;
    bool candidate_shape_present = false;
    bool candidate_selected = false;
    int64_t maximum_active_zpos = INT64_MIN;
    MeisoK230dDrmPlane selected;
    uint32_t index;

    memset(&selected, 0, sizeof(selected));
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
            (plane->possible_crtcs & (UINT32_C(1) << crtc_index)) == 0U) {
            continue;
        }
        candidate_shape_present = true;
        if (!plane->type_immutable || !plane->zpos_present ||
            !plane->zpos_immutable || !active_zpos_valid ||
            (active_present && plane->zpos <= maximum_active_zpos)) {
            continue;
        }
        if (!candidate_selected || plane->zpos < selected.zpos ||
            (plane->zpos == selected.zpos &&
             plane->plane_id < selected.plane_id)) {
            selected = *plane;
            candidate_selected = true;
        }
    }
    if (!candidate_selected) {
        *out_reason = candidate_shape_present ?
            MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE :
            MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY;
        return false;
    }
    *out_plane = selected;
    return true;
}

static void copy_original_state(MeisoK230dDrmScanout *scanout)
{
    uint32_t index;
    scanout->original_plane_state.plane_id = scanout->selected_plane.plane_id;
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        scanout->original_plane_state.properties[index] =
            scanout->selected_plane.properties[index];
    }
}

static void build_present_state(MeisoK230dDrmScanout *scanout)
{
    const uint64_t source_width =
        (uint64_t)scanout->selected_crtc.mode.hdisplay << 16U;
    const uint64_t source_height =
        (uint64_t)scanout->selected_crtc.mode.vdisplay << 16U;
    scanout->present_plane_state = scanout->original_plane_state;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_FB_ID
    ].value = scanout->framebuffer_id;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_CRTC_ID
    ].value = scanout->selected_crtc.crtc_id;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_SRC_X
    ].value = 0U;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_SRC_Y
    ].value = 0U;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_SRC_W
    ].value = source_width;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_SRC_H
    ].value = source_height;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_CRTC_X
    ].value = 0U;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_CRTC_Y
    ].value = 0U;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_CRTC_W
    ].value = scanout->selected_crtc.mode.hdisplay;
    scanout->present_plane_state.properties[
        MEISO_K230D_DRM_PLANE_CRTC_H
    ].value = scanout->selected_crtc.mode.vdisplay;
}

static void populate_route_receipt(MeisoK230dDrmScanout *scanout)
{
    MeisoK230dDrmScanoutReceipt *receipt = &scanout->scanout_receipt;
    memcpy(
        receipt->driver_name,
        scanout->topology.driver_name,
        sizeof(receipt->driver_name)
    );
    receipt->driver_version_major =
        scanout->topology.driver_version_major;
    receipt->driver_version_minor =
        scanout->topology.driver_version_minor;
    receipt->driver_version_patch =
        scanout->topology.driver_version_patch;
    receipt->connector_id = scanout->selected_connector.connector_id;
    receipt->connector_type = scanout->selected_connector.connector_type;
    receipt->connector_type_id =
        scanout->selected_connector.connector_type_id;
    receipt->crtc_id = scanout->selected_crtc.crtc_id;
    receipt->crtc_index = scanout->selected_crtc_index;
    receipt->mode = scanout->selected_crtc.mode;
    receipt->plane_id = scanout->selected_plane.plane_id;
    receipt->plane_type = scanout->selected_plane.type;
    receipt->plane_zpos = scanout->selected_plane.zpos;
    receipt->output_fourcc = MEISO_K230D_DRM_RGB565_FOURCC;
}

MeisoK230dDrmBeginStatus meiso_k230d_drm_scanout_begin(
    MeisoK230dDrmScanout *scanout,
    const MeisoK230dDrmFrameView *frame
)
{
    MeisoK230dDrmAdmissionReason admission_reason;
    MeisoK230dDrmCommitDisposition disposition;
    uint64_t minimum_output_bytes;
    int candidate_fd;
    int error;

    if (!meiso_k230d_drm_private_scanout_valid(scanout) || frame == NULL ||
        scanout->callback_active ||
        meiso_k230d_drm_private_ranges_overlap(
            scanout, scanout->arena_size, frame, sizeof(*frame)
        )) {
        return MEISO_K230D_DRM_BEGIN_INVALID_ARGUMENT;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED) {
        return MEISO_K230D_DRM_BEGIN_TERMINAL;
    }
    if (scanout->phase != MEISO_K230D_DRM_PHASE_STOPPED ||
        resources_owned(scanout)) {
        return MEISO_K230D_DRM_BEGIN_BUSY;
    }
    if (meiso_k230d_drm_private_ranges_overlap(
            scanout, scanout->arena_size, frame->bytes, frame->size
        )) {
        return MEISO_K230D_DRM_BEGIN_INVALID_ARGUMENT;
    }
    memset(&scanout->topology, 0, sizeof(scanout->topology));
    memset(&scanout->selected_connector, 0, sizeof(scanout->selected_connector));
    memset(&scanout->selected_crtc, 0, sizeof(scanout->selected_crtc));
    memset(&scanout->selected_plane, 0, sizeof(scanout->selected_plane));
    memset(&scanout->original_plane_state, 0, sizeof(scanout->original_plane_state));
    memset(&scanout->present_plane_state, 0, sizeof(scanout->present_plane_state));
    memset(&scanout->scanout_receipt, 0, sizeof(scanout->scanout_receipt));
    memset(&scanout->cleanup_receipt, 0, sizeof(scanout->cleanup_receipt));
    scanout->scanout_receipt.receipt_valid = true;
    scanout->scanout_receipt.evidence_class =
        MEISO_K230D_DRM_EVIDENCE_INJECTED_OPS_UNVERIFIED;
    scanout->scanout_receipt.physical_evidence = false;
    if (scanout->generation == UINT64_MAX) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_OPEN, EOVERFLOW
        );
        return MEISO_K230D_DRM_BEGIN_OPERATION_FAILED;
    }
    ++scanout->generation;
    scanout->present_cookie.kind = MEISO_K230D_DRM_COOKIE_PRESENT;
    scanout->present_cookie.generation = scanout->generation;
    scanout->restore_cookie.kind = MEISO_K230D_DRM_COOKIE_RESTORE;
    scanout->restore_cookie.generation = scanout->generation;

    if (!meiso_k230d_drm_private_frame_contract(frame, &admission_reason)) {
        scanout->scanout_receipt.admission_reason = admission_reason;
        return MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED;
    }
    scanout->scanout_receipt.input_format = frame->format;
    scanout->scanout_receipt.input_width = frame->width;
    scanout->scanout_receipt.input_height = frame->height;
    meiso_k230d_drm_private_sha256(
        frame->bytes, frame->size, scanout->scanout_receipt.input_sha256
    );

    candidate_fd = -1;
    error = meiso_k230d_drm_private_call_open(scanout, &candidate_fd);
    if (error != 0) {
        set_primary_failure(scanout, MEISO_K230D_DRM_FAILURE_OPEN, error);
        return MEISO_K230D_DRM_BEGIN_OPERATION_FAILED;
    }
    scanout->fd = candidate_fd;
    scanout->fd_owned = true;
    error = meiso_k230d_drm_private_call_capability(
        scanout, MEISO_K230D_DRM_CLIENT_UNIVERSAL_PLANES
    );
    if (error == 0) {
        error = meiso_k230d_drm_private_call_capability(
            scanout, MEISO_K230D_DRM_CLIENT_ATOMIC
        );
    }
    if (error != 0) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY;
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_CLIENT_CAPABILITY, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    error = meiso_k230d_drm_private_call_acquire_master(scanout);
    if (error != 0) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_MASTER_UNAVAILABLE;
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_ACQUIRE_MASTER, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    scanout->master_owned = true;
    error = meiso_k230d_drm_private_call_topology(
        scanout, &scanout->topology
    );
    if (error != 0) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_TOPOLOGY, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    if (scanout->topology.connector_count >
            MEISO_K230D_DRM_MAX_CONNECTORS ||
        scanout->topology.crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
        scanout->topology.crtc_count > 32U ||
        scanout->topology.plane_count > MEISO_K230D_DRM_MAX_PLANES) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_TOPOLOGY_CAPACITY;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    if (!meiso_k230d_drm_private_topology_valid(&scanout->topology) ||
        !meiso_k230d_drm_private_topology_sha256(
            &scanout->topology,
            scanout->scanout_receipt.topology_sha256
        )) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    if (!scanout->topology.page_flip_timestamps_monotonic) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    admission_reason = MEISO_K230D_DRM_ADMISSION_NONE;
    if (!find_active_route(
            &scanout->topology,
            &scanout->selected_connector,
            &scanout->selected_crtc,
            &scanout->selected_crtc_index,
            &admission_reason
        ) || !find_overlay_plane(
            &scanout->topology,
            scanout->selected_crtc.crtc_id,
            scanout->selected_crtc_index,
            &scanout->selected_plane,
            &admission_reason
        )) {
        scanout->scanout_receipt.admission_reason = admission_reason;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    if (scanout->selected_crtc.mode.hdisplay >
            MEISO_K230D_DRM_MAX_DIMENSION ||
        scanout->selected_crtc.mode.vdisplay >
            MEISO_K230D_DRM_MAX_DIMENSION) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    minimum_output_bytes =
        (uint64_t)scanout->selected_crtc.mode.hdisplay *
        scanout->selected_crtc.mode.vdisplay * UINT64_C(2);
    if (minimum_output_bytes == 0U ||
        minimum_output_bytes > MEISO_K230D_DRM_MAX_DUMB_BYTES) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_BUFFER_ENVELOPE;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    populate_route_receipt(scanout);

    error = meiso_k230d_drm_private_call_create_dumb(
        scanout,
        scanout->selected_crtc.mode.hdisplay,
        scanout->selected_crtc.mode.vdisplay,
        &scanout->dumb
    );
    if (error != 0) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_CREATE_DUMB, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    if (scanout->dumb.handle != 0U) {
        scanout->dumb_owned = true;
    }
    if (!scanout->dumb_owned ||
        (uint64_t)scanout->dumb.pitch <
            (uint64_t)scanout->selected_crtc.mode.hdisplay * UINT64_C(2) ||
        scanout->dumb.size <
            (uint64_t)scanout->dumb.pitch *
                scanout->selected_crtc.mode.vdisplay ||
        scanout->dumb.size > MEISO_K230D_DRM_MAX_DUMB_BYTES ||
        scanout->dumb.size > (uint64_t)SIZE_MAX) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_BUFFER_ENVELOPE;
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    scanout->scanout_receipt.dumb_handle = scanout->dumb.handle;
    scanout->scanout_receipt.pitch = scanout->dumb.pitch;
    scanout->scanout_receipt.dumb_size = scanout->dumb.size;
    error = meiso_k230d_drm_private_call_map(
        scanout, scanout->dumb.handle, scanout->dumb.size, &scanout->mapping
    );
    if (error != 0) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_MAP_DUMB, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    scanout->mapping_owned = true;
    if (meiso_k230d_drm_private_ranges_overlap(
            scanout, scanout->arena_size,
            scanout->mapping, (size_t)scanout->dumb.size
        ) || meiso_k230d_drm_private_ranges_overlap(
            frame->bytes, frame->size,
            scanout->mapping, (size_t)scanout->dumb.size
        ) || meiso_k230d_drm_private_ranges_overlap(
            frame, sizeof(*frame),
            scanout->mapping, (size_t)scanout->dumb.size
        ) || meiso_k230d_drm_private_ranges_overlap(
            scanout->ops_context,
            scanout->ops_context == NULL ? 0U : 1U,
            scanout->mapping, (size_t)scanout->dumb.size
        )) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_MAP_DUMB, EPROTO
        );
        scanout->cleanup_receipt.failure_stage =
            MEISO_K230D_DRM_FAILURE_MAP_DUMB;
        scanout->cleanup_receipt.error = EPROTO;
        scanout->cleanup_receipt.retained_resources = true;
        scanout->cleanup_callbacks_forbidden = true;
        scanout->phase = MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED;
        return MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED;
    }
    if (!meiso_k230d_drm_private_convert_frame(
            frame,
            scanout->selected_crtc.mode.hdisplay,
            scanout->selected_crtc.mode.vdisplay,
            scanout->dumb.pitch,
            (uint8_t *)scanout->mapping,
            scanout->dumb.size,
            &scanout->scanout_receipt.viewport_x,
            &scanout->scanout_receipt.viewport_y,
            &scanout->scanout_receipt.viewport_width,
            &scanout->scanout_receipt.viewport_height
        )) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_MAP_DUMB, EPROTO
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    error = meiso_k230d_drm_private_call_add_framebuffer(
        scanout,
        scanout->selected_crtc.mode.hdisplay,
        scanout->selected_crtc.mode.vdisplay,
        scanout->dumb.handle,
        scanout->dumb.pitch,
        &scanout->framebuffer_id
    );
    if (error != 0) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_ADD_FRAMEBUFFER, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    scanout->framebuffer_owned = true;
    scanout->scanout_receipt.framebuffer_id = scanout->framebuffer_id;
    copy_original_state(scanout);
    build_present_state(scanout);

    disposition = meiso_k230d_drm_private_call_atomic(
        scanout,
        &scanout->present_plane_state,
        MEISO_K230D_DRM_ATOMIC_TEST_ONLY,
        NULL,
        &error
    );
    if (disposition != MEISO_K230D_DRM_COMMIT_ACCEPTED) {
        scanout->scanout_receipt.admission_reason =
            MEISO_K230D_DRM_ADMISSION_ATOMIC_TEST_REJECTED;
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_PRESENT_TEST, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED
        );
    }
    scanout->scanout_receipt.test_only_accepted = true;
    disposition = meiso_k230d_drm_private_call_atomic(
        scanout,
        &scanout->present_plane_state,
        MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT,
        &scanout->present_cookie,
        &error
    );
    if (disposition == MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN) {
        scanout->present_commit_may_be_active = true;
        scanout->scanout_receipt.outcome_unknown = true;
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_PRESENT_COMMIT, error
        );
        scanout->phase = MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN;
        return MEISO_K230D_DRM_BEGIN_OUTCOME_UNKNOWN;
    }
    if (disposition != MEISO_K230D_DRM_COMMIT_ACCEPTED) {
        set_primary_failure(
            scanout, MEISO_K230D_DRM_FAILURE_PRESENT_COMMIT, error
        );
        return finish_begin_failure(
            scanout, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED
        );
    }
    scanout->present_commit_may_be_active = true;
    scanout->scanout_receipt.commit_accepted = true;
    scanout->phase = MEISO_K230D_DRM_PHASE_PRESENT_PENDING;
    return MEISO_K230D_DRM_BEGIN_PRESENT_PENDING;
}

static void mark_present_unknown(
    MeisoK230dDrmScanout *scanout,
    int error
)
{
    scanout->scanout_receipt.outcome_unknown = true;
    set_primary_failure(
        scanout, MEISO_K230D_DRM_FAILURE_PRESENT_EVENT, error
    );
    scanout->phase = MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN;
}

MeisoK230dDrmPollStatus meiso_k230d_drm_scanout_poll_present(
    MeisoK230dDrmScanout *scanout
)
{
    MeisoK230dDrmPageFlipEvent event;
    MeisoK230dDrmEventPollStatus status;
    int error = 0;
    if (!meiso_k230d_drm_private_scanout_valid(scanout) ||
        scanout->callback_active) {
        return MEISO_K230D_DRM_POLL_INVALID_ARGUMENT;
    }
    if (scanout->phase != MEISO_K230D_DRM_PHASE_PRESENT_PENDING) {
        return MEISO_K230D_DRM_POLL_NOT_PENDING;
    }
    memset(&event, 0, sizeof(event));
    status = meiso_k230d_drm_private_call_poll(scanout, &event, &error);
    if (status == MEISO_K230D_DRM_EVENT_WOULD_BLOCK) {
        return MEISO_K230D_DRM_POLL_WOULD_BLOCK;
    }
    if (status == MEISO_K230D_DRM_EVENT_INTERRUPTED) {
        return MEISO_K230D_DRM_POLL_INTERRUPTED;
    }
    if (status != MEISO_K230D_DRM_EVENT) {
        mark_present_unknown(scanout, error);
        return MEISO_K230D_DRM_POLL_OUTCOME_UNKNOWN;
    }
    if (event.cookie != &scanout->present_cookie) {
        return MEISO_K230D_DRM_POLL_WOULD_BLOCK;
    }
    if (event.crtc_id != scanout->selected_crtc.crtc_id ||
        event.monotonic_ns == 0U) {
        mark_present_unknown(scanout, EPROTO);
        return MEISO_K230D_DRM_POLL_OUTCOME_UNKNOWN;
    }
    scanout->scanout_receipt.kms_latched = true;
    scanout->scanout_receipt.matching_event_observed = true;
    scanout->scanout_receipt.event_sequence = event.sequence;
    scanout->scanout_receipt.event_monotonic_ns = event.monotonic_ns;
    scanout->phase = MEISO_K230D_DRM_PHASE_KMS_LATCHED;
    return MEISO_K230D_DRM_POLL_KMS_LATCHED;
}

bool meiso_k230d_drm_scanout_declare_present_outcome_unknown(
    MeisoK230dDrmScanout *scanout,
    int error
)
{
    if (!meiso_k230d_drm_private_scanout_valid(scanout) ||
        scanout->callback_active || error <= 0 ||
        scanout->phase != MEISO_K230D_DRM_PHASE_PRESENT_PENDING) {
        return false;
    }
    mark_present_unknown(scanout, error);
    return true;
}

bool meiso_k230d_drm_scanout_declare_restore_outcome_unknown(
    MeisoK230dDrmScanout *scanout,
    int error
)
{
    if (!meiso_k230d_drm_private_scanout_valid(scanout) ||
        scanout->callback_active || error <= 0 ||
        scanout->phase != MEISO_K230D_DRM_PHASE_RESTORE_PENDING) {
        return false;
    }
    scanout->cleanup_receipt.outcome_unknown = true;
    if (scanout->cleanup_receipt.failure_stage ==
        MEISO_K230D_DRM_FAILURE_NONE) {
        scanout->cleanup_receipt.failure_stage =
            MEISO_K230D_DRM_FAILURE_RESTORE_EVENT;
        scanout->cleanup_receipt.error = error;
    }
    scanout->cleanup_receipt.retained_resources = true;
    scanout->phase = MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN;
    return true;
}
