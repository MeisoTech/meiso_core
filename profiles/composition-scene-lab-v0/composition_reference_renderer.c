#include "composition_reference_renderer.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    RENDERER_MAGIC = 0x43525230U,
    TARGET_SLOT_COUNT = MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT,
    JOB_SLOT_COUNT = MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS
};

typedef struct {
    bool present;
    MeisoCompositionReferenceRendererJobRef ref;
    uint64_t source_clock_epoch;
    MeisoCompositionSceneDesired desired;
    size_t canonical_size;
    uint8_t canonical[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint32_t unique_mesh_count;
    MeisoContentId unique_meshes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
    uint32_t unique_mesh_sizes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
} ReferenceJob;

struct MeisoCompositionReferenceRenderer {
    uint32_t magic;
    size_t arena_size;
    MeisoBlobStore *blob_store;
    uint8_t *colors[TARGET_SLOT_COUNT];
    uint8_t *depths[TARGET_SLOT_COUNT];
    uint64_t renderer_generation;
    uint64_t job_id_high_water;
    bool scope_bound;
    MeisoCompositionSceneControlScope scope;
    ReferenceJob jobs[JOB_SLOT_COUNT];
    ReferenceJob validation_scratch;
    MeisoCompositionSceneRenderResult result_scratch;
    uint32_t queue_head;
    uint32_t queue_count;
    bool complete_available;
    uint32_t complete_slot;
    MeisoCompositionReferenceRendererJobRef latest_complete_job;
    MeisoCompositionSceneRenderResult latest_result;
    MeisoCompositionReferenceRendererStats stats;
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} ReferenceSha256;

_Static_assert(TARGET_SLOT_COUNT == 2U, "reference renderer requires 2 targets");
_Static_assert(JOB_SLOT_COUNT == 2U, "reference renderer requires 2 jobs");
_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_EVIDENCE_FLAGS ==
               UINT32_C(0x00000003), "reference evidence must be exact 0x3");
_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES ==
               UINT32_C(1228800), "reference target allocation changed");

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

static size_t align_up(size_t value, size_t alignment)
{
    const size_t remainder = value % alignment;

    return remainder == 0U ? value : value + alignment - remainder;
}

static bool scope_valid(const MeisoCompositionSceneControlScope *scope)
{
    return scope != NULL && scope->desired_owner_incarnation != 0U &&
        scope->control_scope_generation != 0U;
}

static bool scope_equal(
    const MeisoCompositionSceneControlScope *lhs,
    const MeisoCompositionSceneControlScope *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
        lhs->control_scope_generation == rhs->control_scope_generation;
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

static bool job_ref_equal(
    const MeisoCompositionReferenceRendererJobRef *lhs,
    const MeisoCompositionReferenceRendererJobRef *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->renderer_generation == rhs->renderer_generation &&
        lhs->job_id == rhs->job_id &&
        scope_equal(&lhs->control_scope, &rhs->control_scope) &&
        world_ref_equal(&lhs->world, &rhs->world);
}

static bool content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        meiso_content_id_equal(lhs, rhs);
}

static bool renderer_overlaps(
    const MeisoCompositionReferenceRenderer *renderer,
    const void *value,
    size_t value_size
)
{
    return renderer != NULL &&
        ranges_overlap(renderer, renderer->arena_size, value, value_size);
}

