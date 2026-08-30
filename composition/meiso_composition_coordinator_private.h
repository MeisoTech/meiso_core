#ifndef MEISO_COMPOSITION_COORDINATOR_PRIVATE_H
#define MEISO_COMPOSITION_COORDINATOR_PRIVATE_H

/*
 * Build-private and non-installed. The caller arena owns Coordinator state;
 * Coordinator borrows SceneStore for its lifetime. All transition entry
 * points are single-threaded and non-reentrant, including cross-TU helpers.
 */

#include <meiso/composition/coordinator.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_COORDINATOR_MAGIC = 0x434f4f52U,
    MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE =
        MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE
};

typedef struct {
    bool present;
    MeisoCompositionCoordinatorPlanRef ref;
    MeisoCompositionScenePresentationPlan plan;
    uint8_t canonical_base[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE];
    MeisoCompositionSceneCandidateState candidate_state;
    MeisoCompositionScenePresentationReason reason;
    bool prepared_token_present;
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoTimePoint first_observed_at;
    MeisoTimePoint prepared_at;
    MeisoTimePoint activated_at;
    MeisoCompositionSceneWorldRef scene_ref;
} MeisoCompositionCoordinatorPlan;

struct MeisoCompositionCoordinator {
    uint32_t magic;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionSceneControlScope scene_scope;
    MeisoSessionId session_id;
    uint64_t edge_incarnation;
    uint64_t capability_generation;
    uint64_t edge_clock_epoch;
    uint64_t owner_incarnation;
    uint64_t plan_generation_high_water;
    bool plan_generation_identity_present;
    uint8_t
        plan_generation_identity[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE];
    uint64_t last_event_monotonic_ns;
    uint64_t token_serial;
    bool has_last_nonce;
    uint8_t last_nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t last_token[MEISO_SHA256_DIGEST_SIZE];
    bool has_observation;
    bool disable_pending;
    MeisoCompositionCoordinatorObservation observation;
    size_t observation_wire_size;
    uint8_t observation_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE + 1U];
    MeisoCompositionCoordinatorPlan candidate;
    MeisoCompositionCoordinatorPlan active;
    MeisoCompositionCoordinatorStats stats;
};

void meiso_composition_coordinator_private_increment_saturated(uint64_t *value);
bool meiso_composition_coordinator_private_ranges_overlap(const void *lhs,
                                                          size_t lhs_size,
                                                          const void *rhs,
                                                          size_t rhs_size);
bool meiso_composition_coordinator_private_output_overlaps(
    const MeisoCompositionCoordinator *coordinator, const void *output,
    size_t output_size);
bool meiso_composition_coordinator_private_bytes_zero(const uint8_t *bytes,
                                                      size_t size);
bool meiso_composition_coordinator_private_scope_valid(
    const MeisoCompositionCoordinator *coordinator);
bool meiso_composition_coordinator_private_valid(
    const MeisoCompositionCoordinator *coordinator);
bool meiso_composition_coordinator_private_plan_ref_equal(
    const MeisoCompositionCoordinatorPlanRef *lhs,
    const MeisoCompositionCoordinatorPlanRef *rhs);
bool meiso_composition_coordinator_private_time_valid(
    const MeisoCompositionCoordinator *coordinator, MeisoTimePoint time);
bool meiso_composition_coordinator_private_world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs);
bool meiso_composition_coordinator_private_plan_identity_equal(
    const MeisoCompositionCoordinatorPlan *record,
    const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t canonical_base[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE]);
void meiso_composition_coordinator_private_clear_plan(
    MeisoCompositionCoordinatorPlan *plan);
bool meiso_composition_coordinator_private_selected_config_valid(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionScenePresentationPlan *plan);
bool meiso_composition_coordinator_private_plan_material(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t full_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    uint8_t base_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    uint8_t calculated_digest[MEISO_SHA256_DIGEST_SIZE]);
void meiso_composition_coordinator_private_set_observation(
    MeisoCompositionCoordinator *coordinator, uint64_t owner_incarnation,
    uint64_t state_version, bool enabled, MeisoTimePoint observed_at,
    MeisoCompositionCoordinatorObserveStatus result,
    MeisoCompositionScenePresentationReason reason, const uint8_t *wire,
    size_t wire_size);
void meiso_composition_coordinator_private_set_candidate(
    MeisoCompositionCoordinator *coordinator, uint64_t owner_incarnation,
    uint64_t state_version, const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t base_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    MeisoTimePoint observed_at, MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason);
void meiso_composition_coordinator_private_terminate_candidate(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason);
void meiso_composition_coordinator_private_restart_after_scene_change(
    MeisoCompositionCoordinator *coordinator, MeisoTimePoint boundary_at);
bool meiso_composition_coordinator_private_candidate_scene(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneWorldView *out_scene);
bool meiso_composition_coordinator_private_backend_outcome_valid(
    MeisoCompositionCoordinatorBackendOutcome outcome);
bool meiso_composition_coordinator_private_make_token(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionSceneWorldRef *scene_ref, MeisoTimePoint prepared_at,
    const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE], uint64_t serial,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]);

#endif
