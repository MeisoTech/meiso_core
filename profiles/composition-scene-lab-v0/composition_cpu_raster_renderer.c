#include "composition_cpu_raster_renderer.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    RASTER_RENDERER_MAGIC = 0x43505230U,
    TARGET_SLOT_COUNT = MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT,
    JOB_SLOT_COUNT = MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS,
    CLIP_PLANE_COUNT = 6,
    /* A convex n-gon gains at most one vertex per clipping half-space. */
    CLIP_VERTEX_CAPACITY = 12,
    FIXED_SUBPIXEL_BITS = 8,
    FIXED_SUBPIXEL_SCALE = 1 << FIXED_SUBPIXEL_BITS,
    FIXED_PIXEL_CENTER = FIXED_SUBPIXEL_SCALE / 2
};

typedef struct {
    bool present;
    MeisoCompositionCpuRasterJobRef ref;
    uint64_t source_clock_epoch;
    MeisoCompositionSceneDesired desired;
    uint32_t unique_mesh_count;
    MeisoContentId unique_meshes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
    uint32_t unique_mesh_sizes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
} RasterJob;

struct MeisoCompositionCpuRasterRenderer {
    uint32_t magic;
    size_t arena_size;
    MeisoBlobStore *blob_store;
    uint8_t *colors[TARGET_SLOT_COUNT];
    uint8_t *depths[TARGET_SLOT_COUNT];
    uint64_t renderer_generation;
    uint64_t job_id_high_water;
    bool scope_bound;
    MeisoCompositionSceneControlScope scope;
    RasterJob jobs[JOB_SLOT_COUNT];
    RasterJob validation_scratch;
    uint8_t canonical_scratch[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoCompositionSceneRenderResult result_scratch;
    uint32_t queue_head;
    uint32_t queue_count;
    bool complete_available;
    uint32_t complete_slot;
    MeisoCompositionCpuRasterJobRef latest_complete_job;
    MeisoCompositionSceneRenderResult latest_result;
    MeisoCompositionCpuRasterStats stats;
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} RasterSha256;

typedef struct {
    double position[3];
    double orientation_xyzw[4];
    double focal_y;
    double focal_x;
    double projection_a;
    double projection_b;
} RasterCamera;

typedef struct {
    double clip[4];
    double color[4];
} ClipVertex;

typedef struct {
    int32_t x_fixed;
    int32_t y_fixed;
    double ndc_x;
    double ndc_y;
    double ndc_z;
    double inverse_w;
    double color_over_w[4];
} ScreenVertex;

typedef struct {
    uint64_t submitted;
    uint64_t clipped;
    uint64_t culled;
    uint64_t rasterized;
    uint64_t fragments_depth_passed;
} RasterWorkCounts;

_Static_assert(TARGET_SLOT_COUNT == 2U,
               "cpu raster renderer requires two targets");
_Static_assert(JOB_SLOT_COUNT == 2U,
               "cpu raster renderer requires two jobs");
_Static_assert(CLIP_VERTEX_CAPACITY >= 3 + CLIP_PLANE_COUNT,
               "clip polygon capacity is below the convex worst case");
_Static_assert(MEISO_COMPOSITION_CPU_RASTER_RENDERER_EVIDENCE_FLAGS ==
                   UINT32_C(0x00000003),
               "cpu raster evidence must be exact 0x3");
_Static_assert(MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES ==
                   UINT32_C(1228800),
               "cpu raster target allocation changed");

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

static void increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

static void add_saturated(uint64_t *value, uint64_t addition)
{
    if (addition > UINT64_MAX - *value) {
        *value = UINT64_MAX;
    } else {
        *value += addition;
    }
}

static bool add_checked(uint64_t *value, uint64_t addition)
{
    if (addition > UINT64_MAX - *value) {
        return false;
    }
    *value += addition;
    return true;
}

static bool platform_supported(void)
{
    return CHAR_BIT == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
        DBL_MANT_DIG == 53 && FLT_EVAL_METHOD == 0 &&
        sizeof(float) == 4U && sizeof(double) == 8U &&
        fegetround() == FE_TONEAREST && isfinite(0.0) != 0 &&
        isfinite(HUGE_VAL) == 0 && isnan(NAN) != 0;
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

static bool world_ref_valid(const MeisoCompositionSceneWorldRef *world)
{
    return world != NULL && world->desired_owner_incarnation != 0U &&
        world->desired_state_version != 0U && world->plan_generation != 0U &&
        world->world_generation != 0U;
}

static bool world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs
)
{
    return world_ref_valid(lhs) && world_ref_valid(rhs) &&
        lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
        lhs->desired_state_version == rhs->desired_state_version &&
        lhs->plan_generation == rhs->plan_generation &&
        lhs->world_generation == rhs->world_generation;
}

static bool job_ref_equal(
    const MeisoCompositionCpuRasterJobRef *lhs,
    const MeisoCompositionCpuRasterJobRef *rhs
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
    return lhs != NULL && rhs != NULL && meiso_content_id_equal(lhs, rhs);
}

static bool renderer_overlaps(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const void *value,
    size_t value_size
)
{
    return renderer != NULL &&
        ranges_overlap(renderer, renderer->arena_size, value, value_size);
}

static bool renderer_valid(
    const MeisoCompositionCpuRasterRenderer *renderer
)
{
    MeisoBlobStats ignored;
    const size_t first_target = align_up(
        sizeof(*renderer),
        meiso_composition_cpu_raster_renderer_alignment_required()
    );
    const uint8_t *base = (const uint8_t *)renderer;
    size_t offset = first_target;
    uint32_t slot;

    if (renderer == NULL || renderer->magic != RASTER_RENDERER_MAGIC ||
        renderer->arena_size <
            meiso_composition_cpu_raster_renderer_memory_required() ||
        renderer->blob_store == NULL || renderer->queue_head >= JOB_SLOT_COUNT ||
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
        offset += MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE;
        if (renderer->depths[slot] != base + offset) {
            return false;
        }
        offset += MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE;
    }
    return offset <= renderer->arena_size &&
        meiso_blob_store_stats(renderer->blob_store, &ignored);
}

static RasterJob *queue_head_job(MeisoCompositionCpuRasterRenderer *renderer)
{
    return renderer->queue_count == 0U ? NULL :
        &renderer->jobs[renderer->queue_head];
}

static const RasterJob *const_queue_head_job(
    const MeisoCompositionCpuRasterRenderer *renderer
)
{
    return renderer->queue_count == 0U ? NULL :
        &renderer->jobs[renderer->queue_head];
}

static void clear_jobs(MeisoCompositionCpuRasterRenderer *renderer)
{
    memset(renderer->jobs, 0, sizeof(renderer->jobs));
    renderer->queue_head = 0U;
    renderer->queue_count = 0U;
}

static void pop_head(MeisoCompositionCpuRasterRenderer *renderer)
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
        ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
}

