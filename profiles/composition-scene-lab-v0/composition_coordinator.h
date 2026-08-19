#ifndef MEISO_COMPOSITION_COORDINATOR_H
#define MEISO_COMPOSITION_COORDINATOR_H

#include "scene_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoCompositionCoordinator MeisoCompositionCoordinator;

typedef struct {
    MeisoCompositionSceneStore *scene_store;
    /* Exact non-zero READY Session binding for this coordinator lifetime. */
    MeisoSessionId session_id;
    uint64_t edge_incarnation;
    uint64_t capability_generation;
    uint64_t edge_clock_epoch;
} MeisoCompositionCoordinatorConfig;

typedef struct {
    uint64_t desired_owner_incarnation;
    uint64_t desired_state_version;
    uint64_t plan_generation;
    uint8_t candidate_digest[MEISO_SHA256_DIGEST_SIZE];
} MeisoCompositionCoordinatorPlanRef;

typedef struct {
    MeisoCompositionCoordinatorPlanRef ref;
    const MeisoCompositionScenePresentationPlan *plan;
    MeisoCompositionSceneCandidateState candidate_state;
    MeisoCompositionSceneActiveState active_state;
    bool prepared_token_present;
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoTimePoint first_observed_at;
    MeisoTimePoint prepared_at;
    MeisoTimePoint activated_at;
    uint32_t offline_lease_ms;
} MeisoCompositionCoordinatorPlanView;

typedef uint8_t MeisoCompositionCoordinatorInitStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_INIT_OK = 0,
    MEISO_COMPOSITION_COORDINATOR_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_COORDINATOR_INIT_MISALIGNED_ARENA = 2,
    MEISO_COMPOSITION_COORDINATOR_INIT_ARENA_TOO_SMALL = 3
};

typedef uint8_t MeisoCompositionCoordinatorOwnerStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_OWNER_STARTED = 0,
    MEISO_COMPOSITION_COORDINATOR_OWNER_UNCHANGED = 1,
    MEISO_COMPOSITION_COORDINATOR_OWNER_INVALID_ARGUMENT = 2,
    MEISO_COMPOSITION_COORDINATOR_OWNER_SCOPE_COUNTER_EXHAUSTED = 3
};

typedef uint8_t MeisoCompositionCoordinatorObserveStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_PREPARING = 0,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_DISABLE_PENDING = 1,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_ACTIVATION_PENDING = 2,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_DUPLICATE = 3,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_ALREADY_ACTIVE = 4,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_STALE = 5,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_CONFLICT = 6,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_TOKEN_MISMATCH = 7,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_PLAN = 8,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_WRONG_OWNER = 9,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_TIME = 10,
    MEISO_COMPOSITION_COORDINATOR_OBSERVE_INVALID_ARGUMENT = 11
};

typedef struct {
    uint64_t desired_owner_incarnation;
    uint64_t desired_state_version;
    bool enabled;
    MeisoTimePoint first_observed_at;
    MeisoCompositionCoordinatorObserveStatus result;
    MeisoCompositionScenePresentationReason reason;
} MeisoCompositionCoordinatorObservation;

/* Formal renderer/admission seam; C2 tests and later K1 adapters use it. */
typedef uint8_t MeisoCompositionCoordinatorBackendOutcome;
enum {
    MEISO_COMPOSITION_COORDINATOR_BACKEND_READY = 0,
    MEISO_COMPOSITION_COORDINATOR_BACKEND_PREPARE_FAILED = 1,
    MEISO_COMPOSITION_COORDINATOR_BACKEND_RENDERER_FAILED = 2,
    MEISO_COMPOSITION_COORDINATOR_BACKEND_CAPACITY = 3,
    /* Local prepare/render deadline only; unrelated to offline_lease_ms. */
    MEISO_COMPOSITION_COORDINATOR_BACKEND_DEADLINE = 4
};

typedef uint8_t MeisoCompositionCoordinatorReportStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_REPORT_PREPARED = 0,
    MEISO_COMPOSITION_COORDINATOR_REPORT_WAITING_SCENE = 1,
    MEISO_COMPOSITION_COORDINATOR_REPORT_REJECTED = 2,
    MEISO_COMPOSITION_COORDINATOR_REPORT_FAILED = 3,
    MEISO_COMPOSITION_COORDINATOR_REPORT_NO_CANDIDATE = 4,
    MEISO_COMPOSITION_COORDINATOR_REPORT_CANDIDATE_MISMATCH = 5,
    MEISO_COMPOSITION_COORDINATOR_REPORT_SCENE_MISMATCH = 6,
    MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_NONCE = 7,
    MEISO_COMPOSITION_COORDINATOR_REPORT_NONCE_REUSED = 8,
    MEISO_COMPOSITION_COORDINATOR_REPORT_COUNTER_EXHAUSTED = 9,
    MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_TIME = 10,
    MEISO_COMPOSITION_COORDINATOR_REPORT_INVALID_ARGUMENT = 11
};

