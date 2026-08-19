#include "composition_coordinator.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    COORDINATOR_MAGIC = 0x434f4f52U,
    PLAN_WIRE_SIZE = MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE
};

typedef struct {
    bool present;
    MeisoCompositionCoordinatorPlanRef ref;
    MeisoCompositionScenePresentationPlan plan;
    uint8_t canonical_base[PLAN_WIRE_SIZE];
    MeisoCompositionSceneCandidateState candidate_state;
    MeisoCompositionScenePresentationReason reason;
    bool prepared_token_present;
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoTimePoint first_observed_at;
    MeisoTimePoint prepared_at;
    MeisoTimePoint activated_at;
    MeisoCompositionSceneWorldRef scene_ref;
} CoordinatorPlan;

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
    uint8_t plan_generation_identity[PLAN_WIRE_SIZE];
    uint64_t last_event_monotonic_ns;
    uint64_t token_serial;
    bool has_last_nonce;
    uint8_t last_nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t last_token[MEISO_SHA256_DIGEST_SIZE];
    bool has_observation;
    bool disable_pending;
    MeisoCompositionCoordinatorObservation observation;
    size_t observation_wire_size;
    uint8_t observation_wire[PLAN_WIRE_SIZE + 1U];
    CoordinatorPlan candidate;
    CoordinatorPlan active;
    MeisoCompositionCoordinatorStats stats;
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} CoordinatorSha256;

static void increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
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

static bool output_overlaps_coordinator(
    const MeisoCompositionCoordinator *coordinator,
    const void *output,
    size_t output_size
)
{
    return ranges_overlap(
        coordinator, sizeof(*coordinator), output, output_size
    );
}

static bool bytes_zero(const uint8_t *bytes, size_t size)
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

static bool coordinator_scope_valid(
    const MeisoCompositionCoordinator *coordinator
)
{
    MeisoCompositionSceneControlScope current_scope;

    return coordinator != NULL && coordinator->magic == COORDINATOR_MAGIC &&
        coordinator->scene_store != NULL &&
        meiso_session_nonce_valid(coordinator->session_id.host_nonce) &&
        meiso_session_nonce_valid(coordinator->session_id.edge_nonce) &&
        coordinator->edge_incarnation != 0U &&
        coordinator->capability_generation != 0U &&
        coordinator->edge_clock_epoch != 0U &&
        meiso_composition_scene_store_control_scope(
            coordinator->scene_store, &current_scope
        ) && current_scope.desired_owner_incarnation ==
            coordinator->scene_scope.desired_owner_incarnation &&
        current_scope.control_scope_generation ==
            coordinator->scene_scope.control_scope_generation;
}

static bool coordinator_valid(
    const MeisoCompositionCoordinator *coordinator
)
{
    return coordinator_scope_valid(coordinator) &&
        coordinator->owner_incarnation != 0U &&
        coordinator->scene_scope.desired_owner_incarnation ==
            coordinator->owner_incarnation;
}

static bool plan_ref_equal(
    const MeisoCompositionCoordinatorPlanRef *lhs,
    const MeisoCompositionCoordinatorPlanRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->desired_owner_incarnation ==
            rhs->desired_owner_incarnation &&
        lhs->desired_state_version == rhs->desired_state_version &&
        lhs->plan_generation == rhs->plan_generation &&
        memcmp(lhs->candidate_digest, rhs->candidate_digest,
               MEISO_SHA256_DIGEST_SIZE) == 0;
}

static bool time_valid(
    const MeisoCompositionCoordinator *coordinator,
    MeisoTimePoint time
)
{
    return meiso_time_point_valid(time) &&
        time.clock_epoch == coordinator->edge_clock_epoch &&
        time.monotonic_ns >= coordinator->last_event_monotonic_ns;
}

static bool world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->desired_owner_incarnation ==
            rhs->desired_owner_incarnation &&
        lhs->desired_state_version == rhs->desired_state_version &&
        lhs->plan_generation == rhs->plan_generation &&
        lhs->world_generation == rhs->world_generation;
}

static bool plan_identity_equal(
    const CoordinatorPlan *record,
    const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t canonical_base[PLAN_WIRE_SIZE]
)
{
    return record != NULL && record->present && plan != NULL &&
        record->ref.plan_generation == plan->plan_generation &&
        memcmp(record->ref.candidate_digest, plan->candidate_digest,
               MEISO_SHA256_DIGEST_SIZE) == 0 &&
        memcmp(record->canonical_base, canonical_base,
               PLAN_WIRE_SIZE) == 0;
}

static void clear_plan(CoordinatorPlan *plan)
{
    if (plan != NULL) {
        memset(plan, 0, sizeof(*plan));
    }
}

static void strip_activation(
    MeisoCompositionScenePresentationPlan *plan
)
{
    plan->prepared_token_echo_present = false;
    memset(plan->prepared_token_echo, 0,
           sizeof(plan->prepared_token_echo));
}

