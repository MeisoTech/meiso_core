#include "adapters/meiso_drm_single_plane_test_only_supervisor_private.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig config;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorOps ops;
    void *context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;
    uint64_t previous_now_ns;
} SupervisorExecution;

typedef enum {
    TIMED_WAIT_REAPED,
    TIMED_WAIT_DEADLINE,
    TIMED_WAIT_FAIL_SAFE
} TimedWaitResult;

static bool ranges_overlap(
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

static bool bytes_are_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool byte_flag_valid(uint8_t value)
{
    return value == 0U || value == 1U;
}

static uint32_t normalize_callback_error(int callback_result)
{
    return callback_result < 0 ? (uint32_t)EPROTO :
        (uint32_t)callback_result;
}

static void increment_counter(
    SupervisorExecution *execution,
    uint32_t *counter
)
{
    if (*counter == UINT32_MAX) {
        execution->result.counters_saturated = 1U;
        return;
    }
    ++*counter;
}

static void record_failure(
    SupervisorExecution *execution,
    MeisoK230dDrmSupervisorFailure failure,
    uint32_t error_number
)
{
    if (execution->result.first_failure ==
        MEISO_K230D_DRM_SUPERVISOR_FAILURE_NONE) {
        execution->result.first_failure = failure;
        execution->result.first_failure_error = error_number;
    }
}

static void record_callback_failure(
    SupervisorExecution *execution,
    MeisoK230dDrmSupervisorFailure operation_failure,
    uint32_t error_number,
    bool protocol_failure
)
{
    record_failure(
        execution,
        protocol_failure ?
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL :
            operation_failure,
        error_number
    );
}

static void update_outcome(SupervisorExecution *execution)
{
    if (execution->result.child_spawned == 0U) {
        execution->result.outcome =
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_NO_CHILD_FAILURE;
    } else if (execution->result.quarantine_record_attempted != 0U) {
        execution->result.outcome =
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_QUARANTINE_ATTEMPT;
    } else if (execution->result.kill_call_count != 0U) {
        execution->result.outcome =
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_KILL_CALL;
    } else if (execution->result.terminate_call_count != 0U) {
        execution->result.outcome =
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_TERM_CALL;
    } else {
        execution->result.outcome =
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_WITHOUT_SIGNAL_CALL;
    }
}

static bool supervisor_ops_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorOps *ops
)
{
    return ops->inspect_preflight != NULL &&
        ops->claim_global_launch_once != NULL &&
        ops->claim_evidence_launch_once != NULL &&
        ops->spawn_one_child != NULL &&
        ops->await_child_ready_and_sigstop != NULL &&
        ops->publish_launch_ready_record != NULL &&
        ops->release_gate_and_sample_post_cont_t0 != NULL &&
        ops->wait_owned_child_until != NULL &&
        ops->monotonic_now != NULL &&
        ops->terminate_owned_child_once != NULL &&
        ops->kill_owned_child_once != NULL &&
        ops->publish_quarantine_record != NULL &&
        ops->wait_owned_child_to_reap != NULL &&
        ops->inspect_postflight != NULL &&
        ops->publish_terminal_result != NULL;
}

static bool binding_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding
)
{
    return !bytes_are_zero(binding->run_nonce, sizeof(binding->run_nonce)) &&
        !bytes_are_zero(
            binding->expected_boot_id_sha256,
            sizeof(binding->expected_boot_id_sha256)
        ) &&
        !bytes_are_zero(
            binding->expected_artifact_sha256,
            sizeof(binding->expected_artifact_sha256)
        ) &&
        !bytes_are_zero(
            binding->derive_request.expected_topology_sha256,
            sizeof(binding->derive_request.expected_topology_sha256)
        ) &&
        binding->derive_request.connector_id != 0U &&
        binding->derive_request.crtc_id != 0U;
}

static MeisoK230dDrmSupervisorInvokeStatus validate_parent_invocation(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig *config,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorOps *ops,
    void *context,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *out_result
)
{
    if (config == NULL || ops == NULL || out_result == NULL) {
        return MEISO_K230D_DRM_SUPERVISOR_INVALID_ARGUMENT;
    }
    if (ranges_overlap(config, sizeof(*config), ops, sizeof(*ops)) ||
        ranges_overlap(config, sizeof(*config), out_result,
                       sizeof(*out_result)) ||
        ranges_overlap(ops, sizeof(*ops), out_result,
                       sizeof(*out_result)) ||
        !binding_valid(&config->binding)) {
        return MEISO_K230D_DRM_SUPERVISOR_INVALID_ARGUMENT;
    }
    if (context == NULL || !supervisor_ops_valid(ops) ||
        config->term_after_ns == 0U ||
        config->kill_after_ns <= config->term_after_ns ||
        config->quarantine_after_ns <= config->kill_after_ns) {
        return MEISO_K230D_DRM_SUPERVISOR_INVALID_CONFIG;
    }
    return MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE;
}

static void seed_preflight(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *preflight
)
{
    memset(preflight, 0xff, sizeof(*preflight));
    preflight->primary_holder_count = UINT32_MAX;
}

static bool preflight_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *preflight
)
{
    return byte_flag_valid(preflight->boot_identity_matches) &&
        byte_flag_valid(preflight->artifact_identity_matches) &&
        byte_flag_valid(preflight->primary_node_identity_matches) &&
        byte_flag_valid(preflight->holder_scan_complete) &&
        byte_flag_valid(preflight->single_thread_verified) &&
        bytes_are_zero(preflight->reserved, sizeof(preflight->reserved)) &&
        preflight->primary_holder_count != UINT32_MAX &&
        (preflight->holder_scan_complete != 0U ||
         preflight->primary_holder_count == 0U);
}

