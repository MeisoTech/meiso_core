#include "composition_scene.h"

#include <float.h>
#include <limits.h>
#include <string.h>

#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128
#error "composition-scene-lab-v0 requires IEEE-754 binary32 float"
#endif

_Static_assert(sizeof(float) == sizeof(uint32_t),
               "composition-scene-lab-v0 requires 32-bit float");
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 144U +
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE <= 8192U,
    "RenderResult Stream frame must fit the selected Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE <= 8192U,
    "SceneDesired State frame must fit the selected Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE <=
            8192U,
    "PresentationPlan State frame must fit the Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE <=
            8192U,
    "PresentationStatus State frame must fit the Runtime payload limit"
);
_Static_assert(
    MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U +
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE <= 8192U,
    "SceneStatus State frame must fit the Runtime payload limit"
);

enum {
    MESH_MAGIC = 0x4d534830U,
    SCENE_MAGIC = 0x53434e30U,
    STATUS_MAGIC = 0x53535430U,
    RESULT_MAGIC = 0x52525330U,
    CODEC_WIRE_VERSION = 1U,
    MESH_FLAG_VERTEX_COLOR = 1U,
    MESH_KNOWN_FLAGS = MESH_FLAG_VERTEX_COLOR,
    CAMERA_WIRE_SIZE = 96U,
    CONTENT_ID_WIRE_SIZE = 36U,
    PRESENTATION_PLAN_KNOWN_FLAGS =
        MEISO_COMPOSITION_SCENE_PLAN_USE_SAFETY_FRAME
};

static const MeisoProfileRef COMPOSITE_PROFILE = {
    {
        UINT64_C(0x4b3233305f53434e),
        UINT64_C(0x5f4c41425f563030)
    },
    MEISO_COMPOSITION_SCENE_PROFILE_VERSION
};

static const MeisoEndpointId PRESENTATION_ENDPOINT = {
    UINT64_C(0x4b3233305f53434e),
    UINT64_C(0x50524553454e545f)
};

static const MeisoEndpointId SCENE_ENDPOINT = {
    UINT64_C(0x4b3233305f53434e),
    UINT64_C(0x5343454e455f5f5f)
};

static const MeisoSchemaRef PRESENTATION_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x4b3233305f534348),
        UINT64_C(0x504341505f563030)
    },
    1U
};

static const MeisoSchemaRef SCENE_CAPABILITY_SCHEMA = {
    {
        UINT64_C(0x4b3233305f534348),
        UINT64_C(0x534341505f563030)
    },
    1U
};

/*
 * Literal, catalog-embedded capability bytes. They are deliberately exposed
 * as views so conformance tests can freeze the complete catalog payload.
 */
static const uint8_t PRESENTATION_CAPABILITY[
    MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE
] = {
    0x50U, 0x43U, 0x41U, 0x50U, /* PCAP */
    0x00U, 0x01U,               /* version */
    0x00U, 0x01U,               /* one required layer */
    0x01U, 0x40U, 0x00U, 0xf0U, /* 320 x 240 */
    0x00U, 0x01U,               /* RGBA8888 */
    0x00U, 0x01U,               /* implementation-local depth32 */
    0x00U, 0x0aU,               /* max 10 Hz */
    0x00U, 0x00U,               /* reserved */
    0x00U, 0x00U, 0x13U, 0x88U, /* max 5000-ms offline lease */
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U
};

static const uint8_t SCENE_CAPABILITY[
    MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE
] = {
    0x53U, 0x43U, 0x41U, 0x50U, /* SCAP */
    0x00U, 0x01U, 0x00U, 0x00U, /* version, flags */
    0x00U, 0x00U, 0x00U, 0x20U, /* 32 objects */
    0x00U, 0x00U, 0x00U, 0x08U, /* 8 unique meshes */
    0x00U, 0x04U, 0x00U, 0x00U, /* 256 KiB Blob */
    0x00U, 0x00U, 0x00U, 0x10U, /* 16 published slots */
    0x00U, 0x00U, 0x00U, 0x01U, /* one staging transfer */
    0x00U, 0x00U, 0x10U, 0x00U, /* 4096-byte chunks */
    0x00U, 0x20U, 0x00U, 0x00U, /* 2 MiB per world */
    0x00U, 0x40U, 0x00U, 0x00U, /* 4 MiB total */
    0x00U, 0x40U, 0x00U, 0x30U, /* 64 x 48 thumbnail */
    0x00U, 0x02U, 0x0aU, 0x04U, /* RGB565, 10 Hz, 4 slots */
    0x02U, 0x02U, 0x00U, 0x00U  /* 2 jobs, 2 target sets, reserved */
};

static const MeisoResourceDescriptor RESOURCES[
    MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
] = {
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x504c414e5f444553)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x504c414e5f563030)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_PRESENTATION_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x504c414e5f414354)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x50535441545f3030)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_PRESENTATION_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x53434e5f44455300)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x5343454e455f3030)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x53434e5f41435400)
        },
        MEISO_RESOURCE_KIND_STATE,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x53535441545f3030)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    },
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x4d4553485f424c42)
        },
        MEISO_RESOURCE_KIND_BLOB,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x4d4553485f563030)
            },
            1U
        },
        MEISO_OWNER_ROLE_HOST,
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE
    },
    {
        {
            UINT64_C(0x4b3233305f53434e),
            UINT64_C(0x5245535f53545200)
        },
        MEISO_RESOURCE_KIND_STREAM,
        {
            {
                UINT64_C(0x4b3233305f534348),
                UINT64_C(0x525245535f563030)
            },
            1U
        },
        MEISO_OWNER_ROLE_EDGE,
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE
    }
};

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

static void write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

static void write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

static void write_u64(uint8_t *destination, uint64_t value)
{
    destination[0] = (uint8_t)(value >> 56U);
    destination[1] = (uint8_t)(value >> 48U);
    destination[2] = (uint8_t)(value >> 40U);
    destination[3] = (uint8_t)(value >> 32U);
    destination[4] = (uint8_t)(value >> 24U);
    destination[5] = (uint8_t)(value >> 16U);
    destination[6] = (uint8_t)(value >> 8U);
    destination[7] = (uint8_t)value;
}

