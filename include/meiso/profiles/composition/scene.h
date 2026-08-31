#ifndef MEISO_PROFILES_COMPOSITION_SCENE_H
#define MEISO_PROFILES_COMPOSITION_SCENE_H

#include <meiso/runtime/runtime.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT 2U
#define MEISO_COMPOSITION_SCENE_RESOURCE_COUNT 6U

#define MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE 32U
#define MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE 52U

#define MEISO_COMPOSITION_SCENE_MAX_OBJECTS 32U
#define MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES 8U
#define MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE (256U * 1024U)
#define MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS 16U
#define MEISO_COMPOSITION_SCENE_MAX_STAGING_TRANSFERS 1U
#define MEISO_COMPOSITION_SCENE_BLOB_CHUNK_PAYLOAD_SIZE 4096U
#define MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_PER_WORLD \
    (2U * 1024U * 1024U)
#define MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL \
    (4U * 1024U * 1024U)

#define MEISO_COMPOSITION_SCENE_TARGET_WIDTH 320U
#define MEISO_COMPOSITION_SCENE_TARGET_HEIGHT 240U
#define MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT 2U
#define MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS 2U
#define MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS 4U
/* PCAP upper bound; PresentationPlan selects an integer rate in [1, 10]. */
#define MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ 10U
#define MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS 5000U

#define MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH 64U
#define MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT 48U
#define MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTES_PER_PIXEL 2U
#define MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE                       \
    (MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH *                           \
     MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT *                          \
     MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTES_PER_PIXEL)

#define MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE 64U
#define MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE 12U
#define MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE 16U
#define MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE 128U
#define MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE 160U
#define MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE 112U
#define MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE 112U
#define MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE                       \
    (MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +                     \
     MEISO_COMPOSITION_SCENE_MAX_OBJECTS *                                \
         MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE)
#define MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE 112U
#define MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE 160U
#define MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE                   \
    (MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE +           \
     MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE)

#define MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE            \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +                            \
     MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE)
#define MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE             \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE +                             \
     MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE)
#define MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +                              \
     MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE)
#define MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE +                               \
     MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE)

typedef uint8_t MeisoCompositionSceneCodecStatus;
enum {
    MEISO_COMPOSITION_SCENE_CODEC_OK = 0,
    MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE = 2,
    MEISO_COMPOSITION_SCENE_CODEC_CAPACITY = 3,
    MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED = 4,
    MEISO_COMPOSITION_SCENE_CODEC_MALFORMED = 5
};

typedef uint8_t MeisoCompositionSceneResource;
enum {
    MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_DESIRED = 0,
    MEISO_COMPOSITION_SCENE_RESOURCE_PRESENTATION_ACTUAL = 1,
    MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_DESIRED = 2,
    MEISO_COMPOSITION_SCENE_RESOURCE_SCENE_ACTUAL = 3,
    MEISO_COMPOSITION_SCENE_RESOURCE_MESH_BLOB = 4,
    MEISO_COMPOSITION_SCENE_RESOURCE_RENDER_RESULT = 5
};

typedef uint8_t MeisoCompositionSceneSpace;
enum {
    MEISO_COMPOSITION_SCENE_SPACE_WORLD = 1
};

typedef uint8_t MeisoCompositionSceneLayerSource;
enum {
    MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE = 1
};

typedef uint8_t MeisoCompositionSceneGeometry;
enum {
    MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION = 1
};

typedef uint8_t MeisoCompositionSceneProjection;
enum {
    MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE = 1
};

typedef uint16_t MeisoCompositionSceneColorFormat;
enum {
    MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 = 1,
    MEISO_COMPOSITION_SCENE_COLOR_RGB565 = 2
};

typedef uint16_t MeisoCompositionSceneDepthFormat;
enum {
    MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 = 1
};

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoCompositionSceneObjectId;

typedef uint16_t MeisoCompositionScenePlanFlags;
enum {
    MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME = UINT16_C(1) << 0U
};

