#include "meiso_composition_coordinator_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Frame transitions own backend outcomes, activation and projected queries. */

MeisoCompositionCoordinatorReportStatus
meiso_composition_coordinator_report_prepare_outcome(
    MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionCoordinatorPlanRef *expected_candidate,
    MeisoCompositionCoordinatorBackendOutcome outcome,
    MeisoTimePoint completed_at, const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE])
{
    MeisoCompositionSceneWorldView scene;
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    uint64_t next_serial;

    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        expected_candidate == NULL ||
        meiso_composition_coordinator_private_ranges_overlap(
            coordinator, sizeof(*coordinator), expected_candidate,
            sizeof(*expected_candidate)) ||
        !meiso_composition_coordinator_private_backend_outcome_valid(outcome) ||
        (outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_READY &&
         (nonce == NULL || meiso_composition_coordinator_private_ranges_overlap(
                               coordinator, sizeof(*coordinator), nonce,
                               MEISO_SHA256_DIGEST_SIZE))) ||
        (outcome != MEISO_COMPOSITION_COORDINATOR_BACKEND_READY &&
         nonce != NULL)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT;
    }
    if (!meiso_composition_coordinator_private_time_valid(coordinator,
                                                          completed_at)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_TIME;
    }
    if (!coordinator->candidate.present ||
        coordinator->candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_NO_CANDIDATE;
    }
    if (!meiso_composition_coordinator_private_plan_ref_equal(
            expected_candidate, &coordinator->candidate.ref)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_CANDIDATE_MISMATCH;
    }
    if (outcome != MEISO_COMPOSITION_COORDINATOR_BACKEND_READY) {
        coordinator->last_event_monotonic_ns = completed_at.monotonic_ns;
        switch (outcome) {
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED:
            meiso_composition_coordinator_private_terminate_candidate(
                coordinator, MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.backend_rejections);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED;
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY:
            meiso_composition_coordinator_private_terminate_candidate(
                coordinator, MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_CAPACITY);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.backend_rejections);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED;
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_RENDERER_FAILED:
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE:
            meiso_composition_coordinator_private_terminate_candidate(
                coordinator, MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.backend_failures);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED;
        default:
            return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT;
        }
    }

    if (meiso_composition_coordinator_private_bytes_zero(
            nonce, MEISO_SHA256_DIGEST_SIZE)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_NONCE;
    }
    if (coordinator->has_last_nonce &&
        memcmp(coordinator->last_nonce, nonce, MEISO_SHA256_DIGEST_SIZE) == 0) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_NONCE_REUSED;
    }
    coordinator->last_event_monotonic_ns = completed_at.monotonic_ns;
    if (!meiso_composition_scene_store_candidate(coordinator->scene_store,
                                                 &scene)) {
        coordinator->candidate.reason =
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE;
        return MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE;
    }
    if (!meiso_composition_coordinator_private_candidate_scene(coordinator,
                                                               &scene)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_SCENE_MISMATCH;
    }
    if (coordinator->token_serial == UINT64_MAX) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_COUNTER_EXHAUSTED;
    }
    next_serial = coordinator->token_serial + 1U;
    if (!meiso_composition_coordinator_private_make_token(
            coordinator, &scene.ref, completed_at, nonce, next_serial, token) ||
        (!meiso_composition_coordinator_private_bytes_zero(
             coordinator->last_token, MEISO_SHA256_DIGEST_SIZE) &&
         memcmp(coordinator->last_token, token, MEISO_SHA256_DIGEST_SIZE) ==
             0)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_COUNTER_EXHAUSTED;
    }
    coordinator->token_serial = next_serial;
    coordinator->has_last_nonce = true;
    memcpy(coordinator->last_nonce, nonce, MEISO_SHA256_DIGEST_SIZE);
    memcpy(coordinator->last_token, token, MEISO_SHA256_DIGEST_SIZE);
    coordinator->candidate.prepared_token_present = true;
    memcpy(coordinator->candidate.prepared_token, token,
           MEISO_SHA256_DIGEST_SIZE);
    coordinator->candidate.prepared_at = completed_at;
    coordinator->candidate.scene_ref = scene.ref;
    coordinator->candidate.candidate_state =
        MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED;
    coordinator->candidate.reason =
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
    meiso_composition_coordinator_private_increment_saturated(
        &coordinator->stats.candidates_prepared);
    return MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED;
}

static MeisoCompositionCoordinatorActivateStatus
map_activate_status(MeisoCompositionSceneActivateStatus status)
{
    switch (status) {
    case MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE:
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISSING;
    case MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH:
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISMATCH;
    case MEISO_COMPOSITION_SCENE_ACTIVATE_COUNTER_EXHAUSTED:
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_COUNTER_EXHAUSTED;
    default:
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT;
    }
}

