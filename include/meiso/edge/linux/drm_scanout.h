#ifndef MEISO_EDGE_LINUX_DRM_SCANOUT_H
#define MEISO_EDGE_LINUX_DRM_SCANOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Portable policy/state-machine core for a future K230D Linux local DRM/KMS
 * one-frame probe. This build-only adapter is not a Resource, Runtime,
 * transport, native libdrm implementation, or installed SDK surface. It never
 * changes a connector, CRTC, mode, or vo_init state. The only writable KMS
 * object is an unused OVERLAY plane already compatible with the active CRTC.
 */

#define MEISO_K230D_DRM_DEVICE_PATH_CAPACITY 192U
#define MEISO_K230D_DRM_DRIVER_NAME_CAPACITY 64U
#define MEISO_K230D_DRM_MODE_NAME_CAPACITY 32U
#define MEISO_K230D_DRM_MAX_CONNECTORS 8U
#define MEISO_K230D_DRM_MAX_CRTCS 8U
#define MEISO_K230D_DRM_MAX_PLANES 16U
#define MEISO_K230D_DRM_SHA256_SIZE 32U
#define MEISO_K230D_DRM_RGB565_FOURCC UINT32_C(0x36314752) /* DRM_FORMAT_RGB565 */
#define MEISO_K230D_DRM_MAX_DIMENSION 4096U
#define MEISO_K230D_DRM_MAX_INPUT_BYTES UINT64_C(67108864)
#define MEISO_K230D_DRM_MAX_DUMB_BYTES UINT64_C(67108864)

typedef struct MeisoK230dDrmScanout MeisoK230dDrmScanout;

typedef uint8_t MeisoK230dDrmInputFormat;
enum {
    /* Packed R,G,B,A bytes with A == 255 for every pixel; no row stride. */
    MEISO_K230D_DRM_INPUT_RGBA8888 = 1,
    /* Packed 16-bit RGB565 pixels, most-significant byte first; no stride. */
    MEISO_K230D_DRM_INPUT_RGB565_BE = 2
};

/* size is exactly width * height * bytes-per-pixel for the selected format. */
typedef struct {
    MeisoK230dDrmInputFormat format;
    uint32_t width;
    uint32_t height;
    const uint8_t *bytes;
    size_t size;
} MeisoK230dDrmFrameView;

typedef struct {
    char name[MEISO_K230D_DRM_MODE_NAME_CAPACITY];
    uint32_t clock_khz;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t flags;
    uint32_t type;
} MeisoK230dDrmMode;

typedef struct {
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    bool connected;
} MeisoK230dDrmConnector;

typedef struct {
    uint32_t crtc_id;
    uint32_t framebuffer_id;
    uint32_t x;
    uint32_t y;
    bool active;
    bool mode_valid;
    MeisoK230dDrmMode mode;
} MeisoK230dDrmCrtc;

typedef uint8_t MeisoK230dDrmPlaneType;
enum {
    MEISO_K230D_DRM_PLANE_TYPE_UNKNOWN = 0,
    MEISO_K230D_DRM_PLANE_TYPE_PRIMARY = 1,
    MEISO_K230D_DRM_PLANE_TYPE_OVERLAY = 2,
    MEISO_K230D_DRM_PLANE_TYPE_CURSOR = 3
};

typedef uint8_t MeisoK230dDrmPlanePropertyIndex;
enum {
    MEISO_K230D_DRM_PLANE_FB_ID = 0,
    MEISO_K230D_DRM_PLANE_CRTC_ID = 1,
    MEISO_K230D_DRM_PLANE_SRC_X = 2,
    MEISO_K230D_DRM_PLANE_SRC_Y = 3,
    MEISO_K230D_DRM_PLANE_SRC_W = 4,
    MEISO_K230D_DRM_PLANE_SRC_H = 5,
    MEISO_K230D_DRM_PLANE_CRTC_X = 6,
    MEISO_K230D_DRM_PLANE_CRTC_Y = 7,
    MEISO_K230D_DRM_PLANE_CRTC_W = 8,
    MEISO_K230D_DRM_PLANE_CRTC_H = 9,
    MEISO_K230D_DRM_PLANE_PROPERTY_COUNT = 10
};

typedef struct {
    uint32_t property_id;
    uint64_t value;
} MeisoK230dDrmPlaneProperty;

