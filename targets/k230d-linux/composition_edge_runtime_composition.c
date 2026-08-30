#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Scene, presentation, C2 coordination, and render progression. */

static bool candidate_nonce(
    const MeisoCompositionCoordinatorPlanRef *ref,
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE]
)
{
    size_t index;
    if (ref == NULL || nonce == NULL || ref->plan_generation == 0U) {
        return false;
    }
    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        nonce[index] = (uint8_t)(
            ref->candidate_digest[index] ^
            (uint8_t)(ref->plan_generation >> ((index % 8U) * 8U)) ^
            (uint8_t)(index + 1U)
        );
    }
    nonce[0] |= 1U;
    return true;
}
static bool world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
        lhs->desired_state_version == rhs->desired_state_version &&
        lhs->plan_generation == rhs->plan_generation &&
        lhs->world_generation == rhs->world_generation;
}

bool meiso_k230d_composition_edge_runtime_private_disable_inline_producer(EdgeInlineChannel *channel)
{
    if (channel == NULL || channel->sample_queued || channel->sample_sent) {
        return false;
    }
    while (meiso_stream_count(channel->stream) != 0U) {
        if (!meiso_stream_pop(channel->stream)) {
            return false;
        }
    }
    channel->snapshot.enabled = false;
    channel->snapshot.stream_epoch = 0U;
    channel->snapshot.next_sequence = 0U;
    channel->snapshot.queue_depth = 0U;
    channel->pending_sequence = 0U;
    return true;
}

static bool clear_render_pipeline(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool disable_inline
)
{
    EdgeInlineChannel *channel;
    if (runtime == NULL) {
        return false;
    }
    channel = &runtime->inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT
    ];
    if (disable_inline) {
        if (!meiso_k230d_composition_edge_runtime_private_disable_inline_producer(channel)) {
            return false;
        }
    } else {
        if (channel->sample_queued || channel->sample_sent) {
            return false;
        }
        while (meiso_stream_count(channel->stream) != 0U) {
            if (!meiso_stream_pop(channel->stream)) {
                return false;
            }
        }
        channel->snapshot.queue_depth = 0U;
        channel->pending_sequence = 0U;
    }
    runtime->render_job_pending = false;
    memset(&runtime->render_job, 0, sizeof(runtime->render_job));
    runtime->render_complete_present = false;
    memset(&runtime->render_complete, 0, sizeof(runtime->render_complete));
    runtime->render_inline_materialized = false;
    runtime->render_inline_accepted = false;
    runtime->render_media_materialized = false;
    meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->render_result_cadence);
    return true;
}

static bool retire_world(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoCompositionSceneWorldRef *world
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionCpuRasterRetireStatus status;
    const EdgeInlineChannel *channel;
    if (runtime == NULL || world == NULL ||
        !meiso_composition_scene_store_control_scope(
            runtime->scene_store, &scope
        )) {
        return false;
    }
    channel = &runtime->inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT
    ];
    if (channel->sample_queued || channel->sample_sent) {
        return false;
    }
    status = meiso_composition_cpu_raster_renderer_retire_world(
        runtime->renderer, &scope, world
    );
    return (status == MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED ||
            status == MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED) &&
        clear_render_pipeline(runtime, true);
}

