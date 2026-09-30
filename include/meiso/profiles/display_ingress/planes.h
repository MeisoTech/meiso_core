#ifndef MEISO_DISPLAY_INGRESS_PLANES_H
#define MEISO_DISPLAY_INGRESS_PLANES_H
#include <meiso/profiles/display_ingress/profile.h>
#include <meiso/profiles/composition/profile.h>
#include <meiso/profiles/display_ingress/tracking.h>
#include <meiso/profiles/display_ingress/input.h>
#include <meiso/profiles/display_ingress/local_pose.h>
#include <meiso/profiles/display_ingress/views.h>
#include <meiso/profiles/display_ingress/audio.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Application Profile v12: planes in space (planes-v12.md). Display (v2 H.264,
 * a texture source), planes (model/status/evidence/image Blob), optional camera,
 * optional Host tracking OR Edge local pose, optional input, optional independent views,
 * optional push-to-talk audio. */
enum {
    MEISO_DISPLAY_PLANES_PROFILE_VERSION = 12,
    MEISO_DISPLAY_PLANES_MAX_ENDPOINTS = 7,
    MEISO_DISPLAY_PLANES_MAX_ROUTES = 21,
    /* Session registry sizes (endpoints + routes) at which each optional endpoint fits: a peer from
     * before §10 negotiates 14 (3 + 11), one from before §11 negotiates 19 (4 + 15), one from before
     * §15 negotiates 24 or less. */
    MEISO_DISPLAY_PLANES_TRACKING_REGISTRY = 4 + 15,
    MEISO_DISPLAY_PLANES_INPUT_REGISTRY = 5 + 18,
    MEISO_DISPLAY_PLANES_EDGE_REGISTRY = 6 + 18,
    MEISO_DISPLAY_PLANES_AUDIO_REGISTRY = 7 + 21,
    MEISO_DISPLAY_PLANES_ROUTE_MODEL = 4,
    MEISO_DISPLAY_PLANES_ROUTE_STATUS = 5,
    MEISO_DISPLAY_PLANES_ROUTE_EVIDENCE = 6,
    MEISO_DISPLAY_PLANES_ROUTE_IMAGE = 7,
    MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES = 368,
    MEISO_DISPLAY_PLANES_CAPABILITY_BYTES = 64,
    MEISO_DISPLAY_PLANES_MAX_PLANES = 16,
    MEISO_DISPLAY_PLANES_MAX_APPS = 8,
    MEISO_DISPLAY_PLANES_MAX_IMAGES = 8,
    MEISO_DISPLAY_PLANES_MAX_IMAGE_BYTES = 1048576,
    MEISO_DISPLAY_PLANES_MAX_NAME = 32,
    MEISO_DISPLAY_PLANES_HEADER_BYTES = 32,
    MEISO_DISPLAY_PLANES_APP_BYTES = 40,
    MEISO_DISPLAY_PLANES_PLANE_BYTES = 96,
    MEISO_DISPLAY_PLANES_MAX_MODEL_BYTES = 32 + 8 * 40 + 16 * 96,
    MEISO_DISPLAY_PLANES_STATUS_BYTES = 16,
    MEISO_DISPLAY_PLANES_EVIDENCE_BYTES = 64,
    MEISO_DISPLAY_PLANES_NO_APP = 0xff
};
typedef uint8_t MeisoDisplayPlanesViewLayout;
enum { MEISO_DISPLAY_PLANES_VIEW_MONO = 1, MEISO_DISPLAY_PLANES_VIEW_SIDE_BY_SIDE = 2,
    MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS = 3 };
typedef uint8_t MeisoDisplayPlanesTracking;
enum { MEISO_DISPLAY_PLANES_TRACKING_NONE = 0, MEISO_DISPLAY_PLANES_TRACKING_ROTATION = 1,
    MEISO_DISPLAY_PLANES_TRACKING_POSE = 2, MEISO_DISPLAY_PLANES_TRACKING_EDGE = 3 };
typedef uint8_t MeisoDisplayPlanesAnchor;
enum { MEISO_DISPLAY_PLANES_ANCHOR_HEAD = 1, MEISO_DISPLAY_PLANES_ANCHOR_WORLD = 2 };
typedef uint8_t MeisoDisplayPlanesSource;
enum { MEISO_DISPLAY_PLANES_SOURCE_STREAM = 1, MEISO_DISPLAY_PLANES_SOURCE_IMAGE = 2 };

