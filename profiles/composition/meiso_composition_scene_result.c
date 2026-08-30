#include "meiso_composition_scene_private.h"

#include <string.h>

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 144U +
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE <= 8192U,
    "RenderResult Stream frame must fit the selected Runtime payload limit"
);
enum {

    RESULT_MAGIC = 0x52525330U,

    CODEC_WIRE_VERSION = 1U

};

/* Render-result validation and fixed-size canonical stream codec. */

static bool render_result_valid(
    const MeisoCompositionSceneRenderResult *value
)
{
    const uint32_t required_evidence =
        MEISO_COMPOSITION_SCENE_EVIDENCE_OFFSCREEN_COMPLETE |
        MEISO_COMPOSITION_SCENE_EVIDENCE_THUMBNAIL_INCLUDED;

    if (value == NULL ||
        value->applied_scene_owner_incarnation == 0U ||
        value->applied_scene_state_version == 0U ||
        value->active_plan_generation == 0U ||
        value->world_generation == 0U ||
        value->result_code != MEISO_COMPOSITION_SCENE_RESULT_COMPLETE ||
        (value->evidence_flags &
            (MeisoCompositionSceneEvidenceFlags)
                ~MEISO_COMPOSITION_SCENE_EVIDENCE_ALL) != 0U ||
        (value->evidence_flags & required_evidence) != required_evidence ||
        value->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS ||
        value->unique_mesh_count >
            MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES ||
        value->unique_mesh_count > value->object_count ||
        value->source_clock_epoch == 0U ||
        value->render_end_monotonic_ns <
            value->render_start_monotonic_ns ||
        !meiso_composition_scene_private_bytes_nonzero(value->full_target_sha256,
                       MEISO_SHA256_DIGEST_SIZE) ||
        value->target_width != MEISO_COMPOSITION_SCENE_TARGET_WIDTH ||
        value->target_height != MEISO_COMPOSITION_SCENE_TARGET_HEIGHT ||
        value->color_format != MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 ||
        value->depth_format !=
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 ||
        value->logical_color_row_bytes !=
            MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U ||
        value->logical_depth_row_bytes !=
            MEISO_COMPOSITION_SCENE_TARGET_WIDTH * 4U ||
        value->thumbnail_width !=
            MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH ||
        value->thumbnail_height !=
            MEISO_COMPOSITION_SCENE_THUMBNAIL_HEIGHT ||
        value->thumbnail_format != MEISO_COMPOSITION_SCENE_COLOR_RGB565 ||
        value->thumbnail_stride_bytes !=
            MEISO_COMPOSITION_SCENE_THUMBNAIL_WIDTH *
                MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTES_PER_PIXEL ||
        value->thumbnail_byte_size !=
            MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE) {
        return false;
    }
    if ((value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED) != 0U &&
        (value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_FENCE_COMPLETE) == 0U) {
        return false;
    }
    if ((value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_VBLANK_OBSERVED) != 0U &&
        (value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_DISPLAY_COMMIT_ACCEPTED) == 0U) {
        return false;
    }
    if ((value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_PHYSICAL_VERIFIED) != 0U &&
        (value->evidence_flags &
            MEISO_COMPOSITION_SCENE_EVIDENCE_VBLANK_OBSERVED) == 0U) {
        return false;
    }
    return true;
}