typedef struct {
    uint32_t plane_id;
    /* Bits index the canonical, object-id-sorted topology.crtcs[] array. */
    uint32_t possible_crtcs;
    MeisoK230dDrmPlaneType type;
    bool type_immutable;
    bool supports_rgb565;
    bool zpos_present;
    bool zpos_immutable;
    int64_t zpos;
    MeisoK230dDrmPlaneProperty
        properties[MEISO_K230D_DRM_PLANE_PROPERTY_COUNT];
} MeisoK230dDrmPlane;

/*
 * A native topology callback returns every connector, CRTC and universal
 * plane visible on the fd, in strictly increasing object-id order. A future
 * native translator must remap libdrm's resource-order possible_crtcs bits to
 * this canonical sorted CRTC order before returning. Counts
 * exceeding these fixed capacities are an admission failure, never truncation.
 * Unused array suffixes and string suffixes are canonical zero.
 */
typedef struct {
    char driver_name[MEISO_K230D_DRM_DRIVER_NAME_CAPACITY];
    uint32_t driver_version_major;
    uint32_t driver_version_minor;
    uint32_t driver_version_patch;
    bool page_flip_timestamps_monotonic;
    uint32_t connector_count;
    uint32_t crtc_count;
    uint32_t plane_count;
    MeisoK230dDrmConnector connectors[MEISO_K230D_DRM_MAX_CONNECTORS];
    MeisoK230dDrmCrtc crtcs[MEISO_K230D_DRM_MAX_CRTCS];
    MeisoK230dDrmPlane planes[MEISO_K230D_DRM_MAX_PLANES];
} MeisoK230dDrmTopology;

typedef struct {
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
} MeisoK230dDrmDumbBuffer;

typedef struct {
    uint32_t plane_id;
    MeisoK230dDrmPlaneProperty
        properties[MEISO_K230D_DRM_PLANE_PROPERTY_COUNT];
} MeisoK230dDrmAtomicPlaneState;

typedef uint8_t MeisoK230dDrmAtomicKind;
enum {
    MEISO_K230D_DRM_ATOMIC_TEST_ONLY = 1,
    MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT = 2
};

typedef uint8_t MeisoK230dDrmCommitDisposition;
enum {
    MEISO_K230D_DRM_COMMIT_ACCEPTED = 0,
    MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED = 1,
    MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN = 2
};

typedef struct {
    const void *cookie;
    uint32_t crtc_id;
    uint32_t sequence;
    uint64_t monotonic_ns;
} MeisoK230dDrmPageFlipEvent;

typedef uint8_t MeisoK230dDrmEventPollStatus;
enum {
    MEISO_K230D_DRM_EVENT = 0,
    MEISO_K230D_DRM_EVENT_WOULD_BLOCK = 1,
    MEISO_K230D_DRM_EVENT_INTERRUPTED = 2,
    MEISO_K230D_DRM_EVENT_TIMEOUT = 3,
    MEISO_K230D_DRM_EVENT_FAILED = 4
};

typedef uint8_t MeisoK230dDrmClientCapability;
enum {
    MEISO_K230D_DRM_CLIENT_UNIVERSAL_PLANES = 1,
    MEISO_K230D_DRM_CLIENT_ATOMIC = 2
};

/*
 * Narrow test/native seam. Every integer callback returns zero on success or
 * a positive errno on failure and does not modify output bytes on failure.
 * It is serialized and non-reentrant. atomic_commit accepts only the two
 * kinds above: TEST_ONLY or NONBLOCK|PAGE_FLIP_EVENT; ALLOW_MODESET and
 * connector/CRTC properties cannot be expressed through this interface.
 *
 * TEST_ONLY always receives a NULL cookie and may return only ACCEPTED or
 * DEFINITELY_NOT_APPLIED. PAGE_FLIP_EVENT always receives a
 * non-NULL cookie. ACCEPTED writes zero to out_error; both other dispositions
 * write a positive error. A real-commit OUTCOME_UNKNOWN is never safe to
 * release. poll_event may return WOULD_BLOCK/INTERRUPTED without changing out_event or
 * out_error. EVENT changes only out_event. TIMEOUT/FAILED changes only
 * out_error to a positive errno. read_plane returns the current plane,
 * including the ten writable property ids/values and immutable type/zpos,
 * for exact post-restore comparison.
 *
 * A successful map_dumb_buffer must return a range disjoint from this
 * adapter's arena, the current FrameView and its bytes, and callback context.
 * The policy core rejects every overlap it can prove. Because this narrow Ops
 * API does not know the context object's size, it can only check whether the
 * context base address itself falls inside the returned mapping; the callback
 * implementation remains responsible for the complete context-range rule.
 *
 * remove_framebuffer, unmap_dumb_buffer, destroy_dumb_buffer and drop_master
 * retain ownership when they fail. close_device transfers fd ownership on its
 * first invocation even if it reports an error; it must never be retried.
 */