static bool encode_plan(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t output[PLAN_WIRE_SIZE]
)
{
    size_t size = 0U;

    return plan != NULL && output != NULL &&
        meiso_composition_scene_presentation_plan_encode(
            plan, (MeisoMutableBuffer){output, PLAN_WIRE_SIZE}, &size
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        size == PLAN_WIRE_SIZE;
}

static bool selected_config_valid(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionScenePresentationPlan *plan
)
{
    const MeisoEndpointId *scene_endpoint =
        meiso_composition_scene_scene_endpoint();

    return plan != NULL && scene_endpoint != NULL &&
        plan->capability_generation == coordinator->capability_generation &&
        endpoint_equal(plan->scene_binding, *scene_endpoint) &&
        plan->source ==
            MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE &&
        plan->geometry == MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION &&
        plan->target_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
        plan->target_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT &&
        plan->color_format == MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 &&
        plan->depth_format ==
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 &&
        plan->max_result_hz == MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ &&
        plan->offline_lease_ms <=
            MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS;
}

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t read_be_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
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

static void sha256_transform(
    CoordinatorSha256 *sha,
    const uint8_t block[64]
)
{
    static const uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
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
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
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
        const uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
            rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice +
            constants[index] + words[index];
        const uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
            rotate_right(a, 22U);
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

static void sha256_init(CoordinatorSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(
    CoordinatorSha256 *sha,
    const uint8_t *data,
    size_t size
)
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

static void sha256_final(
    CoordinatorSha256 *sha,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
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
        sha->block[56U + index] =
            (uint8_t)(total_bits >> (56U - index * 8U));
    }
    sha256_transform(sha, sha->block);
    for (index = 0U; index < 8U; ++index) {
        write_be_u32(digest + index * 4U, sha->state[index]);
    }
}

bool meiso_composition_coordinator_plan_digest(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionScenePresentationPlan canonical;
    CoordinatorSha256 sha;
    uint8_t wire[PLAN_WIRE_SIZE];

    if (plan == NULL || out_digest == NULL ||
        ranges_overlap(plan, sizeof(*plan), out_digest,
                       MEISO_SHA256_DIGEST_SIZE)) {
        return false;
    }
    canonical = *plan;
    memset(canonical.candidate_digest, 0x01,
           sizeof(canonical.candidate_digest));
    strip_activation(&canonical);
    if (!encode_plan(&canonical, wire)) {
        return false;
    }
    memset(wire + 24U, 0, MEISO_SHA256_DIGEST_SIZE);
    wire[75U] = 0U;
    memset(wire + 96U, 0, MEISO_SHA256_DIGEST_SIZE);
    sha256_init(&sha);
    sha256_update(&sha, wire, sizeof(wire));
    sha256_final(&sha, out_digest);
    return true;
}

static bool plan_material(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t full_wire[PLAN_WIRE_SIZE],
    uint8_t base_wire[PLAN_WIRE_SIZE],
    uint8_t calculated_digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionScenePresentationPlan base;

    if (!encode_plan(plan, full_wire) ||
        !meiso_composition_coordinator_plan_digest(
            plan, calculated_digest)) {
        return false;
    }
    base = *plan;
    strip_activation(&base);
    return encode_plan(&base, base_wire);
}

static void set_observation(
    MeisoCompositionCoordinator *coordinator,
    uint64_t owner_incarnation,
    uint64_t state_version,
    bool enabled,
    MeisoTimePoint observed_at,
    MeisoCompositionCoordinatorObserveStatus result,
    MeisoCompositionScenePresentationReason reason,
    const uint8_t *wire,
    size_t wire_size
)
{
    coordinator->has_observation = true;
    coordinator->observation.desired_owner_incarnation = owner_incarnation;
    coordinator->observation.desired_state_version = state_version;
    coordinator->observation.enabled = enabled;
    coordinator->observation.first_observed_at = observed_at;
    coordinator->observation.result = result;
    coordinator->observation.reason = reason;
    coordinator->observation_wire_size =
        wire_size <= sizeof(coordinator->observation_wire) ?
            wire_size : SIZE_MAX;
    memset(coordinator->observation_wire, 0,
           sizeof(coordinator->observation_wire));
    if (wire != NULL && wire_size != 0U &&
        wire_size <= sizeof(coordinator->observation_wire)) {
        memcpy(coordinator->observation_wire, wire, wire_size);
    }
}

static void set_candidate(
    MeisoCompositionCoordinator *coordinator,
    uint64_t owner_incarnation,
    uint64_t state_version,
    const MeisoCompositionScenePresentationPlan *plan,
    const uint8_t base_wire[PLAN_WIRE_SIZE],
    MeisoTimePoint observed_at,
    MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason
)
{
    clear_plan(&coordinator->candidate);
    coordinator->candidate.present = true;
    coordinator->candidate.ref.desired_owner_incarnation = owner_incarnation;
    coordinator->candidate.ref.desired_state_version = state_version;
    coordinator->candidate.ref.plan_generation = plan->plan_generation;
    memcpy(coordinator->candidate.ref.candidate_digest,
           plan->candidate_digest, MEISO_SHA256_DIGEST_SIZE);
    coordinator->candidate.plan = *plan;
    strip_activation(&coordinator->candidate.plan);
    memcpy(coordinator->candidate.canonical_base, base_wire,
           PLAN_WIRE_SIZE);
    coordinator->candidate.candidate_state = state;
    coordinator->candidate.reason = reason;
    coordinator->candidate.first_observed_at = observed_at;
}

static void terminate_candidate(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneCandidateState state,
    MeisoCompositionScenePresentationReason reason
)
{
    coordinator->candidate.candidate_state = state;
    coordinator->candidate.reason = reason;
    coordinator->candidate.prepared_token_present = false;
    memset(coordinator->candidate.prepared_token, 0,
           sizeof(coordinator->candidate.prepared_token));
    memset(&coordinator->candidate.scene_ref, 0,
           sizeof(coordinator->candidate.scene_ref));
    (void)meiso_composition_scene_store_discard_candidate(
        coordinator->scene_store
    );
}

static void restart_candidate_after_scene_change(
    MeisoCompositionCoordinator *coordinator,
    MeisoTimePoint boundary_at
)
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

static bool candidate_scene(
    MeisoCompositionCoordinator *coordinator,
    MeisoCompositionSceneWorldView *out_scene
)
{
    return meiso_composition_scene_store_candidate(
            coordinator->scene_store, out_scene) &&
        out_scene->ref.desired_owner_incarnation ==
            coordinator->owner_incarnation &&
        out_scene->ref.plan_generation ==
            coordinator->candidate.ref.plan_generation &&
        out_scene->ref.world_generation == 0U;
}

static bool backend_outcome_valid(
    MeisoCompositionCoordinatorBackendOutcome outcome
)
{
    return outcome <= MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE;
}

static void hash_u64(CoordinatorSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];

    write_be_u64(bytes, value);
    sha256_update(sha, bytes, sizeof(bytes));
}

static void hash_u16(CoordinatorSha256 *sha, uint16_t value)
{
    const uint8_t bytes[2] = {
        (uint8_t)(value >> 8U), (uint8_t)value
    };

    sha256_update(sha, bytes, sizeof(bytes));
}

static bool make_token(
    const MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionSceneWorldRef *scene_ref,
    MeisoTimePoint prepared_at,
    const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE],
    uint64_t serial,
    uint8_t out_token[MEISO_SHA256_DIGEST_SIZE]
)
{
    static const uint8_t domain[] = {
        'M', 'E', 'I', 'S', 'O', '_', 'C', 'O', 'M', 'P', '_',
        'P', 'R', 'E', 'P', 'A', 'R', 'E', 'D', '_', 'V', '0', 0U
    };
    CoordinatorSha256 sha;

    sha256_init(&sha);
    sha256_update(&sha, domain, sizeof(domain));
    hash_u64(&sha, coordinator->session_id.host_nonce.high);
    hash_u64(&sha, coordinator->session_id.host_nonce.low);
    hash_u64(&sha, coordinator->session_id.edge_nonce.high);
    hash_u64(&sha, coordinator->session_id.edge_nonce.low);
    hash_u64(&sha,
             coordinator->scene_scope.control_scope_generation);
    hash_u64(&sha, coordinator->edge_incarnation);
    hash_u64(&sha, coordinator->owner_incarnation);
    hash_u64(&sha, serial);
    hash_u64(&sha, coordinator->capability_generation);
    hash_u64(&sha,
             coordinator->candidate.ref.desired_owner_incarnation);
    hash_u64(&sha,
             coordinator->candidate.ref.desired_state_version);
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
    return !bytes_zero(out_token, MEISO_SHA256_DIGEST_SIZE);
}