typedef uint8_t MeisoCompositionCoordinatorActivateStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_ACTIVATED = 0,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_NOT_PENDING = 1,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_TOKEN_MISMATCH = 2,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISSING = 3,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_MISMATCH = 4,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_SCENE_COUNTER_EXHAUSTED = 5,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_TIME = 6,
    MEISO_COMPOSITION_COORDINATOR_ACTIVATE_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionCoordinatorSwapStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_SWAP_ACTIVATED = 0,
    MEISO_COMPOSITION_COORDINATOR_SWAP_NO_ACTIVE_PLAN = 1,
    MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISSING = 2,
    MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_MISMATCH = 3,
    MEISO_COMPOSITION_COORDINATOR_SWAP_SCENE_COUNTER_EXHAUSTED = 4,
    MEISO_COMPOSITION_COORDINATOR_SWAP_DISABLE_PENDING = 5,
    MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_TIME = 6,
    MEISO_COMPOSITION_COORDINATOR_SWAP_INVALID_ARGUMENT = 7
};

typedef uint8_t MeisoCompositionCoordinatorDisableStatus;
enum {
    MEISO_COMPOSITION_COORDINATOR_DISABLE_DISABLED = 0,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_ALREADY_DISABLED = 1,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_WAITING_DRAIN = 2,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_NO_INTENT = 3,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_SCENE_MISMATCH = 4,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_TIME = 5,
    MEISO_COMPOSITION_COORDINATOR_DISABLE_INVALID_ARGUMENT = 6
};

typedef struct {
    uint64_t owner_starts;
    uint64_t owner_changes;
    uint64_t desired_observations;
    uint64_t desired_duplicates;
    uint64_t desired_stale;
    uint64_t desired_conflicts;
    uint64_t invalid_plans;
    uint64_t token_mismatches;
    uint64_t candidates_superseded;
    uint64_t candidates_prepared;
    uint64_t backend_rejections;
    uint64_t backend_failures;
    uint64_t activations;
    uint64_t scene_swaps;
    uint64_t disables;
} MeisoCompositionCoordinatorStats;

size_t meiso_composition_coordinator_alignment_required(void);
size_t meiso_composition_coordinator_memory_required(void);

/*
 * The coordinator borrows SceneStore and owns no Core State/Stream, Session,
 * Runtime, heap, renderer, target, or driver handle. Caller must keep the two
 * arenas disjoint. Public outputs must not overlap either arena; overlap with
 * the coordinator arena is rejected, while opaque SceneStore overlap remains
 * a caller precondition. Calls are single-threaded and non-reentrant. The
 * coordinator is bound to exactly one READY SessionId. A new Session,
 * including a reconnect with the same desired owner incarnation, requires a
 * fresh coordinator init; its first begin_owner resets SceneStore scope. The
 * coordinator binds the resulting monotonic control-scope generation and
 * validates it on every API call, so any older coordinator sharing the same
 * SceneStore becomes unusable immediately after the reset.
 * Only begin_owner is valid between init and the first successful owner
 * start; all desired/backend/boundary/query APIs require that binding.
 */
MeisoCompositionCoordinatorInitStatus meiso_composition_coordinator_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionCoordinatorConfig *config,
    MeisoCompositionCoordinator **out_coordinator
);

MeisoCompositionCoordinatorOwnerStatus
meiso_composition_coordinator_begin_owner(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation
);

/*
 * Computes SHA-256 over canonical PLN0 with candidate_digest, token presence,
 * and token bytes zeroed. It validates every other Plan field against the v0
 * codec but does not require the input candidate_digest to be populated.
 */
bool meiso_composition_coordinator_plan_digest(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
);

