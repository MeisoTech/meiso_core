#include "meiso_composition_coordinator_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Shared primitives own canonical identity, validation and prepared tokens. */

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} MeisoCompositionCoordinatorSha256;

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

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t read_be_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U) |
           ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

static void write_be_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
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

static void sha256_transform(MeisoCompositionCoordinatorSha256 *sha,
                             const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = read_be_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                            rotate_right(words[index - 15U], 18U) ^
                            (words[index - 15U] >> 3U);
        const uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                            rotate_right(words[index - 2U], 19U) ^
                            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    a = sha->state[0];
    b = sha->state[1];
    c = sha->state[2];
    d = sha->state[3];
    e = sha->state[4];
    f = sha->state[5];
    g = sha->state[6];
    h = sha->state[7];
    for (index = 0U; index < 64U; ++index) {
        const uint32_t sum1 =
            rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 =
            h + sum1 + choice + constants[index] + words[index];
        const uint32_t sum0 =
            rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }
    sha->state[0] += a;
    sha->state[1] += b;
    sha->state[2] += c;
    sha->state[3] += d;
    sha->state[4] += e;
    sha->state[5] += f;
    sha->state[6] += g;
    sha->state[7] += h;
}

static void sha256_init(MeisoCompositionCoordinatorSha256 *sha)
{
    static const uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                        0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                        0x1f83d9abU, 0x5be0cd19U};

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(MeisoCompositionCoordinatorSha256 *sha,
                          const uint8_t *data, size_t size)
{
    while (size != 0U) {
        const size_t available = sizeof(sha->block) - sha->block_size;
        const size_t take = size < available ? size : available;

        memcpy(sha->block + sha->block_size, data, take);
        sha->block_size += take;
        sha->total_bytes += take;
        data += take;
        size -= take;
        if (sha->block_size == sizeof(sha->block)) {
            sha256_transform(sha, sha->block);
            sha->block_size = 0U;
        }
    }
}

static void sha256_final(MeisoCompositionCoordinatorSha256 *sha,
                         uint8_t digest[MEISO_SHA256_DIGEST_SIZE])
{
    const uint64_t total_bits = sha->total_bytes * 8U;
    size_t index;

    sha->block[sha->block_size++] = 0x80U;
    if (sha->block_size > 56U) {
        memset(sha->block + sha->block_size, 0,
               sizeof(sha->block) - sha->block_size);
        sha256_transform(sha, sha->block);
        sha->block_size = 0U;
    }
    memset(sha->block + sha->block_size, 0, 56U - sha->block_size);
    for (index = 0U; index < 8U; ++index) {
        sha->block[56U + index] = (uint8_t)(total_bits >> (56U - index * 8U));
    }
    sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        write_be_u32(digest + index * 4U, sha->state[index]);
    }
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

static void hash_u64(MeisoCompositionCoordinatorSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];

    write_be_u64(bytes, value);
    sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_u16(MeisoCompositionCoordinatorSha256 *sha, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)(value >> 8U), (uint8_t)value};

    sha256_update(sha, bytes, sizeof(bytes));
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
    MeisoCompositionCoordinatorSha256 sha;

    sha256_init(&sha);
    sha256_update(&sha, domain, sizeof(domain));
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
    sha256_update(&sha, coordinator->candidate.ref.candidate_digest,
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
    sha256_update(&sha, nonce, MEISO_SHA256_DIGEST_SIZE);
    sha256_final(&sha, out_token);
    return !meiso_composition_coordinator_private_bytes_zero(
        out_token, MEISO_SHA256_DIGEST_SIZE);
}