static bool preflight_admits(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *preflight
)
{
    return preflight->boot_identity_matches == 1U &&
        preflight->artifact_identity_matches == 1U &&
        preflight->primary_node_identity_matches == 1U &&
        preflight->holder_scan_complete == 1U &&
        preflight->single_thread_verified == 1U &&
        preflight->primary_holder_count == 0U;
}

static uint32_t preflight_denial_error(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *preflight
)
{
    if (preflight->holder_scan_complete == 0U) {
        return (uint32_t)EIO;
    }
    return (uint32_t)EBUSY;
}

static uint32_t call_preflight(
    SupervisorExecution *execution,
    uint8_t index,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *out_preflight,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding frozen_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight seed;
    int callback_result;

    binding = execution->config.binding;
    frozen_binding = binding;
    seed_preflight(&candidate);
    seed = candidate;
    increment_counter(execution, &execution->result.preflight_call_count);
    callback_result = execution->ops.inspect_preflight(
        execution->context, &binding, index, &candidate
    );
    *out_protocol_failure = callback_result < 0 ||
        memcmp(&binding, &frozen_binding, sizeof(binding)) != 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (*out_protocol_failure || !preflight_valid(&candidate)) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    *out_preflight = candidate;
    return 0U;
}

static void seed_durable(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *durable
)
{
    memset(durable, 0xff, sizeof(*durable));
    durable->error_number = UINT32_MAX;
}

static bool durable_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *durable
)
{
    if (!bytes_are_zero(durable->reserved, sizeof(durable->reserved))) {
        return false;
    }
    switch (durable->disposition) {
        case MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED:
            return durable->progress ==
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PARENT_DIRECTORY_SYNCED &&
                durable->error_number == 0U;
        case MEISO_K230D_DRM_SUPERVISOR_DURABLE_ALREADY_EXISTS:
            return durable->progress ==
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE &&
                durable->error_number == (uint32_t)EEXIST;
        case MEISO_K230D_DRM_SUPERVISOR_DURABLE_FAILED_CLEAN:
            return durable->progress ==
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE &&
                durable->error_number > 0U &&
                durable->error_number != (uint32_t)EEXIST &&
                durable->error_number != UINT32_MAX;
        case MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN:
            return (durable->progress ==
                        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PRIVATE_BODY_CREATED ||
                    durable->progress ==
                        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_BODY_SYNCED ||
                    durable->progress ==
                        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNIQUE_NAME_PUBLISHED ||
                    durable->progress ==
                        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN) &&
                durable->error_number > 0U &&
                durable->error_number != UINT32_MAX;
        default:
            return false;
    }
}

static void set_uncertain_durable(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *durable,
    uint32_t error_number
)
{
    memset(durable, 0, sizeof(*durable));
    durable->disposition =
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN;
    durable->progress =
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN;
    durable->error_number = error_number;
}

static uint32_t call_binding_durable(
    SupervisorExecution *execution,
    int (*callback)(
        void *,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *
    ),
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_durable,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding frozen_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate seed;
    uint32_t error_number;
    int callback_result;

    binding = execution->config.binding;
    frozen_binding = binding;
    seed_durable(&candidate);
    seed = candidate;
    callback_result = callback(execution->context, &binding, &candidate);
    *out_protocol_failure = callback_result < 0 ||
        memcmp(&binding, &frozen_binding, sizeof(binding)) != 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        error_number = *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
        set_uncertain_durable(out_durable, error_number);
        return error_number;
    }
    if (*out_protocol_failure || !durable_valid(&candidate)) {
        *out_protocol_failure = true;
        set_uncertain_durable(out_durable, (uint32_t)EPROTO);
        return (uint32_t)EPROTO;
    }
    *out_durable = candidate;
    return 0U;
}

static uint32_t call_result_durable(
    SupervisorExecution *execution,
    int (*callback)(
        void *,
        const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *,
        MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *
    ),
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_durable,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult input;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult frozen_input;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate seed;
    uint32_t error_number;
    int callback_result;

    memcpy(&input, &execution->result, sizeof(input));
    memcpy(&frozen_input, &input, sizeof(frozen_input));
    seed_durable(&candidate);
    seed = candidate;
    callback_result = callback(execution->context, &input, &candidate);
    *out_protocol_failure = callback_result < 0 ||
        memcmp(&input, &frozen_input, sizeof(input)) != 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        error_number = *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
        set_uncertain_durable(out_durable, error_number);
        return error_number;
    }
    if (*out_protocol_failure || !durable_valid(&candidate)) {
        *out_protocol_failure = true;
        set_uncertain_durable(out_durable, (uint32_t)EPROTO);
        return (uint32_t)EPROTO;
    }
    *out_durable = candidate;
    return 0U;
}

static void seed_gate(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *gate
)
{
    memset(gate, 0xff, sizeof(*gate));
    gate->child_exit_code = UINT16_MAX;
    gate->child_start_ticks = UINT64_MAX;
}

