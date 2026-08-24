#ifndef MEISO_COMPOSITION_CPU_RASTER_RENDERER_H
#define MEISO_COMPOSITION_CPU_RASTER_RENDERER_H

#include "scene_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bounded, Profile-local CPU triangle rasterizer.
 *
 * This is a real geometry-processing feasibility backend, separate from the
 * synthetic composition_reference_renderer. It consumes canonical MSH0
 * triangle-list Blob bytes, the active SceneDesired object TRS values, and
 * the Scene camera pose/FOV. It owns no Core, SceneStore, Coordinator,
 * Stream, Session, Runtime, heap, thread, socket, driver, scanout, or native
 * target handle.
 *
 * The rules below are an implementation-local, source-commit-pinned oracle.
 * They are not a Composition Profile/wire contract, not a cross-target
 * bit-exact promise, and not physical display, performance, fence, vblank, or
 * scanout evidence. A complete result therefore carries exactly OFFSCREEN +
 * THUMBNAIL (0x00000003), never any physical evidence bit.
 */
#define MEISO_COMPOSITION_CPU_RASTER_RENDERER_EVIDENCE_FLAGS             \
    (MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |              \
     MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED)

#define MEISO_COMPOSITION_CPU_RASTER_COLOR_BYTES_PER_PIXEL 4U
#define MEISO_COMPOSITION_CPU_RASTER_DEPTH_BYTES_PER_PIXEL 4U
#define MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE               \
    (MEISO_COMPOSITION_SCENE_TARGET_WIDTH *                              \
     MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *                             \
     MEISO_COMPOSITION_CPU_RASTER_COLOR_BYTES_PER_PIXEL)
#define MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE               \
    (MEISO_COMPOSITION_SCENE_TARGET_WIDTH *                              \
     MEISO_COMPOSITION_SCENE_TARGET_HEIGHT *                             \
     MEISO_COMPOSITION_CPU_RASTER_DEPTH_BYTES_PER_PIXEL)
#define MEISO_COMPOSITION_CPU_RASTER_TARGET_SET_BYTE_SIZE                 \
    (MEISO_COMPOSITION_CPU_RASTER_COLOR_TARGET_BYTE_SIZE +               \
     MEISO_COMPOSITION_CPU_RASTER_DEPTH_TARGET_BYTE_SIZE)
#define MEISO_COMPOSITION_CPU_RASTER_TARGET_BYTES                         \
    (MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT *                          \
     MEISO_COMPOSITION_CPU_RASTER_TARGET_SET_BYTE_SIZE)

/* Backend admission/work bounds; they do not enlarge Profile limits. */
#define MEISO_COMPOSITION_CPU_RASTER_MAX_OBJECTS                         \
    MEISO_COMPOSITION_SCENE_MAX_OBJECTS
#define MEISO_COMPOSITION_CPU_RASTER_MAX_UNIQUE_MESHES                   \
    MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES
#define MEISO_COMPOSITION_CPU_RASTER_MAX_MESH_BLOB_SIZE                  \
    MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
#define MEISO_COMPOSITION_CPU_RASTER_JOB_SLOTS                           \
    MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS

/* Exact clear values in the public complete target views. */
#define MEISO_COMPOSITION_CPU_RASTER_CLEAR_R UINT8_C(0x10)
#define MEISO_COMPOSITION_CPU_RASTER_CLEAR_G UINT8_C(0x18)
#define MEISO_COMPOSITION_CPU_RASTER_CLEAR_B UINT8_C(0x20)
#define MEISO_COMPOSITION_CPU_RASTER_CLEAR_A UINT8_C(0xff)
#define MEISO_COMPOSITION_CPU_RASTER_CLEAR_DEPTH UINT32_C(0xffffffff)

