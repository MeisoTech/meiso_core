#include "meiso_composition_scene_private.h"

#include <limits.h>

#include <string.h>

_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE <= 8192U,
    "SceneDesired State frame must fit the selected Runtime payload limit"
);
enum {

    SCENE_MAGIC = 0x53434e30U,

    CODEC_WIRE_VERSION = 1U,

    CAMERA_WIRE_SIZE = 96U,

    CONTENT_ID_WIRE_SIZE = 36U

};

/* Scene desired-state validation and variable-size canonical codec. */

static bool camera_valid(const MeisoCompositionSceneCamera *value)
{
    return value != NULL &&
        value->space == MEISO_COMPOSITION_SCENE_SPACE_WORLD &&
        value->space_epoch != 0U &&
        meiso_time_point_valid(value->source_time) &&
        meiso_composition_scene_private_canonical_float_array(value->position, 3U) &&
        meiso_composition_scene_private_canonical_quaternion(value->orientation_xyzw) &&
        value->projection ==
            MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE &&
        meiso_composition_scene_private_canonical_float_array(&value->vertical_fov_degrees, 1U) &&
        meiso_composition_scene_private_canonical_float_array(&value->near_z, 1U) &&
        meiso_composition_scene_private_canonical_float_array(&value->far_z, 1U) &&
        value->vertical_fov_degrees >= 1.0F &&
        value->vertical_fov_degrees <= 179.0F &&
        value->near_z > 0.0F && value->far_z > value->near_z &&
        value->viewport_x == 0U && value->viewport_y == 0U &&
        value->viewport_width == MEISO_COMPOSITION_SCENE_TARGET_WIDTH &&
        value->viewport_height == MEISO_COMPOSITION_SCENE_TARGET_HEIGHT;
}

static bool transform_valid(const MeisoCompositionSceneTransform *value)
{
    size_t axis;

    if (value == NULL || !meiso_composition_scene_private_canonical_float_array(value->position, 3U) ||
        !meiso_composition_scene_private_canonical_quaternion(value->orientation_xyzw) ||
        !meiso_composition_scene_private_canonical_float_array(value->scale, 3U)) {
        return false;
    }
    for (axis = 0U; axis < 3U; ++axis) {
        if (value->scale[axis] <= 0.0F) {
            return false;
        }
    }
    return true;
}

static bool object_valid(const MeisoCompositionSceneObject *value)
{
    return value != NULL && meiso_composition_scene_private_object_id_valid(value->id) &&
        meiso_content_id_valid(&value->mesh_content_id) &&
        value->space == MEISO_COMPOSITION_SCENE_SPACE_WORLD &&
        value->space_epoch != 0U &&
        transform_valid(&value->transform) &&
        (value->flags & (MeisoCompositionSceneObjectFlags)
            ~MEISO_COMPOSITION_SCENE_OBJECT_VISIBLE) == 0U;
}

static bool desired_valid(const MeisoCompositionSceneDesired *value)
{
    uint32_t index;
    uint32_t unique_mesh_count = 0U;

    if (value == NULL || value->flags !=
            MEISO_COMPOSITION_SCENE_FLAGS_NONE ||
        !camera_valid(&value->camera) ||
        value->object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS) {
        return false;
    }
    for (index = 0U; index < value->object_count; ++index) {
        uint32_t previous;
        bool mesh_seen = false;

        if (!object_valid(&value->objects[index]) ||
            value->objects[index].space_epoch !=
                value->camera.space_epoch) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (meiso_composition_scene_private_object_id_equal(value->objects[index].id,
                                value->objects[previous].id)) {
                return false;
            }
            if (meiso_composition_scene_private_content_id_equal(&value->objects[index].mesh_content_id,
                                 &value->objects[previous].mesh_content_id)) {
                mesh_seen = true;
            }
        }
        if (!mesh_seen) {
            ++unique_mesh_count;
            if (unique_mesh_count >
                MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
                return false;
            }
        }
    }
    return true;
}

