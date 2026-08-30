#include <meiso/composition/coordinator.h>
#include <meiso/composition/reference_renderer.h>
#include <meiso/profiles/composition/scene.h>
#include <meiso/core/blob.h>
#include <meiso/composition/scene_store.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    RUNNER_ARENA_COUNT = 5U,
    RUNNER_BLOB_SLOTS = MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS,
    RUNNER_LINE_BYTES = 768U,
    RUNNER_CUBE_BYTES = 512U,
    RUNNER_CAPABILITY_GENERATION = 5U
};

#ifdef MEISO_K230_LINUX_TARGET
#define RUNNER_VERIFICATION_LABEL "OFFSCREEN_REFERENCE_VECTOR_PASSED"
#define RUNNER_BUILD_TARGET "k230d_linux"
#else
#define RUNNER_VERIFICATION_LABEL "HOST_OFFSCREEN_REFERENCE_VERIFIED"
#define RUNNER_BUILD_TARGET "native_host"
#endif

static const uint8_t GOLDEN_COLOR_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0x3fU, 0xd3U, 0x97U, 0xc5U, 0xe2U, 0x59U, 0xc4U, 0x3dU,
    0x5eU, 0xc4U, 0x80U, 0xf9U, 0xb8U, 0x6aU, 0x7cU, 0x86U,
    0xe9U, 0x4fU, 0xb2U, 0x39U, 0xd0U, 0x34U, 0x04U, 0xf3U,
    0x30U, 0xb8U, 0x8eU, 0x69U, 0x0cU, 0x0aU, 0xeeU, 0x65U
};

static const uint8_t GOLDEN_DEPTH_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0x6dU, 0x9cU, 0x95U, 0x7aU, 0xf7U, 0xb2U, 0x50U, 0x0bU,
    0x30U, 0x9aU, 0x42U, 0x5dU, 0x8cU, 0xd4U, 0x31U, 0x4aU,
    0xeaU, 0x3fU, 0x99U, 0x34U, 0x8fU, 0x5dU, 0x16U, 0x99U,
    0xb9U, 0x63U, 0x15U, 0xa1U, 0x77U, 0x2fU, 0xaaU, 0xaeU
};

static const uint8_t GOLDEN_THUMBNAIL_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0x07U, 0xf0U, 0x56U, 0x87U, 0x2dU, 0x92U, 0xa2U, 0x33U,
    0x5eU, 0x0dU, 0x3fU, 0x1dU, 0xa6U, 0x75U, 0xbfU, 0x02U,
    0x89U, 0x22U, 0x38U, 0xc2U, 0xf8U, 0xb7U, 0xe2U, 0x7cU,
    0x24U, 0xe8U, 0x5dU, 0xf5U, 0xa4U, 0xcdU, 0xdbU, 0x91U
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
    uint32_t words[64];
} RunnerSha256;

typedef struct {
    const char *name;
    const char *domain;
    size_t memory_required;
    size_t alignment;
    void *memory;
} RunnerArena;

typedef struct {
    uint64_t allocation_calls;
    uint64_t successful_allocations;
    uint64_t free_calls;
    uint64_t realloc_calls;
    uint64_t allocation_attempts_after_enable;
    uint64_t allocations_at_enable;
    uint64_t frees_at_enable;
    bool enabled;
} RunnerAllocatorLedger;

