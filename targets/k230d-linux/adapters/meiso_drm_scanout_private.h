#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SCANOUT_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_SCANOUT_PRIVATE_H

#include <meiso/edge/linux/drm_scanout.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_K230D_DRM_PRIVATE_API __attribute__((visibility("hidden")))
#else
#define MEISO_K230D_DRM_PRIVATE_API
#endif

enum {
    MEISO_K230D_DRM_MAGIC = 0x4b44524dU, /* KDRM */
    MEISO_K230D_DRM_COOKIE_PRESENT = 0x50524553U, /* PRES */
    MEISO_K230D_DRM_COOKIE_RESTORE = 0x52455354U /* REST */
};

typedef struct {
    uint32_t kind;
    uint64_t generation;
} MeisoK230dDrmEventCookie;

typedef uint8_t MeisoK230dDrmTopologyAdmissionStatus;
enum {
    MEISO_K230D_DRM_TOPOLOGY_ELIGIBLE = 0,
    MEISO_K230D_DRM_TOPOLOGY_BLOCKED = 1,
    MEISO_K230D_DRM_TOPOLOGY_INVALID_ARGUMENT = 2
};

/*
 * Pure, allocation-free result shared by the injected-Ops scanout path and
 * the native read-only topology probe. ELIGIBLE authorizes only a later
 * TEST_ONLY attempt; it is not a display/presentation result.
 */
typedef struct {
    bool topology_hash_valid;
    bool route_selected;
    bool plane_selected;
    MeisoK230dDrmAdmissionReason reason;
    uint8_t topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint32_t connector_index;
    uint32_t crtc_index;
    uint32_t plane_index;
    uint64_t minimum_output_bytes;
} MeisoK230dDrmTopologyAdmission;

typedef uint8_t MeisoK230dDrmSinglePlaneTestCandidateStatus;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_IDENTIFIED = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_BLOCKED = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_INVALID_ARGUMENT = 2
};

typedef uint8_t MeisoK230dDrmSinglePlaneTestCandidateReason;
enum {
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NONE = 0,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_CAPACITY = 1,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_NONCANONICAL = 2,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH = 3,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NO_ACTIVE_ROUTE = 4,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_ACTIVE_ROUTES = 5,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ROUTE_IDENTITY_MISMATCH = 6,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ACTIVE_CRTC_COUNT = 7,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_LEGACY_FRAMEBUFFER_ACTIVE = 8,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_ENUMERATED_PLANE_ACTIVE = 9,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NO_TEST_TARGET = 10,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_TEST_TARGETS = 11,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TEST_TARGET_IDENTITY_MISMATCH = 12,
    MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MODE_ENVELOPE = 13
};

typedef struct {
    uint8_t expected_topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;
} MeisoK230dDrmSinglePlaneTestCandidateRequest;

/*
 * Additive selector for the unique-plane diagnostic mode. The caller still
 * pins the expected topology identity and active route, but deliberately
 * supplies no plane id. A plane id is derived only after the supplied
 * snapshot passes every common candidate gate and contains exactly one
 * compatible target. This type is never reinterpreted as the explicit
 * request above, and zero is not a plane-id sentinel in either API.
 */
typedef struct {
    uint8_t expected_topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint32_t connector_id;
    uint32_t crtc_id;
} MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest;

/*
 * Pure selector result for a future diagnostic TEST_ONLY call.
 * IDENTIFIED means only that this exact snapshot and selector identify one
 * compatible, currently inactive enumerated DRM plane. It does not construct
 * an atomic request. No TEST_ONLY or real commit was attempted, and
 * visibility, exclusivity beyond enumerated DRM planes, Display Actual
 * RUNNING and physical evidence remain unproven.
 */
typedef struct {
    bool topology_hash_valid;
    bool topology_identity_matched;
    bool route_selected;
    bool enumerated_planes_inactive;
    bool test_target_selected;
    MeisoK230dDrmSinglePlaneTestCandidateReason reason;
    uint8_t topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint32_t connector_index;
    uint32_t crtc_index;
    uint32_t plane_index;
    uint32_t test_target_count;
    uint64_t minimum_output_bytes;
} MeisoK230dDrmSinglePlaneTestCandidate;

