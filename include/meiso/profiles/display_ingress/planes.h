#ifndef MEISO_DISPLAY_INGRESS_PLANES_H
#define MEISO_DISPLAY_INGRESS_PLANES_H
#include <meiso/profiles/display_ingress/profile.h>
#include <meiso/profiles/composition/profile.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Application Profile v12: planes in space (planes-v12.md). Display (v2 H.264,
 * a texture source), planes (model/status/evidence/image Blob), optional camera. */
enum {
    MEISO_DISPLAY_PLANES_PROFILE_VERSION = 12,
    MEISO_DISPLAY_PLANES_MAX_ENDPOINTS = 3,
    MEISO_DISPLAY_PLANES_MAX_ROUTES = 11,
    MEISO_DISPLAY_PLANES_ROUTE_MODEL = 4,
    MEISO_DISPLAY_PLANES_ROUTE_STATUS = 5,
    MEISO_DISPLAY_PLANES_ROUTE_EVIDENCE = 6,
    MEISO_DISPLAY_PLANES_ROUTE_IMAGE = 7,
    MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES = 160,
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
enum { MEISO_DISPLAY_PLANES_VIEW_MONO = 1, MEISO_DISPLAY_PLANES_VIEW_SIDE_BY_SIDE = 2 };
typedef uint8_t MeisoDisplayPlanesTracking;
enum { MEISO_DISPLAY_PLANES_TRACKING_NONE = 0, MEISO_DISPLAY_PLANES_TRACKING_ROTATION = 1,
    MEISO_DISPLAY_PLANES_TRACKING_POSE = 2 };
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
/* Model fits this capability and the active enabled display Desired
 * (version and input size; version 0 means no enabled video). Image source
 * rectangles are checked by the Edge after decoding. */
bool meiso_display_planes_model_supported(const MeisoDisplayPlanesCapability *,
    const MeisoDisplayPlanesModel *, uint64_t active_video_desired_version,
    uint32_t video_input_width, uint32_t video_input_height);
/* Number of distinct images the model references (0 on an invalid model). */
uint32_t meiso_display_planes_model_image_count(const MeisoDisplayPlanesModel *);
/* Baseline JPEG header check for the image Blob: SOI, SOF0 8-bit 1 or 3
 * components; reports the frame size. Does not decode entropy data. */
bool meiso_display_planes_jpeg_header(MeisoByteView, uint32_t *width, uint32_t *height);

typedef struct {
    MeisoDisplayIngressCatalogConfig video;   /* v2 H.264 display, 32-byte capability */
    MeisoDisplayPlanesCapability planes;
    bool camera_enabled;
    MeisoCompositionMediaCapability camera;
} MeisoDisplayPlanesCatalogConfig;

bool meiso_display_planes_configure_session(MeisoSessionConfig *);
/* max_image_bytes sizes the Runtime Blob admission (and Edge reassembly). */
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