static void write_be_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void sha256_transform(RasterSha256 *sha, const uint8_t block[64])
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

static void sha256_init(RasterSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(RasterSha256 *sha, const uint8_t *data, size_t size)
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
    RasterSha256 *sha,
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

static MeisoCompositionCpuRasterSubmitStatus validate_world(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    RasterJob *out_job
)
{
    uint32_t object_index;
    uint64_t resident_bytes = 0U;
    size_t canonical_size = 0U;

    memset(out_job, 0, sizeof(*out_job));
    if (world->desired == NULL || !world_ref_valid(&world->ref) ||
        world->ref.desired_owner_incarnation !=
            scope->desired_owner_incarnation ||
        world->object_count != world->desired->object_count ||
        world->object_count > MEISO_COMPOSITION_CPU_RASTER_MAX_OBJECTS ||
        world->unique_mesh_count >
            MEISO_COMPOSITION_CPU_RASTER_MAX_UNIQUE_MESHES ||
        meiso_composition_scene_desired_encode(
            world->desired,
            (MeisoMutableBuffer){
                renderer->canonical_scratch,
                sizeof(renderer->canonical_scratch)
            },
            &canonical_size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK || canonical_size == 0U) {
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_WORLD;
    }
    out_job->desired = *world->desired;
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
                    MEISO_COMPOSITION_CPU_RASTER_MAX_UNIQUE_MESHES) {
                return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_WORLD;
            }
            if (!meiso_blob_store_lookup(
                    renderer->blob_store, content_id, &blob)) {
                return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_MISSING_BLOB;
            }
            if (!content_id_equal(content_id, &blob.content_id) ||
                blob.content.size >
                    MEISO_COMPOSITION_CPU_RASTER_MAX_MESH_BLOB_SIZE ||
                !meiso_composition_scene_mesh_blob_verify(
                    content_id, blob.content, NULL) ||
                meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                    MEISO_COMPOSITION_SCENE_CODEC_OK ||
                mesh.vertex_records.size >
                    SIZE_MAX - mesh.index_records.size) {
                return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_MESH;
            }
            mesh_resident =
                mesh.vertex_records.size + mesh.index_records.size;
            if (mesh_resident >
                    MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD ||
                resident_bytes >
                    MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD -
                        mesh_resident) {
                return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_WORLD;
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
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_WORLD;
    }
    return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED;
}

static MeisoCompositionCpuRasterRenderStatus validate_job_blobs(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const RasterJob *job
)
{
    uint32_t index;

    for (index = 0U; index < job->unique_mesh_count; ++index) {
        MeisoBlobView blob;
        MeisoCompositionSceneMeshView mesh;

        if (!meiso_blob_store_lookup(
                renderer->blob_store, &job->unique_meshes[index], &blob)) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_MISSING_BLOB;
        }
        if (!content_id_equal(
                &job->unique_meshes[index], &blob.content_id) ||
            blob.content.size != job->unique_mesh_sizes[index] ||
            blob.content.size >
                MEISO_COMPOSITION_CPU_RASTER_MAX_MESH_BLOB_SIZE ||
            !meiso_composition_scene_mesh_blob_verify(
                &job->unique_meshes[index], blob.content, NULL) ||
            meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                MEISO_COMPOSITION_SCENE_CODEC_OK) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH;
        }
    }
    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE;
}

static bool expected_mesh_size(
    const RasterJob *job,
    const MeisoContentId *content_id,
    uint32_t *out_size
)
{
    uint32_t index;

    for (index = 0U; index < job->unique_mesh_count; ++index) {
        if (content_id_equal(content_id, &job->unique_meshes[index])) {
            *out_size = job->unique_mesh_sizes[index];
            return true;
        }
    }
    return false;
}