typedef struct {
    uint32_t output_width, output_height;
    MeisoDisplayPlanesViewLayout view_layout;
    MeisoDisplayPlanesTracking tracking;
    uint8_t max_planes, max_images;
    uint32_t max_image_bytes;
    uint16_t max_image_width, max_image_height;
    float fov_left, fov_right, fov_up, fov_down; /* radians, per view */
    float ipd_m;
    uint16_t max_updates_per_second;
} MeisoDisplayPlanesCapability;

typedef struct {
    uint32_t app_id;
    uint8_t name_size;
    char name[MEISO_DISPLAY_PLANES_MAX_NAME];
} MeisoDisplayPlanesApp;

typedef struct {
    uint32_t plane_id;
    uint8_t app; /* APP table index or MEISO_DISPLAY_PLANES_NO_APP */
    MeisoDisplayPlanesAnchor anchor;
    MeisoDisplayPlanesSource source;
    uint8_t opacity;
    uint32_t source_x, source_y, source_width, source_height;
    uint8_t image_sha256[32]; /* IMAGE only; zero for STREAM */
    float position[3];        /* metres; x right, y up, looking down -z */
    float orientation[4];     /* unit quaternion x, y, z, w */
    float width_m, height_m;
} MeisoDisplayPlane;

typedef struct {
    uint64_t generation, video_desired_version;
    uint8_t plane_count, app_count, focused_app;
    MeisoDisplayPlanesApp apps[MEISO_DISPLAY_PLANES_MAX_APPS];
    MeisoDisplayPlane planes[MEISO_DISPLAY_PLANES_MAX_PLANES];
    uint32_t world_epoch; /* nonzero for EDGE tracking, zero otherwise */
} MeisoDisplayPlanesModel;

typedef struct {
    uint64_t generation;
    uint8_t drawn, waiting;
} MeisoDisplayPlanesStatus;

typedef struct {
    MeisoDisplayIngressEvidenceClass evidence_class;
    uint8_t drawn, waiting;
    uint64_t generation, desired_version;
    uint8_t model_sha256[32];
    uint64_t predicted_display_time_ns; /* nonzero only for XR_FRAME_SUBMITTED */
} MeisoDisplayPlanesEvidence;

/* Codecs: inputs and outputs must be disjoint; failure leaves outputs unchanged. */
bool meiso_display_planes_capability_encode(const MeisoDisplayPlanesCapability *, MeisoMutableBuffer);
bool meiso_display_planes_capability_decode(MeisoByteView, MeisoDisplayPlanesCapability *);
/* Canonical model size, or 0 when the model is not self-consistent. */
size_t meiso_display_planes_model_bytes(const MeisoDisplayPlanesModel *);
bool meiso_display_planes_model_encode(const MeisoDisplayPlanesModel *, MeisoMutableBuffer);
bool meiso_display_planes_model_decode(MeisoByteView, MeisoDisplayPlanesModel *);
bool meiso_display_planes_status_encode(const MeisoDisplayPlanesStatus *, MeisoMutableBuffer);
bool meiso_display_planes_status_decode(MeisoByteView, MeisoDisplayPlanesStatus *);
bool meiso_display_planes_evidence_encode(const MeisoDisplayPlanesEvidence *, MeisoMutableBuffer);
bool meiso_display_planes_evidence_decode(MeisoByteView, MeisoDisplayPlanesEvidence *);
/* Valid evidence whose class is declared by this Session's video capability. */
bool meiso_display_planes_evidence_supported(const MeisoDisplayIngressCapability *, const MeisoDisplayPlanesEvidence *);
/* Model fits this capability and the active enabled display Desired
 * (version and input size; version 0 means no enabled video). Image source
 * rectangles are checked by the Edge after decoding. */
bool meiso_display_planes_model_supported(const MeisoDisplayPlanesCapability *,
    const MeisoDisplayPlanesModel *, uint64_t active_video_desired_version,
    uint32_t video_input_width, uint32_t video_input_height);
/* Number of distinct images the model references (0 on an invalid model). */
uint32_t meiso_display_planes_model_image_count(const MeisoDisplayPlanesModel *);
/* SHA-256 used by v12 evidence: the canonical model bytes (model_sha256) and a
 * displayed H.264 access unit (the v2 PRES_EVT digest). False on bad arguments. */
bool meiso_display_planes_sha256(MeisoByteView bytes, uint8_t out[32]);
/* Baseline JPEG header check for the image Blob: SOI, SOF0 8-bit 1 or 3
 * components; reports the frame size. Does not decode entropy data. */
bool meiso_display_planes_jpeg_header(MeisoByteView, uint32_t *width, uint32_t *height);

