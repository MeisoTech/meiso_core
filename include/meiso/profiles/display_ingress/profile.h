#ifndef MEISO_PROFILES_DISPLAY_INGRESS_PROFILE_H
#define MEISO_PROFILES_DISPLAY_INGRESS_PROFILE_H

#include <meiso/runtime/runtime.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_DISPLAY_INGRESS_PROFILE_VERSION 1U
#define MEISO_DISPLAY_INGRESS_H264_PROFILE_VERSION 2U
#define MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_WIRE_VERSION 4U
#define MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT 1U
#define MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT 4U
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_REGISTRY_ENTRIES 5U
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_ENDPOINT_HISTORY 2U
#define MEISO_DISPLAY_INGRESS_PROFILE_CAPABILITY_WIRE_SIZE 32U
#define MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE 12U
#define MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE 32U
#define MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE 64U
#define MEISO_DISPLAY_INGRESS_XR_EVIDENCE_WIRE_SIZE 72U
#define MEISO_DISPLAY_INGRESS_MAX_EVIDENCE_WIRE_SIZE 72U
#define MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_WIRE_SIZE 428U
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_DIMENSION 4096U
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_RATE_MILLIHZ 240000U
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_FRAME_BYTES UINT32_C(67108864)
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES             \
    (MEISO_RUNTIME_V3_BLOB_CHUNK_PREFIX_WIRE_SIZE +                         \
     MEISO_RUNTIME_V3_BLOB_CHUNK_PAYLOAD_SIZE)
#define MEISO_DISPLAY_INGRESS_PROFILE_MAX_SESSION_FRAME_BYTES               \
    (MEISO_SESSION_WIRE_HEADER_SIZE + MEISO_SESSION_ID_WIRE_SIZE +          \
     MEISO_DISPLAY_INGRESS_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES)
#define MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE        \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE +                              \
     MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE)
#define MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE         \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE +                               \
     MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE)

typedef uint8_t MeisoDisplayIngressProfileRoute;
enum {
    MEISO_DISPLAY_INGRESS_ROUTE_DESIRED = 0,
    MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL = 1,
    MEISO_DISPLAY_INGRESS_ROUTE_MEDIA = 2,
    MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE = 3
};

typedef uint8_t MeisoDisplayIngressEvidenceClass;
enum {
    MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED = 1,
    MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP = 2,
    MEISO_DISPLAY_INGRESS_EVIDENCE_XR_FRAME_SUBMITTED = 3
};

typedef uint32_t MeisoDisplayIngressEvidenceClassMask;
enum {
    MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_SIMULATED = UINT32_C(1) << 0U,
    MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP =
        UINT32_C(1) << 1U,
    MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_ALL = UINT32_C(0x00000003),
    /* Legacy masks retain their meaning. XR is exclusive and v12-only. */
    MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_XR_FRAME_SUBMITTED = UINT32_C(1) << 2U
};

typedef uint8_t MeisoDisplayIngressConnectorKind;
enum {
    MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED = 0,
    MEISO_DISPLAY_INGRESS_CONNECTOR_HDMI = 1,
    MEISO_DISPLAY_INGRESS_CONNECTOR_DSI = 2
};

typedef uint8_t MeisoDisplayIngressFrameFormat;
enum {
    MEISO_DISPLAY_INGRESS_FRAME_RGBA8888 = 0,
    MEISO_DISPLAY_INGRESS_FRAME_H264_IDR = 1
};

/* Catalog-embedded immutable truth for one accepted catalog generation. */
typedef struct {
    uint32_t max_input_width;
    uint32_t max_input_height;
    uint32_t max_frame_rate_millihz;
    uint32_t max_frame_bytes;
    uint32_t max_fragment_payload_bytes;
    MeisoDisplayIngressEvidenceClassMask evidence_classes;
    MeisoStreamDeadlinePolicy deadline_policy;
    MeisoDisplayIngressFrameFormat frame_format;
} MeisoDisplayIngressCapability;

