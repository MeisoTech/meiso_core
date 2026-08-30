#include "meiso_composition_control_session_private.h"

#include <errno.h>
#include <string.h>

/* Fair pumping, RX obligations, readiness, terminal lifecycle, and snapshots. */

static bool session_is_ready(MeisoCompositionControlSession *control_session)
{
    MeisoSessionSnapshot snapshot;

    return meiso_session_snapshot(control_session->session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_READY &&
        snapshot.session_id_known;
}

static bool scheduler_has_obligation(
    MeisoCompositionControlSession *control_session,
    MeisoRuntimeSchedulerRxObligation *out_obligation
)
{
    MeisoRuntimeSchedulerSnapshot snapshot;

    if (!control_session->scheduler_present ||
        !meiso_runtime_scheduler_snapshot(control_session->scheduler, &snapshot)) {
        if (out_obligation != NULL) {
            *out_obligation = MEISO_RUNTIME_SCHEDULER_RX_NONE;
        }
        return false;
    }
    if (out_obligation != NULL) {
        *out_obligation = snapshot.rx_obligation;
    }
    return snapshot.rx_obligation != MEISO_RUNTIME_SCHEDULER_RX_NONE;
}

static MeisoCompositionControlSessionPumpStatus pump_read(
    MeisoCompositionControlSession *control_session,
    MeisoCompositionControlSessionPumpResult *out_result
)
{
    MeisoSessionRuntimeMessage message;
    MeisoRuntimeEvent event;
    MeisoCompositionControlSessionPumpResult result;
    MeisoPosixTcpReadStatus read_status;
    MeisoRuntimeStatus runtime_status;
    MeisoRuntimeSchedulerRxObligation obligation;

    memset(&message, 0, sizeof(message));
    memset(&event, 0, sizeof(event));
    memset(&result, 0, sizeof(result));
    read_status = meiso_posix_tcp_on_readable(control_session->adapter, &message);
    result.read_attempted = true;
    result.read_status = read_status;
    switch (read_status) {
    case MEISO_POSIX_TCP_READ_PROGRESS:
    case MEISO_POSIX_TCP_READ_SESSION_PROGRESS:
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS;
    case MEISO_POSIX_TCP_READ_WOULD_BLOCK:
    case MEISO_POSIX_TCP_READ_INTERRUPTED:
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK;
    case MEISO_POSIX_TCP_READ_RUNTIME:
        if (!control_session->scheduler_present) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
        }
        runtime_status = meiso_runtime_scheduler_receive(
            control_session->scheduler, &message, &event
        );
        if (runtime_status != MEISO_RUNTIME_OK) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
        }
        if (event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED) {
            if (control_session->local_role != MEISO_SESSION_ROLE_HOST ||
                event.catalog_generation != control_session->catalog_generation ||
                !meiso_composition_control_session_private_staged_catalog_exact(control_session) ||
                !meiso_composition_control_session_private_queue_internal_catalog(control_session)) {
                meiso_composition_control_session_private_terminate(control_session, EPROTO);
                return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
            }
        } else if (event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
                   (control_session->local_role != MEISO_SESSION_ROLE_EDGE ||
                    event.catalog_generation != control_session->catalog_generation)) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
        }
        result.event = event;
        result.event_present = true;
        if (scheduler_has_obligation(control_session, &obligation)) {
            result.rx_obligation = obligation;
            control_session->phase =
                MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION;
        }
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT;
    case MEISO_POSIX_TCP_READ_PEER_CLOSED:
    case MEISO_POSIX_TCP_READ_SESSION_FAILED:
    case MEISO_POSIX_TCP_READ_TRANSPORT_LOST:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL;
    case MEISO_POSIX_TCP_READ_INVALID_ARGUMENT:
    default:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
    }
}

