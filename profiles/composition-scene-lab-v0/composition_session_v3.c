#include "composition_session_v3.h"

#include <limits.h>
#include <string.h>

enum {
    MEDIA_CAPABILITY_MAGIC = 0x4d434150U, /* MCAP */
    MEDIA_DESIRED_MAGIC = 0x4d444553U,    /* MDES */
    MEDIA_ACTUAL_MAGIC = 0x4d414354U,     /* MACT */
    MEDIA_OPEN_MAGIC = 0x4d4f504eU,       /* MOPN */
    MEDIA_BIND_MAGIC = 0x4d424e44U,       /* MBND */
    MEDIA_FRAME_MAGIC = 0x4d46524dU,      /* MFRM */
    MEDIA_CODEC_VERSION = 1U,
    MRT0_CATALOG_TYPE = 1U,
    ENDPOINT_RECORD_WIRE_SIZE = 48U,
    CATALOG_PREFIX_WIRE_SIZE = 16U,
    MEDIA_RESOURCE_COUNT = 3U
};

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_WIRE_SIZE +
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT *
            ENDPOINT_RECORD_WIRE_SIZE +
        MEISO_COMPOSITION_SESSION_V3_CAPABILITY_BYTES +
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT *
            MEISO_COMPOSITION_SESSION_V3_ROUTE_WIRE_SIZE ==
        MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE,
    "Profile v3 catalog size"
);
_Static_assert(
    MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
        2U * MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE ==
        MEISO_COMPOSITION_SESSION_V3_CAPABILITY_BYTES,
    "Profile v3 capability bytes"
);

static const MeisoProfileRef PROFILE_V3 = {
    {UINT64_C(0x4b3233305f53434e), UINT64_C(0x5f4c41425f563030)},
    MEISO_COMPOSITION_SESSION_V3_PROFILE_VERSION
};
static const MeisoEndpointId RENDER_ENDPOINT = {
    UINT64_C(0x52454e4445525f4d), UINT64_C(0x454449415f454e44)
};
static const MeisoEndpointId CAMERA_ENDPOINT = {
    UINT64_C(0x43414d4552415f4d), UINT64_C(0x454449415f454e44)
};
static const MeisoSchemaRef MEDIA_CAPABILITY_SCHEMA = {
    {UINT64_C(0x4d454449415f5343), UINT64_C(0x4341505f56303030)}, 1U
};

static const MeisoResourceDescriptor RENDER_RESOURCES[MEDIA_RESOURCE_COUNT] = {
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x4445535f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x4445535f56303030)}, 1U},
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x4143545f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x4143545f56303030)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x52454e4445525f4d), UINT64_C(0x444154415f4d4544)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x554e49545f563030)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES
    }
};
static const MeisoResourceDescriptor CAMERA_RESOURCES[MEDIA_RESOURCE_COUNT] = {
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x4445535f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x4445535f56303030)}, 1U},
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_MEDIA_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x4143545f53544154)},
        MEISO_RESOURCE_KIND_STATE,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x4143545f56303030)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {UINT64_C(0x43414d4552415f4d), UINT64_C(0x444154415f4d4544)},
        MEISO_RESOURCE_KIND_STREAM,
        {{UINT64_C(0x4d454449415f5343), UINT64_C(0x554e49545f563030)}, 1U},
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES
    }
};

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

static void put_u64(uint8_t *out, uint64_t value)
{
    out[0] = (uint8_t)(value >> 56U);
    out[1] = (uint8_t)(value >> 48U);
    out[2] = (uint8_t)(value >> 40U);
    out[3] = (uint8_t)(value >> 32U);
    out[4] = (uint8_t)(value >> 24U);
    out[5] = (uint8_t)(value >> 16U);
    out[6] = (uint8_t)(value >> 8U);
    out[7] = (uint8_t)value;
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
        ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

static uint64_t get_u64(const uint8_t *in)
{
    return ((uint64_t)in[0] << 56U) | ((uint64_t)in[1] << 48U) |
        ((uint64_t)in[2] << 40U) | ((uint64_t)in[3] << 32U) |
        ((uint64_t)in[4] << 24U) | ((uint64_t)in[5] << 16U) |
        ((uint64_t)in[6] << 8U) | (uint64_t)in[7];
}

static void put_id(uint8_t *out, uint64_t high, uint64_t low)
{
    put_u64(out, high);
    put_u64(out + 8U, low);
}

static bool bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    const uintptr_t lhs_begin = (uintptr_t)lhs;
    const uintptr_t rhs_begin = (uintptr_t)rhs;
    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_begin > UINTPTR_MAX - lhs_size ||
        rhs_begin > UINTPTR_MAX - rhs_size) {
        return true;
    }
    return lhs_begin < rhs_begin + rhs_size &&
        rhs_begin < lhs_begin + lhs_size;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

static bool schema_equal(MeisoSchemaRef lhs, MeisoSchemaRef rhs)
{
    return lhs.id.high == rhs.id.high && lhs.id.low == rhs.id.low &&
        lhs.version == rhs.version;
}

static bool media_source_valid(MeisoCompositionMediaSource source)
{
    return source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ||
        source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
}

static bool pixel_mask_valid(MeisoCompositionMediaPixelFormatMask mask)
{
    return mask != 0U &&
        (mask & (MeisoCompositionMediaPixelFormatMask)
            ~MEISO_COMPOSITION_MEDIA_PIXEL_MASK_ALL) == 0U;
}

static bool codec_mask_valid(MeisoCompositionMediaCodecMask mask)
{
    return mask != 0U &&
        (mask & (MeisoCompositionMediaCodecMask)
            ~MEISO_COMPOSITION_MEDIA_CODEC_MASK_ALL) == 0U;
}

static bool pixel_format_valid(MeisoCompositionMediaPixelFormat format)
{
    return format >= MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888 &&
        format <= MEISO_COMPOSITION_MEDIA_PIXEL_YUYV;
}

static bool media_capability_valid(
    const MeisoCompositionMediaCapability *value
)
{
    const uint32_t selected_limit = value != NULL &&
        value->source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES :
        MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES;
    return value != NULL && media_source_valid(value->source) &&
        pixel_mask_valid(value->pixel_formats) &&
        codec_mask_valid(value->codecs) && value->min_width != 0U &&
        value->min_height != 0U && value->min_width <= value->max_width &&
        value->min_height <= value->max_height &&
        value->max_fps_millihz != 0U &&
        value->max_access_unit_bytes != 0U &&
        value->max_access_unit_bytes <= selected_limit &&
        value->max_fragment_payload_bytes != 0U &&
        value->max_fragment_payload_bytes <=
            MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE;
}