size_t meiso_composition_scene_render_result_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_render_result_encode(
    const MeisoCompositionSceneRenderResult *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, render_result_valid(value), output,
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE);
    meiso_composition_scene_private_write_u32(output.data, RESULT_MAGIC);
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u16(output.data + 6U, value->result_code);
    meiso_composition_scene_private_write_u32(output.data + 8U, value->evidence_flags);
    meiso_composition_scene_private_write_u16(output.data + 12U, (uint16_t)value->object_count);
    meiso_composition_scene_private_write_u16(output.data + 14U, (uint16_t)value->unique_mesh_count);
    meiso_composition_scene_private_write_u64(output.data + 16U,
              value->applied_scene_owner_incarnation);
    meiso_composition_scene_private_write_u64(output.data + 24U, value->applied_scene_state_version);
    meiso_composition_scene_private_write_u64(output.data + 32U, value->active_plan_generation);
    meiso_composition_scene_private_write_u64(output.data + 40U, value->world_generation);
    meiso_composition_scene_private_write_u64(output.data + 48U, value->source_clock_epoch);
    meiso_composition_scene_private_write_u64(output.data + 56U, value->render_start_monotonic_ns);
    meiso_composition_scene_private_write_u64(output.data + 64U, value->render_end_monotonic_ns);
    meiso_composition_scene_private_write_u16(output.data + 72U, value->target_width);
    meiso_composition_scene_private_write_u16(output.data + 74U, value->target_height);
    meiso_composition_scene_private_write_u16(output.data + 76U, value->color_format);
    meiso_composition_scene_private_write_u16(output.data + 78U, value->depth_format);
    meiso_composition_scene_private_write_u32(output.data + 80U, value->logical_color_row_bytes);
    meiso_composition_scene_private_write_u32(output.data + 84U, value->logical_depth_row_bytes);
    memcpy(output.data + 88U, value->full_target_sha256,
           MEISO_SHA256_DIGEST_SIZE);
    meiso_composition_scene_private_write_u16(output.data + 120U, value->thumbnail_width);
    meiso_composition_scene_private_write_u16(output.data + 122U, value->thumbnail_height);
    meiso_composition_scene_private_write_u16(output.data + 124U, value->thumbnail_format);
    meiso_composition_scene_private_write_u32(output.data + 128U, value->thumbnail_stride_bytes);
    meiso_composition_scene_private_write_u32(output.data + 132U, value->thumbnail_byte_size);
    memcpy(output.data +
               MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE,
           value->thumbnail_rgb565,
           MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE);
    *out_size = MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_render_result_decode(
    MeisoByteView input,
    MeisoCompositionSceneRenderResult *out_value
)
{
    MeisoCompositionSceneRenderResult value;
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE, out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != RESULT_MAGIC ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !meiso_composition_scene_private_bytes_zero(input.data + 126U, 2U) ||
        !meiso_composition_scene_private_bytes_zero(input.data + 136U, 24U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.result_code = meiso_composition_scene_private_read_u16(input.data + 6U);
    value.evidence_flags = meiso_composition_scene_private_read_u32(input.data + 8U);
    value.object_count = meiso_composition_scene_private_read_u16(input.data + 12U);
    value.unique_mesh_count = meiso_composition_scene_private_read_u16(input.data + 14U);
    value.applied_scene_owner_incarnation = meiso_composition_scene_private_read_u64(input.data + 16U);
    value.applied_scene_state_version = meiso_composition_scene_private_read_u64(input.data + 24U);
    value.active_plan_generation = meiso_composition_scene_private_read_u64(input.data + 32U);
    value.world_generation = meiso_composition_scene_private_read_u64(input.data + 40U);
    value.source_clock_epoch = meiso_composition_scene_private_read_u64(input.data + 48U);
    value.render_start_monotonic_ns = meiso_composition_scene_private_read_u64(input.data + 56U);
    value.render_end_monotonic_ns = meiso_composition_scene_private_read_u64(input.data + 64U);
    value.target_width = meiso_composition_scene_private_read_u16(input.data + 72U);
    value.target_height = meiso_composition_scene_private_read_u16(input.data + 74U);
    value.color_format = meiso_composition_scene_private_read_u16(input.data + 76U);
    value.depth_format = meiso_composition_scene_private_read_u16(input.data + 78U);
    value.logical_color_row_bytes = meiso_composition_scene_private_read_u32(input.data + 80U);
    value.logical_depth_row_bytes = meiso_composition_scene_private_read_u32(input.data + 84U);
    memcpy(value.full_target_sha256, input.data + 88U,
           MEISO_SHA256_DIGEST_SIZE);
    value.thumbnail_width = meiso_composition_scene_private_read_u16(input.data + 120U);
    value.thumbnail_height = meiso_composition_scene_private_read_u16(input.data + 122U);
    value.thumbnail_format = meiso_composition_scene_private_read_u16(input.data + 124U);
    value.thumbnail_stride_bytes = meiso_composition_scene_private_read_u32(input.data + 128U);
    value.thumbnail_byte_size = meiso_composition_scene_private_read_u32(input.data + 132U);
    if (!render_result_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    memcpy(value.thumbnail_rgb565,
           input.data +
               MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE,
           MEISO_COMPOSITION_SCENE_THUMBNAIL_BYTE_SIZE);
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}
