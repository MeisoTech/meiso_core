#ifndef MEISO_COMPOSITION_SCENE_STORE_PRIVATE_H
#define MEISO_COMPOSITION_SCENE_STORE_PRIVATE_H

/*
 * Build-private and non-installed. The caller arena owns the store and both
 * inline worlds for their full lifetime; the store borrows BlobStore. Public
 * calls are single-threaded and non-reentrant, so no cross-TU helper locks.
 */

#include <meiso/composition/scene_store.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MEISO_COMPOSITION_SCENE_STORE_MAGIC = 0x53435452U,
    MEISO_COMPOSITION_SCENE_STORE_WORLD_SLOT_COUNT = 2U,
    MEISO_COMPOSITION_SCENE_STORE_NO_SLOT = 0xffU
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
} MeisoCompositionSceneStoreWorld;

struct MeisoCompositionSceneStore {
    uint32_t magic;
    MeisoBlobStore *blob_store;
    MeisoCompositionSceneStoreWorld
        worlds[MEISO_COMPOSITION_SCENE_STORE_WORLD_SLOT_COUNT];
    uint8_t active_slot;
    uint8_t candidate_slot;
    uint64_t world_generation;
    uint64_t control_scope_generation;
    uint64_t owner_incarnation;
    bool disabled;
    bool has_observation;
    bool observation_has_canonical;
    MeisoCompositionSceneObservation observation;
    size_t observation_canonical_size;
    uint8_t observation_canonical[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    MeisoCompositionSceneDesired validation_desired;
    uint8_t validation_wire[MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE];
    uint32_t tracked_blob_count;
    MeisoContentId
        tracked_blob_ids[MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS];
    uint32_t tracked_blob_sizes[MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS];
    uint64_t tracked_blob_bytes;
    uint64_t last_result_stream_epoch;
    uint64_t last_result_sequence;
    MeisoCompositionSceneStoreStats stats;
};

void meiso_composition_scene_store_private_increment_saturated(uint64_t *value);
bool meiso_composition_scene_store_private_ranges_overlap(const void *lhs,
                                                          size_t lhs_size,
                                                          const void *rhs,
                                                          size_t rhs_size);
bool meiso_composition_scene_store_private_output_overlaps(
    const MeisoCompositionSceneStore *store, const void *output,
    size_t output_size);
bool meiso_composition_scene_store_private_world_ref_equal(
    const MeisoCompositionSceneWorldRef *lhs,
    const MeisoCompositionSceneWorldRef *rhs);
bool meiso_composition_scene_store_private_valid(
    const MeisoCompositionSceneStore *store);
const MeisoCompositionSceneStoreWorld *
meiso_composition_scene_store_private_const_active_world(
    const MeisoCompositionSceneStore *store);
const MeisoCompositionSceneStoreWorld *
meiso_composition_scene_store_private_const_candidate_world(
    const MeisoCompositionSceneStore *store);

#endif