size_t meiso_composition_coordinator_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_composition_coordinator_memory_required(void)
{
    return sizeof(MeisoCompositionCoordinator);
}

MeisoCompositionCoordinatorInitStatus meiso_composition_coordinator_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionCoordinatorConfig *config,
    MeisoCompositionCoordinator **out_coordinator
)
{
    MeisoCompositionCoordinatorConfig config_copy;
    MeisoCompositionSceneControlScope scene_scope;
    MeisoCompositionCoordinator *coordinator;

    if (out_coordinator == NULL) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    if (ranges_overlap(arena, arena_size, out_coordinator,
                       sizeof(*out_coordinator)) ||
        ranges_overlap(arena, arena_size, config, sizeof(*config)) ||
        ranges_overlap(config, sizeof(*config), out_coordinator,
                       sizeof(*out_coordinator))) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    *out_coordinator = NULL;
    if (arena == NULL || config == NULL || config->scene_store == NULL ||
        !meiso_session_nonce_valid(config->session_id.host_nonce) ||
        !meiso_session_nonce_valid(config->session_id.edge_nonce) ||
        config->edge_incarnation == 0U ||
        config->capability_generation == 0U ||
        config->edge_clock_epoch == 0U ||
        !meiso_composition_scene_store_control_scope(
            config->scene_store, &scene_scope)) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT;
    }
    if ((uintptr_t)arena %
            meiso_composition_coordinator_alignment_required() != 0U) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(*coordinator)) {
        return MEISO_COMPOSITION_COORDINATOR_INIT_ARENA_TOO_SMALL;
    }
    config_copy = *config;
    memset(arena, 0, sizeof(*coordinator));
    coordinator = arena;
    coordinator->magic = COORDINATOR_MAGIC;
    coordinator->scene_store = config_copy.scene_store;
    coordinator->scene_scope = scene_scope;
    coordinator->session_id = config_copy.session_id;
    coordinator->edge_incarnation = config_copy.edge_incarnation;
    coordinator->capability_generation =
        config_copy.capability_generation;
    coordinator->edge_clock_epoch = config_copy.edge_clock_epoch;
    *out_coordinator = coordinator;
    return MEISO_COMPOSITION_COORDINATOR_INIT_OK;
}