bool meiso_k230d_composition_edge_runtime_private_prepare_stored_scene(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool *out_attempted,
    bool *out_active_plan,
    MeisoCompositionScenePrepareStatus *out_status
)
{
    MeisoCompositionCoordinatorPlanView plan;
    MeisoCompositionScenePrepareStatus status;
    const EdgeDesiredChannel *desired;
    bool active_plan = false;
    if (runtime == NULL || out_attempted == NULL || out_active_plan == NULL ||
        out_status == NULL) {
        return false;
    }
    *out_attempted = false;
    *out_active_plan = false;
    *out_status = MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB;
    desired = &runtime->desired[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE];
    if (!runtime->scene_desired_present || !desired->reference.present ||
        !desired->decoded.enabled) {
        return true;
    }
    if (!meiso_composition_coordinator_candidate(runtime->coordinator, &plan)) {
        if (!meiso_composition_coordinator_active(runtime->coordinator, &plan)) {
            return true;
        }
        active_plan = true;
    }
    status = meiso_composition_scene_store_prepare(
        runtime->scene_store, desired->reference.owner.incarnation,
        desired->reference.version, plan.ref.plan_generation,
        &runtime->scene_desired
    );
    if (status != MEISO_COMPOSITION_SCENE_PREPARE_PREPARED &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE &&
        status != MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
        return false;
    }
    *out_attempted = true;
    *out_active_plan = active_plan;
    *out_status = status;
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_try_prepare_presentation(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionSceneWorldView scene;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    if (!runtime->coordinator_initialized ||
        !meiso_composition_coordinator_candidate(
            runtime->coordinator, &candidate
        ) || candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return true;
    }
    if (!meiso_composition_scene_store_candidate(runtime->scene_store, &scene) ||
        scene.ref.plan_generation != candidate.ref.plan_generation) {
        return meiso_composition_coordinator_report_prepare_outcome(
            runtime->coordinator, &candidate.ref,
            MEISO_COMPOSITION_COORDINATOR_BACKEND_READY, now,
            (const uint8_t[MEISO_SHA256_DIGEST_SIZE]){1U}
        ) == MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE;
    }
    if (!candidate_nonce(&candidate.ref, nonce)) {
        return false;
    }
    return meiso_composition_coordinator_report_prepare_outcome(
        runtime->coordinator, &candidate.ref,
        MEISO_COMPOSITION_COORDINATOR_BACKEND_READY, now, nonce
    ) == MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED;
}

bool meiso_k230d_composition_edge_runtime_private_apply_presentation(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
)
{
    MeisoCompositionCoordinatorObserveStatus observe;
    MeisoCompositionCoordinatorActivateStatus activate;
    MeisoCompositionCoordinatorDisableStatus disable;
    MeisoCompositionSceneWorldRef active;
    MeisoCompositionSceneWorldView old_world;
    MeisoCompositionCoordinatorPlanView plan;
    bool scene_attempted;
    bool active_plan;
    MeisoCompositionScenePrepareStatus scene_prepare;
    bool old_world_present;
    EdgeActualChannel *actual = &runtime->actual[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
    ];
    if (desired->enabled) {
        if (meiso_composition_scene_presentation_plan_decode(
                desired->profile, &runtime->presentation_plan
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
            meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION,
                reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
            );
        }
        runtime->presentation_plan_present = true;
        observe = meiso_composition_coordinator_observe_desired(
            runtime->coordinator, reference.owner.incarnation, reference.version,
            desired, now
        );
        if (observe == MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING) {
            if (!meiso_k230d_composition_edge_runtime_private_prepare_stored_scene(
                    runtime, &scene_attempted, &active_plan, &scene_prepare
                ) || !meiso_k230d_composition_edge_runtime_private_try_prepare_presentation(runtime, now)) {
                return false;
            }
        } else if (observe ==
                   MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING) {
            if (!meiso_composition_coordinator_candidate(
                    runtime->coordinator, &plan
                ) || !plan.prepared_token_present) {
                return false;
            }
            old_world_present = meiso_composition_scene_store_active(
                runtime->scene_store, &old_world
            );
            activate = meiso_composition_coordinator_activate_frame_boundary(
                runtime->coordinator, plan.prepared_token, now, &active
            );
            if (activate != MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED) {
                return false;
            }
            if (old_world_present &&
                !world_ref_equal(&old_world.ref, &active) &&
                !retire_world(runtime, &old_world.ref)) {
                return false;
            }
            if (runtime->desired[
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE
                ].reference.present) {
                if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                        runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE,
                        runtime->desired[
                            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE
                        ].reference,
                        true, true, true
                    ) || !meiso_k230d_composition_edge_runtime_private_start_inline_producer(
                        runtime,
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT,
                        runtime->actual[
                            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
                        ].snapshot.production_epoch
                    ) || !meiso_k230d_composition_edge_runtime_private_publish_actual(
                        runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
                    )) {
                    return false;
                }
            }
        } else if (observe != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE &&
                   observe !=
                       MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE) {
            meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION,
                reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
            );
        }
        {
            const bool running =
                meiso_composition_coordinator_active(
                    runtime->coordinator, &plan
                ) && plan.ref.desired_owner_incarnation ==
                    reference.owner.incarnation &&
                plan.ref.desired_state_version == reference.version;
            if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                    runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION,
                    reference, true, running, running
                )) {
                return false;
            }
        }
    } else {
        if (desired->profile.size != 0U ||
            meiso_composition_coordinator_observe_desired(
                runtime->coordinator, reference.owner.incarnation,
                reference.version, desired, now
            ) != MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING) {
            return false;
        }
        old_world_present = meiso_composition_scene_store_active(
            runtime->scene_store, &old_world
        );
        disable = meiso_composition_coordinator_disable_frame_boundary(
            runtime->coordinator, true, now
        );
        if (disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED &&
            disable != MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED) {
            return false;
        }
        if ((old_world_present && !retire_world(runtime, &old_world.ref)) ||
            (!old_world_present && !clear_render_pipeline(runtime, true))) {
            return false;
        }
        runtime->presentation_plan_present = false;
        meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->render_media_cadence);
        if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION,
                reference, false, false, false
            )) {
            return false;
        }
        if (runtime->desired[
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE
            ].reference.present && runtime->desired[
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE
            ].decoded.enabled && runtime->scene_desired_present) {
            if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                    runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE,
                    runtime->desired[
                        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE
                    ].reference,
                    true, false, false
                ) || !meiso_k230d_composition_edge_runtime_private_publish_actual(
                    runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
                )) {
                return false;
            }
        }
    }
    actual->snapshot.applied_desired = reference;
    return meiso_k230d_composition_edge_runtime_private_publish_actual(
        runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
    );
}

