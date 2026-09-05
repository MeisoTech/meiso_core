#include "meiso_drm_single_plane_test_only_private.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlyOps ops;
    void *ops_context;
    char device_path[MEISO_K230D_DRM_DEVICE_PATH_CAPACITY];
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    MeisoK230dDrmTopology initial_topology;
    MeisoK230dDrmTopology pretest_topology;
    MeisoK230dDrmTopology posttest_topology;
    MeisoK230dDrmSinglePlaneTestCandidate initial_candidate;
    MeisoK230dDrmSinglePlaneTestCandidate pretest_candidate;
    MeisoK230dDrmSinglePlaneTestCandidate posttest_candidate;
    int fd;
    bool fd_owned;
    bool master_owned;
    bool dumb_owned;
    bool framebuffer_owned;
} SinglePlaneTestExecution;

static bool ops_valid(const MeisoK230dDrmSinglePlaneTestOnlyOps *ops)
{
    return ops != NULL && ops->open_primary != NULL &&
        ops->enable_universal_planes != NULL &&
        ops->enable_atomic != NULL && ops->acquire_master != NULL &&
        ops->verify_master != NULL && ops->read_topology != NULL &&
        ops->create_rgb565_dumb != NULL &&
        ops->add_rgb565_framebuffer != NULL &&
        ops->atomic_test_only != NULL &&
        ops->remove_framebuffer != NULL && ops->destroy_dumb != NULL &&
        ops->drop_master != NULL && ops->close_device != NULL;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    return meiso_k230d_drm_private_ranges_overlap(
        lhs, lhs_size, rhs, rhs_size
    );
}

static bool known_config_ranges_overlap(
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyResult *out_result,
    size_t path_bytes
)
{
    const void *context = config->ops_context;
    const void *objects[] = {
        config, request, out_result, config->ops, config->device_path
    };
    const size_t sizes[] = {
        sizeof(*config), sizeof(*request), sizeof(*out_result),
        sizeof(*config->ops), path_bytes
    };
    size_t lhs;
    size_t rhs;

    for (lhs = 0U; lhs < sizeof(objects) / sizeof(objects[0]); ++lhs) {
        for (rhs = lhs + 1U;
             rhs < sizeof(objects) / sizeof(objects[0]); ++rhs) {
            if (ranges_overlap(objects[lhs], sizes[lhs],
                               objects[rhs], sizes[rhs])) {
                return true;
            }
        }
        if (ranges_overlap(context, 1U, objects[lhs], sizes[lhs])) {
            return true;
        }
    }
    return false;
}

static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus validate_invocation(
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyResult *out_result,
    size_t *out_path_bytes
)
{
    size_t path_size;

    if (config == NULL || request == NULL || out_result == NULL) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT;
    }
    if (ranges_overlap(config, sizeof(*config), request, sizeof(*request)) ||
        ranges_overlap(config, sizeof(*config), out_result,
                       sizeof(*out_result)) ||
        ranges_overlap(request, sizeof(*request), out_result,
                       sizeof(*out_result)) ||
        request->connector_id == 0U || request->crtc_id == 0U ||
        request->plane_id == 0U) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT;
    }
    if (config->device_path == NULL || config->ops == NULL ||
        config->ops_context == NULL) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG;
    }
    /* Reject known fixed-range aliases before scanning path or reading Ops. */
    if (ranges_overlap(config->ops, sizeof(*config->ops),
                       config, sizeof(*config)) ||
        ranges_overlap(config->ops, sizeof(*config->ops),
                       request, sizeof(*request)) ||
        ranges_overlap(config->ops, sizeof(*config->ops),
                       out_result, sizeof(*out_result)) ||
        ranges_overlap(config->device_path, 1U,
                       config, sizeof(*config)) ||
        ranges_overlap(config->device_path, 1U,
                       request, sizeof(*request)) ||
        ranges_overlap(config->device_path, 1U,
                       out_result, sizeof(*out_result)) ||
        ranges_overlap(config->device_path, 1U,
                       config->ops, sizeof(*config->ops)) ||
        ranges_overlap(config->device_path, 1U,
                       config->ops_context, 1U) ||
        ranges_overlap(config->ops_context, 1U,
                       config, sizeof(*config)) ||
        ranges_overlap(config->ops_context, 1U,
                       request, sizeof(*request)) ||
        ranges_overlap(config->ops_context, 1U,
                       out_result, sizeof(*out_result)) ||
        ranges_overlap(config->ops_context, 1U,
                       config->ops, sizeof(*config->ops))) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG;
    }
    if (!meiso_k230d_drm_private_path_size(
            config->device_path, &path_size
        )) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG;
    }
    ++path_size;
    if (known_config_ranges_overlap(
            config, request, out_result, path_size
        ) || !ops_valid(config->ops)) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG;
    }
    *out_path_bytes = path_size;
    return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE;
}

