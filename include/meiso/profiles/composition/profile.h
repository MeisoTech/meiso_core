#ifndef MEISO_PROFILES_COMPOSITION_PROFILE_H
#define MEISO_PROFILES_COMPOSITION_PROFILE_H

#include <meiso/profiles/composition/scene.h>
#include <meiso/profiles/device_telemetry.h>
#include <meiso/profiles/wifi_telemetry.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_PROFILE_VERSION 1U
#define MEISO_COMPOSITION_PROFILE_RUNTIME_WIRE_VERSION 4U
#define MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT 6U
#define MEISO_COMPOSITION_PROFILE_ROUTE_COUNT 18U
#define MEISO_COMPOSITION_PROFILE_ROUTE_WIRE_SIZE \
    MEISO_RUNTIME_V4_ROUTE_RECORD_WIRE_SIZE
#define MEISO_COMPOSITION_PROFILE_CAPABILITY_BYTES 276U
#define MEISO_COMPOSITION_PROFILE_DYNAMIC_CAPABILITY_BYTES 192U
#define MEISO_COMPOSITION_PROFILE_CATALOG_WIRE_SIZE 2032U
#define MEISO_COMPOSITION_PROFILE_MAX_FRAME_BYTES 8236U
#define MEISO_COMPOSITION_PROFILE_MAX_RUNTIME_PAYLOAD_BYTES 8192U
#define MEISO_COMPOSITION_PROFILE_MAX_REGISTRY_ENTRIES 24U
#define MEISO_COMPOSITION_PROFILE_MAX_ENDPOINT_HISTORY 12U

#define MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE 64U
#define MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE 32U
#define MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE 80U
#define MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE 64U
#define MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE 128U
#define MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE 240U
#define MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE 4096U
#define MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES 1U
#define MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES 1U
#define MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES \
    (MEISO_COMPOSITION_SCENE_TARGET_WIDTH * \
     MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 4U)
#define MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES \
    (640U * 480U * 3U / 2U)
#define MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + \
     MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE)
#define MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE \
    (MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + \
     MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE)

typedef MeisoRuntimeDeliveryClass MeisoCompositionDeliveryClass;
#define MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME \
    MEISO_RUNTIME_DELIVERY_INLINE
#define MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA \
    MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA

typedef uint8_t MeisoCompositionMediaSource;
enum {
    MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW = 1,
    MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA = 2
};

typedef uint16_t MeisoCompositionMediaPixelFormat;
enum {
    MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888 = 1,
    MEISO_COMPOSITION_MEDIA_PIXEL_RGB565 = 2,
    MEISO_COMPOSITION_MEDIA_PIXEL_NV12 = 3,
    MEISO_COMPOSITION_MEDIA_PIXEL_YUYV = 4
};

typedef uint16_t MeisoCompositionMediaCodec;
enum {
    MEISO_COMPOSITION_MEDIA_CODEC_RAW = 1
};

typedef uint32_t MeisoCompositionMediaPixelFormatMask;
enum {
    MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888 = UINT32_C(1) << 0U,
    MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGB565 = UINT32_C(1) << 1U,
    MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12 = UINT32_C(1) << 2U,
    MEISO_COMPOSITION_MEDIA_PIXEL_MASK_YUYV = UINT32_C(1) << 3U,
    MEISO_COMPOSITION_MEDIA_PIXEL_MASK_ALL = UINT32_C(0x0000000f)
};

typedef uint32_t MeisoCompositionMediaCodecMask;
enum {
    MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW = UINT32_C(1) << 0U,
    MEISO_COMPOSITION_MEDIA_CODEC_MASK_ALL =
        MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW
};

typedef struct {
    MeisoCompositionMediaSource source;
    MeisoCompositionMediaPixelFormatMask pixel_formats;
    MeisoCompositionMediaCodecMask codecs;
    uint16_t min_width;
    uint16_t min_height;
    uint16_t max_width;
    uint16_t max_height;
    uint32_t max_fps_millihz;
    uint32_t max_access_unit_bytes;
    uint32_t max_fragment_payload_bytes;
    uint32_t max_bitrate_bps;
} MeisoCompositionMediaCapability;

typedef uint16_t MeisoCompositionMediaRecoveryFlags;
enum {
    MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY = UINT16_C(1) << 0U
};

typedef struct {
    MeisoCompositionMediaPixelFormat pixel_format;
    MeisoCompositionMediaCodec codec;
    uint16_t width;
    uint16_t height;
    uint32_t fps_millihz;
    uint32_t bitrate_bps;
    uint32_t gop_frames;
    MeisoCompositionMediaRecoveryFlags recovery_flags;
} MeisoCompositionMediaDesired;