MeisoCompositionCoordinatorOwnerStatus
meiso_composition_coordinator_begin_owner(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation
)
{
    MeisoCompositionSceneOwnerStatus scene_status;

    if (!coordinator_scope_valid(coordinator) ||
        desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (coordinator->owner_incarnation == desired_owner_incarnation) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_UNCHANGED;
    }
    scene_status = meiso_composition_scene_store_reset_control_scope(
        coordinator->scene_store, desired_owner_incarnation
    );
    if (scene_status == MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (scene_status ==
            MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_SCOPE_COUNTER_EXHAUSTED;
    }
    if (!meiso_composition_scene_store_control_scope(
            coordinator->scene_store, &coordinator->scene_scope)) {
        return MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT;
    }
    if (coordinator->owner_incarnation == 0U) {
        increment_saturated(&coordinator->stats.owner_starts);
    } else {
        increment_saturated(&coordinator->stats.owner_changes);
    }
    coordinator->owner_incarnation = desired_owner_incarnation;
    coordinator->plan_generation_high_water = 0U;
    coordinator->plan_generation_identity_present = false;
    memset(coordinator->plan_generation_identity, 0,
           sizeof(coordinator->plan_generation_identity));
    coordinator->has_observation = false;
    coordinator->disable_pending = false;
    memset(&coordinator->observation, 0,
           sizeof(coordinator->observation));
    coordinator->observation_wire_size = 0U;
    memset(coordinator->observation_wire, 0,
           sizeof(coordinator->observation_wire));
    clear_plan(&coordinator->candidate);
    clear_plan(&coordinator->active);
    coordinator->has_last_nonce = false;
    memset(coordinator->last_nonce, 0,
           sizeof(coordinator->last_nonce));
    memset(coordinator->last_token, 0,
           sizeof(coordinator->last_token));
    return MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED;
}

MeisoCompositionCoordinatorObserveStatus
meiso_composition_coordinator_observe_desired(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation,
    uint64_t desired_state_version,
    const MeisoRuntimeDesiredState *desired,
    MeisoTimePoint observed_at
)
{
    MeisoCompositionScenePresentationPlan decoded_plan;
    const MeisoCompositionScenePresentationPlan *plan = NULL;
    uint8_t full_wire[PLAN_WIRE_SIZE];
    uint8_t base_wire[PLAN_WIRE_SIZE];
    uint8_t calculated_digest[MEISO_SHA256_DIGEST_SIZE];
    uint8_t observed_wire_copy[PLAN_WIRE_SIZE + 1U];
    const uint8_t *observed_wire = NULL;
    size_t observed_wire_size = 0U;
    bool enabled;
    bool shape_valid;
    bool material_valid = false;
    bool digest_self_consistent = false;
    bool config_selected = false;
    bool same_generation_present = false;
    bool exact_duplicate;

    if (!coordinator_valid(coordinator) || desired_state_version == 0U ||
        desired == NULL || !meiso_byte_view_valid(desired->profile) ||
        ranges_overlap(coordinator, sizeof(*coordinator), desired,
                       sizeof(*desired)) ||
        ranges_overlap(coordinator, sizeof(*coordinator),
                       desired->profile.data, desired->profile.size)) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_ARGUMENT;
    }
    if (desired_owner_incarnation == 0U ||
        desired_owner_incarnation != coordinator->owner_incarnation) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_WRONG_OWNER;
    }
    if (!time_valid(coordinator, observed_at)) {
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_TIME;
    }
    enabled = desired->enabled;
    shape_valid = enabled ?
        desired->profile.size == PLAN_WIRE_SIZE :
        desired->profile.size == 0U;
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
        meiso_composition_scene_presentation_plan_decode(
            desired->profile, &decoded_plan
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK) {
        plan = &decoded_plan;
        material_valid = plan_material(
            plan, full_wire, base_wire, calculated_digest
        );
        if (material_valid) {
            digest_self_consistent = memcmp(
                plan->candidate_digest, calculated_digest,
                MEISO_SHA256_DIGEST_SIZE
            ) == 0;
            config_selected = selected_config_valid(coordinator, plan);
        }
    }

    if (coordinator->has_observation) {
        if (desired_state_version <
                coordinator->observation.desired_state_version) {
            coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
            increment_saturated(&coordinator->stats.desired_stale);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_STALE;
        }
        if (desired_state_version ==
                coordinator->observation.desired_state_version) {
            exact_duplicate =
                enabled == coordinator->observation.enabled &&
                observed_wire_size <=
                    sizeof(coordinator->observation_wire) &&
                observed_wire_size ==
                    coordinator->observation_wire_size &&
                (observed_wire_size == 0U ||
                 (observed_wire != NULL &&
                  memcmp(observed_wire, coordinator->observation_wire,
                         observed_wire_size) == 0));
            coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
            if (exact_duplicate) {
                increment_saturated(&coordinator->stats.desired_duplicates);
                return MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE;
            }
            increment_saturated(&coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
    }

    coordinator->last_event_monotonic_ns = observed_at.monotonic_ns;
    increment_saturated(&coordinator->stats.desired_observations);
    if (!enabled && shape_valid) {
        if (coordinator->candidate.present) {
            increment_saturated(&coordinator->stats.candidates_superseded);
        }
        clear_plan(&coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store
        );
        coordinator->disable_pending = true;
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            false, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE, NULL, 0U
        );
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING;
    }

    coordinator->disable_pending = false;
    if (material_valid && plan != NULL) {
        same_generation_present =
            (coordinator->candidate.present &&
             coordinator->candidate.ref.plan_generation ==
                 plan->plan_generation) ||
            (coordinator->active.present &&
             coordinator->active.ref.plan_generation ==
                 plan->plan_generation);
    }
    if (digest_self_consistent && !same_generation_present &&
        plan->plan_generation <= coordinator->plan_generation_high_water) {
        const bool immutable_conflict =
            plan->plan_generation ==
                coordinator->plan_generation_high_water &&
            coordinator->plan_generation_identity_present &&
            memcmp(base_wire, coordinator->plan_generation_identity,
                   PLAN_WIRE_SIZE) != 0;

        if (coordinator->candidate.present) {
            increment_saturated(&coordinator->stats.candidates_superseded);
        }
        clear_plan(&coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store
        );
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            immutable_conflict ?
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT :
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size
        );
        if (immutable_conflict) {
            increment_saturated(&coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
        increment_saturated(&coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }
    if (material_valid && plan != NULL &&
        (!digest_self_consistent || !config_selected) &&
        ((coordinator->candidate.present &&
          coordinator->candidate.ref.plan_generation ==
              plan->plan_generation) ||
         (coordinator->active.present &&
          coordinator->active.ref.plan_generation ==
              plan->plan_generation))) {
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size
        );
        increment_saturated(&coordinator->stats.desired_conflicts);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
    }
    if (digest_self_consistent && !config_selected &&
        plan->plan_generation > coordinator->plan_generation_high_water) {
        coordinator->plan_generation_high_water = plan->plan_generation;
        coordinator->plan_generation_identity_present = true;
        memcpy(coordinator->plan_generation_identity, base_wire,
               PLAN_WIRE_SIZE);
    }
    if (!shape_valid || !material_valid || plan == NULL ||
        !digest_self_consistent || !config_selected) {
        if (coordinator->candidate.present) {
            increment_saturated(&coordinator->stats.candidates_superseded);
        }
        clear_plan(&coordinator->candidate);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store
        );
        if (material_valid && digest_self_consistent &&
            plan->plan_generation != 0U &&
            !bytes_zero(plan->candidate_digest,
                        MEISO_SHA256_DIGEST_SIZE)) {
            set_candidate(
                coordinator, desired_owner_incarnation,
                desired_state_version, plan, base_wire, observed_at,
                MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN
            );
        }
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            enabled, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            observed_wire, observed_wire_size
        );
        increment_saturated(&coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }

    if (coordinator->candidate.present &&
        coordinator->candidate.ref.plan_generation == plan->plan_generation) {
        if (!plan_identity_equal(
                &coordinator->candidate, plan, base_wire)) {
            set_observation(
                coordinator, desired_owner_incarnation,
                desired_state_version, true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, PLAN_WIRE_SIZE
            );
            increment_saturated(&coordinator->stats.desired_conflicts);
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
                    terminate_candidate(
                        coordinator,
                        MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN
                    );
                }
                set_observation(
                    coordinator, desired_owner_incarnation,
                    desired_state_version, true, observed_at,
                    MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                    full_wire, PLAN_WIRE_SIZE
                );
                increment_saturated(&coordinator->stats.token_mismatches);
                return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
            }
            coordinator->candidate.ref.desired_state_version =
                desired_state_version;
            coordinator->candidate.candidate_state =
                MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING;
            coordinator->candidate.reason =
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE;
            set_observation(
                coordinator, desired_owner_incarnation,
                desired_state_version, true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE,
                full_wire, PLAN_WIRE_SIZE
            );
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING;
        }

        if (coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
            coordinator->candidate.candidate_state ==
                MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
            set_observation(
                coordinator, desired_owner_incarnation,
                desired_state_version, true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, PLAN_WIRE_SIZE
            );
            increment_saturated(&coordinator->stats.token_mismatches);
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
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE,
            full_wire, PLAN_WIRE_SIZE
        );
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING;
    }

    if (coordinator->active.present &&
        coordinator->active.ref.plan_generation == plan->plan_generation) {
        if (!plan_identity_equal(&coordinator->active, plan, base_wire)) {
            if (coordinator->candidate.present) {
                increment_saturated(
                    &coordinator->stats.candidates_superseded
                );
                clear_plan(&coordinator->candidate);
                (void)meiso_composition_scene_store_discard_candidate(
                    coordinator->scene_store
                );
            }
            set_observation(
                coordinator, desired_owner_incarnation,
                desired_state_version, true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, PLAN_WIRE_SIZE
            );
            increment_saturated(&coordinator->stats.desired_conflicts);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT;
        }
        if (coordinator->candidate.present) {
            increment_saturated(&coordinator->stats.candidates_superseded);
            clear_plan(&coordinator->candidate);
            (void)meiso_composition_scene_store_discard_candidate(
                coordinator->scene_store
            );
        }
        if (plan->prepared_token_echo_present &&
            memcmp(plan->prepared_token_echo,
                   coordinator->active.prepared_token,
                   MEISO_SHA256_DIGEST_SIZE) != 0) {
            set_observation(
                coordinator, desired_owner_incarnation,
                desired_state_version, true, observed_at,
                MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
                full_wire, PLAN_WIRE_SIZE
            );
            increment_saturated(&coordinator->stats.token_mismatches);
            return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
        }
        coordinator->active.ref.desired_state_version =
            desired_state_version;
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE,
            full_wire, PLAN_WIRE_SIZE
        );
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE;
    }

    if (coordinator->candidate.present) {
        increment_saturated(&coordinator->stats.candidates_superseded);
    }
    clear_plan(&coordinator->candidate);
    if (plan->plan_generation <=
            coordinator->plan_generation_high_water) {
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store
        );
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            full_wire, PLAN_WIRE_SIZE
        );
        increment_saturated(&coordinator->stats.invalid_plans);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN;
    }
    {
        MeisoCompositionSceneWorldView scene;
        if (meiso_composition_scene_store_candidate(
                coordinator->scene_store, &scene) &&
            scene.ref.plan_generation != plan->plan_generation) {
            (void)meiso_composition_scene_store_discard_candidate(
                coordinator->scene_store
            );
        }
    }
    if (plan->prepared_token_echo_present) {
        coordinator->plan_generation_high_water = plan->plan_generation;
        coordinator->plan_generation_identity_present = true;
        memcpy(coordinator->plan_generation_identity, base_wire,
               PLAN_WIRE_SIZE);
        (void)meiso_composition_scene_store_discard_candidate(
            coordinator->scene_store
        );
        set_candidate(
            coordinator, desired_owner_incarnation, desired_state_version,
            plan, base_wire, observed_at,
            MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN
        );
        set_observation(
            coordinator, desired_owner_incarnation, desired_state_version,
            true, observed_at,
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH,
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN,
            full_wire, PLAN_WIRE_SIZE
        );
        increment_saturated(&coordinator->stats.token_mismatches);
        return MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH;
    }
    coordinator->plan_generation_high_water = plan->plan_generation;
    coordinator->plan_generation_identity_present = true;
    memcpy(coordinator->plan_generation_identity, base_wire,
           PLAN_WIRE_SIZE);
    set_candidate(
        coordinator, desired_owner_incarnation, desired_state_version,
        plan, base_wire, observed_at,
        MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE
    );
    set_observation(
        coordinator, desired_owner_incarnation, desired_state_version,
        true, observed_at,
        MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING,
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE,
        full_wire, PLAN_WIRE_SIZE
    );
    return MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING;
}