static int normalized_callback_error(int callback_result)
{
    return callback_result < 0 ? EPROTO : callback_result;
}

static void record_operation_failure(
    SinglePlaneTestExecution *execution,
    MeisoK230dDrmSinglePlaneTestOnlyOperationStage stage,
    int error
)
{
    if (execution->result.operation_failure_stage ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE) {
        execution->result.operation_failure_stage = stage;
        execution->result.operation_error = error;
    }
    execution->result.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
    execution->result.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
}

static void record_postcondition_failure(
    SinglePlaneTestExecution *execution,
    MeisoK230dDrmSinglePlaneTestOnlyPostconditionStage stage,
    int error
)
{
    if (execution->result.postcondition_failure_stage ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE) {
        execution->result.postcondition_failure_stage = stage;
        execution->result.postcondition_error = error;
    }
    execution->result.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
}

static void record_cleanup_failure(
    SinglePlaneTestExecution *execution,
    MeisoK230dDrmSinglePlaneTestOnlyCleanupStage stage,
    int error
)
{
    if (execution->result.cleanup_failure_stage ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE) {
        execution->result.cleanup_failure_stage = stage;
        execution->result.cleanup_error = error;
    }
    execution->result.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE;
}

static int call_open(SinglePlaneTestExecution *execution, int *out_fd)
{
    int candidate = -1;
    int callback_result = execution->ops.open_primary(
        execution->ops_context, execution->device_path, &candidate
    );
    if (callback_result != 0) {
        if (candidate != -1) {
            return EPROTO;
        }
        return normalized_callback_error(callback_result);
    }
    if (candidate < 0) {
        return EPROTO;
    }
    *out_fd = candidate;
    return 0;
}

static int call_simple(
    int (*callback)(void *context, int fd),
    SinglePlaneTestExecution *execution
)
{
    return normalized_callback_error(
        callback(execution->ops_context, execution->fd)
    );
}

static int call_read_topology(
    SinglePlaneTestExecution *execution,
    MeisoK230dDrmTopology *out_topology
)
{
    MeisoK230dDrmTopology candidate;
    MeisoK230dDrmTopology safe_zero;
    int callback_result;

    memset(&candidate, 0, sizeof(candidate));
    memset(&safe_zero, 0, sizeof(safe_zero));
    ++execution->result.topology_read_attempt_count;
    callback_result = execution->ops.read_topology(
        execution->ops_context, execution->fd, &candidate
    );
    if (callback_result != 0) {
        if (memcmp(&candidate, &safe_zero, sizeof(candidate)) != 0) {
            return EPROTO;
        }
        return normalized_callback_error(callback_result);
    }
    ++execution->result.topology_read_success_count;
    *out_topology = candidate;
    return 0;
}

