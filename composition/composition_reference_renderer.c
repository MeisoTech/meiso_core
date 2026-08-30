#include "meiso_composition_reference_renderer_private.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Renderer lifecycle owns arena layout, queue admission and retirement. */

_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT == 2U,
               "reference renderer requires 2 targets");
_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT == 2U,
               "reference renderer requires 2 jobs");
_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_EVIDENCE_FLAGS ==
                   UINT32_C(0x00000003),
               "reference evidence must be exact 0x3");
_Static_assert(MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES ==
                   UINT32_C(1228800),
               "reference target allocation changed");

void meiso_composition_reference_renderer_private_increment_saturated(
    uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

bool meiso_composition_reference_renderer_private_ranges_overlap(
    const void *lhs, size_t lhs_size, const void *rhs, size_t rhs_size)
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

static bool scope_equal(const MeisoCompositionSceneControlScope *lhs,
                        const MeisoCompositionSceneControlScope *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
           lhs->control_scope_generation == rhs->control_scope_generation;
}

static bool world_ref_equal(const MeisoCompositionSceneWorldRef *lhs,
                            const MeisoCompositionSceneWorldRef *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
           lhs->desired_state_version == rhs->desired_state_version &&
           lhs->plan_generation == rhs->plan_generation &&
           lhs->world_generation == rhs->world_generation;
}

bool meiso_composition_reference_renderer_private_job_ref_equal(
    const MeisoCompositionReferenceRendererJobRef *lhs,
    const MeisoCompositionReferenceRendererJobRef *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->renderer_generation == rhs->renderer_generation &&
           lhs->job_id == rhs->job_id &&
           scope_equal(&lhs->control_scope, &rhs->control_scope) &&
           world_ref_equal(&lhs->world, &rhs->world);
}

bool meiso_composition_reference_renderer_private_content_id_equal(
    const MeisoContentId *lhs, const MeisoContentId *rhs)
{
    return lhs != NULL && rhs != NULL && meiso_content_id_equal(lhs, rhs);
}

bool meiso_composition_reference_renderer_private_overlaps(
    const MeisoCompositionReferenceRenderer *renderer, const void *value,
    size_t value_size)
{
    return renderer != NULL &&
           meiso_composition_reference_renderer_private_ranges_overlap(
               renderer, renderer->arena_size, value, value_size);
}

bool meiso_composition_reference_renderer_private_valid(
    const MeisoCompositionReferenceRenderer *renderer)
{
    MeisoBlobStats ignored;
    const size_t first_target =
        align_up(sizeof(*renderer),
                 meiso_composition_reference_renderer_alignment_required());
    const uint8_t *base = (const uint8_t *)renderer;
    size_t offset = first_target;
    uint32_t slot;

    if (renderer == NULL ||
        renderer->magic != MEISO_COMPOSITION_REFERENCE_RENDERER_MAGIC ||
        renderer->arena_size <
            meiso_composition_reference_renderer_memory_required() ||
        renderer->blob_store == NULL ||
        renderer->queue_head >=
            MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT ||
        renderer->queue_count >
            MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT ||
        renderer->complete_slot >=
            MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT ||
        ((renderer->renderer_generation == 0U) != !renderer->scope_bound) ||
        (renderer->scope_bound && !scope_valid(&renderer->scope))) {
        return false;
    }
    for (slot = 0U;
         slot < MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT;
         ++slot) {
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

MeisoCompositionReferenceRendererJob *
meiso_composition_reference_renderer_private_queue_head(
    MeisoCompositionReferenceRenderer *renderer)
{
    return renderer->queue_count == 0U ? NULL
                                       : &renderer->jobs[renderer->queue_head];
}

static const MeisoCompositionReferenceRendererJob *
const_queue_head_job(const MeisoCompositionReferenceRenderer *renderer)
{
    return renderer->queue_count == 0U ? NULL
                                       : &renderer->jobs[renderer->queue_head];
}

static void clear_jobs(MeisoCompositionReferenceRenderer *renderer)
{
    memset(renderer->jobs, 0, sizeof(renderer->jobs));
    renderer->queue_head = 0U;
    renderer->queue_count = 0U;
}

void meiso_composition_reference_renderer_private_pop_head(
    MeisoCompositionReferenceRenderer *renderer)
{
    memset(&renderer->jobs[renderer->queue_head], 0,
           sizeof(renderer->jobs[renderer->queue_head]));
    renderer->queue_head = (renderer->queue_head + 1U) %
                           MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT;
    --renderer->queue_count;
    if (renderer->queue_count == 0U) {
        renderer->queue_head = 0U;
    }
}

static MeisoCompositionReferenceRendererSubmitStatus
validate_world(const MeisoCompositionReferenceRenderer *renderer,
               const MeisoCompositionSceneControlScope *scope,
               const MeisoCompositionSceneWorldView *world,
               MeisoCompositionReferenceRendererJob *out_job)
{
    uint32_t object_index;
    uint64_t resident_bytes = 0U;
    size_t canonical_size = 0U;

    memset(out_job, 0, sizeof(*out_job));
    if (world->desired == NULL || world->ref.desired_owner_incarnation == 0U ||
        world->ref.desired_state_version == 0U ||
        world->ref.plan_generation == 0U || world->ref.world_generation == 0U ||
        world->ref.desired_owner_incarnation !=
            scope->desired_owner_incarnation ||
        world->object_count != world->desired->object_count ||
        world->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS ||
        world->unique_mesh_count > MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES ||
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

        for (unique_index = 0U; unique_index < out_job->unique_mesh_count;
             ++unique_index) {
            if (meiso_composition_reference_renderer_private_content_id_equal(
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
                return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
            }
            if (!meiso_blob_store_lookup(renderer->blob_store, content_id,
                                         &blob)) {
                return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_MISSING_BLOB;
            }
            if (!meiso_composition_reference_renderer_private_content_id_equal(
                    content_id, &blob.content_id) ||
                blob.content.size >
                    MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
                !meiso_composition_scene_mesh_blob_verify(content_id,
                                                          blob.content, NULL) ||
                meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                    MEISO_COMPOSITION_SCENE_CODEC_OK ||
                mesh.vertex_records.size > SIZE_MAX - mesh.index_records.size) {
                return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_MESH;
            }
            mesh_resident = mesh.vertex_records.size + mesh.index_records.size;
            if (resident_bytes >
                MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD -
                    mesh_resident) {
                return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD;
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

void meiso_composition_reference_renderer_private_make_complete_view(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete)
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
    const size_t base_size =
        align_up(sizeof(MeisoCompositionReferenceRenderer), alignment);

    return base_size + MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
}

MeisoCompositionReferenceRendererInitStatus
meiso_composition_reference_renderer_init(
    void *arena, size_t arena_size, MeisoBlobStore *blob_store,
    MeisoCompositionReferenceRenderer **out_renderer)
{
    MeisoCompositionReferenceRenderer *renderer;
    MeisoBlobStats ignored;
    size_t offset;
    uint32_t slot;

    if (arena == NULL || out_renderer == NULL ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            arena, arena_size, out_renderer, sizeof(*out_renderer))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_ARGUMENT;
    }
    if (blob_store == NULL || !meiso_blob_store_stats(blob_store, &ignored)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_BLOB_STORE;
    }
    if ((uintptr_t)arena %
            meiso_composition_reference_renderer_alignment_required() !=
        0U) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < meiso_composition_reference_renderer_memory_required()) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_ARENA_TOO_SMALL;
    }
    *out_renderer = NULL;
    memset(arena, 0, arena_size);
    renderer = (MeisoCompositionReferenceRenderer *)arena;
    renderer->magic = MEISO_COMPOSITION_REFERENCE_RENDERER_MAGIC;
    renderer->arena_size = arena_size;
    renderer->blob_store = blob_store;
    renderer->complete_slot = 0U;
    offset =
        align_up(sizeof(*renderer),
                 meiso_composition_reference_renderer_alignment_required());
    for (slot = 0U;
         slot < MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT;
         ++slot) {
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
    const MeisoCompositionSceneControlScope *scope)
{
    uint32_t slot;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        !scope_valid(scope) ||
        meiso_composition_reference_renderer_private_overlaps(renderer, scope,
                                                              sizeof(*scope))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_INVALID_ARGUMENT;
    }
    if (renderer->scope_bound && scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_UNCHANGED;
    }
    if (renderer->renderer_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_COUNTER_EXHAUSTED;
    }
    clear_jobs(renderer);
    for (slot = 0U;
         slot < MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT;
         ++slot) {
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
    meiso_composition_reference_renderer_private_increment_saturated(
        &renderer->stats.resets);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED;
}

MeisoCompositionReferenceRendererSubmitStatus
meiso_composition_reference_renderer_submit(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world, uint64_t source_clock_epoch,
    MeisoCompositionReferenceRendererJobRef *out_job)
{
    MeisoCompositionReferenceRendererJob *candidate;
    MeisoCompositionReferenceRendererSubmitStatus status;
    uint32_t tail;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        scope == NULL || world == NULL || world->desired == NULL ||
        out_job == NULL || source_clock_epoch == 0U ||
        meiso_composition_reference_renderer_private_overlaps(renderer, scope,
                                                              sizeof(*scope)) ||
        meiso_composition_reference_renderer_private_overlaps(renderer, world,
                                                              sizeof(*world)) ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, world->desired, sizeof(*world->desired)) ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, out_job, sizeof(*out_job)) ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            scope, sizeof(*scope), out_job, sizeof(*out_job)) ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            world, sizeof(*world), out_job, sizeof(*out_job)) ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            world->desired, sizeof(*world->desired), out_job,
            sizeof(*out_job))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_SCOPE_MISMATCH;
    }
    if (renderer->queue_count ==
        MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT) {
        meiso_composition_reference_renderer_private_increment_saturated(
            &renderer->stats.queue_capacity_rejections);
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUE_FULL;
    }
    if (renderer->job_id_high_water == UINT64_MAX) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_COUNTER_EXHAUSTED;
    }
    candidate = &renderer->validation_scratch;
    status = validate_world(renderer, scope, world, candidate);
    if (status != MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED) {
        if (status ==
            MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_MISSING_BLOB) {
            meiso_composition_reference_renderer_private_increment_saturated(
                &renderer->stats.missing_blob_rejections);
        } else if (status ==
                   MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_MESH) {
            meiso_composition_reference_renderer_private_increment_saturated(
                &renderer->stats.invalid_mesh_rejections);
        } else {
            meiso_composition_reference_renderer_private_increment_saturated(
                &renderer->stats.invalid_world_rejections);
        }
        return status;
    }
    candidate->present = true;
    candidate->source_clock_epoch = source_clock_epoch;
    candidate->ref.renderer_generation = renderer->renderer_generation;
    candidate->ref.job_id = renderer->job_id_high_water + 1U;
    candidate->ref.control_scope = renderer->scope;
    candidate->ref.world = world->ref;
    tail = (renderer->queue_head + renderer->queue_count) %
           MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT;
    renderer->jobs[tail] = *candidate;
    ++renderer->queue_count;
    renderer->job_id_high_water = candidate->ref.job_id;
    meiso_composition_reference_renderer_private_increment_saturated(
        &renderer->stats.jobs_submitted);
    *out_job = candidate->ref;
    return MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED;
}

