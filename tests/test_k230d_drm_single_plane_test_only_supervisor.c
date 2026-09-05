#include "meiso_drm_single_plane_test_only_supervisor_private.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf( \
                stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition \
            ); \
            return false; \
        } \
    } while (0)

enum {
    SCRIPT_CAPACITY = 24,
    EVENT_CAPACITY = 128
};

typedef uint8_t TestEvent;
enum {
    EVENT_PREFLIGHT_0 = 1,
    EVENT_GLOBAL_CLAIM = 2,
    EVENT_EVIDENCE_CLAIM = 3,
    EVENT_SPAWN = 4,
    EVENT_GATE = 5,
    EVENT_PREFLIGHT_1 = 6,
    EVENT_READY_RECORD = 7,
    EVENT_RELEASE = 8,
    EVENT_TIMED_WAIT = 9,
    EVENT_CLOCK = 10,
    EVENT_TERM = 11,
    EVENT_KILL = 12,
    EVENT_QUARANTINE_RECORD = 13,
    EVENT_INDEFINITE_WAIT = 14,
    EVENT_POSTFLIGHT = 15,
    EVENT_TERMINAL_RECORD = 16
};

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding expected_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight preflight[2];
    int preflight_return[2];
    bool preflight_mutate_output[2];
    bool preflight_mutate_binding[2];
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate global_claim;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate evidence_claim;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate ready_record;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate quarantine_record;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate terminal_record;
    int global_return;
    int evidence_return;
    int ready_return;
    int quarantine_return;
    int terminal_return;
    bool global_mutate_output;
    bool evidence_mutate_output;
    bool ready_mutate_output;
    bool quarantine_mutate_output;
    bool terminal_mutate_output;
    bool global_mutate_binding;
    bool evidence_mutate_binding;
    bool ready_mutate_result;
    bool quarantine_mutate_result;
    bool terminal_mutate_result;
    uint32_t spawn_pid;
    int spawn_return;
    bool spawn_mutate_output;
    bool spawn_mutate_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation
        gate_script[SCRIPT_CAPACITY];
    int gate_return[SCRIPT_CAPACITY];
    bool gate_mutate_output[SCRIPT_CAPACITY];
    size_t gate_size;
    size_t gate_index;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation release;
    int release_return;
    bool release_mutate_output;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation
        timed_script[SCRIPT_CAPACITY];
    int timed_return[SCRIPT_CAPACITY];
    bool timed_mutate_output[SCRIPT_CAPACITY];
    size_t timed_size;
    size_t timed_index;
    uint64_t timed_deadline_seen[SCRIPT_CAPACITY];
    uint64_t clock_value[SCRIPT_CAPACITY];
    int clock_return[SCRIPT_CAPACITY];
    bool clock_mutate_output[SCRIPT_CAPACITY];
    size_t clock_size;
    size_t clock_index;
    int term_return;
    int kill_return;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation
        indefinite_script[SCRIPT_CAPACITY];
    int indefinite_return[SCRIPT_CAPACITY];
    bool indefinite_mutate_output[SCRIPT_CAPACITY];
    size_t indefinite_size;
    size_t indefinite_index;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight postflight;
    int postflight_return;
    bool postflight_mutate_output;
    bool postflight_mutate_binding;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig *config_to_mutate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorOps *ops_to_mutate;
    bool mutate_originals_on_first_preflight;
    bool input_contract_ok;
    bool canary_contract_ok;
    TestEvent events[EVENT_CAPACITY];
    size_t event_count;
    uint32_t preflight_calls[2];
    uint32_t global_calls;
    uint32_t evidence_calls;
    uint32_t spawn_calls;
    uint32_t gate_calls;
    uint32_t ready_calls;
    uint32_t release_calls;
    uint32_t timed_calls;
    uint32_t clock_calls;
    uint32_t term_calls;
    uint32_t kill_calls;
    uint32_t quarantine_calls;
    uint32_t indefinite_calls;
    uint32_t postflight_calls;
    uint32_t terminal_calls;
} SupervisorFixture;

