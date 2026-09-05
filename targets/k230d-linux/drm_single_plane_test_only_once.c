#define _POSIX_C_SOURCE 200809L

#include "adapters/meiso_drm_libdrm_single_plane_test_only_private.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char bytes[MEISO_K230D_DRM_NATIVE_TEST_ONLY_RECEIPT_CAPACITY];
    size_t size;
    bool valid;
} ReceiptBuffer;

static bool bool_representation_valid(const bool *value)
{
    bool zero = false;
    bool one = true;
    unsigned char bytes[sizeof(*value)];
    unsigned char zero_bytes[sizeof(zero)];
    unsigned char one_bytes[sizeof(one)];

    memcpy(bytes, value, sizeof(bytes));
    memcpy(zero_bytes, &zero, sizeof(zero_bytes));
    memcpy(one_bytes, &one, sizeof(one_bytes));
    return memcmp(bytes, zero_bytes, sizeof(bytes)) == 0 ||
        memcmp(bytes, one_bytes, sizeof(bytes)) == 0;
}

#define BOOL_REP_VALID(object, member) \
    bool_representation_valid(&(object)->member)

static bool result_boolean_representations_valid(
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result
)
{
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable;
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native;
    uint32_t index;

    if (result == NULL) {
        return false;
    }
    portable = &result->portable;
    native = &result->native;
    if (!BOOL_REP_VALID(portable, initial_candidate_identified) ||
        !BOOL_REP_VALID(portable, pretest_candidate_identified) ||
        !BOOL_REP_VALID(portable, posttest_candidate_identified) ||
        !BOOL_REP_VALID(portable, pretest_topology_unchanged) ||
        !BOOL_REP_VALID(portable, posttest_topology_unchanged) ||
        !BOOL_REP_VALID(portable, test_only_attempted) ||
        !BOOL_REP_VALID(
            portable, test_callback_reported_accepted_not_applied
        ) ||
        !BOOL_REP_VALID(portable, fd_was_opened) ||
        !BOOL_REP_VALID(portable, universal_planes_enabled) ||
        !BOOL_REP_VALID(portable, atomic_enabled) ||
        !BOOL_REP_VALID(portable, master_was_acquired) ||
        !BOOL_REP_VALID(portable, master_verified) ||
        !BOOL_REP_VALID(portable, dumb_was_created) ||
        !BOOL_REP_VALID(portable, framebuffer_was_created) ||
        !BOOL_REP_VALID(portable, remove_framebuffer_attempted) ||
        !BOOL_REP_VALID(portable, remove_framebuffer_succeeded) ||
        !BOOL_REP_VALID(portable, destroy_dumb_attempted) ||
        !BOOL_REP_VALID(portable, destroy_dumb_succeeded) ||
        !BOOL_REP_VALID(portable, drop_master_attempted) ||
        !BOOL_REP_VALID(portable, drop_master_succeeded) ||
        !BOOL_REP_VALID(portable, close_attempted) ||
        !BOOL_REP_VALID(portable, close_succeeded) ||
        !BOOL_REP_VALID(portable, fd_ownership_consumed) ||
        !BOOL_REP_VALID(portable, master_ownership_consumed) ||
        !BOOL_REP_VALID(portable, dumb_ownership_consumed) ||
        !BOOL_REP_VALID(portable, framebuffer_ownership_consumed) ||
        !BOOL_REP_VALID(portable, pixel_payload_present) ||
        !BOOL_REP_VALID(native, native_fd_opened) ||
        !BOOL_REP_VALID(native, primary_node_verified) ||
        !BOOL_REP_VALID(native, open_failure_close_terminal_consumed) ||
        !BOOL_REP_VALID(native, same_fd_contract_applicable) ||
        !BOOL_REP_VALID(native, same_fd_contract_verified) ||
        !BOOL_REP_VALID(native, universal_planes_cap_set) ||
        !BOOL_REP_VALID(native, atomic_cap_set) ||
        !BOOL_REP_VALID(native, master_was_acquired) ||
        !BOOL_REP_VALID(native, master_verified) ||
        !BOOL_REP_VALID(native, atomic_request_alloc_succeeded) ||
        !BOOL_REP_VALID(native, atomic_commit_user_data_null) ||
        !BOOL_REP_VALID(native, close_terminal_consumed)) {
        return false;
    }
    for (index = 0U;
         index < MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT;
         ++index) {
        if (!bool_representation_valid(
                &native->topology_read_attempted[index]
            ) ||
            !bool_representation_valid(
                &native->topology_read_ready_for_evaluation[index]
            ) ||
            !bool_representation_valid(
                &native->topology_read_topology_complete[index]
            ) ||
            !bool_representation_valid(
                &native->topology_read_all_plane_properties_complete[index]
            ) ||
            !bool_representation_valid(
                &native->topology_read_objects_all_released[index]
            )) {
            return false;
        }
    }
    return true;
}

#undef BOOL_REP_VALID

static bool error_pair_valid(uint8_t stage, int error)
{
    return (stage == 0U && error == 0) || (stage != 0U && error > 0);
}

static bool hash_is_zero(const uint8_t hash[MEISO_K230D_DRM_SHA256_SIZE])
{
    static const uint8_t ZERO[MEISO_K230D_DRM_SHA256_SIZE] = {0U};
    return memcmp(hash, ZERO, sizeof(ZERO)) == 0;
}

static bool hashes_equal(
    const uint8_t lhs[MEISO_K230D_DRM_SHA256_SIZE],
    const uint8_t rhs[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    return memcmp(lhs, rhs, MEISO_K230D_DRM_SHA256_SIZE) == 0;
}

static bool candidate_observation_contract(
    bool evaluated,
    bool identified,
    bool evaluator_invalid,
    MeisoK230dDrmSinglePlaneTestCandidateReason reason,
    const uint8_t hash[MEISO_K230D_DRM_SHA256_SIZE],
    const uint8_t expected_hash[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    if (!evaluated || evaluator_invalid) {
        return !identified && reason ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE &&
            hash_is_zero(hash);
    }
    if (identified) {
        return reason ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE &&
            hashes_equal(hash, expected_hash);
    }
    if (reason ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_CAPACITY ||
        reason ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_NONCANONICAL) {
        return hash_is_zero(hash);
    }
    if (reason ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH) {
        return !hash_is_zero(hash) && !hashes_equal(hash, expected_hash);
    }
    return reason >
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH &&
        reason <=
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MODE_ENVELOPE &&
        hashes_equal(hash, expected_hash);
}

static const char *candidate_reason_token(
    MeisoK230dDrmSinglePlaneTestCandidateReason reason
)
{
    switch (reason) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE:
            return "NONE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_CAPACITY:
            return "TOPOLOGY_CAPACITY";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_NONCANONICAL:
            return "TOPOLOGY_NONCANONICAL";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH:
            return "TOPOLOGY_IDENTITY_MISMATCH";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NO_ACTIVE_ROUTE:
            return "NO_ACTIVE_ROUTE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_ACTIVE_ROUTES:
            return "MULTIPLE_ACTIVE_ROUTES";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ROUTE_IDENTITY_MISMATCH:
            return "ROUTE_IDENTITY_MISMATCH";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ACTIVE_CRTC_COUNT:
            return "ACTIVE_CRTC_COUNT";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_LEGACY_FRAMEBUFFER_ACTIVE:
            return "LEGACY_FRAMEBUFFER_ACTIVE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ENUMERATED_PLANE_ACTIVE:
            return "ENUMERATED_PLANE_ACTIVE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NO_TEST_TARGET:
            return "NO_TEST_TARGET";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_TEST_TARGETS:
            return "MULTIPLE_TEST_TARGETS";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TEST_TARGET_IDENTITY_MISMATCH:
            return "TEST_TARGET_IDENTITY_MISMATCH";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MODE_ENVELOPE:
            return "MODE_ENVELOPE";
        default:
            return NULL;
    }
}

static const char *outcome_token(
    MeisoK230dDrmSinglePlaneTestOnlyOutcome outcome
)
{
    switch (outcome) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED:
            return "NOT_TESTED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED:
            return "TEST_REJECTED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED:
            return "TEST_ACCEPTED_NOT_APPLIED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE:
            return "OPERATION_FAILURE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE:
            return "CLEANUP_FAILURE";
        default:
            return NULL;
    }
}

static const char *primary_outcome_token(
    MeisoK230dDrmSinglePlaneTestOnlyPrimaryOutcome outcome
)
{
    switch (outcome) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED:
            return "NOT_TESTED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED:
            return "TEST_REJECTED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED:
            return "TEST_ACCEPTED_NOT_APPLIED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE:
            return "OPERATION_FAILURE";
        default:
            return NULL;
    }
}

static const char *test_observation_token(
    MeisoK230dDrmSinglePlaneTestOnlyTestObservation observation
)
{
    switch (observation) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED:
            return "NOT_ATTEMPTED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED:
            return "REJECTED_NOT_APPLIED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED:
            return "ACCEPTED_NOT_APPLIED";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED:
            return "OPERATION_FAILED_NOT_APPLIED";
        default:
            return NULL;
    }
}

static const char *operation_stage_token(
    MeisoK230dDrmSinglePlaneTestOnlyOperationStage stage
)
{
    switch (stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE:
            return "NONE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY:
            return "OPEN_PRIMARY";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES:
            return "ENABLE_UNIVERSAL_PLANES";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC:
            return "ENABLE_ATOMIC";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER:
            return "ACQUIRE_MASTER";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER:
            return "VERIFY_MASTER";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY:
            return "READ_INITIAL_TOPOLOGY";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE:
            return "EVALUATE_INITIAL_CANDIDATE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB:
            return "CREATE_DUMB";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB:
            return "VALIDATE_DUMB";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER:
            return "ADD_FRAMEBUFFER";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY:
            return "READ_PRETEST_TOPOLOGY";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE:
            return "EVALUATE_PRETEST_CANDIDATE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY:
            return "ATOMIC_TEST_ONLY";
        default:
            return NULL;
    }
}