typedef struct {
    int (*open_device)(void *context, const char *path, int *out_fd);
    int (*set_client_capability)(
        void *context,
        int fd,
        MeisoK230dDrmClientCapability capability
    );
    int (*acquire_master)(void *context, int fd);
    int (*read_topology)(
        void *context,
        int fd,
        MeisoK230dDrmTopology *out_topology
    );
    int (*create_dumb_buffer)(
        void *context,
        int fd,
        uint32_t width,
        uint32_t height,
        MeisoK230dDrmDumbBuffer *out_buffer
    );
    int (*map_dumb_buffer)(
        void *context,
        int fd,
        uint32_t handle,
        uint64_t size,
        void **out_mapping
    );
    int (*add_framebuffer)(
        void *context,
        int fd,
        uint32_t width,
        uint32_t height,
        uint32_t fourcc,
        uint32_t handle,
        uint32_t pitch,
        uint32_t *out_framebuffer_id
    );
    MeisoK230dDrmCommitDisposition (*atomic_commit)(
        void *context,
        int fd,
        const MeisoK230dDrmAtomicPlaneState *state,
        MeisoK230dDrmAtomicKind kind,
        void *event_cookie,
        int *out_error
    );
    MeisoK230dDrmEventPollStatus (*poll_event)(
        void *context,
        int fd,
        MeisoK230dDrmPageFlipEvent *out_event,
        int *out_error
    );
    int (*read_plane)(
        void *context,
        int fd,
        uint32_t plane_id,
        MeisoK230dDrmPlane *out_plane
    );
    int (*remove_framebuffer)(
        void *context,
        int fd,
        uint32_t framebuffer_id
    );
    int (*unmap_dumb_buffer)(void *context, void *mapping, uint64_t size);
    int (*destroy_dumb_buffer)(void *context, int fd, uint32_t handle);
    int (*drop_master)(void *context, int fd);
    int (*close_device)(void *context, int fd);
} MeisoK230dDrmOps;

typedef struct {
    const char *device_path;
    /* The table is copied by init. The context remains borrowed. */
    const MeisoK230dDrmOps *ops;
    /* Must remain valid until STOPPED or operator handling of a terminal state. */
    void *ops_context;
} MeisoK230dDrmConfig;

typedef uint8_t MeisoK230dDrmPhase;
enum {
    MEISO_K230D_DRM_PHASE_STOPPED = 1,
    MEISO_K230D_DRM_PHASE_PRESENT_PENDING = 2,
    MEISO_K230D_DRM_PHASE_KMS_LATCHED = 3,
    MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN = 4,
    MEISO_K230D_DRM_PHASE_RESTORE_PENDING = 5,
    MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN = 6,
    MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED = 7
};

typedef uint8_t MeisoK230dDrmAdmissionReason;
enum {
    MEISO_K230D_DRM_ADMISSION_NONE = 0,
    MEISO_K230D_DRM_ADMISSION_INPUT_FORMAT = 1,
    MEISO_K230D_DRM_ADMISSION_INPUT_ENVELOPE = 2,
    MEISO_K230D_DRM_ADMISSION_INPUT_NOT_OPAQUE = 3,
    MEISO_K230D_DRM_ADMISSION_MASTER_UNAVAILABLE = 4,
    MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY = 5,
    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_CAPACITY = 6,
    MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL = 7,
    MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE = 8,
    MEISO_K230D_DRM_ADMISSION_MULTIPLE_ACTIVE_ROUTES = 9,
    MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY = 10,
    MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE = 11,
    MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE = 12,
    MEISO_K230D_DRM_ADMISSION_BUFFER_ENVELOPE = 13,
    MEISO_K230D_DRM_ADMISSION_ATOMIC_TEST_REJECTED = 14,
    MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE = 15
};

typedef uint8_t MeisoK230dDrmEvidenceClass;
enum {
    /* This is the only class this portable/injected-Ops slice can emit. */
    MEISO_K230D_DRM_EVIDENCE_INJECTED_OPS_UNVERIFIED = 1
};

