#include "meiso_runtime_scheduler_private.h"

#include <string.h>

/* One-slot scheduling transaction and read-only scheduler snapshot. */

MeisoRuntimeSchedulerStepStatus meiso_runtime_scheduler_step(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerStepResult *out_result
)
{
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeSchedulerStatus boundary_status;
    MeisoRuntimeBlobSnapshot blob;
    MeisoRuntimeSchedulerSource *source = NULL;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeBlobTransferRef generated;
    MeisoRuntimeStatus runtime_status;
    uint16_t source_index = 0U;
    bool source_invalid = false;
    bool invalid_seen = false;
    bool control_scanned = false;
    bool data_probe_performed = false;
    bool generated_present = false;
    bool new_data_round = false;
    bool found;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerStepStatus step_status;

    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) || out_result == NULL ||
        meiso_runtime_scheduler_private_output_overlaps(scheduler, out_result, sizeof(*out_result))) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT;
    }
    if (scheduler->callback_active) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT;
    }
    if (!scheduler->sealed) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE;
    }
    boundary_status = meiso_runtime_scheduler_private_refresh_boundary(scheduler, &session_snapshot);
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_REENTRANT) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_INVALID_STATE) {
        return MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_STALE_SESSION) {
        return MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL;
    }
    if (boundary_status == MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED ||
        scheduler->phase ==
            MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED) {
        return MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED;
    }
    if (scheduler->rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE ||
        (meiso_runtime_blob_snapshot(scheduler->runtime, &blob) &&
         blob.publish_pending)) {
        return MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION;
    }
    if (scheduler->phase ==
        MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL) {
        return MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL;
    }
    if (session_snapshot.outbound_pending) {
        return MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY;
    }
    memset(&result, 0, sizeof(result));

    if (scheduler->phase == MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND) {
        found = meiso_runtime_scheduler_private_find_data_work(
            scheduler,
            &source,
            &source_index,
            &work,
            &source_invalid
        );
        invalid_seen = source_invalid;
        if (!found) {
            source_invalid = false;
            found = meiso_runtime_scheduler_private_find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = invalid_seen || source_invalid;
        }
    } else {
        found = false;
        if (!scheduler->data_probe_due &&
            scheduler->successful_control_burst <
                MEISO_RUNTIME_SCHEDULER_CONTROL_BURST) {
            control_scanned = true;
            found = meiso_runtime_scheduler_private_find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = source_invalid;
        }
        if (!found) {
            data_probe_performed = true;
            source_invalid = false;
            if (meiso_runtime_scheduler_private_begin_data_round(scheduler, &source_invalid)) {
                new_data_round = true;
                found = meiso_runtime_scheduler_private_find_data_work(
                    scheduler,
                    &source,
                    &source_index,
                    &work,
                    &source_invalid
                );
            }
            invalid_seen = invalid_seen || source_invalid;
        }
        if (!found && !control_scanned) {
            new_data_round = false;
            source_invalid = false;
            found = meiso_runtime_scheduler_private_find_control_work(
                scheduler,
                &source,
                &source_index,
                &work,
                &source_invalid
            );
            invalid_seen = invalid_seen || source_invalid;
        }
    }
    if (!found) {
        if (data_probe_performed) {
            scheduler->data_probe_due = false;
        }
        return invalid_seen ?
            MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID :
            MEISO_RUNTIME_SCHEDULER_STEP_IDLE;
    }
    if (!meiso_runtime_scheduler_private_counters_available(
            scheduler, source->source_class, new_data_round
        )) {
        if (new_data_round) {
            meiso_runtime_scheduler_private_rollback_new_data_round(scheduler);
        }
        return MEISO_RUNTIME_SCHEDULER_STEP_COUNTER_EXHAUSTED;
    }
    runtime_status = meiso_runtime_scheduler_private_dispatch_work(
        scheduler,
        &work,
        &generated,
        &generated_present
    );
    step_status = meiso_runtime_scheduler_private_finish_attempt(
        scheduler,
        source,
        source_index,
        &work,
        new_data_round,
        runtime_status,
        &generated,
        generated_present,
        data_probe_performed,
        &result
    );
    if (step_status != MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY) {
        *out_result = result;
    }
    return step_status;
}

bool meiso_runtime_scheduler_snapshot(
    const MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSnapshot *out_snapshot
)
{
    if (!meiso_runtime_scheduler_private_layout_valid(scheduler) || out_snapshot == NULL ||
        scheduler->callback_active || meiso_runtime_scheduler_private_output_overlaps(
            scheduler, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->session_id = scheduler->session_id;
    out_snapshot->catalog_generation = scheduler->catalog_generation;
    out_snapshot->successful_slots = scheduler->successful_slots;
    out_snapshot->successful_control_slots =
        scheduler->successful_control_slots;
    out_snapshot->successful_data_slots =
        scheduler->successful_data_slots;
    out_snapshot->data_rounds = scheduler->data_rounds;
    out_snapshot->control_source_count = scheduler->control_source_count;
    out_snapshot->data_source_count = scheduler->data_source_count;
    out_snapshot->data_round_member_count =
        scheduler->data_round_member_count;
    out_snapshot->successful_control_burst =
        scheduler->successful_control_burst;
    out_snapshot->phase = scheduler->phase;
    out_snapshot->rx_obligation = scheduler->rx_obligation;
    out_snapshot->data_probe_due = scheduler->data_probe_due;
    out_snapshot->sealed = scheduler->sealed;
    return true;
}
