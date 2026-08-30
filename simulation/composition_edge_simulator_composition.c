#include "meiso_composition_edge_simulator_private.h"

/* Simulator Composition owner advances Blob, Scene and coordinator state. */

#include <string.h>

bool meiso_composition_edge_simulator_private_clear_render_truth(MeisoEdgeHandle *handle)
{
    MeisoCompositionEdgeInlineView *stream =
        &handle->simulator_snapshot.inline_streams[
            MEISO_COMPOSITION_EDGE_INLINE_RENDER_RESULT
        ];
    const uint64_t received_count = stream->received_count;
    const uint64_t replaced_count = stream->replaced_count;
    MeisoCompositionCpuRasterCompleteView complete;
    MeisoCompositionSceneControlScope scope;

    if (handle->simulator_cpu_raster_enabled &&
        handle->simulator_renderer != NULL &&
        meiso_composition_cpu_raster_renderer_latest_complete(
            handle->simulator_renderer, &complete
        )) {
        const MeisoCompositionCpuRasterRetireStatus status =
            meiso_composition_scene_store_control_scope(
                handle->simulator_scene_store, &scope
            ) ? meiso_composition_cpu_raster_renderer_retire_world(
                    handle->simulator_renderer, &scope, &complete.job.world
                ) : MEISO_COMPOSITION_CPU_RASTER_RETIRE_INVALID_ARGUMENT;
        if (status != MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED &&
            status != MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED) {
            return false;
        }
    }

    memset(stream, 0, sizeof(*stream));
    stream->received_count = received_count;
    stream->replaced_count = replaced_count;
    handle->simulator_snapshot.render_result_present = false;
    memset(&handle->simulator_snapshot.render_result, 0,
           sizeof(handle->simulator_snapshot.render_result));
    handle->simulator_render_result_sequence = 0U;
    handle->simulator_render_result_pending = false;
    handle->simulator_raster_preview_pending = false;
    return true;
}

MeisoCompositionEdgeSimulatorPrepareResult meiso_composition_edge_simulator_private_prepare_candidate(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionScenePrepareStatus prepare =
        MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB;
    MeisoCompositionCoordinatorReportStatus report;
    MeisoCompositionCoordinatorBackendOutcome outcome =
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE] = {1U};
    const uint8_t *nonce_pointer = nonce;

    if (!meiso_composition_coordinator_candidate(
            handle->simulator_coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_WAITING;
    }
    if (handle->simulator_scene_value_present &&
        handle->simulator_scene_enabled) {
        prepare = meiso_composition_scene_store_prepare(
            handle->simulator_scene_store,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].state_version,
            candidate.ref.plan_generation,
            &handle->simulator_scene_value
        );
    }
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED ||
        prepare == MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED ||
        prepare == MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        if (!meiso_composition_edge_simulator_private_candidate_nonce(&candidate.ref, nonce)) {
            return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL;
        }
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        /* READY without a matching Scene candidate yields WAITING_SCENE. */
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY) {
        outcome = MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY;
        nonce_pointer = NULL;
    } else if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED ||
               prepare == MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH) {
        outcome = MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED;
        nonce_pointer = NULL;
    } else {
        return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL;
    }
    report = meiso_composition_coordinator_report_prepare_outcome(
        handle->simulator_coordinator, &candidate.ref, outcome, now,
        nonce_pointer
    );
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED) {
        handle->simulator_presentation_activation_pending = true;
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_PREPARED;
    }
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE) {
        handle->simulator_scene_retry_pending =
            handle->simulator_scene_value_present &&
            handle->simulator_scene_enabled;
        handle->simulator_scene_retry_ready = false;
        return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_WAITING;
    }
    if (report == MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED ||
        report == MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED) {
        meiso_composition_edge_simulator_private_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY ?
                MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
        );
        if (handle->simulator_scene_value_present &&
            handle->simulator_scene_enabled) {
            meiso_composition_edge_simulator_private_reject_actual(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY ?
                    MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                    MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
        }
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_REJECTED;
    }
    return MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL;
}