typedef struct {
    MeisoCompositionMediaSource source;
    MeisoCompositionMediaDesired effective;
    uint64_t media_stream_epoch;
    MeisoSessionNonce channel_nonce;
    uint64_t producer_dropped_frames;
    uint32_t queue_depth;
    uint32_t recovery_generation;
} MeisoCompositionMediaActual;

typedef struct {
    MeisoSessionId session_id;
    uint64_t catalog_generation;
} MeisoCompositionMediaOpen;

typedef struct {
    MeisoSessionId session_id;
    uint64_t catalog_generation;
    MeisoEndpointId endpoint_id;
    uint64_t endpoint_incarnation;
    MeisoResourceId resource_id;
    MeisoCompositionMediaSource source;
    uint64_t production_epoch;
    uint64_t media_stream_epoch;
    MeisoSessionNonce channel_nonce;
} MeisoCompositionMediaBinding;

typedef uint16_t MeisoCompositionMediaFrameFlags;
enum {
    MEISO_COMPOSITION_MEDIA_FRAME_BEGIN = UINT16_C(1) << 0U,
    MEISO_COMPOSITION_MEDIA_FRAME_END = UINT16_C(1) << 1U,
    MEISO_COMPOSITION_MEDIA_FRAME_KEY = UINT16_C(1) << 2U,
    MEISO_COMPOSITION_MEDIA_FRAME_RECOVERY = UINT16_C(1) << 3U
};

typedef struct {
    MeisoCompositionMediaBinding binding;
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoOwnerScope applied_scene_owner;
    uint64_t applied_scene_state_version;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    uint64_t dependency_sequence;
    uint64_t producer_dropped_frames;
    MeisoCompositionMediaPixelFormat pixel_format;
    MeisoCompositionMediaCodec codec;
    uint16_t width;
    uint16_t height;
    uint32_t access_unit_bytes;
    uint32_t fragment_offset;
    MeisoByteView fragment;
    MeisoCompositionMediaFrameFlags flags;
} MeisoCompositionMediaFrame;

/*
 * Canonical big-endian wire (all omitted bytes are reserved zero):
 *   MCAP/64: magic0,ver4,source6,format-mask8,codec-mask12,
 *     min-wh16,max-wh20,max-fps24,max-unit28,max-fragment32,
 *     in-flight36,latest38,max-bitrate40,reserved44..63.
 *   MDES/32: magic0,ver4,format8,codec10,wh12,fps16,bitrate20,
 *     gop24,recovery28,reserved30..31.
 *   MACT/80: MDES effective fields at 8..29, stream-epoch32,
 *     channel-nonce40,drop56,queue64,recovery-generation68,reserved72..79.
 *   MOPN/64: magic0,ver4,Host-role6,SessionId8,catalog40,reserved48..63.
 *   MBND/128: magic0,ver4,SessionId8,catalog40,endpoint48,
 *     incarnation64,resource72,source88,DATA89,EXTERNAL90,reserved91..95,
 *     production96,stream-epoch104,channel-nonce112.
 *   MFRM/240 + fragment: magic0,ver4,flags6,SessionId8,catalog40,
 *     endpoint48,incarnation64,resource72,production88,stream-epoch96,
 *     frame-sequence104,source-time112,scene-owner-role128/incarnation136,
 *     scene-version144,plan152,world160,dependency168,channel-nonce176,
 *     unit-bytes192,fragment-offset196,fragment-bytes200,drop208,
 *     format216,codec218,width220,height222,reserved224..239.
 * Render frames require the scene/plan/world binding; camera frames require
 * those five fields to be canonical zero.
 */

typedef MeisoRuntimeV4RouteDescriptor MeisoCompositionProfileRouteDescriptor;

/*
 * MRT0 v4 catalog preserves the v3 48-byte endpoint record. Its 80-byte
 * route record preserves offsets 0..71 exactly (endpoint0, incarnation16,
 * resource24, kind40, owner41, facet42, reserved43, schema44,
 * max-payload/access-unit64), then delivery-class72 and reserved73..79.
 */

typedef struct {
    uint64_t wifi_incarnation;
    uint64_t presentation_incarnation;
    uint64_t scene_incarnation;
    uint64_t device_telemetry_incarnation;
    uint64_t render_media_incarnation;
    uint64_t camera_media_incarnation;
    MeisoWifiTelemetryCapability wifi_capability;
    MeisoDeviceTelemetryCapability device_telemetry_capability;
    MeisoCompositionMediaCapability render_media_capability;
    MeisoCompositionMediaCapability camera_media_capability;
} MeisoCompositionProfileCatalogConfig;