static MeisoCompositionControlSessionPumpStatus pump_write(
    MeisoCompositionControlSession *control_session,
    MeisoCompositionControlSessionPumpResult *out_result
)
{
    MeisoCompositionControlSessionPumpResult result;
    MeisoPosixTcpWriteStatus write_status;

    memset(&result, 0, sizeof(result));
    write_status = meiso_posix_tcp_on_writable(control_session->adapter);
    result.write_attempted = true;
    result.write_status = write_status;
    switch (write_status) {
    case MEISO_POSIX_TCP_WRITE_PROGRESS:
    case MEISO_POSIX_TCP_WRITE_FRAME_SENT:
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS;
    case MEISO_POSIX_TCP_WRITE_WOULD_BLOCK:
    case MEISO_POSIX_TCP_WRITE_INTERRUPTED:
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK;
    case MEISO_POSIX_TCP_WRITE_IDLE:
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE;
    case MEISO_POSIX_TCP_WRITE_TRANSPORT_LOST:
    case MEISO_POSIX_TCP_WRITE_SESSION_INVALID:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL;
    case MEISO_POSIX_TCP_WRITE_INVALID_ARGUMENT:
    default:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
    }
}

static MeisoCompositionControlSessionPumpStatus pump_scheduler(
    MeisoCompositionControlSession *control_session,
    MeisoCompositionControlSessionPumpResult *out_result
)
{
    MeisoRuntimeSchedulerStepResult step;
    MeisoCompositionControlSessionPumpResult result;
    MeisoRuntimeSchedulerStepStatus status;

    memset(&step, 0, sizeof(step));
    memset(&result, 0, sizeof(result));
    status = meiso_runtime_scheduler_step(control_session->scheduler, &step);
    switch (status) {
    case MEISO_RUNTIME_SCHEDULER_STEP_SENT:
        result.scheduler_step = step;
        result.scheduler_step_present = true;
        for (uint8_t index = 0U;
             index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
            if (!control_session->sources[index].internal_catalog &&
                control_session->sources[index].source_id ==
                step.completion.source_id) {
                result.completed_source_index = index;
                result.completed_source_present = true;
                break;
            }
        }
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS;
    case MEISO_RUNTIME_SCHEDULER_STEP_IDLE:
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE;
    case MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY:
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY;
    case MEISO_RUNTIME_SCHEDULER_STEP_REJECTED:
        result.scheduler_step = step;
        result.scheduler_step_present = true;
        for (uint8_t index = 0U;
             index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
            if (!control_session->sources[index].internal_catalog &&
                control_session->sources[index].source_id ==
                step.completion.source_id) {
                result.completed_source_index = index;
                result.completed_source_present = true;
                break;
            }
        }
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_REJECTED;
    case MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID:
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_INVALID;
    case MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED:
        if (!meiso_composition_control_session_private_rebind_sources(control_session)) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_REBIND_PROGRESS;
    case MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION:
        control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_RX_OBLIGATION;
    case MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL;
    case MEISO_RUNTIME_SCHEDULER_STEP_COUNTER_EXHAUSTED:
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_COUNTER_EXHAUSTED;
    case MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT:
    case MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE:
    case MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT:
    default:
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
    }
}