static const char *postcondition_stage_token(
    MeisoK230dDrmSinglePlaneTestOnlyPostconditionStage stage
)
{
    switch (stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE:
            return "NONE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY:
            return "READ_TOPOLOGY";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_EVALUATE_CANDIDATE:
            return "EVALUATE_CANDIDATE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED:
            return "TOPOLOGY_CHANGED";
        default:
            return NULL;
    }
}

static const char *cleanup_stage_token(
    MeisoK230dDrmSinglePlaneTestOnlyCleanupStage stage
)
{
    switch (stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE:
            return "NONE";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER:
            return "REMOVE_FRAMEBUFFER";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB:
            return "DESTROY_DUMB";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER:
            return "DROP_MASTER";
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE:
            return "CLOSE_DEVICE";
        default:
            return NULL;
    }
}

static const char *native_read_status_token(
    bool attempted,
    MeisoK230dDrmNativeReadStatus status
)
{
    if (!attempted) {
        return "NOT_ATTEMPTED";
    }
    switch (status) {
        case MEISO_K230D_DRM_NATIVE_READ_COMPLETE:
            return "COMPLETE";
        case MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED:
            return "PRECLASSIFIED";
        case MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED:
            return "OPERATION_FAILED";
        default:
            return NULL;
    }
}

static const char *native_stage_token(MeisoK230dDrmNativeStage stage)
{
    switch (stage) {
        case MEISO_K230D_DRM_NATIVE_STAGE_NONE:
            return "NONE";
        case MEISO_K230D_DRM_NATIVE_STAGE_OPEN:
            return "OPEN";
        case MEISO_K230D_DRM_NATIVE_STAGE_FD_CONTRACT:
            return "FD_CONTRACT";
        case MEISO_K230D_DRM_NATIVE_STAGE_MASTER_OBSERVATION:
            return "MASTER_OBSERVATION";
        case MEISO_K230D_DRM_NATIVE_STAGE_UNIVERSAL_PLANES_CAP:
            return "UNIVERSAL_PLANES_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_ATOMIC_CAP:
            return "ATOMIC_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION:
            return "DRIVER_VERSION";
        case MEISO_K230D_DRM_NATIVE_STAGE_TIMESTAMP_CAP:
            return "TIMESTAMP_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES:
            return "RESOURCES";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES:
            return "PLANE_RESOURCES";
        case MEISO_K230D_DRM_NATIVE_STAGE_CONNECTOR:
            return "CONNECTOR";
        case MEISO_K230D_DRM_NATIVE_STAGE_ENCODER:
            return "ENCODER";
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC:
            return "CRTC";
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES:
            return "CRTC_PROPERTIES";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE:
            return "PLANE";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES:
            return "PLANE_PROPERTIES";
        case MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION:
            return "TRANSLATION";
        case MEISO_K230D_DRM_NATIVE_STAGE_CLOSE:
            return "CLOSE";
        default:
            return NULL;
    }
}

static bool portable_operation_stage_reachable(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *result
)
{
    switch (result->operation_failure_stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY:
            return true;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY:
            return result->topology_read_attempt_count == 1U &&
                result->topology_read_success_count == 0U &&
                !result->initial_candidate_identified;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE:
            return result->topology_read_attempt_count == 1U &&
                result->topology_read_success_count == 1U &&
                !result->initial_candidate_identified;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB:
            return result->topology_read_attempt_count == 1U &&
                result->topology_read_success_count == 1U &&
                result->initial_candidate_identified &&
                !result->dumb_was_created &&
                !result->framebuffer_was_created;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB:
            return result->topology_read_attempt_count == 1U &&
                result->topology_read_success_count == 1U &&
                result->initial_candidate_identified &&
                result->dumb_was_created &&
                !result->framebuffer_was_created;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER:
            return result->topology_read_attempt_count == 1U &&
                result->topology_read_success_count == 1U &&
                result->initial_candidate_identified &&
                result->dumb_was_created &&
                !result->framebuffer_was_created;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY:
            return result->topology_read_attempt_count == 2U &&
                result->topology_read_success_count == 1U &&
                result->initial_candidate_identified &&
                result->framebuffer_was_created &&
                !result->pretest_candidate_identified;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE:
            return result->topology_read_attempt_count == 2U &&
                result->topology_read_success_count == 2U &&
                result->initial_candidate_identified &&
                result->framebuffer_was_created &&
                (!result->pretest_candidate_identified ||
                 !result->pretest_topology_unchanged);
        default:
            return false;
    }
}

static bool portable_result_contract(
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    const MeisoK230dDrmSinglePlaneTestOnlyResult *result
)
{
    bool operation_failed;
    bool postcondition_failed;
    bool cleanup_failed;

    if (request == NULL || result == NULL ||
        outcome_token(result->outcome) == NULL ||
        primary_outcome_token(result->primary_outcome) == NULL ||
        test_observation_token(result->test_observation) == NULL ||
        operation_stage_token(result->operation_failure_stage) == NULL ||
        postcondition_stage_token(
            result->postcondition_failure_stage
        ) == NULL ||
        cleanup_stage_token(result->cleanup_failure_stage) == NULL ||
        candidate_reason_token(result->initial_candidate_reason) == NULL ||
        candidate_reason_token(result->pretest_candidate_reason) == NULL ||
        candidate_reason_token(result->posttest_candidate_reason) == NULL ||
        result->connector_id != request->connector_id ||
        result->crtc_id != request->crtc_id ||
        result->plane_id != request->plane_id ||
        result->pixel_payload_present ||
        result->topology_read_attempt_count > 3U ||
        result->topology_read_success_count >
            result->topology_read_attempt_count ||
        result->atomic_test_only_call_count > 1U ||
        !portable_operation_stage_reachable(result) ||
        !error_pair_valid(
            result->operation_failure_stage, result->operation_error
        ) ||
        !error_pair_valid(
            result->postcondition_failure_stage,
            result->postcondition_error
        ) ||
        !error_pair_valid(
            result->cleanup_failure_stage, result->cleanup_error
        )) {
        return false;
    }
    operation_failed = result->operation_failure_stage !=
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE;
    postcondition_failed = result->postcondition_failure_stage !=
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE;
    cleanup_failed = result->cleanup_failure_stage !=
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE;

    if (result->test_only_attempted !=
            (result->atomic_test_only_call_count == 1U) ||
        result->test_only_attempted !=
            (result->test_observation !=
             MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED) ||
        result->test_callback_reported_accepted_not_applied !=
            (result->test_observation ==
             MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED) ||
        ((result->test_observation ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED ||
          result->test_observation ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED) &&
         result->test_error != 0) ||
        ((result->test_observation ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED ||
          result->test_observation ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED) &&
         result->test_error <= 0)) {
        return false;
    }

    if (!candidate_observation_contract(
            result->topology_read_success_count >= 1U,
            result->initial_candidate_identified,
            result->operation_failure_stage ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE,
            result->initial_candidate_reason,
            result->initial_topology_sha256,
            request->expected_topology_sha256
        ) ||
        !candidate_observation_contract(
            result->topology_read_success_count >= 2U &&
                result->initial_candidate_identified,
            result->pretest_candidate_identified,
            result->operation_failure_stage ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE &&
                !result->pretest_candidate_identified,
            result->pretest_candidate_reason,
            result->pretest_topology_sha256,
            request->expected_topology_sha256
        ) ||
        !candidate_observation_contract(
            result->topology_read_success_count >= 3U &&
                result->test_only_attempted,
            result->posttest_candidate_identified,
            result->postcondition_failure_stage ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_EVALUATE_CANDIDATE,
            result->posttest_candidate_reason,
            result->posttest_topology_sha256,
            request->expected_topology_sha256
        ) ||
        (result->initial_candidate_identified &&
         (result->initial_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          result->topology_read_success_count < 1U ||
          !hashes_equal(
              result->initial_topology_sha256,
              request->expected_topology_sha256
          ))) ||
        (result->pretest_candidate_identified &&
         (result->pretest_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          !result->initial_candidate_identified ||
          result->topology_read_success_count < 2U ||
          !hashes_equal(
              result->pretest_topology_sha256,
              request->expected_topology_sha256
          ))) ||
        (result->posttest_candidate_identified &&
         (result->posttest_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          !result->pretest_candidate_identified ||
          result->topology_read_success_count < 3U ||
          !hashes_equal(
              result->posttest_topology_sha256,
              request->expected_topology_sha256
          ))) ||
        (result->pretest_topology_unchanged &&
         (!result->initial_candidate_identified ||
          !result->pretest_candidate_identified)) ||
        (result->posttest_topology_unchanged &&
         (!result->pretest_candidate_identified ||
          !result->posttest_candidate_identified)) ||
        (result->initial_candidate_identified &&
         (result->minimum_output_bytes == 0U ||
          result->minimum_output_bytes > MEISO_K230D_DRM_MAX_DUMB_BYTES)) ||
        (!result->initial_candidate_identified &&
         result->minimum_output_bytes != 0U)) {
        return false;
    }

    if (result->universal_planes_enabled && !result->fd_was_opened) {
        return false;
    }
    if (result->atomic_enabled && !result->universal_planes_enabled) {
        return false;
    }
    if (result->master_was_acquired && !result->atomic_enabled) {
        return false;
    }
    if (result->master_verified && !result->master_was_acquired) {
        return false;
    }
    if (result->dumb_was_created &&
        (!result->master_verified || !result->initial_candidate_identified ||
         result->created_dumb.handle == 0U)) {
        return false;
    }
    if (result->dumb_was_created &&
        result->operation_failure_stage !=
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB &&
        (result->created_dumb.pitch == 0U ||
         result->created_dumb.size < result->minimum_output_bytes ||
         result->created_dumb.size > MEISO_K230D_DRM_MAX_DUMB_BYTES)) {
        return false;
    }
    if (!result->dumb_was_created &&
        (result->created_dumb.handle != 0U ||
         result->created_dumb.pitch != 0U ||
         result->created_dumb.size != 0U)) {
        return false;
    }
    if (result->framebuffer_was_created &&
        (!result->dumb_was_created || result->created_framebuffer_id == 0U)) {
        return false;
    }
    if (!result->framebuffer_was_created &&
        result->created_framebuffer_id != 0U) {
        return false;
    }
    if (result->test_only_attempted &&
        (!result->framebuffer_was_created ||
         !result->pretest_candidate_identified ||
         !result->pretest_topology_unchanged)) {
        return false;
    }

    if (result->remove_framebuffer_attempted !=
            result->framebuffer_was_created ||
        result->destroy_dumb_attempted != result->dumb_was_created ||
        result->drop_master_attempted != result->master_was_acquired ||
        result->close_attempted != result->fd_was_opened ||
        result->remove_framebuffer_succeeded >
            result->remove_framebuffer_attempted ||
        result->destroy_dumb_succeeded > result->destroy_dumb_attempted ||
        result->drop_master_succeeded > result->drop_master_attempted ||
        result->close_succeeded > result->close_attempted ||
        result->fd_ownership_consumed != result->fd_was_opened ||
        result->master_ownership_consumed != result->master_was_acquired ||
        result->dumb_ownership_consumed != result->dumb_was_created ||
        result->framebuffer_ownership_consumed !=
            result->framebuffer_was_created) {
        return false;
    }

    if ((result->master_verified &&
         result->topology_read_attempt_count < 1U) ||
        (result->framebuffer_was_created &&
         result->topology_read_attempt_count < 2U) ||
        (result->topology_read_attempt_count >= 1U &&
         !result->master_verified) ||
        (result->topology_read_attempt_count >= 2U &&
         (!result->framebuffer_was_created ||
          !result->initial_candidate_identified)) ||
        (result->topology_read_attempt_count == 3U) !=
            result->test_only_attempted ||
        (result->topology_read_attempt_count > 0U &&
         !result->initial_candidate_identified &&
         result->topology_read_attempt_count != 1U) ||
        (result->topology_read_attempt_count >= 2U &&
         !result->pretest_candidate_identified &&
         result->topology_read_attempt_count != 2U) ||
        (result->topology_read_attempt_count == 0U &&
         (result->initial_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          !hash_is_zero(result->initial_topology_sha256))) ||
        (result->topology_read_attempt_count < 2U &&
         (result->pretest_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          !hash_is_zero(result->pretest_topology_sha256) ||
          result->pretest_candidate_identified ||
          result->pretest_topology_unchanged)) ||
        (!result->test_only_attempted &&
         (result->postcondition_failure_stage !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE ||
          result->postcondition_error != 0 ||
          result->posttest_candidate_reason !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE ||
          !hash_is_zero(result->posttest_topology_sha256) ||
          result->posttest_candidate_identified ||
          result->posttest_topology_unchanged)) ||
        (result->test_only_attempted &&
         result->topology_read_attempt_count != 3U) ||
        (result->pretest_candidate_identified &&
         !result->pretest_topology_unchanged &&
         (result->primary_outcome !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE ||
          result->operation_failure_stage !=
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE ||
          result->operation_error != EPROTO)) ||
        (result->pretest_topology_unchanged &&
         !result->test_only_attempted)) {
        return false;
    }

    if (result->test_only_attempted) {
        if (result->posttest_candidate_identified) {
            if (result->posttest_topology_unchanged) {
                if (postcondition_failed) {
                    return false;
                }
            } else if (result->postcondition_failure_stage !=
                           MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED ||
                       result->postcondition_error != ESTALE) {
                return false;
            }
        } else if (result->topology_read_success_count == 3U) {
            if (result->posttest_candidate_reason ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE) {
                if (result->postcondition_failure_stage !=
                        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_EVALUATE_CANDIDATE ||
                    result->postcondition_error != EPROTO) {
                    return false;
                }
            } else if (result->postcondition_failure_stage !=
                           MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED ||
                       result->postcondition_error != ESTALE) {
                return false;
            }
        } else if (result->postcondition_failure_stage !=
                       MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY ||
                   result->postcondition_error <= 0) {
            return false;
        }
    }

    switch (result->primary_outcome) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED:
            if (result->test_observation !=
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED ||
                operation_failed || postcondition_failed) {
                return false;
            }
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED:
            if (result->test_observation !=
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED ||
                operation_failed) {
                return false;
            }
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED:
            if (result->test_observation !=
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED ||
                operation_failed) {
                return false;
            }
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE:
            if (!operation_failed ||
                (result->test_observation !=
                     MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED &&
                 result->test_observation !=
                     MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED)) {
                return false;
            }
            break;
        default:
            return false;
    }

    switch (result->outcome) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED:
            return result->primary_outcome ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED &&
                result->test_observation ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED &&
                !operation_failed && !postcondition_failed && !cleanup_failed;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED:
            return result->primary_outcome ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED &&
                result->test_observation ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED &&
                result->posttest_topology_unchanged &&
                !operation_failed && !postcondition_failed && !cleanup_failed;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED:
            return result->primary_outcome ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED &&
                result->test_observation ==
                    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED &&
                result->posttest_topology_unchanged &&
                !operation_failed && !postcondition_failed && !cleanup_failed;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE:
            if (cleanup_failed || (!operation_failed && !postcondition_failed)) {
                return false;
            }
            if (result->primary_outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE) {
                return operation_failed;
            }
            return postcondition_failed &&
                ((result->primary_outcome ==
                      MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED &&
                  result->test_observation ==
                      MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED) ||
                 (result->primary_outcome ==
                      MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED &&
                  result->test_observation ==
                      MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED));
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE:
            if (!cleanup_failed) {
                return false;
            }
            if (result->primary_outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE) {
                return operation_failed;
            }
            return true;
        default:
            return false;
    }
}