typedef uint8_t MeisoK230dDrmFailureStage;
enum {
    MEISO_K230D_DRM_FAILURE_NONE = 0,
    MEISO_K230D_DRM_FAILURE_OPEN = 1,
    MEISO_K230D_DRM_FAILURE_CLIENT_CAPABILITY = 2,
    MEISO_K230D_DRM_FAILURE_ACQUIRE_MASTER = 3,
    MEISO_K230D_DRM_FAILURE_TOPOLOGY = 4,
    MEISO_K230D_DRM_FAILURE_CREATE_DUMB = 5,
    MEISO_K230D_DRM_FAILURE_MAP_DUMB = 6,
    MEISO_K230D_DRM_FAILURE_ADD_FRAMEBUFFER = 7,
    MEISO_K230D_DRM_FAILURE_PRESENT_TEST = 8,
    MEISO_K230D_DRM_FAILURE_PRESENT_COMMIT = 9,
    MEISO_K230D_DRM_FAILURE_PRESENT_EVENT = 10,
    MEISO_K230D_DRM_FAILURE_RESTORE_TEST = 11,
    MEISO_K230D_DRM_FAILURE_RESTORE_COMMIT = 12,
    MEISO_K230D_DRM_FAILURE_RESTORE_EVENT = 13,
    MEISO_K230D_DRM_FAILURE_RESTORE_READBACK = 14,
    MEISO_K230D_DRM_FAILURE_REMOVE_FRAMEBUFFER = 15,
    MEISO_K230D_DRM_FAILURE_UNMAP_DUMB = 16,
    MEISO_K230D_DRM_FAILURE_DESTROY_DUMB = 17,
    MEISO_K230D_DRM_FAILURE_DROP_MASTER = 18,
    MEISO_K230D_DRM_FAILURE_CLOSE = 19
};

typedef struct {
    bool receipt_valid;
    bool kms_latched;
    bool outcome_unknown;
    bool physical_evidence;
    MeisoK230dDrmEvidenceClass evidence_class;
    MeisoK230dDrmAdmissionReason admission_reason;
    MeisoK230dDrmFailureStage failure_stage;
    int error;
    char driver_name[MEISO_K230D_DRM_DRIVER_NAME_CAPACITY];
    uint32_t driver_version_major;
    uint32_t driver_version_minor;
    uint32_t driver_version_patch;
    uint8_t topology_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    uint8_t input_sha256[MEISO_K230D_DRM_SHA256_SIZE];
    MeisoK230dDrmInputFormat input_format;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t crtc_id;
    uint32_t crtc_index;
    MeisoK230dDrmMode mode;
    uint32_t plane_id;
    MeisoK230dDrmPlaneType plane_type;
    int64_t plane_zpos;
    uint32_t framebuffer_id;
    uint32_t dumb_handle;
    uint32_t pitch;
    uint64_t dumb_size;
    uint32_t output_fourcc;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t viewport_width;
    uint32_t viewport_height;
    bool test_only_accepted;
    bool commit_accepted;
    bool matching_event_observed;
    uint32_t event_sequence;
    uint64_t event_monotonic_ns;
} MeisoK230dDrmScanoutReceipt;

typedef struct {
    bool restore_required;
    bool restore_test_accepted;
    bool restore_commit_accepted;
    bool matching_event_observed;
    bool property_readback_matched;
    bool outcome_unknown;
    bool framebuffer_removed;
    bool mapping_unmapped;
    bool dumb_buffer_destroyed;
    bool master_dropped;
    bool device_closed;
    bool retained_resources;
    MeisoK230dDrmFailureStage failure_stage;
    int error;
    uint32_t event_sequence;
    uint64_t event_monotonic_ns;
} MeisoK230dDrmCleanupReceipt;

typedef struct {
    MeisoK230dDrmPhase phase;
    bool fd_owned;
    bool master_owned;
    bool dumb_owned;
    bool mapping_owned;
    bool framebuffer_owned;
    bool present_commit_may_be_active;
    bool restore_commit_may_be_active;
    /* A detected Ops mapping alias makes every later cleanup callback unsafe. */
    bool cleanup_callbacks_forbidden;
    MeisoK230dDrmScanoutReceipt scanout;
    MeisoK230dDrmCleanupReceipt cleanup;
    size_t arena_bytes;
} MeisoK230dDrmSnapshot;

