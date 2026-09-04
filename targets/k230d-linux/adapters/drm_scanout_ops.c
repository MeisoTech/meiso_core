#include "meiso_drm_scanout_private.h"

#include <errno.h>
#include <string.h>

static int normalized_error(int error)
{
    return error > 0 ? error : EIO;
}

int meiso_k230d_drm_private_call_open(
    MeisoK230dDrmScanout *scanout,
    int *out_fd
)
{
    int candidate = -1;
    int result;
    scanout->callback_active = true;
    result = scanout->ops.open_device(
        scanout->ops_context, scanout->device_path, &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    if (candidate < 0) {
        return EPROTO;
    }
    *out_fd = candidate;
    return 0;
}

int meiso_k230d_drm_private_call_capability(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmClientCapability capability
)
{
    int result;
    scanout->callback_active = true;
    result = scanout->ops.set_client_capability(
        scanout->ops_context, scanout->fd, capability
    );
    scanout->callback_active = false;
    return result == 0 ? 0 : normalized_error(result);
}

int meiso_k230d_drm_private_call_acquire_master(
    MeisoK230dDrmScanout *scanout
)
{
    int result;
    scanout->callback_active = true;
    result = scanout->ops.acquire_master(
        scanout->ops_context, scanout->fd
    );
    scanout->callback_active = false;
    return result == 0 ? 0 : normalized_error(result);
}

int meiso_k230d_drm_private_call_topology(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmTopology *out_topology
)
{
    MeisoK230dDrmTopology candidate;
    int result;
    memset(&candidate, 0xa5, sizeof(candidate));
    scanout->callback_active = true;
    result = scanout->ops.read_topology(
        scanout->ops_context, scanout->fd, &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    *out_topology = candidate;
    return 0;
}

int meiso_k230d_drm_private_call_create_dumb(
    MeisoK230dDrmScanout *scanout,
    uint32_t width,
    uint32_t height,
    MeisoK230dDrmDumbBuffer *out_buffer
)
{
    MeisoK230dDrmDumbBuffer candidate;
    int result;
    memset(&candidate, 0xa5, sizeof(candidate));
    scanout->callback_active = true;
    result = scanout->ops.create_dumb_buffer(
        scanout->ops_context, scanout->fd, width, height, &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    *out_buffer = candidate;
    return 0;
}

int meiso_k230d_drm_private_call_map(
    MeisoK230dDrmScanout *scanout,
    uint32_t handle,
    uint64_t size,
    void **out_mapping
)
{
    void *candidate = NULL;
    int result;
    scanout->callback_active = true;
    result = scanout->ops.map_dumb_buffer(
        scanout->ops_context, scanout->fd, handle, size, &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    if (candidate == NULL) {
        return EPROTO;
    }
    *out_mapping = candidate;
    return 0;
}

int meiso_k230d_drm_private_call_add_framebuffer(
    MeisoK230dDrmScanout *scanout,
    uint32_t width,
    uint32_t height,
    uint32_t handle,
    uint32_t pitch,
    uint32_t *out_framebuffer_id
)
{
    uint32_t candidate = 0U;
    int result;
    scanout->callback_active = true;
    result = scanout->ops.add_framebuffer(
        scanout->ops_context,
        scanout->fd,
        width,
        height,
        MEISO_K230D_DRM_RGB565_FOURCC,
        handle,
        pitch,
        &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    if (candidate == 0U) {
        return EPROTO;
    }
    *out_framebuffer_id = candidate;
    return 0;
}

MeisoK230dDrmCommitDisposition meiso_k230d_drm_private_call_atomic(
    MeisoK230dDrmScanout *scanout,
    const MeisoK230dDrmAtomicPlaneState *state,
    MeisoK230dDrmAtomicKind kind,
    void *event_cookie,
    int *out_error
)
{
    MeisoK230dDrmCommitDisposition disposition;
    int error = 0;
    scanout->callback_active = true;
    disposition = scanout->ops.atomic_commit(
        scanout->ops_context,
        scanout->fd,
        state,
        kind,
        event_cookie,
        &error
    );
    scanout->callback_active = false;
    if (disposition > MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN ||
        (disposition == MEISO_K230D_DRM_COMMIT_ACCEPTED && error != 0) ||
        (disposition != MEISO_K230D_DRM_COMMIT_ACCEPTED && error <= 0) ||
        (kind == MEISO_K230D_DRM_ATOMIC_TEST_ONLY &&
         (event_cookie != NULL ||
          disposition == MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN)) ||
        (kind == MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT &&
         event_cookie == NULL)) {
        *out_error = EPROTO;
        return kind == MEISO_K230D_DRM_ATOMIC_TEST_ONLY ?
            MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED :
            MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN;
    }
    *out_error = error;
    return disposition;
}

MeisoK230dDrmEventPollStatus meiso_k230d_drm_private_call_poll(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmPageFlipEvent *out_event,
    int *out_error
)
{
    MeisoK230dDrmPageFlipEvent event_candidate;
    MeisoK230dDrmEventPollStatus status;
    int error = 0;
    memset(&event_candidate, 0xa5, sizeof(event_candidate));
    scanout->callback_active = true;
    status = scanout->ops.poll_event(
        scanout->ops_context, scanout->fd, &event_candidate, &error
    );
    scanout->callback_active = false;
    if (status > MEISO_K230D_DRM_EVENT_FAILED ||
        ((status == MEISO_K230D_DRM_EVENT_TIMEOUT ||
          status == MEISO_K230D_DRM_EVENT_FAILED) && error <= 0) ||
        ((status == MEISO_K230D_DRM_EVENT ||
          status == MEISO_K230D_DRM_EVENT_WOULD_BLOCK ||
          status == MEISO_K230D_DRM_EVENT_INTERRUPTED) && error != 0)) {
        *out_error = EPROTO;
        return MEISO_K230D_DRM_EVENT_FAILED;
    }
    if (status == MEISO_K230D_DRM_EVENT) {
        *out_event = event_candidate;
    }
    *out_error = error;
    return status;
}

int meiso_k230d_drm_private_call_read_plane(
    MeisoK230dDrmScanout *scanout,
    uint32_t plane_id,
    MeisoK230dDrmPlane *out_plane
)
{
    MeisoK230dDrmPlane candidate;
    int result;
    memset(&candidate, 0xa5, sizeof(candidate));
    scanout->callback_active = true;
    result = scanout->ops.read_plane(
        scanout->ops_context, scanout->fd, plane_id, &candidate
    );
    scanout->callback_active = false;
    if (result != 0) {
        return normalized_error(result);
    }
    *out_plane = candidate;
    return 0;
}

static void record_cleanup_failure(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmFailureStage stage,
    int error
)
{
    if (scanout->cleanup_receipt.failure_stage ==
        MEISO_K230D_DRM_FAILURE_NONE) {
        scanout->cleanup_receipt.failure_stage = stage;
        scanout->cleanup_receipt.error = normalized_error(error);
    }
}

bool meiso_k230d_drm_private_release_resources(
    MeisoK230dDrmScanout *scanout
)
{
    int result;

    if (scanout->cleanup_callbacks_forbidden) {
        scanout->cleanup_receipt.retained_resources = true;
        scanout->phase = MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED;
        return false;
    }
    if (scanout->present_commit_may_be_active ||
        scanout->restore_commit_may_be_active) {
        scanout->cleanup_receipt.retained_resources = true;
        scanout->phase = MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED;
        return false;
    }
    if (scanout->framebuffer_owned) {
        scanout->callback_active = true;
        result = scanout->ops.remove_framebuffer(
            scanout->ops_context, scanout->fd, scanout->framebuffer_id
        );
        scanout->callback_active = false;
        if (result != 0) {
            record_cleanup_failure(
                scanout,
                MEISO_K230D_DRM_FAILURE_REMOVE_FRAMEBUFFER,
                result
            );
            goto retained;
        }
        scanout->framebuffer_owned = false;
        scanout->framebuffer_id = 0U;
        scanout->cleanup_receipt.framebuffer_removed = true;
    }
    if (scanout->mapping_owned) {
        scanout->callback_active = true;
        result = scanout->ops.unmap_dumb_buffer(
            scanout->ops_context, scanout->mapping, scanout->dumb.size
        );
        scanout->callback_active = false;
        if (result != 0) {
            record_cleanup_failure(
                scanout, MEISO_K230D_DRM_FAILURE_UNMAP_DUMB, result
            );
            goto retained;
        }
        scanout->mapping_owned = false;
        scanout->mapping = NULL;
        scanout->cleanup_receipt.mapping_unmapped = true;
    }
    if (scanout->dumb_owned) {
        scanout->callback_active = true;
        result = scanout->ops.destroy_dumb_buffer(
            scanout->ops_context, scanout->fd, scanout->dumb.handle
        );
        scanout->callback_active = false;
        if (result != 0) {
            record_cleanup_failure(
                scanout, MEISO_K230D_DRM_FAILURE_DESTROY_DUMB, result
            );
            goto retained;
        }
        scanout->dumb_owned = false;
        memset(&scanout->dumb, 0, sizeof(scanout->dumb));
        scanout->cleanup_receipt.dumb_buffer_destroyed = true;
    }
    if (scanout->master_owned) {
        scanout->callback_active = true;
        result = scanout->ops.drop_master(
            scanout->ops_context, scanout->fd
        );
        scanout->callback_active = false;
        if (result != 0) {
            record_cleanup_failure(
                scanout, MEISO_K230D_DRM_FAILURE_DROP_MASTER, result
            );
            goto retained;
        }
        scanout->master_owned = false;
        scanout->cleanup_receipt.master_dropped = true;
    }
    if (scanout->fd_owned) {
        const int fd = scanout->fd;
        scanout->fd_owned = false;
        scanout->fd = -1;
        scanout->callback_active = true;
        result = scanout->ops.close_device(scanout->ops_context, fd);
        scanout->callback_active = false;
        if (result != 0) {
            record_cleanup_failure(
                scanout, MEISO_K230D_DRM_FAILURE_CLOSE, result
            );
            scanout->cleanup_receipt.device_closed = false;
            scanout->cleanup_receipt.retained_resources = false;
            scanout->phase = MEISO_K230D_DRM_PHASE_STOPPED;
            return false;
        }
        scanout->cleanup_receipt.device_closed = true;
    }
    scanout->cleanup_receipt.retained_resources = false;
    scanout->phase = MEISO_K230D_DRM_PHASE_STOPPED;
    return true;

retained:
    scanout->cleanup_receipt.retained_resources = true;
    scanout->phase = MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED;
    return false;
}
