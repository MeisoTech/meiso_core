#include "meiso_runtime_scheduler_private.h"

#include <string.h>

/* Runtime/Session boundary reconciliation and receive obligations. */

static void call_boundary(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t generation
)
{
    MeisoRuntimeSchedulerSource *controls = meiso_runtime_scheduler_private_control_sources(scheduler);
    MeisoRuntimeSchedulerSource *data = meiso_runtime_scheduler_private_data_sources(scheduler);
    uint16_t index;

    scheduler->callback_active = true;
    for (index = 0U; index < scheduler->control_source_count; ++index) {
        controls[index].boundary(
            controls[index].context, boundary, generation
        );
    }
    for (index = 0U; index < scheduler->data_source_count; ++index) {
        data[index].boundary(data[index].context, boundary, generation);
    }
    scheduler->callback_active = false;
}

MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_private_refresh_boundary(
    MeisoRuntimeScheduler *scheduler,
    MeisoSessionSnapshot *out_session
)
{
    MeisoRuntimeSnapshot runtime_snapshot;
    MeisoRuntimeBlobSnapshot blob_snapshot;
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeStatus runtime_status;
    uint64_t generation;
    bool present;

    runtime_status = meiso_runtime_refresh_bound_session(scheduler->runtime);
    if (runtime_status == MEISO_RUNTIME_CONFLICT) {
        return MEISO_RUNTIME_SCHEDULER_REENTRANT;
    }
    if (runtime_status != MEISO_RUNTIME_OK &&
        runtime_status != MEISO_RUNTIME_CORE_NOT_READY &&
        runtime_status != MEISO_RUNTIME_STALE_SESSION) {
        return MEISO_RUNTIME_SCHEDULER_INVALID_STATE;
    }
    if (runtime_status != MEISO_RUNTIME_OK || !meiso_runtime_scheduler_private_session_runtime_ready(
            scheduler->runtime,
            scheduler->session,
            &runtime_snapshot,
            &session_snapshot
        ) || !meiso_session_id_equal(
            scheduler->session_id, session_snapshot.session_id
        ) || !meiso_runtime_blob_snapshot(
            scheduler->runtime, &blob_snapshot
        ) || blob_snapshot.wire_version != scheduler->wire_version) {
        if (scheduler->phase !=
            MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
            meiso_runtime_scheduler_private_clear_route_bindings(scheduler);
            scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
            scheduler->data_probe_due = false;
            scheduler->active_catalog_present = false;
            scheduler->catalog_generation = 0U;
            scheduler->phase =
                MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL;
            call_boundary(
                scheduler,
                MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL,
                0U
            );
        }
        return MEISO_RUNTIME_SCHEDULER_STALE_SESSION;
    }
    if (out_session != NULL) {
        *out_session = session_snapshot;
    }
    present = runtime_snapshot.active_catalog_present;
    generation = present ? runtime_snapshot.active_catalog_generation : 0U;
    if (scheduler->sealed &&
        (present != scheduler->active_catalog_present ||
         generation != scheduler->catalog_generation)) {
        meiso_runtime_scheduler_private_clear_route_bindings(scheduler);
        scheduler->active_catalog_present = present;
        scheduler->catalog_generation = generation;
        scheduler->successful_control_burst = 0U;
        scheduler->data_probe_due = false;
        scheduler->phase = present ?
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED :
            MEISO_RUNTIME_SCHEDULER_PHASE_READY;
        call_boundary(
            scheduler,
            MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED,
            generation
        );
        if (present) {
            return MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED;
        }
    }
    return MEISO_RUNTIME_SCHEDULER_OK;
}

MeisoRuntimeStatus meiso_runtime_scheduler_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
)
{
    MeisoRuntimeStatus status;
    MeisoRuntimeBlobSnapshot blob;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) || message == NULL ||
        out_event == NULL || meiso_runtime_scheduler_private_output_overlaps(
            scheduler, out_event, sizeof(*out_event)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active || !scheduler->sealed ||
        scheduler->phase ==
            MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_receive(scheduler->runtime, message, out_event);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (out_event->type == MEISO_RUNTIME_EVENT_STATE_OFFER) {
        scheduler->rx_obligation =
            MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT;
    } else if (out_event->type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
        out_event->blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
        meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
        blob.publish_pending) {
        scheduler->rx_obligation =
            MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH;
    }
    return MEISO_RUNTIME_OK;
}

MeisoRuntimeStatus meiso_runtime_scheduler_commit_state_receive(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
)
{
    MeisoRuntimeStatus status;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler)) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation !=
        MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_commit_state_receive(
        scheduler->runtime, route, offer, receive_status
    );
    scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
    return status;
}

MeisoRuntimeStatus meiso_runtime_scheduler_publish_blob_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status;
    MeisoRuntimeBlobSnapshot blob;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) ||
        meiso_runtime_scheduler_private_output_overlaps(
            scheduler, out_report, sizeof(*out_report)
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_CONFLICT;
    }
    if (scheduler->rx_obligation !=
        MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH) {
        return MEISO_RUNTIME_CONFLICT;
    }
    status = meiso_runtime_publish_blob_receive(
        scheduler->runtime,
        expected_transfer,
        store,
        verifier,
        verifier_context,
        out_report
    );
    if (meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
        !blob.publish_pending) {
        scheduler->rx_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
    }
    return status;
}