typedef struct {
    RunnerSha256 sha;
    uint8_t cube_storage[RUNNER_CUBE_BYTES];
    MeisoByteView cube_wire;
    MeisoContentId cube_id;
    MeisoCompositionSceneDesired scene_desired;
    MeisoCompositionScenePresentationPlan plan;
    uint8_t plan_wire[MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE];
    size_t plan_wire_size;
    MeisoRuntimeDesiredState runtime_desired;
    MeisoCompositionCoordinatorPlanView candidate;
    MeisoCompositionCoordinatorPlanView active_plan;
    MeisoCompositionCoordinatorPlanRef expected_candidate;
    uint8_t nonce[MEISO_SHA256_DIGEST_SIZE];
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoCompositionSceneWorldRef active_world;
    MeisoCompositionSceneWorldView active_view;
    MeisoCompositionSceneControlScope control_scope;
    MeisoCompositionReferenceRendererJobRef job;
    MeisoCompositionReferenceRendererCompleteView complete;
    MeisoCompositionReferenceRendererSnapshot renderer_snapshot;
    MeisoBlobStats blob_stats;
    MeisoCompositionSceneStoreStats scene_stats;
    MeisoCompositionCoordinatorStats coordinator_stats;
    MeisoCompositionReferenceRendererStats renderer_stats;
    uint8_t color_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint8_t depth_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint8_t thumbnail_sha256[MEISO_SHA256_DIGEST_SIZE];
    char color_hex[MEISO_SHA256_DIGEST_SIZE * 2U + 1U];
    char depth_hex[MEISO_SHA256_DIGEST_SIZE * 2U + 1U];
    char thumbnail_hex[MEISO_SHA256_DIGEST_SIZE * 2U + 1U];
    char line[RUNNER_LINE_BYTES];
} RunnerScratch;

typedef struct {
    RunnerArena arenas[RUNNER_ARENA_COUNT];
    RunnerAllocatorLedger allocator;
    RunnerScratch *scratch;
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStore *scene_store;
    MeisoCompositionCoordinator *coordinator;
    MeisoCompositionReferenceRenderer *renderer;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoSessionId session_id;
    char final_line[384];
} Runner;

/*
 * Testable lifecycle seam for the standalone program.  The writer contract is
 * FILE-based: each bounded NDJSON line must be accepted in full; a later write
 * failure is reported but previously written lines are not rolled back.
 */
int meiso_composition_offscreen_runner_run(
    FILE *output,
    FILE *error_output
);

static uint32_t rotate_right(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static uint32_t read_be_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) |
        ((uint32_t)input[1] << 16U) |
        ((uint32_t)input[2] << 8U) |
        (uint32_t)input[3];
}