static int call_create_dumb(
    SinglePlaneTestExecution *execution,
    uint32_t width,
    uint32_t height,
    MeisoK230dDrmDumbBuffer *out_dumb
)
{
    MeisoK230dDrmDumbBuffer candidate;
    MeisoK230dDrmDumbBuffer safe_zero;
    int callback_result;

    memset(&candidate, 0, sizeof(candidate));
    memset(&safe_zero, 0, sizeof(safe_zero));
    callback_result = execution->ops.create_rgb565_dumb(
        execution->ops_context, execution->fd, width, height, &candidate
    );
    if (callback_result != 0) {
        if (memcmp(&candidate, &safe_zero, sizeof(candidate)) != 0) {
            return EPROTO;
        }
        return normalized_callback_error(callback_result);
    }
    if (candidate.handle == 0U) {
        return EPROTO;
    }
    execution->dumb_owned = true;
    execution->result.dumb_was_created = true;
    execution->result.created_dumb.handle = candidate.handle;
    execution->result.created_dumb.pitch = candidate.pitch;
    execution->result.created_dumb.size = candidate.size;
    *out_dumb = candidate;
    return 0;
}

static int call_add_framebuffer(
    SinglePlaneTestExecution *execution,
    uint32_t width,
    uint32_t height,
    const MeisoK230dDrmDumbBuffer *dumb,
    uint32_t *out_framebuffer_id
)
{
    uint32_t candidate = 0U;
    int callback_result = execution->ops.add_rgb565_framebuffer(
        execution->ops_context,
        execution->fd,
        width,
        height,
        dumb->handle,
        dumb->pitch,
        &candidate
    );
    if (callback_result != 0) {
        if (candidate != 0U) {
            return EPROTO;
        }
        return normalized_callback_error(callback_result);
    }
    if (candidate == 0U) {
        return EPROTO;
    }
    execution->framebuffer_owned = true;
    execution->result.framebuffer_was_created = true;
    execution->result.created_framebuffer_id = candidate;
    *out_framebuffer_id = candidate;
    return 0;
}

static MeisoK230dDrmSinglePlaneTestOnlyTestObservation call_test_only(
    SinglePlaneTestExecution *execution,
    const MeisoK230dDrmAtomicPlaneState *state,
    int *out_error
)
{
    MeisoK230dDrmSinglePlaneTestOnlyCallDisposition disposition;
    MeisoK230dDrmAtomicPlaneState state_before;
    int error = INT_MIN;

    ++execution->result.atomic_test_only_call_count;
    execution->result.test_only_attempted = true;
    memcpy(&state_before, state, sizeof(state_before));
    disposition = execution->ops.atomic_test_only(
        execution->ops_context, execution->fd, state, &error
    );
    if (memcmp(state, &state_before, sizeof(state_before)) != 0 ||
        disposition >
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED ||
        (disposition ==
             MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED &&
         error != 0) ||
        (disposition !=
             MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED &&
         error <= 0)) {
        *out_error = EPROTO;
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED;
    }
    *out_error = error;
    if (disposition ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED;
    }
    if (disposition ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED;
    }
    return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED;
}

static int cleanup_call_result(int callback_result)
{
    return callback_result < 0 ? EPROTO : callback_result;
}

