#include "meiso_composition_reference_renderer_private.h"
#include "../src/core/meiso_sha256_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Render execution owns deterministic targets, hashes and atomic completion. */

static void write_be_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
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
    MeisoSha256 sha;
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

    meiso_sha256_init(&sha);
    meiso_sha256_update(&sha, renderer->colors[target_slot],
                  MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE);
    meiso_sha256_final(&sha, result->full_target_sha256);
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