static bool finite_vector(const double *values, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (!isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

static void rotate_vector(
    const double quaternion[4],
    const double input[3],
    double output[3]
)
{
    const double tx = 2.0 *
        (quaternion[1] * input[2] - quaternion[2] * input[1]);
    const double ty = 2.0 *
        (quaternion[2] * input[0] - quaternion[0] * input[2]);
    const double tz = 2.0 *
        (quaternion[0] * input[1] - quaternion[1] * input[0]);

    output[0] = input[0] + quaternion[3] * tx +
        quaternion[1] * tz - quaternion[2] * ty;
    output[1] = input[1] + quaternion[3] * ty +
        quaternion[2] * tx - quaternion[0] * tz;
    output[2] = input[2] + quaternion[3] * tz +
        quaternion[0] * ty - quaternion[1] * tx;
}

static bool prepare_camera(
    const MeisoCompositionSceneCamera *source,
    RasterCamera *out_camera
)
{
    static const double degrees_to_radians =
        0.017453292519943295769236907684886;
    const double half_fov =
        (double)source->vertical_fov_degrees * degrees_to_radians * 0.5;
    const double tangent = tan(half_fov);
    const double near_z = (double)source->near_z;
    const double far_z = (double)source->far_z;
    size_t index;

    memset(out_camera, 0, sizeof(*out_camera));
    if (!isfinite(tangent) || tangent <= 0.0 || !isfinite(near_z) ||
        !isfinite(far_z) || near_z <= 0.0 || far_z <= near_z) {
        return false;
    }
    for (index = 0U; index < 3U; ++index) {
        out_camera->position[index] = source->position[index];
        out_camera->orientation_xyzw[index] =
            -(double)source->orientation_xyzw[index];
    }
    out_camera->orientation_xyzw[3] = source->orientation_xyzw[3];
    out_camera->focal_y = 1.0 / tangent;
    out_camera->focal_x = out_camera->focal_y *
        ((double)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT /
         (double)MEISO_COMPOSITION_SCENE_TARGET_WIDTH);
    out_camera->projection_a = (far_z + near_z) / (near_z - far_z);
    out_camera->projection_b =
        (2.0 * far_z * near_z) / (near_z - far_z);
    return finite_vector(out_camera->position, 3U) &&
        finite_vector(out_camera->orientation_xyzw, 4U) &&
        isfinite(out_camera->focal_x) && isfinite(out_camera->focal_y) &&
        isfinite(out_camera->projection_a) &&
        isfinite(out_camera->projection_b);
}

static bool make_clip_vertex(
    const RasterCamera *camera,
    const MeisoCompositionSceneObject *object,
    const MeisoCompositionSceneMeshVertex *vertex,
    bool has_vertex_color,
    ClipVertex *out_vertex
)
{
    double object_quaternion[4];
    double local[3];
    double world_offset[3];
    double world[3];
    double relative[3];
    double view[3];
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        local[index] =
            (double)vertex->position[index] * object->transform.scale[index];
        object_quaternion[index] = object->transform.orientation_xyzw[index];
    }
    object_quaternion[3] = object->transform.orientation_xyzw[3];
    if (!finite_vector(local, 3U) ||
        !finite_vector(object_quaternion, 4U)) {
        return false;
    }
    rotate_vector(object_quaternion, local, world_offset);
    for (index = 0U; index < 3U; ++index) {
        world[index] = world_offset[index] + object->transform.position[index];
        relative[index] = world[index] - camera->position[index];
    }
    rotate_vector(camera->orientation_xyzw, relative, view);
    out_vertex->clip[0] = camera->focal_x * view[0];
    out_vertex->clip[1] = camera->focal_y * view[1];
    out_vertex->clip[2] =
        camera->projection_a * view[2] + camera->projection_b;
    out_vertex->clip[3] = -view[2];
    for (index = 0U; index < 4U; ++index) {
        out_vertex->color[index] = has_vertex_color ?
            (double)vertex->color_rgba[index] : 255.0;
    }
    return finite_vector(out_vertex->clip, 4U) &&
        finite_vector(out_vertex->color, 4U);
}

static double clip_distance(const ClipVertex *vertex, uint32_t plane)
{
    switch (plane) {
        case 0U:
            return vertex->clip[0] + vertex->clip[3];
        case 1U:
            return vertex->clip[3] - vertex->clip[0];
        case 2U:
            return vertex->clip[1] + vertex->clip[3];
        case 3U:
            return vertex->clip[3] - vertex->clip[1];
        case 4U:
            return vertex->clip[2] + vertex->clip[3];
        default:
            return vertex->clip[3] - vertex->clip[2];
    }
}

static bool interpolate_clip_vertex(
    const ClipVertex *start,
    const ClipVertex *end,
    double start_distance,
    double end_distance,
    ClipVertex *out_vertex
)
{
    const double denominator = start_distance - end_distance;
    double t;
    size_t index;

    if (!isfinite(denominator) || denominator == 0.0) {
        return false;
    }
    t = start_distance / denominator;
    if (!isfinite(t)) {
        return false;
    }
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    for (index = 0U; index < 4U; ++index) {
        out_vertex->clip[index] =
            start->clip[index] + t * (end->clip[index] - start->clip[index]);
        out_vertex->color[index] = start->color[index] +
            t * (end->color[index] - start->color[index]);
    }
    return finite_vector(out_vertex->clip, 4U) &&
        finite_vector(out_vertex->color, 4U);
}

static bool clip_triangle(
    const ClipVertex input[3],
    ClipVertex output[CLIP_VERTEX_CAPACITY],
    uint32_t *out_count,
    bool *out_changed
)
{
    ClipVertex first[CLIP_VERTEX_CAPACITY];
    ClipVertex second[CLIP_VERTEX_CAPACITY];
    ClipVertex *current = first;
    ClipVertex *next = second;
    uint32_t current_count = 3U;
    uint32_t plane;
    bool changed = false;

    memcpy(first, input, sizeof(ClipVertex) * 3U);
    for (plane = 0U; plane < CLIP_PLANE_COUNT; ++plane) {
        uint32_t next_count = 0U;
        uint32_t index;

        if (current_count == 0U) {
            break;
        }
        for (index = 0U; index < current_count; ++index) {
            const ClipVertex *start =
                &current[(index + current_count - 1U) % current_count];
            const ClipVertex *end = &current[index];
            const double start_distance = clip_distance(start, plane);
            const double end_distance = clip_distance(end, plane);
            const bool start_inside = start_distance >= 0.0;
            const bool end_inside = end_distance >= 0.0;

            if (!isfinite(start_distance) || !isfinite(end_distance)) {
                return false;
            }
            if (start_inside != end_inside) {
                if (next_count >= CLIP_VERTEX_CAPACITY ||
                    !interpolate_clip_vertex(
                        start, end, start_distance, end_distance,
                        &next[next_count])) {
                    return false;
                }
                ++next_count;
                changed = true;
            }
            if (end_inside) {
                if (next_count >= CLIP_VERTEX_CAPACITY) {
                    return false;
                }
                next[next_count++] = *end;
            } else {
                changed = true;
            }
        }
        current_count = next_count;
        {
            ClipVertex *temporary = current;
            current = next;
            next = temporary;
        }
    }
    if (current_count > CLIP_VERTEX_CAPACITY) {
        return false;
    }
    for (plane = 0U; plane < current_count; ++plane) {
        if (!finite_vector(current[plane].clip, 4U) ||
            !finite_vector(current[plane].color, 4U) ||
            current[plane].clip[3] <= 0.0) {
            return false;
        }
    }
    memcpy(output, current, sizeof(ClipVertex) * current_count);
    *out_count = current_count;
    *out_changed = changed;
    return true;
}

static bool round_even_nonnegative(
    double value,
    uint64_t maximum,
    uint64_t *out_value
)
{
    double base_double;
    double fraction;
    uint64_t base;

    if (!isfinite(value) || value < 0.0 || value > (double)maximum) {
        return false;
    }
    base_double = floor(value);
    base = (uint64_t)base_double;
    fraction = value - base_double;
    if (fraction > 0.5 ||
        (fraction == 0.5 && (base & UINT64_C(1)) != 0U)) {
        if (base == maximum) {
            return false;
        }
        ++base;
    }
    *out_value = base;
    return true;
}

static bool make_screen_vertex(
    const ClipVertex *clip,
    ScreenVertex *out_vertex
)
{
    double x;
    double y;
    double z;
    double screen_x;
    double screen_y;
    uint64_t fixed_x;
    uint64_t fixed_y;
    size_t index;

    if (clip->clip[3] <= 0.0 || !isfinite(clip->clip[3])) {
        return false;
    }
    out_vertex->inverse_w = 1.0 / clip->clip[3];
    x = clip->clip[0] * out_vertex->inverse_w;
    y = clip->clip[1] * out_vertex->inverse_w;
    z = clip->clip[2] * out_vertex->inverse_w;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) {
        return false;
    }
    if (x < -1.0) {
        x = -1.0;
    } else if (x > 1.0) {
        x = 1.0;
    }
    if (y < -1.0) {
        y = -1.0;
    } else if (y > 1.0) {
        y = 1.0;
    }
    if (z < -1.0) {
        z = -1.0;
    } else if (z > 1.0) {
        z = 1.0;
    }
    screen_x = (x * 0.5 + 0.5) *
        (double)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
        (double)FIXED_SUBPIXEL_SCALE;
    screen_y = (0.5 - y * 0.5) *
        (double)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *
        (double)FIXED_SUBPIXEL_SCALE;
    if (!round_even_nonnegative(
            screen_x,
            (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
                FIXED_SUBPIXEL_SCALE,
            &fixed_x) ||
        !round_even_nonnegative(
            screen_y,
            (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *
                FIXED_SUBPIXEL_SCALE,
            &fixed_y) || fixed_x > INT32_MAX || fixed_y > INT32_MAX) {
        return false;
    }
    out_vertex->x_fixed = (int32_t)fixed_x;
    out_vertex->y_fixed = (int32_t)fixed_y;
    out_vertex->ndc_x = x;
    out_vertex->ndc_y = y;
    out_vertex->ndc_z = z;
    for (index = 0U; index < 4U; ++index) {
        out_vertex->color_over_w[index] =
            clip->color[index] * out_vertex->inverse_w;
    }
    return isfinite(out_vertex->inverse_w) &&
        finite_vector(out_vertex->color_over_w, 4U);
}

static int64_t edge_value(
    const ScreenVertex *start,
    const ScreenVertex *end,
    int64_t x,
    int64_t y
)
{
    return ((int64_t)end->x_fixed - start->x_fixed) *
            (y - start->y_fixed) -
        ((int64_t)end->y_fixed - start->y_fixed) *
            (x - start->x_fixed);
}

static bool edge_top_left(
    const ScreenVertex *start,
    const ScreenVertex *end
)
{
    const int32_t dx = end->x_fixed - start->x_fixed;
    const int32_t dy = end->y_fixed - start->y_fixed;

    return dy < 0 || (dy == 0 && dx > 0);
}

static int64_t floor_div_fixed(int64_t value)
{
    return value >= 0 ? value / FIXED_SUBPIXEL_SCALE :
        -((-value + FIXED_SUBPIXEL_SCALE - 1) / FIXED_SUBPIXEL_SCALE);
}

static int64_t ceil_div_fixed(int64_t value)
{
    return -floor_div_fixed(-value);
}

static bool prepare_screen_triangle(
    const ClipVertex *clip_a,
    const ClipVertex *clip_b,
    const ClipVertex *clip_c,
    ScreenVertex output[3],
    bool *out_culled
)
{
    const double inverse_a = 1.0 / clip_a->clip[3];
    const double inverse_b = 1.0 / clip_b->clip[3];
    const double inverse_c = 1.0 / clip_c->clip[3];
    const double ax = clip_a->clip[0] * inverse_a;
    const double ay = clip_a->clip[1] * inverse_a;
    const double bx = clip_b->clip[0] * inverse_b;
    const double by = clip_b->clip[1] * inverse_b;
    const double cx = clip_c->clip[0] * inverse_c;
    const double cy = clip_c->clip[1] * inverse_c;
    const double ndc_area = (bx - ax) * (cy - ay) -
        (by - ay) * (cx - ax);
    int64_t fixed_area;

    *out_culled = false;
    if (!isfinite(ndc_area)) {
        return false;
    }
    if (ndc_area <= 0.0) {
        *out_culled = true;
        return true;
    }
    if (!make_screen_vertex(clip_a, &output[0]) ||
        !make_screen_vertex(clip_b, &output[1]) ||
        !make_screen_vertex(clip_c, &output[2])) {
        return false;
    }
    fixed_area = edge_value(
        &output[0], &output[1], output[2].x_fixed, output[2].y_fixed
    );
    if (fixed_area == 0) {
        *out_culled = true;
        return true;
    }
    if (fixed_area < 0) {
        const ScreenVertex temporary = output[1];
        output[1] = output[2];
        output[2] = temporary;
    }
    return true;
}

static bool edge_covered(int64_t value, bool top_left)
{
    return value > 0 || (value == 0 && top_left);
}

static void rasterize_screen_triangle(
    const ScreenVertex input[3],
    uint8_t *color_target,
    uint8_t *depth_target,
    uint64_t *in_out_fragments
)
{
    const int32_t min_fixed_x =
        input[0].x_fixed < input[1].x_fixed ?
            (input[0].x_fixed < input[2].x_fixed ?
                input[0].x_fixed : input[2].x_fixed) :
            (input[1].x_fixed < input[2].x_fixed ?
                input[1].x_fixed : input[2].x_fixed);
    const int32_t max_fixed_x =
        input[0].x_fixed > input[1].x_fixed ?
            (input[0].x_fixed > input[2].x_fixed ?
                input[0].x_fixed : input[2].x_fixed) :
            (input[1].x_fixed > input[2].x_fixed ?
                input[1].x_fixed : input[2].x_fixed);
    const int32_t min_fixed_y =
        input[0].y_fixed < input[1].y_fixed ?
            (input[0].y_fixed < input[2].y_fixed ?
                input[0].y_fixed : input[2].y_fixed) :
            (input[1].y_fixed < input[2].y_fixed ?
                input[1].y_fixed : input[2].y_fixed);
    const int32_t max_fixed_y =
        input[0].y_fixed > input[1].y_fixed ?
            (input[0].y_fixed > input[2].y_fixed ?
                input[0].y_fixed : input[2].y_fixed) :
            (input[1].y_fixed > input[2].y_fixed ?
                input[1].y_fixed : input[2].y_fixed);
    int64_t min_x = ceil_div_fixed((int64_t)min_fixed_x - FIXED_PIXEL_CENTER);
    int64_t max_x = floor_div_fixed((int64_t)max_fixed_x - FIXED_PIXEL_CENTER);
    int64_t min_y = ceil_div_fixed((int64_t)min_fixed_y - FIXED_PIXEL_CENTER);
    int64_t max_y = floor_div_fixed((int64_t)max_fixed_y - FIXED_PIXEL_CENTER);
    const int64_t area = edge_value(
        &input[0], &input[1], input[2].x_fixed, input[2].y_fixed
    );
    const bool top_left_0 = edge_top_left(&input[1], &input[2]);
    const bool top_left_1 = edge_top_left(&input[2], &input[0]);
    const bool top_left_2 = edge_top_left(&input[0], &input[1]);
    int64_t y;

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x >= MEISO_COMPOSITION_SCENE_TARGET_WIDTH) {
        max_x = MEISO_COMPOSITION_SCENE_TARGET_WIDTH - 1U;
    }
    if (max_y >= MEISO_COMPOSITION_SCENE_TARGET_HEIGHT) {
        max_y = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT - 1U;
    }
    for (y = min_y; y <= max_y; ++y) {
        int64_t x;
        const int64_t sample_y =
            y * FIXED_SUBPIXEL_SCALE + FIXED_PIXEL_CENTER;

        for (x = min_x; x <= max_x; ++x) {
            const int64_t sample_x =
                x * FIXED_SUBPIXEL_SCALE + FIXED_PIXEL_CENTER;
            const int64_t edge0 =
                edge_value(&input[1], &input[2], sample_x, sample_y);
            const int64_t edge1 =
                edge_value(&input[2], &input[0], sample_x, sample_y);
            const int64_t edge2 =
                edge_value(&input[0], &input[1], sample_x, sample_y);
            double lambda0;
            double lambda1;
            double lambda2;
            double depth_normalized;
            uint64_t depth_value;
            size_t pixel;
            uint32_t old_depth;

            if (!edge_covered(edge0, top_left_0) ||
                !edge_covered(edge1, top_left_1) ||
                !edge_covered(edge2, top_left_2)) {
                continue;
            }
            lambda0 = (double)edge0 / (double)area;
            lambda1 = (double)edge1 / (double)area;
            lambda2 = (double)edge2 / (double)area;
            depth_normalized =
                (lambda0 * input[0].ndc_z +
                 lambda1 * input[1].ndc_z +
                 lambda2 * input[2].ndc_z) * 0.5 + 0.5;
            if (depth_normalized < 0.0) {
                depth_normalized = 0.0;
            } else if (depth_normalized > 1.0) {
                depth_normalized = 1.0;
            }
            if (!round_even_nonnegative(
                    depth_normalized * 4294967294.0,
                    UINT32_C(0xfffffffe), &depth_value)) {
                continue;
            }
            pixel = ((size_t)y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
                     (size_t)x) * 4U;
            old_depth = read_be_u32(depth_target + pixel);
            if ((uint32_t)depth_value < old_depth) {
                const double denominator =
                    lambda0 * input[0].inverse_w +
                    lambda1 * input[1].inverse_w +
                    lambda2 * input[2].inverse_w;
                size_t channel;

                if (!isfinite(denominator) || denominator <= 0.0) {
                    continue;
                }
                for (channel = 0U; channel < 4U; ++channel) {
                    double value =
                        (lambda0 * input[0].color_over_w[channel] +
                         lambda1 * input[1].color_over_w[channel] +
                         lambda2 * input[2].color_over_w[channel]) /
                        denominator;
                    uint64_t rounded;

                    if (value < 0.0) {
                        value = 0.0;
                    } else if (value > 255.0) {
                        value = 255.0;
                    }
                    if (!round_even_nonnegative(value, 255U, &rounded)) {
                        rounded = 0U;
                    }
                    color_target[pixel + channel] = (uint8_t)rounded;
                }
                write_be_u32(depth_target + pixel, (uint32_t)depth_value);
                if (*in_out_fragments != UINT64_MAX) {
                    ++*in_out_fragments;
                }
            }
        }
    }
}

static MeisoCompositionCpuRasterRenderStatus process_job_geometry(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const RasterJob *job,
    const RasterCamera *camera,
    bool draw,
    uint8_t *color_target,
    uint8_t *depth_target,
    RasterWorkCounts *counts
)
{
    uint32_t object_index;

    memset(counts, 0, sizeof(*counts));
    for (object_index = 0U; object_index < job->desired.object_count;
         ++object_index) {
        const MeisoCompositionSceneObject *object =
            &job->desired.objects[object_index];
        MeisoBlobView blob;
        MeisoCompositionSceneMeshView mesh;
        uint32_t expected_size;
        uint32_t index_offset;

        if ((object->flags & MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE) == 0U) {
            continue;
        }
        if (!expected_mesh_size(
                job, &object->mesh_content_id, &expected_size)) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH;
        }
        if (!meiso_blob_store_lookup(
                renderer->blob_store, &object->mesh_content_id, &blob)) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_MISSING_BLOB;
        }
        if (!content_id_equal(&object->mesh_content_id, &blob.content_id) ||
            blob.content.size != expected_size ||
            !meiso_composition_scene_mesh_blob_verify(
                &object->mesh_content_id, blob.content, NULL) ||
            meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                MEISO_COMPOSITION_SCENE_CODEC_OK ||
            mesh.index_count % 3U != 0U) {
            return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH;
        }
        for (index_offset = 0U; index_offset < mesh.index_count;
             index_offset += 3U) {
            uint32_t vertex_indices[3];
            MeisoCompositionSceneMeshVertex mesh_vertices[3];
            ClipVertex triangle[3];
            ClipVertex clipped[CLIP_VERTEX_CAPACITY];
            uint32_t clipped_count;
            bool changed;
            uint32_t vertex;
            uint32_t fan;

            if (!add_checked(&counts->submitted, 1U)) {
                return
                    MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
            }
            for (vertex = 0U; vertex < 3U; ++vertex) {
                if (meiso_composition_scene_mesh_view_index(
                        &mesh, index_offset + vertex,
                        &vertex_indices[vertex]) !=
                        MEISO_COMPOSITION_SCENE_CODEC_OK ||
                    meiso_composition_scene_mesh_view_vertex(
                        &mesh, vertex_indices[vertex],
                        &mesh_vertices[vertex]) !=
                        MEISO_COMPOSITION_SCENE_CODEC_OK ||
                    !make_clip_vertex(
                        camera, object, &mesh_vertices[vertex],
                        mesh.has_vertex_color, &triangle[vertex])) {
                    return
                        MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
                }
            }
            if (!clip_triangle(
                    triangle, clipped, &clipped_count, &changed)) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
            }
            if (changed && !add_checked(&counts->clipped, 1U)) {
                return
                    MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
            }
            if (clipped_count < 3U) {
                continue;
            }
            if (clipped_count > CLIP_VERTEX_CAPACITY) {
                return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
            }
            for (fan = 1U; fan + 1U < clipped_count; ++fan) {
                ScreenVertex screen[3];
                bool culled;

                if (!prepare_screen_triangle(
                        &clipped[0], &clipped[fan], &clipped[fan + 1U],
                        screen, &culled)) {
                    return
                        MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
                }
                if (culled) {
                    if (!add_checked(&counts->culled, 1U)) {
                        return MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
                    }
                    continue;
                }
                if (!add_checked(&counts->rasterized, 1U)) {
                    return
                        MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED;
                }
                if (draw) {
                    rasterize_screen_triangle(
                        screen, color_target, depth_target,
                        &counts->fragments_depth_passed
                    );
                }
            }
        }
    }
    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE;
}