bool meiso_composition_edge_simulator_private_publish_mesh(MeisoEdgeHandle *handle)
{
    MeisoMutableBuffer staging;
    MeisoBlobStageStatus stage;
    MeisoBlobCommitStatus commit = MEISO_BLOB_COMMIT_INVALID;
    MeisoRuntimeBlobResult result;
    bool published = false;
    bool duplicate = false;

    stage = meiso_blob_store_stage_begin(
        handle->backend_runtime.simulator_blob_store,
        &handle->simulator_mesh_content_id,
        handle->simulator_mesh_size
    );
    if (stage == MEISO_BLOB_STAGE_STARTED) {
        if (!meiso_blob_store_staging_buffer(
                handle->backend_runtime.simulator_blob_store, &staging
            ) || staging.data == NULL ||
            staging.capacity < handle->simulator_mesh_size) {
            return false;
        }
        memmove(staging.data, handle->simulator_mesh_buffer,
                handle->simulator_mesh_size);
        commit = meiso_blob_store_commit_verified(
            handle->backend_runtime.simulator_blob_store,
            handle->simulator_mesh_size,
            meiso_composition_scene_mesh_blob_verify, NULL
        );
        published = commit == MEISO_BLOB_COMMIT_PUBLISHED;
        duplicate = commit == MEISO_BLOB_COMMIT_DUPLICATE;
        if (published) {
            result = MEISO_RUNTIME_BLOB_RESULT_PUBLISHED;
        } else if (duplicate) {
            result = MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
        } else if (commit == MEISO_BLOB_COMMIT_CAPACITY) {
            result = MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
        } else if (commit == MEISO_BLOB_COMMIT_INTEGRITY_FAILED) {
            result = MEISO_RUNTIME_BLOB_RESULT_INTEGRITY_FAILED;
        } else if (commit == MEISO_BLOB_COMMIT_LENGTH_MISMATCH) {
            result = MEISO_RUNTIME_BLOB_RESULT_LENGTH_MISMATCH;
        } else {
            return false;
        }
        if (!published && !duplicate &&
            meiso_blob_store_abort_staging(
                handle->backend_runtime.simulator_blob_store,
                MEISO_BLOB_ABORT_TRANSFER_DISCONTINUITY
            ) != MEISO_BLOB_ABORTED) {
            return false;
        }
    } else if (stage == MEISO_BLOB_STAGE_UNCHANGED) {
        duplicate = true;
        result = MEISO_RUNTIME_BLOB_RESULT_DUPLICATE;
    } else if (stage == MEISO_BLOB_STAGE_CAPACITY) {
        result = MEISO_RUNTIME_BLOB_RESULT_CAPACITY;
    } else if (stage == MEISO_BLOB_STAGE_CONFLICT) {
        result = MEISO_RUNTIME_BLOB_RESULT_CONFLICT;
    } else if (stage == MEISO_BLOB_STAGE_INVALID_CONTENT_ID) {
        result = MEISO_RUNTIME_BLOB_RESULT_INVALID_CONTENT_ID;
    } else {
        return false;
    }
    if (published || duplicate) {
        const MeisoCompositionSceneBlobVisibilityStatus visibility =
            meiso_composition_scene_store_mark_blob_visible(
                handle->simulator_scene_store,
                handle->simulator_desired_owner.incarnation,
                &handle->simulator_mesh_content_id,
                handle->simulator_mesh_size
            );
        if (visibility != MEISO_COMPOSITION_SCENE_BLOB_VISIBLE &&
            visibility !=
                MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED) {
            return false;
        }
        if (published) {
            if (handle->simulator_published_blob_bytes >
                UINT64_MAX - handle->simulator_mesh_size) {
                return false;
            }
            handle->simulator_published_blob_bytes +=
                handle->simulator_mesh_size;
        }
    }
    handle->simulator_mesh_pending = false;
    handle->simulator_snapshot.mesh.queued = false;
    handle->simulator_snapshot.mesh.transfer_active = false;
    handle->simulator_snapshot.mesh.begin_ready = published || duplicate;
    handle->simulator_snapshot.mesh.waiting_result = false;
    handle->simulator_snapshot.mesh.terminal_result_present = true;
    handle->simulator_snapshot.mesh.terminal_result = result;
    handle->simulator_snapshot.mesh.edge_core_published =
        published || duplicate;
    handle->simulator_snapshot.mesh.next_offset =
        published || duplicate ? handle->simulator_mesh_size : 0U;
    if ((published || duplicate) &&
        handle->simulator_scene_retry_pending) {
        handle->simulator_scene_retry_ready = true;
    }
    return meiso_composition_edge_simulator_private_refresh_scene_status(handle);
}

