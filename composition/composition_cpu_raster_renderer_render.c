#include "meiso_composition_cpu_raster_renderer_private.h"

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Render execution owns preflight, inactive-target fill and atomic commit. */

static void clear_target(uint8_t *color, uint8_t *depth)
{
    size_t pixel;

    for (pixel = 0U; pixel < (size_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
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

static void build_result(MeisoCompositionCpuRasterRenderer *renderer,
                         const MeisoCompositionCpuRasterRendererJob *job,
                         uint32_t target_slot,
                         uint64_t render_start_monotonic_ns,
                         uint64_t render_end_monotonic_ns)
{
    MeisoCompositionSceneRenderResult *result = &renderer->result_scratch;
    MeisoCompositionCpuRasterSha256 sha;
    uint32_t y;

    memset(result, 0, sizeof(*result));
    result->applied_scene_owner_incarnation =
        job->ref.world.desired_owner_incarnation;
    result->applied_scene_state_version = job->ref.world.desired_state_version;
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
    result->logical_color_row_bytes = MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->logical_depth_row_bytes = MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    result->thumbnail_width = MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    result->thumbnail_height = MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    result->thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    result->thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U;
    result->thumbnail_byte_size = MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;

    meiso_composition_cpu_raster_renderer_private_sha256_init(&sha);
    meiso_composition_cpu_raster_renderer_private_sha256_update(
        &sha, renderer->colors[target_slot],
        MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE);
    meiso_composition_cpu_raster_renderer_private_sha256_final(
        &sha, result->full_target_sha256);
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
}

MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_render_next(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterJobRef *expected_job,
    uint64_t render_start_monotonic_ns, uint64_t render_end_monotonic_ns,
    MeisoCompositionCpuRasterCompleteView *out_complete)
{
    MeisoCompositionCpuRasterRendererJob *job;
    MeisoCompositionCpuRasterRenderStatus status;
    MeisoCompositionCpuRasterCamera camera;
    MeisoCompositionCpuRasterWorkCounts preflight;
    MeisoCompositionCpuRasterWorkCounts rendered;
    uint32_t target_slot;

    if (!meiso_composition_cpu_raster_renderer_private_valid(renderer) ||
        expected_job == NULL || out_complete == NULL ||
        meiso_composition_cpu_raster_renderer_private_overlaps(
            renderer, expected_job, sizeof(*expected_job)) ||
        meiso_composition_cpu_raster_renderer_private_overlaps(
            renderer, out_complete, sizeof(*out_complete)) ||
        meiso_composition_cpu_raster_renderer_private_ranges_overlap(
            expected_job, sizeof(*expected_job), out_complete,
            sizeof(*out_complete))) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_ARGUMENT;
    }
    job = meiso_composition_cpu_raster_renderer_private_queue_head(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB;
    }
    if (!meiso_composition_cpu_raster_renderer_private_job_ref_equal(
            &job->ref, expected_job)) {
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH;
    }
    if (render_end_monotonic_ns < render_start_monotonic_ns) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_TIME;
    }
    if (!meiso_composition_cpu_raster_renderer_private_platform_supported()) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
    }
    status = meiso_composition_cpu_raster_renderer_private_validate_job_blobs(
        renderer, job);
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return status;
    }
    if (!meiso_composition_cpu_raster_renderer_private_prepare_camera(
            &job->desired.camera, &camera)) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY;
    }
    status = meiso_composition_cpu_raster_renderer_private_process_geometry(
        renderer, job, &camera, false, NULL, NULL, &preflight);
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return status;
    }

    target_slot = renderer->complete_available
                      ? (renderer->complete_slot == 0U ? 1U : 0U)
                      : 0U;
    clear_target(renderer->colors[target_slot], renderer->depths[target_slot]);
    status = meiso_composition_cpu_raster_renderer_private_process_geometry(
        renderer, job, &camera, true, renderer->colors[target_slot],
        renderer->depths[target_slot], &rendered);
    if (status != MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE ||
        rendered.submitted != preflight.submitted ||
        rendered.clipped != preflight.clipped ||
        rendered.culled != preflight.culled ||
        rendered.rasterized != preflight.rasterized) {
        meiso_composition_cpu_raster_renderer_private_increment_saturated(
            &renderer->stats.render_failures);
        return status == MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE
                   ? MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY
                   : status;
    }

    build_result(renderer, job, target_slot, render_start_monotonic_ns,
                 render_end_monotonic_ns);
    renderer->complete_slot = target_slot;
    renderer->complete_available = true;
    renderer->latest_complete_job = job->ref;
    renderer->latest_result = renderer->result_scratch;
    meiso_composition_cpu_raster_renderer_private_pop_head(renderer);
    meiso_composition_cpu_raster_renderer_private_increment_saturated(
        &renderer->stats.renders_completed);
    meiso_composition_cpu_raster_renderer_private_add_saturated(
        &renderer->stats.triangles_submitted, preflight.submitted);
    meiso_composition_cpu_raster_renderer_private_add_saturated(
        &renderer->stats.triangles_clipped, preflight.clipped);
    meiso_composition_cpu_raster_renderer_private_add_saturated(
        &renderer->stats.triangles_culled, preflight.culled);
    meiso_composition_cpu_raster_renderer_private_add_saturated(
        &renderer->stats.triangles_rasterized, preflight.rasterized);
    meiso_composition_cpu_raster_renderer_private_add_saturated(
        &renderer->stats.fragments_depth_passed,
        rendered.fragments_depth_passed);
    meiso_composition_cpu_raster_renderer_private_make_complete_view(
        renderer, out_complete);
    return MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE;
}
