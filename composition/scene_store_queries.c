#include "meiso_composition_scene_store_private.h"

/* SceneStore queries project borrowed world/status views without mutation. */

#include <string.h>

static bool world_view(const MeisoCompositionSceneStore *store,
                       const MeisoCompositionSceneStoreWorld *world,
                       MeisoCompositionSceneWorldView *out_view)
{
    if (world == NULL || out_view == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_view, sizeof(*out_view))) {
        return false;
    }
    out_view->ref = world->ref;
    out_view->desired = &world->desired;
    out_view->object_count = world->desired.object_count;
    out_view->unique_mesh_count = world->unique_mesh_count;
    out_view->resident_mesh_bytes = world->resident_mesh_bytes;
    return true;
}

bool meiso_composition_scene_store_active(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneWorldView *out_view)
{
    if (!meiso_composition_scene_store_private_valid(store)) {
        return false;
    }
    return world_view(
        store, meiso_composition_scene_store_private_const_active_world(store),
        out_view);
}

bool meiso_composition_scene_store_candidate(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneWorldView *out_view)
{
    if (!meiso_composition_scene_store_private_valid(store)) {
        return false;
    }
    return world_view(
        store,
        meiso_composition_scene_store_private_const_candidate_world(store),
        out_view);
}

bool meiso_composition_scene_store_observation(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneObservation *out_observation)
{
    if (!meiso_composition_scene_store_private_valid(store) ||
        out_observation == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_observation, sizeof(*out_observation)) ||
        !store->has_observation) {
        return false;
    }
    *out_observation = store->observation;
    return true;
}

bool meiso_composition_scene_store_status(
    const MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneResourceUsage *resource_usage,
    MeisoCompositionSceneStatus *out_status)
{
    const MeisoCompositionSceneStoreWorld *active;
    const MeisoCompositionSceneStoreWorld *candidate;
    uint64_t resident_bytes = 0U;

    const uint64_t bytes_per_target_set =
        (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 8U;

    if (!meiso_composition_scene_store_private_valid(store) ||
        resource_usage == NULL || out_status == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_status, sizeof(*out_status)) ||
        meiso_composition_scene_store_private_ranges_overlap(
            resource_usage, sizeof(*resource_usage), out_status,
            sizeof(*out_status)) ||
        resource_usage->published_blob_count !=
            meiso_blob_store_count(store->blob_store) ||
        resource_usage->published_blob_count >
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS ||
        resource_usage->published_blob_bytes >
            (uint64_t)resource_usage->published_blob_count *
                MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
        ((resource_usage->published_blob_count == 0U) !=
         (resource_usage->published_blob_bytes == 0U)) ||
        resource_usage->target_slots_used >
            MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT ||
        resource_usage->target_bytes !=
            bytes_per_target_set * resource_usage->target_slots_used ||
        resource_usage->render_jobs_in_flight >
            MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS ||
        resource_usage->render_jobs_in_flight >
            resource_usage->target_slots_used ||
        resource_usage->result_queue_depth >
            MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS) {
        return false;
    }
    memset(out_status, 0, sizeof(*out_status));
    active = meiso_composition_scene_store_private_const_active_world(store);
    candidate =
        meiso_composition_scene_store_private_const_candidate_world(store);
    if (!store->disabled && store->has_observation) {
        out_status->reason = store->observation.reason;
    }
    if (active != NULL) {
        out_status->active_plan_generation = active->ref.plan_generation;
        out_status->world_generation = active->ref.world_generation;
        out_status->active_object_count = active->desired.object_count;
        out_status->unique_mesh_count = active->unique_mesh_count;
        resident_bytes = active->resident_mesh_bytes;
        if (out_status->reason == MEISO_COMPOSITION_SCENE_REASON_NONE) {
            out_status->reason = MEISO_COMPOSITION_SCENE_REASON_READY;
        }
    }
    if (candidate != NULL) {
        resident_bytes += candidate->resident_mesh_bytes;
    }
    out_status->published_blob_count = resource_usage->published_blob_count;
    out_status->published_blob_bytes = resource_usage->published_blob_bytes;
    out_status->resident_mesh_bytes = resident_bytes;
    out_status->target_slots_used = resource_usage->target_slots_used;
    out_status->target_bytes = resource_usage->target_bytes;
    out_status->render_jobs_in_flight = resource_usage->render_jobs_in_flight;
    out_status->result_queue_depth = resource_usage->result_queue_depth;
    out_status->last_result_stream_epoch = store->last_result_stream_epoch;
    out_status->last_result_sequence = store->last_result_sequence;
    return true;
}

bool meiso_composition_scene_store_stats(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneStoreStats *out_stats)
{
    if (!meiso_composition_scene_store_private_valid(store) ||
        out_stats == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = store->stats;
    return true;
}