static bool native_candidate_read_cross_contract(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    const bool identified[3] = {
        portable->initial_candidate_identified,
        portable->pretest_candidate_identified,
        portable->posttest_candidate_identified
    };
    const MeisoK230dDrmSinglePlaneTestCandidateReason reasons[3] = {
        portable->initial_candidate_reason,
        portable->pretest_candidate_reason,
        portable->posttest_candidate_reason
    };
    uint32_t index;

    for (index = 0U; index < native->topology_read_call_count; ++index) {
        if (native->topology_read_status[index] !=
            MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
            if (reasons[index] !=
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE) {
                return false;
            }
            continue;
        }
        if (!native->topology_read_topology_complete[index]) {
            if (reasons[index] !=
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_CAPACITY) {
                return false;
            }
        } else if (reasons[index] ==
                       MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_CAPACITY ||
                   reasons[index] ==
                       MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_NONCANONICAL) {
            return false;
        }
        if (identified[index] &&
            (native->topology_read_raw_connector_count[index] == 0U ||
             native->topology_read_raw_crtc_count[index] == 0U ||
             native->topology_read_raw_plane_count[index] == 0U)) {
            return false;
        }
        if (reasons[index] ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_TEST_TARGETS &&
            native->topology_read_raw_plane_count[index] < 2U) {
            return false;
        }
    }
    return true;
}

static int native_read_effective_error(
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native,
    uint32_t slot
)
{
    if (native->topology_read_status[slot] ==
        MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED) {
        return native->topology_read_error[slot];
    }
    return EPROTO;
}

static bool native_read_history_reachable(
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native,
    uint32_t slot
)
{
    const uint32_t seen = native->topology_read_objects_seen[slot];
    const uint32_t raw_connectors =
        native->topology_read_raw_connector_count[slot];
    const uint32_t raw_crtcs = native->topology_read_raw_crtc_count[slot];
    const uint32_t raw_planes = native->topology_read_raw_plane_count[slot];

    if ((raw_connectors != 0U || raw_crtcs != 0U || raw_planes != 0U) &&
        seen < 3U) {
        return false;
    }
    if (native->topology_read_status[slot] ==
        MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED) {
        return seen == 1U || seen >= 3U;
    }
    if (native->topology_read_status[slot] !=
        MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED) {
        return true;
    }
    switch (native->topology_read_stage[slot]) {
        case MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION:
            return seen == 0U && raw_connectors == 0U &&
                raw_crtcs == 0U && raw_planes == 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_TIMESTAMP_CAP:
        case MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES:
            return seen == 1U && raw_connectors == 0U &&
                raw_crtcs == 0U && raw_planes == 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES:
            return seen == 2U && raw_connectors == 0U &&
                raw_crtcs == 0U && raw_planes == 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC:
            return seen >= 3U && raw_crtcs > 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES:
            return seen >= 4U && raw_crtcs > 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_CONNECTOR:
            return seen >= 3U && raw_connectors > 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_ENCODER:
            return seen >= 4U && raw_connectors > 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE:
            return seen >= 3U && raw_planes > 0U;
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES:
            return seen >= 4U && raw_planes > 0U;
        default:
            return false;
    }
}

