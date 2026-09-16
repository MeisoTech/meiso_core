#include "meiso_composition_coordinator_private.h"
#include "../src/core/meiso_sha256_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Shared primitives own canonical identity, validation and prepared tokens. */

void meiso_composition_coordinator_private_increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

bool meiso_composition_coordinator_private_ranges_overlap(const void *lhs,
                                                          size_t lhs_size,
                                                          const void *rhs,
                                                          size_t rhs_size)
{
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

bool meiso_composition_coordinator_private_output_overlaps(
    const MeisoCompositionCoordinator *coordinator, const void *output,
    size_t output_size)
{
    return meiso_composition_coordinator_private_ranges_overlap(
        coordinator, sizeof(*coordinator), output, output_size);
}

bool meiso_composition_coordinator_private_bytes_zero(const uint8_t *bytes,
                                                      size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_composition_coordinator_private_scope_valid(
    const MeisoCompositionCoordinator *coordinator)
{
    MeisoCompositionSceneControlScope current_scope;

    return coordinator != NULL &&
           coordinator->magic == MEISO_COMPOSITION_COORDINATOR_MAGIC &&
           coordinator->scene_store != NULL &&
           meiso_session_nonce_valid(coordinator->session_id.host_nonce) &&
           meiso_session_nonce_valid(coordinator->session_id.edge_nonce) &&
           coordinator->edge_incarnation != 0U &&
           coordinator->capability_generation != 0U &&
           coordinator->edge_clock_epoch != 0U &&
           meiso_composition_scene_store_control_scope(coordinator->scene_store,
                                                       &current_scope) &&
           current_scope.desired_owner_incarnation ==
               coordinator->scene_scope.desired_owner_incarnation &&
           current_scope.control_scope_generation ==
               coordinator->scene_scope.control_scope_generation;
}

bool meiso_composition_coordinator_private_valid(
    const MeisoCompositionCoordinator *coordinator)
{
    return meiso_composition_coordinator_private_scope_valid(coordinator) &&
           coordinator->owner_incarnation != 0U &&
           coordinator->scene_scope.desired_owner_incarnation ==
               coordinator->owner_incarnation;
}

bool meiso_composition_coordinator_private_plan_ref_equal(
    const MeisoCompositionCoordinatorPlanRef *lhs,
    const MeisoCompositionCoordinatorPlanRef *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
           lhs->desired_state_version == rhs->desired_state_version &&
           lhs->plan_generation == rhs->plan_generation &&
           memcmp(lhs->candidate_digest, rhs->candidate_digest,
                  MEISO_SHA256_DIGEST_SIZE) == 0;
}

bool meiso_composition_coordinator_private_time_valid(
    const MeisoCompositionCoordinator *coordinator, MeisoTimePoint time)
{
    return meiso_time_point_valid(time) &&
           time.clock_epoch == coordinator->edge_clock_epoch &&
           time.monotonic_ns >= coordinator->last_event_monotonic_ns;
}

bool meiso_composition_coordinator_private_world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
           lhs->desired_state_version == rhs->desired_state_version &&
           lhs->plan_generation == rhs->plan_generation &&
           lhs->world_generation == rhs->world_generation;
}

bool meiso_composition_coordinator_private_plan_identity_equal(
    const MeisoCompositionCoordinatorPlan *record,
    const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t canonical_base[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE])
{
    return record != NULL && record->present && plan != NULL &&
           record->ref.plan_generation == plan->plan_generation &&
           memcmp(record->ref.candidate_digest, plan->candidate_digest,
                  MEISO_SHA256_DIGEST_SIZE) == 0 &&
           memcmp(record->canonical_base, canonical_base,
                  MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE) == 0;
}

void meiso_composition_coordinator_private_clear_plan(
    MeisoCompositionCoordinatorPlan *plan)
{
    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
}

static void strip_activation(MeisoCompositionScenePresentationPlan *plan)
{
    plan->prepared_token_echo_present = false;
    memset(plan->prepared_token_echo, 0, sizeof(plan->prepared_token_echo));
}

static bool
encode_plan(const MeisoCompositionScenePresentationPlan *plan,
            uint8_t output[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE])
{
    size_t size = 0U;

    return plan != NULL && output != NULL &&
           meiso_composition_scene_presentation_plan_encode(
               plan,
               (MeisoMutableBuffer){
                   output, MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE},
               &size) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
           size == MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE;
}

bool meiso_composition_coordinator_private_selected_config_valid(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionScenePresentationPlan *plan)
{
    const MeisoEndpointId *scene_endpoint =
        meiso_composition_scene_scene_endpoint();

    return plan != NULL && scene_endpoint != NULL &&
           plan->capability_generation == coordinator->capability_generation &&
           endpoint_equal(plan->scene_binding, *scene_endpoint) &&
           plan->source == MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE &&
           plan->geometry == MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION &&
           plan->target_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
           plan->target_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
           plan->color_format == MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 &&
           plan->depth_format ==
               MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 &&
           plan->max_result_hz != 0U &&
           plan->max_result_hz <= MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ &&
           plan->offline_lease_ms <=
               MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS;
}

static void write_be_u64(uint8_t destination[8], uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

bool meiso_composition_coordinator_private_plan_material(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t full_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    uint8_t base_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    uint8_t calculated_digest[MEISO_SHA256_DIGEST_SIZE])
{
    MeisoCompositionScenePresentationPlan base;

    if (!encode_plan(plan, full_wire) ||
        !meiso_composition_scene_plan_digest(plan, calculated_digest)) {
        return false;
    }
    base = *plan;
    strip_activation(&base);
    return encode_plan(&base, base_wire);
}

void meiso_composition_coordinator_private_set_observation(
    MeisoCompositionCoordinator *coordinator, uint64_t owner_incarnation,
    uint64_t state_version, bool enabled, MeisoTimePoint observed_at,
    MeisoCompositionCoordinatorObserveStatus result,
    MeisoCompositionScenePresentationReason reason, const uint8_t *wire,
    size_t wire_size)
{
    coordinator->has_observation = true;
    coordinator->observation.desired_owner_incarnation = owner_incarnation;
    coordinator->observation.desired_state_version = state_version;
    coordinator->observation.enabled = enabled;
    coordinator->observation.first_observed_at = observed_at;
    coordinator->observation.result = result;
    coordinator->observation.reason = reason;
    coordinator->observation_wire_size =
        wire_size <= sizeof(coordinator->observation_wire) ? wire_size
                                                           : SIZE_MAX;
    memset(coordinator->observation_wire, 0,
           sizeof(coordinator->observation_wire));
    if (wire != NULL && wire_size != 0U &&
        wire_size <= sizeof(coordinator->observation_wire)) {
        memcpy(coordinator->observation_wire, wire, wire_size);
    }
}

void meiso_composition_coordinator_private_set_candidate(
    MeisoCompositionCoordinator *coordinator, uint64_t owner_incarnation,
    uint64_t state_version, const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t base_wire[MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE],
    MeisoTimePoint observed_at, MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason)
{
    meiso_composition_coordinator_private_clear_plan(&coordinator->candidate);
    coordinator->candidate.present = true;
    coordinator->candidate.ref.desired_owner_incarnation = owner_incarnation;
    coordinator->candidate.ref.desired_state_version = state_version;
    coordinator->candidate.ref.plan_generation = plan->plan_generation;
    memcpy(coordinator->candidate.ref.candidate_digest, plan->candidate_digest,
           MEISO_SHA256_DIGEST_SIZE);
    coordinator->candidate.plan = *plan;
    strip_activation(&coordinator->candidate.plan);
    memcpy(coordinator->candidate.canonical_base, base_wire,
           MEISO_COMPOSITION_COORDINATOR_PLAN_WIRE_SIZE);
    coordinator->candidate.candidate_state = state;
    coordinator->candidate.reason = reason;
    coordinator->candidate.first_observed_at = observed_at;
}

void meiso_composition_coordinator_private_terminate_candidate(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason)
{
    coordinator->candidate.candidate_state = state;
    coordinator->candidate.reason = reason;
    coordinator->candidate.prepared_token_present = false;
    memset(coordinator->candidate.prepared_token, 0,
           sizeof(coordinator->candidate.prepared_token));
    memset(&coordinator->candidate.scene_ref, 0,
           sizeof(coordinator->candidate.scene_ref));
    (void)meiso_composition_scene_store_discard_candidate(
        coordinator->scene_store);
}

void meiso_composition_coordinator_private_restart_after_scene_change(
    MeisoCompositionCoordinator *coordinator, MeisoTimePoint boundary_at)
{
    coordinator->candidate.candidate_state =
        MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING;
    coordinator->candidate.reason =
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE;
    coordinator->candidate.prepared_token_present = false;
    memset(coordinator->candidate.prepared_token, 0,
           sizeof(coordinator->candidate.prepared_token));
    memset(&coordinator->candidate.prepared_at, 0,
           sizeof(coordinator->candidate.prepared_at));
    memset(&coordinator->candidate.scene_ref, 0,
           sizeof(coordinator->candidate.scene_ref));
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
}

bool meiso_composition_coordinator_private_candidate_scene(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneWorldView *out_scene)
{
    return meiso_composition_scene_store_candidate(coordinator->scene_store,
                                                   out_scene) &&
           out_scene->ref.desired_owner_incarnation ==
               coordinator->owner_incarnation &&
           out_scene->ref.plan_generation ==
               coordinator->candidate.ref.plan_generation &&
           out_scene->ref.world_generation == 0U;
}

bool meiso_composition_coordinator_private_backend_outcome_valid(
    MeisoCompositionCoordinatorBackendOutcome outcome)
{
    return outcome <= MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE;
}

static void hash_u64(MeisoSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];

    write_be_u64(bytes, value);
    meiso_sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_u16(MeisoSha256 *sha, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)(value >> 8U), (uint8_t)value};

    meiso_sha256_update(sha, bytes, sizeof(bytes));
}