static bool media_desired_valid(const MeisoCompositionMediaDesired *value)
{
    return value != NULL && pixel_format_valid(value->pixel_format) &&
        value->codec == MEISO_COMPOSITION_MEDIA_CODEC_RAW &&
        value->width != 0U && value->height != 0U &&
        value->fps_millihz != 0U && value->bitrate_bps == 0U &&
        value->gop_frames == 1U &&
        (value->recovery_flags &
         (MeisoCompositionMediaRecoveryFlags)
             ~MEISO_COMPOSITION_MEDIA_RECOVERY_REQUEST_KEY) == 0U;
}

bool meiso_composition_media_desired_supported(
    const MeisoCompositionMediaCapability *capability,
    const MeisoCompositionMediaDesired *desired
)
{
    uint32_t pixel_bit = 0U;
    uint64_t pixels;
    uint64_t access_unit_bytes;
    if (!media_capability_valid(capability) || !media_desired_valid(desired)) {
        return false;
    }
    pixel_bit = UINT32_C(1) << (desired->pixel_format - 1U);
    pixels = (uint64_t)desired->width * desired->height;
    switch (desired->pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        access_unit_bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        access_unit_bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        if ((desired->width & 1U) != 0U ||
            (desired->height & 1U) != 0U) {
            return false;
        }
        access_unit_bytes = pixels + pixels / 2U;
        break;
    default:
        return false;
    }
    return (capability->pixel_formats & pixel_bit) != 0U &&
        (capability->codecs & MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW) != 0U &&
        desired->width >= capability->min_width &&
        desired->width <= capability->max_width &&
        desired->height >= capability->min_height &&
        desired->height <= capability->max_height &&
        desired->fps_millihz <= capability->max_fps_millihz &&
        access_unit_bytes <= capability->max_access_unit_bytes;
}

static bool media_actual_valid(const MeisoCompositionMediaActual *value)
{
    return value != NULL && media_source_valid(value->source) &&
        media_desired_valid(&value->effective) &&
        value->media_stream_epoch != 0U &&
        meiso_session_nonce_valid(value->channel_nonce) &&
        value->queue_depth <=
            MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES +
                MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES;
}

static bool media_open_valid(const MeisoCompositionMediaOpen *value)
{
    return value != NULL &&
        meiso_session_nonce_valid(value->session_id.host_nonce) &&
        meiso_session_nonce_valid(value->session_id.edge_nonce) &&
        value->catalog_generation != 0U;
}

static const MeisoEndpointId *endpoint_for_source(
    MeisoCompositionMediaSource source
)
{
    return source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
        &RENDER_ENDPOINT : source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ?
            &CAMERA_ENDPOINT : NULL;
}

static const MeisoResourceDescriptor *data_for_source(
    MeisoCompositionMediaSource source
)
{
    return source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
        &RENDER_RESOURCES[2] : source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ?
            &CAMERA_RESOURCES[2] : NULL;
}

static bool binding_valid(const MeisoCompositionMediaBinding *value)
{
    const MeisoEndpointId *endpoint;
    const MeisoResourceDescriptor *resource;
    if (value == NULL || !media_source_valid(value->source)) {
        return false;
    }
    endpoint = endpoint_for_source(value->source);
    resource = data_for_source(value->source);
    return meiso_session_nonce_valid(value->session_id.host_nonce) &&
        meiso_session_nonce_valid(value->session_id.edge_nonce) &&
        value->catalog_generation != 0U && value->endpoint_incarnation != 0U &&
        value->production_epoch != 0U && value->media_stream_epoch != 0U &&
        meiso_session_nonce_valid(value->channel_nonce) && endpoint != NULL &&
        resource != NULL && endpoint_equal(value->endpoint_id, *endpoint) &&
        resource_equal(value->resource_id, resource->id);
}

static bool access_unit_size_valid(const MeisoCompositionMediaFrame *value)
{
    uint64_t pixels;
    uint64_t bytes;
    if (value == NULL || value->width == 0U || value->height == 0U) {
        return false;
    }
    pixels = (uint64_t)value->width * value->height;
    switch (value->pixel_format) {
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888:
        bytes = pixels * 4U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_RGB565:
    case MEISO_COMPOSITION_MEDIA_PIXEL_YUYV:
        bytes = pixels * 2U;
        break;
    case MEISO_COMPOSITION_MEDIA_PIXEL_NV12:
        if ((value->width & 1U) != 0U || (value->height & 1U) != 0U) {
            return false;
        }
        bytes = pixels + pixels / 2U;
        break;
    default:
        return false;
    }
    return bytes == value->access_unit_bytes &&
        bytes <= (value->binding.source ==
                MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
            MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES :
            MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES);
}

static bool frame_scene_binding_valid(
    const MeisoCompositionMediaFrame *value
)
{
    if (value->binding.source ==
        MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW) {
        return meiso_owner_scope_valid(value->applied_scene_owner) &&
            value->applied_scene_owner.role == MEISO_OWNER_ROLE_HOST &&
            value->applied_scene_state_version != 0U &&
            value->active_plan_generation != 0U &&
            value->world_generation != 0U;
    }
    return value->applied_scene_owner.role == 0U &&
        value->applied_scene_owner.incarnation == 0U &&
        value->applied_scene_state_version == 0U &&
        value->active_plan_generation == 0U && value->world_generation == 0U;
}

static bool frame_valid(const MeisoCompositionMediaFrame *value)
{
    uint64_t end;
    bool begin;
    bool finish;
    if (value == NULL || !binding_valid(&value->binding) ||
        value->frame_sequence == 0U ||
        !meiso_time_point_valid(value->source_time) ||
        !frame_scene_binding_valid(value) ||
        value->codec != MEISO_COMPOSITION_MEDIA_CODEC_RAW ||
        !access_unit_size_valid(value) || value->fragment.data == NULL ||
        value->fragment.size == 0U ||
        value->fragment.size >
            MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE ||
        value->fragment.size > UINT32_MAX ||
        value->fragment_offset > value->access_unit_bytes ||
        (value->flags & (MeisoCompositionMediaFrameFlags)~UINT16_C(0x000f)) !=
            0U) {
        return false;
    }
    end = (uint64_t)value->fragment_offset + value->fragment.size;
    if (end > value->access_unit_bytes) {
        return false;
    }
    begin = (value->flags & MEISO_COMPOSITION_MEDIA_FRAME_BEGIN) != 0U;
    finish = (value->flags & MEISO_COMPOSITION_MEDIA_FRAME_END) != 0U;
    return begin == (value->fragment_offset == 0U) &&
        finish == (end == value->access_unit_bytes) &&
        (value->flags & MEISO_COMPOSITION_MEDIA_FRAME_KEY) != 0U &&
        value->dependency_sequence == 0U;
}