static bool native_read_slot_contract(
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native,
    uint32_t slot
)
{
    const bool attempted = native->topology_read_attempted[slot];
    const MeisoK230dDrmNativeReadStatus status =
        native->topology_read_status[slot];
    const bool over_capacity =
        native->topology_read_raw_connector_count[slot] >
            MEISO_K230D_DRM_MAX_CONNECTORS ||
        native->topology_read_raw_crtc_count[slot] >
            MEISO_K230D_DRM_MAX_CRTCS ||
        native->topology_read_raw_crtc_count[slot] > 32U ||
        native->topology_read_raw_plane_count[slot] >
            MEISO_K230D_DRM_MAX_PLANES;

    if (!attempted) {
        return status == MEISO_K230D_DRM_NATIVE_READ_COMPLETE &&
            native->topology_read_stage[slot] ==
                MEISO_K230D_DRM_NATIVE_STAGE_NONE &&
            native->topology_read_preclassified_reason[slot] ==
                MEISO_K230D_DRM_ADMISSION_NONE &&
            native->topology_read_error[slot] == 0 &&
            !native->topology_read_ready_for_evaluation[slot] &&
            !native->topology_read_topology_complete[slot] &&
            !native->topology_read_all_plane_properties_complete[slot] &&
            native->topology_read_objects_seen[slot] == 0U &&
            native->topology_read_objects_released[slot] == 0U &&
            native->topology_read_raw_connector_count[slot] == 0U &&
            native->topology_read_raw_crtc_count[slot] == 0U &&
            native->topology_read_raw_plane_count[slot] == 0U &&
            !native->topology_read_objects_all_released[slot];
    }
    if (native->topology_read_objects_seen[slot] !=
            native->topology_read_objects_released[slot] ||
        !native->topology_read_objects_all_released[slot] ||
        native_stage_token(native->topology_read_stage[slot]) == NULL ||
        !native_read_history_reachable(native, slot)) {
        return false;
    }
    if (status == MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
        return native->topology_read_stage[slot] ==
                MEISO_K230D_DRM_NATIVE_STAGE_NONE &&
            native->topology_read_error[slot] == 0 &&
            native->topology_read_preclassified_reason[slot] ==
                MEISO_K230D_DRM_ADMISSION_NONE &&
            native->topology_read_ready_for_evaluation[slot] &&
            native->topology_read_objects_seen[slot] >= 3U &&
            (native->topology_read_topology_complete[slot]
                 ? (!over_capacity &&
                    native->topology_read_all_plane_properties_complete[slot])
                 : (over_capacity &&
                    !native->topology_read_all_plane_properties_complete[slot]));
    }
    if (status == MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED) {
        return native->topology_read_stage[slot] ==
                MEISO_K230D_DRM_NATIVE_STAGE_NONE &&
            native->topology_read_error[slot] == 0 &&
            native->topology_read_preclassified_reason[slot] ==
                MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL &&
            !over_capacity &&
            !native->topology_read_ready_for_evaluation[slot] &&
            !native->topology_read_topology_complete[slot] &&
            native->topology_read_objects_seen[slot] >= 1U;
    }
    return status == MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED &&
        native->topology_read_stage[slot] >=
            MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION &&
        native->topology_read_stage[slot] <=
            MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION &&
        native->topology_read_error[slot] > 0 &&
        native->topology_read_preclassified_reason[slot] ==
            MEISO_K230D_DRM_ADMISSION_NONE &&
        !over_capacity &&
        !native->topology_read_ready_for_evaluation[slot] &&
        !native->topology_read_topology_complete[slot] &&
        !native->topology_read_all_plane_properties_complete[slot];
}

static bool native_zero_after_open_failure(
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    return !native->universal_planes_cap_set &&
        !native->atomic_cap_set && !native->master_was_acquired &&
        !native->master_verified && native->same_fd_check_count == 0U &&
        native->universal_planes_cap_call_count == 0U &&
        native->universal_planes_cap_error == 0 &&
        native->atomic_cap_call_count == 0U &&
        native->atomic_cap_error == 0 &&
        native->set_master_call_count == 0U &&
        native->set_master_error == 0 &&
        native->is_master_call_count == 0U &&
        native->is_master_error == 0 &&
        native->topology_read_call_count == 0U &&
        native->create_dumb_api_call_count == 0U &&
        native->create_dumb_error == 0 &&
        native->add_framebuffer_call_count == 0U &&
        native->add_framebuffer_error == 0 &&
        native->portable_atomic_callback_call_count == 0U &&
        native->atomic_request_alloc_call_count == 0U &&
        !native->atomic_request_alloc_succeeded &&
        native->atomic_request_alloc_error == 0 &&
        native->atomic_plane_property_add_attempt_count == 0U &&
        native->atomic_plane_property_add_success_count == 0U &&
        native->atomic_plane_property_add_error == 0 &&
        native->atomic_commit_api_call_count == 0U &&
        native->atomic_commit_flags == 0U &&
        !native->atomic_commit_user_data_null &&
        native->atomic_commit_error == 0 &&
        native->atomic_request_free_call_count == 0U &&
        native->remove_framebuffer_call_count == 0U &&
        native->remove_framebuffer_error == 0 &&
        native->destroy_dumb_api_call_count == 0U &&
        native->destroy_dumb_error == 0 &&
        native->drop_master_call_count == 0U &&
        native->drop_master_error == 0 &&
        native->close_call_count == 0U && native->close_error == 0 &&
        !native->close_terminal_consumed;
}

static bool native_operation_error_matches(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    int native_error = 0;

    switch (portable->operation_failure_stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE:
            return portable->operation_error == 0;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY:
            native_error = native->open_primary_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES:
            native_error = native->universal_planes_cap_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC:
            native_error = native->atomic_cap_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER:
            native_error = native->set_master_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER:
            native_error = native->is_master_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY:
            if (native->topology_read_call_count < 1U ||
                native->topology_read_status[0] ==
                    MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
                return false;
            }
            native_error = native_read_effective_error(native, 0U);
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB:
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE:
            native_error = EPROTO;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB:
            native_error = native->create_dumb_error == 0
                ? EPROTO : native->create_dumb_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER:
            native_error = native->add_framebuffer_error == 0
                ? EPROTO : native->add_framebuffer_error;
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY:
            if (native->topology_read_call_count < 2U ||
                native->topology_read_status[1] ==
                    MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
                return false;
            }
            native_error = native_read_effective_error(native, 1U);
            break;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY:
            if (native->atomic_request_alloc_error > 0) {
                native_error = native->atomic_request_alloc_error;
            } else if (native->atomic_plane_property_add_error > 0) {
                native_error = native->atomic_plane_property_add_error;
            } else {
                native_error = native->atomic_commit_error;
            }
            break;
        default:
            return false;
    }
    return native_error == portable->operation_error;
}

static bool native_cleanup_error_matches(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    switch (portable->cleanup_failure_stage) {
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE:
            return portable->cleanup_error == 0;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER:
            return portable->cleanup_error ==
                native->remove_framebuffer_error;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB:
            return portable->cleanup_error == native->destroy_dumb_error;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER:
            return portable->cleanup_error == native->drop_master_error;
        case MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE:
            return portable->cleanup_error == native->close_error;
        default:
            return false;
    }
}

static bool native_first_failures_are_reported(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    MeisoK230dDrmSinglePlaneTestOnlyOperationStage operation_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE;
    MeisoK230dDrmSinglePlaneTestOnlyCleanupStage cleanup_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE;
    int operation_error = 0;
    int cleanup_error = 0;

#define TAKE_OPERATION_FAILURE(condition, stage_value, error_value) \
    do { \
        if (operation_stage == \
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE && \
            (condition)) { \
            operation_stage = (stage_value); \
            operation_error = (error_value); \
        } \
    } while (0)
    TAKE_OPERATION_FAILURE(
        native->universal_planes_cap_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES,
        native->universal_planes_cap_error
    );
    TAKE_OPERATION_FAILURE(
        native->atomic_cap_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC,
        native->atomic_cap_error
    );
    TAKE_OPERATION_FAILURE(
        native->set_master_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER,
        native->set_master_error
    );
    TAKE_OPERATION_FAILURE(
        native->is_master_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER,
        native->is_master_error
    );
    TAKE_OPERATION_FAILURE(
        native->topology_read_call_count >= 1U &&
            native->topology_read_status[0] !=
                MEISO_K230D_DRM_NATIVE_READ_COMPLETE,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY,
        native->topology_read_call_count >= 1U
            ? native_read_effective_error(native, 0U) : 0
    );
    TAKE_OPERATION_FAILURE(
        native->create_dumb_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB,
        native->create_dumb_error
    );
    TAKE_OPERATION_FAILURE(
        native->add_framebuffer_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER,
        native->add_framebuffer_error
    );
    TAKE_OPERATION_FAILURE(
        native->topology_read_call_count >= 2U &&
            native->topology_read_status[1] !=
                MEISO_K230D_DRM_NATIVE_READ_COMPLETE,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY,
        native->topology_read_call_count >= 2U
            ? native_read_effective_error(native, 1U) : 0
    );
    TAKE_OPERATION_FAILURE(
        native->atomic_request_alloc_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY,
        native->atomic_request_alloc_error
    );
    TAKE_OPERATION_FAILURE(
        native->atomic_plane_property_add_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY,
        native->atomic_plane_property_add_error
    );
    TAKE_OPERATION_FAILURE(
        native->atomic_commit_error > 0 &&
            native->atomic_commit_error != EINVAL,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY,
        native->atomic_commit_error
    );
#undef TAKE_OPERATION_FAILURE

#define TAKE_CLEANUP_FAILURE(condition, stage_value, error_value) \
    do { \
        if (cleanup_stage == \
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE && \
            (condition)) { \
            cleanup_stage = (stage_value); \
            cleanup_error = (error_value); \
        } \
    } while (0)
    TAKE_CLEANUP_FAILURE(
        native->remove_framebuffer_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER,
        native->remove_framebuffer_error
    );
    TAKE_CLEANUP_FAILURE(
        native->destroy_dumb_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB,
        native->destroy_dumb_error
    );
    TAKE_CLEANUP_FAILURE(
        native->drop_master_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER,
        native->drop_master_error
    );
    TAKE_CLEANUP_FAILURE(
        native->close_error > 0,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE,
        native->close_error
    );
#undef TAKE_CLEANUP_FAILURE

    if (operation_stage !=
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE) {
        if (portable->operation_failure_stage != operation_stage ||
            portable->operation_error != operation_error) {
            return false;
        }
    }
    if (portable->cleanup_failure_stage != cleanup_stage ||
        portable->cleanup_error != cleanup_error) {
        return false;
    }
    if (native->topology_read_call_count == 3U &&
        native->topology_read_status[2] !=
            MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
        return portable->postcondition_failure_stage ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY &&
            portable->postcondition_error ==
                native_read_effective_error(native, 2U);
    }
    return true;
}

