#include "meiso_composition_profile_private.h"

#include <limits.h>

#include <string.h>

/* Shared canonical endian, overlap, size, and identifier primitives. */

void meiso_composition_profile_private_put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)value;
}

void meiso_composition_profile_private_put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)(value >> 16U);
    out[2] = (uint8_t)(value >> 8U);
    out[3] = (uint8_t)value;
}

void meiso_composition_profile_private_put_u64(uint8_t *out, uint64_t value)
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

uint16_t meiso_composition_profile_private_get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

uint32_t meiso_composition_profile_private_get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) | ((uint32_t)in[1] << 16U) |
        ((uint32_t)in[2] << 8U) | (uint32_t)in[3];
}

uint64_t meiso_composition_profile_private_get_u64(const uint8_t *in)
{
    return ((uint64_t)in[0] << 56U) | ((uint64_t)in[1] << 48U) |
        ((uint64_t)in[2] << 40U) | ((uint64_t)in[3] << 32U) |
        ((uint64_t)in[4] << 24U) | ((uint64_t)in[5] << 16U) |
        ((uint64_t)in[6] << 8U) | (uint64_t)in[7];
}

void meiso_composition_profile_private_put_id(uint8_t *out, uint64_t high, uint64_t low)
{
    meiso_composition_profile_private_put_u64(out, high);
    meiso_composition_profile_private_put_u64(out + 8U, low);
}

bool meiso_composition_profile_private_bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t i;
    for (i = 0U; i < size; ++i) {
        if (bytes[i] != 0U) {
            return false;
        }
    }
    return true;
}

bool meiso_composition_profile_private_ranges_overlap(
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

bool meiso_composition_profile_private_checked_multiply(size_t lhs, size_t rhs, size_t *out_value)
{
    if (out_value == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out_value = lhs * rhs;
    return true;
}

bool meiso_composition_profile_private_endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_composition_profile_private_resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_composition_profile_private_schema_equal(MeisoSchemaRef lhs, MeisoSchemaRef rhs)
{
    return lhs.id.high == rhs.id.high && lhs.id.low == rhs.id.low &&
        lhs.version == rhs.version;
}
