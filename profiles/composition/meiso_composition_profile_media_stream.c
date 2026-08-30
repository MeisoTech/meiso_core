#include "meiso_composition_profile_private.h"

#include <limits.h>

#include <string.h>

/* Media channel open/binding and fragmented frame Stream codecs. */

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
        &meiso_composition_profile_private_render_endpoint : source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ?
            &meiso_composition_profile_private_camera_endpoint : NULL;
}

static const MeisoResourceDescriptor *data_for_source(
    MeisoCompositionMediaSource source
)
{
    return source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ?
        &meiso_composition_profile_private_render_resources[2] : source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ?
            &meiso_composition_profile_private_camera_resources[2] : NULL;
}

static bool binding_valid(const MeisoCompositionMediaBinding *value)
{
    const MeisoEndpointId *endpoint;
    const MeisoResourceDescriptor *resource;
    if (value == NULL || !meiso_composition_profile_private_media_source_valid(value->source)) {
        return false;
    }
    endpoint = endpoint_for_source(value->source);
    resource = data_for_source(value->source);
    return meiso_session_nonce_valid(value->session_id.host_nonce) &&
        meiso_session_nonce_valid(value->session_id.edge_nonce) &&
        value->catalog_generation != 0U && value->endpoint_incarnation != 0U &&
        value->production_epoch != 0U && value->media_stream_epoch != 0U &&
        meiso_session_nonce_valid(value->channel_nonce) && endpoint != NULL &&
        resource != NULL && meiso_composition_profile_private_endpoint_equal(value->endpoint_id, *endpoint) &&
        meiso_composition_profile_private_resource_equal(value->resource_id, resource->id);
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

static void encode_session_id(uint8_t *out, MeisoSessionId id)
{
    meiso_composition_profile_private_put_u64(out, id.host_nonce.high);
    meiso_composition_profile_private_put_u64(out + 8U, id.host_nonce.low);
    meiso_composition_profile_private_put_u64(out + 16U, id.edge_nonce.high);
    meiso_composition_profile_private_put_u64(out + 24U, id.edge_nonce.low);
}

static MeisoSessionId decode_session_id(const uint8_t *in)
{
    MeisoSessionId id;
    id.host_nonce.high = meiso_composition_profile_private_get_u64(in);
    id.host_nonce.low = meiso_composition_profile_private_get_u64(in + 8U);
    id.edge_nonce.high = meiso_composition_profile_private_get_u64(in + 16U);
    id.edge_nonce.low = meiso_composition_profile_private_get_u64(in + 24U);
    return id;
}

MeisoCompositionProfileStatus meiso_composition_media_open_encode(
    const MeisoCompositionMediaOpen *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), media_open_valid(value), output,
        MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE);
    meiso_composition_profile_private_put_u32(output.data, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_OPEN_MAGIC);
    meiso_composition_profile_private_put_u16(output.data + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    output.data[6] = MEISO_SESSION_ROLE_HOST;
    encode_session_id(output.data + 8U, value->session_id);
    meiso_composition_profile_private_put_u64(output.data + 40U, value->catalog_generation);
    *out_size = MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_open_decode(
    MeisoByteView input,
    MeisoCompositionMediaOpen *out_value
)
{
    MeisoCompositionMediaOpen value;
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_OPEN_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_OPEN_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION ||
        input.data[6] != MEISO_SESSION_ROLE_HOST || input.data[7] != 0U ||
        !meiso_composition_profile_private_bytes_zero(input.data + 48U, 16U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    value.session_id = decode_session_id(input.data + 8U);
    value.catalog_generation = meiso_composition_profile_private_get_u64(input.data + 40U);
    if (!media_open_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_binding_encode(
    const MeisoCompositionMediaBinding *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), binding_valid(value), output,
        MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE);
    meiso_composition_profile_private_put_u32(output.data, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_BIND_MAGIC);
    meiso_composition_profile_private_put_u16(output.data + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    encode_session_id(output.data + 8U, value->session_id);
    meiso_composition_profile_private_put_u64(output.data + 40U, value->catalog_generation);
    meiso_composition_profile_private_put_id(output.data + 48U, value->endpoint_id.high, value->endpoint_id.low);
    meiso_composition_profile_private_put_u64(output.data + 64U, value->endpoint_incarnation);
    meiso_composition_profile_private_put_id(output.data + 72U, value->resource_id.high, value->resource_id.low);
    output.data[88] = value->source;
    output.data[89] = MEISO_RUNTIME_FACET_DATA;
    output.data[90] = MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA;
    meiso_composition_profile_private_put_u64(output.data + 96U, value->production_epoch);
    meiso_composition_profile_private_put_u64(output.data + 104U, value->media_stream_epoch);
    meiso_composition_profile_private_put_u64(output.data + 112U, value->channel_nonce.high);
    meiso_composition_profile_private_put_u64(output.data + 120U, value->channel_nonce.low);
    *out_size = MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_binding_decode(
    MeisoByteView input,
    MeisoCompositionMediaBinding *out_value
)
{
    MeisoCompositionMediaBinding value;
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_BIND_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_BIND_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION ||
        !meiso_composition_profile_private_bytes_zero(input.data + 6U, 2U) ||
        input.data[89] != MEISO_RUNTIME_FACET_DATA ||
        input.data[90] != MEISO_COMPOSITION_DELIVERY_EXTERNAL_MEDIA ||
        !meiso_composition_profile_private_bytes_zero(input.data + 91U, 5U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.session_id = decode_session_id(input.data + 8U);
    value.catalog_generation = meiso_composition_profile_private_get_u64(input.data + 40U);
    value.endpoint_id.high = meiso_composition_profile_private_get_u64(input.data + 48U);
    value.endpoint_id.low = meiso_composition_profile_private_get_u64(input.data + 56U);
    value.endpoint_incarnation = meiso_composition_profile_private_get_u64(input.data + 64U);
    value.resource_id.high = meiso_composition_profile_private_get_u64(input.data + 72U);
    value.resource_id.low = meiso_composition_profile_private_get_u64(input.data + 80U);
    value.source = input.data[88];
    value.production_epoch = meiso_composition_profile_private_get_u64(input.data + 96U);
    value.media_stream_epoch = meiso_composition_profile_private_get_u64(input.data + 104U);
    value.channel_nonce.high = meiso_composition_profile_private_get_u64(input.data + 112U);
    value.channel_nonce.low = meiso_composition_profile_private_get_u64(input.data + 120U);
    if (!binding_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_frame_encode(
    const MeisoCompositionMediaFrame *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    uint8_t header[MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE];
    size_t required = MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
    MeisoCompositionProfileStatus status;
    if (out_size == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (value != NULL && value->fragment.size <=
            SIZE_MAX - MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        required += value->fragment.size;
    }
    if (value != NULL && out_size != NULL &&
        (meiso_composition_profile_private_ranges_overlap(value, sizeof(*value), value->fragment.data,
                        value->fragment.size) ||
         meiso_composition_profile_private_ranges_overlap(out_size, sizeof(*out_size), value->fragment.data,
                        value->fragment.size))) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), frame_valid(value), output, required, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_ranges_overlap(output.data, required, value->fragment.data,
                       value->fragment.size)) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    memset(header, 0, sizeof(header));
    meiso_composition_profile_private_put_u32(header, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_FRAME_MAGIC);
    meiso_composition_profile_private_put_u16(header + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    meiso_composition_profile_private_put_u16(header + 6U, value->flags);
    encode_session_id(header + 8U, value->binding.session_id);
    meiso_composition_profile_private_put_u64(header + 40U, value->binding.catalog_generation);
    meiso_composition_profile_private_put_id(header + 48U, value->binding.endpoint_id.high,
           value->binding.endpoint_id.low);
    meiso_composition_profile_private_put_u64(header + 64U, value->binding.endpoint_incarnation);
    meiso_composition_profile_private_put_id(header + 72U, value->binding.resource_id.high,
           value->binding.resource_id.low);
    meiso_composition_profile_private_put_u64(header + 88U, value->binding.production_epoch);
    meiso_composition_profile_private_put_u64(header + 96U, value->binding.media_stream_epoch);
    meiso_composition_profile_private_put_u64(header + 104U, value->frame_sequence);
    meiso_composition_profile_private_put_u64(header + 112U, value->source_time.clock_epoch);
    meiso_composition_profile_private_put_u64(header + 120U, value->source_time.monotonic_ns);
    header[128] = value->applied_scene_owner.role;
    meiso_composition_profile_private_put_u64(header + 136U, value->applied_scene_owner.incarnation);
    meiso_composition_profile_private_put_u64(header + 144U, value->applied_scene_state_version);
    meiso_composition_profile_private_put_u64(header + 152U, value->active_plan_generation);
    meiso_composition_profile_private_put_u64(header + 160U, value->world_generation);
    meiso_composition_profile_private_put_u64(header + 168U, value->dependency_sequence);
    meiso_composition_profile_private_put_u64(header + 176U, value->binding.channel_nonce.high);
    meiso_composition_profile_private_put_u64(header + 184U, value->binding.channel_nonce.low);
    meiso_composition_profile_private_put_u32(header + 192U, value->access_unit_bytes);
    meiso_composition_profile_private_put_u32(header + 196U, value->fragment_offset);
    meiso_composition_profile_private_put_u32(header + 200U, (uint32_t)value->fragment.size);
    meiso_composition_profile_private_put_u64(header + 208U, value->producer_dropped_frames);
    meiso_composition_profile_private_put_u16(header + 216U, value->pixel_format);
    meiso_composition_profile_private_put_u16(header + 218U, value->codec);
    meiso_composition_profile_private_put_u16(header + 220U, value->width);
    meiso_composition_profile_private_put_u16(header + 222U, value->height);
    memmove(output.data, header, sizeof(header));
    memmove(output.data + sizeof(header), value->fragment.data,
            value->fragment.size);
    *out_size = required;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_frame_decode(
    MeisoByteView input,
    MeisoCompositionMediaFrame *out_value
)
{
    MeisoCompositionMediaFrame value;
    uint32_t fragment_size;
    if (input.data == NULL || out_value == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_composition_profile_private_ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_PROFILE_TRUNCATED;
    }
    fragment_size = meiso_composition_profile_private_get_u32(input.data + 200U);
#if SIZE_MAX < UINT32_MAX
    if ((size_t)fragment_size >
            SIZE_MAX - MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
#endif
    if (input.size < MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE +
            (size_t)fragment_size) {
        return MEISO_COMPOSITION_PROFILE_TRUNCATED;
    }
    if (input.size > MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE +
            (size_t)fragment_size ||
        meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_FRAME_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION ||
        !meiso_composition_profile_private_bytes_zero(input.data + 129U, 7U) ||
        !meiso_composition_profile_private_bytes_zero(input.data + 204U, 4U) ||
        !meiso_composition_profile_private_bytes_zero(input.data + 224U, 16U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.binding.session_id = decode_session_id(input.data + 8U);
    value.binding.catalog_generation = meiso_composition_profile_private_get_u64(input.data + 40U);
    value.binding.endpoint_id.high = meiso_composition_profile_private_get_u64(input.data + 48U);
    value.binding.endpoint_id.low = meiso_composition_profile_private_get_u64(input.data + 56U);
    value.binding.endpoint_incarnation = meiso_composition_profile_private_get_u64(input.data + 64U);
    value.binding.resource_id.high = meiso_composition_profile_private_get_u64(input.data + 72U);
    value.binding.resource_id.low = meiso_composition_profile_private_get_u64(input.data + 80U);
    if (meiso_composition_profile_private_endpoint_equal(value.binding.endpoint_id, meiso_composition_profile_private_render_endpoint)) {
        value.binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    } else if (meiso_composition_profile_private_endpoint_equal(value.binding.endpoint_id, meiso_composition_profile_private_camera_endpoint)) {
        value.binding.source = MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
    } else {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    value.binding.production_epoch = meiso_composition_profile_private_get_u64(input.data + 88U);
    value.binding.media_stream_epoch = meiso_composition_profile_private_get_u64(input.data + 96U);
    value.frame_sequence = meiso_composition_profile_private_get_u64(input.data + 104U);
    value.source_time.clock_epoch = meiso_composition_profile_private_get_u64(input.data + 112U);
    value.source_time.monotonic_ns = meiso_composition_profile_private_get_u64(input.data + 120U);
    value.applied_scene_owner.role = input.data[128];
    value.applied_scene_owner.incarnation = meiso_composition_profile_private_get_u64(input.data + 136U);
    value.applied_scene_state_version = meiso_composition_profile_private_get_u64(input.data + 144U);
    value.active_plan_generation = meiso_composition_profile_private_get_u64(input.data + 152U);
    value.world_generation = meiso_composition_profile_private_get_u64(input.data + 160U);
    value.dependency_sequence = meiso_composition_profile_private_get_u64(input.data + 168U);
    value.binding.channel_nonce.high = meiso_composition_profile_private_get_u64(input.data + 176U);
    value.binding.channel_nonce.low = meiso_composition_profile_private_get_u64(input.data + 184U);
    value.access_unit_bytes = meiso_composition_profile_private_get_u32(input.data + 192U);
    value.fragment_offset = meiso_composition_profile_private_get_u32(input.data + 196U);
    value.fragment.data = input.data +
        MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE;
    value.fragment.size = fragment_size;
    value.producer_dropped_frames = meiso_composition_profile_private_get_u64(input.data + 208U);
    value.pixel_format = meiso_composition_profile_private_get_u16(input.data + 216U);
    value.codec = meiso_composition_profile_private_get_u16(input.data + 218U);
    value.width = meiso_composition_profile_private_get_u16(input.data + 220U);
    value.height = meiso_composition_profile_private_get_u16(input.data + 222U);
    value.flags = meiso_composition_profile_private_get_u16(input.data + 6U);
    if (!frame_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}