static bool gate_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *gate
)
{
    bool common = byte_flag_valid(gate->ready_byte_observed) &&
        byte_flag_valid(gate->exact_wait_child_matched) &&
        byte_flag_valid(gate->exit_code_valid) &&
        byte_flag_valid(gate->child_start_ticks_valid) &&
        gate->reserved == 0U &&
        bytes_are_zero(gate->reserved_tail, sizeof(gate->reserved_tail));

    if (!common) {
        return false;
    }
    switch (gate->kind) {
        case MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED:
            return gate->ready_byte_observed == 1U &&
                gate->exact_wait_child_matched == 1U &&
                gate->exit_code_valid == 0U &&
                gate->child_exit_code == 0U && gate->child_signal == 0U &&
                gate->child_start_ticks_valid == 1U &&
                gate->child_start_ticks > 0U &&
                gate->child_start_ticks != UINT64_MAX;
        case MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED:
            return gate->exact_wait_child_matched == 1U &&
                gate->exit_code_valid == 1U &&
                gate->child_exit_code <= 255U && gate->child_signal == 0U &&
                gate->child_start_ticks_valid == 0U &&
                gate->child_start_ticks == 0U;
        case MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_SIGNALED:
            return gate->exact_wait_child_matched == 1U &&
                gate->exit_code_valid == 0U && gate->child_exit_code == 0U &&
                gate->child_signal > 0U &&
                gate->child_signal <=
                    MEISO_K230D_DRM_SUPERVISOR_MAX_SIGNAL_NUMBER &&
                gate->child_start_ticks_valid == 0U &&
                gate->child_start_ticks == 0U;
        case MEISO_K230D_DRM_SUPERVISOR_GATE_INTERRUPTED:
        case MEISO_K230D_DRM_SUPERVISOR_GATE_STOP_REQUESTED:
            return gate->exact_wait_child_matched == 0U &&
                gate->exit_code_valid == 0U && gate->child_exit_code == 0U &&
                gate->child_signal == 0U &&
                gate->child_start_ticks_valid == 0U &&
                gate->child_start_ticks == 0U;
        default:
            return false;
    }
}

static uint32_t call_gate(
    SupervisorExecution *execution,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *out_gate,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation seed;
    int callback_result;

    seed_gate(&candidate);
    seed = candidate;
    increment_counter(
        execution, &execution->result.gate_observation_call_count
    );
    callback_result = execution->ops.await_child_ready_and_sigstop(
        execution->context, execution->result.child_pid, &candidate
    );
    *out_protocol_failure = callback_result < 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (!gate_valid(&candidate)) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    *out_gate = candidate;
    return 0U;
}

static void gate_to_final_reap(
    SupervisorExecution *execution,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *gate
)
{
    memset(&execution->result.final_reap, 0,
           sizeof(execution->result.final_reap));
    if (gate->kind == MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED) {
        execution->result.final_reap.kind =
            MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED;
        execution->result.final_reap.exit_code_valid = 1U;
        execution->result.final_reap.child_exit_code = gate->child_exit_code;
    } else {
        execution->result.final_reap.kind =
            MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED;
        execution->result.final_reap.child_signal = gate->child_signal;
    }
    execution->result.child_reaped = 1U;
}

static void seed_release(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *release
)
{
    memset(release, 0xff, sizeof(*release));
    release->error_number = UINT32_MAX;
    release->post_cont_t0_ns = UINT64_MAX;
}

static bool release_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *release
)
{
    if (!byte_flag_valid(release->sigcont_dispatch_known) ||
        !byte_flag_valid(release->sigcont_dispatched) ||
        !byte_flag_valid(release->t0_valid)) {
        return false;
    }
    switch (release->kind) {
        case MEISO_K230D_DRM_SUPERVISOR_RELEASE_NOT_RELEASED:
            return release->sigcont_dispatch_known == 1U &&
                release->sigcont_dispatched == 0U &&
                release->t0_valid == 0U && release->error_number > 0U &&
                release->error_number != UINT32_MAX &&
                release->post_cont_t0_ns == UINT64_MAX;
        case MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0:
            return release->sigcont_dispatch_known == 1U &&
                release->sigcont_dispatched == 1U &&
                release->t0_valid == 1U && release->error_number == 0U &&
                release->post_cont_t0_ns != UINT64_MAX;
        case MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITHOUT_T0:
            return release->sigcont_dispatch_known == 1U &&
                release->sigcont_dispatched == 1U &&
                release->t0_valid == 0U && release->error_number > 0U &&
                release->error_number != UINT32_MAX &&
                release->post_cont_t0_ns == UINT64_MAX;
        case MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN:
            return release->sigcont_dispatch_known == 0U &&
                release->sigcont_dispatched == 0U &&
                release->t0_valid == 0U && release->error_number > 0U &&
                release->error_number != UINT32_MAX &&
                release->post_cont_t0_ns == UINT64_MAX;
        default:
            return false;
    }
}

static void set_uncertain_release(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *release,
    uint32_t error_number
)
{
    memset(release, 0, sizeof(*release));
    release->kind = MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN;
    release->error_number = error_number;
    release->post_cont_t0_ns = UINT64_MAX;
}

