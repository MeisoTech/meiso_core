#ifndef MEISO_COMPOSITION_SCENE_STORE_H
#define MEISO_COMPOSITION_SCENE_STORE_H

#include <meiso/profiles/composition/scene.h>
#include <meiso/core/blob.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoCompositionSceneStore MeisoCompositionSceneStore;

typedef struct {
    uint64_t desired_owner_incarnation;
    uint64_t desired_state_version;
    uint64_t plan_generation;
    /* Zero for a prepared candidate; non-zero only for an active world. */
    uint64_t world_generation;
} MeisoCompositionSceneWorldRef;

typedef struct {
    MeisoCompositionSceneWorldRef ref;
    const MeisoCompositionSceneDesired *desired;
    uint32_t object_count;
    uint32_t unique_mesh_count;
    /*
     * Per-world unique-mesh sum of canonical vertex+index record bytes.
     * Shared meshes count once in each world. Storage remains zero-copy in
     * MeisoBlobStore; this is not separate GPU/CMA allocation evidence.
     */
    uint64_t resident_mesh_bytes;
} MeisoCompositionSceneWorldView;

typedef struct {
    uint64_t desired_owner_incarnation;
    uint64_t control_scope_generation;
} MeisoCompositionSceneControlScope;

typedef uint8_t MeisoCompositionSceneStoreInitStatus;
enum {
    MEISO_COMPOSITION_SCENE_STORE_INIT_OK = 0,
    MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_SCENE_STORE_INIT_INVALID_BLOB_STORE = 2,
    MEISO_COMPOSITION_SCENE_STORE_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_SCENE_STORE_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoCompositionSceneOwnerStatus;
enum {
    MEISO_COMPOSITION_SCENE_OWNER_STARTED = 0,
    MEISO_COMPOSITION_SCENE_OWNER_UNCHANGED = 1,
    MEISO_COMPOSITION_SCENE_OWNER_INVALID_ARGUMENT = 2,
    MEISO_COMPOSITION_SCENE_OWNER_SCOPE_RESET = 3,
    MEISO_COMPOSITION_SCENE_OWNER_SCOPE_COUNTER_EXHAUSTED = 4
};

typedef uint8_t MeisoCompositionSceneBlobVisibilityStatus;
enum {
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE = 0,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_UNCHANGED = 1,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_NOT_PUBLISHED = 2,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_LENGTH_MISMATCH = 3,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INTEGRITY_FAILED = 4,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_MESH = 5,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_CAPACITY = 6,
    MEISO_COMPOSITION_SCENE_BLOB_VISIBLE_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionScenePrepareStatus;
enum {
    MEISO_COMPOSITION_SCENE_PREPARE_PREPARED = 0,
    MEISO_COMPOSITION_SCENE_PREPARE_UNCHANGED = 1,
    MEISO_COMPOSITION_SCENE_PREPARE_ALREADY_ACTIVE = 2,
    MEISO_COMPOSITION_SCENE_PREPARE_INVALID_ARGUMENT = 3,
    MEISO_COMPOSITION_SCENE_PREPARE_INVALID_DESIRED = 4,
    MEISO_COMPOSITION_SCENE_PREPARE_MISSING_BLOB = 5,
    MEISO_COMPOSITION_SCENE_PREPARE_INVALID_MESH = 6,
    MEISO_COMPOSITION_SCENE_PREPARE_CAPACITY = 7,
    MEISO_COMPOSITION_SCENE_PREPARE_CONFLICT = 8
};

typedef struct {
    uint64_t desired_owner_incarnation;
    uint64_t desired_state_version;
    uint64_t plan_generation;
    MeisoCompositionScenePrepareStatus prepare_status;
    MeisoCompositionSceneStableReason reason;
} MeisoCompositionSceneObservation;

typedef uint8_t MeisoCompositionSceneActivateStatus;
enum {
    MEISO_COMPOSITION_SCENE_ACTIVATE_ACTIVATED = 0,
    MEISO_COMPOSITION_SCENE_ACTIVATE_NO_CANDIDATE = 1,
    MEISO_COMPOSITION_SCENE_ACTIVATE_MISMATCH = 2,
    MEISO_COMPOSITION_SCENE_ACTIVATE_COUNTER_EXHAUSTED = 3,
    MEISO_COMPOSITION_SCENE_ACTIVATE_INVALID_ARGUMENT = 4
};

typedef uint8_t MeisoCompositionSceneDisableStatus;
enum {
    MEISO_COMPOSITION_SCENE_DISABLE_DISABLED = 0,
    MEISO_COMPOSITION_SCENE_DISABLE_ALREADY_DISABLED = 1,
    MEISO_COMPOSITION_SCENE_DISABLE_MISMATCH = 2,
    MEISO_COMPOSITION_SCENE_DISABLE_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoCompositionSceneRenderStatus;
enum {
    MEISO_COMPOSITION_SCENE_RENDER_COMPLETE = 0,
    MEISO_COMPOSITION_SCENE_RENDER_NO_ACTIVE_WORLD = 1,
    MEISO_COMPOSITION_SCENE_RENDER_INVALID_TIME = 2,
    MEISO_COMPOSITION_SCENE_RENDER_MISSING_BLOB = 3,
    MEISO_COMPOSITION_SCENE_RENDER_INVALID_ARGUMENT = 4
};

typedef struct {
    uint64_t owner_starts;
    uint64_t owner_changes;
    uint64_t control_scope_resets;
    uint64_t blobs_made_visible;
    uint64_t blob_visibility_duplicates;
    uint64_t blob_visibility_rejections;
    uint64_t prepare_calls;
    uint64_t prepared;
    uint64_t unchanged;
    uint64_t already_active;
    uint64_t superseded_candidates;
    uint64_t invalid_desired;
    uint64_t missing_blobs;
    uint64_t invalid_meshes;
    uint64_t capacity_rejections;
    uint64_t conflicts;
    uint64_t activations;
    uint64_t activation_mismatches;
    uint64_t candidate_discards;
    uint64_t disables;
    uint64_t renders;
    uint64_t render_failures;
} MeisoCompositionSceneStoreStats;

/*
 * MeisoBlobStore deliberately has no operational-use API. The caller that
 * owns Core Stream/renderer resources supplies this bounded ledger when
 * constructing SceneStatus. Blob count must match the process BlobStore;
 * published bytes come from the caller's publication ledger. Per-owner Blob
 * visibility is only an authorization gate and does not hide process cache
 * usage. Queue depth must come from the live Stream rather than inference.
 * Target/job values describe real external allocations; the streaming C1
 * fake renderer itself owns no target/job and therefore contributes zero.
 */
typedef struct {
    uint32_t published_blob_count;
    uint64_t published_blob_bytes;
    uint32_t target_slots_used;
    uint64_t target_bytes;
    uint32_t render_jobs_in_flight;
    uint32_t result_queue_depth;
} MeisoCompositionSceneResourceUsage;

size_t meiso_composition_scene_store_alignment_required(void);
size_t meiso_composition_scene_store_memory_required(void);

/*
 * The caller owns the arena and BlobStore. The scene store only borrows the
 * BlobStore for its complete lifetime; it allocates no heap and owns no Core
 * State, Blob, Stream, Session, or Runtime handle. Calls are not thread-safe.
 * Caller precondition: the complete external BlobStore arena is disjoint from
 * the complete scene-store arena; BlobStore is opaque, so init cannot prove
 * this boundary at runtime.
 * Every public output object must be disjoint from both arenas. Scene-store
 * overlap is rejected at runtime; external BlobStore-arena overlap remains a
 * caller precondition because Core intentionally keeps that arena opaque.
 */
MeisoCompositionSceneStoreInitStatus meiso_composition_scene_store_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionSceneStore **out_store
);

/*
 * Mirrors the Core StateReplica owner boundary. A new owner clears active and
 * candidate worlds, observations, result references, and per-owner Blob
 * visibility. The immutable BlobStore cache remains allocated, but a new
 * owner cannot use a cached ContentId until Host publication/duplicate has
 * been observed and mark_blob_visible succeeds for that owner. The local
 * world_generation counter remains monotonic across owner changes.
 */
MeisoCompositionSceneOwnerStatus meiso_composition_scene_store_begin_owner(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation
);

/*
 * Starts a fresh Session/control scope even when the desired owner
 * incarnation is unchanged. This clears active/candidate worlds,
 * observations, result references, and per-scope Blob visibility while
 * retaining the immutable BlobStore cache and world-generation high-water.
 */
MeisoCompositionSceneOwnerStatus
meiso_composition_scene_store_reset_control_scope(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation
);

/* Generation is zero only before the first successful owner/scope start. */
bool meiso_composition_scene_store_control_scope(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneControlScope *out_scope
);

MeisoCompositionSceneBlobVisibilityStatus
meiso_composition_scene_store_mark_blob_visible(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation,
    const MeisoContentId *content_id,
    size_t content_size
);

/*
 * Call prepare only for a Core StateReplica RECEIVE_NEW value or for an exact
 * duplicate of a value previously passed here. A different desired owner /
 * version supersedes and clears the old candidate before validation; a
 * rejected new desired therefore can never leave a stale candidate
 * activatable. The active world is never changed by prepare. desired must not
 * overlap the scene-store arena; pass the external StateReplica value.
 */
MeisoCompositionScenePrepareStatus meiso_composition_scene_store_prepare(
    MeisoCompositionSceneStore *store,
    uint64_t desired_owner_incarnation,
    uint64_t desired_state_version,
    uint64_t plan_generation,
    const MeisoCompositionSceneDesired *desired
);

/* Logical frame-boundary compare-and-swap; no renderer work is performed. */
MeisoCompositionSceneActivateStatus meiso_composition_scene_store_activate(
    MeisoCompositionSceneStore *store,
    uint64_t expected_desired_owner_incarnation,
    uint64_t expected_desired_state_version,
    uint64_t expected_plan_generation,
    MeisoCompositionSceneWorldRef *out_active_ref
);

bool meiso_composition_scene_store_discard_candidate(
    MeisoCompositionSceneStore *store
);

/*
 * Explicit frame-boundary retirement. If expected_active is non-NULL it must
 * exactly match the current active ref. A successful disable also clears the
 * candidate; Wi-Fi and all external Core resources remain untouched.
 */
MeisoCompositionSceneDisableStatus meiso_composition_scene_store_disable(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *expected_active
);

/*
 * WorldView.desired is borrowed. An active borrow ends at activate, disable,
 * begin_owner, or reset_control_scope; a candidate borrow also ends at
 * prepare or discard.
 */
bool meiso_composition_scene_store_active(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneWorldView *out_view
);
bool meiso_composition_scene_store_candidate(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneWorldView *out_view
);
bool meiso_composition_scene_store_observation(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneObservation *out_observation
);

/*
 * Suffix only; the caller publishes it through the Edge StatePublisher.
 * resident_mesh_bytes is active+candidate logical residency, so a mesh shared
 * across those worlds is counted once per world; it is not physical memory.
 */
bool meiso_composition_scene_store_status(
    const MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneResourceUsage *resource_usage,
    MeisoCompositionSceneStatus *out_status
);

/*
 * Resource-only fake renderer. It hashes the canonical active SceneDesired
 * plus the actual immutable mesh bytes, then deterministically produces the
 * canonical 320x240 logical RGBA8888 target hash and 64x48 RGB565 thumbnail.
 * The caller owns/publishes out_result through the Edge Stream.
 */
MeisoCompositionSceneRenderStatus meiso_composition_scene_store_fake_render(
    MeisoCompositionSceneStore *store,
    uint64_t source_clock_epoch,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionSceneRenderResult *out_result
);

/*
 * Call only after the matching result was accepted by the Core Stream, and
 * serialize accept->record before any activate, disable, or begin_owner.
 */
bool meiso_composition_scene_store_record_result_published(
    MeisoCompositionSceneStore *store,
    const MeisoCompositionSceneWorldRef *rendered_world,
    uint64_t stream_epoch,
    uint64_t sequence
);

bool meiso_composition_scene_store_stats(
    const MeisoCompositionSceneStore *store,
    MeisoCompositionSceneStoreStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
