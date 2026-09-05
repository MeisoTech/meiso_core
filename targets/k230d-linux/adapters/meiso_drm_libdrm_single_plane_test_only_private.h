#ifndef MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_LIBDRM_SINGLE_PLANE_TEST_ONLY_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_ADAPTERS_DRM_LIBDRM_SINGLE_PLANE_TEST_ONLY_PRIVATE_H

#include "meiso_drm_libdrm_topology_private.h"
#include "meiso_drm_single_plane_test_only_private.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT = 3,
    MEISO_K230D_DRM_NATIVE_TEST_ONLY_FLAG_VALUE = 0x0100,
    MEISO_K230D_DRM_NATIVE_TEST_ONLY_RECEIPT_CAPACITY = 8192
};

/*
 * Native call ledger owned by the one-shot libdrm binder. It is independent
 * of the portable executor result: a runner must validate and serialize both
 * rather than infer native calls from portable state transitions.
 *
 * topology_read_* slots are written in call order. Each slot proves the
 * libdrm objects acquired by that read were released before it returned.
 * portable_atomic_callback_call_count distinguishes entry into the injected
 * Ops callback from atomic_commit_api_call_count: allocation or property
 * construction can fail before calling drmModeAtomicCommit. These counters
 * count this adapter's API calls, not libdrm's internal syscall attempts or
 * retries.
 *
 * The final counters are structural non-claims. This adapter has no callback
 * or helper capable of mapping/writing pixels, adding connector/CRTC
 * properties, creating mode blobs, issuing legacy modesets, or submitting a
 * non-TEST_ONLY atomic commit.
 */
typedef struct {
    uint32_t open_primary_call_count;
    int open_primary_error;
    uint32_t fstat_call_count;
    uint32_t primary_node_type_call_count;
    bool native_fd_opened;
    bool primary_node_verified;
    uint32_t open_failure_close_call_count;
    int open_failure_close_error;
    bool open_failure_close_terminal_consumed;

    bool same_fd_contract_applicable;
    bool same_fd_contract_verified;
    uint32_t same_fd_check_count;

    uint32_t universal_planes_cap_call_count;
    uint32_t atomic_cap_call_count;
    int universal_planes_cap_error;
    int atomic_cap_error;
    bool universal_planes_cap_set;
    bool atomic_cap_set;
    uint32_t set_master_call_count;
    uint32_t is_master_call_count;
    int set_master_error;
    int is_master_error;
    bool master_was_acquired;
    bool master_verified;

    uint32_t topology_read_call_count;
    bool topology_read_attempted[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    MeisoK230dDrmNativeReadStatus topology_read_status[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    MeisoK230dDrmNativeStage topology_read_stage[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    MeisoK230dDrmAdmissionReason topology_read_preclassified_reason[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    int topology_read_error[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    bool topology_read_ready_for_evaluation[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    bool topology_read_topology_complete[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    bool topology_read_all_plane_properties_complete[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    uint32_t topology_read_raw_connector_count[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    uint32_t topology_read_raw_crtc_count[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    uint32_t topology_read_raw_plane_count[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    uint32_t topology_read_objects_seen[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    uint32_t topology_read_objects_released[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];
    bool topology_read_objects_all_released[
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT
    ];

    uint32_t create_dumb_api_call_count;
    uint32_t add_framebuffer_call_count;
    int create_dumb_error;
    int add_framebuffer_error;
    uint32_t portable_atomic_callback_call_count;
    uint32_t atomic_request_alloc_call_count;
    bool atomic_request_alloc_succeeded;
    int atomic_request_alloc_error;
    uint32_t atomic_plane_property_add_attempt_count;
    uint32_t atomic_plane_property_add_success_count;
    int atomic_plane_property_add_error;
    uint32_t atomic_commit_api_call_count;
    uint32_t atomic_commit_flags;
    bool atomic_commit_user_data_null;
    int atomic_commit_error;
    uint32_t atomic_request_free_call_count;

    uint32_t remove_framebuffer_call_count;
    uint32_t destroy_dumb_api_call_count;
    uint32_t drop_master_call_count;
    uint32_t close_call_count;
    int remove_framebuffer_error;
    int destroy_dumb_error;
    int drop_master_error;
    int close_error;
    bool close_terminal_consumed;

    uint32_t map_call_count;
    uint32_t pixel_write_call_count;
    uint32_t connector_property_add_call_count;
    uint32_t crtc_property_add_call_count;
    uint32_t mode_blob_create_call_count;
    uint32_t legacy_modeset_call_count;
    uint32_t non_test_atomic_commit_call_count;
    uint32_t allow_modeset_flag_use_count;
    uint32_t page_flip_event_flag_use_count;
} MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger;

typedef struct {
    MeisoK230dDrmSinglePlaneTestOnlyResult portable;
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger native;
} MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult;

/*
 * Synchronously binds the portable TEST_ONLY-only executor to libdrm. The
 * three input/output ranges must be pairwise disjoint. INVALID_ARGUMENT or
 * INVALID_CONFIG leaves out_result byte-exact unchanged. RESULT_AVAILABLE
 * writes both the portable result and an independent native ledger once all
 * cleanup, including the Linux close terminal fence, has completed.
 *
 * The at-most-one drmModeAtomicCommit API call uses exactly
 * DRM_MODE_ATOMIC_TEST_ONLY and NULL user data. Only EINVAL is
 * rejected-not-applied. Any other positive errno that libdrm ultimately
 * returns to this program is operation-failed-not-applied; this one-shot
 * diagnostic makes no second API call. Any retry internal to libdrm is
 * outside this ledger's observation.
 */
MEISO_K230D_DRM_PRIVATE_API
MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_k230d_drm_libdrm_run_single_plane_test_only(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
);

typedef MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
    (*MeisoK230dDrmNativeSinglePlaneTestOnlyExecuteFn)(
        const char *device_path,
        const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
        MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
    );

/*
 * Host-testable process seam. The production main binds the native function
 * above. The exact CLI is:
 *
 *   PROGRAM --device ABSOLUTE_PATH --topology-sha256 LOWER_HEX_64
 *       --connector-id CANONICAL_U32 --crtc-id CANONICAL_U32
 *       --plane-id CANONICAL_U32
 *
 * NULL or aliased output/error streams return 2 without I/O. Other argument
 * failure writes only E_ARGUMENT to error_stream and returns 2.
 * An execute/result contract violation writes only E_PROVIDER_CONTRACT and
 * returns 1. A valid result emits exactly one bounded canonical JSON line to
 * out_stream; normal NOT_TESTED/REJECTED/ACCEPTED_NOT_APPLIED returns zero,
 * while operational or cleanup failure returns 1 with the result receipt.
 */
MEISO_K230D_DRM_PRIVATE_API
int meiso_k230d_drm_single_plane_test_only_once_run(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeSinglePlaneTestOnlyExecuteFn execute
);

#ifdef __cplusplus
}
#endif

#endif
