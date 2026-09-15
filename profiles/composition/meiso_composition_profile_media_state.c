#include "meiso_composition_profile_private.h"

#include <string.h>

/* Media capability and desired/actual State validation and codecs. */

bool meiso_composition_profile_private_media_source_valid(MeisoCompositionMediaSource source)
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
    return value != NULL && meiso_composition_profile_private_media_source_valid(value->source) &&
        pixel_mask_valid(value->pixel_formats) &&
        codec_mask_valid(value->codecs) &&
        (!(value->codecs & MEISO_COMPOSITION_MEDIA_CODEC_MASK_JPEG) ||
         (value->source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
          (value->pixel_formats & MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888))) && value->min_width != 0U &&
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
        (value->codec == MEISO_COMPOSITION_MEDIA_CODEC_RAW ||
         (value->codec == MEISO_COMPOSITION_MEDIA_CODEC_JPEG &&
          value->pixel_format == MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888)) &&
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
    if (desired->codec == MEISO_COMPOSITION_MEDIA_CODEC_JPEG) access_unit_bytes = 1U;
    return (capability->pixel_formats & pixel_bit) != 0U &&
        (capability->codecs & (UINT32_C(1) << (desired->codec - 1U))) != 0U &&
        desired->width >= capability->min_width &&
        desired->width <= capability->max_width &&
        desired->height >= capability->min_height &&
        desired->height <= capability->max_height &&
        desired->fps_millihz <= capability->max_fps_millihz &&
        access_unit_bytes <= capability->max_access_unit_bytes;
}

static bool media_actual_valid(const MeisoCompositionMediaActual *value)
{
    return value != NULL && meiso_composition_profile_private_media_source_valid(value->source) &&
        media_desired_valid(&value->effective) &&
        value->media_stream_epoch != 0U &&
        meiso_session_nonce_valid(value->channel_nonce) &&
        value->queue_depth <=
            MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES +
                MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES;
}

