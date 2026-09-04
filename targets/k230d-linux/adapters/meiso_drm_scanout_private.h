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