static bool native_result_contract(
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    uint32_t expected_same_fd_checks;
    uint32_t expected_add_calls;
    uint32_t native_successful_reads = 0U;
    uint32_t index;

    if (portable == NULL || native == NULL ||
        native->open_primary_call_count != 1U ||
        native->fstat_call_count > 1U ||
        native->primary_node_type_call_count > 1U ||
        native->open_failure_close_call_count > 1U ||
        native->topology_read_call_count !=
            portable->topology_read_attempt_count ||
        native->topology_read_call_count >
            MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT ||
        native->portable_atomic_callback_call_count !=
            portable->atomic_test_only_call_count ||
        native->map_call_count != 0U ||
        native->pixel_write_call_count != 0U ||
        native->connector_property_add_call_count != 0U ||
        native->crtc_property_add_call_count != 0U ||
        native->mode_blob_create_call_count != 0U ||
        native->legacy_modeset_call_count != 0U ||
        native->non_test_atomic_commit_call_count != 0U ||
        native->allow_modeset_flag_use_count != 0U ||
        native->page_flip_event_flag_use_count != 0U) {
        return false;
    }
    for (index = 0U;
         index < MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT;
         ++index) {
        if (native->topology_read_attempted[index] !=
                (index < native->topology_read_call_count) ||
            !native_read_slot_contract(native, index)) {
            return false;
        }
        if (index + 1U < native->topology_read_call_count &&
            native->topology_read_status[index] !=
                MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
            return false;
        }
        if (native->topology_read_attempted[index] &&
            native->topology_read_status[index] ==
                MEISO_K230D_DRM_NATIVE_READ_COMPLETE &&
            native->topology_read_ready_for_evaluation[index]) {
            ++native_successful_reads;
        }
    }
    if (native_successful_reads != portable->topology_read_success_count ||
        (portable->atomic_test_only_call_count == 1U &&
         native->topology_read_call_count != 3U)) {
        return false;
    }

    if (!portable->fd_was_opened) {
        if (native->primary_node_verified ||
            native->same_fd_contract_applicable ||
            native->same_fd_contract_verified ||
            !native_zero_after_open_failure(native) ||
            portable->operation_failure_stage !=
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY ||
            native->open_primary_error != portable->operation_error) {
            return false;
        }
        if (!native->native_fd_opened) {
            return native->fstat_call_count == 0U &&
                native->primary_node_type_call_count == 0U &&
                native->open_failure_close_call_count == 0U &&
                native->open_failure_close_error == 0 &&
                !native->open_failure_close_terminal_consumed;
        }
        return native->fstat_call_count == 1U &&
            native->open_failure_close_call_count == 1U &&
            native->open_failure_close_error >= 0 &&
            native->open_failure_close_terminal_consumed;
    }

    if (!native->native_fd_opened || !native->primary_node_verified ||
        native->open_primary_error != 0 || native->fstat_call_count != 1U ||
        native->primary_node_type_call_count != 1U ||
        native->open_failure_close_call_count != 0U ||
        native->open_failure_close_error != 0 ||
        native->open_failure_close_terminal_consumed ||
        !native->same_fd_contract_applicable ||
        !native->same_fd_contract_verified ||
        native->universal_planes_cap_call_count != 1U ||
        native->universal_planes_cap_set !=
            portable->universal_planes_enabled ||
        (native->universal_planes_cap_set
             ? native->universal_planes_cap_error != 0
             : native->universal_planes_cap_error <= 0) ||
        native->atomic_cap_call_count !=
            (portable->universal_planes_enabled ? 1U : 0U) ||
        native->atomic_cap_set != portable->atomic_enabled ||
        (native->atomic_cap_call_count == 0U
             ? native->atomic_cap_error != 0
             : (native->atomic_cap_set
                    ? native->atomic_cap_error != 0
                    : native->atomic_cap_error <= 0)) ||
        native->set_master_call_count !=
            (portable->atomic_enabled ? 1U : 0U) ||
        native->master_was_acquired != portable->master_was_acquired ||
        (native->set_master_call_count == 0U
             ? native->set_master_error != 0
             : (native->master_was_acquired
                    ? native->set_master_error != 0
                    : native->set_master_error <= 0)) ||
        native->is_master_call_count !=
            (portable->master_was_acquired ? 1U : 0U) ||
        native->master_verified != portable->master_verified ||
        (native->is_master_call_count == 0U
             ? native->is_master_error != 0
             : (native->master_verified
                    ? native->is_master_error != 0
                    : native->is_master_error <= 0))) {
        return false;
    }

    if (native->create_dumb_api_call_count !=
            (portable->initial_candidate_identified ? 1U : 0U)) {
        return false;
    }
    if ((native->create_dumb_api_call_count == 0U &&
         native->create_dumb_error != 0) ||
        (portable->dumb_was_created &&
         native->create_dumb_error != 0) ||
        (native->create_dumb_api_call_count == 1U &&
         !portable->dumb_was_created &&
         (native->create_dumb_error < 0 ||
          (native->create_dumb_error == 0 &&
           (portable->operation_failure_stage !=
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB ||
            portable->operation_error != EPROTO))))) {
        return false;
    }
    expected_add_calls = portable->dumb_was_created &&
        portable->operation_failure_stage !=
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB
        ? 1U : 0U;
    if (native->add_framebuffer_call_count != expected_add_calls) {
        return false;
    }
    if ((expected_add_calls == 0U && native->add_framebuffer_error != 0) ||
        (portable->framebuffer_was_created &&
         native->add_framebuffer_error != 0) ||
        (expected_add_calls == 1U && !portable->framebuffer_was_created &&
         (native->add_framebuffer_error < 0 ||
          (native->add_framebuffer_error == 0 &&
           (portable->operation_failure_stage !=
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER ||
            portable->operation_error != EPROTO))))) {
        return false;
    }

    if (native->portable_atomic_callback_call_count == 0U) {
        if (native->atomic_request_alloc_call_count != 0U ||
            native->atomic_request_alloc_succeeded ||
            native->atomic_request_alloc_error != 0 ||
            native->atomic_plane_property_add_attempt_count != 0U ||
            native->atomic_plane_property_add_success_count != 0U ||
            native->atomic_plane_property_add_error != 0 ||
            native->atomic_commit_api_call_count != 0U ||
            native->atomic_commit_flags != 0U ||
            native->atomic_commit_user_data_null ||
            native->atomic_commit_error != 0 ||
            native->atomic_request_free_call_count != 0U) {
            return false;
        }
    } else {
        int atomic_error;
        if (native->atomic_request_alloc_call_count != 1U) {
            return false;
        }
        if (!native->atomic_request_alloc_succeeded) {
            if (native->atomic_request_alloc_error <= 0 ||
                native->atomic_plane_property_add_attempt_count != 0U ||
                native->atomic_plane_property_add_success_count != 0U ||
                native->atomic_plane_property_add_error != 0 ||
                native->atomic_commit_api_call_count != 0U ||
                native->atomic_request_free_call_count != 0U) {
                return false;
            }
        } else if (native->atomic_request_alloc_error != 0 ||
                   native->atomic_request_free_call_count != 1U ||
                   native->atomic_plane_property_add_attempt_count == 0U ||
                   native->atomic_plane_property_add_attempt_count >
                       MEISO_K230D_DRM_PLANE_PROPERTY_COUNT ||
                   native->atomic_plane_property_add_success_count >
                       native->atomic_plane_property_add_attempt_count) {
            return false;
        } else if (native->atomic_plane_property_add_success_count !=
                   MEISO_K230D_DRM_PLANE_PROPERTY_COUNT) {
            if (native->atomic_plane_property_add_error <= 0 ||
                native->atomic_plane_property_add_success_count + 1U !=
                    native->atomic_plane_property_add_attempt_count ||
                native->atomic_commit_api_call_count != 0U) {
                return false;
            }
        } else if (native->atomic_plane_property_add_attempt_count !=
                       MEISO_K230D_DRM_PLANE_PROPERTY_COUNT ||
                   native->atomic_plane_property_add_error != 0 ||
                   native->atomic_commit_api_call_count != 1U ||
                   native->atomic_commit_flags !=
                       MEISO_K230D_DRM_NATIVE_TEST_ONLY_FLAG_VALUE ||
                   !native->atomic_commit_user_data_null ||
                   (portable->test_observation ==
                        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED
                        ? native->atomic_commit_error != 0
                        : (native->atomic_commit_error <= 0 ||
                           (portable->test_observation ==
                                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED &&
                            native->atomic_commit_error != EINVAL) ||
                           (portable->test_observation ==
                                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED &&
                            native->atomic_commit_error == EINVAL)))) {
            return false;
        }
        if (native->atomic_request_alloc_error > 0) {
            atomic_error = native->atomic_request_alloc_error;
        } else if (native->atomic_plane_property_add_error > 0) {
            atomic_error = native->atomic_plane_property_add_error;
        } else {
            atomic_error = native->atomic_commit_error;
        }
        if (portable->test_error != atomic_error ||
            ((portable->test_observation ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED ||
              portable->test_observation ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED) &&
             (native->atomic_request_alloc_call_count != 1U ||
              !native->atomic_request_alloc_succeeded ||
              native->atomic_plane_property_add_attempt_count !=
                  MEISO_K230D_DRM_PLANE_PROPERTY_COUNT ||
              native->atomic_plane_property_add_success_count !=
                  MEISO_K230D_DRM_PLANE_PROPERTY_COUNT ||
              native->atomic_commit_api_call_count != 1U)) ||
            (portable->test_observation ==
                 MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED &&
             atomic_error != EINVAL) ||
            (portable->test_observation ==
                 MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED &&
             (atomic_error <= 0 ||
              portable->operation_failure_stage !=
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY ||
              portable->operation_error != atomic_error))) {
            return false;
        }
    }
    if (native->atomic_commit_api_call_count == 0U &&
        (native->atomic_commit_flags != 0U ||
         native->atomic_commit_user_data_null ||
         native->atomic_commit_error != 0)) {
        return false;
    }

    if (native->remove_framebuffer_call_count !=
            (portable->remove_framebuffer_attempted ? 1U : 0U) ||
        native->destroy_dumb_api_call_count !=
            (portable->destroy_dumb_attempted ? 1U : 0U) ||
        native->drop_master_call_count !=
            (portable->drop_master_attempted ? 1U : 0U) ||
        native->close_call_count !=
            (portable->close_attempted ? 1U : 0U) ||
        native->close_terminal_consumed !=
            portable->fd_ownership_consumed ||
        (portable->remove_framebuffer_attempted
             ? (portable->remove_framebuffer_succeeded
                    ? native->remove_framebuffer_error != 0
                    : native->remove_framebuffer_error <= 0)
             : native->remove_framebuffer_error != 0) ||
        (portable->destroy_dumb_attempted
             ? (portable->destroy_dumb_succeeded
                    ? native->destroy_dumb_error != 0
                    : native->destroy_dumb_error <= 0)
             : native->destroy_dumb_error != 0) ||
        (portable->drop_master_attempted
             ? (portable->drop_master_succeeded
                    ? native->drop_master_error != 0
                    : native->drop_master_error <= 0)
             : native->drop_master_error != 0) ||
        (portable->close_attempted
             ? (portable->close_succeeded
                    ? native->close_error != 0
                    : native->close_error <= 0)
             : native->close_error != 0)) {
        return false;
    }

    expected_same_fd_checks =
        native->universal_planes_cap_call_count +
        native->atomic_cap_call_count + native->set_master_call_count +
        native->is_master_call_count + native->topology_read_call_count +
        native->create_dumb_api_call_count +
        native->add_framebuffer_call_count +
        native->portable_atomic_callback_call_count +
        native->remove_framebuffer_call_count +
        native->destroy_dumb_api_call_count +
        native->drop_master_call_count + native->close_call_count;
    if (native->same_fd_check_count != expected_same_fd_checks ||
        !native_operation_error_matches(portable, native) ||
        !native_cleanup_error_matches(portable, native) ||
        !native_candidate_read_cross_contract(portable, native) ||
        !native_first_failures_are_reported(portable, native)) {
        return false;
    }

    if (portable->operation_failure_stage ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY &&
        native_read_effective_error(native, 0U) != portable->operation_error) {
        return false;
    }
    if (portable->operation_failure_stage ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY &&
        native_read_effective_error(native, 1U) != portable->operation_error) {
        return false;
    }
    if (portable->postcondition_failure_stage ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY &&
        (native->topology_read_call_count < 3U ||
         native->topology_read_status[2] ==
             MEISO_K230D_DRM_NATIVE_READ_COMPLETE ||
         native_read_effective_error(native, 2U) !=
             portable->postcondition_error)) {
        return false;
    }
    return true;
}