/*
 * Call only for Core StateReplica RECEIVE_NEW or an exact duplicate, passing
 * its decoded Runtime desired value. enabled=false requires profile.size==0,
 * freezing exactly the 8-byte Runtime prefix with zero Profile suffix.
 * enabled=true requires profile.size==128; coordinator decodes PLN0 itself.
 * plan_generation is a per-owner/control-scope no-reuse high-water: a new
 * identity must be greater than every consumed generation. Exact PREPARING,
 * backend-terminal, or active identity may advance State version; activation
 * echo may advance a PREPARED candidate at a higher State version.
 * Retired or superseded generations cannot be reused.
 * Once PREPARED, a higher version without the exact token echo returns
 * TOKEN_MISMATCH and leaves the prepared candidate/token/ref unchanged.
 */
MeisoCompositionCoordinatorObserveStatus
meiso_composition_coordinator_observe_desired(
    MeisoCompositionCoordinator *coordinator,
    uint64_t desired_owner_incarnation,
    uint64_t desired_state_version,
    const MeisoRuntimeDesiredState *desired,
    MeisoTimePoint observed_at
);

/*
 * READY requires a non-zero 32-byte nonce that the caller guarantees is
 * globally fresh for this coordinator lifetime. NONCE_REUSED only detects
 * equality with the most recent successful nonce as defense in depth; it is
 * not a complete freshness oracle. The resulting token is a deterministic
 * binding, not authentication, confidentiality, or proof of entropy quality.
 * Other outcomes require nonce==NULL and retain old active plan/world while
 * terminating the candidate before activation.
 * expected_candidate must exactly match PREPARING, preventing a late backend
 * completion from mutating a superseding candidate. Terminal outcomes are
 * retryable only after Host publishes a higher desired State version.
 */
MeisoCompositionCoordinatorReportStatus
meiso_composition_coordinator_report_prepare_outcome(
    MeisoCompositionCoordinator *coordinator,
    const MeisoCompositionCoordinatorPlanRef *expected_candidate,
    MeisoCompositionCoordinatorBackendOutcome outcome,
    MeisoTimePoint completed_at,
    const uint8_t nonce[MEISO_SHA256_DIGEST_SIZE]
);

/*
 * Single logical frame boundary. A successful SceneStore activation is the
 * final fallible operation; plan/status commit follows by in-arena assignment
 * with no callback, so observers cannot see a half-activated transaction.
 * If SceneStore candidate identity changed after PREPARED, SCENE_MISSING or
 * SCENE_MISMATCH preserves the old active world but returns the Plan candidate
 * to PREPARING and invalidates its token. Backend must prepare again and Host
 * must echo the replacement token at a higher Plan State version.
 */
MeisoCompositionCoordinatorActivateStatus
meiso_composition_coordinator_activate_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    const uint8_t expected_token[MEISO_SHA256_DIGEST_SIZE],
    MeisoTimePoint boundary_at,
    MeisoCompositionSceneWorldRef *out_active_world
);

/* Activates a later SceneDesired under the already-active plan generation. */
MeisoCompositionCoordinatorSwapStatus
meiso_composition_coordinator_swap_scene_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    uint64_t expected_scene_owner_incarnation,
    uint64_t expected_scene_state_version,
    MeisoTimePoint boundary_at,
    MeisoCompositionSceneWorldRef *out_active_world
);

/*
 * Valid only for the current enabled=false desired. producer_drained=false is
 * a zero-side-effect WAITING_DRAIN result; true retires plan+world at the
 * supplied frame boundary. This API cannot manufacture a disable intent.
 */
MeisoCompositionCoordinatorDisableStatus
meiso_composition_coordinator_disable_frame_boundary(
    MeisoCompositionCoordinator *coordinator,
    bool producer_drained,
    MeisoTimePoint boundary_at
);

bool meiso_composition_coordinator_observation(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorObservation *out_observation
);
/*
 * PlanView.plan borrows coordinator storage. Any non-query call that can
 * mutate or supersede state ends the borrow regardless of its protocol
 * outcome; begin_owner and coordinator re-init also end it.
 */
bool meiso_composition_coordinator_candidate(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorPlanView *out_view
);
bool meiso_composition_coordinator_active(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorPlanView *out_view
);
bool meiso_composition_coordinator_status(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionScenePresentationStatus *out_status
);
bool meiso_composition_coordinator_stats(
    const MeisoCompositionCoordinator *coordinator,
    MeisoCompositionCoordinatorStats *out_stats
);

/*
 * offline_lease_ms is validated and retained in Plan views only. C2 has no
 * Session-loss observation, so it neither derives an offline deadline nor
 * performs hold/expiry. Backend DEADLINE is a separate local outcome.
 */

#ifdef __cplusplus
}
#endif

#endif
