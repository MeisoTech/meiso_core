#include "meiso_composition_scene_private.h"

#include <limits.h>

#include <string.h>

enum {

    MESH_MAGIC = 0x4d534830U,

    CODEC_WIRE_VERSION = 1U,

    MESH_FLAG_VERTEX_COLOR = 1U,

    MESH_KNOWN_FLAGS = MESH_FLAG_VERTEX_COLOR

};

/* Canonical mesh encoding, borrowed mesh views, and cube fixture data. */

static const MeisoCompositionSceneMeshVertex CUBE_VERTICES[8] = {
    {{-0.5F, -0.5F, -0.5F}, {0U, 0U, 0U, 0U}},
    {{ 0.5F, -0.5F, -0.5F}, {0U, 0U, 0U, 0U}},
    {{ 0.5F,  0.5F, -0.5F}, {0U, 0U, 0U, 0U}},
    {{-0.5F,  0.5F, -0.5F}, {0U, 0U, 0U, 0U}},
    {{-0.5F, -0.5F,  0.5F}, {0U, 0U, 0U, 0U}},
    {{ 0.5F, -0.5F,  0.5F}, {0U, 0U, 0U, 0U}},
    {{ 0.5F,  0.5F,  0.5F}, {0U, 0U, 0U, 0U}},
    {{-0.5F,  0.5F,  0.5F}, {0U, 0U, 0U, 0U}}
};

static const uint32_t CUBE_INDICES[36] = {
    0U, 2U, 1U, 0U, 3U, 2U,
    4U, 5U, 6U, 4U, 6U, 7U,
    0U, 1U, 5U, 0U, 5U, 4U,
    3U, 7U, 6U, 3U, 6U, 2U,
    0U, 4U, 7U, 0U, 7U, 3U,
    1U, 2U, 6U, 1U, 6U, 5U
};