static bool renderer_valid(
    const MeisoCompositionReferenceRenderer *renderer
)
{
    MeisoBlobStats ignored;
    const size_t first_target = align_up(
        sizeof(*renderer), meiso_composition_reference_renderer_alignment_required()
    );
    const uint8_t *base = (const uint8_t *)renderer;
    size_t offset = first_target;
    uint32_t slot;

    if (renderer == NULL || renderer->magic != RENDERER_MAGIC ||
        renderer->arena_size <
            meiso_composition_reference_renderer_memory_required() ||
        renderer->blob_store == NULL ||
        renderer->queue_head >= JOB_SLOT_COUNT ||
        renderer->queue_count > JOB_SLOT_COUNT ||
        renderer->complete_slot >= TARGET_SLOT_COUNT ||
        ((renderer->renderer_generation == 0U) != !renderer->scope_bound) ||
        (renderer->scope_bound && !scope_valid(&renderer->scope))) {
        return false;
    }
    for (slot = 0U; slot < TARGET_SLOT_COUNT; ++slot) {
        if (renderer->colors[slot] != base + offset) {
            return false;
        }
        offset += MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE;
        if (renderer->depths[slot] != base + offset) {
            return false;
        }
        offset += MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE;
    }
    return offset <= renderer->arena_size &&
        meiso_blob_store_stats(renderer->blob_store, &ignored);
}

static ReferenceJob *queue_head_job(
    MeisoCompositionReferenceRenderer *renderer
)
{
    return renderer->queue_count == 0U ? NULL :
        &renderer->jobs[renderer->queue_head];
}

static const ReferenceJob *const_queue_head_job(
    const MeisoCompositionReferenceRenderer *renderer
)
{
    return renderer->queue_count == 0U ? NULL :
        &renderer->jobs[renderer->queue_head];
}

static void clear_jobs(MeisoCompositionReferenceRenderer *renderer)
{
    memset(renderer->jobs, 0, sizeof(renderer->jobs));
    renderer->queue_head = 0U;
    renderer->queue_count = 0U;
}

static void pop_head(MeisoCompositionReferenceRenderer *renderer)
{
    memset(&renderer->jobs[renderer->queue_head], 0,
           sizeof(renderer->jobs[renderer->queue_head]));
    renderer->queue_head = (renderer->queue_head + 1U) % JOB_SLOT_COUNT;
    --renderer->queue_count;
    if (renderer->queue_count == 0U) {
        renderer->queue_head = 0U;
    }
}

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

