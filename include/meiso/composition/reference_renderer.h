#ifndef MEISO_COMPOSITION_REFERENCE_RENDERER_H
#define MEISO_COMPOSITION_REFERENCE_RENDERER_H

#include <meiso/composition/scene_store.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Portable synthetic reference backend for the Composition Profile.
 *
 * This backend is deliberately not a geometry-correct rasterizer and does
 * not provide scanout, fence, vblank, display-commit, or physical evidence.
 * It deterministically depends on the complete canonical SceneDesired and
 * every referenced immutable mesh byte, so it is useful as an SDK seam and a
 * source-commit-pinned synthetic oracle. Its pixel algorithm is not a Profile
 * wire contract and another renderer implementation need not reproduce it. A
 * complete result always carries exactly the two off-screen evidence bits
 * below (0x00000003).
 */
#define MEISO_COMPOSITION_REFERENCE_RENDERER_EVIDENCE_FLAGS              \
    (MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |              \
     MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED)

#define MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_BYTES_PER_PIXEL 4U
#define MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_BYTES_PER_PIXEL 4U
#define MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE       \
    (MEISO_COMPOSITION_SCENE_TARGET_WIDTH *                              \
     MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *                             \
     MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_BYTES_PER_PIXEL)
#define MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE       \
    (MEISO_COMPOSITION_SCENE_TARGET_WIDTH *                              \
     MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *                             \
     MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_BYTES_PER_PIXEL)
#define MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SET_BYTE_SIZE         \
    (MEISO_COMPOSITION_REFERENCE_RENDERER_COLOR_TARGET_BYTE_SIZE +       \
     MEISO_COMPOSITION_REFERENCE_RENDERER_DEPTH_TARGET_BYTE_SIZE)
#define MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_BYTES                 \
    (MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT *                          \
     MEISO_COMPOSITION_REFERENCE_RENDERER_TARGET_SET_BYTE_SIZE)

typedef struct MeisoCompositionReferenceRenderer
    MeisoCompositionReferenceRenderer;

typedef struct {
    uint64_t renderer_generation;
    uint64_t job_id;
    MeisoCompositionSceneControlScope control_scope;
    MeisoCompositionSceneWorldRef world;
} MeisoCompositionReferenceRendererJobRef;

/*
 * result is a value copy. The two pixel views borrow the renderer arena and
 * are valid until the next successful render, a successful retire_world of
 * this complete job's exact world, changed control scope, or re-init. Color
 * is canonical row-major RGBA8888 with no row padding. Depth
 * is deterministic row-major implementation-32, encoded as BE u32 per pixel
 * with no row padding and no geometric-depth claim. The 64x48 thumbnail uses
 * the center pixel `(x * 5 + 2, y * 5 + 2)` of each 5x5 color block and
 * stores each RGB565 pixel as BE u16.
 */
typedef struct {
    MeisoCompositionReferenceRendererJobRef job;
    MeisoCompositionSceneRenderResult result;
    uint32_t target_slot;
    MeisoByteView color_rgba8888;
    MeisoByteView depth_implementation_32;
} MeisoCompositionReferenceRendererCompleteView;

typedef struct {
    uint64_t renderer_generation;
    bool control_scope_bound;
    MeisoCompositionSceneControlScope control_scope;
    uint32_t queued_jobs;
    /* Exact physical caller-arena allocations, always 2 / 1,228,800. */
    uint32_t target_slots_allocated;
    uint64_t target_bytes;
    /* Exact caller arena_size minus target_bytes, including any extra tail. */
    uint64_t renderer_metadata_bytes;
    bool complete_available;
    uint32_t complete_target_slot;
    MeisoCompositionReferenceRendererJobRef head_job;
    MeisoCompositionReferenceRendererJobRef latest_complete_job;
} MeisoCompositionReferenceRendererSnapshot;

typedef struct {
    uint64_t resets;
    uint64_t jobs_submitted;
    uint64_t queue_capacity_rejections;
    uint64_t invalid_world_rejections;
    uint64_t missing_blob_rejections;
    uint64_t invalid_mesh_rejections;
    uint64_t renders_completed;
    uint64_t render_failures;
    uint64_t jobs_discarded;
    uint64_t world_retires;
    uint64_t jobs_retired;
    uint64_t completes_retired;
} MeisoCompositionReferenceRendererStats;

typedef uint8_t MeisoCompositionReferenceRendererInitStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_OK = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_INVALID_BLOB_STORE = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_REFERENCE_RENDERER_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoCompositionReferenceRendererScopeStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_STARTED = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_UNCHANGED = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_COUNTER_EXHAUSTED = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SCOPE_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoCompositionReferenceRendererSubmitStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUED = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_QUEUE_FULL = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_WORLD = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_MISSING_BLOB = 3,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_MESH = 4,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_COUNTER_EXHAUSTED = 5,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_SCOPE_MISMATCH = 6,
    MEISO_COMPOSITION_REFERENCE_RENDERER_SUBMIT_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionReferenceRendererRenderStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_COMPLETE = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_NO_JOB = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_JOB_MISMATCH = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_TIME = 3,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_MISSING_BLOB = 4,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_MESH = 5,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RENDER_INVALID_ARGUMENT = 6
};