static void clear_target(uint8_t *color, uint8_t *depth)
{
    size_t pixel;

    for (pixel = 0U;
         pixel < (size_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
             MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
         ++pixel) {
        const size_t offset = pixel * 4U;

        color[offset] = MEISO_COMPOSITION_CPU_RASTER_CLEAR_R;
        color[offset + 1U] = MEISO_COMPOSITION_CPU_RASTER_CLEAR_G;
        color[offset + 2U] = MEISO_COMPOSITION_CPU_RASTER_CLEAR_B;
        color[offset + 3U] = MEISO_COMPOSITION_CPU_RASTER_CLEAR_A;
    }
    memset(depth, 0xff, MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE);
}

static void build_result(
    MeisoCompositionCpuRasterRenderer *renderer,
    const RasterJob *job,
    uint32_t target_slot,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns
)
{
    MeisoCompositionSceneRenderResult *result = &renderer->result_scratch;
    RasterSha256 sha;
    uint32_t y;

    memset(result, 0, sizeof(*result));
    result->applied_scene_owner_incarnation =
        job->ref.world.desired_owner_incarnation;
    result->applied_scene_state_version =
        job->ref.world.desired_state_version;
    result->active_plan_generation = job->ref.world.plan_generation;
    result->world_generation = job->ref.world.world_generation;
    result->result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    result->evidence_flags =
        MEISO_COMPOSITION_CPU_RASTER_RENDERER_EVIDENCE_FLAGS;
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
        MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE
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
}

static void make_complete_view(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterCompleteView *out_complete
)
{
    memset(out_complete, 0, sizeof(*out_complete));
    out_complete->job = renderer->latest_complete_job;
    out_complete->result = renderer->latest_result;
    out_complete->target_slot = renderer->complete_slot;
    out_complete->color_rgba8888.data =
        renderer->colors[renderer->complete_slot];
    out_complete->color_rgba8888.size =
        MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE;
    out_complete->depth_implementation_32.data =
        renderer->depths[renderer->complete_slot];
    out_complete->depth_implementation_32.size =
        MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE;
}

size_t meiso_composition_cpu_raster_renderer_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_composition_cpu_raster_renderer_memory_required(void)
{
    const size_t alignment =
        meiso_composition_cpu_raster_renderer_alignment_required();
    const size_t metadata = align_up(
        sizeof(MeisoCompositionCpuRasterRenderer), alignment
    );

    return metadata + MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES;
}

MeisoCompositionCpuRasterInitStatus
meiso_composition_cpu_raster_renderer_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionCpuRasterRenderer **out_renderer
)
{
    MeisoCompositionCpuRasterRenderer *renderer;
    MeisoBlobStats ignored;
    size_t offset;
    uint32_t slot;

    if (arena == NULL || out_renderer == NULL ||
        ranges_overlap(arena, arena_size,
                       out_renderer, sizeof(*out_renderer))) {
        return MEISO_COMPOSITION_CPU_RASTER_INIT_INVALID_ARGUMENT;
    }
    if (blob_store == NULL || !meiso_blob_store_stats(blob_store, &ignored)) {
        return MEISO_COMPOSITION_CPU_RASTER_INIT_INVALID_BLOB_STORE;
    }
    if (!platform_supported()) {
        return MEISO_COMPOSITION_CPU_RASTER_INIT_PLATFORM_UNSUPPORTED;
    }
    if ((uintptr_t)arena %
            meiso_composition_cpu_raster_renderer_alignment_required() != 0U) {
        return MEISO_COMPOSITION_CPU_RASTER_INIT_MISALIGNED_ARENA;
    }
    if (arena_size <
            meiso_composition_cpu_raster_renderer_memory_required()) {
        return MEISO_COMPOSITION_CPU_RASTER_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, arena_size);
    renderer = (MeisoCompositionCpuRasterRenderer *)arena;
    renderer->magic = RASTER_RENDERER_MAGIC;
    renderer->arena_size = arena_size;
    renderer->blob_store = blob_store;
    renderer->complete_slot = 0U;
    offset = align_up(
        sizeof(*renderer),
        meiso_composition_cpu_raster_renderer_alignment_required()
    );
    for (slot = 0U; slot < TARGET_SLOT_COUNT; ++slot) {
        renderer->colors[slot] = (uint8_t *)arena + offset;
        offset += MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE;
        renderer->depths[slot] = (uint8_t *)arena + offset;
        offset += MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE;
    }
    *out_renderer = renderer;
    return MEISO_COMPOSITION_CPU_RASTER_INIT_OK;
}