static MeisoCompositionSessionV3Status prepare_encode(
    const void *value,
    size_t value_size,
    bool valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (value != NULL &&
        ranges_overlap(value, value_size, out_size, sizeof(*out_size))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (value == NULL || output.data == NULL) {
        *out_size = 0U;
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (ranges_overlap(value, value_size, output.data, required) ||
        ranges_overlap(output.data, required, out_size, sizeof(*out_size))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (!valid) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }
    return output.capacity < required ? MEISO_COMPOSITION_SESSION_V3_CAPACITY :
        MEISO_COMPOSITION_SESSION_V3_OK;
}

static MeisoCompositionSessionV3Status prepare_decode(
    MeisoByteView input,
    size_t required,
    void *output,
    size_t output_size
)
{
    if (input.data == NULL || output == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (ranges_overlap(input.data, input.size, output, output_size)) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (input.size < required) {
        return MEISO_COMPOSITION_SESSION_V3_TRUNCATED;
    }
    return input.size > required ? MEISO_COMPOSITION_SESSION_V3_MALFORMED :
        MEISO_COMPOSITION_SESSION_V3_OK;
}

const MeisoProfileRef *meiso_composition_scene_profile_v3(void)
{
    return &PROFILE_V3;
}

const MeisoEndpointId *meiso_composition_render_media_endpoint(void)
{
    return &RENDER_ENDPOINT;
}

const MeisoEndpointId *meiso_composition_camera_media_endpoint(void)
{
    return &CAMERA_ENDPOINT;
}

const MeisoSchemaRef *meiso_composition_media_capability_schema(void)
{
    return &MEDIA_CAPABILITY_SCHEMA;
}

const MeisoResourceDescriptor *meiso_composition_render_media_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEDIA_RESOURCE_COUNT;
    }
    return RENDER_RESOURCES;
}

const MeisoResourceDescriptor *meiso_composition_camera_media_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEDIA_RESOURCE_COUNT;
    }
    return CAMERA_RESOURCES;
}