static void sha256_transform(ReferenceSha256 *sha, const uint8_t block[64])
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
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = read_be_u32(block + (size_t)index * 4U);
    }
    for (index = 16U; index < 64U; ++index) {
        const uint32_t x = words[index - 15U];
        const uint32_t y = words[index - 2U];
        const uint32_t small0 = rotate_right(x, 7U) ^
            rotate_right(x, 18U) ^ (x >> 3U);
        const uint32_t small1 = rotate_right(y, 17U) ^
            rotate_right(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + small0 +
            words[index - 7U] + small1;
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
            constants[index] + words[index];
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

static void sha256_init(ReferenceSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(
    ReferenceSha256 *sha,
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
    ReferenceSha256 *sha,
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

static uint64_t fnv1a_update(
    uint64_t hash,
    const uint8_t *data,
    size_t size
)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        hash ^= data[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30U;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27U;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static void synthetic_pixel(
    uint64_t scene_seed,
    uint32_t x,
    uint32_t y,
    uint8_t rgba[4],
    uint32_t *depth
)
{
    const uint64_t mixed = mix64(
        scene_seed ^ ((uint64_t)x * UINT64_C(0x9e3779b185ebca87)) ^
        ((uint64_t)y * UINT64_C(0xc2b2ae3d27d4eb4f))
    );

    rgba[0] = (uint8_t)(mixed >> 8U);
    rgba[1] = (uint8_t)(mixed >> 24U);
    rgba[2] = (uint8_t)(mixed >> 40U);
    rgba[3] = 0xffU;
    *depth = (uint32_t)mix64(mixed ^ UINT64_C(0x4450544833320000));
}

static MeisoCompositionReferenceRendererSubmitStatus validate_world(
    const MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    ReferenceJob *out_job
)
{
    uint32_t object_index;
    uint64_t resident_bytes = 0U;
    size_t canonical_size = 0U;

    memset(out_job, 0, sizeof(*out_job));
    if (world->desired == NULL || world->ref.desired_owner_incarnation == 0U ||
        world->ref.desired_state_version == 0U ||
        world->ref.plan_generation == 0U ||
        world->ref.world_generation == 0U ||
        world->ref.desired_owner_incarnation !=
            scope->desired_owner_incarnation ||
        world->object_count != world->desired->object_count ||
        world->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS ||
        world->unique_mesh_count >
            MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES ||
        meiso_composition_scene_desired_encode(
            world->desired,
            (MeisoMutableBuffer){out_job->canonical,
                                 sizeof(out_job->canonical)},
            &canonical_size) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
    }
    out_job->desired = *world->desired;
    out_job->canonical_size = canonical_size;
    for (object_index = 0U; object_index < world->object_count;
         ++object_index) {
        const MeisoContentId *content_id =
            &world->desired->objects[object_index].mesh_content_id;
        uint32_t unique_index;
        bool seen = false;

        for (unique_index = 0U;
             unique_index < out_job->unique_mesh_count;
             ++unique_index) {
            if (content_id_equal(
                    content_id, &out_job->unique_meshes[unique_index])) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            MeisoBlobView blob;
            MeisoCompositionSceneMeshView mesh;
            size_t mesh_resident;

            if (out_job->unique_mesh_count >=
                    MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
                return
                    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
            }
            if (!meiso_blob_store_lookup(
                    renderer->blob_store, content_id, &blob)) {
                return
                    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_MISSING_BLOB;
            }
            if (!content_id_equal(content_id, &blob.content_id) ||
                blob.content.size > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
                !meiso_composition_scene_mesh_blob_verify(
                    content_id, blob.content, NULL) ||
                meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                    MEISO_COMPOSITION_SCENE_CODEC_OK ||
                mesh.vertex_records.size >
                    SIZE_MAX - mesh.index_records.size) {
                return
                    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_MESH;
            }
            mesh_resident = mesh.vertex_records.size +
                mesh.index_records.size;
            if (resident_bytes >
                    MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD -
                        mesh_resident) {
                return
                    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
            }
            unique_index = out_job->unique_mesh_count++;
            out_job->unique_meshes[unique_index] = *content_id;
            out_job->unique_mesh_sizes[unique_index] =
                (uint32_t)blob.content.size;
            resident_bytes += mesh_resident;
        }
    }
    if (out_job->unique_mesh_count != world->unique_mesh_count ||
        resident_bytes != world->resident_mesh_bytes) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
    }
    return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED;
}

static MeisoCompositionReferenceRendererRenderStatus job_seed(
    const MeisoCompositionReferenceRenderer *renderer,
    const ReferenceJob *job,
    uint64_t *out_seed
)
{
    static const uint8_t domain[] = {
        'M', 'E', 'I', 'S', 'O', '_', 'C', 'O', 'M', 'P', '_', 'R', 'E',
        'F', 'E', 'R', 'E', 'N', 'C', 'E', '_', 'R', 'E', 'N', 'D', 'E',
        'R', '_', 'V', '0', 0U
    };
    uint64_t seed = UINT64_C(14695981039346656037);
    uint32_t index;

    seed = fnv1a_update(seed, domain, sizeof(domain));
    seed = fnv1a_update(seed, job->canonical, job->canonical_size);
    for (index = 0U; index < job->unique_mesh_count; ++index) {
        MeisoBlobView blob;
        uint8_t size_be[8];
        uint64_t size = job->unique_mesh_sizes[index];
        size_t byte_index;

        if (!meiso_blob_store_lookup(
                renderer->blob_store, &job->unique_meshes[index], &blob)) {
            return
                MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_MISSING_BLOB;
        }
        if (!content_id_equal(&job->unique_meshes[index], &blob.content_id) ||
            blob.content.size != job->unique_mesh_sizes[index] ||
            !meiso_composition_scene_mesh_blob_verify(
                &job->unique_meshes[index], blob.content, NULL)) {
            return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_MESH;
        }
        for (byte_index = 0U; byte_index < sizeof(size_be); ++byte_index) {
            size_be[byte_index] =
                (uint8_t)(size >> (56U - byte_index * 8U));
        }
        seed = fnv1a_update(seed, size_be, sizeof(size_be));
        seed = fnv1a_update(seed, blob.content.data, blob.content.size);
    }
    *out_seed = seed;
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE;
}

static void make_complete_view(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete
)
{
    memset(out_complete, 0, sizeof(*out_complete));
    out_complete->job = renderer->latest_complete_job;
    out_complete->result = renderer->latest_result;
    out_complete->target_slot = renderer->complete_slot;
    out_complete->color_rgba8888.data =
        renderer->colors[renderer->complete_slot];
    out_complete->color_rgba8888.size =
        MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE;
    out_complete->depth_implementation_32.data =
        renderer->depths[renderer->complete_slot];
    out_complete->depth_implementation_32.size =
        MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE;
}

size_t meiso_composition_reference_renderer_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_composition_reference_renderer_memory_required(void)
{
    const size_t alignment =
        meiso_composition_reference_renderer_alignment_required();
    const size_t base_size = align_up(
        sizeof(MeisoCompositionReferenceRenderer), alignment
    );

    return base_size + MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
}

MeisoCompositionReferenceRendererInitStatus
meiso_composition_reference_renderer_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionReferenceRenderer **out_renderer
)
{
    MeisoCompositionReferenceRenderer *renderer;
    MeisoBlobStats ignored;
    size_t offset;
    uint32_t slot;

    if (arena == NULL || out_renderer == NULL ||
        ranges_overlap(arena, arena_size,
                       out_renderer, sizeof(*out_renderer))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_ARGUMENT;
    }
    if (blob_store == NULL || !meiso_blob_store_stats(blob_store, &ignored)) {
        return
            MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_BLOB_STORE;
    }
    if ((uintptr_t)arena %
            meiso_composition_reference_renderer_alignment_required() != 0U) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < meiso_composition_reference_renderer_memory_required()) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_ARENA_TOO_SMALL;
    }
    *out_renderer = NULL;
    memset(arena, 0, arena_size);
    renderer = (MeisoCompositionReferenceRenderer *)arena;
    renderer->magic = RENDERER_MAGIC;
    renderer->arena_size = arena_size;
    renderer->blob_store = blob_store;
    renderer->complete_slot = 0U;
    offset = align_up(
        sizeof(*renderer),
        meiso_composition_reference_renderer_alignment_required()
    );
    for (slot = 0U; slot < TARGET_SLOT_COUNT; ++slot) {
        renderer->colors[slot] = (uint8_t *)arena + offset;
        offset += MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE;
        renderer->depths[slot] = (uint8_t *)arena + offset;
        offset += MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE;
    }
    *out_renderer = renderer;
    return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_OK;
}

MeisoCompositionReferenceRendererScopeStatus
meiso_composition_reference_renderer_begin_control_scope(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope
)
{
    uint32_t slot;

    if (!renderer_valid(renderer) || !scope_valid(scope) ||
        renderer_overlaps(renderer, scope, sizeof(*scope))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_INVALID_ARGUMENT;
    }
    if (renderer->scope_bound && scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_UNCHANGED;
    }
    if (renderer->renderer_generation == UINT64_MAX) {
        return
            MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_COUNTER_EXHAUSTED;
    }
    clear_jobs(renderer);
    for (slot = 0U; slot < TARGET_SLOT_COUNT; ++slot) {
        memset(renderer->colors[slot], 0,
               MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE);
        memset(renderer->depths[slot], 0,
               MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE);
    }
    renderer->complete_available = false;
    renderer->complete_slot = 0U;
    memset(&renderer->latest_complete_job, 0,
           sizeof(renderer->latest_complete_job));
    memset(&renderer->latest_result, 0, sizeof(renderer->latest_result));
    memset(&renderer->validation_scratch, 0,
           sizeof(renderer->validation_scratch));
    memset(&renderer->result_scratch, 0, sizeof(renderer->result_scratch));
    renderer->scope = *scope;
    renderer->scope_bound = true;
    ++renderer->renderer_generation;
    increment_saturated(&renderer->stats.resets);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED;
}

MeisoCompositionReferenceRendererSubmitStatus
meiso_composition_reference_renderer_submit(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    uint64_t source_clock_epoch,
    MeisoCompositionReferenceRendererJobRef *out_job
)
{
    ReferenceJob *candidate;
    MeisoCompositionReferenceRendererSubmitStatus status;
    uint32_t tail;

    if (!renderer_valid(renderer) || scope == NULL || world == NULL ||
        world->desired == NULL || out_job == NULL ||
        source_clock_epoch == 0U ||
        renderer_overlaps(renderer, scope, sizeof(*scope)) ||
        renderer_overlaps(renderer, world, sizeof(*world)) ||
        renderer_overlaps(renderer, world->desired, sizeof(*world->desired)) ||
        renderer_overlaps(renderer, out_job, sizeof(*out_job)) ||
        ranges_overlap(scope, sizeof(*scope), out_job, sizeof(*out_job)) ||
        ranges_overlap(world, sizeof(*world), out_job, sizeof(*out_job)) ||
        ranges_overlap(world->desired, sizeof(*world->desired),
                       out_job, sizeof(*out_job))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_SCOPE_MISMATCH;
    }
    if (renderer->queue_count == JOB_SLOT_COUNT) {
        increment_saturated(&renderer->stats.queue_capacity_rejections);
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUE_FULL;
    }
    if (renderer->job_id_high_water == UINT64_MAX) {
        return
            MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_COUNTER_EXHAUSTED;
    }
    candidate = &renderer->validation_scratch;
    status = validate_world(renderer, scope, world, candidate);
    if (status != MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED) {
        if (status ==
                MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_MISSING_BLOB) {
            increment_saturated(&renderer->stats.missing_blob_rejections);
        } else if (status ==
                       MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_MESH) {
            increment_saturated(&renderer->stats.invalid_mesh_rejections);
        } else {
            increment_saturated(&renderer->stats.invalid_world_rejections);
        }
        return status;
    }
    candidate->present = true;
    candidate->source_clock_epoch = source_clock_epoch;
    candidate->ref.renderer_generation = renderer->renderer_generation;
    candidate->ref.job_id = renderer->job_id_high_water + 1U;
    candidate->ref.control_scope = renderer->scope;
    candidate->ref.world = world->ref;
    tail = (renderer->queue_head + renderer->queue_count) % JOB_SLOT_COUNT;
    renderer->jobs[tail] = *candidate;
    ++renderer->queue_count;
    renderer->job_id_high_water = candidate->ref.job_id;
    increment_saturated(&renderer->stats.jobs_submitted);
    *out_job = candidate->ref;
    return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED;
}

MeisoCompositionReferenceRendererRenderStatus
meiso_composition_reference_renderer_render_next(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionReferenceRendererCompleteView *out_complete
)
{
    ReferenceJob *job;
    MeisoCompositionReferenceRendererRenderStatus seed_status;
    MeisoCompositionSceneRenderResult *result;
    ReferenceSha256 sha;
    uint64_t seed;
    uint32_t target_slot;
    uint32_t y;

    if (!renderer_valid(renderer) || expected_job == NULL ||
        out_complete == NULL ||
        renderer_overlaps(renderer, expected_job, sizeof(*expected_job)) ||
        renderer_overlaps(renderer, out_complete, sizeof(*out_complete)) ||
        ranges_overlap(expected_job, sizeof(*expected_job),
                       out_complete, sizeof(*out_complete))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_ARGUMENT;
    }
    job = queue_head_job(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB;
    }
    if (!job_ref_equal(&job->ref, expected_job)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH;
    }
    if (render_end_monotonic_ns < render_start_monotonic_ns) {
        increment_saturated(&renderer->stats.render_failures);
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_TIME;
    }
    seed_status = job_seed(renderer, job, &seed);
    if (seed_status != MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE) {
        increment_saturated(&renderer->stats.render_failures);
        return seed_status;
    }

    target_slot = renderer->complete_available ?
        (renderer->complete_slot == 0U ? 1U : 0U) : 0U;
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_TARGET_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_TARGET_WIDTH; ++x) {
            const size_t pixel =
                ((size_t)y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH + x) * 4U;
            uint8_t rgba[4];
            uint32_t depth;

            synthetic_pixel(seed, x, y, rgba, &depth);
            memcpy(renderer->colors[target_slot] + pixel, rgba, sizeof(rgba));
            write_be_u32(renderer->depths[target_slot] + pixel, depth);
        }
    }

    result = &renderer->result_scratch;
    memset(result, 0, sizeof(*result));
    result->applied_scene_owner_incarnation =
        job->ref.world.desired_owner_incarnation;
    result->applied_scene_state_version =
        job->ref.world.desired_state_version;
    result->active_plan_generation = job->ref.world.plan_generation;
    result->world_generation = job->ref.world.world_generation;
    result->result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    result->evidence_flags =
        MEISO_COMPOSITION_REFERENCE_RENDERER_EVIDENCE_FLAGS;
    result->object_count = job->desired.object_count;
    result->unique_mesh_count = job->unique_mesh_count;
    result->source_clock_epoch = job->source_clock_epoch;
    result->render_start_monotonic_ns = render_start_monotonic_ns;
    result->render_end_monotonic_ns = render_end_monotonic_ns;
    result->target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    result->target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    result->color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    result->depth_format = MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    result->logical_color_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->logical_depth_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    result->thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    result->thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    result->thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U;
    result->thumbnail_byte_size =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;

    sha256_init(&sha);
    sha256_update(
        &sha, renderer->colors[target_slot],
        MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE
    );
    sha256_final(&sha, result->full_target_sha256);
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH; ++x) {
            const uint32_t source_x = x * 5U + 2U;
            const uint32_t source_y = y * 5U + 2U;
            const size_t source =
                ((size_t)source_y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
                 source_x) * 4U;
            const size_t output =
                ((size_t)y * MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH + x) *
                2U;
            const uint8_t *rgba = renderer->colors[target_slot] + source;
            const uint16_t rgb565 = (uint16_t)(
                ((uint16_t)(rgba[0] >> 3U) << 11U) |
                ((uint16_t)(rgba[1] >> 2U) << 5U) |
                (uint16_t)(rgba[2] >> 3U)
            );

            result->thumbnail_rgb565[output] = (uint8_t)(rgb565 >> 8U);
            result->thumbnail_rgb565[output + 1U] = (uint8_t)rgb565;
        }
    }

    renderer->complete_slot = target_slot;
    renderer->complete_available = true;
    renderer->latest_complete_job = job->ref;
    renderer->latest_result = *result;
    pop_head(renderer);
    increment_saturated(&renderer->stats.renders_completed);
    make_complete_view(renderer, out_complete);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE;
}