MeisoCompositionCpuRasterScopeStatus
meiso_composition_cpu_raster_renderer_begin_control_scope(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope
)
{
    uint32_t slot;

    if (!renderer_valid(renderer) || !scope_valid(scope) ||
        renderer_overlaps(renderer, scope, sizeof(*scope))) {
        return MEISO_COMPOSITION_CPU_RASTER_SCOPE_INVALID_ARGUMENT;
    }
    if (renderer->scope_bound && scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_CPU_RASTER_SCOPE_UNCHANGED;
    }
    if (renderer->renderer_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_CPU_RASTER_SCOPE_COUNTER_EXHAUSTED;
    }
    clear_jobs(renderer);
    for (slot = 0U; slot < TARGET_SLOT_COUNT; ++slot) {
        memset(renderer->colors[slot], 0,
               MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE);
        memset(renderer->depths[slot], 0,
               MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE);
    }
    renderer->complete_available = false;
    renderer->complete_slot = 0U;
    memset(&renderer->latest_complete_job, 0,
           sizeof(renderer->latest_complete_job));
    memset(&renderer->latest_result, 0, sizeof(renderer->latest_result));
    memset(&renderer->validation_scratch, 0,
           sizeof(renderer->validation_scratch));
    memset(renderer->canonical_scratch, 0,
           sizeof(renderer->canonical_scratch));
    memset(&renderer->result_scratch, 0, sizeof(renderer->result_scratch));
    renderer->scope = *scope;
    renderer->scope_bound = true;
    ++renderer->renderer_generation;
    increment_saturated(&renderer->stats.scope_resets);
    return MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED;
}