/*
 * Source-pinned raster rules:
 *
 * - Coordinates are right-handed. Camera local +X is right, +Y is up, and
 *   forward is -Z. Quaternion storage is xyzw. An object vertex is transformed
 *   by translation + rotate(orientation, scale * position). The view transform
 *   is rotate(conjugate(camera orientation), world - camera position).
 * - Perspective uses the Scene vertical FOV, viewport aspect, near_z/far_z,
 *   and an OpenGL-style homogeneous clip volume: -w <= x,y,z <= w, w > 0.
 *   Triangles are clipped against all six planes before division; newly
 *   generated attributes use the same edge parameter as clip position.
 * - MSH0 indices are consumed in stored triangle-list order. Counter-clockwise
 *   projected winding in NDC is front-facing; back-facing and zero-area
 *   triangles are discarded. Invisible objects produce no triangles.
 * - NDC maps to a top-left 320x240 viewport, with +Y inverted. Screen x/y are
 *   quantized to signed 24.8 fixed point using round-to-nearest, ties-to-even.
 *   Coverage is tested at pixel centers with the top-left rule. Clipped
 *   polygons are fan-triangulated in stable generated-vertex order.
 * - Depth maps NDC -1..+1 to 0..1, then to 0..0xfffffffe using
 *   round-to-nearest, ties-to-even. 0xffffffff is reserved for clear. The
 *   comparison is strictly LESS; an equal-depth later fragment never replaces
 *   the earlier object/index-order fragment.
 * - Vertex RGBA is perspective-correct interpolated using reciprocal w and
 *   rounded to nearest integer, ties-to-even, clamped to 0..255. MSH0 without
 *   vertex colors uses opaque white. There is no blending, lighting, texture,
 *   gamma conversion, or material state.
 * - Non-finite/overflowing intermediate geometry or an invalid canonical MSH0
 *   fails the head job before the complete target/result is committed. This
 *   source-pinned backend requires IEC 60559-shaped binary32/binary64,
 *   FLT_EVAL_METHOD=0, and FE_TONEAREST, and uses the target libm. Init fails
 *   closed on an unsupported platform and render fails if the rounding mode
 *   later changes. Different targets are not promised identical pixels or
 *   hashes.
 *
 * Color is row-major RGBA8888 without padding. Depth is row-major BE u32
 * without padding. The thumbnail takes center pixel (x*5+2,y*5+2) from each
 * 5x5 color block and stores RGB565 as BE u16. RenderResult.full_target_sha256
 * hashes only the complete logical RGBA8888 target bytes.
 */

typedef struct MeisoCompositionCpuRasterRenderer
    MeisoCompositionCpuRasterRenderer;

typedef struct {
    uint64_t renderer_generation;
    uint64_t job_id;
    MeisoCompositionSceneControlScope control_scope;
    MeisoCompositionSceneWorldRef world;
} MeisoCompositionCpuRasterJobRef;

/*
 * result is a value copy. Pixel views borrow the renderer arena until the
 * next successful render, successful retirement of complete.job.world,
 * changed control scope, or re-init.
 */
typedef struct {
    MeisoCompositionCpuRasterJobRef job;
    MeisoCompositionSceneRenderResult result;
    uint32_t target_slot;
    MeisoByteView color_rgba8888;
    MeisoByteView depth_implementation_32;
} MeisoCompositionCpuRasterCompleteView;

typedef struct {
    uint64_t renderer_generation;
    bool control_scope_bound;
    MeisoCompositionSceneControlScope control_scope;
    uint32_t queued_jobs;
    /* Exact physical caller-arena target allocations: always 2 / 1,228,800. */
    uint32_t target_slots_allocated;
    uint64_t target_bytes;
    /* Exact arena_size minus target_bytes, including caller-supplied tail. */
    uint64_t renderer_metadata_bytes;
    bool complete_available;
    uint32_t complete_target_slot;
    MeisoCompositionCpuRasterJobRef head_job;
    MeisoCompositionCpuRasterJobRef latest_complete_job;
} MeisoCompositionCpuRasterSnapshot;