MeisoCompositionCoordinatorReportStatus
meiso_composition_coordinator_report_prepare_outcome(
    MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionCoordinatorPlanRef *expected_candidate,
    MeisoCompositionCoordinatorBackendOutcome outcome,
    MeisoTimePoint completed_at,
    const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE]
)
{
    MeisoCompositionSceneWorldView scene;
    uint8_t token[MEISO_SHA256_DIGEST_SIZE];
    uint64_t next_serial;

    if (!coordinator_valid(coordinator) || expected_candidate == NULL ||
        ranges_overlap(coordinator, sizeof(*coordinator),
                       expected_candidate, sizeof(*expected_candidate)) ||
        !backend_outcome_valid(outcome) ||
        (outcome == MEISO_COMPOSITION_COORDINATOR_BACKEND_READY &&
         (nonce == NULL || ranges_overlap(
             coordinator, sizeof(*coordinator), nonce,
             MEISO_SHA256_DIGEST_SIZE))) ||
        (outcome != MEISO_COMPOSITION_COORDINATOR_BACKEND_READY &&
         nonce != NULL)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT;
    }
    if (!time_valid(coordinator, completed_at)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_TIME;
    }
    if (!coordinator->candidate.present ||
        coordinator->candidate.candidate_state !=
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_NO_CANDIDATE;
    }
    if (!plan_ref_equal(expected_candidate, &coordinator->candidate.ref)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_CANDIDATE_MISMATCH;
    }
    if (outcome != MEISO_COMPOSITION_COORDINATOR_BACKEND_READY) {
        coordinator->last_event_monotonic_ns = completed_at.monotonic_ns;
        switch (outcome) {
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED:
            terminate_candidate(
                coordinator,
                MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN
            );
            increment_saturated(&coordinator->stats.backend_rejections);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED;
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY:
            terminate_candidate(
                coordinator,
                MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_CAPACITY
            );
            increment_saturated(&coordinator->stats.backend_rejections);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED;
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_RENDERER_FAILED:
        case MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE:
            terminate_candidate(
                coordinator,
                MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED,
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED
            );
            increment_saturated(&coordinator->stats.backend_failures);
            return MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED;
        default:
            return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT;
        }
    }

    if (bytes_zero(nonce, MEISO_SHA256_DIGEST_SIZE)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_NONCE;
    }
    if (coordinator->has_last_nonce &&
        memcmp(coordinator->last_nonce, nonce,
               MEISO_SHA256_DIGEST_SIZE) == 0) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_NONCE_REUSED;
    }
    coordinator->last_event_monotonic_ns = completed_at.monotonic_ns;
    if (!meiso_composition_scene_store_candidate(
            coordinator->scene_store, &scene)) {
        coordinator->candidate.reason =
            MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE;
        return MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE;
    }
    if (!candidate_scene(coordinator, &scene)) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_SCENE_MISMATCH;
    }
    if (coordinator->token_serial == UINT64_MAX) {
        return MEISO_COMPOSITION_COORDINATOR_REPORT_COUNTER_EXHAUSTED;
    }
    next_serial = coordinator->token_serial + 1U;
    if (!make_token(
            coordinator, &scene.ref, completed_at, nonce,
            next_serial, token) ||
        (!bytes_zero(coordinator->last_token,
                     MEISO_SHA256_DIGEST_SIZE) &&
         memcmp(coordinator->last_token, token,
                MEISO_SHA256_DIGEST_SIZE) == 0)) {
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
    increment_saturated(&coordinator->stats.candidates_prepared);
    return MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED;
}