MeisoCompositionCpuRasterSubmitStatus
meiso_composition_cpu_raster_renderer_submit(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    uint64_t source_clock_epoch,
    MeisoCompositionCpuRasterJobRef *out_job
)
{
    RasterJob *candidate;
    MeisoCompositionCpuRasterSubmitStatus status;
    uint32_t tail;

    if (!renderer_valid(renderer) || scope == NULL || world == NULL ||
        world->desired == NULL || out_job == NULL || source_clock_epoch == 0U ||
        renderer_overlaps(renderer, scope, sizeof(*scope)) ||
        renderer_overlaps(renderer, world, sizeof(*world)) ||
        renderer_overlaps(renderer, world->desired, sizeof(*world->desired)) ||
        renderer_overlaps(renderer, out_job, sizeof(*out_job)) ||
        ranges_overlap(scope, sizeof(*scope), out_job, sizeof(*out_job)) ||
        ranges_overlap(world, sizeof(*world), out_job, sizeof(*out_job)) ||
        ranges_overlap(world->desired, sizeof(*world->desired),
                       out_job, sizeof(*out_job))) {
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_SCOPE_MISMATCH;
    }
    if (renderer->queue_count == JOB_SLOT_COUNT) {
        increment_saturated(&renderer->stats.queue_capacity_rejections);
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUE_FULL;
    }
    if (renderer->job_id_high_water == UINT64_MAX) {
        return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_COUNTER_EXHAUSTED;
    }
    candidate = &renderer->validation_scratch;
    status = validate_world(renderer, scope, world, candidate);
    if (status != MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED) {
        if (status == MEISO_COMPOSITION_CPU_RASTER_SUBMIT_MISSING_BLOB) {
            increment_saturated(&renderer->stats.missing_blob_rejections);
        } else if (status ==
                       MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_MESH) {
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
    return MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED;
}

MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_render_next(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterJobRef *expected_job,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionCpuRasterCompleteView *out_complete
)
{
    RasterJob *job;
    MeisoCompositionCpuRasterRenderStatus status;
    RasterCamera camera;
    RasterWorkCounts preflight;
    RasterWorkCounts rendered;
    uint32_t target_slot;

    if (!renderer_valid(renderer) || expected_job == NULL ||
        out_complete == NULL ||
        renderer_overlaps(renderer, expected_job, sizeof(*expected_job)) ||
        renderer_overlaps(renderer, out_complete, sizeof(*out_complete)) ||
        ranges_overlap(expected_job, sizeof(*expected_job),
                       out_complete, sizeof(*out_complete))) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_ARGUMENT;
    }
    job = queue_head_job(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB;
    }
    if (!job_ref_equal(&job->ref, expected_job)) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH;
    }
    if (render_end_monotonic_ns < render_start_monotonic_ns) {
        increment_saturated(&renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_TIME;
    }
    if (!platform_supported()) {
        increment_saturated(&renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
    }
    status = validate_job_blobs(renderer, job);
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
        increment_saturated(&renderer->stats.render_failures);
        return status;
    }
    if (!prepare_camera(&job->desired.camera, &camera)) {
        increment_saturated(&renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
    }
    status = process_job_geometry(
        renderer, job, &camera, false, NULL, NULL, &preflight
    );
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
        increment_saturated(&renderer->stats.render_failures);
        return status;
    }

    target_slot = renderer->complete_available ?
        (renderer->complete_slot == 0U ? 1U : 0U) : 0U;
    clear_target(renderer->colors[target_slot], renderer->depths[target_slot]);
    status = process_job_geometry(
        renderer, job, &camera, true, renderer->colors[target_slot],
        renderer->depths[target_slot], &rendered
    );
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE ||
        rendered.submitted != preflight.submitted ||
        rendered.clipped != preflight.clipped ||
        rendered.culled != preflight.culled ||
        rendered.rasterized != preflight.rasterized) {
        increment_saturated(&renderer->stats.render_failures);
        return status == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE ?
            MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY : status;
    }

    build_result(
        renderer, job, target_slot, render_start_monotonic_ns,
        render_end_monotonic_ns
    );
    renderer->complete_slot = target_slot;
    renderer->complete_available = true;
    renderer->latest_complete_job = job->ref;
    renderer->latest_result = renderer->result_scratch;
    pop_head(renderer);
    increment_saturated(&renderer->stats.renders_completed);
    add_saturated(&renderer->stats.triangles_submitted, preflight.submitted);
    add_saturated(&renderer->stats.triangles_clipped, preflight.clipped);
    add_saturated(&renderer->stats.triangles_culled, preflight.culled);
    add_saturated(&renderer->stats.triangles_rasterized, preflight.rasterized);
    add_saturated(
        &renderer->stats.fragments_depth_passed,
        rendered.fragments_depth_passed
    );
    make_complete_view(renderer, out_complete);
    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE;
}

