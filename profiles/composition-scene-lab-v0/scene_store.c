#include "scene_store.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    STORE_MAGIC = 0x53435452U,
    WORLD_SLOT_COUNT = 2U,
    NO_SLOT = 0xffU
};

typedef struct {
    bool present;
    MeisoCompositionSceneWorldRef ref;
    MeisoCompositionSceneDesired desired;
    size_t canonical_size;
    uint8_t canonical[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint32_t unique_mesh_count;
    MeisoContentId unique_meshes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
    uint32_t unique_mesh_sizes[MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES];
    uint64_t resident_mesh_bytes;
} SceneWorld;

struct MeisoCompositionSceneStore {
    uint32_t magic;
    MeisoBlobStore *blob_store;
    SceneWorld worlds[WORLD_SLOT_COUNT];
    uint8_t active_slot;
    uint8_t candidate_slot;
    uint64_t world_generation;
    uint64_t owner_incarnation;
    bool disabled;
    bool has_observation;
    bool observation_has_canonical;
    MeisoCompositionSceneObservation observation;
    size_t observation_canonical_size;
    uint8_t observation_canonical[
        MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE
    ];
    MeisoCompositionSceneDesired validation_desired;
    uint8_t validation_wire[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint32_t tracked_blob_count;
    MeisoContentId tracked_blob_ids[
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    ];
    uint32_t tracked_blob_sizes[
        MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS
    ];
    uint64_t tracked_blob_bytes;
    uint64_t last_result_stream_epoch;
    uint64_t last_result_sequence;
    MeisoCompositionSceneStoreStats stats;
};

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    size_t block_size;
} SceneSha256;

static void increment_saturated(uint64_t *value)
{
    if (*value != UINT64_MAX) {
        ++*value;
    }
}

static bool content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
)
{
    return meiso_content_id_equal(lhs, rhs);
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

static bool output_overlaps_store(
    const MeisoCompositionSceneStore *store,
    const void *output,
    size_t output_size
)
{
    return ranges_overlap(store, sizeof(*store), output, output_size);
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

static bool desired_ref_equal(
    uint64_t owner_incarnation,
    uint64_t state_version,
    const MeisoCompositionSceneObservation *observation
)
{
    return observation != NULL &&
        owner_incarnation == observation->desired_owner_incarnation &&
        state_version == observation->desired_state_version;
}

static bool store_valid(const MeisoCompositionSceneStore *store)
{
    MeisoBlobStats ignored;

    return store != NULL && store->magic == STORE_MAGIC &&
        store->blob_store != NULL &&
        store->active_slot < WORLD_SLOT_COUNT &&
        (store->candidate_slot == NO_SLOT ||
         store->candidate_slot < WORLD_SLOT_COUNT) &&
        store->candidate_slot != store->active_slot &&
        store->tracked_blob_count <=
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS &&
        meiso_blob_store_stats(store->blob_store, &ignored);
}

static SceneWorld *active_world(MeisoCompositionSceneStore *store)
{
    SceneWorld *world = &store->worlds[store->active_slot];

    return world->present ? world : NULL;
}

static const SceneWorld *const_active_world(
    const MeisoCompositionSceneStore *store
)
{
    const SceneWorld *world = &store->worlds[store->active_slot];

    return world->present ? world : NULL;
}

static SceneWorld *candidate_world(MeisoCompositionSceneStore *store)
{
    if (store->candidate_slot == NO_SLOT) {
        return NULL;
    }
    return store->worlds[store->candidate_slot].present ?
        &store->worlds[store->candidate_slot] : NULL;
}

static const SceneWorld *const_candidate_world(
    const MeisoCompositionSceneStore *store
)
{
    if (store->candidate_slot == NO_SLOT) {
        return NULL;
    }
    return store->worlds[store->candidate_slot].present ?
        &store->worlds[store->candidate_slot] : NULL;
}

static void clear_world(SceneWorld *world)
{
    if (world != NULL) {
        memset(world, 0, sizeof(*world));
    }
}

static bool discard_candidate_internal(
    MeisoCompositionSceneStore *store,
    bool count_discard
)
{
    SceneWorld *candidate = candidate_world(store);

    if (candidate == NULL) {
        store->candidate_slot = NO_SLOT;
        return false;
    }
    clear_world(candidate);
    store->candidate_slot = NO_SLOT;
    if (count_discard) {
        increment_saturated(&store->stats.candidate_discards);
    }
    return true;
}

static uint8_t available_candidate_slot(
    const MeisoCompositionSceneStore *store
)
{
    return store->active_slot == 0U ? 1U : 0U;
}

static void set_observation(
    MeisoCompositionSceneStore *store,
    uint64_t owner_incarnation,
    uint64_t state_version,
    uint64_t plan_generation,
    MeisoCompositionScenePrepareStatus prepare_status,
    MeisoCompositionSceneStableReason reason,
    bool has_canonical,
    size_t canonical_size
)
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

static int tracked_blob_index(
    const MeisoCompositionSceneStore *store,
    const MeisoContentId *content_id
)
{
    uint32_t index;

    for (index = 0U; index < store->tracked_blob_count; ++index) {
        if (content_id_equal(&store->tracked_blob_ids[index], content_id)) {
            return (int)index;
        }
    }
    return -1;
}

static bool build_world_mesh_set(
    MeisoCompositionSceneStore *store,
    SceneWorld *world,
    MeisoCompositionScenePrepareStatus *out_status,
    MeisoCompositionSceneStableReason *out_reason
)
{
    uint32_t object_index;

    world->unique_mesh_count = 0U;
    world->resident_mesh_bytes = 0U;
    for (object_index = 0U;
         object_index < world->desired.object_count;
         ++object_index) {
        const MeisoContentId *content_id =
            &world->desired.objects[object_index].mesh_content_id;
        uint32_t unique_index;
        bool seen = false;

        for (unique_index = 0U;
             unique_index < world->unique_mesh_count;
             ++unique_index) {
            if (content_id_equal(
                    content_id, &world->unique_meshes[unique_index])) {
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
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY;
                return false;
            }
            if (tracked_blob_index(store, content_id) < 0 ||
                !meiso_blob_store_lookup(
                    store->blob_store, content_id, &blob)) {
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
            if (!meiso_composition_scene_mesh_blob_verify(
                    &blob.content_id, blob.content, NULL) ||
                meiso_composition_scene_mesh_decode(
                    blob.content, &mesh) !=
                    MEISO_COMPOSITION_SCENE_CODEC_OK) {
                *out_status =
                    MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH;
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE;
                return false;
            }
            if (mesh.vertex_records.size >
                    SIZE_MAX - mesh.index_records.size) {
                *out_status = MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
                *out_reason =
                    MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY;
                return false;
            }
            resident_size = mesh.vertex_records.size +
                mesh.index_records.size;
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

static MeisoCompositionSceneStableReason desired_capacity_preflight(
    const MeisoCompositionSceneDesired *desired
)
{
    uint32_t unique_count = 0U;
    uint32_t index;

    if (desired->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS) {
        return MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY;
    }
    for (index = 0U; index < desired->object_count; ++index) {
        uint32_t previous;
        bool seen = false;

        if (!meiso_content_id_valid(
                &desired->objects[index].mesh_content_id)) {
            return MEISO_COMPOSITION_SCENE_REASON_NONE;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (content_id_equal(
                    &desired->objects[index].mesh_content_id,
                    &desired->objects[previous].mesh_content_id)) {
                seen = true;
                break;
            }
        }
        if (!seen && ++unique_count >
                MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
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

MeisoCompositionSceneStoreInitStatus meiso_composition_scene_store_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionSceneStore **out_store
)
{
    MeisoBlobStats ignored;
    MeisoCompositionSceneStore *store;

    if (out_store == NULL) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT;
    }
    if (ranges_overlap(
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
    if ((uintptr_t)arena % meiso_composition_scene_store_alignment_required()
            != 0U) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < sizeof(*store)) {
        return MEISO_COMPOSITION_SCENE_STORE_INIT_ARENA_TOO_SMALL;
    }
    memset(arena, 0, sizeof(*store));
    store = arena;
    store->magic = STORE_MAGIC;
    store->blob_store = blob_store;
    store->active_slot = 0U;
    store->candidate_slot = NO_SLOT;
    store->disabled = true;
    *out_store = store;
    return MEISO_COMPOSITION_SCENE_STORE_INIT_OK;
}

MeisoCompositionSceneOwnerStatus meiso_composition_scene_store_begin_owner(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation
)
{
    if (!store_valid(store) || desired_owner_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT;
    }
    if (store->owner_incarnation == desired_owner_incarnation) {
        return MEISO_COMPOSITION_SCENE_OWNER_UNCHANGED;
    }
    if (store->owner_incarnation == 0U) {
        increment_saturated(&store->stats.owner_starts);
    } else {
        increment_saturated(&store->stats.owner_changes);
    }
    clear_world(&store->worlds[0]);
    clear_world(&store->worlds[1]);
    store->active_slot = 0U;
    store->candidate_slot = NO_SLOT;
    store->owner_incarnation = desired_owner_incarnation;
    store->disabled = true;
    store->has_observation = false;
    store->observation_has_canonical = false;
    memset(&store->observation, 0, sizeof(store->observation));
    store->observation_canonical_size = 0U;
    memset(store->observation_canonical, 0,
           sizeof(store->observation_canonical));
    memset(&store->validation_desired, 0,
           sizeof(store->validation_desired));
    memset(store->validation_wire, 0, sizeof(store->validation_wire));
    store->tracked_blob_count = 0U;
    store->tracked_blob_bytes = 0U;
    memset(store->tracked_blob_ids, 0, sizeof(store->tracked_blob_ids));
    memset(store->tracked_blob_sizes, 0, sizeof(store->tracked_blob_sizes));
    store->last_result_stream_epoch = 0U;
    store->last_result_sequence = 0U;
    return MEISO_COMPOSITION_SCENE_OWNER_STARTED;
}

MeisoCompositionSceneBlobVisibilityStatus
meiso_composition_scene_store_mark_blob_visible(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation,
    const MeisoContentId *content_id,
    size_t content_size
)
{
    MeisoBlobView blob;
    MeisoCompositionSceneMeshView mesh;
    int existing;

    if (!store_valid(store) || content_id == NULL ||
        desired_owner_incarnation == 0U ||
        desired_owner_incarnation != store->owner_incarnation ||
        !meiso_content_id_valid(content_id) ||
        content_size > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE) {
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_ARGUMENT;
    }
    if (!meiso_blob_store_lookup(store->blob_store, content_id, &blob)) {
        increment_saturated(&store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_NOT_PUBLISHED;
    }
    if (blob.content.size != content_size) {
        increment_saturated(&store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_LENGTH_MISMATCH;
    }
    if (!meiso_composition_scene_mesh_content_id(
            blob.content, &blob.content_id)) {
        if (meiso_composition_scene_mesh_decode(blob.content, &mesh) !=
                MEISO_COMPOSITION_SCENE_CODEC_OK) {
            increment_saturated(&store->stats.blob_visibility_rejections);
            return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_MESH;
        }
        increment_saturated(&store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INTEGRITY_FAILED;
    }
    if (!content_id_equal(content_id, &blob.content_id)) {
        increment_saturated(&store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INTEGRITY_FAILED;
    }
    existing = tracked_blob_index(store, content_id);
    if (existing >= 0) {
        if (store->tracked_blob_sizes[(uint32_t)existing] != content_size) {
            increment_saturated(&store->stats.blob_visibility_rejections);
            return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_LENGTH_MISMATCH;
        }
        increment_saturated(&store->stats.blob_visibility_duplicates);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED;
    }
    if (store->tracked_blob_count >=
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS ||
        store->tracked_blob_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL -
                content_size) {
        increment_saturated(&store->stats.blob_visibility_rejections);
        return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_CAPACITY;
    }
    store->tracked_blob_ids[store->tracked_blob_count] = *content_id;
    store->tracked_blob_sizes[store->tracked_blob_count] =
        (uint32_t)content_size;
    ++store->tracked_blob_count;
    store->tracked_blob_bytes += content_size;
    increment_saturated(&store->stats.blobs_made_visible);
    return MEISO_COMPOSITION_SCENE_BLOB_VISIBLE;
}

MeisoCompositionScenePrepareStatus meiso_composition_scene_store_prepare(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation,
    uint64_t desired_state_version,
    uint64_t plan_generation,
    const MeisoCompositionSceneDesired *desired
)
{
    MeisoCompositionSceneCodecStatus codec_status;
    MeisoCompositionScenePrepareStatus prepare_status =
        MEISO_COMPOSITION_SCENE_PREPARE_PREPARED;
    MeisoCompositionSceneStableReason reason =
        MEISO_COMPOSITION_SCENE_REASON_READY;
    SceneWorld *candidate;
    const SceneWorld *active;
    size_t encoded_size = 0U;
    bool same_observed_ref;
    bool same_observed_payload = false;
    bool different_observation;

    if (!store_valid(store) || desired == NULL ||
        ranges_overlap(store, sizeof(*store), desired, sizeof(*desired)) ||
        desired_owner_incarnation == 0U ||
        desired_owner_incarnation != store->owner_incarnation ||
        desired_state_version == 0U ||
        plan_generation == 0U) {
        return MEISO_COMPOSITION_SCENE_PREPARE_INVALID_ARGUMENT;
    }
    increment_saturated(&store->stats.prepare_calls);
    store->disabled = false;
    same_observed_ref = store->has_observation && desired_ref_equal(
        desired_owner_incarnation, desired_state_version,
        &store->observation
    );
    different_observation = !same_observed_ref ||
        store->observation.plan_generation != plan_generation;

    codec_status = meiso_composition_scene_desired_encode(
        desired,
        (MeisoMutableBuffer){
            store->validation_wire,
            sizeof(store->validation_wire)
        },
        &encoded_size
    );
    if (codec_status == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        meiso_composition_scene_desired_decode(
            (MeisoByteView){store->validation_wire, encoded_size},
            &store->validation_desired
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        codec_status = MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    if (codec_status == MEISO_COMPOSITION_SCENE_CODEC_OK &&
        same_observed_ref && store->observation_has_canonical) {
        same_observed_payload =
            encoded_size == store->observation_canonical_size &&
            memcmp(store->validation_wire,
                   store->observation_canonical, encoded_size) == 0;
        if (!same_observed_payload) {
            increment_saturated(&store->stats.conflicts);
            return MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT;
        }
    } else if (same_observed_ref && store->observation_has_canonical) {
        increment_saturated(&store->stats.conflicts);
        return MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT;
    }

    if (different_observation) {
        if (discard_candidate_internal(store, false)) {
            increment_saturated(&store->stats.superseded_candidates);
        }
    }

    if (codec_status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        const MeisoCompositionSceneStableReason capacity_reason =
            desired_capacity_preflight(desired);

        if (capacity_reason != MEISO_COMPOSITION_SCENE_REASON_NONE) {
            increment_saturated(&store->stats.capacity_rejections);
            set_observation(
                store, desired_owner_incarnation, desired_state_version,
                plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY,
                capacity_reason, false, 0U
            );
            return MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
        }
        increment_saturated(&store->stats.invalid_desired);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation,
            MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED,
            MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE, false, 0U
        );
        return MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED;
    }

    candidate = candidate_world(store);
    if (candidate != NULL && same_observed_payload &&
        candidate->ref.desired_owner_incarnation ==
            desired_owner_incarnation &&
        candidate->ref.desired_state_version == desired_state_version &&
        candidate->ref.plan_generation == plan_generation) {
        increment_saturated(&store->stats.unchanged);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED,
            MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size
        );
        return MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED;
    }
    active = const_active_world(store);
    if (active != NULL && same_observed_payload &&
        active->ref.desired_owner_incarnation ==
            desired_owner_incarnation &&
        active->ref.desired_state_version == desired_state_version &&
        active->ref.plan_generation == plan_generation) {
        increment_saturated(&store->stats.already_active);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE,
            MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size
        );
        return MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE;
    }

    if (candidate != NULL && !different_observation) {
        clear_world(candidate);
        store->candidate_slot = NO_SLOT;
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

    if (!build_world_mesh_set(
            store, candidate, &prepare_status, &reason)) {
        clear_world(candidate);
        store->candidate_slot = NO_SLOT;
        if (prepare_status ==
                MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB) {
            increment_saturated(&store->stats.missing_blobs);
        } else if (prepare_status ==
                MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH) {
            increment_saturated(&store->stats.invalid_meshes);
        } else {
            increment_saturated(&store->stats.capacity_rejections);
        }
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, prepare_status, reason, true, encoded_size
        );
        return prepare_status;
    }
    active = const_active_world(store);
    if (active != NULL &&
         active->resident_mesh_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL -
                candidate->resident_mesh_bytes) {
        clear_world(candidate);
        store->candidate_slot = NO_SLOT;
        increment_saturated(&store->stats.capacity_rejections);
        set_observation(
            store, desired_owner_incarnation, desired_state_version,
            plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY,
            MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY, true,
            encoded_size
        );
        return MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY;
    }

    candidate->present = true;
    increment_saturated(&store->stats.prepared);
    set_observation(
        store, desired_owner_incarnation, desired_state_version,
        plan_generation, MEISO_COMPOSITION_SCENE_PREPARE_PREPARED,
        MEISO_COMPOSITION_SCENE_REASON_READY, true, encoded_size
    );
    return MEISO_COMPOSITION_SCENE_PREPARE_PREPARED;
}

MeisoCompositionSceneActivateStatus meiso_composition_scene_store_activate(
    MeisoCompositionSceneStore *store,
    uint64_t expected_desired_owner_incarnation,
    uint64_t expected_desired_state_version,
    uint64_t expected_plan_generation,
    MeisoCompositionSceneWorldRef *out_active_ref
)
{
    SceneWorld *candidate;
    SceneWorld *old_active;
    uint8_t old_active_slot;

    if (!store_valid(store) || out_active_ref == NULL ||
        expected_desired_owner_incarnation == 0U ||
        expected_desired_state_version == 0U ||
        expected_plan_generation == 0U ||
        output_overlaps_store(
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
        increment_saturated(&store->stats.activation_mismatches);
        return MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH;
    }
    if (store->world_generation == UINT64_MAX) {
        return MEISO_COMPOSITION_SCENE_ACTIVATE_COUNTER_EXHAUSTED;
    }

    old_active_slot = store->active_slot;
    old_active = &store->worlds[old_active_slot];
    store->active_slot = store->candidate_slot;
    store->candidate_slot = NO_SLOT;
    candidate->ref.world_generation = ++store->world_generation;
    store->disabled = false;
    clear_world(old_active);
    *out_active_ref = candidate->ref;
    increment_saturated(&store->stats.activations);
    return MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED;
}

bool meiso_composition_scene_store_discard_candidate(
    MeisoCompositionSceneStore *store
)
{
    if (!store_valid(store)) {
        return false;
    }
    return discard_candidate_internal(store, true);
}

MeisoCompositionSceneDisableStatus meiso_composition_scene_store_disable(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *expected_active
)
{
    SceneWorld *active;

    if (!store_valid(store)) {
        return MEISO_COMPOSITION_SCENE_DISABLE_INVALID_ARGUMENT;
    }
    active = active_world(store);
    if (expected_active != NULL &&
        (active == NULL ||
         !world_ref_equal(&active->ref, expected_active))) {
        return MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH;
    }
    (void)discard_candidate_internal(store, true);
    store->disabled = true;
    if (active == NULL) {
        return MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED;
    }
    clear_world(active);
    increment_saturated(&store->stats.disables);
    return MEISO_COMPOSITION_SCENE_DISABLE_DISABLED;
}

static bool world_view(
    const MeisoCompositionSceneStore *store,
    const SceneWorld *world,
    MeisoCompositionSceneWorldView *out_view
)
{
    if (world == NULL || out_view == NULL ||
        output_overlaps_store(store, out_view, sizeof(*out_view))) {
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
    MeisoCompositionSceneWorldView *out_view
)
{
    if (!store_valid(store)) {
        return false;
    }
    return world_view(store, const_active_world(store), out_view);
}

bool meiso_composition_scene_store_candidate(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneWorldView *out_view
)
{
    if (!store_valid(store)) {
        return false;
    }
    return world_view(store, const_candidate_world(store), out_view);
}

bool meiso_composition_scene_store_observation(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneObservation *out_observation
)
{
    if (!store_valid(store) || out_observation == NULL ||
        output_overlaps_store(
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
    MeisoCompositionSceneStatus *out_status
)
{
    const SceneWorld *active;
    const SceneWorld *candidate;
    uint64_t resident_bytes = 0U;

    const uint64_t bytes_per_target_set =
        (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 8U;

    if (!store_valid(store) || resource_usage == NULL ||
        out_status == NULL ||
        output_overlaps_store(store, out_status, sizeof(*out_status)) ||
        ranges_overlap(resource_usage, sizeof(*resource_usage),
                       out_status, sizeof(*out_status)) ||
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
    active = const_active_world(store);
    candidate = const_candidate_world(store);
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
    out_status->published_blob_count =
        resource_usage->published_blob_count;
    out_status->published_blob_bytes =
        resource_usage->published_blob_bytes;
    out_status->resident_mesh_bytes = resident_bytes;
    out_status->target_slots_used = resource_usage->target_slots_used;
    out_status->target_bytes = resource_usage->target_bytes;
    out_status->render_jobs_in_flight =
        resource_usage->render_jobs_in_flight;
    out_status->result_queue_depth = resource_usage->result_queue_depth;
    out_status->last_result_stream_epoch =
        store->last_result_stream_epoch;
    out_status->last_result_sequence = store->last_result_sequence;
    return true;
}

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t read_be_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static void write_be_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void sha256_transform(SceneSha256 *sha, const uint8_t block[64])
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
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        words[index] = read_be_u32(block + index * 4U);
    }
    for (; index < 64U; ++index) {
        const uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
            rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
            rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 +
            words[index - 7U] + s1;
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
        const uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
            rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice +
            constants[index] + words[index];
        const uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
            rotate_right(a, 22U);
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

static void sha256_init(SceneSha256 *sha)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    memset(sha, 0, sizeof(*sha));
    memcpy(sha->state, initial, sizeof(initial));
}

static void sha256_update(
    SceneSha256 *sha,
    const uint8_t *data,
    size_t size
)
{
    while (size != 0U) {
        size_t available = sizeof(sha->block) - sha->block_size;
        size_t take = size < available ? size : available;

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
    SceneSha256 *sha,
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

bool meiso_composition_scene_mesh_content_id(
    MeisoByteView content,
    MeisoContentId *out_content_id
)
{
    MeisoCompositionSceneMeshView mesh;
    MeisoContentId content_id;
    SceneSha256 sha;

    if (out_content_id == NULL || content.data == NULL ||
        ranges_overlap(content.data, content.size,
                       out_content_id, sizeof(*out_content_id)) ||
        content.size > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE ||
        meiso_composition_scene_mesh_decode(content, &mesh) !=
            MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    memset(&content_id, 0, sizeof(content_id));
    content_id.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    content_id.digest_size = MEISO_SHA256_DIGEST_SIZE;
    sha256_init(&sha);
    sha256_update(&sha, content.data, content.size);
    sha256_final(&sha, content_id.digest);
    *out_content_id = content_id;
    return true;
}

bool meiso_composition_scene_mesh_blob_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
)
{
    MeisoContentId calculated;

    (void)context;
    return meiso_content_id_valid(expected_content_id) &&
        meiso_composition_scene_mesh_content_id(
            staged_content, &calculated) &&
        content_id_equal(expected_content_id, &calculated);
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

static void fake_pixel(
    uint64_t scene_seed,
    uint32_t x,
    uint32_t y,
    uint8_t rgba[4]
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
}

static bool active_scene_seed(
    const MeisoCompositionSceneStore *store,
    const SceneWorld *world,
    uint64_t *out_seed
)
{
    static const uint8_t domain[] = {
        'M', 'E', 'I', 'S', 'O', '_', 'S', 'C', 'E', 'N', 'E', '_',
        'F', 'A', 'K', 'E', '_', 'R', 'E', 'N', 'D', 'E', 'R', '_',
        'V', '0', 0U
    };
    uint64_t seed = UINT64_C(14695981039346656037);
    uint32_t index;

    seed = fnv1a_update(seed, domain, sizeof(domain));
    seed = fnv1a_update(seed, world->canonical, world->canonical_size);
    for (index = 0U; index < world->unique_mesh_count; ++index) {
        MeisoBlobView blob;
        uint8_t size_be[8];
        uint64_t size = world->unique_mesh_sizes[index];
        size_t byte_index;

        if (!meiso_blob_store_lookup(
                store->blob_store, &world->unique_meshes[index], &blob) ||
            blob.content.size != world->unique_mesh_sizes[index] ||
            !meiso_composition_scene_mesh_blob_verify(
                &world->unique_meshes[index], blob.content, NULL)) {
            return false;
        }
        for (byte_index = 0U; byte_index < sizeof(size_be); ++byte_index) {
            size_be[byte_index] =
                (uint8_t)(size >> (56U - byte_index * 8U));
        }
        seed = fnv1a_update(seed, size_be, sizeof(size_be));
        seed = fnv1a_update(seed, blob.content.data, blob.content.size);
    }
    *out_seed = seed;
    return true;
}

MeisoCompositionSceneRenderStatus meiso_composition_scene_store_fake_render(
    MeisoCompositionSceneStore *store,
    uint64_t source_clock_epoch,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionSceneRenderResult *out_result
)
{
    const SceneWorld *world;
    SceneSha256 sha;
    uint64_t seed;
    uint32_t y;

    if (!store_valid(store) || out_result == NULL ||
        output_overlaps_store(store, out_result, sizeof(*out_result))) {
        return MEISO_COMPOSITION_SCENE_RENDER_INVALID_ARGUMENT;
    }
    if (source_clock_epoch == 0U ||
        render_end_monotonic_ns < render_start_monotonic_ns) {
        increment_saturated(&store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_INVALID_TIME;
    }
    world = const_active_world(store);
    if (world == NULL) {
        increment_saturated(&store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_NO_ACTIVE_WORLD;
    }
    if (!active_scene_seed(store, world, &seed)) {
        increment_saturated(&store->stats.render_failures);
        return MEISO_COMPOSITION_SCENE_RENDER_MISSING_BLOB;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->applied_scene_owner_incarnation =
        world->ref.desired_owner_incarnation;
    out_result->applied_scene_state_version =
        world->ref.desired_state_version;
    out_result->active_plan_generation = world->ref.plan_generation;
    out_result->world_generation = world->ref.world_generation;
    out_result->result_code = MEISO_COMPOSITION_SCENE_RESULT_COMPLETE;
    out_result->evidence_flags =
        MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
        MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED;
    out_result->object_count = world->desired.object_count;
    out_result->unique_mesh_count = world->unique_mesh_count;
    out_result->source_clock_epoch = source_clock_epoch;
    out_result->render_start_monotonic_ns = render_start_monotonic_ns;
    out_result->render_end_monotonic_ns = render_end_monotonic_ns;
    out_result->target_width = MEISO_COMPOSITION_SCENE_TARGET_WIDTH;
    out_result->target_height = MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
    out_result->color_format = MEISO_COMPOSITION_SCENE_COLOR_RGBA8888;
    out_result->depth_format =
        MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32;
    out_result->logical_color_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    out_result->logical_depth_row_bytes =
        MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U;
    out_result->thumbnail_width =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH;
    out_result->thumbnail_height =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT;
    out_result->thumbnail_format = MEISO_COMPOSITION_SCENE_COLOR_RGB565;
    out_result->thumbnail_stride_bytes =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH * 2U;
    out_result->thumbnail_byte_size =
        MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE;

    sha256_init(&sha);
    for (y = 0U; y < MEISO_COMPOSITION_SCENE_TARGET_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_TARGET_WIDTH; ++x) {
            uint8_t rgba[4];
            fake_pixel(seed, x, y, rgba);
            sha256_update(&sha, rgba, sizeof(rgba));
        }
    }
    sha256_final(&sha, out_result->full_target_sha256);

    for (y = 0U; y < MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT; ++y) {
        uint32_t x;
        for (x = 0U; x < MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH; ++x) {
            uint8_t rgba[4];
            uint16_t rgb565;
            const size_t offset =
                ((size_t)y * MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH + x) *
                2U;
            fake_pixel(seed, x * 5U + 2U, y * 5U + 2U, rgba);
            rgb565 = (uint16_t)(
                ((uint16_t)(rgba[0] >> 3U) << 11U) |
                ((uint16_t)(rgba[1] >> 2U) << 5U) |
                (uint16_t)(rgba[2] >> 3U)
            );
            out_result->thumbnail_rgb565[offset] =
                (uint8_t)(rgb565 >> 8U);
            out_result->thumbnail_rgb565[offset + 1U] = (uint8_t)rgb565;
        }
    }
    increment_saturated(&store->stats.renders);
    return MEISO_COMPOSITION_SCENE_RENDER_COMPLETE;
}

bool meiso_composition_scene_store_record_result_published(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *rendered_world,
    uint64_t stream_epoch,
    uint64_t sequence
)
{
    const SceneWorld *active;

    if (!store_valid(store) || rendered_world == NULL ||
        stream_epoch == 0U || sequence == 0U) {
        return false;
    }
    active = const_active_world(store);
    if (active == NULL || !world_ref_equal(&active->ref, rendered_world)) {
        return false;
    }
    if (store->last_result_stream_epoch == stream_epoch &&
        sequence <= store->last_result_sequence) {
        return false;
    }
    store->last_result_stream_epoch = stream_epoch;
    store->last_result_sequence = sequence;
    return true;
}

bool meiso_composition_scene_store_stats(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneStoreStats *out_stats
)
{
    if (!store_valid(store) || out_stats == NULL ||
        output_overlaps_store(store, out_stats, sizeof(*out_stats))) {
        return false;
    }
    *out_stats = store->stats;
    return true;
}