static void record_event(SupervisorFixture *fixture, TestEvent event)
{
    if (fixture->event_count < EVENT_CAPACITY) {
        fixture->events[fixture->event_count++] = event;
    }
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate
created_durable(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate durable;

    memset(&durable, 0, sizeof(durable));
    durable.disposition = MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED;
    durable.progress =
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PARENT_DIRECTORY_SYNCED;
    return durable;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate
already_exists_durable(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate durable;

    memset(&durable, 0, sizeof(durable));
    durable.disposition =
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_ALREADY_EXISTS;
    durable.progress = MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE;
    durable.error_number = (uint32_t)EEXIST;
    return durable;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate
uncertain_durable(
    MeisoK230dDrmSupervisorDurableProgress progress,
    uint32_t error_number
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate durable;

    memset(&durable, 0, sizeof(durable));
    durable.disposition =
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN;
    durable.progress = progress;
    durable.error_number = error_number;
    return durable;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight good_preflight(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight preflight;

    memset(&preflight, 0, sizeof(preflight));
    preflight.boot_identity_matches = 1U;
    preflight.artifact_identity_matches = 1U;
    preflight.primary_node_identity_matches = 1U;
    preflight.holder_scan_complete = 1U;
    preflight.single_thread_verified = 1U;
    return preflight;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation ready_gate(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;

    memset(&gate, 0, sizeof(gate));
    gate.kind = MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED;
    gate.ready_byte_observed = 1U;
    gate.exact_wait_child_matched = 1U;
    gate.child_start_ticks_valid = 1U;
    gate.child_start_ticks = 77U;
    return gate;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation exited_gate(
    uint16_t exit_code
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;

    memset(&gate, 0, sizeof(gate));
    gate.kind = MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED;
    gate.exact_wait_child_matched = 1U;
    gate.exit_code_valid = 1U;
    gate.child_exit_code = exit_code;
    return gate;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation
released_at(uint64_t t0_ns)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation release;

    memset(&release, 0, sizeof(release));
    release.kind =
        MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0;
    release.sigcont_dispatch_known = 1U;
    release.sigcont_dispatched = 1U;
    release.t0_valid = 1U;
    release.post_cont_t0_ns = t0_ns;
    return release;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation
released_without_t0(uint32_t error_number)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation release;

    memset(&release, 0, sizeof(release));
    release.kind = MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITHOUT_T0;
    release.sigcont_dispatch_known = 1U;
    release.sigcont_dispatched = 1U;
    release.error_number = error_number;
    release.post_cont_t0_ns = UINT64_MAX;
    return release;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait_exit(
    uint16_t exit_code
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait;

    memset(&wait, 0, sizeof(wait));
    wait.kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED;
    wait.exit_code_valid = 1U;
    wait.child_exit_code = exit_code;
    return wait;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait_simple(
    MeisoK230dDrmSupervisorWaitKind kind
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait;

    memset(&wait, 0, sizeof(wait));
    wait.kind = kind;
    return wait;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight good_postflight(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight postflight;
    size_t index;

    memset(&postflight, 0, sizeof(postflight));
    for (index = 0U; index < sizeof(postflight.stdout_sha256); ++index) {
        postflight.stdout_sha256[index] = (uint8_t)(index + 1U);
        postflight.stderr_sha256[index] = (uint8_t)(index + 33U);
    }
    postflight.stdout_bytes = 100U;
    postflight.stderr_bytes = 20U;
    postflight.boot_identity_matches = 1U;
    postflight.artifact_identity_matches = 1U;
    postflight.primary_node_identity_matches = 1U;
    postflight.holder_scan_complete = 1U;
    postflight.stdout_complete = 1U;
    postflight.stderr_complete = 1U;
    postflight.stdout_hash_valid = 1U;
    postflight.stderr_hash_valid = 1U;
    postflight.stdout_synced = 1U;
    postflight.stderr_synced = 1U;
    postflight.stdout_closed = 1U;
    postflight.stderr_closed = 1U;
    return postflight;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding fixture_binding(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    size_t index;

    memset(&binding, 0, sizeof(binding));
    for (index = 0U; index < sizeof(binding.run_nonce); ++index) {
        binding.run_nonce[index] = (uint8_t)(index + 1U);
        binding.expected_boot_id_sha256[index] = (uint8_t)(index + 2U);
        binding.expected_artifact_sha256[index] = (uint8_t)(index + 3U);
        binding.derive_request.expected_topology_sha256[index] =
            (uint8_t)(index + 4U);
    }
    binding.derive_request.connector_id = 10U;
    binding.derive_request.crtc_id = 30U;
    return binding;
}

static void seed_preflight_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *preflight
)
{
    memset(preflight, 0xff, sizeof(*preflight));
    preflight->primary_holder_count = UINT32_MAX;
}

static void seed_durable_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *durable
)
{
    memset(durable, 0xff, sizeof(*durable));
    durable->error_number = UINT32_MAX;
}

static void seed_gate_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *gate
)
{
    memset(gate, 0xff, sizeof(*gate));
    gate->child_exit_code = UINT16_MAX;
    gate->child_start_ticks = UINT64_MAX;
}

static void seed_release_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *release
)
{
    memset(release, 0xff, sizeof(*release));
    release->error_number = UINT32_MAX;
    release->post_cont_t0_ns = UINT64_MAX;
}

static void seed_wait_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *wait
)
{
    memset(wait, 0xff, sizeof(*wait));
    wait->child_exit_code = UINT16_MAX;
}

static void seed_postflight_canary(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *postflight
)
{
    memset(postflight, 0xff, sizeof(*postflight));
    postflight->stdout_bytes = UINT64_MAX;
    postflight->stderr_bytes = UINT64_MAX;
    postflight->primary_holder_count = UINT32_MAX;
}

static void check_binding_input(
    SupervisorFixture *fixture,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding
)
{
    if (memcmp(binding, &fixture->expected_binding, sizeof(*binding)) != 0) {
        fixture->input_contract_ok = false;
    }
}

static int fake_inspect_preflight(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint8_t preflight_index,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *out_preflight
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight canary;
    uint8_t index = preflight_index > 1U ? 1U : preflight_index;

    seed_preflight_canary(&canary);
    if (memcmp(out_preflight, &canary, sizeof(canary)) != 0 ||
        preflight_index > 1U) {
        fixture->canary_contract_ok = false;
    }
    check_binding_input(fixture, binding);
    record_event(
        fixture, preflight_index == 0U ?
            EVENT_PREFLIGHT_0 : EVENT_PREFLIGHT_1
    );
    ++fixture->preflight_calls[index];
    if (fixture->mutate_originals_on_first_preflight &&
        preflight_index == 0U) {
        memset(fixture->config_to_mutate, 0,
               sizeof(*fixture->config_to_mutate));
        memset(fixture->ops_to_mutate, 0, sizeof(*fixture->ops_to_mutate));
    }
    if (fixture->preflight_mutate_binding[index]) {
        ((MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *)binding)
            ->derive_request.connector_id ^= 1U;
    }
    if (fixture->preflight_return[index] == 0) {
        *out_preflight = fixture->preflight[index];
    } else if (fixture->preflight_mutate_output[index]) {
        memset(out_preflight, 0, sizeof(*out_preflight));
    }
    return fixture->preflight_return[index];
}

static int fake_binding_durable(
    SupervisorFixture *fixture,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_claim,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *value,
    int callback_return,
    bool mutate_output,
    bool mutate_binding,
    TestEvent event,
    uint32_t *calls
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate canary;

    seed_durable_canary(&canary);
    if (memcmp(out_claim, &canary, sizeof(canary)) != 0) {
        fixture->canary_contract_ok = false;
    }
    check_binding_input(fixture, binding);
    record_event(fixture, event);
    ++*calls;
    if (mutate_binding) {
        ((MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *)binding)
            ->derive_request.crtc_id ^= 1U;
    }
    if (callback_return == 0) {
        *out_claim = *value;
    } else if (mutate_output) {
        memset(out_claim, 0, sizeof(*out_claim));
    }
    return callback_return;
}

static int fake_global_claim(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_claim
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    return fake_binding_durable(
        fixture, binding, out_claim, &fixture->global_claim,
        fixture->global_return, fixture->global_mutate_output,
        fixture->global_mutate_binding, EVENT_GLOBAL_CLAIM,
        &fixture->global_calls
    );
}

static int fake_evidence_claim(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_claim
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    return fake_binding_durable(
        fixture, binding, out_claim, &fixture->evidence_claim,
        fixture->evidence_return, fixture->evidence_mutate_output,
        fixture->evidence_mutate_binding, EVENT_EVIDENCE_CLAIM,
        &fixture->evidence_calls
    );
}

static int fake_spawn(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint32_t *out_child_pid
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    if (*out_child_pid != 0U) {
        fixture->canary_contract_ok = false;
    }
    check_binding_input(fixture, binding);
    record_event(fixture, EVENT_SPAWN);
    ++fixture->spawn_calls;
    if (fixture->spawn_mutate_binding) {
        ((MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *)binding)
            ->derive_request.connector_id ^= 1U;
    }
    if (fixture->spawn_return == 0 || fixture->spawn_mutate_output) {
        *out_child_pid = fixture->spawn_pid;
    }
    return fixture->spawn_return;
}

static int fake_gate(
    void *context,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *out_gate
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation canary;
    size_t index = fixture->gate_index++;

    seed_gate_canary(&canary);
    if (memcmp(out_gate, &canary, sizeof(canary)) != 0 ||
        child_pid != fixture->spawn_pid || index >= fixture->gate_size) {
        fixture->canary_contract_ok = false;
    }
    record_event(fixture, EVENT_GATE);
    ++fixture->gate_calls;
    if (index >= fixture->gate_size) {
        return EDOM;
    }
    if (fixture->gate_return[index] == 0) {
        *out_gate = fixture->gate_script[index];
    } else if (fixture->gate_mutate_output[index]) {
        memset(out_gate, 0, sizeof(*out_gate));
    }
    return fixture->gate_return[index];
}

static int fake_result_durable(
    SupervisorFixture *fixture,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *value,
    int callback_return,
    bool mutate_output,
    bool mutate_result,
    TestEvent event,
    uint32_t *calls
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate canary;

    seed_durable_canary(&canary);
    if (memcmp(out_record, &canary, sizeof(canary)) != 0 ||
        memcmp(&result->binding, &fixture->expected_binding,
               sizeof(result->binding)) != 0 ||
        result->child_pid != fixture->spawn_pid) {
        fixture->canary_contract_ok = false;
    }
    if ((event == EVENT_READY_RECORD &&
         (result->ready_record_attempted != 1U ||
          result->child_ready_and_stopped != 1U ||
          result->release_attempted != 0U ||
          result->outcome != MEISO_K230D_DRM_SUPERVISOR_OUTCOME_UNSET)) ||
        (event == EVENT_QUARANTINE_RECORD &&
         (result->quarantine_record_attempted != 1U ||
          result->child_reaped != 0U ||
          result->outcome != MEISO_K230D_DRM_SUPERVISOR_OUTCOME_UNSET)) ||
        (event == EVENT_TERMINAL_RECORD &&
         (result->terminal_record_attempted != 1U ||
          result->child_reaped != 1U ||
          result->postflight_succeeded != 1U ||
          result->outcome == MEISO_K230D_DRM_SUPERVISOR_OUTCOME_UNSET))) {
        fixture->input_contract_ok = false;
    }
    record_event(fixture, event);
    ++*calls;
    if (mutate_result) {
        ((MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *)result)
            ->child_pid ^= 1U;
    }
    if (callback_return == 0) {
        *out_record = *value;
    } else if (mutate_output) {
        memset(out_record, 0, sizeof(*out_record));
    }
    return callback_return;
}

static int fake_ready_record(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    return fake_result_durable(
        fixture, result, out_record, &fixture->ready_record,
        fixture->ready_return, fixture->ready_mutate_output,
        fixture->ready_mutate_result, EVENT_READY_RECORD,
        &fixture->ready_calls
    );
}

static int fake_release(
    void *context,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *out_release
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation canary;

    seed_release_canary(&canary);
    if (memcmp(out_release, &canary, sizeof(canary)) != 0 ||
        child_pid != fixture->spawn_pid) {
        fixture->canary_contract_ok = false;
    }
    record_event(fixture, EVENT_RELEASE);
    ++fixture->release_calls;
    if (fixture->release_return == 0) {
        *out_release = fixture->release;
    } else if (fixture->release_mutate_output) {
        memset(out_release, 0, sizeof(*out_release));
    }
    return fixture->release_return;
}

static int fake_timed_wait(
    void *context,
    uint32_t child_pid,
    uint64_t absolute_deadline_ns,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation canary;
    size_t index = fixture->timed_index++;

    seed_wait_canary(&canary);
    if (memcmp(out_wait, &canary, sizeof(canary)) != 0 ||
        child_pid != fixture->spawn_pid || index >= fixture->timed_size) {
        fixture->canary_contract_ok = false;
    }
    record_event(fixture, EVENT_TIMED_WAIT);
    ++fixture->timed_calls;
    if (index >= SCRIPT_CAPACITY || index >= fixture->timed_size) {
        return EDOM;
    }
    fixture->timed_deadline_seen[index] = absolute_deadline_ns;
    if (fixture->timed_return[index] == 0) {
        *out_wait = fixture->timed_script[index];
    } else if (fixture->timed_mutate_output[index]) {
        memset(out_wait, 0, sizeof(*out_wait));
    }
    return fixture->timed_return[index];
}

static int fake_clock(void *context, uint64_t *out_now_ns)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    size_t index = fixture->clock_index++;

    if (*out_now_ns != UINT64_MAX || index >= fixture->clock_size) {
        fixture->canary_contract_ok = false;
    }
    record_event(fixture, EVENT_CLOCK);
    ++fixture->clock_calls;
    if (index >= fixture->clock_size) {
        return EDOM;
    }
    if (fixture->clock_return[index] == 0 ||
        fixture->clock_mutate_output[index]) {
        *out_now_ns = fixture->clock_value[index];
    }
    return fixture->clock_return[index];
}

static int fake_term(void *context, uint32_t child_pid)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    if (child_pid != fixture->spawn_pid) {
        fixture->input_contract_ok = false;
    }
    record_event(fixture, EVENT_TERM);
    ++fixture->term_calls;
    return fixture->term_return;
}

static int fake_kill(void *context, uint32_t child_pid)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    if (child_pid != fixture->spawn_pid) {
        fixture->input_contract_ok = false;
    }
    record_event(fixture, EVENT_KILL);
    ++fixture->kill_calls;
    return fixture->kill_return;
}

static int fake_quarantine_record(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    return fake_result_durable(
        fixture, result, out_record, &fixture->quarantine_record,
        fixture->quarantine_return, fixture->quarantine_mutate_output,
        fixture->quarantine_mutate_result, EVENT_QUARANTINE_RECORD,
        &fixture->quarantine_calls
    );
}

static int fake_indefinite_wait(
    void *context,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *out_wait
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation canary;
    size_t index = fixture->indefinite_index++;

    seed_wait_canary(&canary);
    if (memcmp(out_wait, &canary, sizeof(canary)) != 0 ||
        child_pid != fixture->spawn_pid ||
        index >= fixture->indefinite_size) {
        fixture->canary_contract_ok = false;
    }
    record_event(fixture, EVENT_INDEFINITE_WAIT);
    ++fixture->indefinite_calls;
    if (index >= fixture->indefinite_size) {
        return EDOM;
    }
    if (fixture->indefinite_return[index] == 0) {
        *out_wait = fixture->indefinite_script[index];
    } else if (fixture->indefinite_mutate_output[index]) {
        memset(out_wait, 0, sizeof(*out_wait));
    }
    return fixture->indefinite_return[index];
}

static int fake_postflight(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *out_postflight
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight canary;

    seed_postflight_canary(&canary);
    if (memcmp(out_postflight, &canary, sizeof(canary)) != 0 ||
        child_pid != fixture->spawn_pid) {
        fixture->canary_contract_ok = false;
    }
    check_binding_input(fixture, binding);
    record_event(fixture, EVENT_POSTFLIGHT);
    ++fixture->postflight_calls;
    if (fixture->postflight_mutate_binding) {
        ((MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *)binding)
            ->derive_request.crtc_id ^= 1U;
    }
    if (fixture->postflight_return == 0) {
        *out_postflight = fixture->postflight;
    } else if (fixture->postflight_mutate_output) {
        memset(out_postflight, 0, sizeof(*out_postflight));
    }
    return fixture->postflight_return;
}

static int fake_terminal_record(
    void *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *out_record
)
{
    SupervisorFixture *fixture = (SupervisorFixture *)context;

    return fake_result_durable(
        fixture, result, out_record, &fixture->terminal_record,
        fixture->terminal_return, fixture->terminal_mutate_output,
        fixture->terminal_mutate_result, EVENT_TERMINAL_RECORD,
        &fixture->terminal_calls
    );
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorOps supervisor_ops(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorOps ops;

    memset(&ops, 0, sizeof(ops));
    ops.inspect_preflight = fake_inspect_preflight;
    ops.claim_global_launch_once = fake_global_claim;
    ops.claim_evidence_launch_once = fake_evidence_claim;
    ops.spawn_one_child = fake_spawn;
    ops.await_child_ready_and_sigstop = fake_gate;
    ops.publish_launch_ready_record = fake_ready_record;
    ops.release_gate_and_sample_post_cont_t0 = fake_release;
    ops.wait_owned_child_until = fake_timed_wait;
    ops.monotonic_now = fake_clock;
    ops.terminate_owned_child_once = fake_term;
    ops.kill_owned_child_once = fake_kill;
    ops.publish_quarantine_record = fake_quarantine_record;
    ops.wait_owned_child_to_reap = fake_indefinite_wait;
    ops.inspect_postflight = fake_postflight;
    ops.publish_terminal_result = fake_terminal_record;
    return ops;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig supervisor_config(
    const SupervisorFixture *fixture
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig config;

    memset(&config, 0, sizeof(config));
    config.binding = fixture->expected_binding;
    config.term_after_ns = 15U;
    config.kill_after_ns = 17U;
    config.quarantine_after_ns = 20U;
    return config;
}

static void init_supervisor_fixture(SupervisorFixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->expected_binding = fixture_binding();
    fixture->preflight[0] = good_preflight();
    fixture->preflight[1] = good_preflight();
    fixture->global_claim = created_durable();
    fixture->evidence_claim = created_durable();
    fixture->ready_record = created_durable();
    fixture->quarantine_record = created_durable();
    fixture->terminal_record = created_durable();
    fixture->spawn_pid = 123U;
    fixture->gate_script[0] = ready_gate();
    fixture->gate_size = 1U;
    fixture->release = released_at(100U);
    fixture->timed_script[0] = wait_exit(0U);
    fixture->timed_size = 1U;
    fixture->indefinite_script[0] = wait_exit(0U);
    fixture->indefinite_size = 1U;
    fixture->postflight = good_postflight();
    fixture->input_contract_ok = true;
    fixture->canary_contract_ok = true;
}

static MeisoK230dDrmSupervisorInvokeStatus run_supervisor(
    SupervisorFixture *fixture,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *out_result
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig config =
        supervisor_config(fixture);
    MeisoK230dDrmSinglePlaneTestOnlySupervisorOps ops = supervisor_ops();

    fixture->config_to_mutate = &config;
    fixture->ops_to_mutate = &ops;
    return meiso_k230d_drm_private_supervise_single_plane_test_only_child(
        &config, &ops, fixture, out_result
    );
}

static bool test_happy_natural_reap(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    memset(&result, 0xa5, sizeof(result));
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure == MEISO_K230D_DRM_SUPERVISOR_FAILURE_NONE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_WITHOUT_SIGNAL_CALL);
    CHECK(result.child_spawned == 1U && result.child_pid == 123U);
    CHECK(result.child_ready_and_stopped == 1U);
    CHECK(result.child_gate_released == 1U);
    CHECK(result.child_reaped == 1U);
    CHECK(result.final_reap.kind ==
          MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED);
    CHECK(result.final_reap.exit_code_valid == 1U);
    CHECK(result.final_reap.child_exit_code == 0U);
    CHECK(result.term_deadline_ns == 115U);
    CHECK(result.kill_deadline_ns == 117U);
    CHECK(result.quarantine_deadline_ns == 120U);
    CHECK(result.preflight_call_count == 2U);
    CHECK(result.timed_wait_call_count == 1U);
    CHECK(result.terminate_call_count == 0U && result.kill_call_count == 0U);
    CHECK(result.quarantine_record_attempted == 0U);
    CHECK(result.postflight_succeeded == 1U);
    CHECK(result.terminal_record_attempted == 1U);
    CHECK(result.terminal_record.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    CHECK(fixture.event_count == 11U);
    CHECK(fixture.events[0] == EVENT_PREFLIGHT_0);
    CHECK(fixture.events[1] == EVENT_GLOBAL_CLAIM);
    CHECK(fixture.events[2] == EVENT_EVIDENCE_CLAIM);
    CHECK(fixture.events[3] == EVENT_SPAWN);
    CHECK(fixture.events[4] == EVENT_GATE);
    CHECK(fixture.events[5] == EVENT_PREFLIGHT_1);
    CHECK(fixture.events[6] == EVENT_READY_RECORD);
    CHECK(fixture.events[7] == EVENT_RELEASE);
    CHECK(fixture.events[8] == EVENT_TIMED_WAIT);
    CHECK(fixture.events[9] == EVENT_POSTFLIGHT);
    CHECK(fixture.events[10] == EVENT_TERMINAL_RECORD);
    return true;
}

static bool test_claim_failures_and_partial_state(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.global_claim = already_exists_durable();
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_NO_CHILD_FAILURE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_GLOBAL_CLAIM);
    CHECK(result.first_failure_error == (uint32_t)EEXIST);
    CHECK(result.global_claim_attempted == 1U);
    CHECK(result.evidence_claim_attempted == 0U);
    CHECK(result.spawn_attempted == 0U);

    init_supervisor_fixture(&fixture);
    fixture.evidence_claim = uncertain_durable(
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_BODY_SYNCED,
        (uint32_t)EIO
    );
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_EVIDENCE_CLAIM);
    CHECK(result.first_failure_error == (uint32_t)EIO);
    CHECK(result.global_claim.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED);
    CHECK(result.evidence_claim.progress ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_BODY_SYNCED);
    CHECK(result.spawn_attempted == 0U);

    init_supervisor_fixture(&fixture);
    fixture.global_return = EIO;
    fixture.global_mutate_output = true;
    fixture.global_mutate_binding = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.global_claim_callback_error == (uint32_t)EPROTO);
    CHECK(result.global_claim.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.global_claim.progress ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN);
    CHECK(result.global_claim.error_number == (uint32_t)EPROTO);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    return true;
}

static bool test_gate_early_reap(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.gate_script[0] = exited_gate(7U);
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CHILD_GATE);
    CHECK(result.first_failure_error == (uint32_t)ECHILD);
    CHECK(result.child_reaped == 1U);
    CHECK(result.child_ready_and_stopped == 0U);
    CHECK(result.final_reap.child_exit_code == 7U);
    CHECK(result.release_attempted == 0U);
    CHECK(result.kill_call_count == 0U);
    CHECK(result.quarantine_record_attempted == 0U);
    CHECK(result.postflight_succeeded == 1U);
    CHECK(result.terminal_record_attempted == 1U);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_WITHOUT_SIGNAL_CALL);
    return true;
}