typedef struct {
    MeisoCompositionScenePlanFlags flags;
    uint64_t plan_generation;
    /* Runtime catalog_generation carrying this endpoint capability. */
    uint64_t capability_generation;
    /*
     * SHA-256 of canonical 128-byte Plan with this digest and the token
     * presence/token bytes zeroed. Token echo therefore cannot change it.
     */
    uint8_t candidate_digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoEndpointId scene_binding;
    MeisoCompositionSceneLayerSource source;
    MeisoCompositionSceneGeometry geometry;
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
    /* Requested result cadence, from 1 through the PCAP upper bound. */
    uint16_t max_result_hz;
    uint32_t offline_lease_ms;
    bool prepared_token_echo_present;
    uint8_t prepared_token_echo[MEISO_SHA256_DIGEST_SIZE];
} MeisoCompositionScenePresentationPlan;

typedef uint8_t MeisoCompositionSceneCandidateState;
enum {
    MEISO_COMPOSITION_SCENE_CANDIDATE_NONE = 1,
    MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARING = 2,
    MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED = 3,
    MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING = 4,
    MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED = 5,
    MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED = 6
};

typedef uint8_t MeisoCompositionSceneActiveState;
enum {
    MEISO_COMPOSITION_SCENE_ACTIVE_NONE = 1,
    MEISO_COMPOSITION_SCENE_ACTIVE_ACTIVE = 2,
    MEISO_COMPOSITION_SCENE_ACTIVE_RETIRING = 3,
    MEISO_COMPOSITION_SCENE_ACTIVE_SAFETY = 4
};

typedef uint16_t MeisoCompositionScenePresentationReason;
enum {
    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE = 0,
    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_WAITING_SCENE = 1,
    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_CAPACITY = 2,
    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_INVALID_PLAN = 3,
    MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED = 4
};

typedef struct {
    /* Runtime catalog_generation carrying this endpoint capability. */
    uint64_t capability_generation;
    MeisoCompositionSceneCandidateState candidate_state;
    uint64_t candidate_plan_generation;
    uint8_t candidate_digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoCompositionSceneActiveState active_state;
    uint64_t active_plan_generation;
    uint8_t active_digest[MEISO_SHA256_DIGEST_SIZE];
    MeisoCompositionScenePresentationReason reason;
    bool prepared_token_present;
    uint8_t prepared_token[MEISO_SHA256_DIGEST_SIZE];
    MeisoEndpointId scene_binding;
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
} MeisoCompositionScenePresentationStatus;

typedef struct {
    float position[3];
    float orientation_xyzw[4];
    float scale[3];
} MeisoCompositionSceneTransform;

typedef struct {
    MeisoCompositionSceneSpace space;
    uint64_t space_epoch;
    MeisoTimePoint source_time;
    float position[3];
    float orientation_xyzw[4];
    MeisoCompositionSceneProjection projection;
    float vertical_fov_degrees;
    float near_z;
    float far_z;
    uint16_t viewport_x;
    uint16_t viewport_y;
    uint16_t viewport_width;
    uint16_t viewport_height;
} MeisoCompositionSceneCamera;

typedef uint8_t MeisoCompositionSceneObjectFlags;
enum {
    MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE = UINT8_C(1) << 0U
};

typedef struct {
    MeisoCompositionSceneObjectId id;
    MeisoContentId mesh_content_id;
    MeisoCompositionSceneSpace space;
    uint64_t space_epoch;
    MeisoCompositionSceneTransform transform;
    MeisoCompositionSceneObjectFlags flags;
} MeisoCompositionSceneObject;

typedef uint16_t MeisoCompositionSceneFlags;
enum {
    MEISO_COMPOSITION_SCENE_FLAGS_NONE = 0
};

/* Profile suffix only; Runtime owns the canonical desired State prefix. */
typedef struct {
    MeisoCompositionSceneFlags flags;
    MeisoCompositionSceneCamera camera;
    uint32_t object_count;
    MeisoCompositionSceneObject objects[MEISO_COMPOSITION_SCENE_MAX_OBJECTS];
} MeisoCompositionSceneDesired;

