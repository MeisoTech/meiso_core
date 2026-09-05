#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SINGLE_PLANE_TEST_ONLY_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SINGLE_PLANE_TEST_ONLY_PRIVATE_H

#include "meiso_drm_scanout_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Private, portable seam for exactly one diagnostic DRM atomic TEST_ONLY.
 * This type deliberately cannot express a real commit, page-flip event,
 * polling, restore, mapping, pixel writes, connector/CRTC mutation,
 * ALLOW_MODESET, generic atomic flags, or an event cookie.
 *
 * Every int callback returns zero on success or a positive errno on failure.
 * A negative return is a callback protocol violation and is normalized to
 * EPROTO. Output callbacks receive sentinel/canary storage and must leave it
 * byte-exact unchanged on failure. A failure that writes output, or a success
 * that returns an invalid fd/handle/id/dumb contract, is also EPROTO. The core
 * never guesses ownership from output written by a failed callback. The
 * callback table is called synchronously, serially and non-reentrantly on one
 * instance. Every pointer other than context is borrowed only for the active
 * callback and must not be retained: this includes path, topology/dumb/fb
 * outputs, the executor's local atomic state and out_error. context remains
 * caller-owned for the whole synchronous invocation.
 *
 * Ownership transfers to the core only after a successful callback returns a
 * nonnegative fd, or a nonzero handle or framebuffer id. fd 0 is valid. The
 * primary fd returned by open_primary must be the only user-space descriptor
 * reference owned by this executor invocation: callbacks must not dup, export,
 * lease or share that fd, nor export/share any GEM handle or framebuffer
 * created through it. Except for close_device, every callback failure must
 * leave the primary fd valid for the unified cleanup.
 * remove_framebuffer, destroy_dumb and drop_master retain their respective
 * ownership on failure, but cleanup still proceeds to the next step.
 * close_device is the terminal fence required of a future Linux DRM native
 * adapter: its first call consumes the primary fd and every remaining
 * fd-associated master/GEM/framebuffer ownership even when it reports an
 * error. It is never retried. This special rule does not apply to the existing
 * MeisoK230dDrmOps type.
 */
typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyCallDisposition;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED = 2
};

typedef struct {
    int (*open_primary)(void *context, const char *path, int *out_fd);
    int (*enable_universal_planes)(void *context, int fd);
    int (*enable_atomic)(void *context, int fd);
    int (*acquire_master)(void *context, int fd);
    int (*verify_master)(void *context, int fd);
    int (*read_topology)(
        void *context,
        int fd,
        MeisoK230dDrmTopology *out_topology
    );
    int (*create_rgb565_dumb)(
        void *context,
        int fd,
        uint32_t width,
        uint32_t height,
        MeisoK230dDrmDumbBuffer *out_buffer
    );
    int (*add_rgb565_framebuffer)(
        void *context,
        int fd,
        uint32_t width,
        uint32_t height,
        uint32_t handle,
        uint32_t pitch,
        uint32_t *out_framebuffer_id
    );
    /*
     * ACCEPTED and REJECTED both guarantee that no state was applied.
     * OPERATION_FAILED also guarantees no state was applied. ACCEPTED writes
     * zero to out_error; the other dispositions write a positive errno. An
     * out-of-domain disposition, accepted with nonzero error, or either other
     * disposition with non-positive error is EPROTO/OPERATION_FAILURE. Once
     * this callback is invoked, the core attempts topology read #3 even after
     * any such protocol violation. A violation records
     * TEST_OPERATION_FAILED_NOT_APPLIED with test_error EPROTO.
     */
    MeisoK230dDrmSinglePlaneTestOnlyCallDisposition (*atomic_test_only)(
        void *context,
        int fd,
        const MeisoK230dDrmAtomicPlaneState *state,
        int *out_error
    );
    int (*remove_framebuffer)(void *context, int fd, uint32_t framebuffer_id);
    int (*destroy_dumb)(void *context, int fd, uint32_t handle);
    int (*drop_master)(void *context, int fd);
    int (*close_device)(void *context, int fd);
} MeisoK230dDrmSinglePlaneTestOnlyOps;

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyTestObservation;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED = 3
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG = 2
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyOutcome;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE = 3,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE = 4
};

/* The primary outcome is retained even if postconditions or cleanup fail. */
typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyPrimaryOutcome;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE = 3
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyOperationStage;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC = 3,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER = 4,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER = 5,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY = 6,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE = 7,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB = 8,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB = 9,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER = 10,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY = 11,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE = 12,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY = 13
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyPostconditionStage;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_EVALUATE_CANDIDATE = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED = 3
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestOnlyCleanupStage;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER = 3,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE = 4
};

typedef struct {
    const char *device_path;
    const MeisoK230dDrmSinglePlaneTestOnlyOps *ops;
    void *ops_context;
} MeisoK230dDrmSinglePlaneTestOnlyConfig;