typedef struct {
    MeisoDisplayIngressCatalogConfig video;   /* v2 H.264 display, 32-byte capability */
    MeisoDisplayPlanesCapability planes;
    bool camera_enabled;
    MeisoCompositionMediaCapability camera;
    /* Host tracking: POSE; Edge local pose: EDGE; neither: NONE. */
    bool tracking_enabled;
    MeisoDisplayTrackingCapability tracking;
    bool input_enabled;
    MeisoDisplayInputCapability input;
    bool local_pose_enabled; /* mutually exclusive with tracking_enabled */
    MeisoDisplayLocalPoseCapability local_pose;
    bool views_enabled; /* iff view_layout == RUNTIME_VIEWS */
    MeisoDisplayViewsCapability views;
    bool audio_enabled; /* needs input with the SYSTEM key */
    MeisoDisplayAudioCapability audio;
} MeisoDisplayPlanesCatalogConfig;

/* Tracking routes, in catalog order after the camera's: Desired, Actual, IMU, pose. */
enum {
    MEISO_DISPLAY_TRACKING_ROUTE_DESIRED = 0,
    MEISO_DISPLAY_TRACKING_ROUTE_ACTUAL = 1,
    MEISO_DISPLAY_TRACKING_ROUTE_IMU = 2,
    MEISO_DISPLAY_TRACKING_ROUTE_POSE = 3
};
/* Index of the first tracking route in a catalog built from this configuration. */
size_t meiso_display_planes_tracking_route_base(const MeisoDisplayPlanesCatalogConfig *);
/* Local pose occupies the same position as Host tracking (three routes). */
size_t meiso_display_planes_local_pose_route_base(const MeisoDisplayPlanesCatalogConfig *);
/* Input routes, in catalog order after tracking's: Desired, Actual, key events. */
enum {
    MEISO_DISPLAY_INPUT_ROUTE_DESIRED = 0,
    MEISO_DISPLAY_INPUT_ROUTE_ACTUAL = 1,
    MEISO_DISPLAY_INPUT_ROUTE_KEY = 2
};
size_t meiso_display_planes_input_route_base(const MeisoDisplayPlanesCatalogConfig *);
/* Audio routes, last in the catalog: Desired, Actual, records. */
enum {
    MEISO_DISPLAY_AUDIO_ROUTE_DESIRED = 0,
    MEISO_DISPLAY_AUDIO_ROUTE_ACTUAL = 1,
    MEISO_DISPLAY_AUDIO_ROUTE_RECORD = 2
};
size_t meiso_display_planes_audio_route_base(const MeisoDisplayPlanesCatalogConfig *);

bool meiso_display_planes_configure_session(MeisoSessionConfig *);
/* A READY Session whose peer can hold the tracking endpoint (planes-v12.md §10). A peer from before
 * tracking negotiates room for the camera only (3 endpoints, 11 routes): the Edge then leaves
 * tracking out of the catalog. */
bool meiso_display_planes_session_allows_tracking(const MeisoSession *);
/* A READY Session whose peer can hold the input endpoint too (planes-v12.md §11): one from before it
 * negotiates 19, and the Edge then leaves input out. */
bool meiso_display_planes_session_allows_input(const MeisoSession *);
/* The caller must require this before offering/accepting any section 14
 * extension (local pose, runtime views or XR evidence). No fake fallback. */
bool meiso_display_planes_session_allows_edge_extensions(const MeisoSession *);
/* A READY Session whose peer can hold the audio endpoint too (planes-v12.md §15): one from before it
 * negotiates 24 or less, and the Edge then leaves audio out (keys still work, without voice). */
bool meiso_display_planes_session_allows_audio(const MeisoSession *);
/* max_image_bytes sizes the Runtime Blob admission (and Edge reassembly). The registry is sized to
 * what the Session allows: registry 28/24/23/19/14 retain distinct capacities. */
bool meiso_display_planes_configure_runtime(MeisoRuntimeV4Config *, MeisoSession *, uint32_t max_image_bytes);
bool meiso_display_planes_build_catalog(const MeisoDisplayPlanesCatalogConfig *, MeisoMutableBuffer capability_storage,
    MeisoRuntimeEndpointDescriptor *, size_t, MeisoRuntimeV4RouteDescriptor *, size_t);
bool meiso_display_planes_validate_catalog(const MeisoRuntimeEndpointDescriptor *, size_t,
    const MeisoRuntimeV4RouteDescriptor *, size_t, MeisoDisplayPlanesCatalogConfig *);
const MeisoEndpointId *meiso_display_planes_endpoint(void);
#ifdef __cplusplus
}
#endif
#endif