static void cleanup_execution(SinglePlaneTestExecution *execution)
{
    int error;

    if (execution->framebuffer_owned) {
        execution->result.remove_framebuffer_attempted = true;
        error = cleanup_call_result(execution->ops.remove_framebuffer(
            execution->ops_context,
            execution->fd,
            execution->result.created_framebuffer_id
        ));
        if (error == 0) {
            execution->framebuffer_owned = false;
            execution->result.remove_framebuffer_succeeded = true;
            execution->result.framebuffer_ownership_consumed = true;
        } else {
            record_cleanup_failure(
                execution,
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER,
                error
            );
        }
    }
    if (execution->dumb_owned) {
        execution->result.destroy_dumb_attempted = true;
        error = cleanup_call_result(execution->ops.destroy_dumb(
            execution->ops_context,
            execution->fd,
            execution->result.created_dumb.handle
        ));
        if (error == 0) {
            execution->dumb_owned = false;
            execution->result.destroy_dumb_succeeded = true;
            execution->result.dumb_ownership_consumed = true;
        } else {
            record_cleanup_failure(
                execution,
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB,
                error
            );
        }
    }
    if (execution->master_owned) {
        execution->result.drop_master_attempted = true;
        error = cleanup_call_result(execution->ops.drop_master(
            execution->ops_context, execution->fd
        ));
        if (error == 0) {
            execution->master_owned = false;
            execution->result.drop_master_succeeded = true;
            execution->result.master_ownership_consumed = true;
        } else {
            record_cleanup_failure(
                execution,
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER,
                error
            );
        }
    }
    if (execution->fd_owned) {
        execution->result.close_attempted = true;
        error = cleanup_call_result(execution->ops.close_device(
            execution->ops_context, execution->fd
        ));
        execution->fd_owned = false;
        execution->master_owned = false;
        execution->dumb_owned = false;
        execution->framebuffer_owned = false;
        execution->result.fd_ownership_consumed = true;
        if (execution->result.master_was_acquired) {
            execution->result.master_ownership_consumed = true;
        }
        if (execution->result.dumb_was_created) {
            execution->result.dumb_ownership_consumed = true;
        }
        if (execution->result.framebuffer_was_created) {
            execution->result.framebuffer_ownership_consumed = true;
        }
        if (error == 0) {
            execution->result.close_succeeded = true;
        } else {
            record_cleanup_failure(
                execution,
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE,
                error
            );
        }
    }
}

static bool candidate_same(
    const MeisoK230dDrmSinglePlaneTestCandidate *lhs,
    const MeisoK230dDrmSinglePlaneTestCandidate *rhs
)
{
    return lhs->topology_hash_valid == rhs->topology_hash_valid &&
        lhs->topology_identity_matched == rhs->topology_identity_matched &&
        lhs->route_selected == rhs->route_selected &&
        lhs->enumerated_planes_inactive == rhs->enumerated_planes_inactive &&
        lhs->test_target_selected == rhs->test_target_selected &&
        lhs->reason == rhs->reason &&
        memcmp(lhs->topology_sha256, rhs->topology_sha256,
               sizeof(lhs->topology_sha256)) == 0 &&
        lhs->connector_index == rhs->connector_index &&
        lhs->crtc_index == rhs->crtc_index &&
        lhs->plane_index == rhs->plane_index &&
        lhs->test_target_count == rhs->test_target_count &&
        lhs->minimum_output_bytes == rhs->minimum_output_bytes;
}

static void copy_candidate_observation(
    MeisoK230dDrmSinglePlaneTestCandidateReason *out_reason,
    uint8_t out_sha256[MEISO_K230D_DRM_SHA256_SIZE],
    const MeisoK230dDrmSinglePlaneTestCandidate *candidate
)
{
    *out_reason = candidate->reason;
    memcpy(out_sha256, candidate->topology_sha256,
           MEISO_K230D_DRM_SHA256_SIZE);
}

static MeisoK230dDrmSinglePlaneTestCandidateStatus evaluate_candidate(
    SinglePlaneTestExecution *execution,
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmSinglePlaneTestCandidate *out_candidate
)
{
    memset(out_candidate, 0, sizeof(*out_candidate));
    return meiso_k230d_drm_private_evaluate_single_plane_test_candidate(
        topology, &execution->request, out_candidate
    );
}

