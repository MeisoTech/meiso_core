#include "meiso_drm_scanout_private.h"

#include <limits.h>
#include <string.h>

bool meiso_k230d_drm_private_frame_contract(
    const MeisoK230dDrmFrameView *frame,
    MeisoK230dDrmAdmissionReason *out_reason
)
{
    uint32_t bytes_per_pixel;
    uint64_t expected_size;
    size_t index;

    if (frame == NULL || out_reason == NULL) {
        return false;
    }
    if (frame->format == MEISO_K230D_DRM_INPUT_RGBA8888) {
        bytes_per_pixel = 4U;
    } else if (frame->format == MEISO_K230D_DRM_INPUT_RGB565_BE) {
        bytes_per_pixel = 2U;
    } else {
        *out_reason = MEISO_K230D_DRM_ADMISSION_INPUT_FORMAT;
        return false;
    }
    if (frame->bytes == NULL || frame->width == 0U || frame->height == 0U ||
        frame->width > MEISO_K230D_DRM_MAX_DIMENSION ||
        frame->height > MEISO_K230D_DRM_MAX_DIMENSION) {
        *out_reason = MEISO_K230D_DRM_ADMISSION_INPUT_ENVELOPE;
        return false;
    }
    expected_size = (uint64_t)frame->width * (uint64_t)frame->height *
        bytes_per_pixel;
    if (expected_size == 0U ||
        expected_size > MEISO_K230D_DRM_MAX_INPUT_BYTES ||
        expected_size > (uint64_t)SIZE_MAX ||
        frame->size != (size_t)expected_size) {
        *out_reason = MEISO_K230D_DRM_ADMISSION_INPUT_ENVELOPE;
        return false;
    }
    if (frame->format == MEISO_K230D_DRM_INPUT_RGBA8888) {
        for (index = 3U; index < frame->size; index += 4U) {
            if (frame->bytes[index] != UINT8_C(255)) {
                *out_reason = MEISO_K230D_DRM_ADMISSION_INPUT_NOT_OPAQUE;
                return false;
            }
        }
    }
    *out_reason = MEISO_K230D_DRM_ADMISSION_NONE;
    return true;
}

static uint16_t source_pixel(
    const MeisoK230dDrmFrameView *frame,
    uint32_t x,
    uint32_t y
)
{
    const size_t pixel_index = (size_t)y * frame->width + x;
    if (frame->format == MEISO_K230D_DRM_INPUT_RGB565_BE) {
        const uint8_t *pixel = frame->bytes + pixel_index * 2U;
        return (uint16_t)(((uint16_t)pixel[0] << 8U) | pixel[1]);
    }
    {
        const uint8_t *pixel = frame->bytes + pixel_index * 4U;
        return (uint16_t)(
            ((uint16_t)(pixel[0] >> 3U) << 11U) |
            ((uint16_t)(pixel[1] >> 2U) << 5U) |
            (uint16_t)(pixel[2] >> 3U)
        );
    }
}

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
)
{
    uint64_t minimum_pitch;
    uint64_t visible_bytes;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t viewport_x;
    uint32_t viewport_y;
    uint32_t y;

    if (frame == NULL || output == NULL || out_viewport_x == NULL ||
        out_viewport_y == NULL || out_viewport_width == NULL ||
        out_viewport_height == NULL || output_width == 0U ||
        output_height == 0U ||
        output_width > MEISO_K230D_DRM_MAX_DIMENSION ||
        output_height > MEISO_K230D_DRM_MAX_DIMENSION ||
        output_size == 0U || output_size > MEISO_K230D_DRM_MAX_DUMB_BYTES ||
        output_size > (uint64_t)SIZE_MAX) {
        return false;
    }
    minimum_pitch = (uint64_t)output_width * UINT64_C(2);
    visible_bytes = (uint64_t)output_pitch * output_height;
    if ((uint64_t)output_pitch < minimum_pitch || visible_bytes > output_size ||
        visible_bytes == 0U) {
        return false;
    }

    if ((uint64_t)output_width * frame->height <=
        (uint64_t)output_height * frame->width) {
        viewport_width = output_width;
        viewport_height = (uint32_t)(
            ((uint64_t)frame->height * output_width) / frame->width
        );
    } else {
        viewport_height = output_height;
        viewport_width = (uint32_t)(
            ((uint64_t)frame->width * output_height) / frame->height
        );
    }
    if (viewport_width == 0U || viewport_height == 0U) {
        return false;
    }
    viewport_x = (output_width - viewport_width) / 2U;
    viewport_y = (output_height - viewport_height) / 2U;
    memset(output, 0, (size_t)output_size);
    for (y = 0U; y < viewport_height; ++y) {
        const uint32_t source_y = (uint32_t)(
            ((uint64_t)y * frame->height) / viewport_height
        );
        uint8_t *row = output +
            (size_t)(viewport_y + y) * output_pitch +
            (size_t)viewport_x * 2U;
        uint32_t x;
        for (x = 0U; x < viewport_width; ++x) {
            const uint32_t source_x = (uint32_t)(
                ((uint64_t)x * frame->width) / viewport_width
            );
            const uint16_t pixel = source_pixel(frame, source_x, source_y);
            row[(size_t)x * 2U] = (uint8_t)pixel;
            row[(size_t)x * 2U + 1U] = (uint8_t)(pixel >> 8U);
        }
    }
    *out_viewport_x = viewport_x;
    *out_viewport_y = viewport_y;
    *out_viewport_width = viewport_width;
    *out_viewport_height = viewport_height;
    return true;
}
