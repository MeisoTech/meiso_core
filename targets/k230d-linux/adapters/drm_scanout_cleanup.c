#include "meiso_drm_scanout_private.h"

#include <errno.h>
#include <string.h>

static void set_cleanup_failure(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmFailureStage stage,
    int error
)
{
    if (scanout->cleanup_receipt.failure_stage ==
        MEISO_K230D_DRM_FAILURE_NONE) {
        scanout->cleanup_receipt.failure_stage = stage;
        scanout->cleanup_receipt.error = error > 0 ? error : EIO;
    }
}

static MeisoK230dDrmPhase unresolved_present_phase(
    const MeisoK230dDrmScanout *scanout
)
{
    return scanout->scanout_receipt.outcome_unknown ?
        MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN :
        MEISO_K230D_DRM_PHASE_KMS_LATCHED;
}

static MeisoK230dDrmCleanupStatus begin_restore(
    MeisoK230dDrmScanout *scanout
)
{
    MeisoK230dDrmCommitDisposition disposition;
    int error = 0;

    memset(&scanout->cleanup_receipt, 0, sizeof(scanout->cleanup_receipt));
    scanout->cleanup_receipt.restore_required = true;
    scanout->cleanup_receipt.retained_resources = true;
    disposition = meiso_k230d_drm_private_call_atomic(
        scanout,
        &scanout->original_plane_state,
        MEISO_K230D_DRM_ATOMIC_TEST_ONLY,
        NULL,
        &error
    );
    if (disposition != MEISO_K230D_DRM_COMMIT_ACCEPTED) {
        set_cleanup_failure(
            scanout, MEISO_K230D_DRM_FAILURE_RESTORE_TEST, error
        );
        return MEISO_K230D_DRM_CLEANUP_OPERATION_FAILED;
    }
    scanout->cleanup_receipt.restore_test_accepted = true;
    disposition = meiso_k230d_drm_private_call_atomic(
        scanout,
        &scanout->original_plane_state,
        MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT,
        &scanout->restore_cookie,
        &error
    );
    if (disposition == MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN) {
        scanout->restore_commit_may_be_active = true;
        scanout->cleanup_receipt.outcome_unknown = true;
        set_cleanup_failure(
            scanout, MEISO_K230D_DRM_FAILURE_RESTORE_COMMIT, error
        );
        scanout->phase = MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN;
        return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
    }
    if (disposition != MEISO_K230D_DRM_COMMIT_ACCEPTED) {
        set_cleanup_failure(
            scanout, MEISO_K230D_DRM_FAILURE_RESTORE_COMMIT, error
        );
        scanout->phase = unresolved_present_phase(scanout);
        return MEISO_K230D_DRM_CLEANUP_OPERATION_FAILED;
    }
    scanout->restore_commit_may_be_active = true;
    scanout->cleanup_receipt.restore_commit_accepted = true;
    scanout->phase = MEISO_K230D_DRM_PHASE_RESTORE_PENDING;
    return MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING;
}

static MeisoK230dDrmCleanupStatus poll_restore(
    MeisoK230dDrmScanout *scanout
)
{
    MeisoK230dDrmPageFlipEvent event;
    MeisoK230dDrmEventPollStatus status;
    MeisoK230dDrmPlane readback;
    int error = 0;

    memset(&event, 0, sizeof(event));
    status = meiso_k230d_drm_private_call_poll(scanout, &event, &error);
    if (status == MEISO_K230D_DRM_EVENT_WOULD_BLOCK) {
        return MEISO_K230D_DRM_CLEANUP_WOULD_BLOCK;
    }
    if (status == MEISO_K230D_DRM_EVENT_INTERRUPTED) {
        return MEISO_K230D_DRM_CLEANUP_INTERRUPTED;
    }
    if (status != MEISO_K230D_DRM_EVENT) {
        (void)meiso_k230d_drm_scanout_declare_restore_outcome_unknown(
            scanout, error
        );
        return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
    }

    /* Any unrelated/late cookie is consumed but cannot resolve this restore. */
    if (event.cookie != &scanout->restore_cookie) {
        return MEISO_K230D_DRM_CLEANUP_WOULD_BLOCK;
    }
    if (event.crtc_id != scanout->selected_crtc.crtc_id ||
        event.monotonic_ns == 0U) {
        (void)meiso_k230d_drm_scanout_declare_restore_outcome_unknown(
            scanout, EPROTO
        );
        return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
    }
    scanout->cleanup_receipt.matching_event_observed = true;
    scanout->cleanup_receipt.event_sequence = event.sequence;
    scanout->cleanup_receipt.event_monotonic_ns = event.monotonic_ns;

    memset(&readback, 0, sizeof(readback));
    error = meiso_k230d_drm_private_call_read_plane(
        scanout, scanout->selected_plane.plane_id, &readback
    );
    if (error != 0 || !meiso_k230d_drm_private_plane_readback_equal(
            &scanout->selected_plane, &readback
        )) {
        scanout->cleanup_receipt.outcome_unknown = true;
        set_cleanup_failure(
            scanout,
            MEISO_K230D_DRM_FAILURE_RESTORE_READBACK,
            error == 0 ? EPROTO : error
        );
        scanout->cleanup_receipt.retained_resources = true;
        scanout->phase = MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN;
        return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
    }
    scanout->cleanup_receipt.property_readback_matched = true;
    scanout->present_commit_may_be_active = false;
    scanout->restore_commit_may_be_active = false;
    if (!meiso_k230d_drm_private_release_resources(scanout)) {
        return MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED;
    }
    return MEISO_K230D_DRM_CLEANUP_COMPLETE;
}

MeisoK230dDrmCleanupStatus meiso_k230d_drm_scanout_cleanup_step(
    MeisoK230dDrmScanout *scanout
)
{
    if (!meiso_k230d_drm_private_scanout_valid(scanout) ||
        scanout->callback_active) {
        return MEISO_K230D_DRM_CLEANUP_INVALID_ARGUMENT;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_STOPPED) {
        return MEISO_K230D_DRM_CLEANUP_UNCHANGED;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_PRESENT_PENDING) {
        return MEISO_K230D_DRM_CLEANUP_INVALID_ARGUMENT;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_RESTORE_PENDING) {
        return poll_restore(scanout);
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN) {
        return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED) {
        if (scanout->cleanup_callbacks_forbidden) {
            return MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED;
        }
        if (scanout->present_commit_may_be_active ||
            scanout->restore_commit_may_be_active) {
            return MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN;
        }
        return meiso_k230d_drm_private_release_resources(scanout) ?
            MEISO_K230D_DRM_CLEANUP_COMPLETE :
            MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED;
    }
    if (scanout->phase == MEISO_K230D_DRM_PHASE_KMS_LATCHED ||
        scanout->phase == MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN) {
        return begin_restore(scanout);
    }
    return MEISO_K230D_DRM_CLEANUP_INVALID_ARGUMENT;
}