static bool test_gate_interrupt_then_stop_fences_owned_child(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    memset(&fixture.gate_script[0], 0,
           sizeof(fixture.gate_script[0]));
    fixture.gate_script[0].kind =
        MEISO_K230D_DRM_SUPERVISOR_GATE_INTERRUPTED;
    fixture.gate_script[0].ready_byte_observed = 1U;
    memset(&fixture.gate_script[1], 0,
           sizeof(fixture.gate_script[1]));
    fixture.gate_script[1].kind =
        MEISO_K230D_DRM_SUPERVISOR_GATE_STOP_REQUESTED;
    fixture.gate_size = 2U;

    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.gate_observation_call_count == 2U);
    CHECK(result.interrupted_wait_count == 1U);
    CHECK(result.stop_requested == 1U && result.stop_request_count == 1U);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_STOP_REQUESTED);
    CHECK(result.child_ready_and_stopped == 0U);
    CHECK(result.preflight_call_count == 1U);
    CHECK(result.ready_record_attempted == 0U);
    CHECK(result.release_attempted == 0U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.indefinite_wait_call_count == 1U);
    CHECK(result.child_reaped == 1U);
    return true;
}

static bool test_release_uncertainty_and_missing_t0(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.release_return = EIO;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE);
    CHECK(result.release_callback_error == (uint32_t)EIO);
    CHECK(result.release.kind ==
          MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN);
    CHECK(result.child_release_state_uncertain == 1U);
    CHECK(result.child_gate_released == 0U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.indefinite_wait_call_count == 1U);

    init_supervisor_fixture(&fixture);
    fixture.release = released_without_t0((uint32_t)EIO);
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE);
    CHECK(result.release_callback_error == 0U);
    CHECK(result.child_gate_released == 1U);
    CHECK(result.child_release_state_uncertain == 0U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.timed_wait_call_count == 0U);
    CHECK(result.quarantine_record_attempted == 1U);
    return true;
}