MeisoCompositionSessionV3Status meiso_composition_session_v3_configure_session(
    MeisoSessionConfig *in_out_config
)
{
    MeisoSessionConfig candidate;
    if (in_out_config == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    candidate = *in_out_config;
    candidate.profile = PROFILE_V3;
    candidate.limits = (MeisoSessionLimits){
        MEISO_COMPOSITION_SESSION_V3_MAX_FRAME_BYTES,
        MEISO_COMPOSITION_SESSION_V3_MAX_RUNTIME_PAYLOAD_BYTES,
        MEISO_COMPOSITION_SESSION_V3_MAX_REGISTRY_ENTRIES
    };
    if (meiso_session_memory_required(&candidate) == 0U) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }
    *in_out_config = candidate;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

static bool outputs_disjoint(
    const MeisoCompositionSessionV3CatalogConfig *config,
    MeisoMutableBuffer capability,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_bytes,
    MeisoCompositionSessionV3RouteDescriptor *routes,
    size_t route_bytes
)
{
    return !ranges_overlap(config, sizeof(*config), capability.data,
                           capability.capacity) &&
        !ranges_overlap(config, sizeof(*config), endpoints, endpoint_bytes) &&
        !ranges_overlap(config, sizeof(*config), routes, route_bytes) &&
        !ranges_overlap(capability.data, capability.capacity,
                        endpoints, endpoint_bytes) &&
        !ranges_overlap(capability.data, capability.capacity,
                        routes, route_bytes) &&
        !ranges_overlap(endpoints, endpoint_bytes, routes, route_bytes);
}

static bool descriptor_ids_disjoint(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    const MeisoCompositionSessionV3RouteDescriptor *routes
)
{
    size_t i;
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++i) {
        size_t j;
        for (j = i + 1U; j < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT;
             ++j) {
            if (endpoint_equal(endpoints[i].id, endpoints[j].id)) {
                return false;
            }
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++i) {
        size_t j;
        for (j = i + 1U; j < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++j) {
            if (resource_equal(routes[i].route.resource.id,
                               routes[j].route.resource.id)) {
                return false;
            }
        }
    }
    return true;
}

static void set_routes_for_endpoint(
    MeisoCompositionSessionV3RouteDescriptor *output,
    const MeisoEndpointId *endpoint,
    uint64_t incarnation,
    const MeisoResourceDescriptor *resources,
    MeisoCompositionDeliveryClass data_delivery
)
{
    static const MeisoRuntimeFacet facets[MEDIA_RESOURCE_COUNT] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    size_t i;
    for (i = 0U; i < MEDIA_RESOURCE_COUNT; ++i) {
        output[i].route.endpoint_id = *endpoint;
        output[i].route.endpoint_incarnation = incarnation;
        output[i].route.resource = resources[i];
        output[i].route.facet = facets[i];
        output[i].delivery = i == 2U ? data_delivery :
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
    }
}

MeisoCompositionSessionV3Status meiso_composition_session_v3_build_catalog(
    const MeisoCompositionSessionV3CatalogConfig *config,
    MeisoMutableBuffer dynamic_capability_storage,
    MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_capacity,
    MeisoCompositionSessionV3RouteDescriptor *routes,
    size_t route_capacity
)
{
    static const MeisoRuntimeFacet wifi_facets[3] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeEndpointDescriptor endpoint_candidate[
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT
    ];
    MeisoCompositionSessionV3RouteDescriptor route_candidate[
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT
    ];
    uint8_t capability_candidate[
        MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES
    ];
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *device_resources;
    size_t wifi_count = 0U;
    size_t device_count = 0U;
    size_t composition_endpoint_count = 0U;
    size_t composition_route_count = 0U;
    size_t encoded_size = 0U;
    size_t endpoint_bytes;
    size_t route_bytes;
    size_t i;

    if (config == NULL || endpoints == NULL || routes == NULL ||
        dynamic_capability_storage.data == NULL ||
        !checked_multiply(endpoint_capacity, sizeof(*endpoints),
                          &endpoint_bytes) ||
        !checked_multiply(route_capacity, sizeof(*routes), &route_bytes)) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (endpoint_capacity < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT ||
        route_capacity < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT ||
        dynamic_capability_storage.capacity <
            MEISO_COMPOSITION_SESSION_V3_DYNAMIC_CAPABILITY_BYTES) {
        return MEISO_COMPOSITION_SESSION_V3_CAPACITY;
    }
    endpoint_bytes = MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT *
        sizeof(*endpoints);
    route_bytes = MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT * sizeof(*routes);
    if (!outputs_disjoint(config, dynamic_capability_storage, endpoints,
                          endpoint_bytes, routes, route_bytes)) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (config->wifi_incarnation == 0U ||
        config->presentation_incarnation == 0U ||
        config->scene_incarnation == 0U ||
        config->device_telemetry_incarnation == 0U ||
        config->render_media_incarnation == 0U ||
        config->camera_media_incarnation == 0U ||
        config->render_media_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ||
        config->camera_media_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }

    memset(endpoint_candidate, 0, sizeof(endpoint_candidate));
    memset(route_candidate, 0, sizeof(route_candidate));
    if (meiso_wifi_telemetry_capability_encode(
            &config->wifi_capability,
            (MeisoMutableBuffer){
                capability_candidate,
                MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE ||
        meiso_device_telemetry_capability_encode(
            &config->device_telemetry_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        encoded_size != MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE ||
        meiso_composition_media_capability_encode(
            &config->render_media_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE ||
        meiso_composition_media_capability_encode(
            &config->camera_media_capability,
            (MeisoMutableBuffer){
                capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
                    MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
            },
            &encoded_size
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        encoded_size != MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE) {
        return MEISO_COMPOSITION_SESSION_V3_CODEC_FAILURE;
    }

    endpoint_candidate[0] = (MeisoRuntimeEndpointDescriptor){
        *meiso_wifi_telemetry_endpoint(), config->wifi_incarnation,
        *meiso_wifi_telemetry_capability_schema(),
        {capability_candidate, MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE}
    };
    if (meiso_composition_scene_build_endpoints(
            config->presentation_incarnation, config->scene_incarnation,
            endpoint_candidate + 1U,
            MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT,
            &composition_endpoint_count
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
        composition_endpoint_count != MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT) {
        return MEISO_COMPOSITION_SESSION_V3_CODEC_FAILURE;
    }
    endpoint_candidate[3] = (MeisoRuntimeEndpointDescriptor){
        *meiso_device_telemetry_endpoint(),
        config->device_telemetry_incarnation,
        *meiso_device_telemetry_capability_schema(),
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
            MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE
        }
    };
    endpoint_candidate[4] = (MeisoRuntimeEndpointDescriptor){
        RENDER_ENDPOINT, config->render_media_incarnation,
        MEDIA_CAPABILITY_SCHEMA,
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
        }
    };
    endpoint_candidate[5] = (MeisoRuntimeEndpointDescriptor){
        CAMERA_ENDPOINT, config->camera_media_incarnation,
        MEDIA_CAPABILITY_SCHEMA,
        {
            capability_candidate + MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
                MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
            MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
        }
    };

    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    device_resources = meiso_device_telemetry_resources(&device_count);
    if (wifi_resources == NULL || device_resources == NULL || wifi_count != 3U ||
        device_count != 3U) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }
    for (i = 0U; i < 3U; ++i) {
        route_candidate[i].route = (MeisoRuntimeRouteDescriptor){
            *meiso_wifi_telemetry_endpoint(), config->wifi_incarnation,
            wifi_resources[i], wifi_facets[i]
        };
        route_candidate[i].delivery =
            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
    }
    {
        MeisoRuntimeRouteDescriptor composition_routes[
            MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
        ];
        if (meiso_composition_scene_build_routes(
                config->presentation_incarnation, config->scene_incarnation,
                composition_routes, MEISO_COMPOSITION_SCENE_RESOURCE_COUNT,
                &composition_route_count
            ) != MEISO_COMPOSITION_SCENE_CODEC_OK ||
            composition_route_count != MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
            return MEISO_COMPOSITION_SESSION_V3_CODEC_FAILURE;
        }
        for (i = 0U; i < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT; ++i) {
            route_candidate[3U + i].route = composition_routes[i];
            route_candidate[3U + i].delivery =
                MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME;
        }
    }
    set_routes_for_endpoint(route_candidate + 9U,
                            meiso_device_telemetry_endpoint(),
                            config->device_telemetry_incarnation,
                            device_resources,
                            MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME);
    set_routes_for_endpoint(route_candidate + 12U, &RENDER_ENDPOINT,
                            config->render_media_incarnation,
                            RENDER_RESOURCES,
                            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    set_routes_for_endpoint(route_candidate + 15U, &CAMERA_ENDPOINT,
                            config->camera_media_incarnation,
                            CAMERA_RESOURCES,
                            MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA);
    if (!descriptor_ids_disjoint(endpoint_candidate, route_candidate)) {
        return MEISO_COMPOSITION_SESSION_V3_COLLISION;
    }

    memmove(dynamic_capability_storage.data, capability_candidate,
            sizeof(capability_candidate));
    endpoint_candidate[0].capability.data = dynamic_capability_storage.data;
    endpoint_candidate[3].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    endpoint_candidate[4].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE;
    endpoint_candidate[5].capability.data =
        dynamic_capability_storage.data +
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE +
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE;
    memmove(endpoints, endpoint_candidate, sizeof(endpoint_candidate));
    memmove(routes, route_candidate, sizeof(route_candidate));
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

static bool expected_catalog_shape(
    const MeisoRuntimeEndpointDescriptor *endpoints,
    const MeisoCompositionSessionV3RouteDescriptor *routes
)
{
    static const size_t expected_capability_sizes[6] = {
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE,
        MEISO_DEVICE_TELEMETRY_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE
    };
    const MeisoEndpointId expected_endpoints[6] = {
        *meiso_wifi_telemetry_endpoint(),
        *meiso_composition_scene_presentation_endpoint(),
        *meiso_composition_scene_scene_endpoint(),
        *meiso_device_telemetry_endpoint(), RENDER_ENDPOINT, CAMERA_ENDPOINT
    };
    const MeisoSchemaRef expected_schemas[6] = {
        *meiso_wifi_telemetry_capability_schema(),
        *meiso_composition_scene_presentation_capability_schema(),
        *meiso_composition_scene_capability_schema(),
        *meiso_device_telemetry_capability_schema(),
        MEDIA_CAPABILITY_SCHEMA, MEDIA_CAPABILITY_SCHEMA
    };
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *composition_resources;
    const MeisoResourceDescriptor *device_resources;
    const MeisoResourceDescriptor *expected_resources[18];
    static const MeisoRuntimeFacet expected_facets[18] = {
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_IMMUTABLE, MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA,
        MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoWifiTelemetryCapability wifi_capability;
    MeisoDeviceTelemetryCapability device_capability;
    MeisoCompositionMediaCapability render_capability;
    MeisoCompositionMediaCapability camera_capability;
    MeisoByteView presentation_capability =
        meiso_composition_scene_presentation_capability();
    MeisoByteView scene_capability = meiso_composition_scene_capability();
    size_t wifi_count = 0U;
    size_t composition_count = 0U;
    size_t device_count = 0U;
    size_t capability_sum = 0U;
    size_t i;
    for (i = 0U; i < 6U; ++i) {
        if (!endpoint_equal(endpoints[i].id, expected_endpoints[i]) ||
            endpoints[i].incarnation == 0U ||
            endpoints[i].capability.data == NULL ||
            endpoints[i].capability.size != expected_capability_sizes[i] ||
            !schema_equal(endpoints[i].capability_schema,
                          expected_schemas[i])) {
            return false;
        }
        capability_sum += endpoints[i].capability.size;
    }
    if (capability_sum != MEISO_COMPOSITION_SESSION_V3_CAPABILITY_BYTES ||
        !descriptor_ids_disjoint(endpoints, routes) ||
        meiso_wifi_telemetry_capability_decode(
            endpoints[0].capability, &wifi_capability
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        presentation_capability.size != endpoints[1].capability.size ||
        memcmp(presentation_capability.data, endpoints[1].capability.data,
               presentation_capability.size) != 0 ||
        scene_capability.size != endpoints[2].capability.size ||
        memcmp(scene_capability.data, endpoints[2].capability.data,
               scene_capability.size) != 0 ||
        meiso_device_telemetry_capability_decode(
            endpoints[3].capability, &device_capability
        ) != MEISO_DEVICE_TELEMETRY_CODEC_OK ||
        meiso_composition_media_capability_decode(
            endpoints[4].capability, &render_capability
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        render_capability.source !=
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ||
        meiso_composition_media_capability_decode(
            endpoints[5].capability, &camera_capability
        ) != MEISO_COMPOSITION_SESSION_V3_OK ||
        camera_capability.source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) {
        return false;
    }
    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    composition_resources =
        meiso_composition_scene_resources(&composition_count);
    device_resources = meiso_device_telemetry_resources(&device_count);
    if (wifi_resources == NULL || composition_resources == NULL ||
        device_resources == NULL || wifi_count != 3U ||
        composition_count != 6U || device_count != 3U) {
        return false;
    }
    for (i = 0U; i < 3U; ++i) {
        expected_resources[i] = &wifi_resources[i];
        expected_resources[9U + i] = &device_resources[i];
        expected_resources[12U + i] = &RENDER_RESOURCES[i];
        expected_resources[15U + i] = &CAMERA_RESOURCES[i];
    }
    for (i = 0U; i < 6U; ++i) {
        expected_resources[3U + i] = &composition_resources[i];
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT; ++i) {
        const MeisoRuntimeRouteDescriptor *route = &routes[i].route;
        const size_t endpoint_index = i < 3U ? 0U : i < 5U ? 1U :
            i < 9U ? 2U : i < 12U ? 3U : i < 15U ? 4U : 5U;
        const bool external = (i == 14U || i == 17U);
        if (!endpoint_equal(route->endpoint_id, endpoints[endpoint_index].id) ||
            route->endpoint_incarnation !=
                endpoints[endpoint_index].incarnation ||
            route->facet != expected_facets[i] ||
            !meiso_resource_descriptor_equal(
                &route->resource, expected_resources[i]
            ) ||
            (external ? routes[i].delivery !=
                    MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA :
                routes[i].delivery !=
                    MEISO_COMPOSITION_DELIVERY_INLINE_RUNTIME)) {
            return false;
        }
    }
    return true;
}

MeisoCompositionSessionV3Status meiso_composition_session_v3_catalog_encode(
    uint64_t generation,
    const MeisoRuntimeEndpointDescriptor *endpoints,
    size_t endpoint_count,
    const MeisoCompositionSessionV3RouteDescriptor *routes,
    size_t route_count,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t candidate[MEISO_COMPOSITION_SESSION_V3_CATALOG_WIRE_SIZE];
    const size_t endpoint_bytes =
        MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT * sizeof(*endpoints);
    const size_t route_bytes =
        MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT * sizeof(*routes);
    size_t cursor = MEISO_RUNTIME_WIRE_HEADER_SIZE + CATALOG_PREFIX_WIRE_SIZE;
    size_t i;
    if (out_size == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if ((endpoints != NULL && ranges_overlap(
             endpoints, endpoint_bytes, out_size, sizeof(*out_size)
         )) ||
        (routes != NULL && ranges_overlap(
             routes, route_bytes, out_size, sizeof(*out_size)
         ))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (endpoints == NULL || routes == NULL || output.data == NULL) {
        *out_size = 0U;
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (ranges_overlap(endpoints, endpoint_bytes, routes, route_bytes) ||
        ranges_overlap(endpoints, endpoint_bytes, output.data,
                       sizeof(candidate)) ||
        ranges_overlap(routes, route_bytes, output.data, sizeof(candidate)) ||
        ranges_overlap(output.data, sizeof(candidate), out_size,
                       sizeof(*out_size))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (generation == 0U ||
        endpoint_count != MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT ||
        route_count != MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT) {
        *out_size = 0U;
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }
    for (i = 0U; i < MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT; ++i) {
        if (ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           output.data, sizeof(candidate)) ||
            ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           out_size, sizeof(*out_size)) ||
            ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           endpoints, endpoint_bytes) ||
            ranges_overlap(endpoints[i].capability.data,
                           endpoints[i].capability.size,
                           routes, route_bytes)) {
            return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
        }
    }
    *out_size = 0U;
    if (!expected_catalog_shape(endpoints, routes)) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_CONFIG;
    }
    if (output.capacity < sizeof(candidate)) {
        return MEISO_COMPOSITION_SESSION_V3_CAPACITY;
    }
    memset(candidate, 0, sizeof(candidate));
    candidate[0] = (uint8_t)'M';
    candidate[1] = (uint8_t)'R';
    candidate[2] = (uint8_t)'T';
    candidate[3] = (uint8_t)'0';
    candidate[4] = MEISO_COMPOSITION_SESSION_V3_RUNTIME_WIRE_VERSION;
    candidate[5] = MRT0_CATALOG_TYPE;
    put_u32(candidate + 8U, (uint32_t)sizeof(candidate));
    put_u64(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE, generation);
    put_u32(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE + 8U,
            MEISO_COMPOSITION_SESSION_V3_ENDPOINT_COUNT);
    put_u32(candidate + MEISO_RUNTIME_WIRE_HEADER_SIZE + 12U,
            MEISO_COMPOSITION_SESSION_V3_ROUTE_COUNT);
    for (i = 0U; i < endpoint_count; ++i) {
        put_id(candidate + cursor, endpoints[i].id.high, endpoints[i].id.low);
        put_u64(candidate + cursor + 16U, endpoints[i].incarnation);
        put_id(candidate + cursor + 24U,
               endpoints[i].capability_schema.id.high,
               endpoints[i].capability_schema.id.low);
        put_u32(candidate + cursor + 40U,
                endpoints[i].capability_schema.version);
        put_u32(candidate + cursor + 44U,
                (uint32_t)endpoints[i].capability.size);
        cursor += ENDPOINT_RECORD_WIRE_SIZE;
        memmove(candidate + cursor, endpoints[i].capability.data,
                endpoints[i].capability.size);
        cursor += endpoints[i].capability.size;
    }
    for (i = 0U; i < route_count; ++i) {
        const MeisoRuntimeRouteDescriptor *route = &routes[i].route;
        put_id(candidate + cursor, route->endpoint_id.high,
               route->endpoint_id.low);
        put_u64(candidate + cursor + 16U, route->endpoint_incarnation);
        put_id(candidate + cursor + 24U, route->resource.id.high,
               route->resource.id.low);
        candidate[cursor + 40U] = route->resource.kind;
        candidate[cursor + 41U] = route->resource.owner;
        candidate[cursor + 42U] = route->facet;
        put_id(candidate + cursor + 44U, route->resource.schema.id.high,
               route->resource.schema.id.low);
        put_u32(candidate + cursor + 60U, route->resource.schema.version);
        put_u64(candidate + cursor + 64U,
                (uint64_t)route->resource.max_payload_size);
        candidate[cursor + 72U] = routes[i].delivery;
        cursor += MEISO_COMPOSITION_SESSION_V3_ROUTE_WIRE_SIZE;
    }
    if (cursor != sizeof(candidate)) {
        return MEISO_COMPOSITION_SESSION_V3_CODEC_FAILURE;
    }
    memmove(output.data, candidate, sizeof(candidate));
    *out_size = sizeof(candidate);
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_capability_encode(
    const MeisoCompositionMediaCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSessionV3Status status = prepare_encode(
        value, sizeof(*value), media_capability_valid(value), output,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE);
    put_u32(output.data, MEDIA_CAPABILITY_MAGIC);
    put_u16(output.data + 4U, MEDIA_CODEC_VERSION);
    output.data[6] = value->source;
    put_u32(output.data + 8U, value->pixel_formats);
    put_u32(output.data + 12U, value->codecs);
    put_u16(output.data + 16U, value->min_width);
    put_u16(output.data + 18U, value->min_height);
    put_u16(output.data + 20U, value->max_width);
    put_u16(output.data + 22U, value->max_height);
    put_u32(output.data + 24U, value->max_fps_millihz);
    put_u32(output.data + 28U, value->max_access_unit_bytes);
    put_u32(output.data + 32U, value->max_fragment_payload_bytes);
    put_u16(output.data + 36U, MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES);
    put_u16(output.data + 38U, MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES);
    put_u32(output.data + 40U, value->max_bitrate_bps);
    *out_size = MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_capability_decode(
    MeisoByteView input,
    MeisoCompositionMediaCapability *out_value
)
{
    MeisoCompositionMediaCapability value;
    MeisoCompositionSessionV3Status status = prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (get_u32(input.data) != MEDIA_CAPABILITY_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION || input.data[7] != 0U ||
        get_u16(input.data + 36U) !=
            MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES ||
        get_u16(input.data + 38U) !=
            MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES ||
        !bytes_zero(input.data + 44U, 20U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.source = input.data[6];
    value.pixel_formats = get_u32(input.data + 8U);
    value.codecs = get_u32(input.data + 12U);
    value.min_width = get_u16(input.data + 16U);
    value.min_height = get_u16(input.data + 18U);
    value.max_width = get_u16(input.data + 20U);
    value.max_height = get_u16(input.data + 22U);
    value.max_fps_millihz = get_u32(input.data + 24U);
    value.max_access_unit_bytes = get_u32(input.data + 28U);
    value.max_fragment_payload_bytes = get_u32(input.data + 32U);
    value.max_bitrate_bps = get_u32(input.data + 40U);
    if (!media_capability_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_desired_encode(
    const MeisoCompositionMediaDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSessionV3Status status = prepare_encode(
        value, sizeof(*value), media_desired_valid(value), output,
        MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE);
    put_u32(output.data, MEDIA_DESIRED_MAGIC);
    put_u16(output.data + 4U, MEDIA_CODEC_VERSION);
    put_u16(output.data + 8U, value->pixel_format);
    put_u16(output.data + 10U, value->codec);
    put_u16(output.data + 12U, value->width);
    put_u16(output.data + 14U, value->height);
    put_u32(output.data + 16U, value->fps_millihz);
    put_u32(output.data + 20U, value->bitrate_bps);
    put_u32(output.data + 24U, value->gop_frames);
    put_u16(output.data + 28U, value->recovery_flags);
    *out_size = MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_desired_decode(
    MeisoByteView input,
    MeisoCompositionMediaDesired *out_value
)
{
    MeisoCompositionMediaDesired value;
    MeisoCompositionSessionV3Status status = prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (get_u32(input.data) != MEDIA_DESIRED_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION ||
        !bytes_zero(input.data + 6U, 2U) ||
        !bytes_zero(input.data + 30U, 2U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.pixel_format = get_u16(input.data + 8U);
    value.codec = get_u16(input.data + 10U);
    value.width = get_u16(input.data + 12U);
    value.height = get_u16(input.data + 14U);
    value.fps_millihz = get_u32(input.data + 16U);
    value.bitrate_bps = get_u32(input.data + 20U);
    value.gop_frames = get_u32(input.data + 24U);
    value.recovery_flags = get_u16(input.data + 28U);
    if (!media_desired_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_actual_encode(
    const MeisoCompositionMediaActual *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSessionV3Status status = prepare_encode(
        value, sizeof(*value), media_actual_valid(value), output,
        MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE);
    put_u32(output.data, MEDIA_ACTUAL_MAGIC);
    put_u16(output.data + 4U, MEDIA_CODEC_VERSION);
    output.data[6] = value->source;
    put_u16(output.data + 8U, value->effective.pixel_format);
    put_u16(output.data + 10U, value->effective.codec);
    put_u16(output.data + 12U, value->effective.width);
    put_u16(output.data + 14U, value->effective.height);
    put_u32(output.data + 16U, value->effective.fps_millihz);
    put_u32(output.data + 20U, value->effective.bitrate_bps);
    put_u32(output.data + 24U, value->effective.gop_frames);
    put_u16(output.data + 28U, value->effective.recovery_flags);
    put_u64(output.data + 32U, value->media_stream_epoch);
    put_u64(output.data + 40U, value->channel_nonce.high);
    put_u64(output.data + 48U, value->channel_nonce.low);
    put_u64(output.data + 56U, value->producer_dropped_frames);
    put_u32(output.data + 64U, value->queue_depth);
    put_u32(output.data + 68U, value->recovery_generation);
    *out_size = MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_actual_decode(
    MeisoByteView input,
    MeisoCompositionMediaActual *out_value
)
{
    MeisoCompositionMediaActual value;
    MeisoCompositionSessionV3Status status = prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (get_u32(input.data) != MEDIA_ACTUAL_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION || input.data[7] != 0U ||
        !bytes_zero(input.data + 30U, 2U) ||
        !bytes_zero(input.data + 72U, 8U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.source = input.data[6];
    value.effective.pixel_format = get_u16(input.data + 8U);
    value.effective.codec = get_u16(input.data + 10U);
    value.effective.width = get_u16(input.data + 12U);
    value.effective.height = get_u16(input.data + 14U);
    value.effective.fps_millihz = get_u32(input.data + 16U);
    value.effective.bitrate_bps = get_u32(input.data + 20U);
    value.effective.gop_frames = get_u32(input.data + 24U);
    value.effective.recovery_flags = get_u16(input.data + 28U);
    value.media_stream_epoch = get_u64(input.data + 32U);
    value.channel_nonce.high = get_u64(input.data + 40U);
    value.channel_nonce.low = get_u64(input.data + 48U);
    value.producer_dropped_frames = get_u64(input.data + 56U);
    value.queue_depth = get_u32(input.data + 64U);
    value.recovery_generation = get_u32(input.data + 68U);
    if (!media_actual_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

static void encode_session_id(uint8_t *out, MeisoSessionId id)
{
    put_u64(out, id.host_nonce.high);
    put_u64(out + 8U, id.host_nonce.low);
    put_u64(out + 16U, id.edge_nonce.high);
    put_u64(out + 24U, id.edge_nonce.low);
}

static MeisoSessionId decode_session_id(const uint8_t *in)
{
    MeisoSessionId id;
    id.host_nonce.high = get_u64(in);
    id.host_nonce.low = get_u64(in + 8U);
    id.edge_nonce.high = get_u64(in + 16U);
    id.edge_nonce.low = get_u64(in + 24U);
    return id;
}

MeisoCompositionSessionV3Status meiso_composition_media_open_encode(
    const MeisoCompositionMediaOpen *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSessionV3Status status = prepare_encode(
        value, sizeof(*value), media_open_valid(value), output,
        MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE);
    put_u32(output.data, MEDIA_OPEN_MAGIC);
    put_u16(output.data + 4U, MEDIA_CODEC_VERSION);
    output.data[6] = MEISO_SESSION_ROLE_HOST;
    encode_session_id(output.data + 8U, value->session_id);
    put_u64(output.data + 40U, value->catalog_generation);
    *out_size = MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_open_decode(
    MeisoByteView input,
    MeisoCompositionMediaOpen *out_value
)
{
    MeisoCompositionMediaOpen value;
    MeisoCompositionSessionV3Status status = prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (get_u32(input.data) != MEDIA_OPEN_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION ||
        input.data[6] != MEISO_SESSION_ROLE_HOST || input.data[7] != 0U ||
        !bytes_zero(input.data + 48U, 16U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    value.session_id = decode_session_id(input.data + 8U);
    value.catalog_generation = get_u64(input.data + 40U);
    if (!media_open_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_binding_encode(
    const MeisoCompositionMediaBinding *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSessionV3Status status = prepare_encode(
        value, sizeof(*value), binding_valid(value), output,
        MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE);
    put_u32(output.data, MEDIA_BIND_MAGIC);
    put_u16(output.data + 4U, MEDIA_CODEC_VERSION);
    encode_session_id(output.data + 8U, value->session_id);
    put_u64(output.data + 40U, value->catalog_generation);
    put_id(output.data + 48U, value->endpoint_id.high, value->endpoint_id.low);
    put_u64(output.data + 64U, value->endpoint_incarnation);
    put_id(output.data + 72U, value->resource_id.high, value->resource_id.low);
    output.data[88] = value->source;
    output.data[89] = MEISO_RUNTIME_FACET_DATA;
    output.data[90] = MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA;
    put_u64(output.data + 96U, value->production_epoch);
    put_u64(output.data + 104U, value->media_stream_epoch);
    put_u64(output.data + 112U, value->channel_nonce.high);
    put_u64(output.data + 120U, value->channel_nonce.low);
    *out_size = MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_binding_decode(
    MeisoByteView input,
    MeisoCompositionMediaBinding *out_value
)
{
    MeisoCompositionMediaBinding value;
    MeisoCompositionSessionV3Status status = prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (get_u32(input.data) != MEDIA_BIND_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION ||
        !bytes_zero(input.data + 6U, 2U) ||
        input.data[89] != MEISO_RUNTIME_FACET_DATA ||
        input.data[90] != MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA ||
        !bytes_zero(input.data + 91U, 5U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.session_id = decode_session_id(input.data + 8U);
    value.catalog_generation = get_u64(input.data + 40U);
    value.endpoint_id.high = get_u64(input.data + 48U);
    value.endpoint_id.low = get_u64(input.data + 56U);
    value.endpoint_incarnation = get_u64(input.data + 64U);
    value.resource_id.high = get_u64(input.data + 72U);
    value.resource_id.low = get_u64(input.data + 80U);
    value.source = input.data[88];
    value.production_epoch = get_u64(input.data + 96U);
    value.media_stream_epoch = get_u64(input.data + 104U);
    value.channel_nonce.high = get_u64(input.data + 112U);
    value.channel_nonce.low = get_u64(input.data + 120U);
    if (!binding_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_frame_encode(
    const MeisoCompositionMediaFrame *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t header[MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE];
    size_t required = MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
    MeisoCompositionSessionV3Status status;
    if (out_size == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (value != NULL && value->fragment.size <=
            SIZE_MAX - MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        required += value->fragment.size;
    }
    if (value != NULL && out_size != NULL &&
        (ranges_overlap(value, sizeof(*value), value->fragment.data,
                        value->fragment.size) ||
         ranges_overlap(out_size, sizeof(*out_size), value->fragment.data,
                        value->fragment.size))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    status = prepare_encode(
        value, sizeof(*value), frame_valid(value), output, required, out_size
    );
    if (status != MEISO_COMPOSITION_SESSION_V3_OK) {
        return status;
    }
    if (ranges_overlap(output.data, required, value->fragment.data,
                       value->fragment.size)) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    memset(header, 0, sizeof(header));
    put_u32(header, MEDIA_FRAME_MAGIC);
    put_u16(header + 4U, MEDIA_CODEC_VERSION);
    put_u16(header + 6U, value->flags);
    encode_session_id(header + 8U, value->binding.session_id);
    put_u64(header + 40U, value->binding.catalog_generation);
    put_id(header + 48U, value->binding.endpoint_id.high,
           value->binding.endpoint_id.low);
    put_u64(header + 64U, value->binding.endpoint_incarnation);
    put_id(header + 72U, value->binding.resource_id.high,
           value->binding.resource_id.low);
    put_u64(header + 88U, value->binding.production_epoch);
    put_u64(header + 96U, value->binding.media_stream_epoch);
    put_u64(header + 104U, value->frame_sequence);
    put_u64(header + 112U, value->source_time.clock_epoch);
    put_u64(header + 120U, value->source_time.monotonic_ns);
    header[128] = value->applied_scene_owner.role;
    put_u64(header + 136U, value->applied_scene_owner.incarnation);
    put_u64(header + 144U, value->applied_scene_state_version);
    put_u64(header + 152U, value->active_plan_generation);
    put_u64(header + 160U, value->world_generation);
    put_u64(header + 168U, value->dependency_sequence);
    put_u64(header + 176U, value->binding.channel_nonce.high);
    put_u64(header + 184U, value->binding.channel_nonce.low);
    put_u32(header + 192U, value->access_unit_bytes);
    put_u32(header + 196U, value->fragment_offset);
    put_u32(header + 200U, (uint32_t)value->fragment.size);
    put_u64(header + 208U, value->producer_dropped_frames);
    put_u16(header + 216U, value->pixel_format);
    put_u16(header + 218U, value->codec);
    put_u16(header + 220U, value->width);
    put_u16(header + 222U, value->height);
    memmove(output.data, header, sizeof(header));
    memmove(output.data + sizeof(header), value->fragment.data,
            value->fragment.size);
    *out_size = required;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}

MeisoCompositionSessionV3Status meiso_composition_media_frame_decode(
    MeisoByteView input,
    MeisoCompositionMediaFrame *out_value
)
{
    MeisoCompositionMediaFrame value;
    uint32_t fragment_size;
    if (input.data == NULL || out_value == NULL) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SESSION_V3_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_SESSION_V3_TRUNCATED;
    }
    fragment_size = get_u32(input.data + 200U);
    if ((size_t)fragment_size >
            SIZE_MAX - MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    if (input.size < MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE +
            (size_t)fragment_size) {
        return MEISO_COMPOSITION_SESSION_V3_TRUNCATED;
    }
    if (input.size > MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE +
            (size_t)fragment_size ||
        get_u32(input.data) != MEDIA_FRAME_MAGIC ||
        get_u16(input.data + 4U) != MEDIA_CODEC_VERSION ||
        !bytes_zero(input.data + 129U, 7U) ||
        !bytes_zero(input.data + 204U, 4U) ||
        !bytes_zero(input.data + 224U, 16U)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.binding.session_id = decode_session_id(input.data + 8U);
    value.binding.catalog_generation = get_u64(input.data + 40U);
    value.binding.endpoint_id.high = get_u64(input.data + 48U);
    value.binding.endpoint_id.low = get_u64(input.data + 56U);
    value.binding.endpoint_incarnation = get_u64(input.data + 64U);
    value.binding.resource_id.high = get_u64(input.data + 72U);
    value.binding.resource_id.low = get_u64(input.data + 80U);
    if (endpoint_equal(value.binding.endpoint_id, RENDER_ENDPOINT)) {
        value.binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    } else if (endpoint_equal(value.binding.endpoint_id, CAMERA_ENDPOINT)) {
        value.binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    } else {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    value.binding.production_epoch = get_u64(input.data + 88U);
    value.binding.media_stream_epoch = get_u64(input.data + 96U);
    value.frame_sequence = get_u64(input.data + 104U);
    value.source_time.clock_epoch = get_u64(input.data + 112U);
    value.source_time.monotonic_ns = get_u64(input.data + 120U);
    value.applied_scene_owner.role = input.data[128];
    value.applied_scene_owner.incarnation = get_u64(input.data + 136U);
    value.applied_scene_state_version = get_u64(input.data + 144U);
    value.active_plan_generation = get_u64(input.data + 152U);
    value.world_generation = get_u64(input.data + 160U);
    value.dependency_sequence = get_u64(input.data + 168U);
    value.binding.channel_nonce.high = get_u64(input.data + 176U);
    value.binding.channel_nonce.low = get_u64(input.data + 184U);
    value.access_unit_bytes = get_u32(input.data + 192U);
    value.fragment_offset = get_u32(input.data + 196U);
    value.fragment.data = input.data +
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
    value.fragment.size = fragment_size;
    value.producer_dropped_frames = get_u64(input.data + 208U);
    value.pixel_format = get_u16(input.data + 216U);
    value.codec = get_u16(input.data + 218U);
    value.width = get_u16(input.data + 220U);
    value.height = get_u16(input.data + 222U);
    value.flags = get_u16(input.data + 6U);
    if (!frame_valid(&value)) {
        return MEISO_COMPOSITION_SESSION_V3_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SESSION_V3_OK;
}