static uint32_t call_release(
    SupervisorExecution *execution,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation seed;
    uint32_t error_number;
    int callback_result;

    seed_release(&candidate);
    seed = candidate;
    callback_result =
        execution->ops.release_gate_and_sample_post_cont_t0(
            execution->context, execution->result.child_pid, &candidate
        );
    *out_protocol_failure = callback_result < 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        error_number = *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
        set_uncertain_release(&execution->result.release, error_number);
        execution->result.child_release_state_uncertain = 1U;
        return error_number;
    }
    if (!release_valid(&candidate)) {
        *out_protocol_failure = true;
        set_uncertain_release(
            &execution->result.release, (uint32_t)EPROTO
        );
        execution->result.child_release_state_uncertain = 1U;
        return (uint32_t)EPROTO;
    }
    execution->result.release = candidate;
    if (candidate.sigcont_dispatched == 1U) {
        execution->result.child_gate_released = 1U;
    }
    if (candidate.kind ==
        MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN) {
        execution->result.child_release_state_uncertain = 1U;
    }
    return 0U;
}

static void seed_wait(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *wait
)
{
    memset(wait, 0xff, sizeof(*wait));
    wait->child_exit_code = UINT16_MAX;
}

static bool wait_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *wait,
    bool timed
)
{
    if (!byte_flag_valid(wait->exit_code_valid) ||
        !bytes_are_zero(wait->reserved_tail, sizeof(wait->reserved_tail))) {
        return false;
    }
    switch (wait->kind) {
        case MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED:
            return wait->exit_code_valid == 1U &&
                wait->child_exit_code <= 255U && wait->child_signal == 0U;
        case MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED:
            return wait->exit_code_valid == 0U &&
                wait->child_exit_code == 0U && wait->child_signal > 0U &&
                wait->child_signal <=
                    MEISO_K230D_DRM_SUPERVISOR_MAX_SIGNAL_NUMBER;
        case MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE:
            return timed && wait->exit_code_valid == 0U &&
                wait->child_exit_code == 0U && wait->child_signal == 0U;
        case MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED:
        case MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED:
            return wait->exit_code_valid == 0U &&
                wait->child_exit_code == 0U && wait->child_signal == 0U;
        default:
            return false;
    }
}

static uint32_t call_timed_wait(
    SupervisorExecution *execution,
    uint64_t deadline_ns,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation seed;
    int callback_result;

    seed_wait(&candidate);
    seed = candidate;
    increment_counter(execution, &execution->result.timed_wait_call_count);
    callback_result = execution->ops.wait_owned_child_until(
        execution->context, execution->result.child_pid, deadline_ns,
        &candidate
    );
    *out_protocol_failure = callback_result < 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (!wait_valid(&candidate, true)) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    *out_wait = candidate;
    return 0U;
}

static uint32_t call_indefinite_wait(
    SupervisorExecution *execution,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait,
    bool *out_protocol_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation seed;
    int callback_result;

    seed_wait(&candidate);
    seed = candidate;
    increment_counter(execution, &execution->result.indefinite_wait_call_count);
    callback_result = execution->ops.wait_owned_child_to_reap(
        execution->context, execution->result.child_pid, &candidate
    );
    *out_protocol_failure = callback_result < 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (!wait_valid(&candidate, false)) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    *out_wait = candidate;
    return 0U;
}