static MeisoCompositionCoordinatorActivateStatus map_activate_status(
    MeisoCompositionSceneActivateStatus status
)
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
    MeisoTimePoint boundary_at,
    MeisoCompositionSceneWorldRef *out_active_world
)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionSceneActivateStatus scene_status;
    CoordinatorPlan next_active;

    if (!coordinator_valid(coordinator) || expected_token == NULL ||
        out_active_world == NULL ||
        output_overlaps_coordinator(
            coordinator, out_active_world, sizeof(*out_active_world))) {
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT;
    }
    if (!time_valid(coordinator, boundary_at)) {
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
    if (!meiso_composition_scene_store_candidate(
            coordinator->scene_store, &scene)) {
        restart_candidate_after_scene_change(coordinator, boundary_at);
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISSING;
    }
    if (!candidate_scene(coordinator, &scene) ||
        !world_ref_equal(&scene.ref, &coordinator->candidate.scene_ref)) {
        restart_candidate_after_scene_change(coordinator, boundary_at);
        return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISMATCH;
    }
    scene_status = meiso_composition_scene_store_activate(
        coordinator->scene_store,
        scene.ref.desired_owner_incarnation,
        scene.ref.desired_state_version,
        scene.ref.plan_generation,
        &activated
    );
    if (scene_status != MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED) {
        if (scene_status == MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE ||
            scene_status == MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH) {
            restart_candidate_after_scene_change(coordinator, boundary_at);
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
    clear_plan(&coordinator->candidate);
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
    *out_active_world = activated;
    increment_saturated(&coordinator->stats.activations);
    return MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED;
}

MeisoCompositionCoordinatorSwapStatus
meiso_composition_coordinator_swap_scene_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    uint64_t expected_scene_owner_incarnation,
    uint64_t expected_scene_state_version,
    MeisoTimePoint boundary_at,
    MeisoCompositionSceneWorldRef *out_active_world
)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneWorldRef activated;
    MeisoCompositionSceneActivateStatus status;

    if (!coordinator_valid(coordinator) || out_active_world == NULL ||
        expected_scene_owner_incarnation == 0U ||
        expected_scene_state_version == 0U ||
        output_overlaps_coordinator(
            coordinator, out_active_world, sizeof(*out_active_world))) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_ARGUMENT;
    }
    if (!time_valid(coordinator, boundary_at)) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_TIME;
    }
    if (coordinator->disable_pending) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_DISABLE_PENDING;
    }
    if (!coordinator->active.present) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_NO_ACTIVE_PLAN;
    }
    if (!meiso_composition_scene_store_candidate(
            coordinator->scene_store, &scene)) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISSING;
    }
    if (scene.ref.desired_owner_incarnation !=
            expected_scene_owner_incarnation ||
        scene.ref.desired_state_version != expected_scene_state_version ||
        scene.ref.plan_generation !=
            coordinator->active.ref.plan_generation ||
        scene.ref.world_generation != 0U) {
        return MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISMATCH;
    }
    status = meiso_composition_scene_store_activate(
        coordinator->scene_store,
        expected_scene_owner_incarnation,
        expected_scene_state_version,
        coordinator->active.ref.plan_generation,
        &activated
    );
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
    increment_saturated(&coordinator->stats.scene_swaps);
    return MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED;
}