typedef uint16_t MeisoCompositionSceneStableReason;
enum {
    MEISO_COMPOSITION_SCENE_REASON_NONE = 0,
    MEISO_COMPOSITION_SCENE_REASON_READY = 1,
    MEISO_COMPOSITION_SCENE_REASON_WAITING_BLOB = 2,
    MEISO_COMPOSITION_SCENE_REASON_OBJECT_CAPACITY = 3,
    MEISO_COMPOSITION_SCENE_REASON_BLOB_CAPACITY = 4,
    MEISO_COMPOSITION_SCENE_REASON_MESH_BYTES_CAPACITY = 5,
    MEISO_COMPOSITION_SCENE_REASON_BUFFER_CAPACITY = 6,
    MEISO_COMPOSITION_SCENE_REASON_INVALID_SCENE = 7,
    MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED = 8
};

/* Profile suffix only; Runtime owns the canonical actual State prefix. */
typedef struct {
    MeisoCompositionSceneStableReason reason;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    uint32_t active_object_count;
    uint32_t unique_mesh_count;
    uint32_t published_blob_count;
    uint64_t published_blob_bytes;
    uint64_t resident_mesh_bytes;
    uint32_t target_slots_used;
    uint64_t target_bytes;
    uint32_t render_jobs_in_flight;
    uint32_t result_queue_depth;
    uint64_t last_result_stream_epoch;
    uint64_t last_result_sequence;
} MeisoCompositionSceneStatus;

typedef struct {
    float position[3];
    uint8_t color_rgba[4];
} MeisoCompositionSceneMeshVertex;

typedef struct {
    bool has_vertex_color;
    uint32_t vertex_count;
    const MeisoCompositionSceneMeshVertex *vertices;
    uint32_t index_count;
    const uint32_t *indices;
} MeisoCompositionSceneMesh;

/* A validated, borrowed view. Accessors decode BE binary32/u32 records. */
typedef struct {
    bool has_vertex_color;
    uint32_t vertex_count;
    uint32_t index_count;
    float bounds_min[3];
    float bounds_max[3];
    MeisoByteView vertex_records;
    MeisoByteView index_records;
} MeisoCompositionSceneMeshView;

typedef uint16_t MeisoCompositionSceneResultCode;
enum {
    MEISO_COMPOSITION_SCENE_RESULT_COMPLETE = 1
};

typedef uint32_t MeisoCompositionSceneEvidenceFlags;
enum {
    MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE = UINT32_C(1) << 0U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED = UINT32_C(1) << 1U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_FENCE_COMPLETE = UINT32_C(1) << 2U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED =
        UINT32_C(1) << 3U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_VBLANK_OBSERVED = UINT32_C(1) << 4U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_PHYSICAL_VERIFIED = UINT32_C(1) << 5U,
    MEISO_COMPOSITION_SCENE_EVIDENCE_ALL = UINT32_C(0x0000003f)
};

typedef struct {
    uint64_t applied_scene_owner_incarnation;
    uint64_t applied_scene_state_version;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    MeisoCompositionSceneResultCode result_code;
    MeisoCompositionSceneEvidenceFlags evidence_flags;
    uint32_t object_count;
    uint32_t unique_mesh_count;
    uint64_t source_clock_epoch;
    uint64_t render_start_monotonic_ns;
    uint64_t render_end_monotonic_ns;
    /* SHA-256 of visible 320x240 row-major RGBA8888 bytes, no padding/depth. */
    uint8_t full_target_sha256[MEISO_SHA256_DIGEST_SIZE];
    uint16_t target_width;
    uint16_t target_height;
    MeisoCompositionSceneColorFormat color_format;
    MeisoCompositionSceneDepthFormat depth_format;
    /* Canonical logical row bytes; private DMA/physical stride is excluded. */
    uint32_t logical_color_row_bytes;
    uint32_t logical_depth_row_bytes;
    uint16_t thumbnail_width;
    uint16_t thumbnail_height;
    MeisoCompositionSceneColorFormat thumbnail_format;
    uint32_t thumbnail_stride_bytes;
    uint32_t thumbnail_byte_size;
    uint8_t thumbnail_rgb565[MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE];
} MeisoCompositionSceneRenderResult;