static bool combined_result_contract(
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result
)
{
    return result_boolean_representations_valid(result) &&
        portable_result_contract(request, &result->portable) &&
        native_result_contract(&result->portable, &result->native);
}

static bool derive_unique_plane_result_contract(
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result,
    MeisoK230dDrmSinglePlaneTestCandidateRequest *out_pinned_request
)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest pinned_request;
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable;

    if (request == NULL || result == NULL || out_pinned_request == NULL ||
        !result_boolean_representations_valid(result)) {
        return false;
    }
    portable = &result->portable;
    if (portable->connector_id != request->connector_id ||
        portable->crtc_id != request->crtc_id ||
        portable->initial_candidate_identified !=
            (portable->plane_id != 0U) ||
        portable->initial_candidate_reason ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TEST_TARGET_IDENTITY_MISMATCH) {
        return false;
    }

    memset(&pinned_request, 0, sizeof(pinned_request));
    memcpy(
        pinned_request.expected_topology_sha256,
        request->expected_topology_sha256,
        sizeof(pinned_request.expected_topology_sha256)
    );
    pinned_request.connector_id = request->connector_id;
    pinned_request.crtc_id = request->crtc_id;
    pinned_request.plane_id = portable->plane_id;
    if (!combined_result_contract(&pinned_request, result)) {
        return false;
    }
    *out_pinned_request = pinned_request;
    return true;
}