typedef uint8_t MeisoK230dDrmInitStatus;
enum {
    MEISO_K230D_DRM_INIT_OK = 0,
    MEISO_K230D_DRM_INIT_INVALID_ARGUMENT = 1,
    MEISO_K230D_DRM_INIT_INVALID_CONFIG = 2,
    MEISO_K230D_DRM_INIT_MISALIGNED_ARENA = 3,
    MEISO_K230D_DRM_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoK230dDrmBeginStatus;
enum {
    MEISO_K230D_DRM_BEGIN_PRESENT_PENDING = 0,
    MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED = 1,
    MEISO_K230D_DRM_BEGIN_OPERATION_FAILED = 2,
    MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED = 3,
    MEISO_K230D_DRM_BEGIN_BUSY = 4,
    MEISO_K230D_DRM_BEGIN_TERMINAL = 5,
    MEISO_K230D_DRM_BEGIN_INVALID_ARGUMENT = 6,
    MEISO_K230D_DRM_BEGIN_OUTCOME_UNKNOWN = 7
};

typedef uint8_t MeisoK230dDrmPollStatus;
enum {
    MEISO_K230D_DRM_POLL_KMS_LATCHED = 0,
    MEISO_K230D_DRM_POLL_WOULD_BLOCK = 1,
    MEISO_K230D_DRM_POLL_INTERRUPTED = 2,
    MEISO_K230D_DRM_POLL_OUTCOME_UNKNOWN = 3,
    MEISO_K230D_DRM_POLL_NOT_PENDING = 4,
    MEISO_K230D_DRM_POLL_INVALID_ARGUMENT = 5
};

typedef uint8_t MeisoK230dDrmCleanupStatus;
enum {
    MEISO_K230D_DRM_CLEANUP_COMPLETE = 0,
    MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING = 1,
    MEISO_K230D_DRM_CLEANUP_WOULD_BLOCK = 2,
    MEISO_K230D_DRM_CLEANUP_INTERRUPTED = 3,
    MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN = 4,
    MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED = 5,
    MEISO_K230D_DRM_CLEANUP_UNCHANGED = 6,
    MEISO_K230D_DRM_CLEANUP_INVALID_ARGUMENT = 7,
    MEISO_K230D_DRM_CLEANUP_OPERATION_FAILED = 8
};

size_t meiso_k230d_drm_scanout_alignment_required(void);
size_t meiso_k230d_drm_scanout_memory_required(void);

MeisoK230dDrmInitStatus meiso_k230d_drm_scanout_init(
    void *arena,
    size_t arena_size,
    const MeisoK230dDrmConfig *config,
    MeisoK230dDrmScanout **out_scanout
);

/*
 * Copies/converts the complete input synchronously; no input borrow remains on
 * return. Success means only that a plane-only real commit was accepted and a
 * matching event is pending. It is not yet KMS_LATCHED.
 */
MeisoK230dDrmBeginStatus meiso_k230d_drm_scanout_begin(
    MeisoK230dDrmScanout *scanout,
    const MeisoK230dDrmFrameView *frame
);

/*
 * One event-loop step. WOULD_BLOCK, INTERRUPTED, and an event carrying an
 * unrelated cookie retain PRESENT_PENDING. Only this adapter's cookie paired
 * with a wrong CRTC/malformed timestamp makes its outcome unknown.
 */
MeisoK230dDrmPollStatus meiso_k230d_drm_scanout_poll_present(
    MeisoK230dDrmScanout *scanout
);

/*
 * Call only after the caller's bounded event deadline elapses while polling
 * kept returning WOULD_BLOCK/INTERRUPTED. This records an unknown real-commit
 * outcome without releasing any possibly scanned resource.
 */
bool meiso_k230d_drm_scanout_declare_present_outcome_unknown(
    MeisoK230dDrmScanout *scanout,
    int error
);

/* Symmetric bounded-deadline escape for a pending restore event. */
bool meiso_k230d_drm_scanout_declare_restore_outcome_unknown(
    MeisoK230dDrmScanout *scanout,
    int error
);

/*
 * One cleanup step. If a real present may be active, the first call submits
 * exact original plane state after TEST_ONLY. Later calls poll its event. Only
 * matching event plus exact ten-property and immutable type/zpos readback
 * permits resource release. PRESENT_PENDING must first be resolved by an
 * event or declare_present_outcome_unknown(); cleanup never silently converts
 * a pending present into an unknown result. A detected mapping-alias contract
 * breach enters TERMINAL_RETAINED with cleanup_callbacks_forbidden=true;
 * cleanup_step never invokes another injected callback in that state.
 */
MeisoK230dDrmCleanupStatus meiso_k230d_drm_scanout_cleanup_step(
    MeisoK230dDrmScanout *scanout
);

/*
 * Copies a diagnostic snapshot synchronously. out_snapshot must not overlap
 * the adapter arena, an owned dumb mapping, or the borrowed Ops context. The
 * function leaves output bytes unchanged when it returns false. As with the
 * map contract, only the context base address is mechanically checkable.
 */
bool meiso_k230d_drm_scanout_snapshot(
    const MeisoK230dDrmScanout *scanout,
    MeisoK230dDrmSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
