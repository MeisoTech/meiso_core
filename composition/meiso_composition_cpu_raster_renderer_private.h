#ifndef MEISO_COMPOSITION_CPU_RASTER_RENDERER_PRIVATE_H
#define MEISO_COMPOSITION_CPU_RASTER_RENDERER_PRIVATE_H

/*
 * Build-private and non-installed. The caller arena owns renderer state,
 * scratch, jobs and target sets; the renderer borrows BlobStore. All entry
 * points and geometry/hash helpers are single-threaded and non-reentrant.
 */

#include <meiso/composition/cpu_raster_renderer.h>
#include "../src/core/meiso_sha256_private.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_CPU_RASTER_RENDERER_MAGIC = 0x43505230U,
    MEISO_COMPOSITION_CPU_RASTER_RENDERER_TARGET_SLOT_COUNT =
        MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT,
    MEISO_COMPOSITION_CPU_RASTER_RENDERER_JOB_SLOT_COUNT =
        MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS,
    MEISO_COMPOSITION_CPU_RASTER_CLIP_PLANE_COUNT = 6,
    /* A convex n-gon gains at most one vertex per clipping half-space. */
    MEISO_COMPOSITION_CPU_RASTER_CLIP_VERTEX_CAPACITY = 12,
    MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_BITS = 8,
    MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE =
        1 << MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_BITS,
    MEISO_COMPOSITION_CPU_RASTER_FIXED_PIXEL_CENTER =
        MEISO_COMPOSITION_CPU_RASTER_FIXED_SUBPIXEL_SCALE / 2
};

typedef struct {
    bool present;
    MeisoCompositionCpuRasterJobRef ref;
    uint64_t source_clock_epoch;
    MeisoCompositionSceneDesired desired;
    uint32_t unique_mesh_count;
    MeisoContentId unique_meshes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
    uint32_t unique_mesh_sizes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
} MeisoCompositionCpuRasterRendererJob;

struct MeisoCompositionCpuRasterRenderer {
    uint32_t magic;
    size_t arena_size;
    MeisoBlobStore *blob_store;
    uint8_t *colors[MEISO_COMPOSITION_CPU_RASTER_RENDERER_TARGET_SLOT_COUNT];
    uint8_t *depths[MEISO_COMPOSITION_CPU_RASTER_RENDERER_TARGET_SLOT_COUNT];
    uint64_t renderer_generation;
    uint64_t job_id_high_water;
    bool scope_bound;
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionCpuRasterRendererJob
        jobs[MEISO_COMPOSITION_CPU_RASTER_RENDERER_JOB_SLOT_COUNT];
    MeisoCompositionCpuRasterRendererJob validation_scratch;
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
    double position[3];
    double orientation_xyzw[4];
    double focal_y;
    double focal_x;
    double projection_a;
    double projection_b;
} MeisoCompositionCpuRasterCamera;

typedef struct {
    double clip[4];
    double color[4];
} MeisoCompositionCpuRasterClipVertex;

typedef struct {
    int32_t x_fixed;
    int32_t y_fixed;
    double ndc_x;
    double ndc_y;
    double ndc_z;
    double inverse_w;
    double color_over_w[4];
} MeisoCompositionCpuRasterScreenVertex;

typedef struct {
    uint64_t submitted;
    uint64_t clipped;
    uint64_t culled;
    uint64_t rasterized;
    uint64_t fragments_depth_passed;
} MeisoCompositionCpuRasterWorkCounts;

bool meiso_composition_cpu_raster_renderer_private_ranges_overlap(
    const void *lhs, size_t lhs_size, const void *rhs, size_t rhs_size);
void meiso_composition_cpu_raster_renderer_private_increment_saturated(
    uint64_t *value);
void meiso_composition_cpu_raster_renderer_private_add_saturated(
    uint64_t *value, uint64_t addition);
bool meiso_composition_cpu_raster_renderer_private_platform_supported(void);
bool meiso_composition_cpu_raster_renderer_private_content_id_equal(
    const MeisoContentId *lhs, const MeisoContentId *rhs);
bool meiso_composition_cpu_raster_renderer_private_overlaps(
    const MeisoCompositionCpuRasterRenderer *renderer, const void *value,
    size_t value_size);
bool meiso_composition_cpu_raster_renderer_private_valid(
    const MeisoCompositionCpuRasterRenderer *renderer);
MeisoCompositionCpuRasterRendererJob *
meiso_composition_cpu_raster_renderer_private_queue_head(
    MeisoCompositionCpuRasterRenderer *renderer);
bool meiso_composition_cpu_raster_renderer_private_job_ref_equal(
    const MeisoCompositionCpuRasterJobRef *lhs,
    const MeisoCompositionCpuRasterJobRef *rhs);
void meiso_composition_cpu_raster_renderer_private_pop_head(
    MeisoCompositionCpuRasterRenderer *renderer);
MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_private_validate_job_blobs(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterRendererJob *job);
void meiso_composition_cpu_raster_renderer_private_make_complete_view(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterCompleteView *out_complete);

uint32_t
meiso_composition_cpu_raster_renderer_private_read_be_u32(const uint8_t *input);
void meiso_composition_cpu_raster_renderer_private_write_be_u32(uint8_t *output,
                                                                uint32_t value);

bool meiso_composition_cpu_raster_renderer_private_prepare_camera(
    const MeisoCompositionSceneCamera *source,
    MeisoCompositionCpuRasterCamera *out_camera);
MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_private_process_geometry(
    const MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterRendererJob *job,
    const MeisoCompositionCpuRasterCamera *camera, bool draw,
    uint8_t *color_target, uint8_t *depth_target,
    MeisoCompositionCpuRasterWorkCounts *counts);

#endif