static bool test_exact_deadlines_errors_and_indefinite_reap(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[1] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.timed_script[2] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED
    );
    fixture.timed_return[3] = EINTR;
    fixture.timed_script[4] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_size = 5U;
    fixture.clock_value[0] = 115U;
    fixture.clock_value[1] = 116U;
    fixture.clock_value[2] = 117U;
    fixture.clock_value[3] = 119U;
    fixture.clock_value[4] = 120U;
    fixture.clock_size = 5U;
    fixture.term_return = EIO;
    fixture.kill_return = -1;
    fixture.quarantine_return = EIO;
    fixture.indefinite_return[0] = EINTR;
    fixture.indefinite_script[1] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.indefinite_script[2] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED
    );
    fixture.indefinite_script[3] = wait_exit(1U);
    fixture.indefinite_size = 4U;

    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_DEADLINE_EXPIRED);
    CHECK(result.first_failure_error == (uint32_t)ETIMEDOUT);
    CHECK(result.term_deadline_confirmed == 1U);
    CHECK(result.kill_deadline_confirmed == 1U);
    CHECK(result.quarantine_deadline_confirmed == 1U);
    CHECK(result.term_deadline_observed_now_ns == 115U);
    CHECK(result.kill_deadline_observed_now_ns == 117U);
    CHECK(result.quarantine_deadline_observed_now_ns == 120U);
    CHECK(result.terminate_call_count == 1U && fixture.term_calls == 1U);
    CHECK(result.kill_call_count == 1U && fixture.kill_calls == 1U);
    CHECK(result.term_error == (uint32_t)EIO);
    CHECK(result.kill_error == (uint32_t)EPROTO);
    CHECK(result.timed_wait_call_count == 5U);
    CHECK(result.indefinite_wait_call_count == 4U);
    CHECK(result.interrupted_wait_count == 2U);
    CHECK(result.stop_request_count == 2U);
    CHECK(result.stop_requested == 1U);
    CHECK(result.first_wait_callback_error == (uint32_t)EINTR);
    CHECK(fixture.timed_deadline_seen[0] == 115U);
    CHECK(fixture.timed_deadline_seen[1] == 117U);
    CHECK(fixture.timed_deadline_seen[2] == 117U);
    CHECK(fixture.timed_deadline_seen[3] == 120U);
    CHECK(fixture.timed_deadline_seen[4] == 120U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.quarantine_record_callback_error == (uint32_t)EIO);
    CHECK(result.quarantine_record.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.child_reaped == 1U);
    CHECK(result.final_reap.child_exit_code == 1U);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_QUARANTINE_ATTEMPT);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    return true;
}

