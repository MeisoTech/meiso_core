#include "meiso_composition_scene_store_private.h"

/* SceneStore lifecycle owns bounded world mutation and Blob visibility. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void meiso_composition_scene_store_private_increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

static bool content_id_equal(const MeisoContentId *lhs,
                             const MeisoContentId *rhs)
{
    return meiso_content_id_equal(lhs, rhs);
}

bool meiso_composition_scene_store_private_ranges_overlap(const void *lhs,
                                                          size_t lhs_size,
                                                          const void *rhs,
                                                          size_t rhs_size)
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

bool meiso_composition_scene_store_private_output_overlaps(
    const MeisoCompositionSceneStore *store, const void *output,
    size_t output_size)
{
    return meiso_composition_scene_store_private_ranges_overlap(
        store, sizeof(*store), output, output_size);
}

bool meiso_composition_scene_store_private_world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs)
{
    return lhs != NULL && rhs != NULL &&
           lhs->desired_owner_incarnation == rhs->desired_owner_incarnation &&
           lhs->desired_state_version == rhs->desired_state_version &&
           lhs->plan_generation == rhs->plan_generation &&
           lhs->world_generation == rhs->world_generation;
}

static bool
desired_ref_equal(uint64_t owner_incarnation, uint64_t state_version,
                  const MeisoCompositionSceneObservation *observation)
{
    return observation != NULL &&
           owner_incarnation == observation->desired_owner_incarnation &&
           state_version == observation->desired_state_version;
}

bool meiso_composition_scene_store_private_valid(
    const MeisoCompositionSceneStore *store)
{
    MeisoBlobStats ignored;

    return store != NULL &&
           store->magic == MEISO_COMPOSITION_SCENE_STORE_MAGIC &&
           store->blob_store != NULL &&
           store->active_slot <
               MEISO_COMPOSITION_SCENE_STORE_WORLD_SLOT_COUNT &&
           (store->candidate_slot == MEISO_COMPOSITION_SCENE_STORE_NO_SLOT ||
            store->candidate_slot <
                MEISO_COMPOSITION_SCENE_STORE_WORLD_SLOT_COUNT) &&
           store->candidate_slot != store->active_slot &&
           ((store->control_scope_generation == 0U &&
             store->owner_incarnation == 0U) ||
            (store->control_scope_generation != 0U &&
             store->owner_incarnation != 0U)) &&
           store->tracked_blob_count <=
               MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS &&
           meiso_blob_store_stats(store->blob_store, &ignored);
}

static MeisoCompositionSceneStoreWorld *
active_world(MeisoCompositionSceneStore *store)
{
    MeisoCompositionSceneStoreWorld *world = &store->worlds[store->active_slot];

    return world->present ? world : NULL;
}

const MeisoCompositionSceneStoreWorld *
meiso_composition_scene_store_private_const_active_world(
    const MeisoCompositionSceneStore *store)
{
    const MeisoCompositionSceneStoreWorld *world =
        &store->worlds[store->active_slot];

    return world->present ? world : NULL;
}

static MeisoCompositionSceneStoreWorld *
candidate_world(MeisoCompositionSceneStore *store)
{
    if (store->candidate_slot == MEISO_COMPOSITION_SCENE_STORE_NO_SLOT) {
        return NULL;
    }
    return store->worlds[store->candidate_slot].present
               ? &store->worlds[store->candidate_slot]
               : NULL;
}

const MeisoCompositionSceneStoreWorld *
meiso_composition_scene_store_private_const_candidate_world(
    const MeisoCompositionSceneStore *store)
{
    if (store->candidate_slot == MEISO_COMPOSITION_SCENE_STORE_NO_SLOT) {
        return NULL;
    }
    return store->worlds[store->candidate_slot].present
               ? &store->worlds[store->candidate_slot]
               : NULL;
}

static void clear_world(MeisoCompositionSceneStoreWorld *world)
{
    if (world != NULL) {
        memset(world, 0, sizeof(*world));
    }
}

static bool discard_candidate_internal(MeisoCompositionSceneStore *store,
                                       bool count_discard)
{
    MeisoCompositionSceneStoreWorld *candidate = candidate_world(store);

    if (candidate == NULL) {
        store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
        return false;
    }
    clear_world(candidate);
    store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
    if (count_discard) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.candidate_discards);
    }
    return true;
}

static uint8_t available_candidate_slot(const MeisoCompositionSceneStore *store)
{
    return store->active_slot == 0U ? 1U : 0U;
}

static void set_observation(MeisoCompositionSceneStore *store,
                            uint64_t owner_incarnation, uint64_t state_version,
                            uint64_t plan_generation,
                            MeisoCompositionScenePrepareStatus prepare_status,
                            MeisoCompositionSceneStableReason reason,
                            bool has_canonical, size_t canonical_size)
{
    store->has_observation = true;
    store->observation.desired_owner_incarnation = owner_incarnation;
    store->observation.desired_state_version = state_version;
    store->observation.plan_generation = plan_generation;
    store->observation.prepare_status = prepare_status;
    store->observation.reason = reason;
    store->observation_has_canonical = has_canonical;
    store->observation_canonical_size = has_canonical ? canonical_size : 0U;
    if (has_canonical) {
        memcpy(store->observation_canonical, store->validation_wire,
               canonical_size);
    }
}

static int tracked_blob_index(const MeisoCompositionSceneStore *store,
                              const MeisoContentId *content_id)
{
    uint32_t index;

    for (index = 0U; index < store->tracked_blob_count; ++index) {
        if (content_id_equal(&store->tracked_blob_ids[index], content_id)) {
            return (int)index;
        }
    }
    return -1;
}

static bool build_world_mesh_set(MeisoCompositionSceneStore *store,
                                 MeisoCompositionSceneStoreWorld *world,
                                 MeisoCompositionScenePrepareStatus *out_status,
                                 MeisoCompositionSceneStableReason *out_reason)
{
    uint32_t object_index;

    world->unique_mesh_count = 0U;
    world->resident_mesh_bytes = 0U;
    for (object_index = 0U; object_index < world->desired.object_count;
         ++object_index) {
        const MeisoContentId *content_id =
            &world->desired.objects[object_index].mesh_content_id;
        uint32_t unique_index;
        bool seen = false;

        for (unique_index = 0U; unique_index < world->unique_mesh_count;
             ++unique_index) {
            if (content_id_equal(content_id,
                                 &world->unique_meshes[unique_index])) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            MeisoBlobView blob;
            MeisoCompositionSceneMeshView mesh;
            size_t resident_size;

            if (world->unique_mesh_count >=
                MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
                *out_reason = MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY;
                return false;
            }
            if (tracked_blob_index(store, content_id) < 0 ||
                !meiso_blob_store_lookup(store->blob_store, content_id,
                                         &blob)) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB;
                *out_reason = MEISO_COMPOSITION_SCENE_REASON_WAITING_BLOB;
                return false;
            }
            if (blob.content.size >
                MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY;
                return false;
            }
            if (!meiso_composition_scene_mesh_blob_verify(&blob.content_id,
                                                          blob.content, NULL) ||
                meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                    MEISO_COMPOSITION_SCENE_CODEC_OK) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH;
                *out_reason = MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE;
                return false;
            }
            if (mesh.vertex_records.size > SIZE_MAX - mesh.index_records.size) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY;
                return false;
            }
            resident_size = mesh.vertex_records.size + mesh.index_records.size;
            if (world->resident_mesh_bytes >
                MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD -
                    resident_size) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY;
                return false;
            }
            unique_index = world->unique_mesh_count++;
            world->unique_meshes[unique_index] = blob.content_id;
            world->unique_mesh_sizes[unique_index] =
                (uint32_t)blob.content.size;
            world->resident_mesh_bytes += resident_size;
        }
    }
    return true;
}

static MeisoCompositionSceneStableReason
desired_capacity_preflight(const MeisoCompositionSceneDesired *desired)
{
    uint32_t unique_count = 0U;
    uint32_t index;

    if (desired->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS) {
        return MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY;
    }
    for (index = 0U; index < desired->object_count; ++index) {
        uint32_t previous;
        bool seen = false;

        if (!meiso_content_id_valid(&desired->objects[index].mesh_content_id)) {
            return MEISO_COMPOSITION_SCENE_REASON_NONE;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (content_id_equal(&desired->objects[index].mesh_content_id,
                                 &desired->objects[previous].mesh_content_id)) {
                seen = true;
                break;
            }
        }
        if (!seen &&
            ++unique_count > MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
            return MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY;
        }
    }
    return MEISO_COMPOSITION_SCENE_REASON_NONE;
}

size_t meiso_composition_scene_store_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_composition_scene_store_memory_required(void)
{
    return sizeof(MeisoCompositionSceneStore);
}

MeisoCompositionSceneStoreInitStatus
meiso_composition_scene_store_init(void *arena, size_t arena_size,
                                   MeisoBlobStore *blob_store,
                                   MeisoCompositionSceneStore **out_store)
{
    MeisoBlobStats ignored;
    MeisoCompositionSceneStore *store;

    if (out_store == NULL) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT;
    }
    if (meiso_composition_scene_store_private_ranges_overlap(
            arena, arena_size, out_store, sizeof(*out_store))) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT;
    }
    *out_store = NULL;
    if (arena == NULL || blob_store == NULL) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT;
    }
    if (!meiso_blob_store_stats(blob_store, &ignored)) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_BLOB_STORE;
    }
    if ((uintptr_t)arena % meiso_composition_scene_store_alignment_required() !=
        0U) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(*store)) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, sizeof(*store));
    store = arena;
    store->magic = MEISO_COMPOSITION_SCENE_STORE_MAGIC;
    store->blob_store = blob_store;
    store->active_slot = 0U;
    store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
    store->disabled = true;
    *out_store = store;
    return MEISO_COMPOSITION_SCENE_STORE_INIT_OK;
}

static MeisoCompositionSceneOwnerStatus
reset_control_scope(MeisoCompositionSceneStore *store,
                    uint64_t desired_owner_incarnation)
{
    if (!meiso_composition_scene_store_private_valid(store) ||
        desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT;
    }
    if (store->control_scope_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED;
    }
    clear_world(&store->worlds[0]);
    clear_world(&store->worlds[1]);
    store->active_slot = 0U;
    store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
    store->owner_incarnation = desired_owner_incarnation;
    ++store->control_scope_generation;
    store->disabled = true;
    store->has_observation = false;
    store->observation_has_canonical = false;
    memset(&store->observation, 0, sizeof(store->observation));
    store->observation_canonical_size = 0U;
    memset(store->observation_canonical, 0,
           sizeof(store->observation_canonical));
    memset(&store->validation_desired, 0, sizeof(store->validation_desired));
    memset(store->validation_wire, 0, sizeof(store->validation_wire));
    store->tracked_blob_count = 0U;
    store->tracked_blob_bytes = 0U;
    memset(store->tracked_blob_ids, 0, sizeof(store->tracked_blob_ids));
    memset(store->tracked_blob_sizes, 0, sizeof(store->tracked_blob_sizes));
    store->last_result_stream_epoch = 0U;
    store->last_result_sequence = 0U;
    return MEISO_COMPOSITION_SCENE_OWNER_STARTED;
}

MeisoCompositionSceneOwnerStatus
meiso_composition_scene_store_begin_owner(MeisoCompositionSceneStore *store,
                                          uint64_t desired_owner_incarnation)
{
    if (!meiso_composition_scene_store_private_valid(store) ||
        desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT;
    }
    if (store->owner_incarnation == desired_owner_incarnation) {
        return MEISO_COMPOSITION_SCENE_OWNER_UNCHANGED;
    }
    if (store->control_scope_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED;
    }
    if (store->owner_incarnation == 0U) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.owner_starts);
    } else {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.owner_changes);
    }
    return reset_control_scope(store, desired_owner_incarnation);
}

MeisoCompositionSceneOwnerStatus
meiso_composition_scene_store_reset_control_scope(
    MeisoCompositionSceneStore *store, uint64_t desired_owner_incarnation)
{
    MeisoCompositionSceneOwnerStatus status;
    bool same_owner;

    if (!meiso_composition_scene_store_private_valid(store) ||
        desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT;
    }
    if (store->control_scope_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED;
    }
    same_owner = store->owner_incarnation == desired_owner_incarnation;
    if (store->owner_incarnation == 0U) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.owner_starts);
    } else if (same_owner) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.control_scope_resets);
    } else {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.owner_changes);
    }
    status = reset_control_scope(store, desired_owner_incarnation);
    if (status != MEISO_COMPOSITION_SCENE_OWNER_STARTED) {
        return status;
    }
    return same_owner ? MEISO_COMPOSITION_SCENE_OWNER_SCOPE_RESET : status;
}

bool meiso_composition_scene_store_control_scope(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneControlScope *out_scope)
{
    if (!meiso_composition_scene_store_private_valid(store) ||
        out_scope == NULL ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_scope, sizeof(*out_scope))) {
        return false;
    }
    out_scope->desired_owner_incarnation = store->owner_incarnation;
    out_scope->control_scope_generation = store->control_scope_generation;
    return true;
}

MeisoCompositionSceneBlobVisibilityStatus
meiso_composition_scene_store_mark_blob_visible(
    MeisoCompositionSceneStore *store, uint64_t desired_owner_incarnation,
    const MeisoContentId *content_id, size_t content_size)
{
    MeisoBlobView blob;
    MeisoCompositionSceneMeshView mesh;
    int existing;

    if (!meiso_composition_scene_store_private_valid(store) ||
        content_id == NULL || desired_owner_incarnation == 0U ||
        desired_owner_incarnation != store->owner_incarnation ||
        !meiso_content_id_valid(content_id) ||
        content_size > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE) {
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_ARGUMENT;
    }
    if (!meiso_blob_store_lookup(store->blob_store, content_id, &blob)) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_NOT_PUBLISHED;
    }
    if (blob.content.size != content_size) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_LENGTH_MISMATCH;
    }
    if (!meiso_composition_scene_mesh_content_id(blob.content,
                                                 &blob.content_id)) {
        if (meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
            MEISO_COMPOSITION_SCENE_CODEC_OK) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.blob_visibility_rejections);
            return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_MESH;
        }
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INTEGRITY_FAILED;
    }
    if (!content_id_equal(content_id, &blob.content_id)) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INTEGRITY_FAILED;
    }
    existing = tracked_blob_index(store, content_id);
    if (existing >= 0) {
        if (store->tracked_blob_sizes[(uint32_t)existing] != content_size) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.blob_visibility_rejections);
            return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_LENGTH_MISMATCH;
        }
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_duplicates);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED;
    }
    if (store->tracked_blob_count >=
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS ||
        store->tracked_blob_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL -
                content_size) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_CAPACITY;
    }
    store->tracked_blob_ids[store->tracked_blob_count] = *content_id;
    store->tracked_blob_sizes[store->tracked_blob_count] =
        (uint32_t)content_size;
    ++store->tracked_blob_count;
    store->tracked_blob_bytes += content_size;
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.blobs_made_visible);
    return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE;
}

MeisoCompositionScenePrepareStatus meiso_composition_scene_store_prepare(
    MeisoCompositionSceneStore *store, uint64_t desired_owner_incarnation,
    uint64_t desired_state_version, uint64_t plan_generation,
    const MeisoCompositionSceneDesired *desired)
{
    MeisoCompositionSceneCodecStatus codec_status;
    MeisoCompositionScenePrepareStatus prepare_status =
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED;
    MeisoCompositionSceneStableReason reason =
        MEISO_COMPOSITION_SCENE_REASON_READY;
    MeisoCompositionSceneStoreWorld *candidate;
    const MeisoCompositionSceneStoreWorld *active;
    size_t encoded_size = 0U;
    bool same_observed_ref;
    bool same_observed_payload = false;
    bool different_observation;

    if (!meiso_composition_scene_store_private_valid(store) ||
        desired == NULL ||
        meiso_composition_scene_store_private_ranges_overlap(
            store, sizeof(*store), desired, sizeof(*desired)) ||
        desired_owner_incarnation == 0U ||
        desired_owner_incarnation != store->owner_incarnation ||
        desired_state_version == 0U || plan_generation == 0U) {
        return MEISO_COMPOSITION_SCENE_PREPARE_INVALID_ARGUMENT;
    }
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.prepare_calls);
    store->disabled = false;
    same_observed_ref =
        store->has_observation &&
        desired_ref_equal(desired_owner_incarnation, desired_state_version,
                          &store->observation);
    different_observation =
        !same_observed_ref ||
        store->observation.plan_generation != plan_generation;

    codec_status = meiso_composition_scene_desired_encode(
        desired,
        (MeisoMutableBuffer){store->validation_wire,
                             sizeof(store->validation_wire)},
        &encoded_size);
    if (codec_status == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        meiso_composition_scene_desired_decode(
            (MeisoByteView){store->validation_wire, encoded_size},
            &store->validation_desired) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        codec_status = MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    if (codec_status == MEISO_COMPOSITION_SCENE_CODEC_OK && same_observed_ref &&
        store->observation_has_canonical) {
        same_observed_payload =
            encoded_size == store->observation_canonical_size &&
            memcmp(store->validation_wire, store->observation_canonical,
                   encoded_size) == 0;
        if (!same_observed_payload) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.conflicts);
            return MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT;
        }
    } else if (same_observed_ref && store->observation_has_canonical) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.conflicts);
        return MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT;
    }

    if (different_observation) {
        if (discard_candidate_internal(store, false)) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.superseded_candidates);
        }
    }

    if (codec_status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        const MeisoCompositionSceneStableReason capacity_reason =
            desired_capacity_preflight(desired);

        if (capacity_reason != MEISO_COMPOSITION_SCENE_REASON_NONE) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.capacity_rejections);
            set_observation(store, desired_owner_incarnation,
                            desired_state_version, plan_generation,
                            MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY,
                            capacity_reason, false, 0U);
            return MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
        }
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.invalid_desired);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED,
            MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE, false, 0U);
        return MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED;
    }

    candidate = candidate_world(store);
    if (candidate != NULL && same_observed_payload &&
        candidate->ref.desired_owner_incarnation == desired_owner_incarnation &&
        candidate->ref.desired_state_version == desired_state_version &&
        candidate->ref.plan_generation == plan_generation) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.unchanged);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED,
            MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size);
        return MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED;
    }
    active = meiso_composition_scene_store_private_const_active_world(store);
    if (active != NULL && same_observed_payload &&
        active->ref.desired_owner_incarnation == desired_owner_incarnation &&
        active->ref.desired_state_version == desired_state_version &&
        active->ref.plan_generation == plan_generation) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.already_active);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE,
            MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size);
        return MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE;
    }

    if (candidate != NULL && !different_observation) {
        clear_world(candidate);
        store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
    }
    store->candidate_slot = available_candidate_slot(store);
    candidate = &store->worlds[store->candidate_slot];
    clear_world(candidate);
    candidate->desired = store->validation_desired;
    candidate->canonical_size = encoded_size;
    memcpy(candidate->canonical, store->validation_wire, encoded_size);
    candidate->ref.desired_owner_incarnation = desired_owner_incarnation;
    candidate->ref.desired_state_version = desired_state_version;
    candidate->ref.plan_generation = plan_generation;

    if (!build_world_mesh_set(store, candidate, &prepare_status, &reason)) {
        clear_world(candidate);
        store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
        if (prepare_status == MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.missing_blobs);
        } else if (prepare_status ==
                   MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH) {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.invalid_meshes);
        } else {
            meiso_composition_scene_store_private_increment_saturated(
                &store->stats.capacity_rejections);
        }
        set_observation(store, desired_owner_incarnation, desired_state_version,
                        plan_generation, prepare_status, reason, true,
                        encoded_size);
        return prepare_status;
    }
    active = meiso_composition_scene_store_private_const_active_world(store);
    if (active != NULL &&
        active->resident_mesh_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL -
                candidate->resident_mesh_bytes) {
        clear_world(candidate);
        store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.capacity_rejections);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY,
            MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY, true, encoded_size);
        return MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
    }

    candidate->present = true;
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.prepared);
    set_observation(store, desired_owner_incarnation, desired_state_version,
                    plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_PREPARED,
                    MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size);
    return MEISO_COMPOSITION_SCENE_PREPARE_PREPARED;
}

MeisoCompositionSceneActivateStatus meiso_composition_scene_store_activate(
    MeisoCompositionSceneStore *store,
    uint64_t expected_desired_owner_incarnation,
    uint64_t expected_desired_state_version, uint64_t expected_plan_generation,
    MeisoCompositionSceneWorldRef *out_active_ref)
{
    MeisoCompositionSceneStoreWorld *candidate;
    MeisoCompositionSceneStoreWorld *old_active;
    uint8_t old_active_slot;

    if (!meiso_composition_scene_store_private_valid(store) ||
        out_active_ref == NULL || expected_desired_owner_incarnation == 0U ||
        expected_desired_state_version == 0U ||
        expected_plan_generation == 0U ||
        meiso_composition_scene_store_private_output_overlaps(
            store, out_active_ref, sizeof(*out_active_ref))) {
        return MEISO_COMPOSITION_SCENE_ACTIVATE_INVALID_ARGUMENT;
    }
    memset(out_active_ref, 0, sizeof(*out_active_ref));
    candidate = candidate_world(store);
    if (candidate == NULL) {
        return MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE;
    }
    if (candidate->ref.desired_owner_incarnation !=
            expected_desired_owner_incarnation ||
        candidate->ref.desired_state_version !=
            expected_desired_state_version ||
        candidate->ref.plan_generation != expected_plan_generation) {
        meiso_composition_scene_store_private_increment_saturated(
            &store->stats.activation_mismatches);
        return MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH;
    }
    if (store->world_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_SCENE_ACTIVATE_COUNTER_EXHAUSTED;
    }

    old_active_slot = store->active_slot;
    old_active = &store->worlds[old_active_slot];
    store->active_slot = store->candidate_slot;
    store->candidate_slot = MEISO_COMPOSITION_SCENE_STORE_NO_SLOT;
    candidate->ref.world_generation = ++store->world_generation;
    store->disabled = false;
    clear_world(old_active);
    *out_active_ref = candidate->ref;
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.activations);
    return MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED;
}

bool meiso_composition_scene_store_discard_candidate(
    MeisoCompositionSceneStore *store)
{
    if (!meiso_composition_scene_store_private_valid(store)) {
        return false;
    }
    return discard_candidate_internal(store, true);
}

MeisoCompositionSceneDisableStatus meiso_composition_scene_store_disable(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *expected_active)
{
    MeisoCompositionSceneStoreWorld *active;

    if (!meiso_composition_scene_store_private_valid(store)) {
        return MEISO_COMPOSITION_SCENE_DISABLE_INVALID_ARGUMENT;
    }
    active = active_world(store);
    if (expected_active != NULL &&
        (active == NULL ||
         !meiso_composition_scene_store_private_world_ref_equal(
             &active->ref, expected_active))) {
        return MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH;
    }
    (void)discard_candidate_internal(store, true);
    store->disabled = true;
    if (active == NULL) {
        return MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED;
    }
    clear_world(active);
    meiso_composition_scene_store_private_increment_saturated(
        &store->stats.disables);
    return MEISO_COMPOSITION_SCENE_DISABLE_DISABLED;
}
