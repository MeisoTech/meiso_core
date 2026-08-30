#include "meiso_composition_coordinator_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Desired observation owns candidate admission and supersession decisions. */

MeisoCompositionCoordinatorObserveStatus
meiso_composition_coordinator_observe_desired(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation, uint64_t desired_state_version,
    const MeisoRuntimeDesiredState *desired, MeisoTimePoint observed_at)
{
    MeisoCompositionScenePresentationPlan decoded_plan;
    const MeisoCompositionScenePresentationPlan *plan = NULL;
    uint8_t full_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE];
    uint8_t base_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE];
    uint8_t calculated_digest[MEISO_SHA256_DIGEST_SIZE];
    uint8_t
        observed_wire_copy[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE + 1U];
    const uint8_t *observed_wire = NULL;
    size_t observed_wire_size = 0U;
    bool enabled;
    bool shape_valid;
    bool material_valid = false;
    bool digest_self_consistent = false;
    bool config_selected = false;
    bool same_generation_present = false;
    bool exact_duplicate;

    if (!meiso_composition_coordinator_private_valid(coordinator) ||
        desired_state_version == 0U || desired == NULL ||
        !meiso_byte_view_valid(desired->profile) ||
        meiso_composition_coordinator_private_ranges_overlap(
            coordinator, sizeof(*coordinator), desired, sizeof(*desired)) ||
        meiso_composition_coordinator_private_ranges_overlap(
            coordinator, sizeof(*coordinator), desired->profile.data,
            desired->profile.size)) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_ARGUMENT;
    }
    if (desired_owner_incarnation == 0U ||
        desired_owner_incarnation != coordinator->owner_incarnation) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_WRONG_OWNER;
    }
    if (!meiso_composition_coordinator_private_time_valid(coordinator,
                                                          observed_at)) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_TIME;
    }
    enabled = desired->enabled;
    shape_valid = enabled ? desired->profile.size ==
                                MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE
                          : desired->profile.size == 0U;
    observed_wire_size = desired->profile.size;
    memset(observed_wire_copy, 0, sizeof(observed_wire_copy));
    if (desired->profile.size <= sizeof(coordinator->observation_wire)) {
        if (desired->profile.size != 0U) {
            memcpy(observed_wire_copy, desired->profile.data,
                   desired->profile.size);
            observed_wire = observed_wire_copy;
        }
    }
    memset(&decoded_plan, 0, sizeof(decoded_plan));
    memset(full_wire, 0, sizeof(full_wire));
    memset(base_wire, 0, sizeof(base_wire));
    memset(calculated_digest, 0, sizeof(calculated_digest));
    if (shape_valid && enabled &&
        meiso_composition_scene_presentation_plan_decode(desired->profile,
                                                         &decoded_plan) ==
            MEISO_COMPOSITION_SCENE_CODEC_OK) {
        plan = &decoded_plan;
        material_valid = meiso_composition_coordinator_private_plan_material(
            plan, full_wire, base_wire, calculated_digest);
        if (material_valid) {
            digest_self_consistent =
                memcmp(plan->candidate_digest, calculated_digest,
                       MEISO_SHA256_DIGEST_SIZE) == 0;
            config_selected =
                meiso_composition_coordinator_private_selected_config_valid(
                    coordinator, plan);
        }
    }

    if (coordinator->has_observation) {
        if (desired_state_version <
            coordinator->observation.desired_state_version) {
            coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.desired_stale);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_STALE;
        }
        if (desired_state_version ==
            coordinator->observation.desired_state_version) {
            exact_duplicate =
                enabled == coordinator->observation.enabled &&
                observed_wire_size <= sizeof(coordinator->observation_wire) &&
                observed_wire_size == coordinator->observation_wire_size &&
                (observed_wire_size == 0U ||
                 (observed_wire != NULL &&
                  memcmp(observed_wire, coordinator->observation_wire,
                         observed_wire_size) == 0));
            coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
            if (exact_duplicate) {
                meiso_composition_coordinator_private_increment_saturated(
                    &coordinator->stats.desired_duplicates);
                return MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE;
            }
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
    }

    coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
    meiso_composition_coordinator_private_increment_saturated(
        &coordinator->stats.desired_observations);
    if (!enabled && shape_valid) {
        if (coordinator->candidate.present) {
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.candidates_superseded);
        }
        meiso_composition_coordinator_private_clear_plan(
            &coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store);
        coordinator->disable_pending = true;
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            false, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, NULL, 0U);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING;
    }

    coordinator->disable_pending = false;
    if (material_valid && plan != NULL) {
        same_generation_present =
            (coordinator->candidate.present &&
             coordinator->candidate.ref.plan_generation ==
                 plan->plan_generation) ||
            (coordinator->active.present &&
             coordinator->active.ref.plan_generation == plan->plan_generation);
    }
    if (digest_self_consistent && !same_generation_present &&
        plan->plan_generation <= coordinator->plan_generation_high_water) {
        const bool immutable_conflict =
            plan->plan_generation == coordinator->plan_generation_high_water &&
            coordinator->plan_generation_identity_present &&
            memcmp(base_wire, coordinator->plan_generation_identity,
                   MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE) != 0;

        if (coordinator->candidate.present) {
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.candidates_superseded);
        }
        meiso_composition_coordinator_private_clear_plan(
            &coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store);
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at,
            immutable_conflict
                ? MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT
                : MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size);
        if (immutable_conflict) {
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }
    if (material_valid && plan != NULL &&
        (!digest_self_consistent || !config_selected) &&
        ((coordinator->candidate.present &&
          coordinator->candidate.ref.plan_generation ==
              plan->plan_generation) ||
         (coordinator->active.present &&
          coordinator->active.ref.plan_generation == plan->plan_generation))) {
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size);
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.desired_conflicts);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
    }
    if (digest_self_consistent && !config_selected &&
        plan->plan_generation > coordinator->plan_generation_high_water) {
        coordinator->plan_generation_high_water = plan->plan_generation;
        coordinator->plan_generation_identity_present = true;
        memcpy(coordinator->plan_generation_identity, base_wire,
               MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
    }
    if (!shape_valid || !material_valid || plan == NULL ||
        !digest_self_consistent || !config_selected) {
        if (coordinator->candidate.present) {
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.candidates_superseded);
        }
        meiso_composition_coordinator_private_clear_plan(
            &coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store);
        if (material_valid && digest_self_consistent &&
            plan->plan_generation != 0U &&
            !meiso_composition_coordinator_private_bytes_zero(
                plan->candidate_digest, MEISO_SHA256_DIGEST_SIZE)) {
            meiso_composition_coordinator_private_set_candidate(
                coordinator, desired_owner_incarnation, desired_state_version,
                plan, base_wire, observed_at,
                MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN);
        }
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            enabled, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size);
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }

    if (coordinator->candidate.present &&
        coordinator->candidate.ref.plan_generation == plan->plan_generation) {
        if (!meiso_composition_coordinator_private_plan_identity_equal(
                &coordinator->candidate, plan, base_wire)) {
            meiso_composition_coordinator_private_set_observation(
                coordinator, desired_owner_incarnation, desired_state_version,
                true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
        if (plan->prepared_token_echo_present) {
            if (!coordinator->candidate.prepared_token_present ||
                memcmp(plan->prepared_token_echo,
                       coordinator->candidate.prepared_token,
                       MEISO_SHA256_DIGEST_SIZE) != 0) {
                if (!coordinator->candidate.prepared_token_present) {
                    coordinator->candidate.ref.desired_state_version =
                        desired_state_version;
                    meiso_composition_coordinator_private_terminate_candidate(
                        coordinator, MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN);
                }
                meiso_composition_coordinator_private_set_observation(
                    coordinator, desired_owner_incarnation,
                    desired_state_version, true, observed_at,
                    MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                    full_wire, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
                meiso_composition_coordinator_private_increment_saturated(
                    &coordinator->stats.token_mismatches);
                return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
            }
            coordinator->candidate.ref.desired_state_version =
                desired_state_version;
            coordinator->candidate.candidate_state =
                MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING;
            coordinator->candidate.reason =
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
            meiso_composition_coordinator_private_set_observation(
                coordinator, desired_owner_incarnation, desired_state_version,
                true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, full_wire,
                MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING;
        }

        if (coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
            coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
            meiso_composition_coordinator_private_set_observation(
                coordinator, desired_owner_incarnation, desired_state_version,
                true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.token_mismatches);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
        }

        coordinator->candidate.ref.desired_state_version =
            desired_state_version;
        if (coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED ||
            coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED) {
            coordinator->candidate.candidate_state =
                MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING;
            coordinator->candidate.reason =
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
        } else if (coordinator->candidate.candidate_state ==
                   MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
            coordinator->candidate.candidate_state =
                MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED;
        }
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, full_wire,
            MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING;
    }

    if (coordinator->active.present &&
        coordinator->active.ref.plan_generation == plan->plan_generation) {
        if (!meiso_composition_coordinator_private_plan_identity_equal(
                &coordinator->active, plan, base_wire)) {
            if (coordinator->candidate.present) {
                meiso_composition_coordinator_private_increment_saturated(
                    &coordinator->stats.candidates_superseded);
                meiso_composition_coordinator_private_clear_plan(
                    &coordinator->candidate);
                (void)meiso_composition_scene_store_discard_candidate(
                    coordinator->scene_store);
            }
            meiso_composition_coordinator_private_set_observation(
                coordinator, desired_owner_incarnation, desired_state_version,
                true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
        if (coordinator->candidate.present) {
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.candidates_superseded);
            meiso_composition_coordinator_private_clear_plan(
                &coordinator->candidate);
            (void)meiso_composition_scene_store_discard_candidate(
                coordinator->scene_store);
        }
        if (plan->prepared_token_echo_present &&
            memcmp(plan->prepared_token_echo,
                   coordinator->active.prepared_token,
                   MEISO_SHA256_DIGEST_SIZE) != 0) {
            meiso_composition_coordinator_private_set_observation(
                coordinator, desired_owner_incarnation, desired_state_version,
                true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
            meiso_composition_coordinator_private_increment_saturated(
                &coordinator->stats.token_mismatches);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
        }
        coordinator->active.ref.desired_state_version = desired_state_version;
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, full_wire,
            MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE;
    }

    if (coordinator->candidate.present) {
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.candidates_superseded);
    }
    meiso_composition_coordinator_private_clear_plan(&coordinator->candidate);
    if (plan->plan_generation <= coordinator->plan_generation_high_water) {
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store);
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN, full_wire,
            MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }
    {
        MeisoCompositionSceneWorldView scene;
        if (meiso_composition_scene_store_candidate(coordinator->scene_store,
                                                    &scene) &&
            scene.ref.plan_generation != plan->plan_generation) {
            (void)meiso_composition_scene_store_discard_candidate(
                coordinator->scene_store);
        }
    }
    if (plan->prepared_token_echo_present) {
        coordinator->plan_generation_high_water = plan->plan_generation;
        coordinator->plan_generation_identity_present = true;
        memcpy(coordinator->plan_generation_identity, base_wire,
               MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store);
        meiso_composition_coordinator_private_set_candidate(
            coordinator, desired_owner_incarnation, desired_state_version, plan,
            base_wire, observed_at, MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN);
        meiso_composition_coordinator_private_set_observation(
            coordinator, desired_owner_incarnation, desired_state_version, true,
            observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN, full_wire,
            MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
        meiso_composition_coordinator_private_increment_saturated(
            &coordinator->stats.token_mismatches);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
    }
    coordinator->plan_generation_high_water = plan->plan_generation;
    coordinator->plan_generation_identity_present = true;
    memcpy(coordinator->plan_generation_identity, base_wire,
           MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
    meiso_composition_coordinator_private_set_candidate(
        coordinator, desired_owner_incarnation, desired_state_version, plan,
        base_wire, observed_at, MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE);
    meiso_composition_coordinator_private_set_observation(
        coordinator, desired_owner_incarnation, desired_state_version, true,
        observed_at, MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, full_wire,
        MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
    return MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING;
}
