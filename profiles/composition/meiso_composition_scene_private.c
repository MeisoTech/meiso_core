#include "meiso_composition_scene_private.h"

#include <float.h>

#include <limits.h>

#include <string.h>

#if FLT_RADIX != 2 || FLT_MANT_DIG != 24 || FLT_MAX_EXP != 128
#error "Composition scene codecs require IEEE-754 binary32 float"
#endif
_Static_assert(sizeof(float) == sizeof(uint32_t),

               "Composition scene codecs require 32-bit float");

/* Shared canonical endian, overlap, float, identity, and codec guards. */

void meiso_composition_scene_private_write_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8U);
    destination[1] = (uint8_t)value;
}

void meiso_composition_scene_private_write_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24U);
    destination[1] = (uint8_t)(value >> 16U);
    destination[2] = (uint8_t)(value >> 8U);
    destination[3] = (uint8_t)value;
}

void meiso_composition_scene_private_write_u64(uint8_t *destination, uint64_t value)
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

uint16_t meiso_composition_scene_private_read_u16(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8U) | source[1]);
}

uint32_t meiso_composition_scene_private_read_u32(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24U) |
        ((uint32_t)source[1] << 16U) |
        ((uint32_t)source[2] << 8U) |
        (uint32_t)source[3];
}

uint64_t meiso_composition_scene_private_read_u64(const uint8_t *source)
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

bool meiso_composition_scene_private_bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool meiso_composition_scene_private_ranges_overlap(
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

bool meiso_composition_scene_private_size_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (rhs != 0U && lhs > SIZE_MAX / rhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

bool meiso_composition_scene_private_size_add(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out_value = lhs + rhs;
    return true;
}

static bool meiso_composition_scene_private_id128_equal(uint64_t ah, uint64_t al, uint64_t bh, uint64_t bl)
{
    return ah == bh && al == bl;
}

bool meiso_composition_scene_private_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return meiso_composition_scene_private_id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

bool meiso_composition_scene_private_schema_id_equal(MeisoSchemaId lhs, MeisoSchemaId rhs)
{
    return meiso_composition_scene_private_id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

bool meiso_composition_scene_private_resource_id_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return meiso_composition_scene_private_id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

static bool meiso_composition_scene_private_float_bits_canonical(uint32_t bits)
{
    const uint32_t exponent = bits & UINT32_C(0x7f800000);
    const uint32_t fraction = bits & UINT32_C(0x007fffff);

    return exponent != UINT32_C(0x7f800000) &&
        !(exponent == 0U && fraction != 0U) &&
        bits != UINT32_C(0x80000000);
}

bool meiso_composition_scene_private_float_to_bits(float value, uint32_t *out_bits)
{
    uint32_t bits;

    if (out_bits == NULL) {
        return false;
    }
    memcpy(&bits, &value, sizeof(bits));
    if (!meiso_composition_scene_private_float_bits_canonical(bits)) {
        return false;
    }
    *out_bits = bits;
    return true;
}

static bool meiso_composition_scene_private_bits_to_float(uint32_t bits, float *out_value)
{
    if (out_value == NULL || !meiso_composition_scene_private_float_bits_canonical(bits)) {
        return false;
    }
    memcpy(out_value, &bits, sizeof(bits));
    return true;
}

bool meiso_composition_scene_private_write_float(uint8_t *destination, float value)
{
    uint32_t bits;

    if (!meiso_composition_scene_private_float_to_bits(value, &bits)) {
        return false;
    }
    meiso_composition_scene_private_write_u32(destination, bits);
    return true;
}

bool meiso_composition_scene_private_read_float(const uint8_t *source, float *out_value)
{
    return meiso_composition_scene_private_bits_to_float(meiso_composition_scene_private_read_u32(source), out_value);
}

bool meiso_composition_scene_private_canonical_float_array(const float *values, size_t count)
{
    size_t index;
    uint32_t ignored;

    if (values == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (!meiso_composition_scene_private_float_to_bits(values[index], &ignored)) {
            return false;
        }
    }
    return true;
}

bool meiso_composition_scene_private_canonical_quaternion(const float value[4])
{
    double norm;
    double delta;
    size_t index;

    if (!meiso_composition_scene_private_canonical_float_array(value, 4U)) {
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

bool meiso_composition_scene_private_object_id_valid(MeisoCompositionSceneObjectId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_composition_scene_private_object_id_equal(
    MeisoCompositionSceneObjectId lhs,
    MeisoCompositionSceneObjectId rhs
)
{
    return meiso_composition_scene_private_id128_equal(lhs.high, lhs.low, rhs.high, rhs.low);
}

bool meiso_composition_scene_private_content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
)
{
    return lhs != NULL && rhs != NULL &&
        lhs->algorithm == rhs->algorithm &&
        lhs->digest_size == rhs->digest_size &&
        memcmp(lhs->digest, rhs->digest, MEISO_SHA256_DIGEST_SIZE) == 0;
}

bool meiso_composition_scene_private_stable_reason_valid(MeisoCompositionSceneStableReason value)
{
    return value <= MEISO_COMPOSITION_SCENE_REASON_RENDERER_FAILED;
}

bool meiso_composition_scene_private_bytes_nonzero(const uint8_t *bytes, size_t size)
{
    return !meiso_composition_scene_private_bytes_zero(bytes, size);
}

MeisoCompositionSceneCodecStatus meiso_composition_scene_private_prepare_encode(
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
MeisoCompositionSceneCodecStatus meiso_composition_scene_private_prepare_fixed_decode(
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