static const MeisoCompositionSceneMesh CANONICAL_CUBE = {
    false,
    8U,
    CUBE_VERTICES,
    36U,
    CUBE_INDICES
};

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_encoded_size(
    uint32_t vertex_count,
    bool has_vertex_color,
    uint32_t index_count,
    size_t *out_size
)
{
    size_t vertex_bytes;
    size_t index_bytes;
    size_t total;
    const size_t stride = has_vertex_color ?
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE :
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE;

    if (out_size == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (vertex_count == 0U || index_count == 0U ||
        index_count % 3U != 0U ||
        !meiso_composition_scene_private_size_multiply(vertex_count, stride, &vertex_bytes) ||
        !meiso_composition_scene_private_size_multiply(index_count, sizeof(uint32_t), &index_bytes) ||
        !meiso_composition_scene_private_size_add(MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE,
                  vertex_bytes, &total) ||
        !meiso_composition_scene_private_size_add(total, index_bytes, &total) ||
        total > MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    *out_size = total;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

size_t meiso_composition_scene_mesh_max_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
}

static bool mesh_vertex_valid(
    const MeisoCompositionSceneMeshVertex *vertex,
    bool has_color
)
{
    return vertex != NULL &&
        meiso_composition_scene_private_canonical_float_array(vertex->position, 3U) &&
        (has_color || meiso_composition_scene_private_bytes_zero(vertex->color_rgba, 4U));
}

static void bounds_include(
    const float position[3],
    float bounds_min[3],
    float bounds_max[3],
    bool first
)
{
    size_t axis;

    for (axis = 0U; axis < 3U; ++axis) {
        if (first || position[axis] < bounds_min[axis]) {
            bounds_min[axis] = position[axis];
        }
        if (first || position[axis] > bounds_max[axis]) {
            bounds_max[axis] = position[axis];
        }
    }
}

static bool mesh_valid(
    const MeisoCompositionSceneMesh *value,
    size_t *out_size,
    float bounds_min[3],
    float bounds_max[3]
)
{
    size_t size;
    uint32_t index;

    if (value == NULL || out_size == NULL || bounds_min == NULL ||
        bounds_max == NULL || value->vertices == NULL ||
        value->indices == NULL ||
        meiso_composition_scene_mesh_encoded_size(
            value->vertex_count, value->has_vertex_color,
            value->index_count, &size
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return false;
    }
    for (index = 0U; index < value->vertex_count; ++index) {
        if (!mesh_vertex_valid(&value->vertices[index],
                               value->has_vertex_color)) {
            return false;
        }
        bounds_include(value->vertices[index].position,
                       bounds_min, bounds_max, index == 0U);
    }
    for (index = 0U; index < value->index_count; ++index) {
        if (value->indices[index] >= value->vertex_count) {
            return false;
        }
    }
    *out_size = size;
    return true;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_encode(
    const MeisoCompositionSceneMesh *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    float bounds_min[3] = {0.0F, 0.0F, 0.0F};
    float bounds_max[3] = {0.0F, 0.0F, 0.0F};
    size_t required = 0U;
    size_t vertex_bytes;
    size_t index_offset;
    const bool valid = mesh_valid(
        value, &required, bounds_min, bounds_max
    );
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, valid, output, required, out_size
    );
    uint32_t index;
    const size_t stride = value != NULL && value->has_vertex_color ?
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE :
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE;

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    /* Success means mesh_valid already proved the borrowed mesh is present. */
    if (value == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    vertex_bytes = (size_t)value->vertex_count * stride;
    index_offset = MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
        vertex_bytes;
    if (meiso_composition_scene_private_ranges_overlap(output.data, required, value, sizeof(*value)) ||
        meiso_composition_scene_private_ranges_overlap(output.data, required, value->vertices,
                       (size_t)value->vertex_count *
                           sizeof(*value->vertices)) ||
        meiso_composition_scene_private_ranges_overlap(output.data, required, value->indices,
                       (size_t)value->index_count * sizeof(*value->indices))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }

    memset(output.data, 0, required);
    meiso_composition_scene_private_write_u32(output.data, MESH_MAGIC);
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u16(output.data + 6U, value->has_vertex_color ?
              MESH_FLAG_VERTEX_COLOR : 0U);
    meiso_composition_scene_private_write_u32(output.data + 8U, value->vertex_count);
    meiso_composition_scene_private_write_u32(output.data + 12U, value->index_count);
    meiso_composition_scene_private_write_u32(output.data + 16U, (uint32_t)stride);
    meiso_composition_scene_private_write_u32(output.data + 20U, (uint32_t)vertex_bytes);
    meiso_composition_scene_private_write_u32(output.data + 24U,
              value->index_count * (uint32_t)sizeof(uint32_t));
    for (index = 0U; index < 3U; ++index) {
        (void)meiso_composition_scene_private_write_float(output.data + 32U + (size_t)index * 4U,
                          bounds_min[index]);
        (void)meiso_composition_scene_private_write_float(output.data + 44U + (size_t)index * 4U,
                          bounds_max[index]);
    }
    for (index = 0U; index < value->vertex_count; ++index) {
        uint8_t *record = output.data +
            MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
            (size_t)index * stride;
        size_t axis;

        for (axis = 0U; axis < 3U; ++axis) {
            (void)meiso_composition_scene_private_write_float(record + axis * 4U,
                              value->vertices[index].position[axis]);
        }
        if (value->has_vertex_color) {
            memcpy(record + 12U, value->vertices[index].color_rgba, 4U);
        }
    }
    for (index = 0U; index < value->index_count; ++index) {
        meiso_composition_scene_private_write_u32(output.data + index_offset + (size_t)index * 4U,
                  value->indices[index]);
    }
    *out_size = required;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool float_bits_equal(float value, uint32_t expected_bits)
{
    uint32_t actual_bits;

    return meiso_composition_scene_private_float_to_bits(value, &actual_bits) && actual_bits == expected_bits;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_decode(
    MeisoByteView input,
    MeisoCompositionSceneMeshView *out_value
)
{
    MeisoCompositionSceneMeshView value;
    uint16_t flags;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t stride;
    size_t required;
    size_t vertex_bytes;
    size_t index_offset;
    float calculated_min[3];
    float calculated_max[3];
    uint32_t index;

    if (out_value == NULL || input.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (meiso_composition_scene_private_ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != MESH_MAGIC ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    flags = meiso_composition_scene_private_read_u16(input.data + 6U);
    vertex_count = meiso_composition_scene_private_read_u32(input.data + 8U);
    index_count = meiso_composition_scene_private_read_u32(input.data + 12U);
    stride = meiso_composition_scene_private_read_u32(input.data + 16U);
    if ((flags & (uint16_t)~MESH_KNOWN_FLAGS) != 0U ||
        meiso_composition_scene_mesh_encoded_size(
            vertex_count, (flags & MESH_FLAG_VERTEX_COLOR) != 0U,
            index_count, &required
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    if (input.size < required) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (input.size > required) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    vertex_bytes = (size_t)vertex_count * stride;
    index_offset = MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
        vertex_bytes;
    if (stride != ((flags & MESH_FLAG_VERTEX_COLOR) != 0U ?
                  MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE :
                  MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE) ||
        meiso_composition_scene_private_read_u32(input.data + 20U) != vertex_bytes ||
        meiso_composition_scene_private_read_u32(input.data + 24U) != index_count * 4U ||
        !meiso_composition_scene_private_bytes_zero(input.data + 28U, 4U) ||
        !meiso_composition_scene_private_bytes_zero(input.data + 56U, 8U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    for (index = 0U; index < 3U; ++index) {
        if (!meiso_composition_scene_private_read_float(input.data + 32U + (size_t)index * 4U,
                        &value.bounds_min[index]) ||
            !meiso_composition_scene_private_read_float(input.data + 44U + (size_t)index * 4U,
                        &value.bounds_max[index])) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
    }
    for (index = 0U; index < vertex_count; ++index) {
        const uint8_t *record = input.data +
            MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
            (size_t)index * stride;
        float position[3];
        size_t axis;

        for (axis = 0U; axis < 3U; ++axis) {
            if (!meiso_composition_scene_private_read_float(record + axis * 4U, &position[axis])) {
                return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
            }
        }
        bounds_include(position, calculated_min, calculated_max,
                       index == 0U);
    }
    for (index = 0U; index < 3U; ++index) {
        if (!float_bits_equal(calculated_min[index],
                              meiso_composition_scene_private_read_u32(input.data + 32U + index * 4U)) ||
            !float_bits_equal(calculated_max[index],
                              meiso_composition_scene_private_read_u32(input.data + 44U + index * 4U))) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
    }
    for (index = 0U; index < index_count; ++index) {
        if (meiso_composition_scene_private_read_u32(input.data + index_offset + (size_t)index * 4U) >=
            vertex_count) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
    }

    value.has_vertex_color = (flags & MESH_FLAG_VERTEX_COLOR) != 0U;
    value.vertex_count = vertex_count;
    value.index_count = index_count;
    value.vertex_records.data = input.data +
        MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE;
    value.vertex_records.size = vertex_bytes;
    value.index_records.data = input.data + index_offset;
    value.index_records.size = (size_t)index_count * 4U;
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_view_vertex(
    const MeisoCompositionSceneMeshView *view,
    uint32_t index,
    MeisoCompositionSceneMeshVertex *out_vertex
)
{
    MeisoCompositionSceneMeshVertex value;
    const size_t stride = view != NULL && view->has_vertex_color ?
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE :
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE;
    const uint8_t *record;
    size_t axis;

    if (view == NULL || out_vertex == NULL ||
        view->vertex_records.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (index >= view->vertex_count ||
        view->vertex_records.size != (size_t)view->vertex_count * stride) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    record = view->vertex_records.data + (size_t)index * stride;
    memset(&value, 0, sizeof(value));
    for (axis = 0U; axis < 3U; ++axis) {
        if (!meiso_composition_scene_private_read_float(record + axis * 4U, &value.position[axis])) {
            return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
        }
    }
    if (view->has_vertex_color) {
        memcpy(value.color_rgba, record + 12U, 4U);
    }
    *out_vertex = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_mesh_view_index(
    const MeisoCompositionSceneMeshView *view,
    uint32_t index,
    uint32_t *out_vertex_index
)
{
    uint32_t value;

    if (view == NULL || out_vertex_index == NULL ||
        view->index_records.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (index >= view->index_count ||
        view->index_records.size != (size_t)view->index_count * 4U) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    value = meiso_composition_scene_private_read_u32(view->index_records.data + (size_t)index * 4U);
    if (value >= view->vertex_count) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    *out_vertex_index = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

const MeisoCompositionSceneMesh *meiso_composition_scene_canonical_cube(void)
{
    return &CANONICAL_CUBE;
}