MeisoCompositionCoordinatorActivateStatus
meiso_composition_coordinator_activate_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    const uint8_t expected_token[MEISO_SHA256_DIGEST_SIZE],
    MeisoTimePoint boundary_at, MeisoCompositionSceneWorldRef *out_active_world)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionSceneActivateStatus scene_status;
    MeisoCompositionCoordinatorPlan next_active;

    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        expected_token == NULL || out_active_world == NULL ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_active_world, sizeof(*out_active_world))) {
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_coordinator_private_time_valid(coordinator,
                                                          boundary_at)) {
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_TIME;
    }
    if (!coordinator->candidate.present ||
        coordinator->candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_NOT_PENDING;
    }
    if (memcmp(expected_token, coordinator->candidate.prepared_token,
               MEISO_SHA256_DIGEST_SIZE) != 0) {
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_TOKEN_MISMATCH;
    }
    if (!meiso_composition_scene_store_candidate(coordinator->scene_store,
                                                 &scene)) {
        meiso_composition_coordinator_private_restart_after_scene_change(
            coordinator, boundary_at);
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISSING;
    }
    if (!meiso_composition_coordinator_private_candidate_scene(coordinator,
                                                               &scene) ||
        !meiso_composition_coordinator_private_world_ref_equal(
            &scene.ref, &coordinator->candidate.scene_ref)) {
        meiso_composition_coordinator_private_restart_after_scene_change(
            coordinator, boundary_at);
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISMATCH;
    }
    scene_status = meiso_composition_scene_store_activate(
        coordinator->scene_store, scene.ref.desired_owner_incarnation,
        scene.ref.desired_state_version, scene.ref.plan_generation, &activated);
    if (scene_status != MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED) {
        if (scene_status == MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE ||
            scene_status == MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH) {
            meiso_composition_coordinator_private_restart_after_scene_change(
                coordinator, boundary_at);
        }
        return map_activate_status(scene_status);
    }

    next_active = coordinator->candidate;
    next_active.candidate_state = MEISO_COMPOSITION_SCENE_CANDIDATE_NONE;
    next_active.reason = MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
    next_active.prepared_token_present = false;
    next_active.activated_at = boundary_at;
    next_active.scene_ref = activated;
    coordinator->active = next_active;
    meiso_composition_coordinator_private_clear_plan(&coordinator->candidate);
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
    *out_active_world = activated;
    meiso_composition_coordinator_private_increment_saturated(
        &coordinator->stats.activations);
    return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED;
}

MeisoCompositionCoordinatorSwapStatus
meiso_composition_coordinator_swap_scene_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    uint64_t expected_scene_owner_incarnation,
    uint64_t expected_scene_state_version, MeisoTimePoint boundary_at,
    MeisoCompositionSceneWorldRef *out_active_world)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionSceneActivateStatus status;

    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        out_active_world == NULL || expected_scene_owner_incarnation == 0U ||
        expected_scene_state_version == 0U ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_active_world, sizeof(*out_active_world))) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_ARGUMENT;
    }
    if (!meiso_composition_coordinator_private_time_valid(coordinator,
                                                          boundary_at)) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_TIME;
    }
    if (coordinator->disable_pending) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_DISABLE_PENDING;
    }
    if (!coordinator->active.present) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_NO_ACTIVE_PLAN;
    }
    if (!meiso_composition_scene_store_candidate(coordinator->scene_store,
                                                 &scene)) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISSING;
    }
    if (scene.ref.desired_owner_incarnation !=
            expected_scene_owner_incarnation ||
        scene.ref.desired_state_version != expected_scene_state_version ||
        scene.ref.plan_generation != coordinator->active.ref.plan_generation ||
        scene.ref.world_generation != 0U) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISMATCH;
    }
    status = meiso_composition_scene_store_activate(
        coordinator->scene_store, expected_scene_owner_incarnation,
        expected_scene_state_version, coordinator->active.ref.plan_generation,
        &activated);
    if (status == MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISSING;
    }
    if (status == MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISMATCH;
    }
    if (status == MEISO_COMPOSITION_SCENE_ACTIVATE_COUNTER_EXHAUSTED) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_COUNTER_EXHAUSTED;
    }
    if (status != MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_ARGUMENT;
    }
    coordinator->active.scene_ref = activated;
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
    *out_active_world = activated;
    meiso_composition_coordinator_private_increment_saturated(
        &coordinator->stats.scene_swaps);
    return MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED;
}