MeisoCompositionProfileStatus meiso_composition_media_capability_encode(
    const MeisoCompositionMediaCapability *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), media_capability_valid(value), output,
        MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE);
    meiso_composition_profile_private_put_u32(output.data, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CAPABILITY_MAGIC);
    meiso_composition_profile_private_put_u16(output.data + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    output.data[6] = value->source;
    meiso_composition_profile_private_put_u32(output.data + 8U, value->pixel_formats);
    meiso_composition_profile_private_put_u32(output.data + 12U, value->codecs);
    meiso_composition_profile_private_put_u16(output.data + 16U, value->min_width);
    meiso_composition_profile_private_put_u16(output.data + 18U, value->min_height);
    meiso_composition_profile_private_put_u16(output.data + 20U, value->max_width);
    meiso_composition_profile_private_put_u16(output.data + 22U, value->max_height);
    meiso_composition_profile_private_put_u32(output.data + 24U, value->max_fps_millihz);
    meiso_composition_profile_private_put_u32(output.data + 28U, value->max_access_unit_bytes);
    meiso_composition_profile_private_put_u32(output.data + 32U, value->max_fragment_payload_bytes);
    meiso_composition_profile_private_put_u16(output.data + 36U, MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES);
    meiso_composition_profile_private_put_u16(output.data + 38U, MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES);
    meiso_composition_profile_private_put_u32(output.data + 40U, value->max_bitrate_bps);
    *out_size = MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_capability_decode(
    MeisoByteView input,
    MeisoCompositionMediaCapability *out_value
)
{
    MeisoCompositionMediaCapability value;
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_CAPABILITY_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CAPABILITY_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION || input.data[7] != 0U ||
        meiso_composition_profile_private_get_u16(input.data + 36U) !=
            MEISO_COMPOSITION_MEDIA_MAX_IN_FLIGHT_FRAMES ||
        meiso_composition_profile_private_get_u16(input.data + 38U) !=
            MEISO_COMPOSITION_MEDIA_MAX_LATEST_FRAMES ||
        !meiso_composition_profile_private_bytes_zero(input.data + 44U, 20U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.source = input.data[6];
    value.pixel_formats = meiso_composition_profile_private_get_u32(input.data + 8U);
    value.codecs = meiso_composition_profile_private_get_u32(input.data + 12U);
    value.min_width = meiso_composition_profile_private_get_u16(input.data + 16U);
    value.min_height = meiso_composition_profile_private_get_u16(input.data + 18U);
    value.max_width = meiso_composition_profile_private_get_u16(input.data + 20U);
    value.max_height = meiso_composition_profile_private_get_u16(input.data + 22U);
    value.max_fps_millihz = meiso_composition_profile_private_get_u32(input.data + 24U);
    value.max_access_unit_bytes = meiso_composition_profile_private_get_u32(input.data + 28U);
    value.max_fragment_payload_bytes = meiso_composition_profile_private_get_u32(input.data + 32U);
    value.max_bitrate_bps = meiso_composition_profile_private_get_u32(input.data + 40U);
    if (!media_capability_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_desired_encode(
    const MeisoCompositionMediaDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), media_desired_valid(value), output,
        MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE);
    meiso_composition_profile_private_put_u32(output.data, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_DESIRED_MAGIC);
    meiso_composition_profile_private_put_u16(output.data + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    meiso_composition_profile_private_put_u16(output.data + 8U, value->pixel_format);
    meiso_composition_profile_private_put_u16(output.data + 10U, value->codec);
    meiso_composition_profile_private_put_u16(output.data + 12U, value->width);
    meiso_composition_profile_private_put_u16(output.data + 14U, value->height);
    meiso_composition_profile_private_put_u32(output.data + 16U, value->fps_millihz);
    meiso_composition_profile_private_put_u32(output.data + 20U, value->bitrate_bps);
    meiso_composition_profile_private_put_u32(output.data + 24U, value->gop_frames);
    meiso_composition_profile_private_put_u16(output.data + 28U, value->recovery_flags);
    *out_size = MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_desired_decode(
    MeisoByteView input,
    MeisoCompositionMediaDesired *out_value
)
{
    MeisoCompositionMediaDesired value;
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_DESIRED_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_DESIRED_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION ||
        !meiso_composition_profile_private_bytes_zero(input.data + 6U, 2U) ||
        !meiso_composition_profile_private_bytes_zero(input.data + 30U, 2U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.pixel_format = meiso_composition_profile_private_get_u16(input.data + 8U);
    value.codec = meiso_composition_profile_private_get_u16(input.data + 10U);
    value.width = meiso_composition_profile_private_get_u16(input.data + 12U);
    value.height = meiso_composition_profile_private_get_u16(input.data + 14U);
    value.fps_millihz = meiso_composition_profile_private_get_u32(input.data + 16U);
    value.bitrate_bps = meiso_composition_profile_private_get_u32(input.data + 20U);
    value.gop_frames = meiso_composition_profile_private_get_u32(input.data + 24U);
    value.recovery_flags = meiso_composition_profile_private_get_u16(input.data + 28U);
    if (!media_desired_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_actual_encode(
    const MeisoCompositionMediaActual *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_encode(
        value, sizeof(*value), media_actual_valid(value), output,
        MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE, out_size
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    memset(output.data, 0, MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE);
    meiso_composition_profile_private_put_u32(output.data, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_ACTUAL_MAGIC);
    meiso_composition_profile_private_put_u16(output.data + 4U, MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION);
    output.data[6] = value->source;
    meiso_composition_profile_private_put_u16(output.data + 8U, value->effective.pixel_format);
    meiso_composition_profile_private_put_u16(output.data + 10U, value->effective.codec);
    meiso_composition_profile_private_put_u16(output.data + 12U, value->effective.width);
    meiso_composition_profile_private_put_u16(output.data + 14U, value->effective.height);
    meiso_composition_profile_private_put_u32(output.data + 16U, value->effective.fps_millihz);
    meiso_composition_profile_private_put_u32(output.data + 20U, value->effective.bitrate_bps);
    meiso_composition_profile_private_put_u32(output.data + 24U, value->effective.gop_frames);
    meiso_composition_profile_private_put_u16(output.data + 28U, value->effective.recovery_flags);
    meiso_composition_profile_private_put_u64(output.data + 32U, value->media_stream_epoch);
    meiso_composition_profile_private_put_u64(output.data + 40U, value->channel_nonce.high);
    meiso_composition_profile_private_put_u64(output.data + 48U, value->channel_nonce.low);
    meiso_composition_profile_private_put_u64(output.data + 56U, value->producer_dropped_frames);
    meiso_composition_profile_private_put_u32(output.data + 64U, value->queue_depth);
    meiso_composition_profile_private_put_u32(output.data + 68U, value->recovery_generation);
    *out_size = MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE;
    return MEISO_COMPOSITION_PROFILE_OK;
}

MeisoCompositionProfileStatus meiso_composition_media_actual_decode(
    MeisoByteView input,
    MeisoCompositionMediaActual *out_value
)
{
    MeisoCompositionMediaActual value;
    MeisoCompositionProfileStatus status = meiso_composition_profile_private_prepare_decode(
        input, MEISO_COMPOSITION_MEDIA_ACTUAL_WIRE_SIZE, out_value,
        sizeof(*out_value)
    );
    if (status != MEISO_COMPOSITION_PROFILE_OK) {
        return status;
    }
    if (meiso_composition_profile_private_get_u32(input.data) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_ACTUAL_MAGIC ||
        meiso_composition_profile_private_get_u16(input.data + 4U) != MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION || input.data[7] != 0U ||
        !meiso_composition_profile_private_bytes_zero(input.data + 30U, 2U) ||
        !meiso_composition_profile_private_bytes_zero(input.data + 72U, 8U)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.source = input.data[6];
    value.effective.pixel_format = meiso_composition_profile_private_get_u16(input.data + 8U);
    value.effective.codec = meiso_composition_profile_private_get_u16(input.data + 10U);
    value.effective.width = meiso_composition_profile_private_get_u16(input.data + 12U);
    value.effective.height = meiso_composition_profile_private_get_u16(input.data + 14U);
    value.effective.fps_millihz = meiso_composition_profile_private_get_u32(input.data + 16U);
    value.effective.bitrate_bps = meiso_composition_profile_private_get_u32(input.data + 20U);
    value.effective.gop_frames = meiso_composition_profile_private_get_u32(input.data + 24U);
    value.effective.recovery_flags = meiso_composition_profile_private_get_u16(input.data + 28U);
    value.media_stream_epoch = meiso_composition_profile_private_get_u64(input.data + 32U);
    value.channel_nonce.high = meiso_composition_profile_private_get_u64(input.data + 40U);
    value.channel_nonce.low = meiso_composition_profile_private_get_u64(input.data + 48U);
    value.producer_dropped_frames = meiso_composition_profile_private_get_u64(input.data + 56U);
    value.queue_depth = meiso_composition_profile_private_get_u32(input.data + 64U);
    value.recovery_generation = meiso_composition_profile_private_get_u32(input.data + 68U);
    if (!media_actual_valid(&value)) {
        return MEISO_COMPOSITION_PROFILE_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_PROFILE_OK;
}