typedef struct {
    uint64_t scope_resets;
    uint64_t jobs_submitted;
    uint64_t queue_capacity_rejections;
    uint64_t invalid_world_rejections;
    uint64_t missing_blob_rejections;
    uint64_t invalid_mesh_rejections;
    uint64_t renders_completed;
    uint64_t render_failures;
    uint64_t triangles_submitted;
    uint64_t triangles_clipped;
    uint64_t triangles_culled;
    uint64_t triangles_rasterized;
    uint64_t fragments_depth_passed;
    uint64_t jobs_discarded;
    uint64_t world_retires;
    uint64_t jobs_retired;
    uint64_t completes_retired;
} MeisoCompositionCpuRasterStats;

typedef uint8_t MeisoCompositionCpuRasterInitStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_INIT_OK = 0,
    MEISO_COMPOSITION_CPU_RASTER_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_CPU_RASTER_INIT_INVALID_BLOB_STORE = 2,
    MEISO_COMPOSITION_CPU_RASTER_INIT_MISALIGNED_ARENA = 3,
    MEISO_COMPOSITION_CPU_RASTER_INIT_ARENA_TOO_SMALL = 4,
    MEISO_COMPOSITION_CPU_RASTER_INIT_PLATFORM_UNSUPPORTED = 5
};

typedef uint8_t MeisoCompositionCpuRasterScopeStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_SCOPE_STARTED = 0,
    MEISO_COMPOSITION_CPU_RASTER_SCOPE_UNCHANGED = 1,
    MEISO_COMPOSITION_CPU_RASTER_SCOPE_COUNTER_EXHAUSTED = 2,
    MEISO_COMPOSITION_CPU_RASTER_SCOPE_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoCompositionCpuRasterSubmitStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUED = 0,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_QUEUE_FULL = 1,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_WORLD = 2,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_MISSING_BLOB = 3,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_MESH = 4,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_COUNTER_EXHAUSTED = 5,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_SCOPE_MISMATCH = 6,
    MEISO_COMPOSITION_CPU_RASTER_SUBMIT_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionCpuRasterRenderStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_RENDER_COMPLETE = 0,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_NO_JOB = 1,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_JOB_MISMATCH = 2,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_TIME = 3,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_MISSING_BLOB = 4,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_MESH = 5,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_GEOMETRY = 6,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_COUNTER_EXHAUSTED = 7,
    MEISO_COMPOSITION_CPU_RASTER_RENDER_INVALID_ARGUMENT = 8
};

typedef uint8_t MeisoCompositionCpuRasterDiscardStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_DISCARD_DISCARDED = 0,
    MEISO_COMPOSITION_CPU_RASTER_DISCARD_NO_JOB = 1,
    MEISO_COMPOSITION_CPU_RASTER_DISCARD_JOB_MISMATCH = 2,
    MEISO_COMPOSITION_CPU_RASTER_DISCARD_INVALID_ARGUMENT = 3
};

typedef uint8_t MeisoCompositionCpuRasterRetireStatus;
enum {
    MEISO_COMPOSITION_CPU_RASTER_RETIRE_RETIRED = 0,
    MEISO_COMPOSITION_CPU_RASTER_RETIRE_UNCHANGED = 1,
    MEISO_COMPOSITION_CPU_RASTER_RETIRE_SCOPE_MISMATCH = 2,
    MEISO_COMPOSITION_CPU_RASTER_RETIRE_INVALID_ARGUMENT = 3
};

size_t meiso_composition_cpu_raster_renderer_alignment_required(void);
size_t meiso_composition_cpu_raster_renderer_memory_required(void);

/*
 * Caller owns the renderer arena and BlobStore. The renderer borrows the
 * BlobStore for its complete lifetime and uses exactly two full target sets,
 * a two-job queue, and bounded desired/job/RenderResult scratch in its arena.
 * Per-call camera, SHA-256 context, and clip scratch are fixed-size stack
 * values (the clip polygon is capped at 12 vertices); no mesh-sized or
 * frame-sized stack allocation occurs. Calls are single-threaded and
 * non-reentrant; no public pointer is retained except the BlobStore handle.
 *
 * Caller precondition: the opaque BlobStore arena and renderer arena are
 * completely disjoint. Public inputs/outputs must also be disjoint from both.
 * Renderer-arena overlap is rejected; BlobStore-arena overlap cannot be
 * proved because Core intentionally keeps that arena opaque. Every failure
 * leaves out_renderer unchanged.
 */