static uint32_t call_clock(
    SupervisorExecution *execution,
    uint64_t *out_now_ns,
    bool *out_protocol_failure
)
{
    uint64_t candidate = UINT64_MAX;
    int callback_result = execution->ops.monotonic_now(
        execution->context, &candidate
    );

    *out_protocol_failure = callback_result < 0;
    if (callback_result != 0) {
        if (candidate != UINT64_MAX) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (candidate == UINT64_MAX ||
        candidate < execution->result.release.post_cont_t0_ns ||
        candidate < execution->previous_now_ns) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    execution->previous_now_ns = candidate;
    *out_now_ns = candidate;
    return 0U;
}

static void record_wait_error(
    SupervisorExecution *execution,
    uint32_t error_number,
    bool protocol_failure
)
{
    if (execution->result.first_wait_callback_error == 0U) {
        execution->result.first_wait_callback_error = error_number;
    }
    record_callback_failure(
        execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_WAIT,
        error_number, protocol_failure
    );
}

static void record_clock_error(
    SupervisorExecution *execution,
    uint32_t error_number,
    bool protocol_failure
)
{
    if (execution->result.first_clock_callback_error == 0U) {
        execution->result.first_clock_callback_error = error_number;
    }
    record_callback_failure(
        execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_CLOCK,
        error_number, protocol_failure
    );
}

static void accept_reap(
    SupervisorExecution *execution,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *wait
)
{
    execution->result.final_reap = *wait;
    execution->result.child_reaped = 1U;
}

static void observe_stop_request(SupervisorExecution *execution)
{
    execution->result.stop_requested = 1U;
    increment_counter(execution, &execution->result.stop_request_count);
    record_failure(
        execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_STOP_REQUESTED,
        (uint32_t)ECANCELED
    );
}

static TimedWaitResult wait_until_deadline(
    SupervisorExecution *execution,
    uint64_t deadline_ns,
    uint64_t *out_observed_now_ns
)
{
    for (;;) {
        MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait;
        bool protocol_failure = false;
        bool wait_was_deadline = false;
        uint32_t error_number = call_timed_wait(
            execution, deadline_ns, &wait, &protocol_failure
        );
        uint64_t now_ns = 0U;

        if (error_number == 0U) {
            if (wait.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED ||
                wait.kind ==
                    MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED) {
                accept_reap(execution, &wait);
                return TIMED_WAIT_REAPED;
            }
            if (wait.kind ==
                MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED) {
                increment_counter(
                    execution, &execution->result.interrupted_wait_count
                );
            } else if (wait.kind ==
                       MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED) {
                observe_stop_request(execution);
            } else {
                wait_was_deadline = true;
            }
        } else {
            record_wait_error(execution, error_number, protocol_failure);
        }

        protocol_failure = false;
        error_number = call_clock(
            execution, &now_ns, &protocol_failure
        );
        if (error_number != 0U) {
            record_clock_error(execution, error_number, protocol_failure);
            return TIMED_WAIT_FAIL_SAFE;
        }
        if (now_ns >= deadline_ns) {
            *out_observed_now_ns = now_ns;
            return TIMED_WAIT_DEADLINE;
        }
        if (wait_was_deadline) {
            record_wait_error(execution, (uint32_t)EPROTO, true);
        }
    }
}

static void call_signal_once(
    SupervisorExecution *execution,
    bool terminate
)
{
    uint32_t *counter = terminate ?
        &execution->result.terminate_call_count :
        &execution->result.kill_call_count;
    int callback_result;
    uint32_t error_number;

    increment_counter(execution, counter);
    callback_result = terminate ?
        execution->ops.terminate_owned_child_once(
            execution->context, execution->result.child_pid
        ) :
        execution->ops.kill_owned_child_once(
            execution->context, execution->result.child_pid
        );
    if (callback_result == 0) {
        return;
    }
    error_number = normalize_callback_error(callback_result);
    if (terminate) {
        execution->result.term_error = error_number;
        record_callback_failure(
            execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERM,
            error_number, callback_result < 0
        );
    } else {
        execution->result.kill_error = error_number;
        record_callback_failure(
            execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_KILL,
            error_number, callback_result < 0
        );
    }
}

static void attempt_quarantine_record(SupervisorExecution *execution)
{
    bool protocol_failure = false;
    uint32_t error_number;

    if (execution->result.quarantine_record_attempted != 0U) {
        return;
    }
    execution->result.quarantine_record_attempted = 1U;
    error_number = call_result_durable(
        execution, execution->ops.publish_quarantine_record,
        &execution->result.quarantine_record, &protocol_failure
    );
    if (error_number != 0U) {
        execution->result.quarantine_record_callback_error = error_number;
        record_callback_failure(
            execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_QUARANTINE_RECORD,
            error_number, protocol_failure
        );
    } else if (execution->result.quarantine_record.disposition !=
               MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED) {
        record_failure(
            execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_QUARANTINE_RECORD,
            execution->result.quarantine_record.error_number
        );
    }
}

static void indefinite_reap(SupervisorExecution *execution)
{
    while (execution->result.child_reaped == 0U) {
        MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait;
        bool protocol_failure = false;
        uint32_t error_number = call_indefinite_wait(
            execution, &wait, &protocol_failure
        );

        if (error_number != 0U) {
            record_wait_error(execution, error_number, protocol_failure);
            continue;
        }
        if (wait.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED ||
            wait.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED) {
            accept_reap(execution, &wait);
        } else if (wait.kind ==
                   MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED) {
            increment_counter(
                execution, &execution->result.interrupted_wait_count
            );
        } else {
            observe_stop_request(execution);
        }
    }
}

static void enter_ownership_fence(SupervisorExecution *execution)
{
    if (execution->result.kill_call_count == 0U) {
        call_signal_once(execution, false);
    }
    attempt_quarantine_record(execution);
    indefinite_reap(execution);
}

static bool checked_deadlines(SupervisorExecution *execution)
{
    uint64_t t0 = execution->result.release.post_cont_t0_ns;

    if (execution->config.term_after_ns > UINT64_MAX - t0 ||
        execution->config.kill_after_ns > UINT64_MAX - t0 ||
        execution->config.quarantine_after_ns > UINT64_MAX - t0) {
        record_failure(
            execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_CLOCK,
            (uint32_t)EOVERFLOW
        );
        return false;
    }
    execution->result.term_deadline_ns =
        t0 + execution->config.term_after_ns;
    execution->result.kill_deadline_ns =
        t0 + execution->config.kill_after_ns;
    execution->result.quarantine_deadline_ns =
        t0 + execution->config.quarantine_after_ns;
    execution->previous_now_ns = t0;
    return true;
}

static void supervise_released_child(SupervisorExecution *execution)
{
    TimedWaitResult wait_result;
    uint64_t observed_now_ns = 0U;

    wait_result = wait_until_deadline(
        execution, execution->result.term_deadline_ns, &observed_now_ns
    );
    if (wait_result == TIMED_WAIT_REAPED) {
        return;
    }
    if (wait_result == TIMED_WAIT_FAIL_SAFE) {
        enter_ownership_fence(execution);
        return;
    }
    execution->result.term_deadline_observed_now_ns = observed_now_ns;
    execution->result.term_deadline_confirmed = 1U;
    record_failure(
        execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_DEADLINE_EXPIRED,
        (uint32_t)ETIMEDOUT
    );
    call_signal_once(execution, true);

    wait_result = wait_until_deadline(
        execution, execution->result.kill_deadline_ns, &observed_now_ns
    );
    if (wait_result == TIMED_WAIT_REAPED) {
        return;
    }
    if (wait_result == TIMED_WAIT_FAIL_SAFE) {
        enter_ownership_fence(execution);
        return;
    }
    execution->result.kill_deadline_observed_now_ns = observed_now_ns;
    execution->result.kill_deadline_confirmed = 1U;
    call_signal_once(execution, false);

    wait_result = wait_until_deadline(
        execution, execution->result.quarantine_deadline_ns,
        &observed_now_ns
    );
    if (wait_result == TIMED_WAIT_REAPED) {
        return;
    }
    if (wait_result == TIMED_WAIT_FAIL_SAFE) {
        enter_ownership_fence(execution);
        return;
    }
    execution->result.quarantine_deadline_observed_now_ns = observed_now_ns;
    execution->result.quarantine_deadline_confirmed = 1U;
    attempt_quarantine_record(execution);
    indefinite_reap(execution);
}

static void seed_postflight(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *postflight
)
{
    memset(postflight, 0xff, sizeof(*postflight));
    postflight->stdout_bytes = UINT64_MAX;
    postflight->stderr_bytes = UINT64_MAX;
    postflight->primary_holder_count = UINT32_MAX;
}

static bool postflight_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *postflight
)
{
    const uint8_t flags[] = {
        postflight->boot_identity_matches,
        postflight->artifact_identity_matches,
        postflight->primary_node_identity_matches,
        postflight->holder_scan_complete,
        postflight->stdout_complete,
        postflight->stderr_complete,
        postflight->stdout_hash_valid,
        postflight->stderr_hash_valid,
        postflight->stdout_synced,
        postflight->stderr_synced,
        postflight->stdout_closed,
        postflight->stderr_closed
    };
    size_t index;

    for (index = 0U; index < sizeof(flags) / sizeof(flags[0]); ++index) {
        if (!byte_flag_valid(flags[index])) {
            return false;
        }
    }
    if (postflight->stdout_bytes == UINT64_MAX ||
        postflight->stderr_bytes == UINT64_MAX ||
        postflight->primary_holder_count == UINT32_MAX ||
        (postflight->holder_scan_complete == 0U &&
         postflight->primary_holder_count != 0U)) {
        return false;
    }
    if ((postflight->stdout_hash_valid != 0U &&
         postflight->stdout_complete == 0U) ||
        (postflight->stderr_hash_valid != 0U &&
         postflight->stderr_complete == 0U) ||
        (postflight->stdout_synced != 0U &&
         (postflight->stdout_complete == 0U ||
          postflight->stdout_hash_valid == 0U)) ||
        (postflight->stderr_synced != 0U &&
         (postflight->stderr_complete == 0U ||
          postflight->stderr_hash_valid == 0U))) {
        return false;
    }
    return true;
}