MeisoCompositionCoordinatorDisableStatus
meiso_composition_coordinator_disable_frame_boundary(
    MeisoCompositionCoordinator *coordinator, bool producer_drained,
    MeisoTimePoint boundary_at)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneDisableStatus status;
    bool scene_active;

    if (!meiso_composition_coordinator_private_valid(coordinator)) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_ARGUMENT;
    }
    if (!meiso_composition_coordinator_private_time_valid(coordinator,
                                                          boundary_at)) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_TIME;
    }
    if (!coordinator->has_observation || coordinator->observation.enabled ||
        !coordinator->disable_pending) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_NO_INTENT;
    }
    if (!producer_drained) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_WAITING_DRAIN;
    }
    scene_active =
        meiso_composition_scene_store_active(coordinator->scene_store, &scene);
    if (coordinator->active.present) {
        if (!scene_active ||
            !meiso_composition_coordinator_private_world_ref_equal(
                &scene.ref, &coordinator->active.scene_ref)) {
            return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
        }
        status = meiso_composition_scene_store_disable(coordinator->scene_store,
                                                       &scene.ref);
    } else {
        if (scene_active) {
            return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
        }
        status = meiso_composition_scene_store_disable(coordinator->scene_store,
                                                       NULL);
    }
    if (status == MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH ||
        status == MEISO_COMPOSITION_SCENE_DISABLE_INVALID_ARGUMENT) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
    }
    meiso_composition_coordinator_private_clear_plan(&coordinator->candidate);
    meiso_composition_coordinator_private_clear_plan(&coordinator->active);
    coordinator->disable_pending = false;
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
    meiso_composition_coordinator_private_increment_saturated(
        &coordinator->stats.disables);
    return status == MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED
               ? MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED
               : MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED;
}

bool meiso_composition_coordinator_observation(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorObservation *out_observation)
{
    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        out_observation == NULL ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_observation, sizeof(*out_observation)) ||
        !coordinator->has_observation) {
        return false;
    }
    *out_observation = coordinator->observation;
    return true;
}

static bool plan_view(const MeisoCompositionCoordinator *coordinator,
                      const MeisoCompositionCoordinatorPlan *record,
                      bool active,
                      MeisoCompositionCoordinatorPlanView *out_view)
{
    if (record == NULL || !record->present || out_view == NULL ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_view, sizeof(*out_view))) {
        return false;
    }
    memset(out_view, 0, sizeof(*out_view));
    out_view->ref = record->ref;
    out_view->plan = &record->plan;
    out_view->candidate_state = active ? MEISO_COMPOSITION_SCENE_CANDIDATE_NONE
                                       : record->candidate_state;
    out_view->active_state = active ? MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE
                                    : MEISO_COMPOSITION_SCENE_ACTIVE_NONE;
    out_view->prepared_token_present =
        !active && record->prepared_token_present;
    if (out_view->prepared_token_present) {
        memcpy(out_view->prepared_token, record->prepared_token,
               MEISO_SHA256_DIGEST_SIZE);
    }
    out_view->first_observed_at = record->first_observed_at;
    out_view->prepared_at = record->prepared_at;
    out_view->activated_at = record->activated_at;
    out_view->offline_lease_ms = record->plan.offline_lease_ms;
    return true;
}

bool meiso_composition_coordinator_candidate(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorPlanView *out_view)
{
    return meiso_composition_coordinator_private_valid(coordinator) &&
           plan_view(coordinator, &coordinator->candidate, false, out_view);
}

bool meiso_composition_coordinator_active(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorPlanView *out_view)
{
    return meiso_composition_coordinator_private_valid(coordinator) &&
           plan_view(coordinator, &coordinator->active, true, out_view);
}

bool meiso_composition_coordinator_status(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionScenePresentationStatus *out_status)
{
    const MeisoEndpointId *scene_endpoint;

    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        out_status == NULL ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_status, sizeof(*out_status))) {
        return false;
    }
    scene_endpoint = meiso_composition_scene_scene_endpoint();
    if (scene_endpoint == NULL) {
        return false;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capability_generation = coordinator->capability_generation;
    out_status->candidate_state = MEISO_COMPOSITION_SCENE_CANDIDATE_NONE;
    out_status->active_state = MEISO_COMPOSITION_SCENE_ACTIVE_NONE;
    out_status->scene_binding = *scene_endpoint;
    out_status->target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    out_status->target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    out_status->color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    out_status->depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    if (coordinator->candidate.present) {
        out_status->candidate_state = coordinator->candidate.candidate_state;
        out_status->candidate_plan_generation =
            coordinator->candidate.ref.plan_generation;
        memcpy(out_status->candidate_digest,
               coordinator->candidate.ref.candidate_digest,
               MEISO_SHA256_DIGEST_SIZE);
        out_status->reason = coordinator->candidate.reason;
        if (coordinator->candidate.prepared_token_present &&
            (coordinator->candidate.candidate_state ==
                 MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
             coordinator->candidate.candidate_state ==
                 MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING)) {
            out_status->prepared_token_present = true;
            memcpy(out_status->prepared_token,
                   coordinator->candidate.prepared_token,
                   MEISO_SHA256_DIGEST_SIZE);
        }
    }
    if (coordinator->active.present) {
        out_status->active_state = MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE;
        out_status->active_plan_generation =
            coordinator->active.ref.plan_generation;
        memcpy(out_status->active_digest,
               coordinator->active.ref.candidate_digest,
               MEISO_SHA256_DIGEST_SIZE);
    }
    return true;
}

bool meiso_composition_coordinator_stats(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorStats *out_stats)
{
    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        out_stats == NULL ||
        meiso_composition_coordinator_private_output_overlaps(
            coordinator, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = coordinator->stats;
    return true;
}