const MeisoEndpointId *meiso_composition_scene_presentation_endpoint(void);
const MeisoEndpointId *meiso_composition_scene_scene_endpoint(void);
const MeisoSchemaRef *
meiso_composition_scene_presentation_capability_schema(void);
const MeisoSchemaRef *meiso_composition_scene_capability_schema(void);
MeisoByteView meiso_composition_scene_presentation_capability(void);
MeisoByteView meiso_composition_scene_capability(void);
const MeisoResourceDescriptor *meiso_composition_scene_resources(
    size_t *out_count
);
const MeisoResourceDescriptor *meiso_composition_scene_resource(
    MeisoCompositionSceneResource resource
);

/*
 * Builds only the two scene endpoints and six routes. The current Composition
 * Profile adds Wi-Fi, Device, render-media and camera-media endpoints to form
 * its six-endpoint/eighteen-route catalog. The Blob descriptor's
 * max_payload_size is total content size; Runtime wire v2 cannot route
 * IMMUTABLE/Blob and must not be given that route.
 */
MeisoCompositionSceneCodecStatus meiso_composition_scene_build_endpoints(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeEndpointDescriptor *output,
    size_t output_capacity,
    size_t *out_count
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_build_routes(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeRouteDescriptor *output,
    size_t output_capacity,
    size_t *out_count
);
bool meiso_composition_scene_catalog_identifiers_disjoint(
    const MeisoRuntimeEndpointDescriptor *external_endpoints,
    size_t external_endpoint_count,
    const MeisoRuntimeRouteDescriptor *external_routes,
    size_t external_route_count
);

size_t meiso_composition_scene_presentation_plan_encoded_size(void);
MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_encode(
    const MeisoCompositionScenePresentationPlan *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationPlan *out_value
);
/*
 * Computes SHA-256 over canonical PLN0 with candidate_digest, token presence,
 * and token bytes zeroed. It validates every other Plan field but does not
 * require the input candidate_digest to be populated.
 */
bool meiso_composition_scene_plan_digest(
    const MeisoCompositionScenePresentationPlan *plan,
    uint8_t out_digest[MEISO_SHA256_DIGEST_SIZE]
);
size_t meiso_composition_scene_presentation_status_encoded_size(void);
MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_encode(
    const MeisoCompositionScenePresentationStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationStatus *out_value
);

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_encoded_size(
    uint32_t vertex_count,
    bool has_vertex_color,
    uint32_t index_count,
    size_t *out_size
);
size_t meiso_composition_scene_mesh_max_encoded_size(void);
MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_encode(
    const MeisoCompositionSceneMesh *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_decode(
    MeisoByteView input,
    MeisoCompositionSceneMeshView *out_value
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_view_vertex(
    const MeisoCompositionSceneMeshView *view,
    uint32_t index,
    MeisoCompositionSceneMeshVertex *out_vertex
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_view_index(
    const MeisoCompositionSceneMeshView *view,
    uint32_t index,
    uint32_t *out_vertex_index
);
const MeisoCompositionSceneMesh *meiso_composition_scene_canonical_cube(void);
/* Suitable directly as MeisoBlobVerifier for canonical Mesh Blob commits. */
bool meiso_composition_scene_mesh_blob_verify(
    const MeisoContentId *expected_content_id,
    MeisoByteView staged_content,
    void *context
);
/* out_content_id must not overlap the immutable content bytes. */
bool meiso_composition_scene_mesh_content_id(
    MeisoByteView content,
    MeisoContentId *out_content_id
);

MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_encoded_size(
    uint32_t object_count,
    size_t *out_size
);
size_t meiso_composition_scene_desired_max_encoded_size(void);
MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_encode(
    const MeisoCompositionSceneDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_decode(
    MeisoByteView input,
    MeisoCompositionSceneDesired *out_value
);

size_t meiso_composition_scene_status_encoded_size(void);
MeisoCompositionSceneCodecStatus meiso_composition_scene_status_encode(
    const MeisoCompositionSceneStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_status_decode(
    MeisoByteView input,
    MeisoCompositionSceneStatus *out_value
);

size_t meiso_composition_scene_render_result_encoded_size(void);
MeisoCompositionSceneCodecStatus meiso_composition_scene_render_result_encode(
    const MeisoCompositionSceneRenderResult *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionSceneCodecStatus meiso_composition_scene_render_result_decode(
    MeisoByteView input,
    MeisoCompositionSceneRenderResult *out_value
);

#ifdef __cplusplus
}
#endif

#endif