static bool test_clock_rollback_and_schedule_overflow(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.timed_script[1] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.timed_size = 2U;
    fixture.clock_value[0] = 110U;
    fixture.clock_value[1] = 109U;
    fixture.clock_size = 2U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.first_clock_callback_error == (uint32_t)EPROTO);
    CHECK(result.timed_wait_call_count == 2U);
    CHECK(fixture.clock_calls == 2U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.indefinite_wait_call_count == 1U);

    init_supervisor_fixture(&fixture);
    fixture.release = released_at(UINT64_MAX - 10U);
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CLOCK);
    CHECK(result.first_failure_error == (uint32_t)EOVERFLOW);
    CHECK(result.term_deadline_ns == 0U);
    CHECK(result.timed_wait_call_count == 0U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    return true;
}

static bool test_postflight_and_record_debts(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.postflight.primary_holder_count = 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_POSTFLIGHT);
    CHECK(result.first_failure_error == (uint32_t)EBUSY);
    CHECK(result.postflight_attempted == 1U);
    CHECK(result.postflight_succeeded == 0U);
    CHECK(result.postflight_callback_error == 0U);
    CHECK(result.terminal_record_attempted == 0U);

    init_supervisor_fixture(&fixture);
    fixture.postflight_return = EIO;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_POSTFLIGHT);
    CHECK(result.postflight_callback_error == (uint32_t)EIO);
    CHECK(result.terminal_record_attempted == 0U);

    init_supervisor_fixture(&fixture);
    fixture.terminal_record = already_exists_durable();
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_TERMINAL_RECORD);
    CHECK(result.first_failure_error == (uint32_t)EEXIST);
    CHECK(result.postflight_succeeded == 1U);
    CHECK(result.terminal_record_attempted == 1U);
    CHECK(result.terminal_record_callback_error == 0U);

    init_supervisor_fixture(&fixture);
    fixture.ready_record = already_exists_durable();
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_READY_RECORD);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.indefinite_wait_call_count == 1U);
    CHECK(result.terminal_record_attempted == 1U);

    init_supervisor_fixture(&fixture);
    fixture.ready_mutate_result = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.ready_record_callback_error == (uint32_t)EPROTO);
    CHECK(result.child_pid == 123U);
    CHECK(result.kill_call_count == 1U);
    return true;
}

