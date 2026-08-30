#include "meiso_composition_reference_renderer_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Render execution owns deterministic targets, hashes and atomic completion. */

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} ReferenceSha256;

static uint32_t rotate_right(uint32_t value, uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static uint32_t read_be_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24U) | ((uint32_t)input[1] << 16U) |
           ((uint32_t)input[2] << 8U) | (uint32_t)input[3];
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
    uint32_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = read_be_u32(block + (size_t)index * 4U);
    }
    for (index = 16U; index < 64U; ++index) {
        const uint32_t x = words[index - 15U];
        const uint32_t y = words[index - 2U];
        const uint32_t small0 =
            rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
        const uint32_t small1 =
            rotate_right(y, 17U) ^ rotate_right(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + small0 + words[index - 7U] + small1;
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

static void sha256_init(ReferenceSha256 *sha)
{
    static const uint32_t initial[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                        0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                        0x1f83d9abU, 0x5be0cd19U};

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(ReferenceSha256 *sha, const uint8_t *data,
                          size_t size)
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

static void sha256_final(ReferenceSha256 *sha,
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

static uint64_t fnv1a_update(uint64_t hash, const uint8_t *data, size_t size)
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

static void synthetic_pixel(uint64_t scene_seed, uint32_t x, uint32_t y,
                            uint8_t rgba[4], uint32_t *depth)
{
    const uint64_t mixed =
        mix64(scene_seed ^ ((uint64_t)x * UINT64_C(0x9e3779b185ebca87)) ^
              ((uint64_t)y * UINT64_C(0xc2b2ae3d27d4eb4f)));

    rgba[0] = (uint8_t)(mixed >> 8U);
    rgba[1] = (uint8_t)(mixed >> 24U);
    rgba[2] = (uint8_t)(mixed >> 40U);
    rgba[3] = 0xffU;
    *depth = (uint32_t)mix64(mixed ^ UINT64_C(0x4450544833320000));
}

static MeisoCompositionReferenceRendererRenderStatus
job_seed(const MeisoCompositionReferenceRenderer *renderer,
         const MeisoCompositionReferenceRendererJob *job, uint64_t *out_seed)
{
    static const uint8_t domain[] = {'M', 'E', 'I', 'S', 'O', '_', 'C', 'O',
                                     'M', 'P', '_', 'R', 'E', 'F', 'E', 'R',
                                     'E', 'N', 'C', 'E', '_', 'R', 'E', 'N',
                                     'D', 'E', 'R', '_', 'V', '0', 0U};
    uint64_t seed = UINT64_C(14695981039346656037);
    uint32_t index;

    seed = fnv1a_update(seed, domain, sizeof(domain));
    seed = fnv1a_update(seed, job->canonical, job->canonical_size);
    for (index = 0U; index < job->unique_mesh_count; ++index) {
        MeisoBlobView blob;
        uint8_t size_be[8];
        uint64_t size = job->unique_mesh_sizes[index];
        size_t byte_index;

        if (!meiso_blob_store_lookup(renderer->blob_store,
                                     &job->unique_meshes[index], &blob)) {
            return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_MISSING_BLOB;
        }
        if (!meiso_composition_reference_renderer_private_content_id_equal(
                &job->unique_meshes[index], &blob.content_id) ||
            blob.content.size != job->unique_mesh_sizes[index] ||
            !meiso_composition_scene_mesh_blob_verify(
                &job->unique_meshes[index], blob.content, NULL)) {
            return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_MESH;
        }
        for (byte_index = 0U; byte_index < sizeof(size_be); ++byte_index) {
            size_be[byte_index] = (uint8_t)(size >> (56U - byte_index * 8U));
        }
        seed = fnv1a_update(seed, size_be, sizeof(size_be));
        seed = fnv1a_update(seed, blob.content.data, blob.content.size);
    }
    *out_seed = seed;
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE;
}

MeisoCompositionReferenceRendererRenderStatus
meiso_composition_reference_renderer_render_next(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job,
    uint64_t render_start_monotonic_ns, uint64_t render_end_monotonic_ns,
    MeisoCompositionReferenceRendererCompleteView *out_complete)
{
    MeisoCompositionReferenceRendererJob *job;
    MeisoCompositionReferenceRendererRenderStatus seed_status;
    MeisoCompositionSceneRenderResult *result;
    ReferenceSha256 sha;
    uint64_t seed;
    uint32_t target_slot;
    uint32_t y;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        expected_job == NULL || out_complete == NULL ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, expected_job, sizeof(*expected_job)) ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, out_complete, sizeof(*out_complete)) ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            expected_job, sizeof(*expected_job), out_complete,
            sizeof(*out_complete))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_ARGUMENT;
    }
    job = meiso_composition_reference_renderer_private_queue_head(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB;
    }
    if (!meiso_composition_reference_renderer_private_job_ref_equal(
            &job->ref, expected_job)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH;
    }
    if (render_end_monotonic_ns < render_start_monotonic_ns) {
        meiso_composition_reference_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_TIME;
    }
    seed_status = job_seed(renderer, job, &seed);
    if (seed_status != MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE) {
        meiso_composition_reference_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return seed_status;
    }

    target_slot = renderer->complete_available
                      ? (renderer->complete_slot == 0U ? 1U : 0U)
                      : 0U;
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
    result->applied_scene_state_version = job->ref.world.desired_state_version;
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
    result->logical_color_row_bytes = MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->logical_depth_row_bytes = MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    result->thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    result->thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    result->thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U;
    result->thumbnail_byte_size = MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;

    sha256_init(&sha);
    sha256_update(&sha, renderer->colors[target_slot],
                  MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE);
    sha256_final(&sha, result->full_target_sha256);
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH; ++x) {
            const uint32_t source_x = x * 5U + 2U;
            const uint32_t source_y = y * 5U + 2U;
            const size_t source =
                ((size_t)source_y * MEISO_COMPOSITION_SCENE_TARGET_WIDTH +
                 source_x) *
                4U;
            const size_t output =
                ((size_t)y * MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH + x) * 2U;
            const uint8_t *rgba = renderer->colors[target_slot] + source;
            const uint16_t rgb565 =
                (uint16_t)(((uint16_t)(rgba[0] >> 3U) << 11U) |
                           ((uint16_t)(rgba[1] >> 2U) << 5U) |
                           (uint16_t)(rgba[2] >> 3U));

            result->thumbnail_rgb565[output] = (uint8_t)(rgb565 >> 8U);
            result->thumbnail_rgb565[output + 1U] = (uint8_t)rgb565;
        }
    }

    renderer->complete_slot = target_slot;
    renderer->complete_available = true;
    renderer->latest_complete_job = job->ref;
    renderer->latest_result = *result;
    meiso_composition_reference_renderer_private_pop_head(renderer);
    meiso_composition_reference_renderer_private_increment_saturated(
        &renderer->stats.renders_completed);
    meiso_composition_reference_renderer_private_make_complete_view(
        renderer, out_complete);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE;
}