/* Profile suffix of the Host-owned Runtime Desired State. */
typedef struct {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t maximum_frame_rate_millihz;
} MeisoDisplayIngressDesired;

/* Immutable Profile suffix while one Runtime production epoch is active. */
typedef struct {
    uint32_t effective_input_width;
    uint32_t effective_input_height;
    uint32_t effective_maximum_frame_rate_millihz;
    MeisoSessionNonce channel_nonce;
} MeisoDisplayIngressActual;

typedef struct {
    bool data_active;
    MeisoDisplayIngressActual effective;
} MeisoDisplayIngressActualStateView;

/* source_time is the presentation time for classes 1/2; for class 3 it is
 * the accepted submission time (physical=false), on Edge clock epoch 4. */
typedef struct {
    MeisoDisplayIngressEvidenceClass evidence_class;
    MeisoDisplayIngressConnectorKind connector_kind;
    uint32_t output_width;
    uint32_t output_height;
    uint64_t media_stream_epoch;
    uint64_t displayed_frame_sequence;
    union {
        uint8_t input_frame_sha256[32U];
        uint8_t input_rgba_sha256[32U]; /* RAW v1 source compatibility. */
    };
    uint64_t predicted_display_time_ns; /* class 3 only, nonzero; legacy zero */
} MeisoDisplayIngressPresentationEvidence;

typedef struct {
    uint64_t endpoint_incarnation;
    MeisoDisplayIngressCapability capability;
} MeisoDisplayIngressCatalogConfig;

/* Runtime supplies production identity; Profile Actual supplies the nonce. */
typedef struct {
    MeisoRuntimeV4ExternalMediaBinding runtime;
    uint64_t media_stream_epoch;
    MeisoSessionNonce channel_nonce;
    MeisoOwnerScope producer;
} MeisoDisplayIngressMediaBinding;

typedef uint8_t MeisoDisplayIngressProfileStatus;
enum {
    MEISO_DISPLAY_INGRESS_PROFILE_OK = 0,
    MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT = 1,
    MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE = 2,
    MEISO_DISPLAY_INGRESS_PROFILE_CAPACITY = 3,
    MEISO_DISPLAY_INGRESS_PROFILE_TRUNCATED = 4,
    MEISO_DISPLAY_INGRESS_PROFILE_MALFORMED = 5,
    MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH = 6,
    MEISO_DISPLAY_INGRESS_PROFILE_RUNTIME_NOT_ACTIVE = 7
};

const MeisoProfileRef *meiso_display_ingress_profile(void);
/* Format is fixed by the selected Session/Profile and catalog, not per packet. */
const MeisoProfileRef *meiso_display_ingress_profile_for_format(
    MeisoDisplayIngressFrameFormat format);
const MeisoSchemaRef *meiso_display_ingress_capability_schema_for_format(
    MeisoDisplayIngressFrameFormat format);
const MeisoSchemaRef *meiso_display_ingress_resource_schema_for_format(
    MeisoDisplayIngressProfileRoute route, MeisoDisplayIngressFrameFormat format);
MeisoDisplayIngressProfileStatus meiso_display_ingress_configure_session_for_format(
    MeisoSessionConfig *in_out_config, MeisoDisplayIngressFrameFormat format);
/* Structural independent-AU check; decoder validates full syntax and dimensions. */
bool meiso_display_ingress_h264_idr_access_unit_valid(MeisoByteView unit);

const MeisoEndpointId *meiso_display_ingress_endpoint(void);
const MeisoSchemaRef *meiso_display_ingress_capability_schema(void);
const MeisoResourceId *meiso_display_ingress_resource_id(
    MeisoDisplayIngressProfileRoute route
);
const MeisoSchemaRef *meiso_display_ingress_resource_schema(
    MeisoDisplayIngressProfileRoute route
);

MeisoDisplayIngressProfileStatus meiso_display_ingress_configure_session(
    MeisoSessionConfig *in_out_config
);
/* The Session must already be READY; failure leaves out_config unchanged. */
MeisoDisplayIngressProfileStatus
meiso_display_ingress_configure_runtime_v4(
    MeisoRuntimeV4Config *out_config,
    MeisoSession *session
);