MeisoCompositionCoordinatorDisableStatus
meiso_composition_coordinator_disable_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    bool producer_drained,
    MeisoTimePoint boundary_at
)
{
    MeisoCompositionSceneWorldView scene;
    MeisoCompositionSceneDisableStatus status;
    bool scene_active;

    if (!coordinator_valid(coordinator)) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_ARGUMENT;
    }
    if (!time_valid(coordinator, boundary_at)) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_TIME;
    }
    if (!coordinator->has_observation ||
        coordinator->observation.enabled ||
        !coordinator->disable_pending) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_NO_INTENT;
    }
    if (!producer_drained) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_WAITING_DRAIN;
    }
    scene_active = meiso_composition_scene_store_active(
        coordinator->scene_store, &scene
    );
    if (coordinator->active.present) {
        if (!scene_active ||
            !world_ref_equal(
                &scene.ref, &coordinator->active.scene_ref)) {
            return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
        }
        status = meiso_composition_scene_store_disable(
            coordinator->scene_store, &scene.ref
        );
    } else {
        if (scene_active) {
            return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
        }
        status = meiso_composition_scene_store_disable(
            coordinator->scene_store, NULL
        );
    }
    if (status == MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH ||
        status == MEISO_COMPOSITION_SCENE_DISABLE_INVALID_ARGUMENT) {
        return MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH;
    }
    clear_plan(&coordinator->candidate);
    clear_plan(&coordinator->active);
    coordinator->disable_pending = false;
    coordinator->last_event_monotonic_ns = boundary_at.monotonic_ns;
    increment_saturated(&coordinator->stats.disables);
    return status == MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED ?
        MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED :
        MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED;
}