/*
 * RESULT_AVAILABLE always returns a complete ledger. outcome is the final
 * status; primary_outcome preserves the state reached before a postcondition
 * or cleanup failure. operation_failure_* records the first ordinary
 * operation error. postcondition_failure_* independently records the first
 * topology-#3 error after TEST_ONLY. cleanup_failure_* records the first error
 * in remove -> destroy -> drop -> close order.
 *
 * *_was_* fields are history only. Every *_ownership_consumed field must be
 * true for the corresponding successfully acquired resource before return;
 * none of these fields means an object remains available to the caller.
 */
typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlyOutcome outcome;
    MeisoK230dDrmSinglePlaneTestOnlyPrimaryOutcome primary_outcome;
    MeisoK230dDrmSinglePlaneTestOnlyTestObservation test_observation;
    MeisoK230dDrmSinglePlaneTestOnlyOperationStage operation_failure_stage;
    MeisoK230dDrmSinglePlaneTestOnlyPostconditionStage
        postcondition_failure_stage;
    MeisoK230dDrmSinglePlaneTestOnlyCleanupStage cleanup_failure_stage;
    int operation_error;
    int postcondition_error;
    int cleanup_error;
    int test_error;

    MeisoK230dDrmSinglePlaneTestCandidateReason initial_candidate_reason;
    MeisoK230dDrmSinglePlaneTestCandidateReason pretest_candidate_reason;
    MeisoK230dDrmSinglePlaneTestCandidateReason posttest_candidate_reason;
    uint8_t initial_topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint8_t pretest_topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint8_t posttest_topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint32_t topology_read_attempt_count;
    uint32_t topology_read_success_count;
    uint32_t atomic_test_only_call_count;

    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    /*
     * Historical diagnostics only. These values describe objects that were
     * created and consumed; callers must not use them for cleanup or infer a
     * live handle/id from them.
     */
    uint32_t created_framebuffer_id;
    MeisoK230dDrmDumbBuffer created_dumb;
    uint64_t minimum_output_bytes;

    bool initial_candidate_identified;
    bool pretest_candidate_identified;
    bool posttest_candidate_identified;
    bool pretest_topology_unchanged;
    bool posttest_topology_unchanged;
    bool test_only_attempted;
    bool test_callback_reported_accepted_not_applied;

    bool fd_was_opened;
    bool universal_planes_enabled;
    bool atomic_enabled;
    bool master_was_acquired;
    bool master_verified;
    bool dumb_was_created;
    bool framebuffer_was_created;
    bool remove_framebuffer_attempted;
    bool remove_framebuffer_succeeded;
    bool destroy_dumb_attempted;
    bool destroy_dumb_succeeded;
    bool drop_master_attempted;
    bool drop_master_succeeded;
    bool close_attempted;
    bool close_succeeded;
    bool fd_ownership_consumed;
    bool master_ownership_consumed;
    bool dumb_ownership_consumed;
    bool framebuffer_ownership_consumed;

    bool pixel_payload_present;
} MeisoK230dDrmSinglePlaneTestOnlyResult;

/*
 * Invocation validation is ordered and exact. INVALID_ARGUMENT is returned
 * first for a NULL config/request/out_result, overlap among those three fixed
 * objects, or any zero connector/crtc/plane id in request. INVALID_CONFIG is
 * returned next for a NULL device_path/ops/ops_context, a non-absolute,
 * unterminated or over-capacity path, a missing callback, overlap involving
 * the known ops/path ranges, or an ops_context base inside any known range.
 * The callback implementation owns the stronger full-context disjointness
 * rule because this API does not know its allocation size. Both statuses
 * invoke no callback and leave out_result byte-exact unchanged.
 *
 * A RESULT_AVAILABLE invocation is one-shot: it performs all work and its
 * unified epilogue before assigning out_result once. Every opened fd reaches
 * close_device exactly once. The three topology reads are initial selection,
 * fresh pre-TEST_ONLY validation, and mandatory post-TEST_ONLY observation.
 * An accepted/rejected/failed TEST_ONLY always attempts the third read,
 * including after a callback protocol violation. An initial or pre-test
 * candidate BLOCKED result is clean NOT_TESTED with no operation error;
 * evaluator INVALID_ARGUMENT, or an IDENTIFIED pre-test candidate differing
 * from the same-hash initial candidate, is internal
 * EPROTO/OPERATION_FAILURE. Post-test
 * read failure is POSTCONDITION_READ_TOPOLOGY with its positive errno (or
 * EPROTO for callback violation); evaluator INVALID_ARGUMENT is
 * POSTCONDITION_EVALUATE_CANDIDATE/EPROTO; evaluator BLOCKED or semantic
 * topology/candidate drift is POSTCONDITION_TOPOLOGY_CHANGED/ESTALE. These
 * failures preserve the recorded test observation.
 */
MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_k230d_drm_private_run_single_plane_test_only(
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyResult *out_result
);

#ifdef __cplusplus
}
#endif

#endif