static bool refresh_scene_actual(MeisoK230dCompositionEdgeRuntime *runtime)
{
    MeisoCompositionSceneWorldView world;
    EdgeDesiredChannel *scene;
    bool running;
    scene = &runtime->desired[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_SCENE];
    if (scene->reference.present && scene->decoded.enabled &&
        runtime->scene_desired_present) {
        running = meiso_composition_scene_store_active(
            runtime->scene_store, &world
        ) && world.ref.desired_owner_incarnation ==
                scene->reference.owner.incarnation &&
            world.ref.desired_state_version == scene->reference.version;
        if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE,
                scene->reference, true, running, false
            ) || !meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
            )) {
            return false;
        }
    }
    return true;
}

static bool refresh_presentation_actual(MeisoK230dCompositionEdgeRuntime *runtime)
{
    MeisoCompositionCoordinatorPlanView plan;
    EdgeDesiredChannel *presentation;
    bool running;
    presentation = &runtime->desired[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_DESIRED_PRESENTATION
    ];
    if (presentation->reference.present && presentation->decoded.enabled &&
        runtime->presentation_plan_present) {
        running = meiso_composition_coordinator_active(
            runtime->coordinator, &plan
        ) && plan.ref.desired_owner_incarnation ==
                presentation->reference.owner.incarnation &&
            plan.ref.desired_state_version == presentation->reference.version;
        if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION,
                presentation->reference, true, running, false
            ) || !meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
            )) {
            return false;
        }
    }
    return true;
}

bool meiso_k230d_composition_edge_runtime_private_refresh_composition_actuals(MeisoK230dCompositionEdgeRuntime *runtime)
{
    return refresh_scene_actual(runtime) && refresh_presentation_actual(runtime);
}