typedef uint8_t MeisoCompositionReferenceRendererDiscardStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_DISCARDED = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_NO_JOB = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_JOB_MISMATCH = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_DISCARD_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoCompositionReferenceRendererRetireStatus;
enum {
    MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_RETIRED = 0,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_UNCHANGED = 1,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_SCOPE_MISMATCH = 2,
    MEISO_COMPOSITION_REFERENCE_RENDERER_RETIRE_INVALID_ARGUMENT = 3
};

size_t meiso_composition_reference_renderer_alignment_required(void);
size_t meiso_composition_reference_renderer_memory_required(void);

/*
 * The caller owns the renderer arena and BlobStore. The renderer borrows the
 * BlobStore for its complete lifetime and allocates exactly two complete
 * 320x240 color+depth target sets plus a two-job queue in the caller arena.
 * It owns no Core, SceneStore, Coordinator, Stream, Session, Runtime, heap,
 * thread, socket, driver, or native target handle. Calls are single-threaded.
 * Bounded validation-job and RenderResult scratch live in this arena rather
 * than on the renderer call stack. memory_required includes that metadata;
 * snapshot reports the exact caller allocation outside target bytes,
 * including any caller-supplied tail above memory_required.
 *
 * Caller precondition: the complete opaque BlobStore arena and renderer arena
 * are disjoint. Public input/output objects must also be disjoint from both;
 * renderer-arena overlap is rejected, while BlobStore-arena overlap cannot be
 * proved because Core deliberately keeps that arena opaque.
 */
MeisoCompositionReferenceRendererInitStatus
meiso_composition_reference_renderer_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionReferenceRenderer **out_renderer
);

/*
 * Binds the renderer to the exact non-zero SceneStore control scope. A first
 * or changed scope invalidates every queued job and complete-view borrow,
 * clears both target sets and the latest complete result, and advances
 * renderer_generation. The job-id high-water is retained. An exact duplicate
 * scope is UNCHANGED and preserves all state. Counter exhaustion is
 * zero-side-effect. The caller obtains scope from SceneStore after the
 * Coordinator's owner/control-scope boundary; the renderer does not infer a
 * Session from it.
 */
MeisoCompositionReferenceRendererScopeStatus
meiso_composition_reference_renderer_begin_control_scope(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope
);

/*
 * scope must exactly equal the renderer's bound SceneStore control scope.
 * world must be the current active SceneStore view in that scope: all ref
 * generations are non-zero and desired/object/mesh/resident counters must
 * describe the exact desired value. The exact-current-active condition within
 * one scope remains a caller/Coordinator precondition because this renderer
 * intentionally does not borrow SceneStore. submit synchronously borrows the
 * view and BlobStore, verifies every referenced ContentId/mesh, then copies
 * the bounded desired snapshot; it never retains world or world->desired.
 * Queue capacity is exactly two. Every non-QUEUED result leaves the queue and
 * out_job unchanged.
 */
MeisoCompositionReferenceRendererSubmitStatus
meiso_composition_reference_renderer_submit(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    uint64_t source_clock_epoch,
    MeisoCompositionReferenceRendererJobRef *out_job
);

/*
 * Synchronously renders only the exact queue head. All immutable Blob and time
 * validation completes before the inactive target is written. Success fills
 * that inactive target and then atomically commits its slot/result and removes
 * the queue head. Any failure keeps the old complete slot/result and the head
 * job; only bounded failure statistics may advance. out_complete is written
 * only on COMPLETE. Evidence flags are exactly 0x00000003.
 */
MeisoCompositionReferenceRendererRenderStatus
meiso_composition_reference_renderer_render_next(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionReferenceRendererCompleteView *out_complete
);

/* Explicitly removes only the exact queue head; it never changes complete. */
MeisoCompositionReferenceRendererDiscardStatus
meiso_composition_reference_renderer_discard_head(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionReferenceRendererJobRef *expected_job
);

/*
 * Atomic SceneStore swap/disable boundary for one exact world in the current
 * scope. It removes every matching queued job. If latest complete references
 * that world it also invalidates the complete borrow/result and zeroes that
 * complete color+depth target; another world's complete is preserved.
 *
 * Caller contract: after SceneStore activate/swap/disable, call retire_world
 * for the retired active ref before exposing the replacement active world or
 * disabled state to renderer submission/dispatch. This is the machine seam
 * that prevents a queued same-scope old world from completing late; the
 * renderer still does not borrow SceneStore or infer its current active ref.
 */
MeisoCompositionReferenceRendererRetireStatus
meiso_composition_reference_renderer_retire_world(
    MeisoCompositionReferenceRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *retired_world
);

/* Query calls are pure and never invalidate a complete-view pixel borrow. */
bool meiso_composition_reference_renderer_latest_complete(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererCompleteView *out_complete
);
bool meiso_composition_reference_renderer_snapshot(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererSnapshot *out_snapshot
);
bool meiso_composition_reference_renderer_stats(
    const MeisoCompositionReferenceRenderer *renderer,
    MeisoCompositionReferenceRendererStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