static bool postflight_succeeds(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *postflight
)
{
    return postflight->boot_identity_matches == 1U &&
        postflight->artifact_identity_matches == 1U &&
        postflight->primary_node_identity_matches == 1U &&
        postflight->holder_scan_complete == 1U &&
        postflight->primary_holder_count == 0U &&
        postflight->stdout_complete == 1U &&
        postflight->stderr_complete == 1U &&
        postflight->stdout_hash_valid == 1U &&
        postflight->stderr_hash_valid == 1U &&
        postflight->stdout_synced == 1U &&
        postflight->stderr_synced == 1U &&
        postflight->stdout_closed == 1U &&
        postflight->stderr_closed == 1U;
}

static uint32_t call_postflight(
    SupervisorExecution *execution,
    bool *out_protocol_failure,
    bool *out_callback_failure
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding frozen_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight candidate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight seed;
    int callback_result;

    binding = execution->config.binding;
    frozen_binding = binding;
    seed_postflight(&candidate);
    seed = candidate;
    execution->result.postflight_attempted = 1U;
    callback_result = execution->ops.inspect_postflight(
        execution->context, &binding, execution->result.child_pid,
        &candidate
    );
    *out_protocol_failure = callback_result < 0 ||
        memcmp(&binding, &frozen_binding, sizeof(binding)) != 0;
    *out_callback_failure = callback_result != 0;
    if (callback_result != 0) {
        if (memcmp(&candidate, &seed, sizeof(candidate)) != 0) {
            *out_protocol_failure = true;
        }
        return *out_protocol_failure ? (uint32_t)EPROTO :
            normalize_callback_error(callback_result);
    }
    if (*out_protocol_failure || !postflight_valid(&candidate)) {
        *out_protocol_failure = true;
        return (uint32_t)EPROTO;
    }
    execution->result.postflight = candidate;
    if (!postflight_succeeds(&candidate)) {
        return candidate.holder_scan_complete != 0U &&
                candidate.primary_holder_count != 0U ?
            (uint32_t)EBUSY : (uint32_t)EIO;
    }
    execution->result.postflight_succeeded = 1U;
    return 0U;
}

static void finish_spawned_run(SupervisorExecution *execution)
{
    bool protocol_failure = false;
    bool callback_failure = false;
    uint32_t error_number;

    update_outcome(execution);
    error_number = call_postflight(
        execution, &protocol_failure, &callback_failure
    );
    if (error_number != 0U) {
        execution->result.postflight_callback_error =
            callback_failure || protocol_failure ? error_number : 0U;
        record_callback_failure(
            execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_POSTFLIGHT,
            error_number, protocol_failure
        );
        return;
    }

    execution->result.terminal_record_attempted = 1U;
    update_outcome(execution);
    protocol_failure = false;
    error_number = call_result_durable(
        execution, execution->ops.publish_terminal_result,
        &execution->result.terminal_record, &protocol_failure
    );
    if (error_number != 0U) {
        execution->result.terminal_record_callback_error = error_number;
        record_callback_failure(
            execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERMINAL_RECORD,
            error_number, protocol_failure
        );
    } else if (execution->result.terminal_record.disposition !=
               MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED) {
        record_failure(
            execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERMINAL_RECORD,
            execution->result.terminal_record.error_number
        );
    }
}