static bool test_hostile_callback_protocol_matrix(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.preflight_return[0] = EIO;
    fixture.preflight_mutate_output[0] = true;
    fixture.preflight_mutate_binding[0] = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.initial_preflight_callback_error == (uint32_t)EPROTO);
    CHECK(result.spawn_attempted == 0U && result.child_spawned == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.preflight_return[1] = EIO;
    fixture.preflight_mutate_output[1] = true;
    fixture.preflight_mutate_binding[1] = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.pre_release_preflight_callback_error == (uint32_t)EPROTO);
    CHECK(result.child_spawned == 1U && result.child_reaped == 1U);
    CHECK(result.ready_record_attempted == 0U);
    CHECK(result.release_attempted == 0U);
    CHECK(result.kill_call_count == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.evidence_return = EIO;
    fixture.evidence_mutate_output = true;
    fixture.evidence_mutate_binding = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.evidence_claim_callback_error == (uint32_t)EPROTO);
    CHECK(result.spawn_attempted == 0U && result.child_spawned == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.evidence_claim.error_number = (uint32_t)EIO;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.evidence_claim_callback_error == (uint32_t)EPROTO);
    CHECK(result.evidence_claim.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.spawn_attempted == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.spawn_return = EIO;
    fixture.spawn_mutate_output = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.spawn_callback_error == (uint32_t)EPROTO);
    CHECK(result.child_spawned == 0U && result.child_pid == 0U);
    CHECK(result.kill_call_count == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.spawn_mutate_binding = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.spawn_callback_error == (uint32_t)EPROTO);
    CHECK(result.child_spawned == 1U && result.child_reaped == 1U);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    /*
     * The fake callback never creates an OS process. These invalid-success
     * cases test protocol/no-authority reporting only, not hidden-child
     * containment against a Byzantine spawn provider.
     */
    init_supervisor_fixture(&fixture);
    fixture.spawn_pid = 0U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.spawn_callback_error == (uint32_t)EPROTO);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_NO_CHILD_FAILURE);
    CHECK(result.child_spawned == 0U && result.child_pid == 0U);
    CHECK(result.kill_call_count == 0U);
    CHECK(result.postflight_attempted == 0U);
    CHECK(result.terminal_record_attempted == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.spawn_pid = (uint32_t)INT32_MAX + 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.spawn_callback_error == (uint32_t)EPROTO);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_NO_CHILD_FAILURE);
    CHECK(result.child_spawned == 0U && result.child_pid == 0U);
    CHECK(result.kill_call_count == 0U);
    CHECK(result.postflight_attempted == 0U);
    CHECK(result.terminal_record_attempted == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.gate_return[0] = EIO;
    fixture.gate_mutate_output[0] = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.gate_callback_error == (uint32_t)EPROTO);
    CHECK(result.kill_call_count == 1U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.gate_script[0].child_start_ticks = 0U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.gate_callback_error == (uint32_t)EPROTO);
    CHECK(result.child_ready_and_stopped == 0U);
    CHECK(result.kill_call_count == 1U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.ready_return = EIO;
    fixture.ready_mutate_output = true;
    fixture.ready_mutate_result = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.ready_record_callback_error == (uint32_t)EPROTO);
    CHECK(result.ready_record.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.child_pid == 123U && result.child_reaped == 1U);
    CHECK(result.kill_call_count == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.release_return = EIO;
    fixture.release_mutate_output = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.release_callback_error == (uint32_t)EPROTO);
    CHECK(result.release.kind ==
          MEISO_K230D_DRM_SUPERVISOR_RELEASE_STATE_UNCERTAIN);
    CHECK(result.child_release_state_uncertain == 1U);
    CHECK(result.kill_call_count == 1U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.timed_return[0] = EIO;
    fixture.timed_mutate_output[0] = true;
    fixture.timed_script[1] = wait_exit(0U);
    fixture.timed_size = 2U;
    fixture.clock_value[0] = 110U;
    fixture.clock_size = 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.first_wait_callback_error == (uint32_t)EPROTO);
    CHECK(result.timed_wait_call_count == 2U);
    CHECK(fixture.timed_deadline_seen[0] == 115U);
    CHECK(fixture.timed_deadline_seen[1] == 115U);
    CHECK(result.kill_call_count == 0U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.clock_return[0] = EIO;
    fixture.clock_mutate_output[0] = true;
    fixture.clock_value[0] = 110U;
    fixture.clock_size = 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.first_clock_callback_error == (uint32_t)EPROTO);
    CHECK(result.kill_call_count == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.release = released_without_t0((uint32_t)EIO);
    fixture.quarantine_return = EIO;
    fixture.quarantine_mutate_output = true;
    fixture.quarantine_mutate_result = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_RELEASE_GATE);
    CHECK(result.quarantine_record_callback_error == (uint32_t)EPROTO);
    CHECK(result.quarantine_record.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.child_pid == 123U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.release = released_without_t0((uint32_t)EIO);
    fixture.indefinite_return[0] = EIO;
    fixture.indefinite_mutate_output[0] = true;
    fixture.indefinite_script[1] = wait_exit(0U);
    fixture.indefinite_size = 2U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_wait_callback_error == (uint32_t)EPROTO);
    CHECK(result.indefinite_wait_call_count == 2U);
    CHECK(result.kill_call_count == 1U && result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.postflight_return = EIO;
    fixture.postflight_mutate_output = true;
    fixture.postflight_mutate_binding = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.postflight_callback_error == (uint32_t)EPROTO);
    CHECK(result.postflight_attempted == 1U);
    CHECK(result.postflight_succeeded == 0U);
    CHECK(result.terminal_record_attempted == 0U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.terminal_return = EIO;
    fixture.terminal_mutate_output = true;
    fixture.terminal_mutate_result = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.terminal_record_callback_error == (uint32_t)EPROTO);
    CHECK(result.terminal_record.disposition ==
          MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN);
    CHECK(result.postflight_succeeded == 1U);
    CHECK(result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    return true;
}