bool meiso_composition_coordinator_observation(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorObservation *out_observation
)
{
    if (!coordinator_valid(coordinator) || out_observation == NULL ||
        output_overlaps_coordinator(
            coordinator, out_observation, sizeof(*out_observation)) ||
        !coordinator->has_observation) {
        return false;
    }
    *out_observation = coordinator->observation;
    return true;
}

static bool plan_view(
    const MeisoCompositionCoordinator *coordinator,
    const CoordinatorPlan *record,
    bool active,
    MeisoCompositionCoordinatorPlanView *out_view
)
{
    if (record == NULL || !record->present || out_view == NULL ||
        output_overlaps_coordinator(
            coordinator, out_view, sizeof(*out_view))) {
        return false;
    }
    memset(out_view, 0, sizeof(*out_view));
    out_view->ref = record->ref;
    out_view->plan = &record->plan;
    out_view->candidate_state = active ?
        MEISO_COMPOSITION_SCENE_CANDIDATE_NONE : record->candidate_state;
    out_view->active_state = active ?
        MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE :
        MEISO_COMPOSITION_SCENE_ACTIVE_NONE;
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
    MeisoCompositionCoordinatorPlanView *out_view
)
{
    return coordinator_valid(coordinator) &&
        plan_view(coordinator, &coordinator->candidate, false, out_view);
}

bool meiso_composition_coordinator_active(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorPlanView *out_view
)
{
    return coordinator_valid(coordinator) &&
        plan_view(coordinator, &coordinator->active, true, out_view);
}

bool meiso_composition_coordinator_status(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionScenePresentationStatus *out_status
)
{
    const MeisoEndpointId *scene_endpoint;

    if (!coordinator_valid(coordinator) || out_status == NULL ||
        output_overlaps_coordinator(
            coordinator, out_status, sizeof(*out_status))) {
        return false;
    }
    scene_endpoint = meiso_composition_scene_scene_endpoint();
    if (scene_endpoint == NULL) {
        return false;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->capability_generation = coordinator->capability_generation;
    out_status->candidate_state =
        MEISO_COMPOSITION_SCENE_CANDIDATE_NONE;
    out_status->active_state = MEISO_COMPOSITION_SCENE_ACTIVE_NONE;
    out_status->scene_binding = *scene_endpoint;
    out_status->target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    out_status->target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    out_status->color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    out_status->depth_format =
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    if (coordinator->candidate.present) {
        out_status->candidate_state =
            coordinator->candidate.candidate_state;
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
    MeisoCompositionCoordinatorStats *out_stats
)
{
    if (!coordinator_valid(coordinator) || out_stats == NULL ||
        output_overlaps_coordinator(
            coordinator, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = coordinator->stats;
    return true;
}