bool meiso_composition_coordinator_private_make_token(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionSceneWorldRef *scene_ref, MeisoTimePoint prepared_at,
    const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE], uint64_t serial,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE])
{
    static const uint8_t domain[] = {'M', 'E', 'I', 'S', 'O', '_', 'C', 'O',
                                     'M', 'P', '_', 'P', 'R', 'E', 'P', 'A',
                                     'R', 'E', 'D', '_', 'V', '0', 0U};
    MeisoSha256 sha;

    meiso_sha256_init(&sha);
    meiso_sha256_update(&sha, domain, sizeof(domain));
    hash_u64(&sha, coordinator->session_id.host_nonce.high);
    hash_u64(&sha, coordinator->session_id.host_nonce.low);
    hash_u64(&sha, coordinator->session_id.edge_nonce.high);
    hash_u64(&sha, coordinator->session_id.edge_nonce.low);
    hash_u64(&sha, coordinator->scene_scope.control_scope_generation);
    hash_u64(&sha, coordinator->edge_incarnation);
    hash_u64(&sha, coordinator->owner_incarnation);
    hash_u64(&sha, serial);
    hash_u64(&sha, coordinator->capability_generation);
    hash_u64(&sha, coordinator->candidate.ref.desired_owner_incarnation);
    hash_u64(&sha, coordinator->candidate.ref.desired_state_version);
    hash_u64(&sha, coordinator->candidate.ref.plan_generation);
    meiso_sha256_update(&sha, coordinator->candidate.ref.candidate_digest,
                  MEISO_SHA256_DIGEST_SIZE);
    hash_u64(&sha, scene_ref->desired_owner_incarnation);
    hash_u64(&sha, scene_ref->desired_state_version);
    hash_u64(&sha, scene_ref->plan_generation);
    hash_u64(&sha, prepared_at.clock_epoch);
    hash_u64(&sha, prepared_at.monotonic_ns);
    hash_u16(&sha, MEISO_COMPOSITION_SCENE_TARGET_WIDTH);
    hash_u16(&sha, MEISO_COMPOSITION_SCENE_TARGET_HEIGHT);
    hash_u16(&sha, MEISO_COMPOSITION_SCENE_COLOR_RGBA8888);
    hash_u16(&sha, MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32);
    meiso_sha256_update(&sha, nonce, MEISO_SHA256_DIGEST_SIZE);
    meiso_sha256_final(&sha, out_token);
    return !meiso_composition_coordinator_private_bytes_zero(
        out_token, MEISO_SHA256_DIGEST_SIZE);
}