MeisoCompositionControlSessionPumpStatus meiso_composition_control_session_pump(
    MeisoCompositionControlSession *control_session,
    bool readable,
    bool writable,
    MeisoCompositionControlSessionPumpResult *out_result
)
{
    MeisoRuntimeSchedulerRxObligation obligation;
    uint8_t scan;

    if (!meiso_composition_control_session_private_valid(control_session) || out_result == NULL || meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, out_result, sizeof(*out_result)
        )) {
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INVALID_ARGUMENT;
    }
    if (control_session->callback_active) {
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INVALID_ARGUMENT;
    }
    if (control_session->terminal) {
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL;
    }
    if (scheduler_has_obligation(control_session, &obligation)) {
        control_session->phase = MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_RX_OBLIGATION;
    }
    if (!control_session->runtime_present && session_is_ready(control_session)) {
        MeisoCompositionControlSessionPumpResult result;
        memset(&result, 0, sizeof(result));
        if (!meiso_composition_control_session_private_activate_runtime(control_session)) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE;
        }
        *out_result = result;
        return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS;
    }
    for (scan = 0U; scan < 3U; ++scan) {
        const uint8_t candidate = (uint8_t)((control_session->pump_cursor + scan) % 3U);
        if (candidate == 0U && readable &&
            meiso_posix_tcp_wants_read(control_session->adapter)) {
            control_session->pump_cursor = 1U;
            return pump_read(control_session, out_result);
        }
        if (candidate == 1U && writable &&
            meiso_posix_tcp_wants_write(control_session->adapter)) {
            control_session->pump_cursor = 2U;
            return pump_write(control_session, out_result);
        }
        if (candidate == 2U && control_session->scheduler_present) {
            control_session->pump_cursor = 0U;
            return pump_scheduler(control_session, out_result);
        }
    }
    if (!control_session->scheduler_present) {
        MeisoSessionSnapshot snapshot;
        if (!meiso_session_snapshot(control_session->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            meiso_composition_control_session_private_terminate(control_session, EPROTO);
            return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL;
        }
    }
    {
        MeisoCompositionControlSessionPumpResult result;
        memset(&result, 0, sizeof(result));
        *out_result = result;
    }
    return MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE;
}

static MeisoCompositionControlSessionStatus runtime_result_status(
    MeisoCompositionControlSession *control_session,
    MeisoRuntimeStatus status
)
{
    if (status == MEISO_RUNTIME_OK) {
        MeisoRuntimeSchedulerSnapshot snapshot;
        if (meiso_runtime_scheduler_snapshot(control_session->scheduler, &snapshot) &&
            snapshot.rx_obligation == MEISO_RUNTIME_SCHEDULER_RX_NONE) {
            control_session->phase = snapshot.phase ==
                MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED ?
                    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_REBIND :
                    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY;
        }
        return MEISO_COMPOSITION_CONTROL_SESSION_OK;
    }
    if (status == MEISO_RUNTIME_CORE_NOT_READY ||
        status == MEISO_RUNTIME_STALE_SESSION) {
        meiso_composition_control_session_private_terminate(control_session, EPROTO);
        return MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL;
    }
    return status == MEISO_RUNTIME_INVALID_ARGUMENT ?
        MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT :
        MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
}

MeisoCompositionControlSessionStatus
meiso_composition_control_session_commit_state_receive(
    MeisoCompositionControlSession *control_session,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
)
{
    if (!meiso_composition_control_session_private_valid(control_session) || offer == NULL || meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, offer, sizeof(*offer)
        )) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    if (control_session->callback_active) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    if (control_session->terminal) {
        return MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL;
    }
    if (!control_session->scheduler_present || control_session->phase !=
            MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    return runtime_result_status(
        control_session,
        meiso_runtime_scheduler_commit_state_receive(
            control_session->scheduler, route, offer, receive_status
        )
    );
}

MeisoCompositionControlSessionStatus
meiso_composition_control_session_publish_blob_receive(
    MeisoCompositionControlSession *control_session,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
)
{
    MeisoRuntimeStatus status;

    if (!meiso_composition_control_session_private_valid(control_session) || expected_transfer == NULL || store == NULL ||
        verifier == NULL || out_report == NULL || meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, expected_transfer,
            sizeof(*expected_transfer)
        ) || meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, out_report, sizeof(*out_report)
        ) || meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, store, 1U) ||
        meiso_composition_control_session_private_ranges_overlap(control_session, control_session->arena_size, verifier_context, 1U) ||
        meiso_composition_control_session_private_ranges_overlap(
            expected_transfer, sizeof(*expected_transfer),
            out_report, sizeof(*out_report)
        ) || meiso_composition_control_session_private_ranges_overlap(store, 1U, out_report, sizeof(*out_report)) ||
        meiso_composition_control_session_private_ranges_overlap(
            verifier_context, 1U, out_report, sizeof(*out_report)
        )) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT;
    }
    if (control_session->callback_active) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    if (control_session->terminal) {
        return MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL;
    }
    if (!control_session->scheduler_present || control_session->phase !=
            MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION) {
        return MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE;
    }
    control_session->callback_active = true;
    status = meiso_runtime_scheduler_publish_blob_receive(
        control_session->scheduler,
        expected_transfer,
        store,
        verifier,
        verifier_context,
        out_report
    );
    control_session->callback_active = false;
    return runtime_result_status(control_session, status);
}