static bool test_deadline_transition_matrix(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[1] = wait_exit(0U);
    fixture.timed_size = 2U;
    fixture.clock_value[0] = 115U;
    fixture.clock_size = 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.terminate_call_count == 1U && fixture.term_calls == 1U);
    CHECK(result.kill_call_count == 0U && fixture.kill_calls == 0U);
    CHECK(result.quarantine_record_attempted == 0U);
    CHECK(result.term_deadline_confirmed == 1U);
    CHECK(result.kill_deadline_confirmed == 0U);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_TERM_CALL);
    CHECK(fixture.timed_deadline_seen[0] == 115U);
    CHECK(fixture.timed_deadline_seen[1] == 117U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[1] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[2] = wait_exit(0U);
    fixture.timed_size = 3U;
    fixture.clock_value[0] = 115U;
    fixture.clock_value[1] = 117U;
    fixture.clock_size = 2U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.terminate_call_count == 1U && fixture.term_calls == 1U);
    CHECK(result.kill_call_count == 1U && fixture.kill_calls == 1U);
    CHECK(result.quarantine_record_attempted == 0U);
    CHECK(result.term_deadline_confirmed == 1U);
    CHECK(result.kill_deadline_confirmed == 1U);
    CHECK(result.quarantine_deadline_confirmed == 0U);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_KILL_CALL);
    CHECK(fixture.timed_deadline_seen[0] == 115U);
    CHECK(fixture.timed_deadline_seen[1] == 117U);
    CHECK(fixture.timed_deadline_seen[2] == 120U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[1] = wait_exit(0U);
    fixture.timed_size = 2U;
    fixture.clock_value[0] = 110U;
    fixture.clock_size = 1U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_CALLBACK_PROTOCOL);
    CHECK(result.first_wait_callback_error == (uint32_t)EPROTO);
    CHECK(result.timed_wait_call_count == 2U);
    CHECK(fixture.timed_deadline_seen[0] == 115U);
    CHECK(fixture.timed_deadline_seen[1] == 115U);
    CHECK(result.term_deadline_confirmed == 0U);
    CHECK(result.terminate_call_count == 0U && result.kill_call_count == 0U);
    CHECK(result.child_reaped == 1U);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);

    init_supervisor_fixture(&fixture);
    fixture.timed_script[0] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[1] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE
    );
    fixture.timed_script[2] = wait_simple(
        MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED
    );
    fixture.timed_size = 3U;
    fixture.clock_value[0] = 115U;
    fixture.clock_value[1] = 117U;
    fixture.clock_return[2] = EIO;
    fixture.clock_size = 3U;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.first_failure ==
          MEISO_K230D_DRM_SUPERVISOR_FAILURE_DEADLINE_EXPIRED);
    CHECK(result.first_clock_callback_error == (uint32_t)EIO);
    CHECK(result.terminate_call_count == 1U && fixture.term_calls == 1U);
    CHECK(result.kill_call_count == 1U && fixture.kill_calls == 1U);
    CHECK(result.quarantine_record_attempted == 1U);
    CHECK(result.quarantine_deadline_confirmed == 0U);
    CHECK(result.indefinite_wait_call_count == 1U);
    CHECK(result.child_reaped == 1U);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_AFTER_QUARANTINE_ATTEMPT);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    return true;
}

static bool test_parent_alias_validation_and_snapshots(void)
{
    SupervisorFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult before;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig config;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorOps ops;
    union {
        max_align_t alignment;
        uint8_t bytes[
            sizeof(MeisoK230dDrmSinglePlaneTestOnlySupervisorResult)
        ];
    } alias;
    uint8_t alias_before[sizeof(alias.bytes)];

    init_supervisor_fixture(&fixture);
    config = supervisor_config(&fixture);
    ops = supervisor_ops();
    memset(&result, 0xa5, sizeof(result));
    memcpy(&before, &result, sizeof(before));
    CHECK(meiso_k230d_drm_private_supervise_single_plane_test_only_child(
              NULL, &ops, &fixture, &result
          ) == MEISO_K230D_DRM_SUPERVISOR_INVALID_ARGUMENT);
    CHECK(memcmp(&result, &before, sizeof(result)) == 0);

    memset(&alias, 0, sizeof(alias));
    memcpy(alias.bytes, &config, sizeof(config));
    memcpy(alias_before, alias.bytes, sizeof(alias_before));
    CHECK(meiso_k230d_drm_private_supervise_single_plane_test_only_child(
              (const MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig *)
                  alias.bytes,
              &ops, &fixture,
              (MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *)
                  alias.bytes
          ) == MEISO_K230D_DRM_SUPERVISOR_INVALID_ARGUMENT);
    CHECK(memcmp(alias.bytes, alias_before, sizeof(alias_before)) == 0);

    memset(&result, 0xa5, sizeof(result));
    memcpy(&before, &result, sizeof(before));
    ops.inspect_preflight = NULL;
    CHECK(meiso_k230d_drm_private_supervise_single_plane_test_only_child(
              &config, &ops, &fixture, &result
          ) == MEISO_K230D_DRM_SUPERVISOR_INVALID_CONFIG);
    CHECK(memcmp(&result, &before, sizeof(result)) == 0);

    init_supervisor_fixture(&fixture);
    fixture.mutate_originals_on_first_preflight = true;
    CHECK(run_supervisor(&fixture, &result) ==
          MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE);
    CHECK(result.child_reaped == 1U);
    CHECK(result.first_failure == MEISO_K230D_DRM_SUPERVISOR_FAILURE_NONE);
    CHECK(fixture.input_contract_ok && fixture.canary_contract_ok);
    return true;
}

typedef uint8_t ChildEvent;
enum {
    CHILD_EVENT_READ_1 = 1,
    CHILD_EVENT_ARM = 2,
    CHILD_EVENT_READ_2 = 3,
    CHILD_EVENT_ISOLATE = 4,
    CHILD_EVENT_RESET = 5,
    CHILD_EVENT_READY = 6,
    CHILD_EVENT_STOP = 7,
    CHILD_EVENT_RUN = 8
};

typedef struct {
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest expected_request;
    uint32_t parent_id[2];
    int read_return[2];
    bool read_mutate_output[2];
    uint32_t read_calls;
    int arm_return;
    int isolate_return;
    int reset_return;
    int ready_return;
    int stop_return;
    int run_return;
    uint8_t run_exit;
    bool mutate_run_request;
    bool mutate_original_ops;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *original_ops;
    ChildEvent events[16];
    size_t event_count;
    bool sentinel_ok;
} ChildFixture;

static void child_event(ChildFixture *fixture, ChildEvent event)
{
    fixture->events[fixture->event_count++] = event;
}

static int child_read_parent(void *context, uint32_t *out_parent_id)
{
    ChildFixture *fixture = (ChildFixture *)context;
    uint32_t index = fixture->read_calls++;

    if (*out_parent_id != 0U || index > 1U) {
        fixture->sentinel_ok = false;
    }
    child_event(
        fixture, index == 0U ? CHILD_EVENT_READ_1 : CHILD_EVENT_READ_2
    );
    if (fixture->mutate_original_ops && index == 0U) {
        memset(fixture->original_ops, 0, sizeof(*fixture->original_ops));
    }
    if (fixture->read_return[index] == 0 ||
        fixture->read_mutate_output[index]) {
        *out_parent_id = fixture->parent_id[index];
    }
    return fixture->read_return[index];
}