MeisoCompositionCpuRasterDiscardStatus
meiso_composition_cpu_raster_renderer_discard_head(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterJobRef *expected_job
)
{
    const RasterJob *job;

    if (!renderer_valid(renderer) || expected_job == NULL ||
        renderer_overlaps(renderer, expected_job, sizeof(*expected_job))) {
        return MEISO_COMPOSITION_CPU_RASTER_DISCARD_INVALID_ARGUMENT;
    }
    job = const_queue_head_job(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_CPU_RASTER_DISCARD_NO_JOB;
    }
    if (!job_ref_equal(&job->ref, expected_job)) {
        return MEISO_COMPOSITION_CPU_RASTER_DISCARD_JOB_MISMATCH;
    }
    pop_head(renderer);
    increment_saturated(&renderer->stats.jobs_discarded);
    return MEISO_COMPOSITION_CPU_RASTER_DISCARD_DISCARDED;
}

MeisoCompositionCpuRasterRetireStatus
meiso_composition_cpu_raster_renderer_retire_world(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *retired_world
)
{
    uint64_t jobs_retired = 0U;
    bool complete_retired = false;

    if (!renderer_valid(renderer) || !scope_valid(scope) ||
        !world_ref_valid(retired_world) ||
        renderer_overlaps(renderer, scope, sizeof(*scope)) ||
        renderer_overlaps(renderer, retired_world, sizeof(*retired_world)) ||
        ranges_overlap(scope, sizeof(*scope),
                       retired_world, sizeof(*retired_world))) {
        return MEISO_COMPOSITION_CPU_RASTER_RETIRE_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope) ||
        retired_world->desired_owner_incarnation !=
            scope->desired_owner_incarnation) {
        return MEISO_COMPOSITION_CPU_RASTER_RETIRE_SCOPE_MISMATCH;
    }

    if (renderer->queue_count == 1U) {
        if (world_ref_equal(
                &renderer->jobs[renderer->queue_head].ref.world,
                retired_world)) {
            clear_jobs(renderer);
            jobs_retired = 1U;
        }
    } else if (renderer->queue_count == 2U) {
        const uint32_t first = renderer->queue_head;
        const uint32_t second = (first + 1U) % JOB_SLOT_COUNT;
        const bool retire_first = world_ref_equal(
            &renderer->jobs[first].ref.world, retired_world
        );
        const bool retire_second = world_ref_equal(
            &renderer->jobs[second].ref.world, retired_world
        );

        if (retire_first && retire_second) {
            clear_jobs(renderer);
            jobs_retired = 2U;
        } else if (retire_first || retire_second) {
            renderer->validation_scratch =
                retire_first ? renderer->jobs[second] : renderer->jobs[first];
            memset(renderer->jobs, 0, sizeof(renderer->jobs));
            renderer->jobs[0] = renderer->validation_scratch;
            renderer->queue_head = 0U;
            renderer->queue_count = 1U;
            jobs_retired = 1U;
        }
    }
    if (renderer->complete_available &&
        world_ref_equal(&renderer->latest_complete_job.world,
                        retired_world)) {
        memset(renderer->colors[renderer->complete_slot], 0,
               MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE);
        memset(renderer->depths[renderer->complete_slot], 0,
               MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE);
        renderer->complete_available = false;
        renderer->complete_slot = 0U;
        memset(&renderer->latest_complete_job, 0,
               sizeof(renderer->latest_complete_job));
        memset(&renderer->latest_result, 0, sizeof(renderer->latest_result));
        complete_retired = true;
    }
    if (jobs_retired == 0U && !complete_retired) {
        return MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED;
    }
    increment_saturated(&renderer->stats.world_retires);
    add_saturated(&renderer->stats.jobs_retired, jobs_retired);
    if (complete_retired) {
        increment_saturated(&renderer->stats.completes_retired);
    }
    return MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED;
}

bool meiso_composition_cpu_raster_renderer_latest_complete(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterCompleteView *out_complete
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

bool meiso_composition_cpu_raster_renderer_snapshot(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterSnapshot *out_snapshot
)
{
    MeisoCompositionCpuRasterSnapshot snapshot;
    const RasterJob *head;

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
    snapshot.target_bytes = MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES;
    snapshot.renderer_metadata_bytes =
        renderer->arena_size - MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES;
    snapshot.complete_available = renderer->complete_available;
    snapshot.complete_target_slot =
        renderer->complete_available ? renderer->complete_slot : 0U;
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

bool meiso_composition_cpu_raster_renderer_stats(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterStats *out_stats
)
{
    if (!renderer_valid(renderer) || out_stats == NULL ||
        renderer_overlaps(renderer, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = renderer->stats;
    return true;
}