static void write_content_id(uint8_t *destination, const MeisoContentId *value)
{
    meiso_composition_scene_private_write_u16(destination, value->algorithm);
    destination[2] = value->digest_size;
    destination[3] = 0U;
    memcpy(destination + 4U, value->digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool read_content_id(
    const uint8_t *source,
    MeisoContentId *out_value
)
{
    MeisoContentId value;

    if (source == NULL || out_value == NULL || source[3] != 0U) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.algorithm = meiso_composition_scene_private_read_u16(source);
    value.digest_size = source[2];
    memcpy(value.digest, source + 4U, MEISO_SHA256_DIGEST_SIZE);
    if (!meiso_content_id_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static void encode_camera(
    uint8_t output[CAMERA_WIRE_SIZE],
    const MeisoCompositionSceneCamera *value
)
{
    size_t axis;

    output[0] = value->space;
    output[1] = value->projection;
    meiso_composition_scene_private_write_u64(output + 8U, value->space_epoch);
    meiso_composition_scene_private_write_u64(output + 16U, value->source_time.clock_epoch);
    meiso_composition_scene_private_write_u64(output + 24U, value->source_time.monotonic_ns);
    for (axis = 0U; axis < 3U; ++axis) {
        (void)meiso_composition_scene_private_write_float(output + 32U + axis * 4U,
                          value->position[axis]);
    }
    for (axis = 0U; axis < 4U; ++axis) {
        (void)meiso_composition_scene_private_write_float(output + 44U + axis * 4U,
                          value->orientation_xyzw[axis]);
    }
    (void)meiso_composition_scene_private_write_float(output + 60U, value->vertical_fov_degrees);
    (void)meiso_composition_scene_private_write_float(output + 64U, value->near_z);
    (void)meiso_composition_scene_private_write_float(output + 68U, value->far_z);
    meiso_composition_scene_private_write_u16(output + 72U, value->viewport_x);
    meiso_composition_scene_private_write_u16(output + 74U, value->viewport_y);
    meiso_composition_scene_private_write_u16(output + 76U, value->viewport_width);
    meiso_composition_scene_private_write_u16(output + 78U, value->viewport_height);
}

static bool decode_camera(
    const uint8_t input[CAMERA_WIRE_SIZE],
    MeisoCompositionSceneCamera *out_value
)
{
    MeisoCompositionSceneCamera value;
    size_t axis;

    if (!meiso_composition_scene_private_bytes_zero(input + 2U, 6U) ||
        !meiso_composition_scene_private_bytes_zero(input + 80U, 16U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.space = input[0];
    value.projection = input[1];
    value.space_epoch = meiso_composition_scene_private_read_u64(input + 8U);
    value.source_time.clock_epoch = meiso_composition_scene_private_read_u64(input + 16U);
    value.source_time.monotonic_ns = meiso_composition_scene_private_read_u64(input + 24U);
    for (axis = 0U; axis < 3U; ++axis) {
        if (!meiso_composition_scene_private_read_float(input + 32U + axis * 4U,
                        &value.position[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 4U; ++axis) {
        if (!meiso_composition_scene_private_read_float(input + 44U + axis * 4U,
                        &value.orientation_xyzw[axis])) {
            return false;
        }
    }
    if (!meiso_composition_scene_private_read_float(input + 60U, &value.vertical_fov_degrees) ||
        !meiso_composition_scene_private_read_float(input + 64U, &value.near_z) ||
        !meiso_composition_scene_private_read_float(input + 68U, &value.far_z)) {
        return false;
    }
    value.viewport_x = meiso_composition_scene_private_read_u16(input + 72U);
    value.viewport_y = meiso_composition_scene_private_read_u16(input + 74U);
    value.viewport_width = meiso_composition_scene_private_read_u16(input + 76U);
    value.viewport_height = meiso_composition_scene_private_read_u16(input + 78U);
    if (!camera_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

static void encode_object(
    uint8_t output[MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE],
    const MeisoCompositionSceneObject *value
)
{
    size_t axis;

    meiso_composition_scene_private_write_u64(output, value->id.high);
    meiso_composition_scene_private_write_u64(output + 8U, value->id.low);
    write_content_id(output + 16U, &value->mesh_content_id);
    output[52U] = value->space;
    output[53U] = value->flags;
    meiso_composition_scene_private_write_u64(output + 56U, value->space_epoch);
    for (axis = 0U; axis < 3U; ++axis) {
        (void)meiso_composition_scene_private_write_float(output + 64U + axis * 4U,
                          value->transform.position[axis]);
    }
    for (axis = 0U; axis < 4U; ++axis) {
        (void)meiso_composition_scene_private_write_float(output + 76U + axis * 4U,
                          value->transform.orientation_xyzw[axis]);
    }
    for (axis = 0U; axis < 3U; ++axis) {
        (void)meiso_composition_scene_private_write_float(output + 92U + axis * 4U,
                          value->transform.scale[axis]);
    }
}

static bool decode_object(
    const uint8_t input[MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE],
    MeisoCompositionSceneObject *out_value
)
{
    MeisoCompositionSceneObject value;
    size_t axis;

    if (!meiso_composition_scene_private_bytes_zero(input + 54U, 2U) ||
        !meiso_composition_scene_private_bytes_zero(input + 104U, 8U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.id.high = meiso_composition_scene_private_read_u64(input);
    value.id.low = meiso_composition_scene_private_read_u64(input + 8U);
    if (!read_content_id(input + 16U, &value.mesh_content_id)) {
        return false;
    }
    value.space = input[52U];
    value.flags = input[53U];
    value.space_epoch = meiso_composition_scene_private_read_u64(input + 56U);
    for (axis = 0U; axis < 3U; ++axis) {
        if (!meiso_composition_scene_private_read_float(input + 64U + axis * 4U,
                        &value.transform.position[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 4U; ++axis) {
        if (!meiso_composition_scene_private_read_float(input + 76U + axis * 4U,
                        &value.transform.orientation_xyzw[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 3U; ++axis) {
        if (!meiso_composition_scene_private_read_float(input + 92U + axis * 4U,
                        &value.transform.scale[axis])) {
            return false;
        }
    }
    if (!object_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_encoded_size(
    uint32_t object_count,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (object_count > MEISO_COMPOSITION_SCENE_MAX_OBJECTS) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    *out_size = MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +
        (size_t)object_count * MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

size_t meiso_composition_scene_desired_max_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_SCENE_MAX_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_encode(
    const MeisoCompositionSceneDesired *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    size_t required = 0U;
    const bool valid = desired_valid(value) &&
        meiso_composition_scene_desired_encoded_size(
            value->object_count, &required
        ) == MEISO_COMPOSITION_SCENE_CODEC_OK;
    MeisoCompositionSceneCodecStatus status = meiso_composition_scene_private_prepare_encode(
        value, valid, output, required, out_size
    );
    uint32_t index;

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (meiso_composition_scene_private_ranges_overlap(value, sizeof(*value), output.data, required)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0, required);
    meiso_composition_scene_private_write_u32(output.data, SCENE_MAGIC);
    meiso_composition_scene_private_write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    meiso_composition_scene_private_write_u16(output.data + 6U, value->flags);
    meiso_composition_scene_private_write_u32(output.data + 8U, value->object_count);
    encode_camera(output.data + 16U, &value->camera);
    for (index = 0U; index < value->object_count; ++index) {
        encode_object(
            output.data + MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +
                (size_t)index * MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE,
            &value->objects[index]
        );
    }
    *out_size = required;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_desired_decode(
    MeisoByteView input,
    MeisoCompositionSceneDesired *out_value
)
{
    MeisoCompositionSceneCamera camera;
    uint32_t object_count;
    size_t required;
    uint32_t index;
    uint32_t unique_mesh_count = 0U;

    if (out_value == NULL || input.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (meiso_composition_scene_private_read_u32(input.data) != SCENE_MAGIC ||
        meiso_composition_scene_private_read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        meiso_composition_scene_private_read_u16(input.data + 6U) != MEISO_COMPOSITION_SCENE_FLAGS_NONE ||
        !meiso_composition_scene_private_bytes_zero(input.data + 12U, 4U) ||
        !decode_camera(input.data + 16U, &camera)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    object_count = meiso_composition_scene_private_read_u32(input.data + 8U);
    if (meiso_composition_scene_desired_encoded_size(
            object_count, &required
        ) != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    if (input.size < required) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (input.size > required) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    if (meiso_composition_scene_private_ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }

    for (index = 0U; index < object_count; ++index) {
        const uint8_t *record = input.data +
            MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +
            (size_t)index * MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE;
        MeisoCompositionSceneObject object;
        uint32_t previous;
        bool mesh_seen = false;

        if (!decode_object(record, &object) ||
            object.space_epoch != camera.space_epoch) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
        for (previous = 0U; previous < index; ++previous) {
            const uint8_t *previous_record = input.data +
                MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +
                (size_t)previous *
                    MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE;
            MeisoCompositionSceneObject previous_object;

            if (!decode_object(previous_record, &previous_object)) {
                return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
            }
            if (meiso_composition_scene_private_object_id_equal(object.id, previous_object.id)) {
                return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
            }
            if (meiso_composition_scene_private_content_id_equal(&object.mesh_content_id,
                                 &previous_object.mesh_content_id)) {
                mesh_seen = true;
            }
        }
        if (!mesh_seen && ++unique_mesh_count >
                MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
    }

    memset(out_value, 0, sizeof(*out_value));
    out_value->flags = MEISO_COMPOSITION_SCENE_FLAGS_NONE;
    out_value->camera = camera;
    out_value->object_count = object_count;
    for (index = 0U; index < object_count; ++index) {
        (void)decode_object(
            input.data + MEISO_COMPOSITION_SCENE_SCENE_HEADER_WIRE_SIZE +
                (size_t)index * MEISO_COMPOSITION_SCENE_OBJECT_WIRE_SIZE,
            &out_value->objects[index]
        );
    }
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}
