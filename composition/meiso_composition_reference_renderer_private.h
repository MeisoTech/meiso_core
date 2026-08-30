#ifndef MEISO_COMPOSITION_REFERENCE_RENDERER_PRIVATE_H
#define MEISO_COMPOSITION_REFERENCE_RENDERER_PRIVATE_H

/*
 * Build-private and non-installed. The caller arena owns renderer metadata,
 * queued jobs and both target sets; the renderer borrows BlobStore. Entry
 * points and helpers are single-threaded and non-reentrant.
 */

#include <meiso/composition/reference_renderer.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_MAGIC = 0x43525230U,
    MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT =
        MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT,
    MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT =
        MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS
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
} MeisoCompositionReferenceRendererJob;

struct MeisoCompositionReferenceRenderer {
    uint32_t magic;
    size_t arena_size;
    MeisoBlobStore *blob_store;
    uint8_t *colors[MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT];
    uint8_t *depths[MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT];
    uint64_t renderer_generation;
    uint64_t job_id_high_water;
    bool scope_bound;
    MeisoCompositionSceneControlScope scope;
    MeisoCompositionReferenceRendererJob
        jobs[MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT];
    MeisoCompositionReferenceRendererJob validation_scratch;
    MeisoCompositionSceneRenderResult result_scratch;
    uint32_t queue_head;
    uint32_t queue_count;
    bool complete_available;
    uint32_t complete_slot;
    MeisoCompositionReferenceRendererJobRef latest_complete_job;
    MeisoCompositionSceneRenderResult latest_result;
    MeisoCompositionReferenceRendererStats stats;
};

void meiso_composition_reference_renderer_private_increment_saturated(
    uint64_t *value);
bool meiso_composition_reference_renderer_private_ranges_overlap(
    const void *lhs, size_t lhs_size, const void *rhs, size_t rhs_size);
bool meiso_composition_reference_renderer_private_content_id_equal(
    const MeisoContentId *lhs, const MeisoContentId *rhs);
bool meiso_composition_reference_renderer_private_overlaps(
    const MeisoCompositionReferenceRenderer *renderer, const void *value,
    size_t value_size);
bool meiso_composition_reference_renderer_private_valid(
    const MeisoCompositionReferenceRenderer *renderer);
MeisoCompositionReferenceRendererJob *
meiso_composition_reference_renderer_private_queue_head(
    MeisoCompositionReferenceRenderer *renderer);
bool meiso_composition_reference_renderer_private_job_ref_equal(
    const MeisoCompositionReferenceRendererJobRef *lhs,
    const MeisoCompositionReferenceRendererJobRef *rhs);
void meiso_composition_reference_renderer_private_pop_head(
    MeisoCompositionReferenceRenderer *renderer);
void meiso_composition_reference_renderer_private_make_complete_view(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete);

#endif