typedef uint8_t MeisoCompositionProfileStatus;
enum {
    MEISO_COMPOSITION_PROFILE_OK = 0,
    MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_PROFILE_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_PROFILE_CAPACITY = 3,
    MEISO_COMPOSITION_PROFILE_CODEC_FAILURE = 4,
    MEISO_COMPOSITION_PROFILE_COLLISION = 5,
    MEISO_COMPOSITION_PROFILE_TRUNCATED = 6,
    MEISO_COMPOSITION_PROFILE_MALFORMED = 7
};

const MeisoProfileRef *meiso_composition_profile_ref(void);
const MeisoEndpointId *meiso_composition_render_media_endpoint(void);
const MeisoEndpointId *meiso_composition_camera_media_endpoint(void);
/* Exact media schemas: MEDIA_SC/{CAPABIL_,DESIRED_,ACTUAL__,UNIT____}:v1. */
const MeisoSchemaRef *meiso_composition_media_capability_schema(void);
const MeisoResourceDescriptor *meiso_composition_render_media_resources(
    size_t *out_count
);
const MeisoResourceDescriptor *meiso_composition_camera_media_resources(
    size_t *out_count
);
bool meiso_composition_media_desired_supported(
    const MeisoCompositionMediaCapability *capability,
    const MeisoCompositionMediaDesired *desired
);

MeisoCompositionProfileStatus meiso_composition_profile_configure_session(
    MeisoSessionConfig *in_out_config
);

/*
 * Outputs are caller-owned and mutually disjoint. Dynamic capability storage
 * is borrowed by endpoints until a catalog encoder/Runtime copies it.
 * Failure leaves every output byte unchanged.
 */
MeisoCompositionProfileStatus meiso_composition_profile_build_catalog(
    const MeisoCompositionProfileCatalogConfig *config,
    MeisoMutableBuffer dynamic_capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoCompositionProfileRouteDescriptor *routes,
    size_t route_capacity
);

/* Canonical MRT0 v4 catalog codec; Runtime v4 consumes the same 80-byte truth. */
MeisoCompositionProfileStatus meiso_composition_profile_catalog_encode(
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoCompositionProfileRouteDescriptor *routes,
    size_t route_count,
    MeisoMutableBuffer output,
    size_t *out_size
);

/*
 * Every codec requires typed input, wire input/output, borrowed fragment, and
 * out_size ranges to be mutually disjoint. Alias is INVALID_ARGUMENT with no
 * mutation. Other encode failures set a disjoint out_size to zero and leave
 * output unchanged; decode failures leave out_value unchanged.
 */

MeisoCompositionProfileStatus meiso_composition_media_capability_encode(
    const MeisoCompositionMediaCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionProfileStatus meiso_composition_media_capability_decode(
    MeisoByteView input,
    MeisoCompositionMediaCapability *out_value
);
MeisoCompositionProfileStatus meiso_composition_media_desired_encode(
    const MeisoCompositionMediaDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionProfileStatus meiso_composition_media_desired_decode(
    MeisoByteView input,
    MeisoCompositionMediaDesired *out_value
);
MeisoCompositionProfileStatus meiso_composition_media_actual_encode(
    const MeisoCompositionMediaActual *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionProfileStatus meiso_composition_media_actual_decode(
    MeisoByteView input,
    MeisoCompositionMediaActual *out_value
);
MeisoCompositionProfileStatus meiso_composition_media_open_encode(
    const MeisoCompositionMediaOpen *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionProfileStatus meiso_composition_media_open_decode(
    MeisoByteView input,
    MeisoCompositionMediaOpen *out_value
);
MeisoCompositionProfileStatus meiso_composition_media_binding_encode(
    const MeisoCompositionMediaBinding *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoCompositionProfileStatus meiso_composition_media_binding_decode(
    MeisoByteView input,
    MeisoCompositionMediaBinding *out_value
);
MeisoCompositionProfileStatus meiso_composition_media_frame_encode(
    const MeisoCompositionMediaFrame *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
/* On success out_value.fragment borrows input for exactly input's lifetime. */
MeisoCompositionProfileStatus meiso_composition_media_frame_decode(
    MeisoByteView input,
    MeisoCompositionMediaFrame *out_value
);

#ifdef __cplusplus
}
#endif

#endif