bool meiso_composition_edge_simulator_private_runtime_plan(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE],
    MeisoRuntimeDesiredState *out_desired
)
{
    size_t wire_size = 0U;

    if (plan == NULL || wire == NULL || out_desired == NULL ||
        meiso_composition_scene_presentation_plan_encode(
            plan,
            (MeisoMutableBuffer){
                wire, MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE
            }, &wire_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        wire_size != MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE) {
        return false;
    }
    out_desired->enabled = true;
    out_desired->profile = (MeisoByteView){wire, wire_size};
    return true;
}

bool meiso_composition_edge_simulator_private_apply_scene_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSimulatorScenePending *pending = &handle->simulator_scene_pending;
    MeisoCompositionCoordinatorPlanView plan;
    MeisoCompositionScenePrepareStatus prepare;
    MeisoCompositionCoordinatorSwapStatus swap;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionSceneWorldView active_world;
    const bool command_pending = pending->pending;
    const bool requested_enabled = command_pending ? pending->enabled :
        handle->simulator_scene_enabled;
    bool plan_active = false;
    uint64_t epoch;
    MeisoCompositionSceneDisableStatus disable_status;

    if (command_pending) {
        pending->pending = false;
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
    }
    if (!requested_enabled) {
        if (meiso_composition_scene_store_active(
                handle->simulator_scene_store, &active_world
            )) {
            meiso_composition_edge_simulator_private_reject_actual(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
            return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
        }
        handle->simulator_scene_enabled = false;
        handle->simulator_scene_value_present = false;
        memset(&handle->simulator_scene_value, 0,
               sizeof(handle->simulator_scene_value));
        disable_status = meiso_composition_scene_store_disable(
            handle->simulator_scene_store, NULL
        );
        if (disable_status != MEISO_COMPOSITION_SCENE_DISABLE_DISABLED &&
            disable_status !=
                MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        meiso_composition_edge_simulator_private_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, false, false, 0U
        );
        return meiso_composition_edge_simulator_private_clear_render_truth(handle) &&
            meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    if (command_pending) {
        handle->simulator_scene_enabled = true;
        handle->simulator_scene_value_present = true;
        handle->simulator_scene_value = pending->value;
    }
    if (meiso_composition_coordinator_active(
            handle->simulator_coordinator, &plan
        )) {
        plan_active = true;
    } else if (!meiso_composition_coordinator_candidate(
                   handle->simulator_coordinator, &plan
               )) {
        meiso_composition_edge_simulator_private_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    prepare = meiso_composition_scene_store_prepare(
        handle->simulator_scene_store,
        handle->simulator_desired_owner.incarnation,
        handle->simulator_snapshot.desired[
            MEISO_COMPOSITION_EDGE_DESIRED_SCENE
        ].state_version,
        plan.ref.plan_generation,
        &handle->simulator_scene_value
    );
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        handle->simulator_scene_retry_pending = true;
        handle->simulator_scene_retry_ready = false;
        meiso_composition_edge_simulator_private_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        if (!plan_active &&
            meiso_composition_edge_simulator_private_prepare_candidate(handle, now) ==
                MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL) {
            return false;
        }
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    if (prepare != MEISO_COMPOSITION_SCENE_PREPARE_PREPARED &&
        prepare != MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED &&
        prepare != MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        handle->simulator_scene_retry_pending = false;
        handle->simulator_scene_retry_ready = false;
        meiso_composition_edge_simulator_private_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
            prepare == MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY ?
                MEISO_RUNTIME_PRODUCTION_REASON_ADMISSION_DENIED :
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
        );
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    if (!plan_active) {
        const MeisoCompositionEdgeSimulatorPrepareResult result =
            meiso_composition_edge_simulator_private_prepare_candidate(handle, now);
        if (result == MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL) {
            return false;
        }
        meiso_composition_edge_simulator_private_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, false, 0U
        );
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    if (prepare == MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE) {
        if (!meiso_composition_scene_store_active(
                handle->simulator_scene_store, &active_world
            )) {
            return false;
        }
        epoch = handle->simulator_snapshot.actual[
            MEISO_COMPOSITION_EDGE_ACTUAL_SCENE
        ].production_epoch;
        if (epoch == 0U) {
            return false;
        }
    } else {
        swap = meiso_composition_coordinator_swap_scene_frame_boundary(
            handle->simulator_coordinator,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].state_version,
            now, &world
        );
        if (swap != MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED ||
            !meiso_composition_edge_simulator_private_take_epoch(
                &handle->simulator_scene_epoch,
                &handle->simulator_scene_epoch_available, &epoch
            )) {
            return false;
        }
    }
    handle->simulator_scene_retry_pending = false;
    handle->simulator_scene_retry_ready = false;
    meiso_composition_edge_simulator_private_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE, true, true, epoch
    );
    if (!meiso_composition_edge_simulator_private_clear_render_truth(handle)) {
        return false;
    }
    handle->simulator_render_result_pending = true;
    return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
}

bool meiso_composition_edge_simulator_private_apply_presentation_command(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionEdgeSimulatorPresentationPending *pending =
        &handle->simulator_presentation_pending;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorDisableStatus disable;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];

    pending->pending = false;
    if (!pending->enabled) {
        desired = (MeisoRuntimeDesiredState){false, {NULL, 0U}};
        observe = meiso_composition_coordinator_observe_desired(
            handle->simulator_coordinator,
            handle->simulator_desired_owner.incarnation,
            handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
            ].state_version,
            &desired, now
        );
        if (observe !=
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING &&
            observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE) {
            return false;
        }
        disable = meiso_composition_coordinator_disable_frame_boundary(
            handle->simulator_coordinator, true, now
        );
        if (disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED &&
            disable !=
                MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        handle->simulator_presentation_plan_present = false;
        handle->simulator_presentation_activation_pending = false;
        meiso_composition_edge_simulator_private_apply_actual_state(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            false, false, 0U
        );
        if (!meiso_composition_edge_simulator_private_clear_render_truth(handle)) {
            return false;
        }
        if (handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].requested && !handle->simulator_snapshot.desired[
                MEISO_COMPOSITION_EDGE_DESIRED_SCENE
            ].enabled) {
            handle->simulator_scene_enabled = false;
            handle->simulator_scene_value_present = false;
            memset(&handle->simulator_scene_value, 0,
                   sizeof(handle->simulator_scene_value));
            meiso_composition_edge_simulator_private_apply_actual_state(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                false, false, 0U
            );
        } else if (handle->simulator_scene_value_present &&
                   handle->simulator_scene_enabled) {
            meiso_composition_edge_simulator_private_apply_actual_state(
                handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
                true, false, 0U
            );
        }
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    handle->simulator_presentation_plan = pending->plan;
    handle->simulator_presentation_plan_present = true;
    if (!meiso_composition_edge_simulator_private_runtime_plan(
            &handle->simulator_presentation_plan, wire, &desired
        )) {
        return false;
    }
    observe = meiso_composition_coordinator_observe_desired(
        handle->simulator_coordinator,
        handle->simulator_desired_owner.incarnation,
        handle->simulator_snapshot.desired[
            MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
        ].state_version,
        &desired, now
    );
    if (observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
        observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE &&
        observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE) {
        meiso_composition_edge_simulator_private_reject_actual(
            handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
    }
    meiso_composition_edge_simulator_private_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        true, false, 0U
    );
    if (observe == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING &&
        meiso_composition_edge_simulator_private_prepare_candidate(handle, now) ==
            MEISO_COMPOSITION_EDGE_SIMULATOR_PREPARE_INTERNAL) {
        return false;
    }
    return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
}