MeisoCompositionReferenceRendererDiscardStatus
meiso_composition_reference_renderer_discard_head(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job
)
{
    const ReferenceJob *job;

    if (!renderer_valid(renderer) || expected_job == NULL ||
        renderer_overlaps(renderer, expected_job, sizeof(*expected_job))) {
        return
            MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_INVALID_ARGUMENT;
    }
    job = const_queue_head_job(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_NO_JOB;
    }
    if (!job_ref_equal(&job->ref, expected_job)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_JOB_MISMATCH;
    }
    pop_head(renderer);
    increment_saturated(&renderer->stats.jobs_discarded);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED;
}

MeisoCompositionReferenceRendererRetireStatus
meiso_composition_reference_renderer_retire_world(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *retired_world
)
{
    uint32_t offset = 0U;
    uint64_t jobs_retired = 0U;
    bool complete_retired = false;

    if (!renderer_valid(renderer) || !scope_valid(scope) ||
        retired_world == NULL ||
        retired_world->desired_owner_incarnation == 0U ||
        retired_world->desired_state_version == 0U ||
        retired_world->plan_generation == 0U ||
        retired_world->world_generation == 0U ||
        renderer_overlaps(renderer, scope, sizeof(*scope)) ||
        renderer_overlaps(renderer, retired_world, sizeof(*retired_world)) ||
        ranges_overlap(scope, sizeof(*scope),
                       retired_world, sizeof(*retired_world))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope) ||
        retired_world->desired_owner_incarnation !=
            scope->desired_owner_incarnation) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_SCOPE_MISMATCH;
    }

    while (offset < renderer->queue_count) {
        const uint32_t index =
            (renderer->queue_head + offset) % JOB_SLOT_COUNT;

        if (world_ref_equal(&renderer->jobs[index].ref.world,
                            retired_world)) {
            if (offset == 0U) {
                pop_head(renderer);
            } else {
                memset(&renderer->jobs[index], 0,
                       sizeof(renderer->jobs[index]));
                --renderer->queue_count;
                if (renderer->queue_count == 0U) {
                    renderer->queue_head = 0U;
                }
            }
            ++jobs_retired;
        } else {
            ++offset;
        }
    }
    if (renderer->complete_available &&
        world_ref_equal(&renderer->latest_complete_job.world,
                        retired_world)) {
        memset(renderer->colors[renderer->complete_slot], 0,
               MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE);
        memset(renderer->depths[renderer->complete_slot], 0,
               MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE);
        renderer->complete_available = false;
        renderer->complete_slot = 0U;
        memset(&renderer->latest_complete_job, 0,
               sizeof(renderer->latest_complete_job));
        memset(&renderer->latest_result, 0, sizeof(renderer->latest_result));
        complete_retired = true;
    }
    if (jobs_retired == 0U && !complete_retired) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_UNCHANGED;
    }
    increment_saturated(&renderer->stats.world_retires);
    while (jobs_retired != 0U) {
        increment_saturated(&renderer->stats.jobs_retired);
        --jobs_retired;
    }
    if (complete_retired) {
        increment_saturated(&renderer->stats.completes_retired);
    }
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED;
}