static void construct_plane_state(
    const MeisoK230dDrmTopology *topology,
    const MeisoK230dDrmSinglePlaneTestCandidate *candidate,
    uint32_t framebuffer_id,
    MeisoK230dDrmAtomicPlaneState *out_state
)
{
    const MeisoK230dDrmCrtc *crtc = &topology->crtcs[
        candidate->crtc_index
    ];
    const MeisoK230dDrmPlane *plane = &topology->planes[
        candidate->plane_index
    ];
    uint32_t index;

    memset(out_state, 0, sizeof(*out_state));
    out_state->plane_id = plane->plane_id;
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        out_state->properties[index].property_id =
            plane->properties[index].property_id;
    }
    out_state->properties[MEISO_K230D_DRM_PLANE_FB_ID].value =
        framebuffer_id;
    out_state->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value =
        crtc->crtc_id;
    out_state->properties[MEISO_K230D_DRM_PLANE_SRC_X].value = 0U;
    out_state->properties[MEISO_K230D_DRM_PLANE_SRC_Y].value = 0U;
    out_state->properties[MEISO_K230D_DRM_PLANE_SRC_W].value =
        (uint64_t)crtc->mode.hdisplay << 16U;
    out_state->properties[MEISO_K230D_DRM_PLANE_SRC_H].value =
        (uint64_t)crtc->mode.vdisplay << 16U;
    out_state->properties[MEISO_K230D_DRM_PLANE_CRTC_X].value = 0U;
    out_state->properties[MEISO_K230D_DRM_PLANE_CRTC_Y].value = 0U;
    out_state->properties[MEISO_K230D_DRM_PLANE_CRTC_W].value =
        crtc->mode.hdisplay;
    out_state->properties[MEISO_K230D_DRM_PLANE_CRTC_H].value =
        crtc->mode.vdisplay;
}

static bool dumb_contract_valid(
    const MeisoK230dDrmDumbBuffer *dumb,
    uint32_t width,
    uint32_t height
)
{
    const uint64_t minimum_pitch = (uint64_t)width * UINT64_C(2);
    uint64_t minimum_size;

    if (dumb->handle == 0U || (uint64_t)dumb->pitch < minimum_pitch) {
        return false;
    }
    minimum_size = (uint64_t)dumb->pitch * height;
    return dumb->size >= minimum_size &&
        dumb->size <= MEISO_K230D_DRM_MAX_DUMB_BYTES;
}

MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_k230d_drm_private_run_single_plane_test_only(
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyResult *out_result
)
{
    SinglePlaneTestExecution execution;
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus invoke_status;
    MeisoK230dDrmSinglePlaneTestCandidateStatus candidate_status;
    MeisoK230dDrmAtomicPlaneState state;
    MeisoK230dDrmDumbBuffer dumb;
    const MeisoK230dDrmMode *mode;
    uint32_t framebuffer_id = 0U;
    size_t path_bytes = 0U;
    int error;

    invoke_status = validate_invocation(
        config, request, out_result, &path_bytes
    );
    if (invoke_status !=
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE) {
        return invoke_status;
    }

    memset(&execution, 0, sizeof(execution));
    execution.fd = -1;
    execution.ops = *config->ops;
    execution.ops_context = config->ops_context;
    memcpy(execution.device_path, config->device_path, path_bytes);
    execution.request = *request;
    execution.result.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED;
    execution.result.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED;
    execution.result.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED;
    execution.result.connector_id = request->connector_id;
    execution.result.crtc_id = request->crtc_id;
    execution.result.plane_id = request->plane_id;

    error = call_open(&execution, &execution.fd);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY,
            error
        );
        goto done;
    }
    execution.fd_owned = true;
    execution.result.fd_was_opened = true;

    error = call_simple(execution.ops.enable_universal_planes, &execution);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES,
            error
        );
        goto done;
    }
    execution.result.universal_planes_enabled = true;

    error = call_simple(execution.ops.enable_atomic, &execution);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC,
            error
        );
        goto done;
    }
    execution.result.atomic_enabled = true;

    error = call_simple(execution.ops.acquire_master, &execution);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER,
            error
        );
        goto done;
    }
    execution.master_owned = true;
    execution.result.master_was_acquired = true;

    error = call_simple(execution.ops.verify_master, &execution);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER,
            error
        );
        goto done;
    }
    execution.result.master_verified = true;

    error = call_read_topology(&execution, &execution.initial_topology);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY,
            error
        );
        goto done;
    }
    candidate_status = evaluate_candidate(
        &execution, &execution.initial_topology,
        &execution.initial_candidate
    );
    copy_candidate_observation(
        &execution.result.initial_candidate_reason,
        execution.result.initial_topology_sha256,
        &execution.initial_candidate
    );
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_INVALID_ARGUMENT) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE,
            EPROTO
        );
        goto done;
    }
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_BLOCKED) {
        goto done;
    }
    execution.result.initial_candidate_identified = true;
    execution.result.minimum_output_bytes =
        execution.initial_candidate.minimum_output_bytes;
    mode = &execution.initial_topology.crtcs[
        execution.initial_candidate.crtc_index
    ].mode;

    memset(&dumb, 0, sizeof(dumb));
    error = call_create_dumb(
        &execution, mode->hdisplay, mode->vdisplay, &dumb
    );
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB,
            error
        );
        goto done;
    }
    if (!dumb_contract_valid(&dumb, mode->hdisplay, mode->vdisplay)) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB,
            EPROTO
        );
        goto done;
    }

    error = call_add_framebuffer(
        &execution, mode->hdisplay, mode->vdisplay, &dumb,
        &framebuffer_id
    );
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER,
            error
        );
        goto done;
    }

    error = call_read_topology(&execution, &execution.pretest_topology);
    if (error != 0) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY,
            error
        );
        goto done;
    }
    candidate_status = evaluate_candidate(
        &execution, &execution.pretest_topology,
        &execution.pretest_candidate
    );
    copy_candidate_observation(
        &execution.result.pretest_candidate_reason,
        execution.result.pretest_topology_sha256,
        &execution.pretest_candidate
    );
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_INVALID_ARGUMENT) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE,
            EPROTO
        );
        goto done;
    }
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_BLOCKED) {
        goto done;
    }
    execution.result.pretest_candidate_identified = true;
    if (!candidate_same(
            &execution.initial_candidate, &execution.pretest_candidate
        )) {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE,
            EPROTO
        );
        goto done;
    }
    execution.result.pretest_topology_unchanged = true;

    construct_plane_state(
        &execution.pretest_topology,
        &execution.pretest_candidate,
        framebuffer_id,
        &state
    );
    execution.result.test_observation = call_test_only(
        &execution, &state, &execution.result.test_error
    );
    if (execution.result.test_observation ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED) {
        execution.result.primary_outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED;
        execution.result.outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED;
        execution.result.test_callback_reported_accepted_not_applied = true;
    } else if (execution.result.test_observation ==
               MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED) {
        execution.result.primary_outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED;
        execution.result.outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED;
    } else {
        record_operation_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY,
            execution.result.test_error
        );
    }

    error = call_read_topology(&execution, &execution.posttest_topology);
    if (error != 0) {
        record_postcondition_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY,
            error
        );
        goto done;
    }
    candidate_status = evaluate_candidate(
        &execution, &execution.posttest_topology,
        &execution.posttest_candidate
    );
    copy_candidate_observation(
        &execution.result.posttest_candidate_reason,
        execution.result.posttest_topology_sha256,
        &execution.posttest_candidate
    );
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_INVALID_ARGUMENT) {
        record_postcondition_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_EVALUATE_CANDIDATE,
            EPROTO
        );
        goto done;
    }
    if (candidate_status ==
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_BLOCKED) {
        record_postcondition_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED,
            ESTALE
        );
        goto done;
    }
    execution.result.posttest_candidate_identified = true;
    if (!candidate_same(
            &execution.pretest_candidate, &execution.posttest_candidate
        )) {
        record_postcondition_failure(
            &execution,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED,
            ESTALE
        );
        goto done;
    }
    execution.result.posttest_topology_unchanged = true;

done:
    cleanup_execution(&execution);
    *out_result = execution.result;
    return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE;
}