static void write_be_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void sha256_transform(RunnerSha256 *sha, const uint8_t block[64])
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
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        sha->words[index] = read_be_u32(block + (size_t)index * 4U);
    }
    for (index = 16U; index < 64U; ++index) {
        const uint32_t x = sha->words[index - 15U];
        const uint32_t y = sha->words[index - 2U];
        const uint32_t small0 = rotate_right(x, 7U) ^
            rotate_right(x, 18U) ^ (x >> 3U);
        const uint32_t small1 = rotate_right(y, 17U) ^
            rotate_right(y, 19U) ^ (y >> 10U);
        sha->words[index] = sha->words[index - 16U] + small0 +
            sha->words[index - 7U] + small1;
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
        const uint32_t sum1 = rotate_right(e, 6U) ^
            rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice +
            constants[index] + sha->words[index];
        const uint32_t sum0 = rotate_right(a, 2U) ^
            rotate_right(a, 13U) ^ rotate_right(a, 22U);
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

static void sha256_init(RunnerSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(
    RunnerSha256 *sha,
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
    RunnerSha256 *sha,
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

static void sha256_bytes(
    RunnerSha256 *sha,
    MeisoByteView bytes,
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    sha256_init(sha);
    sha256_update(sha, bytes.data, bytes.size);
    sha256_final(sha, digest);
}

static bool bytes_nonzero(const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
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

static void fill_nonce(uint8_t output[MEISO_SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        output[index] = (uint8_t)(1U + index);
    }
}

static void digest_hex(
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE],
    char output[MEISO_SHA256_DIGEST_SIZE * 2U + 1U]
)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < MEISO_SHA256_DIGEST_SIZE; ++index) {
        output[index * 2U] = digits[digest[index] >> 4U];
        output[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    output[MEISO_SHA256_DIGEST_SIZE * 2U] = '\0';
}

static void *runner_allocate(Runner *runner, size_t size)
{
    void *memory;

    ++runner->allocator.allocation_calls;
    if (runner->allocator.enabled) {
        ++runner->allocator.allocation_attempts_after_enable;
        return NULL;
    }
    memory = calloc(1U, size);
    if (memory != NULL) {
        ++runner->allocator.successful_allocations;
    }
    return memory;
}

static void runner_free(Runner *runner, void **memory)
{
    if (memory != NULL && *memory != NULL) {
        free(*memory);
        *memory = NULL;
        ++runner->allocator.free_calls;
    }
}

static void runner_cleanup(Runner *runner)
{
    size_t index = RUNNER_ARENA_COUNT;

    runner->blob_store = NULL;
    runner->scene_store = NULL;
    runner->coordinator = NULL;
    runner->renderer = NULL;
    runner->scratch = NULL;
    while (index != 0U) {
        --index;
        runner_free(runner, &runner->arenas[index].memory);
    }
}

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool allocate_all_arenas(Runner *runner)
{
    const MeisoResourceDescriptor *blob_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {RUNNER_BLOB_SLOTS};
    size_t index;
    size_t subtotal = 0U;

    if (blob_descriptor == NULL) {
        return false;
    }
    runner->arenas[0] = (RunnerArena){
        "blob_store", "cpu_heap_core_blob",
        meiso_blob_store_memory_required(blob_descriptor, &blob_config),
        meiso_blob_alignment_required(), NULL
    };
    runner->arenas[1] = (RunnerArena){
        "scene_store", "cpu_heap_profile",
        meiso_composition_scene_store_memory_required(),
        meiso_composition_scene_store_alignment_required(), NULL
    };
    runner->arenas[2] = (RunnerArena){
        "coordinator", "cpu_heap_profile",
        meiso_composition_coordinator_memory_required(),
        meiso_composition_coordinator_alignment_required(), NULL
    };
    runner->arenas[3] = (RunnerArena){
        "reference_renderer", "cpu_heap_reference_targets",
        meiso_composition_reference_renderer_memory_required(),
        meiso_composition_reference_renderer_alignment_required(), NULL
    };
    runner->arenas[4] = (RunnerArena){
        "runner_scratch", "cpu_heap_runner",
        sizeof(RunnerScratch), _Alignof(max_align_t), NULL
    };
    for (index = 0U; index < RUNNER_ARENA_COUNT; ++index) {
        if (runner->arenas[index].memory_required == 0U ||
            runner->arenas[index].alignment == 0U ||
            !checked_add_size(
                subtotal, runner->arenas[index].memory_required, &subtotal
            )) {
            return false;
        }
        runner->arenas[index].memory = runner_allocate(
            runner, runner->arenas[index].memory_required
        );
        if (runner->arenas[index].memory == NULL) {
            return false;
        }
    }
    runner->scratch = (RunnerScratch *)runner->arenas[4].memory;
    runner->allocator.allocations_at_enable =
        runner->allocator.successful_allocations;
    runner->allocator.frees_at_enable = runner->allocator.free_calls;
    runner->allocator.enabled = true;
    return runner->allocator.allocations_at_enable == RUNNER_ARENA_COUNT &&
        runner->allocator.frees_at_enable == 0U;
}

static bool initialize_handles(Runner *runner)
{
    const MeisoResourceDescriptor *blob_descriptor =
        meiso_composition_scene_resource(
            MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB
        );
    const MeisoBlobConfig blob_config = {RUNNER_BLOB_SLOTS};
    MeisoCompositionCoordinatorConfig coordinator_config;

    memset(&coordinator_config, 0, sizeof(coordinator_config));
    coordinator_config.session_id = runner->session_id;
    coordinator_config.edge_incarnation = runner->edge_owner.incarnation;
    coordinator_config.capability_generation =
        RUNNER_CAPABILITY_GENERATION;
    coordinator_config.edge_clock_epoch = 77U;
    if (meiso_blob_store_init(
            runner->arenas[0].memory,
            runner->arenas[0].memory_required,
            blob_descriptor,
            &blob_config,
            &runner->blob_store) != MEISO_BLOB_INIT_OK ||
        meiso_composition_scene_store_init(
            runner->arenas[1].memory,
            runner->arenas[1].memory_required,
            runner->blob_store,
            &runner->scene_store) !=
                MEISO_COMPOSITION_SCENE_STORE_INIT_OK) {
        return false;
    }
    coordinator_config.scene_store = runner->scene_store;
    if (meiso_composition_coordinator_init(
            runner->arenas[2].memory,
            runner->arenas[2].memory_required,
            &coordinator_config,
            &runner->coordinator) !=
                MEISO_COMPOSITION_COORDINATOR_INIT_OK ||
        meiso_composition_reference_renderer_init(
            runner->arenas[3].memory,
            runner->arenas[3].memory_required,
            runner->blob_store,
            &runner->renderer) !=
                MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_OK ||
        meiso_composition_coordinator_begin_owner(
            runner->coordinator, runner->host_owner.incarnation) !=
                MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED) {
        return false;
    }
    return true;
}

static void build_scene_desired(Runner *runner)
{
    MeisoCompositionSceneObject *object;
    RunnerScratch *scratch = runner->scratch;

    memset(&scratch->scene_desired, 0, sizeof(scratch->scene_desired));
    scratch->scene_desired.camera.space =
        MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    scratch->scene_desired.camera.space_epoch = 9U;
    scratch->scene_desired.camera.source_time = (MeisoTimePoint){7U, 100U};
    scratch->scene_desired.camera.position[2] = -3.0F;
    scratch->scene_desired.camera.orientation_xyzw[3] = 1.0F;
    scratch->scene_desired.camera.projection =
        MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE;
    scratch->scene_desired.camera.vertical_fov_degrees = 60.0F;
    scratch->scene_desired.camera.near_z = 0.1F;
    scratch->scene_desired.camera.far_z = 100.0F;
    scratch->scene_desired.camera.viewport_width =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    scratch->scene_desired.camera.viewport_height =
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    scratch->scene_desired.object_count = 1U;
    object = &scratch->scene_desired.objects[0];
    object->id = (MeisoCompositionSceneObjectId){1U, 1U};
    object->mesh_content_id = scratch->cube_id;
    object->space = MEISO_COMPOSITION_SCENE_SPACE_WORLD;
    object->space_epoch = 9U;
    object->transform.orientation_xyzw[3] = 1.0F;
    object->transform.scale[0] = 1.0F;
    object->transform.scale[1] = 1.0F;
    object->transform.scale[2] = 1.0F;
    object->flags = MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE;
}

static bool build_plan(Runner *runner)
{
    RunnerScratch *scratch = runner->scratch;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    memset(&scratch->plan, 0, sizeof(scratch->plan));
    scratch->plan.plan_generation = 1U;
    scratch->plan.capability_generation = RUNNER_CAPABILITY_GENERATION;
    scratch->plan.scene_binding =
        *meiso_composition_scene_scene_endpoint();
    scratch->plan.source =
        MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE;
    scratch->plan.geometry =
        MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION;
    scratch->plan.target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    scratch->plan.target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    scratch->plan.color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    scratch->plan.depth_format =
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    scratch->plan.max_result_hz = MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ;
    scratch->plan.offline_lease_ms = 1000U;
    if (!meiso_composition_scene_plan_digest(&scratch->plan, digest)) {
        return false;
    }
    memcpy(scratch->plan.candidate_digest, digest, sizeof(digest));
    return true;
}

static bool observe_plan(
    Runner *runner,
    uint64_t state_version,
    MeisoTimePoint observed_at,
    MeisoCompositionCoordinatorObserveStatus expected
)
{
    RunnerScratch *scratch = runner->scratch;

    scratch->plan_wire_size = 0U;
    if (meiso_composition_scene_presentation_plan_encode(
            &scratch->plan,
            (MeisoMutableBuffer){
                scratch->plan_wire, sizeof(scratch->plan_wire)
            },
            &scratch->plan_wire_size) !=
                MEISO_COMPOSITION_SCENE_CODEC_OK ||
        scratch->plan_wire_size != sizeof(scratch->plan_wire)) {
        return false;
    }
    scratch->runtime_desired.enabled = true;
    scratch->runtime_desired.profile = (MeisoByteView){
        scratch->plan_wire, scratch->plan_wire_size
    };
    return meiso_composition_coordinator_observe_desired(
        runner->coordinator,
        runner->host_owner.incarnation,
        state_version,
        &scratch->runtime_desired,
        observed_at
    ) == expected;
}

static bool publish_cube(Runner *runner)
{
    RunnerScratch *scratch = runner->scratch;
    MeisoMutableBuffer staging;
    size_t cube_size = 0U;

    if (meiso_composition_scene_mesh_encode(
            meiso_composition_scene_canonical_cube(),
            (MeisoMutableBuffer){
                scratch->cube_storage, sizeof(scratch->cube_storage)
            },
            &cube_size) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    scratch->cube_wire = (MeisoByteView){scratch->cube_storage, cube_size};
    if (!meiso_composition_scene_mesh_content_id(
            scratch->cube_wire, &scratch->cube_id) ||
        meiso_blob_store_stage_begin(
            runner->blob_store, &scratch->cube_id, cube_size) !=
                MEISO_BLOB_STAGE_STARTED ||
        !meiso_blob_store_staging_buffer(runner->blob_store, &staging) ||
        staging.capacity != cube_size) {
        return false;
    }
    memcpy(staging.data, scratch->cube_wire.data, cube_size);
    return meiso_blob_store_commit_verified(
               runner->blob_store,
               cube_size,
               meiso_composition_scene_mesh_blob_verify,
               NULL) == MEISO_BLOB_COMMIT_PUBLISHED &&
        meiso_composition_scene_store_mark_blob_visible(
            runner->scene_store,
            runner->host_owner.incarnation,
            &scratch->cube_id,
            cube_size) == MEISO_COMPOSITION_SCENE_BLOB_VISIBLE;
}

static bool prepare_and_activate(Runner *runner)
{
    RunnerScratch *scratch = runner->scratch;

    build_scene_desired(runner);
    if (meiso_composition_scene_store_prepare(
            runner->scene_store,
            runner->host_owner.incarnation,
            1U,
            1U,
            &scratch->scene_desired) !=
                MEISO_COMPOSITION_SCENE_PREPARE_PREPARED ||
        !build_plan(runner) ||
        !observe_plan(
            runner, 1U, (MeisoTimePoint){77U, 100U},
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING) ||
        !meiso_composition_coordinator_candidate(
            runner->coordinator, &scratch->candidate)) {
        return false;
    }
    scratch->expected_candidate = scratch->candidate.ref;
    fill_nonce(scratch->nonce);
    if (meiso_composition_coordinator_report_prepare_outcome(
            runner->coordinator,
            &scratch->expected_candidate,
            MEISO_COMPOSITION_COORDINATOR_BACKEND_READY,
            (MeisoTimePoint){77U, 110U},
            scratch->nonce) != MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED ||
        !meiso_composition_coordinator_candidate(
            runner->coordinator, &scratch->candidate) ||
        !scratch->candidate.prepared_token_present ||
        !bytes_nonzero(
            scratch->candidate.prepared_token,
            sizeof(scratch->candidate.prepared_token))) {
        return false;
    }
    memcpy(scratch->prepared_token, scratch->candidate.prepared_token,
           sizeof(scratch->prepared_token));
    scratch->plan.prepared_token_echo_present = true;
    memcpy(scratch->plan.prepared_token_echo, scratch->prepared_token,
           sizeof(scratch->prepared_token));
    if (!observe_plan(
            runner, 2U, (MeisoTimePoint){77U, 120U},
            MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING) ||
        meiso_composition_coordinator_activate_frame_boundary(
            runner->coordinator,
            scratch->prepared_token,
            (MeisoTimePoint){77U, 130U},
            &scratch->active_world) !=
                MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED ||
        !meiso_composition_coordinator_active(
            runner->coordinator, &scratch->active_plan) ||
        !meiso_composition_scene_store_active(
            runner->scene_store, &scratch->active_view) ||
        !world_ref_equal(
            &scratch->active_world, &scratch->active_view.ref) ||
        scratch->active_plan.ref.plan_generation != 1U) {
        return false;
    }
    return true;
}

static bool render_and_verify(Runner *runner)
{
    RunnerScratch *scratch = runner->scratch;
    MeisoByteView thumbnail;

    if (!meiso_composition_scene_store_control_scope(
            runner->scene_store, &scratch->control_scope) ||
        meiso_composition_reference_renderer_begin_control_scope(
            runner->renderer, &scratch->control_scope) !=
                MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED ||
        meiso_composition_reference_renderer_submit(
            runner->renderer,
            &scratch->control_scope,
            &scratch->active_view,
            77U,
            &scratch->job) !=
                MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED ||
        meiso_composition_reference_renderer_render_next(
            runner->renderer,
            &scratch->job,
            200U,
            210U,
            &scratch->complete) !=
                MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE) {
        return false;
    }
    thumbnail = (MeisoByteView){
        scratch->complete.result.thumbnail_rgb565,
        scratch->complete.result.thumbnail_byte_size
    };
    sha256_bytes(
        &scratch->sha, scratch->complete.color_rgba8888,
        scratch->color_sha256
    );
    sha256_bytes(
        &scratch->sha, scratch->complete.depth_implementation_32,
        scratch->depth_sha256
    );
    sha256_bytes(
        &scratch->sha, thumbnail, scratch->thumbnail_sha256
    );
    if (scratch->complete.result.evidence_flags !=
            UINT32_C(0x00000003) ||
        memcmp(scratch->color_sha256, GOLDEN_COLOR_SHA256,
               MEISO_SHA256_DIGEST_SIZE) != 0 ||
        memcmp(scratch->depth_sha256, GOLDEN_DEPTH_SHA256,
               MEISO_SHA256_DIGEST_SIZE) != 0 ||
        memcmp(scratch->thumbnail_sha256, GOLDEN_THUMBNAIL_SHA256,
               MEISO_SHA256_DIGEST_SIZE) != 0 ||
        memcmp(scratch->complete.result.full_target_sha256,
               GOLDEN_COLOR_SHA256, MEISO_SHA256_DIGEST_SIZE) != 0 ||
        !world_ref_equal(
            &scratch->complete.job.world, &scratch->active_world) ||
        scratch->complete.result.object_count != 1U ||
        scratch->complete.result.unique_mesh_count != 1U ||
        !meiso_composition_reference_renderer_snapshot(
            runner->renderer, &scratch->renderer_snapshot) ||
        scratch->renderer_snapshot.queued_jobs != 0U ||
        !scratch->renderer_snapshot.complete_available ||
        scratch->renderer_snapshot.target_slots_allocated != 2U ||
        scratch->renderer_snapshot.target_bytes !=
            MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES ||
        meiso_blob_store_count(runner->blob_store) != 1U ||
        !meiso_blob_store_stats(
            runner->blob_store, &scratch->blob_stats) ||
        !meiso_composition_scene_store_stats(
            runner->scene_store, &scratch->scene_stats) ||
        !meiso_composition_coordinator_stats(
            runner->coordinator, &scratch->coordinator_stats) ||
        !meiso_composition_reference_renderer_stats(
            runner->renderer, &scratch->renderer_stats) ||
        scratch->blob_stats.publications != 1U ||
        scratch->scene_stats.activations != 1U ||
        scratch->coordinator_stats.activations != 1U ||
        scratch->renderer_stats.renders_completed != 1U) {
        return false;
    }
    digest_hex(scratch->color_sha256, scratch->color_hex);
    digest_hex(scratch->depth_sha256, scratch->depth_hex);
    digest_hex(scratch->thumbnail_sha256, scratch->thumbnail_hex);
    return true;
}

/*
 * A FILE writer is expected to accept each fixed, bounded line in full.
 * EOF is detected and reported, but stdio partial writes are not rolled back.
 */
static bool write_line(FILE *output, const char *line)
{
    return output != NULL && line != NULL &&
        fputs(line, output) >= 0 && fputc('\n', output) != EOF;
}

static bool format_and_write(
    FILE *output,
    char *line,
    size_t line_size,
    const char *format,
    size_t first,
    size_t second,
    size_t third,
    const char *name,
    const char *domain
)
{
    const int written = snprintf(
        line, line_size, format, name, first, second, domain, third
    );

    return written >= 0 && (size_t)written < line_size &&
        write_line(output, line);
}

static bool write_receipt(Runner *runner, FILE *output)
{
    RunnerScratch *scratch = runner->scratch;
    size_t index;
    size_t subtotal = 0U;
    int written;

    written = snprintf(
        scratch->line, sizeof(scratch->line),
        "{\"type\":\"composition_offscreen\",\"schema\":1,"
        "\"verification\":\"%s\",\"build_target\":\"%s\","
        "\"contract\":\"composition-scene\","
        "\"scenario\":\"baseline_cube_once\"}",
        RUNNER_VERIFICATION_LABEL, RUNNER_BUILD_TARGET
    );
    if (written < 0 || (size_t)written >= sizeof(scratch->line) ||
        !write_line(output, scratch->line)) {
        return false;
    }
    for (index = 0U; index < RUNNER_ARENA_COUNT; ++index) {
        if (!checked_add_size(
                subtotal, runner->arenas[index].memory_required, &subtotal
            ) ||
            !format_and_write(
                output,
                scratch->line,
                sizeof(scratch->line),
                "{\"type\":\"arena\",\"name\":\"%s\","
                "\"memory_required\":%zu,\"alignment\":%zu,"
                "\"domain\":\"%s\",\"subtotal\":%zu}",
                runner->arenas[index].memory_required,
                runner->arenas[index].alignment,
                subtotal,
                runner->arenas[index].name,
                runner->arenas[index].domain)) {
            return false;
        }
    }
    written = snprintf(
        scratch->line, sizeof(scratch->line),
        "{\"type\":\"logical_mesh\",\"published_blob_count\":1,"
        "\"published_blob_bytes\":%zu,\"resident_mesh_bytes\":%" PRIu64 ","
        "\"mesh_header_in_resident\":false}",
        scratch->cube_wire.size, scratch->active_view.resident_mesh_bytes
    );
    if (written < 0 || (size_t)written >= sizeof(scratch->line) ||
        !write_line(output, scratch->line)) {
        return false;
    }
    written = snprintf(
        scratch->line, sizeof(scratch->line),
        "{\"type\":\"render\",\"flags\":3,\"objects\":1,"
        "\"unique_meshes\":1,\"target_slots\":2,"
        "\"target_bytes\":%u,\"color_sha256\":\"%s\","
        "\"depth_sha256\":\"%s\",\"thumbnail_sha256\":\"%s\"}",
        (unsigned)MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES,
        scratch->color_hex, scratch->depth_hex, scratch->thumbnail_hex
    );
    if (written < 0 || (size_t)written >= sizeof(scratch->line) ||
        !write_line(output, scratch->line) ||
        !write_line(
            output,
            "{\"type\":\"limitations\",\"network\":false,"
            "\"driver\":false,\"stream\":false,\"runtime\":false,"
            "\"session\":false,\"state\":false,\"scheduler\":false,"
            "\"offline_lease_execution\":false,"
            "\"full_lifecycle\":false,"
            "\"geometry_correct\":false,\"physical\":false,"
            "\"performance\":false}")) {
        return false;
    }
    return fflush(output) == 0;
}

static bool write_lifecycle(Runner *runner, FILE *output)
{
    const uint64_t successful_allocations_after_enable =
        runner->allocator.successful_allocations -
        runner->allocator.allocations_at_enable;
    const uint64_t frees_after_enable =
        runner->allocator.free_calls - runner->allocator.frees_at_enable;
    const int written = snprintf(
        runner->final_line, sizeof(runner->final_line),
        "{\"type\":\"allocator_lifecycle\","
        "\"scope\":\"runner_checked_allocator\","
        "\"allocations_at_enable\":%" PRIu64
        ",\"frees_at_enable\":%" PRIu64 ",\"allocation_calls\":%" PRIu64
        ",\"successful_allocations\":%" PRIu64
        ",\"successful_allocations_after_enable\":%" PRIu64
        ",\"allocation_attempts_after_enable\":%" PRIu64
        ",\"realloc_calls\":%" PRIu64 ",\"free_calls\":%" PRIu64
        ",\"frees_after_enable\":%" PRIu64 "}",
        runner->allocator.allocations_at_enable,
        runner->allocator.frees_at_enable,
        runner->allocator.allocation_calls,
        runner->allocator.successful_allocations,
        successful_allocations_after_enable,
        runner->allocator.allocation_attempts_after_enable,
        runner->allocator.realloc_calls,
        runner->allocator.free_calls,
        frees_after_enable
    );

    return written >= 0 && (size_t)written < sizeof(runner->final_line) &&
        write_line(output, runner->final_line) && fflush(output) == 0;
}

static void fixed_error(FILE *error_output, const char *code)
{
    if (error_output != NULL && code != NULL) {
        (void)fputs(code, error_output);
        (void)fputc('\n', error_output);
    }
}

int meiso_composition_offscreen_runner_run(
    FILE *output,
    FILE *error_output
)
{
    Runner runner;
    bool receipt_written;

    if (output == NULL || error_output == NULL || output == error_output) {
        fixed_error(error_output, "E_ARGUMENT");
        return 2;
    }
    memset(&runner, 0, sizeof(runner));
    runner.host_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 0x7001U};
    runner.edge_owner = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 0x8001U};
    runner.session_id = (MeisoSessionId){
        {UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222)},
        {UINT64_C(0x3333333333333333), UINT64_C(0x4444444444444444)}
    };
    if (!allocate_all_arenas(&runner)) {
        runner_cleanup(&runner);
        fixed_error(error_output, "E_ALLOC");
        return 3;
    }
    if (!initialize_handles(&runner)) {
        runner_cleanup(&runner);
        fixed_error(error_output, "E_INIT");
        return 3;
    }
    if (!publish_cube(&runner) || !prepare_and_activate(&runner)) {
        runner_cleanup(&runner);
        fixed_error(error_output, "E_FLOW");
        return 3;
    }
    if (!render_and_verify(&runner)) {
        runner_cleanup(&runner);
        fixed_error(error_output, "E_VERIFY");
        return 3;
    }
    receipt_written = write_receipt(&runner, output);
    runner_cleanup(&runner);
    if (!receipt_written || !write_lifecycle(&runner, output)) {
        fixed_error(error_output, "E_WRITE");
        return 4;
    }
    return 0;
}

#ifndef MEISO_COMPOSITION_OFFSCREEN_RUNNER_NO_MAIN
int main(void)
{
    return meiso_composition_offscreen_runner_run(stdout, stderr);
}
#endif