MeisoCompositionReferenceRendererDiscardStatus
meiso_composition_reference_renderer_discard_head(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job)
{
    const MeisoCompositionReferenceRendererJob *job;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        expected_job == NULL ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, expected_job, sizeof(*expected_job))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_INVALID_ARGUMENT;
    }
    job = const_queue_head_job(renderer);
    if (job == NULL) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_NO_JOB;
    }
    if (!meiso_composition_reference_renderer_private_job_ref_equal(
            &job->ref, expected_job)) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_JOB_MISMATCH;
    }
    meiso_composition_reference_renderer_private_pop_head(renderer);
    meiso_composition_reference_renderer_private_increment_saturated(
        &renderer->stats.jobs_discarded);
    return MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED;
}

MeisoCompositionReferenceRendererRetireStatus
meiso_composition_reference_renderer_retire_world(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *retired_world)
{
    uint32_t offset = 0U;
    uint64_t jobs_retired = 0U;
    bool complete_retired = false;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        !scope_valid(scope) || retired_world == NULL ||
        retired_world->desired_owner_incarnation == 0U ||
        retired_world->desired_state_version == 0U ||
        retired_world->plan_generation == 0U ||
        retired_world->world_generation == 0U ||
        meiso_composition_reference_renderer_private_overlaps(renderer, scope,
                                                              sizeof(*scope)) ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, retired_world, sizeof(*retired_world)) ||
        meiso_composition_reference_renderer_private_ranges_overlap(
            scope, sizeof(*scope), retired_world, sizeof(*retired_world))) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_INVALID_ARGUMENT;
    }
    if (!renderer->scope_bound || !scope_equal(&renderer->scope, scope) ||
        retired_world->desired_owner_incarnation !=
            scope->desired_owner_incarnation) {
        return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_SCOPE_MISMATCH;
    }

    while (offset < renderer->queue_count) {
        const uint32_t index =
            (renderer->queue_head + offset) %
            MEISO_COMPOSITION_REFERENCE_RENDERER_JOB_SLOT_COUNT;

        if (world_ref_equal(&renderer->jobs[index].ref.world, retired_world)) {
            if (offset == 0U) {
                meiso_composition_reference_renderer_private_pop_head(renderer);
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
        world_ref_equal(&renderer->latest_complete_job.world, retired_world)) {
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
    meiso_composition_reference_renderer_private_increment_saturated(
        &renderer->stats.world_retires);
    while (jobs_retired != 0U) {
        meiso_composition_reference_renderer_private_increment_saturated(
            &renderer->stats.jobs_retired);
        --jobs_retired;
    }
    if (complete_retired) {
        meiso_composition_reference_renderer_private_increment_saturated(
            &renderer->stats.completes_retired);
    }
    return MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED;
}

bool meiso_composition_reference_renderer_latest_complete(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete)
{
    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        out_complete == NULL ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, out_complete, sizeof(*out_complete)) ||
        !renderer->complete_available) {
        return false;
    }
    meiso_composition_reference_renderer_private_make_complete_view(
        renderer, out_complete);
    return true;
}

bool meiso_composition_reference_renderer_snapshot(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererSnapshot *out_snapshot)
{
    MeisoCompositionReferenceRendererSnapshot snapshot;
    const MeisoCompositionReferenceRendererJob *head;

    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        out_snapshot == NULL ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, out_snapshot, sizeof(*out_snapshot))) {
        return false;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.renderer_generation = renderer->renderer_generation;
    snapshot.control_scope_bound = renderer->scope_bound;
    snapshot.control_scope = renderer->scope;
    snapshot.queued_jobs = renderer->queue_count;
    snapshot.target_slots_allocated =
        MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SLOT_COUNT;
    snapshot.target_bytes = MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
    snapshot.renderer_metadata_bytes =
        renderer->arena_size -
        MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES;
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

bool meiso_composition_reference_renderer_stats(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererStats *out_stats)
{
    if (!meiso_composition_reference_renderer_private_valid(renderer) ||
        out_stats == NULL ||
        meiso_composition_reference_renderer_private_overlaps(
            renderer, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = renderer->stats;
    return true;
}