bool meiso_composition_edge_simulator_private_activate_presentation(
    MeisoEdgeHandle *handle,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionScenePresentationPlan echoed;
    MeisoRuntimeDesiredState desired;
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorActivateStatus activate;
    MeisoCompositionSceneWorldRef world;
    MeisoCompositionEdgeDesiredView *presentation_desired;
    uint8_t wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    uint64_t presentation_epoch;
    uint64_t scene_epoch;
    uint64_t echo_version;

    if (!meiso_composition_coordinator_candidate(
            handle->simulator_coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        !candidate.prepared_token_present) {
        return false;
    }
    presentation_desired = &handle->simulator_snapshot.desired[
        MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION
    ];
    if (presentation_desired->state_version == UINT64_MAX ||
        !handle->simulator_presentation_epoch_available ||
        !handle->simulator_scene_epoch_available) {
        return false;
    }
    echo_version = presentation_desired->state_version + 1U;
    echoed = handle->simulator_presentation_plan;
    echoed.prepared_token_echo_present = true;
    memmove(echoed.prepared_token_echo, candidate.prepared_token,
            sizeof(echoed.prepared_token_echo));
    if (!meiso_composition_edge_simulator_private_runtime_plan(&echoed, wire, &desired)) {
        return false;
    }
    observe = meiso_composition_coordinator_observe_desired(
        handle->simulator_coordinator,
        handle->simulator_desired_owner.incarnation,
        echo_version, &desired, now
    );
    if (observe !=
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING) {
        return false;
    }
    activate = meiso_composition_coordinator_activate_frame_boundary(
        handle->simulator_coordinator, candidate.prepared_token,
        now, &world
    );
    if (activate != MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED ||
        !meiso_composition_edge_simulator_private_take_epoch(
            &handle->simulator_presentation_epoch,
            &handle->simulator_presentation_epoch_available,
            &presentation_epoch
        ) ||
        !meiso_composition_edge_simulator_private_take_epoch(
            &handle->simulator_scene_epoch,
            &handle->simulator_scene_epoch_available, &scene_epoch
        )) {
        return false;
    }
    presentation_desired->state_version = echo_version;
    handle->simulator_presentation_plan = echoed;
    handle->simulator_presentation_activation_pending = false;
    meiso_composition_edge_simulator_private_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_PRESENTATION,
        true, true, presentation_epoch
    );
    meiso_composition_edge_simulator_private_apply_actual_state(
        handle, MEISO_COMPOSITION_EDGE_DESIRED_SCENE,
        true, true, scene_epoch
    );
    if (!meiso_composition_edge_simulator_private_clear_render_truth(handle)) {
        return false;
    }
    handle->simulator_render_result_pending = true;
    return meiso_composition_edge_simulator_private_refresh_composition_status(handle);
}