static int child_arm(void *context)
{
    ChildFixture *fixture = (ChildFixture *)context;
    child_event(fixture, CHILD_EVENT_ARM);
    return fixture->arm_return;
}

static int child_isolate(void *context)
{
    ChildFixture *fixture = (ChildFixture *)context;
    child_event(fixture, CHILD_EVENT_ISOLATE);
    return fixture->isolate_return;
}

static int child_reset(void *context)
{
    ChildFixture *fixture = (ChildFixture *)context;
    child_event(fixture, CHILD_EVENT_RESET);
    return fixture->reset_return;
}

static int child_ready(void *context)
{
    ChildFixture *fixture = (ChildFixture *)context;
    child_event(fixture, CHILD_EVENT_READY);
    return fixture->ready_return;
}

static int child_stop(void *context)
{
    ChildFixture *fixture = (ChildFixture *)context;
    child_event(fixture, CHILD_EVENT_STOP);
    return fixture->stop_return;
}

static int child_run(
    void *context,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    uint8_t *out_exit_code
)
{
    ChildFixture *fixture = (ChildFixture *)context;

    if (*out_exit_code != 0xffU ||
        memcmp(request, &fixture->expected_request, sizeof(*request)) != 0) {
        fixture->sentinel_ok = false;
    }
    child_event(fixture, CHILD_EVENT_RUN);
    if (fixture->mutate_run_request) {
        ((MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *)request)
            ->connector_id ^= 1U;
    }
    if (fixture->run_return == 0) {
        *out_exit_code = fixture->run_exit;
    }
    return fixture->run_return;
}

static MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps child_ops(void)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps ops;

    memset(&ops, 0, sizeof(ops));
    ops.read_parent_id = child_read_parent;
    ops.arm_parent_death_kill = child_arm;
    ops.isolate_fixed_child_file_descriptors = child_isolate;
    ops.reset_and_unblock_child_signals_with_sigpipe_ignored = child_reset;
    ops.notify_ready_and_close_channel = child_ready;
    ops.stop_self_until_continued = child_stop;
    ops.run_derived_v2 = child_run;
    return ops;
}

static void init_child_fixture(ChildFixture *fixture)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding =
        fixture_binding();

    memset(fixture, 0, sizeof(*fixture));
    fixture->expected_request = binding.derive_request;
    fixture->parent_id[0] = 42U;
    fixture->parent_id[1] = 42U;
    fixture->run_exit = 1U;
    fixture->sentinel_ok = true;
}

static MeisoK230dDrmSupervisorChildStatus run_child(
    ChildFixture *fixture,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *ops,
    uint8_t *out_exit
)
{
    fixture->original_ops = ops;
    return meiso_k230d_drm_private_run_single_plane_test_only_child(
        42U, &fixture->expected_request, ops, fixture, out_exit
    );
}

static bool test_child_exact_order_and_ops_snapshot(void)
{
    ChildFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps ops;
    uint8_t exit_code = 0xa5U;
    static const ChildEvent expected[] = {
        CHILD_EVENT_READ_1,
        CHILD_EVENT_ARM,
        CHILD_EVENT_READ_2,
        CHILD_EVENT_ISOLATE,
        CHILD_EVENT_RESET,
        CHILD_EVENT_READY,
        CHILD_EVENT_STOP,
        CHILD_EVENT_RUN
    };

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.mutate_original_ops = true;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED);
    CHECK(exit_code == 1U);
    CHECK(fixture.event_count == sizeof(expected) / sizeof(expected[0]));
    CHECK(memcmp(fixture.events, expected, sizeof(expected)) == 0);
    CHECK(fixture.sentinel_ok);
    return true;
}

static bool test_child_parent_and_setup_failures(void)
{
    ChildFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps ops;
    uint8_t exit_code;

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.parent_id[1] = 43U;
    exit_code = 0xa5U;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE);
    CHECK(exit_code == 0xa5U);
    CHECK(fixture.event_count == 3U);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.parent_id[0] = (uint32_t)INT32_MAX + 1U;
    exit_code = 0x5aU;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);
    CHECK(exit_code == 0x5aU);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.read_return[0] = EIO;
    fixture.read_mutate_output[0] = true;
    exit_code = 0x5aU;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);
    CHECK(exit_code == 0x5aU);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.arm_return = EIO;
    exit_code = 0x5aU;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_SETUP_FAILURE);
    CHECK(fixture.event_count == 2U);
    CHECK(exit_code == 0x5aU);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.arm_return = -1;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);
    return true;
}

static bool test_child_request_mutation_provider_and_aliases(void)
{
    ChildFixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps ops;
    uint8_t exit_code;
    union {
        max_align_t alignment;
        uint8_t bytes[
            sizeof(MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps)
        ];
    } alias;

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.mutate_run_request = true;
    exit_code = 0xa5U;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);
    CHECK(exit_code == 0xa5U);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.run_return = EIO;
    exit_code = 0xa5U;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);
    CHECK(exit_code == 0xa5U);

    init_child_fixture(&fixture);
    ops = child_ops();
    fixture.run_exit = 2U;
    CHECK(run_child(&fixture, &ops, &exit_code) ==
          MEISO_K230D_DRM_SUPERVISOR_CHILD_PROVIDER_PROTOCOL_FAILURE);

    memset(&alias, 0, sizeof(alias));
    memcpy(alias.bytes, &fixture.expected_request,
           sizeof(fixture.expected_request));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only_child(
              42U,
              (const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *)
                  alias.bytes,
              (const MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps *)
                  alias.bytes,
              &fixture, &exit_code
          ) == MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_ARGUMENT);

    ops = child_ops();
    ops.run_derived_v2 = NULL;
    exit_code = 0x3cU;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only_child(
              42U, &fixture.expected_request, &ops, &fixture, &exit_code
          ) == MEISO_K230D_DRM_SUPERVISOR_CHILD_INVALID_CONFIG);
    CHECK(exit_code == 0x3cU);
    return true;
}

int main(void)
{
    if (!test_happy_natural_reap() ||
        !test_claim_failures_and_partial_state() ||
        !test_gate_early_reap() ||
        !test_gate_interrupt_then_stop_fences_owned_child() ||
        !test_release_uncertainty_and_missing_t0() ||
        !test_exact_deadlines_errors_and_indefinite_reap() ||
        !test_clock_rollback_and_schedule_overflow() ||
        !test_postflight_and_record_debts() ||
        !test_hostile_callback_protocol_matrix() ||
        !test_deadline_transition_matrix() ||
        !test_parent_alias_validation_and_snapshots() ||
        !test_child_exact_order_and_ops_snapshot() ||
        !test_child_parent_and_setup_failures() ||
        !test_child_request_mutation_provider_and_aliases()) {
        return 1;
    }
    (void)printf("K230D_DRM_SINGLE_PLANE_TEST_ONLY_SUPERVISOR_TEST_PASSED\n");
    return 0;
}