bool meiso_k230d_composition_edge_runtime_private_apply_scene(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoRuntimeDesiredState *desired,
    MeisoRuntimeDesiredReference reference,
    MeisoTimePoint now
)
{
    MeisoCompositionScenePrepareStatus prepare;
    MeisoCompositionSceneWorldView active;
    MeisoCompositionSceneWorldView old_world;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionCoordinatorSwapStatus swap;
    bool prepare_attempted;
    bool active_plan;
    bool old_world_present;
    bool running;
    if (!desired->enabled) {
        old_world_present = meiso_composition_scene_store_active(
            runtime->scene_store, &old_world
        );
        if (desired->profile.size != 0U) {
            meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE, reference,
                MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
            );
            return meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
            );
        }
        if (old_world_present) {
            meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE, reference,
                MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED
            );
            return meiso_k230d_composition_edge_runtime_private_publish_actual(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
            );
        }
        if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE,
                reference, false, false, false
            )) {
            return false;
        }
        runtime->scene_desired_present = false;
        return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE);
    }
    if (meiso_composition_scene_desired_decode(
            desired->profile, &runtime->scene_desired
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE, reference,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE);
    }
    runtime->scene_desired_present = true;
    if (!meiso_k230d_composition_edge_runtime_private_prepare_stored_scene(
            runtime, &prepare_attempted, &active_plan, &prepare
        )) {
        meiso_k230d_composition_edge_runtime_private_set_actual_rejected(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE, reference,
            MEISO_RUNTIME_PRODUCTION_REASON_UNSUPPORTED_PARAMETER
        );
        return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE);
    }
    if (prepare_attempted && active_plan &&
        (prepare == MEISO_COMPOSITION_SCENE_PREPARE_PREPARED ||
         prepare == MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED)) {
        old_world_present = meiso_composition_scene_store_active(
            runtime->scene_store, &old_world
        );
        swap = meiso_composition_coordinator_swap_scene_frame_boundary(
            runtime->coordinator, reference.owner.incarnation, reference.version,
            now, &activated
        );
        if (swap != MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED ||
            (old_world_present &&
             !world_ref_equal(&old_world.ref, &activated) &&
             !retire_world(runtime, &old_world.ref))) {
            return false;
        }
    }
    if (!meiso_k230d_composition_edge_runtime_private_try_prepare_presentation(runtime, now)) {
        return false;
    }
    running = meiso_composition_scene_store_active(runtime->scene_store, &active) &&
        active.ref.desired_owner_incarnation == reference.owner.incarnation &&
        active.ref.desired_state_version == reference.version;
    if (!meiso_k230d_composition_edge_runtime_private_set_actual_applied(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE,
            reference, true, running, running
        )) {
        return false;
    }
    if (running && !meiso_k230d_composition_edge_runtime_private_start_inline_producer(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT,
            runtime->actual[
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
            ].snapshot.production_epoch
        )) {
        return false;
    }
    return meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE) &&
        refresh_presentation_actual(runtime);
}