MeisoK230dDrmSupervisorInvokeStatus
meiso_k230d_drm_private_supervise_single_plane_test_only_child(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig *config,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorOps *ops,
    void *context,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *out_result
)
{
    SupervisorExecution execution;
    MeisoK230dDrmSupervisorInvokeStatus status =
        validate_parent_invocation(config, ops, context, out_result);
    bool protocol_failure;
    uint32_t error_number;

    if (status != MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE) {
        return status;
    }
    memset(&execution, 0, sizeof(execution));
    execution.config = *config;
    execution.ops = *ops;
    execution.context = context;
    execution.result.binding = execution.config.binding;

    protocol_failure = false;
    error_number = call_preflight(
        &execution, 0U, &execution.result.preflight[0],
        &protocol_failure
    );
    if (error_number != 0U) {
        execution.result.initial_preflight_callback_error = error_number;
        record_callback_failure(
            &execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_INITIAL_PREFLIGHT,
            error_number, protocol_failure
        );
        goto no_child_result;
    }
    if (!preflight_admits(&execution.result.preflight[0])) {
        record_failure(
            &execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_INITIAL_PREFLIGHT,
            preflight_denial_error(&execution.result.preflight[0])
        );
        goto no_child_result;
    }

    execution.result.global_claim_attempted = 1U;
    protocol_failure = false;
    error_number = call_binding_durable(
        &execution, execution.ops.claim_global_launch_once,
        &execution.result.global_claim, &protocol_failure
    );
    if (error_number != 0U) {
        execution.result.global_claim_callback_error = error_number;
        record_callback_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_GLOBAL_CLAIM,
            error_number, protocol_failure
        );
        goto no_child_result;
    }
    if (execution.result.global_claim.disposition !=
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED) {
        record_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_GLOBAL_CLAIM,
            execution.result.global_claim.error_number
        );
        goto no_child_result;
    }

    execution.result.evidence_claim_attempted = 1U;
    protocol_failure = false;
    error_number = call_binding_durable(
        &execution, execution.ops.claim_evidence_launch_once,
        &execution.result.evidence_claim, &protocol_failure
    );
    if (error_number != 0U) {
        execution.result.evidence_claim_callback_error = error_number;
        record_callback_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_EVIDENCE_CLAIM,
            error_number, protocol_failure
        );
        goto no_child_result;
    }
    if (execution.result.evidence_claim.disposition !=
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED) {
        record_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_EVIDENCE_CLAIM,
            execution.result.evidence_claim.error_number
        );
        goto no_child_result;
    }

    {
        MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding =
            execution.config.binding;
        MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding frozen_binding =
            binding;
        uint32_t child_pid = 0U;
        int callback_result;

        execution.result.spawn_attempted = 1U;
        callback_result = execution.ops.spawn_one_child(
            execution.context, &binding, &child_pid
        );
        protocol_failure = callback_result < 0 ||
            memcmp(&binding, &frozen_binding, sizeof(binding)) != 0;
        if (callback_result != 0) {
            if (child_pid != 0U) {
                protocol_failure = true;
            }
            error_number = protocol_failure ? (uint32_t)EPROTO :
                normalize_callback_error(callback_result);
            execution.result.spawn_callback_error = error_number;
            record_callback_failure(
                &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_SPAWN,
                error_number, protocol_failure
            );
            goto no_child_result;
        }
        if (child_pid == 0U || child_pid > (uint32_t)INT32_MAX) {
            execution.result.spawn_callback_error = (uint32_t)EPROTO;
            record_callback_failure(
                &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_SPAWN,
                (uint32_t)EPROTO, true
            );
            goto no_child_result;
        }
        execution.result.child_spawned = 1U;
        execution.result.child_pid = child_pid;
        if (protocol_failure) {
            execution.result.spawn_callback_error = (uint32_t)EPROTO;
            record_callback_failure(
                &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_SPAWN,
                (uint32_t)EPROTO, true
            );
            enter_ownership_fence(&execution);
            goto spawned_reaped;
        }
    }

    for (;;) {
        MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;

        protocol_failure = false;
        error_number = call_gate(&execution, &gate, &protocol_failure);
        if (error_number != 0U) {
            execution.result.gate_callback_error = error_number;
            record_callback_failure(
                &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_CHILD_GATE,
                error_number, protocol_failure
            );
            enter_ownership_fence(&execution);
            goto spawned_reaped;
        }
        execution.result.gate = gate;
        if (gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_INTERRUPTED) {
            increment_counter(
                &execution, &execution.result.interrupted_wait_count
            );
            continue;
        }
        if (gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_STOP_REQUESTED) {
            observe_stop_request(&execution);
            enter_ownership_fence(&execution);
            goto spawned_reaped;
        }
        if (gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED ||
            gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_SIGNALED) {
            record_failure(
                &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_CHILD_GATE,
                (uint32_t)ECHILD
            );
            gate_to_final_reap(&execution, &gate);
            goto spawned_reaped;
        }
        execution.result.child_ready_and_stopped = 1U;
        break;
    }

    protocol_failure = false;
    error_number = call_preflight(
        &execution, 1U, &execution.result.preflight[1],
        &protocol_failure
    );
    if (error_number != 0U) {
        execution.result.pre_release_preflight_callback_error = error_number;
        record_callback_failure(
            &execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_PRE_RELEASE_PREFLIGHT,
            error_number, protocol_failure
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }
    if (!preflight_admits(&execution.result.preflight[1])) {
        record_failure(
            &execution,
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_PRE_RELEASE_PREFLIGHT,
            preflight_denial_error(&execution.result.preflight[1])
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }

    execution.result.ready_record_attempted = 1U;
    protocol_failure = false;
    error_number = call_result_durable(
        &execution, execution.ops.publish_launch_ready_record,
        &execution.result.ready_record, &protocol_failure
    );
    if (error_number != 0U) {
        execution.result.ready_record_callback_error = error_number;
        record_callback_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_READY_RECORD,
            error_number, protocol_failure
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }
    if (execution.result.ready_record.disposition !=
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED) {
        record_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_READY_RECORD,
            execution.result.ready_record.error_number
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }

    execution.result.release_attempted = 1U;
    protocol_failure = false;
    error_number = call_release(&execution, &protocol_failure);
    if (error_number != 0U) {
        execution.result.release_callback_error = error_number;
        record_callback_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE,
            error_number, protocol_failure
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }
    if (execution.result.release.kind !=
        MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0) {
        record_failure(
            &execution, MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE,
            execution.result.release.error_number
        );
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }
    if (!checked_deadlines(&execution)) {
        enter_ownership_fence(&execution);
        goto spawned_reaped;
    }
    supervise_released_child(&execution);

spawned_reaped:
    finish_spawned_run(&execution);
    update_outcome(&execution);
    memcpy(out_result, &execution.result, sizeof(*out_result));
    return MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE;

no_child_result:
    update_outcome(&execution);
    memcpy(out_result, &execution.result, sizeof(*out_result));
    return MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE;
}

static bool child_ops_valid(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *ops
)
{
    return ops->read_parent_id != NULL &&
        ops->arm_parent_death_kill != NULL &&
        ops->isolate_fixed_child_file_descriptors != NULL &&
        ops->reset_and_unblock_child_signals_with_sigpipe_ignored != NULL &&
        ops->notify_ready_and_close_channel != NULL &&
        ops->stop_self_until_continued != NULL &&
        ops->run_derived_v2 != NULL;
}

static bool child_request_valid(
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request
)
{
    return !bytes_are_zero(
            request->expected_topology_sha256,
            sizeof(request->expected_topology_sha256)
        ) && request->connector_id != 0U && request->crtc_id != 0U;
}

static MeisoK230dDrmSupervisorChildStatus child_simple_call(
    int (*callback)(void *),
    void *context
)
{
    int callback_result = callback(context);

    if (callback_result < 0) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE;
    }
    if (callback_result > 0) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE;
    }
    return MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED;
}