bool meiso_composition_reference_renderer_latest_complete(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete
)
{
    if (!renderer_valid(renderer) || out_complete == NULL ||
        renderer_overlaps(renderer, out_complete, sizeof(*out_complete)) ||
        !renderer->complete_available) {
        return false;
    }
    make_complete_view(renderer, out_complete);
    return true;
}

bool meiso_composition_reference_renderer_snapshot(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererSnapshot *out_snapshot
)
{
    MeisoCompositionReferenceRendererSnapshot snapshot;
    const ReferenceJob *head;

    if (!renderer_valid(renderer) || out_snapshot == NULL ||
        renderer_overlaps(renderer, out_snapshot, sizeof(*out_snapshot))) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.renderer_generation = renderer->renderer_generation;
    snapshot.control_scope_bound = renderer->scope_bound;
    snapshot.control_scope = renderer->scope;
    snapshot.queued_jobs = renderer->queue_count;
    snapshot.target_slots_allocated = TARGET_SLOT_COUNT;
    snapshot.target_bytes =
        MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
    snapshot.renderer_metadata_bytes =
        renderer->arena_size -
        MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
    snapshot.complete_available = renderer->complete_available;
    snapshot.complete_target_slot = renderer->complete_available ?
        renderer->complete_slot : 0U;
    head = const_queue_head_job(renderer);
    if (head != NULL) {
        snapshot.head_job = head->ref;
    }
    if (renderer->complete_available) {
        snapshot.latest_complete_job = renderer->latest_complete_job;
    }
    *out_snapshot = snapshot;
    return true;
}

bool meiso_composition_reference_renderer_stats(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererStats *out_stats
)
{
    if (!renderer_valid(renderer) || out_stats == NULL ||
        renderer_overlaps(renderer, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = renderer->stats;
    return true;
}