static uint16_t read_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8U) | source[1]);
}

static uint32_t read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

static uint64_t read_u64(const uint8_t *source)
{
    return ((uint64_t)source[0] << 56U) |
        ((uint64_t)source[1] << 48U) |
        ((uint64_t)source[2] << 40U) |
        ((uint64_t)source[3] << 32U) |
        ((uint64_t)source[4] << 24U) |
        ((uint64_t)source[5] << 16U) |
        ((uint64_t)source[6] << 8U) |
        (uint64_t)source[7];
}

static bool bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
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
    const uintptr_t lhs_start = (uintptr_t)lhs;
    const uintptr_t rhs_start = (uintptr_t)rhs;
    uintptr_t lhs_end;
    uintptr_t rhs_end;

    if (lhs == NULL || rhs == NULL || lhs_size == 0U || rhs_size == 0U) {
        return false;
    }
    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static bool size_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (rhs != 0U && lhs > SIZE_MAX / rhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

static bool size_add(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool id128_equal(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl)
{
    return ah == bh && al == bl;
}

static bool endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

static bool schema_id_equal(MeisoSchemaId lhs, MeisoSchemaId rhs)
{
    return id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

static bool resource_id_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

static bool float_bits_canonical(uint32_t bits)
{
    const uint32_t exponent = bits & UINT32_C(0x7f800000);
    const uint32_t fraction = bits & UINT32_C(0x007fffff);

    return exponent != UINT32_C(0x7f800000) &&
        !(exponent == 0U && fraction != 0U) &&
        bits != UINT32_C(0x80000000);
}

static bool float_to_bits(float value, uint32_t *out_bits)
{
    uint32_t bits;

    if (out_bits == NULL) {
        return false;
    }
    memcpy(&bits, &value, sizeof(bits));
    if (!float_bits_canonical(bits)) {
        return false;
    }
    *out_bits = bits;
    return true;
}

static bool bits_to_float(uint32_t bits, float *out_value)
{
    if (out_value == NULL || !float_bits_canonical(bits)) {
        return false;
    }
    memcpy(out_value, &bits, sizeof(bits));
    return true;
}

static bool write_float(uint8_t *destination, float value)
{
    uint32_t bits;

    if (!float_to_bits(value, &bits)) {
        return false;
    }
    write_u32(destination, bits);
    return true;
}

static bool read_float(const uint8_t *source, float *out_value)
{
    return bits_to_float(read_u32(source), out_value);
}

static bool canonical_float_array(const float *values, size_t count)
{
    size_t index;
    uint32_t ignored;

    if (values == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (!float_to_bits(values[index], &ignored)) {
            return false;
        }
    }
    return true;
}

static bool canonical_quaternion(const float value[4])
{
    double norm;
    double delta;
    size_t index;

    if (!canonical_float_array(value, 4U)) {
        return false;
    }
    norm = 0.0;
    for (index = 0U; index < 4U; ++index) {
        norm += (double)value[index] * (double)value[index];
    }
    delta = norm >= 1.0 ? norm - 1.0 : 1.0 - norm;
    if (delta > (1.0 / 4096.0)) {
        return false;
    }
    if (value[3] > 0.0F) {
        return true;
    }
    if (value[3] < 0.0F) {
        return false;
    }
    for (index = 3U; index > 0U; --index) {
        if (value[index - 1U] > 0.0F) {
            return true;
        }
        if (value[index - 1U] < 0.0F) {
            return false;
        }
    }
    return false;
}

static bool object_id_valid(MeisoCompositionSceneObjectId value)
{
    return value.high != 0U || value.low != 0U;
}

static bool object_id_equal(
    MeisoCompositionSceneObjectId lhs,
    MeisoCompositionSceneObjectId rhs
)
{
    return id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

static bool content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->algorithm == rhs->algorithm &&
        lhs->digest_size == rhs->digest_size &&
        memcmp(lhs->digest, rhs->digest, MEISO_SHA256_DIGEST_SIZE) == 0;
}

static bool stable_reason_valid(MeisoCompositionSceneStableReason value)
{
    return value <= MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED;
}

static bool bytes_nonzero(const uint8_t *bytes, size_t size)
{
    return !bytes_zero(bytes, size);
}

static MeisoCompositionSceneCodecStatus prepare_encode(
    const void *value,
    bool value_valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (value == NULL || output.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (!value_valid) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    if (output.capacity < required) {
        return MEISO_COMPOSITION_SCENE_CODEC_CAPACITY;
    }
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static MeisoCompositionSceneCodecStatus prepare_fixed_decode(
    MeisoByteView input,
    size_t required,
    const void *out_value
);

static bool scene_status_valid(const MeisoCompositionSceneStatus *value)
{
    const uint64_t max_blob_bytes =
        (uint64_t)MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS *
        MEISO_COMPOSITION_SCENE_MAX_MESH_BLOB_SIZE;
    const uint64_t bytes_per_target_set =
        (uint64_t)MEISO_COMPOSITION_SCENE_TARGET_WIDTH *
        MEISO_COMPOSITION_SCENE_TARGET_HEIGHT * 4U * 2U;
    bool no_active_world;
    bool active_world;
    bool no_last_result;
    bool has_last_result;

    if (value == NULL) {
        return false;
    }
    no_active_world = value->active_plan_generation == 0U &&
        value->world_generation == 0U;
    active_world = value->active_plan_generation != 0U &&
        value->world_generation != 0U;
    no_last_result = value->last_result_stream_epoch == 0U &&
        value->last_result_sequence == 0U;
    has_last_result = value->last_result_stream_epoch != 0U &&
        value->last_result_sequence != 0U;

    if (!stable_reason_valid(value->reason) ||
        (!no_active_world && !active_world) ||
        (!no_last_result && !has_last_result) ||
        value->active_object_count >
            MEISO_COMPOSITION_SCENE_MAX_OBJECTS ||
        value->unique_mesh_count >
            MEISO_COMPOSITION_SCENE_MAX_UNIQUE_MESHES ||
        value->unique_mesh_count > value->active_object_count ||
        value->published_blob_count >
            MEISO_COMPOSITION_SCENE_MAX_PUBLISHED_BLOBS ||
        value->published_blob_bytes > max_blob_bytes ||
        value->resident_mesh_bytes >
            MEISO_COMPOSITION_SCENE_MAX_RESIDENT_MESH_BYTES_TOTAL ||
        value->target_slots_used >
            MEISO_COMPOSITION_SCENE_TARGET_SET_COUNT ||
        value->target_bytes >
            bytes_per_target_set * value->target_slots_used ||
        value->render_jobs_in_flight >
            MEISO_COMPOSITION_SCENE_MAX_RENDER_JOBS ||
        value->render_jobs_in_flight > value->target_slots_used ||
        value->result_queue_depth >
            MEISO_COMPOSITION_SCENE_RESULT_QUEUE_SLOTS) {
        return false;
    }
    if ((value->published_blob_count == 0U) !=
        (value->published_blob_bytes == 0U)) {
        return false;
    }
    if (no_active_world && (value->active_object_count != 0U ||
                            value->unique_mesh_count != 0U)) {
        return false;
    }
    return true;
}

size_t meiso_composition_scene_status_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_status_encode(
    const MeisoCompositionSceneStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, scene_status_valid(value), output,
        MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0, MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE);
    write_u32(output.data, STATUS_MAGIC);
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u16(output.data + 6U, value->reason);
    write_u64(output.data + 8U, value->active_plan_generation);
    write_u64(output.data + 16U, value->world_generation);
    write_u32(output.data + 24U, value->active_object_count);
    write_u32(output.data + 28U, value->unique_mesh_count);
    write_u32(output.data + 32U, value->published_blob_count);
    write_u32(output.data + 36U, value->target_slots_used);
    write_u64(output.data + 40U, value->published_blob_bytes);
    write_u64(output.data + 48U, value->resident_mesh_bytes);
    write_u64(output.data + 56U, value->target_bytes);
    write_u32(output.data + 64U, value->render_jobs_in_flight);
    write_u32(output.data + 68U, value->result_queue_depth);
    write_u64(output.data + 72U, value->last_result_stream_epoch);
    write_u64(output.data + 80U, value->last_result_sequence);
    *out_size = MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_status_decode(
    MeisoByteView input,
    MeisoCompositionSceneStatus *out_value
)
{
    MeisoCompositionSceneStatus value;
    MeisoCompositionSceneCodecStatus status = prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_STATUS_WIRE_SIZE, out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (read_u32(input.data) != STATUS_MAGIC ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !bytes_zero(input.data + 88U, 24U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.reason = read_u16(input.data + 6U);
    value.active_plan_generation = read_u64(input.data + 8U);
    value.world_generation = read_u64(input.data + 16U);
    value.active_object_count = read_u32(input.data + 24U);
    value.unique_mesh_count = read_u32(input.data + 28U);
    value.published_blob_count = read_u32(input.data + 32U);
    value.target_slots_used = read_u32(input.data + 36U);
    value.published_blob_bytes = read_u64(input.data + 40U);
    value.resident_mesh_bytes = read_u64(input.data + 48U);
    value.target_bytes = read_u64(input.data + 56U);
    value.render_jobs_in_flight = read_u32(input.data + 64U);
    value.result_queue_depth = read_u32(input.data + 68U);
    value.last_result_stream_epoch = read_u64(input.data + 72U);
    value.last_result_sequence = read_u64(input.data + 80U);
    if (!scene_status_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static MeisoCompositionSceneCodecStatus prepare_fixed_decode(
    MeisoByteView input,
    size_t required,
    const void *out_value
)
{
    if (out_value == NULL || input.data == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < required) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (input.size > required) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool plan_valid(const MeisoCompositionScenePresentationPlan *value)
{
    if (value == NULL ||
        (value->flags & (MeisoCompositionScenePlanFlags)
            ~PRESENTATION_PLAN_KNOWN_FLAGS) != 0U ||
        value->plan_generation == 0U ||
        value->capability_generation == 0U ||
        !bytes_nonzero(value->candidate_digest,
                       MEISO_SHA256_DIGEST_SIZE) ||
        !endpoint_id_equal(value->scene_binding, SCENE_ENDPOINT) ||
        value->source != MEISO_COMPOSITION_SCENE_LAYER_SOURCE_EDGE_NATIVE ||
        value->geometry != MEISO_COMPOSITION_SCENE_GEOMETRY_PROJECTION ||
        value->target_width != MEISO_COMPOSITION_SCENE_TARGET_WIDTH ||
        value->target_height != MEISO_COMPOSITION_SCENE_TARGET_HEIGHT ||
        value->color_format != MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 ||
        value->depth_format !=
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32 ||
        value->max_result_hz != MEISO_COMPOSITION_SCENE_MAX_RESULT_HZ ||
        value->offline_lease_ms >
            MEISO_COMPOSITION_SCENE_MAX_OFFLINE_LEASE_MS) {
        return false;
    }
    if (value->prepared_token_echo_present) {
        return bytes_nonzero(value->prepared_token_echo,
                             MEISO_SHA256_DIGEST_SIZE);
    }
    return bytes_zero(value->prepared_token_echo, MEISO_SHA256_DIGEST_SIZE);
}

static bool candidate_state_valid(MeisoCompositionSceneCandidateState value)
{
    return value >= MEISO_COMPOSITION_SCENE_CANDIDATE_NONE &&
        value <= MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED;
}

static bool active_state_valid(MeisoCompositionSceneActiveState value)
{
    return value >= MEISO_COMPOSITION_SCENE_ACTIVE_NONE &&
        value <= MEISO_COMPOSITION_SCENE_ACTIVE_SAFETY;
}

static bool presentation_reason_valid(
    MeisoCompositionScenePresentationReason value
)
{
    return value <=
        MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_RENDERER_FAILED;
}

static bool zero_generation_digest(
    uint64_t generation,
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    return generation == 0U && bytes_zero(digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool nonzero_generation_digest(
    uint64_t generation,
    const uint8_t digest[MEISO_SHA256_DIGEST_SIZE]
)
{
    return generation != 0U &&
        bytes_nonzero(digest, MEISO_SHA256_DIGEST_SIZE);
}

static bool presentation_status_valid(
    const MeisoCompositionScenePresentationStatus *value
)
{
    bool candidate_ref_valid;
    bool active_ref_valid;

    if (value == NULL || value->capability_generation == 0U ||
        !candidate_state_valid(value->candidate_state) ||
        !active_state_valid(value->active_state) ||
        !presentation_reason_valid(value->reason) ||
        !endpoint_id_equal(value->scene_binding, SCENE_ENDPOINT) ||
        value->target_width != MEISO_COMPOSITION_SCENE_TARGET_WIDTH ||
        value->target_height != MEISO_COMPOSITION_SCENE_TARGET_HEIGHT ||
        value->color_format != MEISO_COMPOSITION_SCENE_COLOR_RGBA8888 ||
        value->depth_format !=
            MEISO_COMPOSITION_SCENE_DEPTH_IMPLEMENTATION_32) {
        return false;
    }

    candidate_ref_valid = value->candidate_state ==
        MEISO_COMPOSITION_SCENE_CANDIDATE_NONE ?
        zero_generation_digest(value->candidate_plan_generation,
                               value->candidate_digest) :
        nonzero_generation_digest(value->candidate_plan_generation,
                                  value->candidate_digest);
    active_ref_valid = value->active_state ==
        MEISO_COMPOSITION_SCENE_ACTIVE_NONE ?
        zero_generation_digest(value->active_plan_generation,
                               value->active_digest) :
        nonzero_generation_digest(value->active_plan_generation,
                                  value->active_digest);
    if (!candidate_ref_valid || !active_ref_valid) {
        return false;
    }

    if (value->candidate_state ==
            MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
        value->candidate_state ==
            MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) {
        if (!value->prepared_token_present ||
            !bytes_nonzero(value->prepared_token,
                           MEISO_SHA256_DIGEST_SIZE) ||
            value->reason !=
                MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
            return false;
        }
    } else if (value->prepared_token_present ||
               !bytes_zero(value->prepared_token,
                           MEISO_SHA256_DIGEST_SIZE)) {
        return false;
    }

    if ((value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_REJECTED ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_FAILED) &&
        value->reason == MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
        return false;
    }
    if ((value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_NONE ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_PREPARED ||
         value->candidate_state ==
             MEISO_COMPOSITION_SCENE_CANDIDATE_ACTIVATION_PENDING) &&
        value->reason != MEISO_COMPOSITION_SCENE_PRESENTATION_REASON_NONE) {
        return false;
    }
    return true;
}

const MeisoProfileRef *meiso_composition_scene_profile(void)
{
    return &COMPOSITE_PROFILE;
}

const MeisoEndpointId *meiso_composition_scene_presentation_endpoint(void)
{
    return &PRESENTATION_ENDPOINT;
}

const MeisoEndpointId *meiso_composition_scene_scene_endpoint(void)
{
    return &SCENE_ENDPOINT;
}

const MeisoSchemaRef *
meiso_composition_scene_presentation_capability_schema(void)
{
    return &PRESENTATION_CAPABILITY_SCHEMA;
}

const MeisoSchemaRef *meiso_composition_scene_capability_schema(void)
{
    return &SCENE_CAPABILITY_SCHEMA;
}

MeisoByteView meiso_composition_scene_presentation_capability(void)
{
    MeisoByteView value;

    value.data = PRESENTATION_CAPABILITY;
    value.size = sizeof(PRESENTATION_CAPABILITY);
    return value;
}

MeisoByteView meiso_composition_scene_capability(void)
{
    MeisoByteView value;

    value.data = SCENE_CAPABILITY;
    value.size = sizeof(SCENE_CAPABILITY);
    return value;
}

const MeisoResourceDescriptor *meiso_composition_scene_resources(
    size_t *out_count
)
{
    if (out_count != NULL) {
        *out_count = MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
    }
    return RESOURCES;
}

const MeisoResourceDescriptor *meiso_composition_scene_resource(
    MeisoCompositionSceneResource resource
)
{
    if (resource >= MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
        return NULL;
    }
    return &RESOURCES[resource];
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_build_endpoints(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeEndpointDescriptor *output,
    size_t output_capacity,
    size_t *out_count
)
{
    if (out_count == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_count = 0U;
    if (output == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (presentation_incarnation == 0U || scene_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    if (output_capacity < MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT) {
        return MEISO_COMPOSITION_SCENE_CODEC_CAPACITY;
    }

    output[0].id = PRESENTATION_ENDPOINT;
    output[0].incarnation = presentation_incarnation;
    output[0].capability_schema = PRESENTATION_CAPABILITY_SCHEMA;
    output[0].capability = meiso_composition_scene_presentation_capability();
    output[1].id = SCENE_ENDPOINT;
    output[1].incarnation = scene_incarnation;
    output[1].capability_schema = SCENE_CAPABILITY_SCHEMA;
    output[1].capability = meiso_composition_scene_capability();
    *out_count = MEISO_COMPOSITION_SCENE_ENDPOINT_COUNT;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_build_routes(
    uint64_t presentation_incarnation,
    uint64_t scene_incarnation,
    MeisoRuntimeRouteDescriptor *output,
    size_t output_capacity,
    size_t *out_count
)
{
    static const MeisoRuntimeFacet facets[
        MEISO_COMPOSITION_SCENE_RESOURCE_COUNT
    ] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_IMMUTABLE,
        MEISO_RUNTIME_FACET_DATA
    };
    size_t index;

    if (out_count == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    *out_count = 0U;
    if (output == NULL) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (presentation_incarnation == 0U || scene_incarnation == 0U) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_VALUE;
    }
    if (output_capacity < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT) {
        return MEISO_COMPOSITION_SCENE_CODEC_CAPACITY;
    }

    for (index = 0U; index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
         ++index) {
        const bool presentation = index < 2U;

        output[index].endpoint_id = presentation ?
            PRESENTATION_ENDPOINT : SCENE_ENDPOINT;
        output[index].endpoint_incarnation = presentation ?
            presentation_incarnation : scene_incarnation;
        output[index].resource = RESOURCES[index];
        output[index].facet = facets[index];
    }
    *out_count = MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool schema_collides_with_internal(MeisoSchemaId schema)
{
    size_t index;

    if (schema_id_equal(schema, PRESENTATION_CAPABILITY_SCHEMA.id) ||
        schema_id_equal(schema, SCENE_CAPABILITY_SCHEMA.id)) {
        return true;
    }
    for (index = 0U; index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
         ++index) {
        if (schema_id_equal(schema, RESOURCES[index].schema.id)) {
            return true;
        }
    }
    return false;
}

bool meiso_composition_scene_catalog_identifiers_disjoint(
    const MeisoRuntimeEndpointDescriptor *external_endpoints,
    size_t external_endpoint_count,
    const MeisoRuntimeRouteDescriptor *external_routes,
    size_t external_route_count
)
{
    size_t index;

    if ((external_endpoint_count != 0U && external_endpoints == NULL) ||
        (external_route_count != 0U && external_routes == NULL)) {
        return false;
    }
    for (index = 0U; index < external_endpoint_count; ++index) {
        if (endpoint_id_equal(external_endpoints[index].id,
                              PRESENTATION_ENDPOINT) ||
            endpoint_id_equal(external_endpoints[index].id,
                              SCENE_ENDPOINT) ||
            schema_collides_with_internal(
                external_endpoints[index].capability_schema.id)) {
            return false;
        }
    }
    for (index = 0U; index < external_route_count; ++index) {
        size_t resource_index;

        if (endpoint_id_equal(external_routes[index].endpoint_id,
                              PRESENTATION_ENDPOINT) ||
            endpoint_id_equal(external_routes[index].endpoint_id,
                              SCENE_ENDPOINT) ||
            schema_collides_with_internal(
                external_routes[index].resource.schema.id)) {
            return false;
        }
        for (resource_index = 0U;
             resource_index < MEISO_COMPOSITION_SCENE_RESOURCE_COUNT;
             ++resource_index) {
            if (resource_id_equal(external_routes[index].resource.id,
                                  RESOURCES[resource_index].id)) {
                return false;
            }
        }
    }
    return true;
}

size_t meiso_composition_scene_presentation_plan_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_encode(
    const MeisoCompositionScenePresentationPlan *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, plan_valid(value), output,
        MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE);
    write_u32(output.data, UINT32_C(0x504c4e30)); /* PLN0 */
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u16(output.data + 6U, value->flags);
    write_u64(output.data + 8U, value->plan_generation);
    write_u64(output.data + 16U, value->capability_generation);
    memcpy(output.data + 24U, value->candidate_digest,
           MEISO_SHA256_DIGEST_SIZE);
    write_u64(output.data + 56U, value->scene_binding.high);
    write_u64(output.data + 64U, value->scene_binding.low);
    output.data[72U] = value->source;
    output.data[73U] = value->geometry;
    output.data[74U] = 1U; /* exactly one required layer */
    output.data[75U] = value->prepared_token_echo_present ? 1U : 0U;
    write_u16(output.data + 76U, value->target_width);
    write_u16(output.data + 78U, value->target_height);
    write_u16(output.data + 80U, value->color_format);
    write_u16(output.data + 82U, value->depth_format);
    write_u16(output.data + 84U, value->max_result_hz);
    write_u32(output.data + 88U, value->offline_lease_ms);
    memcpy(output.data + 96U, value->prepared_token_echo,
           MEISO_SHA256_DIGEST_SIZE);
    *out_size = MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_plan_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationPlan *out_value
)
{
    MeisoCompositionScenePresentationPlan value;
    MeisoCompositionSceneCodecStatus status = prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_PRESENTATION_PLAN_WIRE_SIZE,
        out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (read_u32(input.data) != UINT32_C(0x504c4e30) ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        input.data[74U] != 1U || input.data[75U] > 1U ||
        !bytes_zero(input.data + 86U, 2U) ||
        !bytes_zero(input.data + 92U, 4U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    value.flags = read_u16(input.data + 6U);
    value.plan_generation = read_u64(input.data + 8U);
    value.capability_generation = read_u64(input.data + 16U);
    memcpy(value.candidate_digest, input.data + 24U,
           MEISO_SHA256_DIGEST_SIZE);
    value.scene_binding.high = read_u64(input.data + 56U);
    value.scene_binding.low = read_u64(input.data + 64U);
    value.source = input.data[72U];
    value.geometry = input.data[73U];
    value.prepared_token_echo_present = input.data[75U] != 0U;
    value.target_width = read_u16(input.data + 76U);
    value.target_height = read_u16(input.data + 78U);
    value.color_format = read_u16(input.data + 80U);
    value.depth_format = read_u16(input.data + 82U);
    value.max_result_hz = read_u16(input.data + 84U);
    value.offline_lease_ms = read_u32(input.data + 88U);
    memcpy(value.prepared_token_echo, input.data + 96U,
           MEISO_SHA256_DIGEST_SIZE);
    if (!plan_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

size_t meiso_composition_scene_presentation_status_encoded_size(void)
{
    return MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_encode(
    const MeisoCompositionScenePresentationStatus *value,
    MeisoMutableBuffer output,
    size_t *out_size
)
{
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, presentation_status_valid(value), output,
        MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(
            value, sizeof(*value), output.data,
            MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE
        )) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE);
    write_u32(output.data, UINT32_C(0x50535430)); /* PST0 */
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u64(output.data + 8U, value->capability_generation);
    write_u64(output.data + 16U, value->candidate_plan_generation);
    memcpy(output.data + 24U, value->candidate_digest,
           MEISO_SHA256_DIGEST_SIZE);
    write_u64(output.data + 56U, value->active_plan_generation);
    memcpy(output.data + 64U, value->active_digest,
           MEISO_SHA256_DIGEST_SIZE);
    output.data[96U] = value->candidate_state;
    output.data[97U] = value->active_state;
    write_u16(output.data + 98U, value->reason);
    output.data[100U] = value->prepared_token_present ? 1U : 0U;
    memcpy(output.data + 104U, value->prepared_token,
           MEISO_SHA256_DIGEST_SIZE);
    write_u16(output.data + 136U, value->target_width);
    write_u16(output.data + 138U, value->target_height);
    write_u16(output.data + 140U, value->color_format);
    write_u16(output.data + 142U, value->depth_format);
    write_u64(output.data + 144U, value->scene_binding.high);
    write_u64(output.data + 152U, value->scene_binding.low);
    *out_size = MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

MeisoCompositionSceneCodecStatus
meiso_composition_scene_presentation_status_decode(
    MeisoByteView input,
    MeisoCompositionScenePresentationStatus *out_value
)
{
    MeisoCompositionScenePresentationStatus value;
    MeisoCompositionSceneCodecStatus status = prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_PRESENTATION_STATUS_WIRE_SIZE,
        out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (read_u32(input.data) != UINT32_C(0x50535430) ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !bytes_zero(input.data + 6U, 2U) || input.data[100U] > 1U ||
        !bytes_zero(input.data + 101U, 3U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    value.capability_generation = read_u64(input.data + 8U);
    value.candidate_plan_generation = read_u64(input.data + 16U);
    memcpy(value.candidate_digest, input.data + 24U,
           MEISO_SHA256_DIGEST_SIZE);
    value.active_plan_generation = read_u64(input.data + 56U);
    memcpy(value.active_digest, input.data + 64U,
           MEISO_SHA256_DIGEST_SIZE);
    value.candidate_state = input.data[96U];
    value.active_state = input.data[97U];
    value.reason = read_u16(input.data + 98U);
    value.prepared_token_present = input.data[100U] != 0U;
    memcpy(value.prepared_token, input.data + 104U,
           MEISO_SHA256_DIGEST_SIZE);
    value.target_width = read_u16(input.data + 136U);
    value.target_height = read_u16(input.data + 138U);
    value.color_format = read_u16(input.data + 140U);
    value.depth_format = read_u16(input.data + 142U);
    value.scene_binding.high = read_u64(input.data + 144U);
    value.scene_binding.low = read_u64(input.data + 152U);
    if (!presentation_status_valid(&value)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    *out_value = value;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

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
        !size_multiply(vertex_count, stride, &vertex_bytes) ||
        !size_multiply(index_count, sizeof(uint32_t), &index_bytes) ||
        !size_add(MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE,
                  vertex_bytes, &total) ||
        !size_add(total, index_bytes, &total) ||
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
        canonical_float_array(vertex->position, 3U) &&
        (has_color || bytes_zero(vertex->color_rgba, 4U));
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
    float bounds_min[3];
    float bounds_max[3];
    size_t required = 0U;
    size_t vertex_bytes;
    size_t index_offset;
    const bool valid = mesh_valid(
        value, &required, bounds_min, bounds_max
    );
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, valid, output, required, out_size
    );
    uint32_t index;
    const size_t stride = value != NULL && value->has_vertex_color ?
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_COLOR_WIRE_SIZE :
        MEISO_COMPOSITION_SCENE_MESH_VERTEX_POSITION_WIRE_SIZE;

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    vertex_bytes = (size_t)value->vertex_count * stride;
    index_offset = MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
        vertex_bytes;
    if (ranges_overlap(output.data, required, value, sizeof(*value)) ||
        ranges_overlap(output.data, required, value->vertices,
                       (size_t)value->vertex_count *
                           sizeof(*value->vertices)) ||
        ranges_overlap(output.data, required, value->indices,
                       (size_t)value->index_count * sizeof(*value->indices))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }

    memset(output.data, 0, required);
    write_u32(output.data, MESH_MAGIC);
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u16(output.data + 6U, value->has_vertex_color ?
              MESH_FLAG_VERTEX_COLOR : 0U);
    write_u32(output.data + 8U, value->vertex_count);
    write_u32(output.data + 12U, value->index_count);
    write_u32(output.data + 16U, (uint32_t)stride);
    write_u32(output.data + 20U, (uint32_t)vertex_bytes);
    write_u32(output.data + 24U,
              value->index_count * (uint32_t)sizeof(uint32_t));
    for (index = 0U; index < 3U; ++index) {
        (void)write_float(output.data + 32U + (size_t)index * 4U,
                          bounds_min[index]);
        (void)write_float(output.data + 44U + (size_t)index * 4U,
                          bounds_max[index]);
    }
    for (index = 0U; index < value->vertex_count; ++index) {
        uint8_t *record = output.data +
            MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE +
            (size_t)index * stride;
        size_t axis;

        for (axis = 0U; axis < 3U; ++axis) {
            (void)write_float(record + axis * 4U,
                              value->vertices[index].position[axis]);
        }
        if (value->has_vertex_color) {
            memcpy(record + 12U, value->vertices[index].color_rgba, 4U);
        }
    }
    for (index = 0U; index < value->index_count; ++index) {
        write_u32(output.data + index_offset + (size_t)index * 4U,
                  value->indices[index]);
    }
    *out_size = required;
    return MEISO_COMPOSITION_SCENE_CODEC_OK;
}

static bool float_bits_equal(float value, uint32_t expected_bits)
{
    uint32_t actual_bits;

    return float_to_bits(value, &actual_bits) && actual_bits == expected_bits;
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
    if (ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (input.size < MEISO_COMPOSITION_SCENE_MESH_HEADER_WIRE_SIZE) {
        return MEISO_COMPOSITION_SCENE_CODEC_TRUNCATED;
    }
    if (read_u32(input.data) != MESH_MAGIC ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    flags = read_u16(input.data + 6U);
    vertex_count = read_u32(input.data + 8U);
    index_count = read_u32(input.data + 12U);
    stride = read_u32(input.data + 16U);
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
        read_u32(input.data + 20U) != vertex_bytes ||
        read_u32(input.data + 24U) != index_count * 4U ||
        !bytes_zero(input.data + 28U, 4U) ||
        !bytes_zero(input.data + 56U, 8U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }

    memset(&value, 0, sizeof(value));
    for (index = 0U; index < 3U; ++index) {
        if (!read_float(input.data + 32U + (size_t)index * 4U,
                        &value.bounds_min[index]) ||
            !read_float(input.data + 44U + (size_t)index * 4U,
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
            if (!read_float(record + axis * 4U, &position[axis])) {
                return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
            }
        }
        bounds_include(position, calculated_min, calculated_max,
                       index == 0U);
    }
    for (index = 0U; index < 3U; ++index) {
        if (!float_bits_equal(calculated_min[index],
                              read_u32(input.data + 32U + index * 4U)) ||
            !float_bits_equal(calculated_max[index],
                              read_u32(input.data + 44U + index * 4U))) {
            return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
        }
    }
    for (index = 0U; index < index_count; ++index) {
        if (read_u32(input.data + index_offset + (size_t)index * 4U) >=
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
        if (!read_float(record + axis * 4U, &value.position[axis])) {
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
    value = read_u32(view->index_records.data + (size_t)index * 4U);
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

static bool camera_valid(const MeisoCompositionSceneCamera *value)
{
    return value != NULL &&
        value->space == MEISO_COMPOSITION_SCENE_SPACE_WORLD &&
        value->space_epoch != 0U &&
        meiso_time_point_valid(value->source_time) &&
        canonical_float_array(value->position, 3U) &&
        canonical_quaternion(value->orientation_xyzw) &&
        value->projection ==
            MEISO_COMPOSITION_SCENE_PROJECTION_PERSPECTIVE &&
        canonical_float_array(&value->vertical_fov_degrees, 1U) &&
        canonical_float_array(&value->near_z, 1U) &&
        canonical_float_array(&value->far_z, 1U) &&
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

    if (value == NULL || !canonical_float_array(value->position, 3U) ||
        !canonical_quaternion(value->orientation_xyzw) ||
        !canonical_float_array(value->scale, 3U)) {
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
    return value != NULL && object_id_valid(value->id) &&
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
            if (object_id_equal(value->objects[index].id,
                                value->objects[previous].id)) {
                return false;
            }
            if (content_id_equal(&value->objects[index].mesh_content_id,
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
    write_u16(destination, value->algorithm);
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
    value.algorithm = read_u16(source);
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
    write_u64(output + 8U, value->space_epoch);
    write_u64(output + 16U, value->source_time.clock_epoch);
    write_u64(output + 24U, value->source_time.monotonic_ns);
    for (axis = 0U; axis < 3U; ++axis) {
        (void)write_float(output + 32U + axis * 4U,
                          value->position[axis]);
    }
    for (axis = 0U; axis < 4U; ++axis) {
        (void)write_float(output + 44U + axis * 4U,
                          value->orientation_xyzw[axis]);
    }
    (void)write_float(output + 60U, value->vertical_fov_degrees);
    (void)write_float(output + 64U, value->near_z);
    (void)write_float(output + 68U, value->far_z);
    write_u16(output + 72U, value->viewport_x);
    write_u16(output + 74U, value->viewport_y);
    write_u16(output + 76U, value->viewport_width);
    write_u16(output + 78U, value->viewport_height);
}

static bool decode_camera(
    const uint8_t input[CAMERA_WIRE_SIZE],
    MeisoCompositionSceneCamera *out_value
)
{
    MeisoCompositionSceneCamera value;
    size_t axis;

    if (!bytes_zero(input + 2U, 6U) ||
        !bytes_zero(input + 80U, 16U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.space = input[0];
    value.projection = input[1];
    value.space_epoch = read_u64(input + 8U);
    value.source_time.clock_epoch = read_u64(input + 16U);
    value.source_time.monotonic_ns = read_u64(input + 24U);
    for (axis = 0U; axis < 3U; ++axis) {
        if (!read_float(input + 32U + axis * 4U,
                        &value.position[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 4U; ++axis) {
        if (!read_float(input + 44U + axis * 4U,
                        &value.orientation_xyzw[axis])) {
            return false;
        }
    }
    if (!read_float(input + 60U, &value.vertical_fov_degrees) ||
        !read_float(input + 64U, &value.near_z) ||
        !read_float(input + 68U, &value.far_z)) {
        return false;
    }
    value.viewport_x = read_u16(input + 72U);
    value.viewport_y = read_u16(input + 74U);
    value.viewport_width = read_u16(input + 76U);
    value.viewport_height = read_u16(input + 78U);
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

    write_u64(output, value->id.high);
    write_u64(output + 8U, value->id.low);
    write_content_id(output + 16U, &value->mesh_content_id);
    output[52U] = value->space;
    output[53U] = value->flags;
    write_u64(output + 56U, value->space_epoch);
    for (axis = 0U; axis < 3U; ++axis) {
        (void)write_float(output + 64U + axis * 4U,
                          value->transform.position[axis]);
    }
    for (axis = 0U; axis < 4U; ++axis) {
        (void)write_float(output + 76U + axis * 4U,
                          value->transform.orientation_xyzw[axis]);
    }
    for (axis = 0U; axis < 3U; ++axis) {
        (void)write_float(output + 92U + axis * 4U,
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

    if (!bytes_zero(input + 54U, 2U) ||
        !bytes_zero(input + 104U, 8U)) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.id.high = read_u64(input);
    value.id.low = read_u64(input + 8U);
    if (!read_content_id(input + 16U, &value.mesh_content_id)) {
        return false;
    }
    value.space = input[52U];
    value.flags = input[53U];
    value.space_epoch = read_u64(input + 56U);
    for (axis = 0U; axis < 3U; ++axis) {
        if (!read_float(input + 64U + axis * 4U,
                        &value.transform.position[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 4U; ++axis) {
        if (!read_float(input + 76U + axis * 4U,
                        &value.transform.orientation_xyzw[axis])) {
            return false;
        }
    }
    for (axis = 0U; axis < 3U; ++axis) {
        if (!read_float(input + 92U + axis * 4U,
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
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, valid, output, required, out_size
    );
    uint32_t index;

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(value, sizeof(*value), output.data, required)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0, required);
    write_u32(output.data, SCENE_MAGIC);
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u16(output.data + 6U, value->flags);
    write_u32(output.data + 8U, value->object_count);
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
    if (read_u32(input.data) != SCENE_MAGIC ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        read_u16(input.data + 6U) != MEISO_COMPOSITION_SCENE_FLAGS_NONE ||
        !bytes_zero(input.data + 12U, 4U) ||
        !decode_camera(input.data + 16U, &camera)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    object_count = read_u32(input.data + 8U);
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
    if (ranges_overlap(input.data, input.size, out_value,
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
            if (object_id_equal(object.id, previous_object.id)) {
                return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
            }
            if (content_id_equal(&object.mesh_content_id,
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
        !bytes_nonzero(value->full_target_sha256,
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
    MeisoCompositionSceneCodecStatus status = prepare_encode(
        value, render_result_valid(value), output,
        MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE, out_size
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(value, sizeof(*value), output.data,
                       MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE)) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    memset(output.data, 0,
           MEISO_COMPOSITION_SCENE_RENDER_RESULT_METADATA_WIRE_SIZE);
    write_u32(output.data, RESULT_MAGIC);
    write_u16(output.data + 4U, CODEC_WIRE_VERSION);
    write_u16(output.data + 6U, value->result_code);
    write_u32(output.data + 8U, value->evidence_flags);
    write_u16(output.data + 12U, (uint16_t)value->object_count);
    write_u16(output.data + 14U, (uint16_t)value->unique_mesh_count);
    write_u64(output.data + 16U,
              value->applied_scene_owner_incarnation);
    write_u64(output.data + 24U, value->applied_scene_state_version);
    write_u64(output.data + 32U, value->active_plan_generation);
    write_u64(output.data + 40U, value->world_generation);
    write_u64(output.data + 48U, value->source_clock_epoch);
    write_u64(output.data + 56U, value->render_start_monotonic_ns);
    write_u64(output.data + 64U, value->render_end_monotonic_ns);
    write_u16(output.data + 72U, value->target_width);
    write_u16(output.data + 74U, value->target_height);
    write_u16(output.data + 76U, value->color_format);
    write_u16(output.data + 78U, value->depth_format);
    write_u32(output.data + 80U, value->logical_color_row_bytes);
    write_u32(output.data + 84U, value->logical_depth_row_bytes);
    memcpy(output.data + 88U, value->full_target_sha256,
           MEISO_SHA256_DIGEST_SIZE);
    write_u16(output.data + 120U, value->thumbnail_width);
    write_u16(output.data + 122U, value->thumbnail_height);
    write_u16(output.data + 124U, value->thumbnail_format);
    write_u32(output.data + 128U, value->thumbnail_stride_bytes);
    write_u32(output.data + 132U, value->thumbnail_byte_size);
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
    MeisoCompositionSceneCodecStatus status = prepare_fixed_decode(
        input, MEISO_COMPOSITION_SCENE_RENDER_RESULT_WIRE_SIZE, out_value
    );

    if (status != MEISO_COMPOSITION_SCENE_CODEC_OK) {
        return status;
    }
    if (ranges_overlap(input.data, input.size, out_value,
                       sizeof(*out_value))) {
        return MEISO_COMPOSITION_SCENE_CODEC_INVALID_ARGUMENT;
    }
    if (read_u32(input.data) != RESULT_MAGIC ||
        read_u16(input.data + 4U) != CODEC_WIRE_VERSION ||
        !bytes_zero(input.data + 126U, 2U) ||
        !bytes_zero(input.data + 136U, 24U)) {
        return MEISO_COMPOSITION_SCENE_CODEC_MALFORMED;
    }
    memset(&value, 0, sizeof(value));
    value.result_code = read_u16(input.data + 6U);
    value.evidence_flags = read_u32(input.data + 8U);
    value.object_count = read_u16(input.data + 12U);
    value.unique_mesh_count = read_u16(input.data + 14U);
    value.applied_scene_owner_incarnation = read_u64(input.data + 16U);
    value.applied_scene_state_version = read_u64(input.data + 24U);
    value.active_plan_generation = read_u64(input.data + 32U);
    value.world_generation = read_u64(input.data + 40U);
    value.source_clock_epoch = read_u64(input.data + 48U);
    value.render_start_monotonic_ns = read_u64(input.data + 56U);
    value.render_end_monotonic_ns = read_u64(input.data + 64U);
    value.target_width = read_u16(input.data + 72U);
    value.target_height = read_u16(input.data + 74U);
    value.color_format = read_u16(input.data + 76U);
    value.depth_format = read_u16(input.data + 78U);
    value.logical_color_row_bytes = read_u32(input.data + 80U);
    value.logical_depth_row_bytes = read_u32(input.data + 84U);
    memcpy(value.full_target_sha256, input.data + 88U,
           MEISO_SHA256_DIGEST_SIZE);
    value.thumbnail_width = read_u16(input.data + 120U);
    value.thumbnail_height = read_u16(input.data + 122U);
    value.thumbnail_format = read_u16(input.data + 124U);
    value.thumbnail_stride_bytes = read_u32(input.data + 128U);
    value.thumbnail_byte_size = read_u32(input.data + 132U);
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