/*
 * Outputs are caller-owned and mutually disjoint. Capability storage is
 * borrowed by the endpoint until Runtime copies it. Failure leaves all
 * output bytes unchanged.
 */
MeisoDisplayIngressProfileStatus meiso_display_ingress_build_catalog(
    const MeisoDisplayIngressCatalogConfig *config,
    MeisoMutableBuffer capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_capacity
);

/* Validates the exact ordered one-endpoint/four-route Profile graph. */
MeisoDisplayIngressProfileStatus meiso_display_ingress_validate_catalog(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoRuntimeV4RouteDescriptor *routes,
    size_t route_count,
    MeisoDisplayIngressCapability *out_capability
);

bool meiso_display_ingress_desired_supported(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressDesired *desired
);
bool meiso_display_ingress_actual_matches_desired(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressDesired *desired,
    const MeisoDisplayIngressActual *actual
);
bool meiso_display_ingress_evidence_supported(
    const MeisoDisplayIngressCapability *capability,
    const MeisoDisplayIngressPresentationEvidence *evidence,
    uint64_t containing_production_epoch
);

/*
 * Every codec requires typed input, wire input/output and out_size to be
 * mutually disjoint. Alias is INVALID_ARGUMENT with no mutation. Other
 * encode failures set a disjoint out_size to zero and leave output unchanged;
 * decode failures leave their typed output unchanged.
 */
MeisoDisplayIngressProfileStatus meiso_display_ingress_capability_encode(
    const MeisoDisplayIngressCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_capability_decode(
    MeisoByteView input,
    MeisoDisplayIngressCapability *out_value
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_desired_encode(
    const MeisoDisplayIngressDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_desired_decode(
    MeisoByteView input,
    MeisoDisplayIngressDesired *out_value
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_actual_encode(
    const MeisoDisplayIngressActual *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_actual_decode(
    MeisoByteView input,
    MeisoDisplayIngressActual *out_value
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_evidence_encode(
    const MeisoDisplayIngressPresentationEvidence *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoDisplayIngressProfileStatus meiso_display_ingress_evidence_decode(
    MeisoByteView input,
    MeisoDisplayIngressPresentationEvidence *out_value
);

/*
 * Must be called before committing a received Runtime Actual. Runtime treats
 * profile bytes as opaque. Display Ingress v1 accepts only RUNNING as active;
 * it needs an exact 32-byte suffix matching an enabled Runtime Desired State
 * resolved from state.applied_desired. STOPPED, STARTING and FAILED need an
 * empty suffix. DEGRADED/STOPPING are rejected by this narrow Profile.
 * Failure leaves out_view unchanged.
 */
MeisoDisplayIngressProfileStatus
meiso_display_ingress_validate_actual_state(
    const MeisoDisplayIngressCapability *capability,
    const MeisoRuntimeDesiredState *applied_desired_state,
    const MeisoRuntimeActualState *state,
    MeisoDisplayIngressActualStateView *out_view
);

/*
 * Pure Runtime query. Success requires the exact active media route and a
 * locally committed RUNNING Actual whose 32-byte suffix matches the supplied
 * capability and enabled Runtime Desired State resolved from the snapshot's
 * applied_desired reference. The caller owns the proof that out_binding is
 * disjoint from the Runtime and both inputs. v1 derives media_stream_epoch
 * from production_epoch and requires both to be equal. Failure leaves a
 * disjoint out_binding unchanged.
 */
MeisoDisplayIngressProfileStatus
meiso_display_ingress_media_binding_from_runtime(
    const MeisoRuntime *runtime,
    const MeisoDisplayIngressCapability *capability,
    const MeisoRuntimeDesiredState *applied_desired_state,
    MeisoDisplayIngressMediaBinding *out_binding
);

#ifdef __cplusplus
}
#endif

#endif