static bool decode_lower_hex(
    const char *text,
    uint8_t *out_bytes,
    size_t byte_count
)
{
    size_t index;

    if (text == NULL || out_bytes == NULL ||
        strlen(text) != byte_count * 2U) {
        return false;
    }
    for (index = 0U; index < byte_count; ++index) {
        unsigned int high;
        unsigned int low;
        const unsigned char high_byte =
            (unsigned char)text[index * 2U];
        const unsigned char low_byte =
            (unsigned char)text[index * 2U + 1U];
        if (high_byte >= (unsigned char)'0' &&
            high_byte <= (unsigned char)'9') {
            high = (unsigned int)(high_byte - (unsigned char)'0');
        } else if (high_byte >= (unsigned char)'a' &&
                   high_byte <= (unsigned char)'f') {
            high = (unsigned int)(high_byte - (unsigned char)'a') + 10U;
        } else {
            return false;
        }
        if (low_byte >= (unsigned char)'0' &&
            low_byte <= (unsigned char)'9') {
            low = (unsigned int)(low_byte - (unsigned char)'0');
        } else if (low_byte >= (unsigned char)'a' &&
                   low_byte <= (unsigned char)'f') {
            low = (unsigned int)(low_byte - (unsigned char)'a') + 10U;
        } else {
            return false;
        }
        out_bytes[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static bool parse_canonical_u32(const char *text, uint32_t *out_value)
{
    uint64_t value = 0U;
    size_t index;

    if (text == NULL || out_value == NULL || text[0] == '\0' ||
        (text[0] == '0' && text[1] != '\0')) {
        return false;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        value = value * UINT64_C(10) +
            (uint64_t)(unsigned int)(text[index] - '0');
        if (value > UINT32_MAX) {
            return false;
        }
    }
    if (value == 0U) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool parse_arguments(
    int argument_count,
    char **arguments,
    const char **out_device_path,
    MeisoK230dDrmSinglePlaneTestCandidateRequest *out_request
)
{
    size_t path_size;
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;

    if (argument_count != 11 || arguments == NULL ||
        out_device_path == NULL || out_request == NULL ||
        arguments[0] == NULL || arguments[1] == NULL ||
        arguments[2] == NULL || arguments[3] == NULL ||
        arguments[4] == NULL || arguments[5] == NULL ||
        arguments[6] == NULL || arguments[7] == NULL ||
        arguments[8] == NULL || arguments[9] == NULL ||
        arguments[10] == NULL ||
        strcmp(arguments[1], "--device") != 0 ||
        strcmp(arguments[3], "--topology-sha256") != 0 ||
        strcmp(arguments[5], "--connector-id") != 0 ||
        strcmp(arguments[7], "--crtc-id") != 0 ||
        strcmp(arguments[9], "--plane-id") != 0 ||
        arguments[2][0] != '/' ||
        !meiso_k230d_drm_private_path_size(arguments[2], &path_size)) {
        return false;
    }
    (void)path_size;
    memset(&request, 0, sizeof(request));
    if (!decode_lower_hex(
            arguments[4], request.expected_topology_sha256,
            sizeof(request.expected_topology_sha256)
        ) || hash_is_zero(request.expected_topology_sha256) ||
        !parse_canonical_u32(arguments[6], &request.connector_id) ||
        !parse_canonical_u32(arguments[8], &request.crtc_id) ||
        !parse_canonical_u32(arguments[10], &request.plane_id)) {
        return false;
    }
    *out_device_path = arguments[2];
    memcpy(out_request, &request, sizeof(request));
    return true;
}

static bool parse_derive_unique_plane_arguments(
    int argument_count,
    char **arguments,
    const char **out_device_path,
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *out_request
)
{
    size_t path_size;
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest request;

    if (argument_count != 10 || arguments == NULL ||
        out_device_path == NULL || out_request == NULL ||
        arguments[0] == NULL || arguments[1] == NULL ||
        arguments[2] == NULL || arguments[3] == NULL ||
        arguments[4] == NULL || arguments[5] == NULL ||
        arguments[6] == NULL || arguments[7] == NULL ||
        arguments[8] == NULL || arguments[9] == NULL ||
        strcmp(arguments[1], "--derive-unique-plane") != 0 ||
        strcmp(arguments[2], "--device") != 0 ||
        strcmp(arguments[4], "--topology-sha256") != 0 ||
        strcmp(arguments[6], "--connector-id") != 0 ||
        strcmp(arguments[8], "--crtc-id") != 0 ||
        arguments[3][0] != '/' ||
        !meiso_k230d_drm_private_path_size(arguments[3], &path_size)) {
        return false;
    }
    (void)path_size;
    memset(&request, 0, sizeof(request));
    if (!decode_lower_hex(
            arguments[5], request.expected_topology_sha256,
            sizeof(request.expected_topology_sha256)
        ) || hash_is_zero(request.expected_topology_sha256) ||
        !parse_canonical_u32(arguments[7], &request.connector_id) ||
        !parse_canonical_u32(arguments[9], &request.crtc_id)) {
        return false;
    }
    *out_device_path = arguments[3];
    memcpy(out_request, &request, sizeof(request));
    return true;
}

static void receipt_append(ReceiptBuffer *buffer, const char *format, ...)
{
    va_list arguments;
    int written;

    if (buffer == NULL || format == NULL || !buffer->valid ||
        buffer->size >= sizeof(buffer->bytes)) {
        if (buffer != NULL) {
            buffer->valid = false;
        }
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(
        buffer->bytes + buffer->size,
        sizeof(buffer->bytes) - buffer->size,
        format,
        arguments
    );
    va_end(arguments);
    if (written < 0 ||
        (size_t)written >= sizeof(buffer->bytes) - buffer->size) {
        buffer->valid = false;
        return;
    }
    buffer->size += (size_t)written;
}

static void receipt_append_hex(
    ReceiptBuffer *buffer,
    const uint8_t *bytes,
    size_t size
)
{
    size_t index;

    receipt_append(buffer, "\"");
    for (index = 0U; index < size; ++index) {
        receipt_append(buffer, "%02x", (unsigned int)bytes[index]);
    }
    receipt_append(buffer, "\"");
}

static const char *boolean_token(bool value)
{
    return value ? "true" : "false";
}

static bool build_receipt_common(
    ReceiptBuffer *buffer,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result,
    bool derive_unique_plane
)
{
    const MeisoK230dDrmSinglePlaneTestOnlyResult *portable;
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native;
    uint32_t index;

    if (buffer == NULL || request == NULL || result == NULL ||
        !combined_result_contract(request, result)) {
        return false;
    }
    portable = &result->portable;
    native = &result->native;
    memset(buffer, 0, sizeof(*buffer));
    buffer->valid = true;
    if (derive_unique_plane) {
        receipt_append(
            buffer,
            "{\"type\":\"k230d_drm_single_plane_test_only_probe\","
            "\"schema\":\"meiso.k230d_drm_single_plane_test_only_probe.v2\","
            "\"verification\":\"NATIVE_LIBDRM_TEST_ONLY_ONLY\","
            "\"build_target\":\"k230d_linux\","
            "\"scope\":\"single_plane_test_only_no_pixels\","
            "\"selection_mode\":\"UNIQUE_POLICY_DERIVED\","
            "\"outcome\":\"%s\",\"primary_outcome\":\"%s\","
            "\"test_observation\":\"%s\","
            "\"requested_topology_sha256\":",
            outcome_token(portable->outcome),
            primary_outcome_token(portable->primary_outcome),
            test_observation_token(portable->test_observation)
        );
    } else {
        receipt_append(
            buffer,
            "{\"type\":\"k230d_drm_single_plane_test_only_probe\","
            "\"schema\":\"meiso.k230d_drm_single_plane_test_only_probe.v1\","
            "\"verification\":\"NATIVE_LIBDRM_TEST_ONLY_ONLY\","
            "\"build_target\":\"k230d_linux\","
            "\"scope\":\"single_plane_test_only_no_pixels\","
            "\"outcome\":\"%s\",\"primary_outcome\":\"%s\","
            "\"test_observation\":\"%s\","
            "\"requested_topology_sha256\":",
            outcome_token(portable->outcome),
            primary_outcome_token(portable->primary_outcome),
            test_observation_token(portable->test_observation)
        );
    }
    receipt_append_hex(
        buffer, request->expected_topology_sha256,
        sizeof(request->expected_topology_sha256)
    );
    if (derive_unique_plane) {
        receipt_append(
            buffer,
            ",\"requested_connector_id\":%u,\"requested_crtc_id\":%u,"
            "\"requested_plane_id\":0,\"derived_plane_id\":%u,"
            "\"initial_candidate_reason\":\"%s\","
            "\"pretest_candidate_reason\":\"%s\","
            "\"posttest_candidate_reason\":\"%s\","
            "\"initial_topology_sha256\":",
            request->connector_id, request->crtc_id, request->plane_id,
            candidate_reason_token(portable->initial_candidate_reason),
            candidate_reason_token(portable->pretest_candidate_reason),
            candidate_reason_token(portable->posttest_candidate_reason)
        );
    } else {
        receipt_append(
            buffer,
            ",\"requested_connector_id\":%u,\"requested_crtc_id\":%u,"
            "\"requested_plane_id\":%u,\"initial_candidate_reason\":\"%s\","
            "\"pretest_candidate_reason\":\"%s\","
            "\"posttest_candidate_reason\":\"%s\","
            "\"initial_topology_sha256\":",
            request->connector_id, request->crtc_id, request->plane_id,
            candidate_reason_token(portable->initial_candidate_reason),
            candidate_reason_token(portable->pretest_candidate_reason),
            candidate_reason_token(portable->posttest_candidate_reason)
        );
    }
    receipt_append_hex(
        buffer, portable->initial_topology_sha256,
        sizeof(portable->initial_topology_sha256)
    );
    receipt_append(buffer, ",\"pretest_topology_sha256\":");
    receipt_append_hex(
        buffer, portable->pretest_topology_sha256,
        sizeof(portable->pretest_topology_sha256)
    );
    receipt_append(buffer, ",\"posttest_topology_sha256\":");
    receipt_append_hex(
        buffer, portable->posttest_topology_sha256,
        sizeof(portable->posttest_topology_sha256)
    );
    receipt_append(
        buffer,
        ",\"topology_read_attempt_count\":%u,"
        "\"topology_read_success_count\":%u,"
        "\"atomic_test_only_call_count\":%u,"
        "\"initial_candidate_identified\":%s,"
        "\"pretest_candidate_identified\":%s,"
        "\"posttest_candidate_identified\":%s,"
        "\"pretest_topology_unchanged\":%s,"
        "\"posttest_topology_unchanged\":%s,"
        "\"test_only_attempted\":%s,"
        "\"test_callback_reported_accepted_not_applied\":%s,"
        "\"test_error\":%d,\"operation_failure_stage\":\"%s\","
        "\"operation_error\":%d,\"postcondition_failure_stage\":\"%s\","
        "\"postcondition_error\":%d,\"cleanup_failure_stage\":\"%s\","
        "\"cleanup_error\":%d,\"created_dumb_handle\":%u,"
        "\"created_dumb_pitch\":%u,\"created_dumb_size\":%" PRIu64 ","
        "\"created_framebuffer_id\":%u,\"minimum_output_bytes\":%" PRIu64 ",",
        portable->topology_read_attempt_count,
        portable->topology_read_success_count,
        portable->atomic_test_only_call_count,
        boolean_token(portable->initial_candidate_identified),
        boolean_token(portable->pretest_candidate_identified),
        boolean_token(portable->posttest_candidate_identified),
        boolean_token(portable->pretest_topology_unchanged),
        boolean_token(portable->posttest_topology_unchanged),
        boolean_token(portable->test_only_attempted),
        boolean_token(
            portable->test_callback_reported_accepted_not_applied
        ),
        portable->test_error,
        operation_stage_token(portable->operation_failure_stage),
        portable->operation_error,
        postcondition_stage_token(portable->postcondition_failure_stage),
        portable->postcondition_error,
        cleanup_stage_token(portable->cleanup_failure_stage),
        portable->cleanup_error,
        portable->created_dumb.handle,
        portable->created_dumb.pitch,
        portable->created_dumb.size,
        portable->created_framebuffer_id,
        portable->minimum_output_bytes
    );
    receipt_append(
        buffer,
        "\"fd_was_opened\":%s,\"universal_planes_enabled\":%s,"
        "\"atomic_enabled\":%s,\"master_was_acquired\":%s,"
        "\"master_verified\":%s,\"dumb_was_created\":%s,"
        "\"framebuffer_was_created\":%s,"
        "\"remove_framebuffer_attempted\":%s,"
        "\"remove_framebuffer_succeeded\":%s,"
        "\"destroy_dumb_attempted\":%s,"
        "\"destroy_dumb_succeeded\":%s,\"drop_master_attempted\":%s,"
        "\"drop_master_succeeded\":%s,\"close_attempted\":%s,"
        "\"close_succeeded\":%s,\"fd_ownership_consumed\":%s,"
        "\"master_ownership_consumed\":%s,"
        "\"dumb_ownership_consumed\":%s,"
        "\"framebuffer_ownership_consumed\":%s,",
        boolean_token(portable->fd_was_opened),
        boolean_token(portable->universal_planes_enabled),
        boolean_token(portable->atomic_enabled),
        boolean_token(portable->master_was_acquired),
        boolean_token(portable->master_verified),
        boolean_token(portable->dumb_was_created),
        boolean_token(portable->framebuffer_was_created),
        boolean_token(portable->remove_framebuffer_attempted),
        boolean_token(portable->remove_framebuffer_succeeded),
        boolean_token(portable->destroy_dumb_attempted),
        boolean_token(portable->destroy_dumb_succeeded),
        boolean_token(portable->drop_master_attempted),
        boolean_token(portable->drop_master_succeeded),
        boolean_token(portable->close_attempted),
        boolean_token(portable->close_succeeded),
        boolean_token(portable->fd_ownership_consumed),
        boolean_token(portable->master_ownership_consumed),
        boolean_token(portable->dumb_ownership_consumed),
        boolean_token(portable->framebuffer_ownership_consumed)
    );
    receipt_append(
        buffer,
        "\"native_open_primary_call_count\":%u,"
        "\"native_open_primary_error\":%d,\"native_fstat_call_count\":%u,"
        "\"native_primary_node_type_call_count\":%u,"
        "\"native_fd_opened\":%s,\"native_primary_node_verified\":%s,"
        "\"native_open_failure_close_call_count\":%u,"
        "\"native_open_failure_close_error\":%d,"
        "\"native_open_failure_close_terminal_consumed\":%s,"
        "\"native_same_fd_contract_applicable\":%s,"
        "\"native_same_fd_contract_verified\":%s,"
        "\"native_same_fd_check_count\":%u,"
        "\"native_universal_planes_cap_call_count\":%u,"
        "\"native_universal_planes_cap_error\":%d,"
        "\"native_universal_planes_cap_set\":%s,"
        "\"native_atomic_cap_call_count\":%u,"
        "\"native_atomic_cap_error\":%d,\"native_atomic_cap_set\":%s,"
        "\"native_set_master_call_count\":%u,"
        "\"native_set_master_error\":%d,\"native_is_master_call_count\":%u,"
        "\"native_is_master_error\":%d,\"native_master_was_acquired\":%s,"
        "\"native_master_verified\":%s,"
        "\"native_topology_read_call_count\":%u,",
        native->open_primary_call_count,
        native->open_primary_error,
        native->fstat_call_count,
        native->primary_node_type_call_count,
        boolean_token(native->native_fd_opened),
        boolean_token(native->primary_node_verified),
        native->open_failure_close_call_count,
        native->open_failure_close_error,
        boolean_token(native->open_failure_close_terminal_consumed),
        boolean_token(native->same_fd_contract_applicable),
        boolean_token(native->same_fd_contract_verified),
        native->same_fd_check_count,
        native->universal_planes_cap_call_count,
        native->universal_planes_cap_error,
        boolean_token(native->universal_planes_cap_set),
        native->atomic_cap_call_count,
        native->atomic_cap_error,
        boolean_token(native->atomic_cap_set),
        native->set_master_call_count,
        native->set_master_error,
        native->is_master_call_count,
        native->is_master_error,
        boolean_token(native->master_was_acquired),
        boolean_token(native->master_verified),
        native->topology_read_call_count
    );
    for (index = 0U;
         index < MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT;
         ++index) {
        receipt_append(
            buffer,
            "\"native_topology_read_%u_attempted\":%s,"
            "\"native_topology_read_%u_status\":\"%s\","
            "\"native_topology_read_%u_stage\":\"%s\","
            "\"native_topology_read_%u_preclassified_reason\":%u,"
            "\"native_topology_read_%u_error\":%d,"
            "\"native_topology_read_%u_ready_for_evaluation\":%s,"
            "\"native_topology_read_%u_topology_complete\":%s,"
            "\"native_topology_read_%u_all_plane_properties_complete\":%s,"
            "\"native_topology_read_%u_raw_connector_count\":%u,"
            "\"native_topology_read_%u_raw_crtc_count\":%u,"
            "\"native_topology_read_%u_raw_plane_count\":%u,"
            "\"native_topology_read_%u_objects_seen\":%u,"
            "\"native_topology_read_%u_objects_released\":%u,"
            "\"native_topology_read_%u_objects_all_released\":%s,",
            index + 1U,
            boolean_token(native->topology_read_attempted[index]),
            index + 1U,
            native_read_status_token(
                native->topology_read_attempted[index],
                native->topology_read_status[index]
            ),
            index + 1U,
            native->topology_read_attempted[index]
                ? native_stage_token(native->topology_read_stage[index])
                : "NOT_ATTEMPTED",
            index + 1U,
            (unsigned int)native->topology_read_preclassified_reason[index],
            index + 1U, native->topology_read_error[index],
            index + 1U,
            boolean_token(
                native->topology_read_ready_for_evaluation[index]
            ),
            index + 1U,
            boolean_token(native->topology_read_topology_complete[index]),
            index + 1U,
            boolean_token(
                native->topology_read_all_plane_properties_complete[index]
            ),
            index + 1U,
            native->topology_read_raw_connector_count[index],
            index + 1U,
            native->topology_read_raw_crtc_count[index],
            index + 1U,
            native->topology_read_raw_plane_count[index],
            index + 1U, native->topology_read_objects_seen[index],
            index + 1U, native->topology_read_objects_released[index],
            index + 1U,
            boolean_token(
                native->topology_read_objects_all_released[index]
            )
        );
    }
    receipt_append(
        buffer,
        "\"native_create_dumb_api_call_count\":%u,"
        "\"native_create_dumb_error\":%d,"
        "\"native_add_framebuffer_call_count\":%u,"
        "\"native_add_framebuffer_error\":%d,"
        "\"native_portable_atomic_callback_call_count\":%u,"
        "\"native_atomic_request_alloc_call_count\":%u,"
        "\"native_atomic_request_alloc_succeeded\":%s,"
        "\"native_atomic_request_alloc_error\":%d,"
        "\"native_atomic_plane_property_add_attempt_count\":%u,"
        "\"native_atomic_plane_property_add_success_count\":%u,"
        "\"native_atomic_plane_property_add_error\":%d,"
        "\"native_atomic_commit_api_call_count\":%u,"
        "\"native_atomic_commit_flags\":%u,"
        "\"native_atomic_commit_user_data_null\":%s,"
        "\"native_atomic_commit_error\":%d,"
        "\"native_atomic_request_free_call_count\":%u,"
        "\"native_remove_framebuffer_call_count\":%u,"
        "\"native_remove_framebuffer_error\":%d,"
        "\"native_destroy_dumb_api_call_count\":%u,"
        "\"native_destroy_dumb_error\":%d,"
        "\"native_drop_master_call_count\":%u,"
        "\"native_drop_master_error\":%d,"
        "\"native_close_call_count\":%u,\"native_close_error\":%d,"
        "\"native_close_terminal_consumed\":%s,"
        "\"native_map_call_count\":%u,\"native_pixel_write_call_count\":%u,"
        "\"native_connector_property_add_call_count\":%u,"
        "\"native_crtc_property_add_call_count\":%u,"
        "\"native_mode_blob_create_call_count\":%u,"
        "\"native_legacy_modeset_call_count\":%u,"
        "\"native_non_test_atomic_commit_call_count\":%u,"
        "\"native_allow_modeset_flag_use_count\":%u,"
        "\"native_page_flip_event_flag_use_count\":%u,"
        "\"pixel_payload_present\":false,\"framebuffer_mapped\":false,"
        "\"presented\":false,"
        "\"visibility_exclusivity_unproven\":true,"
        "\"real_commit_attempted\":false,"
        "\"allow_modeset_attempted\":false,"
        "\"page_flip_event_requested\":false,"
        "\"display_state_unchanged_unproven\":true,"
        "\"display_actual_authorized\":false,\"physical_evidence\":false}\n",
        native->create_dumb_api_call_count,
        native->create_dumb_error,
        native->add_framebuffer_call_count,
        native->add_framebuffer_error,
        native->portable_atomic_callback_call_count,
        native->atomic_request_alloc_call_count,
        boolean_token(native->atomic_request_alloc_succeeded),
        native->atomic_request_alloc_error,
        native->atomic_plane_property_add_attempt_count,
        native->atomic_plane_property_add_success_count,
        native->atomic_plane_property_add_error,
        native->atomic_commit_api_call_count,
        native->atomic_commit_flags,
        boolean_token(native->atomic_commit_user_data_null),
        native->atomic_commit_error,
        native->atomic_request_free_call_count,
        native->remove_framebuffer_call_count,
        native->remove_framebuffer_error,
        native->destroy_dumb_api_call_count,
        native->destroy_dumb_error,
        native->drop_master_call_count,
        native->drop_master_error,
        native->close_call_count,
        native->close_error,
        boolean_token(native->close_terminal_consumed),
        native->map_call_count,
        native->pixel_write_call_count,
        native->connector_property_add_call_count,
        native->crtc_property_add_call_count,
        native->mode_blob_create_call_count,
        native->legacy_modeset_call_count,
        native->non_test_atomic_commit_call_count,
        native->allow_modeset_flag_use_count,
        native->page_flip_event_flag_use_count
    );
    return buffer->valid && buffer->size > 0U &&
        buffer->size < sizeof(buffer->bytes) &&
        buffer->bytes[buffer->size - 1U] == '\n';
}

static bool build_receipt(
    ReceiptBuffer *buffer,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result
)
{
    return build_receipt_common(buffer, request, result, false);
}

static bool build_derive_unique_plane_receipt(
    ReceiptBuffer *buffer,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    const MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *result
)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest pinned_request;

    if (!derive_unique_plane_result_contract(
            request, result, &pinned_request
        )) {
        return false;
    }
    return build_receipt_common(buffer, &pinned_request, result, true);
}

static void write_diagnostic(FILE *stream, const char *text)
{
    if (stream != NULL && text != NULL) {
        (void)fputs(text, stream);
        (void)fflush(stream);
    }
}

int meiso_k230d_drm_single_plane_test_only_once_run(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeSinglePlaneTestOnlyExecuteFn execute
)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;
    MeisoK230dDrmSinglePlaneTestCandidateRequest execute_request;
    uint8_t execute_request_before[sizeof(execute_request)];
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult result;
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus status;
    ReceiptBuffer receipt;
    const char *device_path = NULL;
    int return_code;

    if (out_stream == NULL || error_stream == NULL ||
        out_stream == error_stream) {
        return 2;
    }
    if (execute == NULL || !parse_arguments(
            argument_count, arguments, &device_path, &request
        )) {
        write_diagnostic(error_stream, "E_ARGUMENT\n");
        return 2;
    }
    memset(&result, 0, sizeof(result));
    memcpy(&execute_request, &request, sizeof(execute_request));
    memcpy(
        execute_request_before, &execute_request,
        sizeof(execute_request_before)
    );
    status = execute(device_path, &execute_request, &result);
    if (status !=
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE ||
        memcmp(
            &execute_request, execute_request_before,
            sizeof(execute_request)
        ) != 0 ||
        !combined_result_contract(&request, &result) ||
        !build_receipt(&receipt, &request, &result)) {
        write_diagnostic(error_stream, "E_PROVIDER_CONTRACT\n");
        return 1;
    }
    if (fwrite(receipt.bytes, 1U, receipt.size, out_stream) != receipt.size ||
        fflush(out_stream) != 0) {
        return 1;
    }
    return_code =
        result.portable.outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE ||
            result.portable.outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE
        ? 1 : 0;
    return return_code;
}

int meiso_k230d_drm_single_plane_test_only_once_derive_unique_plane_run(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeSinglePlaneTestOnlyDeriveUniquePlaneExecuteFn execute
)
{
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest request;
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest execute_request;
    uint8_t execute_request_before[sizeof(execute_request)];
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult result;
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus status;
    MeisoK230dDrmSinglePlaneTestCandidateRequest pinned_request;
    ReceiptBuffer receipt;
    const char *device_path = NULL;
    int return_code;

    if (out_stream == NULL || error_stream == NULL ||
        out_stream == error_stream) {
        return 2;
    }
    if (execute == NULL || !parse_derive_unique_plane_arguments(
            argument_count, arguments, &device_path, &request
        )) {
        write_diagnostic(error_stream, "E_ARGUMENT\n");
        return 2;
    }
    memset(&result, 0, sizeof(result));
    memcpy(&execute_request, &request, sizeof(execute_request));
    memcpy(
        execute_request_before, &execute_request,
        sizeof(execute_request_before)
    );
    status = execute(device_path, &execute_request, &result);
    if (status !=
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE ||
        memcmp(
            &execute_request, execute_request_before,
            sizeof(execute_request)
        ) != 0 ||
        !derive_unique_plane_result_contract(
            &request, &result, &pinned_request
        ) ||
        !build_derive_unique_plane_receipt(&receipt, &request, &result)) {
        write_diagnostic(error_stream, "E_PROVIDER_CONTRACT\n");
        return 1;
    }
    if (fwrite(receipt.bytes, 1U, receipt.size, out_stream) != receipt.size ||
        fflush(out_stream) != 0) {
        return 1;
    }
    return_code =
        result.portable.outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE ||
            result.portable.outcome ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE
        ? 1 : 0;
    return return_code;
}

#if !defined(MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_ONCE_NO_MAIN)
int main(int argument_count, char **arguments)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGPIPE, &action, NULL) != 0) {
        return 1;
    }
    if (argument_count > 1 && arguments != NULL &&
        arguments[1] != NULL &&
        strcmp(arguments[1], "--derive-unique-plane") == 0) {
        return
            meiso_k230d_drm_single_plane_test_only_once_derive_unique_plane_run(
                stdout, stderr, argument_count, arguments,
                meiso_k230d_drm_libdrm_run_single_plane_test_only_derive_unique_plane
            );
    }
    return meiso_k230d_drm_single_plane_test_only_once_run(
        stdout, stderr, argument_count, arguments,
        meiso_k230d_drm_libdrm_run_single_plane_test_only
    );
}
#endif