struct MeisoK230dDrmScanout {
    size_t arena_size;
    MeisoK230dDrmOps ops;
    void *ops_context;
    MeisoK230dDrmTopology topology;
    MeisoK230dDrmConnector selected_connector;
    MeisoK230dDrmCrtc selected_crtc;
    MeisoK230dDrmPlane selected_plane;
    MeisoK230dDrmAtomicPlaneState original_plane_state;
    MeisoK230dDrmAtomicPlaneState present_plane_state;
    MeisoK230dDrmDumbBuffer dumb;
    void *mapping;
    uint64_t generation;
    MeisoK230dDrmEventCookie present_cookie;
    MeisoK230dDrmEventCookie restore_cookie;
    MeisoK230dDrmScanoutReceipt scanout_receipt;
    MeisoK230dDrmCleanupReceipt cleanup_receipt;
    uint32_t magic;
    int fd;
    uint32_t framebuffer_id;
    uint32_t selected_crtc_index;
    MeisoK230dDrmPhase phase;
    bool fd_owned;
    bool master_owned;
    bool dumb_owned;
    bool mapping_owned;
    bool framebuffer_owned;
    bool present_commit_may_be_active;
    bool restore_commit_may_be_active;
    bool cleanup_callbacks_forbidden;
    bool callback_active;
    char device_path[MEISO_K230D_DRM_DEVICE_PATH_CAPACITY];
};

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_scanout_valid(
    const MeisoK230dDrmScanout *scanout
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_ops_valid(const MeisoK230dDrmOps *ops);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_path_size(
    const char *path,
    size_t *out_size
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_topology_valid(
    const MeisoK230dDrmTopology *topology
);

MEISO_K230D_DRM_PRIVATE_API
void meiso_k230d_drm_private_sha256(
    const uint8_t *bytes,
    size_t size,
    uint8_t digest[MEISO_K230D_DRM_SHA256_SIZE]
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_topology_sha256(
    const MeisoK230dDrmTopology *topology,
    uint8_t digest[MEISO_K230D_DRM_SHA256_SIZE]
);

MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmTopologyAdmissionStatus
meiso_k230d_drm_private_evaluate_topology(
    const MeisoK230dDrmTopology *topology,
    MeisoK230dDrmTopologyAdmission *out_admission
);

/*
 * Evaluates only the supplied bytes. The three object ranges must be pairwise
 * disjoint, request object ids must be nonzero, and INVALID_ARGUMENT leaves
 * out_candidate byte-exact unchanged. BLOCKED writes the first reason in this
 * order: capacity, canonical form, expected hash, unique active route, route
 * ids, active-CRTC count, legacy framebuffer, enumerated plane activity,
 * unique compatible target, target id, then mode envelope.
 *
 * A future consumer must fresh-read and re-evaluate on the same master-held fd
 * immediately before its TEST_ONLY call. This function neither authorizes nor
 * provides an API for a real commit. zpos is deliberately not a candidate
 * gate here; the existing begin()/real-commit policy remains unchanged.
 */
MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmSinglePlaneTestCandidateStatus
meiso_k230d_drm_private_evaluate_single_plane_test_candidate(
    const MeisoK230dDrmTopology *topology,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestCandidate *out_candidate
);

/*
 * Applies the same ordered policy as the explicit evaluator, except that no
 * caller-provided plane id exists to check after the unique-target gate.
 * The three object ranges must be pairwise disjoint, connector_id and crtc_id
 * must be nonzero, and INVALID_ARGUMENT leaves out_candidate byte-exact
 * unchanged. BLOCKED writes the first reason in this order: capacity,
 * canonical form, expected hash, unique active route, route ids, active-CRTC
 * count, legacy framebuffer, enumerated plane activity, unique compatible
 * target, then mode envelope. TEST_TARGET_IDENTITY_MISMATCH is therefore not
 * an initial result of this function.
 *
 * IDENTIFIED exposes the unique plane only through plane_index into the
 * caller-owned topology. It does not construct an explicit request, open a
 * device or attempt TEST_ONLY. A consuming executor must copy the observed
 * nonzero plane id and revalidate a frozen explicit request before creating
 * an atomic state.
 */
MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmSinglePlaneTestCandidateStatus
meiso_k230d_drm_private_derive_unique_plane_test_candidate(
    const MeisoK230dDrmTopology *topology,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    MeisoK230dDrmSinglePlaneTestCandidate *out_candidate
);

/*
 * Remap drmModePlane.possible_crtcs from libdrm resource order to the
 * object-id-sorted order required by MeisoK230dDrmTopology. Failure leaves
 * out_mask unchanged.
 */
MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_remap_crtc_mask(
    const uint32_t *resource_order_ids,
    uint32_t resource_count,
    const uint32_t *canonical_ids,
    uint32_t canonical_count,
    uint32_t resource_order_mask,
    uint32_t *out_mask
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_convert_frame(
    const MeisoK230dDrmFrameView *frame,
    uint32_t output_width,
    uint32_t output_height,
    uint32_t output_pitch,
    uint8_t *output,
    uint64_t output_size,
    uint32_t *out_viewport_x,
    uint32_t *out_viewport_y,
    uint32_t *out_viewport_width,
    uint32_t *out_viewport_height
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_frame_contract(
    const MeisoK230dDrmFrameView *frame,
    MeisoK230dDrmAdmissionReason *out_reason
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_plane_readback_equal(
    const MeisoK230dDrmPlane *expected,
    const MeisoK230dDrmPlane *actual
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_open(
    MeisoK230dDrmScanout *scanout,
    int *out_fd
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_capability(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmClientCapability capability
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_acquire_master(
    MeisoK230dDrmScanout *scanout
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_topology(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmTopology *out_topology
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_create_dumb(
    MeisoK230dDrmScanout *scanout,
    uint32_t width,
    uint32_t height,
    MeisoK230dDrmDumbBuffer *out_buffer
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_map(
    MeisoK230dDrmScanout *scanout,
    uint32_t handle,
    uint64_t size,
    void **out_mapping
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_add_framebuffer(
    MeisoK230dDrmScanout *scanout,
    uint32_t width,
    uint32_t height,
    uint32_t handle,
    uint32_t pitch,
    uint32_t *out_framebuffer_id
);

MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmCommitDisposition meiso_k230d_drm_private_call_atomic(
    MeisoK230dDrmScanout *scanout,
    const MeisoK230dDrmAtomicPlaneState *state,
    MeisoK230dDrmAtomicKind kind,
    void *event_cookie,
    int *out_error
);

MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmEventPollStatus meiso_k230d_drm_private_call_poll(
    MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmPageFlipEvent *out_event,
    int *out_error
);

MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_private_call_read_plane(
    MeisoK230dDrmScanout *scanout,
    uint32_t plane_id,
    MeisoK230dDrmPlane *out_plane
);

MEISO_K230D_DRM_PRIVATE_API
bool meiso_k230d_drm_private_release_resources(
    MeisoK230dDrmScanout *scanout
);

#endif