int meiso_k230d_composition_edge_runtime_private_pump_renderer(
    MeisoK230dCompositionEdgeRuntime *runtime,
    MeisoTimePoint now,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionSceneWorldView world;
    MeisoCompositionCpuRasterSubmitStatus submit;
    MeisoCompositionCpuRasterRenderStatus render;
    MeisoCompositionMediaUnit unit;
    size_t size = 0U;
    bool render_route;
    uint32_t result_rate_millihz;
    uint64_t result_epoch;
    if (!runtime->actual[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE]
             .snapshot.effective_enabled ||
        !runtime->actual[MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION]
             .snapshot.effective_enabled ||
        !meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_SCENE
        ) || !meiso_k230d_composition_edge_runtime_private_actual_delivery_current(
            runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_PRESENTATION
        ) ||
        !runtime->presentation_plan_present ||
        !meiso_composition_scene_store_control_scope(
            runtime->scene_store, &scope
        ) || !meiso_composition_scene_store_active(runtime->scene_store, &world)) {
        return 0;
    }
    result_epoch = runtime->inline_streams[
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT
    ].snapshot.stream_epoch;
    result_rate_millihz =
        (uint32_t)runtime->presentation_plan.max_result_hz * UINT32_C(1000);
    if (!runtime->render_job_pending && !runtime->render_complete_present) {
        if (!meiso_k230d_composition_edge_runtime_private_cadence_ready(
                &runtime->render_result_cadence, result_epoch,
                result_rate_millihz, now.monotonic_ns
            )) {
            return 0;
        }
        submit = meiso_composition_cpu_raster_renderer_submit(
            runtime->renderer, &scope, &world, runtime->config.edge_clock_epoch,
            &runtime->render_job
        );
        if (submit != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED) {
            return submit == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUE_FULL ?
                0 : -1;
        }
        runtime->render_job_pending = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_SCENE_PROGRESS;
        return 1;
    }
    if (runtime->render_job_pending) {
        if (now.monotonic_ns == UINT64_MAX) {
            return -1;
        }
        render = meiso_composition_cpu_raster_renderer_render_next(
            runtime->renderer, &runtime->render_job, now.monotonic_ns,
            now.monotonic_ns + 1U, &runtime->render_complete
        );
        if (render != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
            return render == MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB ?
                0 : -1;
        }
        runtime->render_job_pending = false;
        runtime->render_complete_present = true;
        runtime->render_inline_materialized = false;
        runtime->render_inline_accepted = false;
        runtime->render_media_materialized = false;
        if (!meiso_k230d_composition_edge_runtime_private_cadence_commit(
                &runtime->render_result_cadence, now.monotonic_ns
            )) {
            return -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_RENDER_COMPLETE;
        return 1;
    }
    if (!runtime->render_inline_materialized) {
        if (meiso_composition_scene_render_result_encode(
                &runtime->render_complete.result,
                (MeisoMutableBuffer){
                    runtime->stream_scratch, sizeof(runtime->stream_scratch)
                }, &size
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
            !meiso_k230d_composition_edge_runtime_private_enqueue_inline_payload(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT, now,
                (MeisoByteView){runtime->stream_scratch, size}
            )) {
            return -1;
        }
        runtime->render_inline_materialized = true;
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INLINE_SAMPLE;
        out_result->inline_channel_present = true;
        out_result->inline_channel =
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT;
        return 1;
    }
    render_route = meiso_k230d_composition_edge_runtime_private_media_route_active(
        runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_RENDER
    );
    if (render_route && !runtime->render_media_materialized) {
        if (!runtime->render_media_actual_present || !meiso_k230d_composition_edge_runtime_private_cadence_ready(
                &runtime->render_media_cadence,
                runtime->actual[
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA
                ].snapshot.production_epoch,
                runtime->render_media_actual.effective.fps_millihz,
                now.monotonic_ns
            )) {
            runtime->render_media_materialized = true;
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_SCENE_PROGRESS;
            return 1;
        }
        if (runtime->render_media_sequence == UINT64_MAX) {
            return -1;
        }
        memset(&unit, 0, sizeof(unit));
        unit.binding = runtime->active_media->transport_config.routes[0].binding;
        unit.frame_sequence = runtime->render_media_sequence + 1U;
        unit.source_time = now;
        unit.applied_scene_owner = (MeisoOwnerScope){
            MEISO_OWNER_ROLE_HOST,
            runtime->render_complete.job.world.desired_owner_incarnation
        };
        unit.applied_scene_state_version =
            runtime->render_complete.job.world.desired_state_version;
        unit.active_plan_generation =
            runtime->render_complete.job.world.plan_generation;
        unit.world_generation =
            runtime->render_complete.job.world.world_generation;
        unit.dependency_sequence = runtime->inline_streams[
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INLINE_RENDER_RESULT
        ].pending_sequence;
        unit.pixel_format = MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888;
        unit.codec = MEISO_COMPOSITION_MEDIA_CODEC_RAW;
        unit.width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
        unit.height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
        unit.flags = MEISO_COMPOSITION_MEDIA_FRAME_KEY;
        unit.access_unit = runtime->render_complete.color_rgba8888;
        {
            const MeisoCompositionMediaTransportStatus submit_status =
                meiso_composition_media_transport_submit(
                    runtime->active_media->transport, &unit
                );
            if (submit_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
                submit_status !=
                    MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED) {
                return -1;
            }
        }
        runtime->render_media_sequence = unit.frame_sequence;
        runtime->render_media_materialized = true;
        if (!meiso_k230d_composition_edge_runtime_private_cadence_commit(
                &runtime->render_media_cadence, now.monotonic_ns
            )) {
            return -1;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS;
        out_result->media_source_present = true;
        out_result->media_source =
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
        return 1;
    }
    if (!render_route) {
        runtime->render_media_materialized = true;
    }
    if (runtime->render_inline_accepted && runtime->render_media_materialized) {
        runtime->render_complete_present = false;
        memset(&runtime->render_complete, 0, sizeof(runtime->render_complete));
        return 0;
    }
    return 0;
}