MeisoCompositionCpuRasterInitStatus
meiso_composition_cpu_raster_renderer_init(
    void *arena,
    size_t arena_size,
    MeisoBlobStore *blob_store,
    MeisoCompositionCpuRasterRenderer **out_renderer
);

/*
 * Binds an exact non-zero SceneStore control scope. A changed scope clears
 * all queued jobs, complete/result state, scratch, and both target sets, then
 * advances renderer_generation. Job-id high-water is retained. Exact repeat
 * is UNCHANGED. Counter exhaustion and every failure are zero-side-effect.
 */
MeisoCompositionCpuRasterScopeStatus
meiso_composition_cpu_raster_renderer_begin_control_scope(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope
);

/*
 * Synchronously validates/copies the exact active WorldView and canonical
 * desired value, then validates all referenced immutable Blob/MSH0 bytes.
 * No WorldView/desire/Blob byte pointer is retained. scope must equal the
 * bound scope; world must be the SceneStore active view in that scope. The
 * exact-current-active condition is a caller/Coordinator precondition because
 * the renderer deliberately does not borrow SceneStore. Queue capacity is 2.
 * Every non-QUEUED result leaves queue, counters used for identity, and
 * out_job unchanged; bounded diagnostic rejection counters may saturate.
 */
MeisoCompositionCpuRasterSubmitStatus
meiso_composition_cpu_raster_renderer_submit(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldView *world,
    uint64_t source_clock_epoch,
    MeisoCompositionCpuRasterJobRef *out_job
);

/*
 * Renders only the exact queue head. Immutable Blob validation and all
 * geometry/counter preflight complete before the inactive target is touched.
 * Success writes the inactive target, builds RenderResult/hash/thumbnail, and
 * atomically commits target+result while removing the head. Failure preserves
 * the old complete target/result and head; diagnostic failure counters may
 * saturate. out_complete changes only on COMPLETE. Evidence is exactly 0x3.
 */
MeisoCompositionCpuRasterRenderStatus
meiso_composition_cpu_raster_renderer_render_next(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterJobRef *expected_job,
    uint64_t render_start_monotonic_ns,
    uint64_t render_end_monotonic_ns,
    MeisoCompositionCpuRasterCompleteView *out_complete
);

/* Removes only the exact queue head and never changes the complete target. */
MeisoCompositionCpuRasterDiscardStatus
meiso_composition_cpu_raster_renderer_discard_head(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionCpuRasterJobRef *expected_job
);

/*
 * Atomic renderer-side SceneStore swap/disable boundary for an exact world in
 * the current scope. It removes all matching jobs. If latest complete matches,
 * it invalidates that borrow/result and zeroes that complete target set;
 * another world's complete remains intact.
 *
 * Caller contract: retire the old active world at the same logical boundary
 * as SceneStore activate/swap/disable and before exposing a replacement or
 * disabled state to renderer submission/dispatch. This prevents same-scope
 * retired jobs from completing late. Every non-RETIRED result is zero-change.
 */
MeisoCompositionCpuRasterRetireStatus
meiso_composition_cpu_raster_renderer_retire_world(
    MeisoCompositionCpuRasterRenderer *renderer,
    const MeisoCompositionSceneControlScope *scope,
    const MeisoCompositionSceneWorldRef *retired_world
);

/* Pure queries; false leaves output byte-exact unchanged. */
bool meiso_composition_cpu_raster_renderer_latest_complete(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterCompleteView *out_complete
);
bool meiso_composition_cpu_raster_renderer_snapshot(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterSnapshot *out_snapshot
);
bool meiso_composition_cpu_raster_renderer_stats(
    const MeisoCompositionCpuRasterRenderer *renderer,
    MeisoCompositionCpuRasterStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