bool meiso_composition_control_session_wants_read(
    const MeisoCompositionControlSession *control_session
)
{
    return meiso_composition_control_session_private_valid(control_session) && !control_session->callback_active && !control_session->terminal &&
        meiso_posix_tcp_wants_read(control_session->adapter);
}

bool meiso_composition_control_session_wants_write(
    const MeisoCompositionControlSession *control_session
)
{
    return meiso_composition_control_session_private_valid(control_session) && !control_session->callback_active && !control_session->terminal &&
        meiso_posix_tcp_wants_write(control_session->adapter);
}

bool meiso_composition_control_session_snapshot(
    const MeisoCompositionControlSession *control_session,
    MeisoCompositionControlSessionSnapshot *out_snapshot
)
{
    MeisoCompositionControlSessionSnapshot snapshot;
    size_t index;

    if (!meiso_composition_control_session_private_valid(control_session) || control_session->callback_active || out_snapshot == NULL || meiso_composition_control_session_private_ranges_overlap(
            control_session, control_session->arena_size, out_snapshot, sizeof(*out_snapshot)
        )) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (!meiso_session_snapshot(control_session->session, &snapshot.session)) {
        return false;
    }
    snapshot.phase = control_session->phase;
    snapshot.catalog_generation = control_session->catalog_generation;
    snapshot.next_ticket = control_session->next_ticket;
    snapshot.boundary_serial = control_session->boundary_serial;
    snapshot.last_boundary_generation = control_session->last_boundary_generation;
    snapshot.last_boundary = control_session->last_boundary;
    snapshot.arena_bytes = control_session->arena_size;
    snapshot.session_bytes = control_session->session_bytes;
    snapshot.adapter_reserve_bytes = control_session->adapter_reserve_bytes;
    snapshot.adapter_bytes = control_session->adapter_bytes;
    snapshot.runtime_reserve_bytes = control_session->runtime_reserve_bytes;
    snapshot.runtime_bytes = control_session->runtime_bytes;
    snapshot.scheduler_reserve_bytes = control_session->scheduler_reserve_bytes;
    snapshot.scheduler_bytes = control_session->scheduler_bytes;
    snapshot.runtime_present = control_session->runtime_present;
    snapshot.scheduler_present = control_session->scheduler_present;
    snapshot.terminal = control_session->terminal;
    if (control_session->runtime_present &&
        !meiso_runtime_snapshot(control_session->runtime, &snapshot.runtime)) {
        return false;
    }
    if (control_session->scheduler_present && !meiso_runtime_scheduler_snapshot(
            control_session->scheduler, &snapshot.scheduler
        )) {
        return false;
    }
    for (index = 0U;
         index < MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT; ++index) {
        if (control_session->sources[index].pending ||
            control_session->sources[index].completion_pending) {
            ++snapshot.pending_source_count;
        }
    }
    *out_snapshot = snapshot;
    return true;
}

void meiso_composition_control_session_abort(
    MeisoCompositionControlSession *control_session,
    int error_number
)
{
    MeisoRuntimeSchedulerStepResult ignored;

    if (!meiso_composition_control_session_private_valid(control_session) || control_session->terminal || control_session->callback_active) {
        return;
    }
    meiso_posix_tcp_abort(control_session->adapter, error_number);
    if (control_session->scheduler_present) {
        (void)meiso_runtime_scheduler_step(control_session->scheduler, &ignored);
    }
    meiso_composition_control_session_private_mark_terminal(control_session);
}