static MeisoK230dDrmSupervisorChildStatus child_read_parent(
    int (*callback)(void *, uint32_t *),
    void *context,
    uint32_t expected_parent_id
)
{
    uint32_t parent_id = 0U;
    int callback_result = callback(context, &parent_id);

    if (callback_result != 0) {
        if (callback_result < 0 || parent_id != 0U) {
            return MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE;
        }
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE;
    }
    if (parent_id == 0U || parent_id > (uint32_t)INT32_MAX) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE;
    }
    if (parent_id != expected_parent_id) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE;
    }
    return MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED;
}

MeisoK230dDrmSupervisorChildStatus
meiso_k230d_drm_private_run_single_plane_test_only_child(
    uint32_t expected_parent_id,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *ops,
    void *context,
    uint8_t *out_exit_code
)
{
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest frozen_request;
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest callback_request;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps frozen_ops;
    MeisoK230dDrmSupervisorChildStatus status;
    uint8_t derived_exit = 0xffU;
    int callback_result;

    if (request == NULL || ops == NULL || out_exit_code == NULL ||
        expected_parent_id == 0U ||
        expected_parent_id > (uint32_t)INT32_MAX) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_ARGUMENT;
    }
    if (ranges_overlap(request, sizeof(*request), ops, sizeof(*ops)) ||
        ranges_overlap(request, sizeof(*request), out_exit_code,
                       sizeof(*out_exit_code)) ||
        ranges_overlap(ops, sizeof(*ops), out_exit_code,
                       sizeof(*out_exit_code)) ||
        !child_request_valid(request)) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_ARGUMENT;
    }
    if (context == NULL || !child_ops_valid(ops)) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_CONFIG;
    }
    frozen_request = *request;
    frozen_ops = *ops;

    status = child_read_parent(
        frozen_ops.read_parent_id, context, expected_parent_id
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_simple_call(frozen_ops.arm_parent_death_kill, context);
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_read_parent(
        frozen_ops.read_parent_id, context, expected_parent_id
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_simple_call(
        frozen_ops.isolate_fixed_child_file_descriptors, context
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_simple_call(
        frozen_ops.reset_and_unblock_child_signals_with_sigpipe_ignored,
        context
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_simple_call(
        frozen_ops.notify_ready_and_close_channel, context
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }
    status = child_simple_call(
        frozen_ops.stop_self_until_continued, context
    );
    if (status != MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED) {
        return status;
    }

    callback_request = frozen_request;
    callback_result = frozen_ops.run_derived_v2(
        context, &callback_request, &derived_exit
    );
    if (callback_result != 0 ||
        memcmp(&callback_request, &frozen_request,
               sizeof(callback_request)) != 0 ||
        derived_exit > 1U) {
        return MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE;
    }
    *out_exit_code = derived_exit;
    return MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED;
}
