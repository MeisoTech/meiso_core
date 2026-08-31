#include "meiso_runtime_private.h"

#include <string.h>

/* Canonical MRT0 scalar encoding and overflow-safe buffer primitives. */

bool meiso_runtime_private_blob_wire_supported(const MeisoRuntime *runtime)
{
    return runtime != NULL &&
        (runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V3 ||
         runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4);
}

bool meiso_runtime_private_checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

bool meiso_runtime_private_checked_multiply(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || (lhs != 0U && rhs > SIZE_MAX / lhs)) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

bool meiso_runtime_private_align_up(size_t value, size_t alignment, size_t *out)
{
    const size_t remainder = alignment == 0U ? 0U : value % alignment;

    if (alignment == 0U || out == NULL) {
        return false;
    }
    return remainder == 0U ? (*out = value, true) :
        meiso_runtime_private_checked_add(value, alignment - remainder, out);
}

bool meiso_runtime_private_add_aligned_array(
    size_t *cursor,
    size_t alignment,
    size_t count,
    size_t item_size,
    size_t *out_offset
)
{
    size_t bytes;
    size_t aligned;

    if (cursor == NULL || out_offset == NULL ||
        !meiso_runtime_private_align_up(*cursor, alignment, &aligned) ||
        !meiso_runtime_private_checked_multiply(count, item_size, &bytes) ||
        !meiso_runtime_private_checked_add(aligned, bytes, cursor)) {
        return false;
    }
    *out_offset = aligned;
    return true;
}

bool meiso_runtime_private_ranges_overlap(
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

void meiso_runtime_private_put_u16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)(value >> 8U);
    target[1] = (uint8_t)value;
}

void meiso_runtime_private_put_u32(uint8_t *target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24U);
    target[1] = (uint8_t)(value >> 16U);
    target[2] = (uint8_t)(value >> 8U);
    target[3] = (uint8_t)value;
}

void meiso_runtime_private_put_u64(uint8_t *target, uint64_t value)
{
    target[0] = (uint8_t)(value >> 56U);
    target[1] = (uint8_t)(value >> 48U);
    target[2] = (uint8_t)(value >> 40U);
    target[3] = (uint8_t)(value >> 32U);
    target[4] = (uint8_t)(value >> 24U);
    target[5] = (uint8_t)(value >> 16U);
    target[6] = (uint8_t)(value >> 8U);
    target[7] = (uint8_t)value;
}

uint16_t meiso_runtime_private_get_u16(const uint8_t *source)
{
    return (uint16_t)((uint16_t)source[0] << 8U | (uint16_t)source[1]);
}

uint32_t meiso_runtime_private_get_u32(const uint8_t *source)
{
    return (uint32_t)source[0] << 24U |
        (uint32_t)source[1] << 16U |
        (uint32_t)source[2] << 8U |
        (uint32_t)source[3];
}

uint64_t meiso_runtime_private_get_u64(const uint8_t *source)
{
    return (uint64_t)source[0] << 56U |
        (uint64_t)source[1] << 48U |
        (uint64_t)source[2] << 40U |
        (uint64_t)source[3] << 32U |
        (uint64_t)source[4] << 24U |
        (uint64_t)source[5] << 16U |
        (uint64_t)source[6] << 8U |
        (uint64_t)source[7];
}

static void put_id128(uint8_t *target, uint64_t high, uint64_t low)
{
    meiso_runtime_private_put_u64(target, high);
    meiso_runtime_private_put_u64(target + 8U, low);
}

void meiso_runtime_private_put_endpoint_id(uint8_t *target, MeisoEndpointId value)
{
    put_id128(target, value.high, value.low);
}

MeisoEndpointId meiso_runtime_private_get_endpoint_id(const uint8_t *source)
{
    const MeisoEndpointId value = {meiso_runtime_private_get_u64(source), meiso_runtime_private_get_u64(source + 8U)};
    return value;
}

void meiso_runtime_private_put_resource_id(uint8_t *target, MeisoResourceId value)
{
    put_id128(target, value.high, value.low);
}

MeisoResourceId meiso_runtime_private_get_resource_id(const uint8_t *source)
{
    const MeisoResourceId value = {meiso_runtime_private_get_u64(source), meiso_runtime_private_get_u64(source + 8U)};
    return value;
}

void meiso_runtime_private_put_schema(uint8_t *target, MeisoSchemaRef value)
{
    put_id128(target, value.id.high, value.id.low);
    meiso_runtime_private_put_u32(target + 16U, value.version);
}

MeisoSchemaRef meiso_runtime_private_get_schema(const uint8_t *source)
{
    MeisoSchemaRef value;

    value.id.high = meiso_runtime_private_get_u64(source);
    value.id.low = meiso_runtime_private_get_u64(source + 8U);
    value.version = meiso_runtime_private_get_u32(source + 16U);
    return value;
}

bool meiso_runtime_private_bytes_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool meiso_runtime_private_schema_valid(MeisoSchemaRef value)
{
    return meiso_schema_id_valid(value.id) && value.version != 0U;
}

bool meiso_runtime_private_reject_reason_valid(MeisoRuntimeCatalogRejectReason value)
{
    return value >= MEISO_RUNTIME_CATALOG_REJECT_UNSUPPORTED &&
        value <= MEISO_RUNTIME_CATALOG_REJECT_CAPACITY;
}

bool meiso_endpoint_id_valid(MeisoEndpointId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_endpoint_id_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_runtime_facet_valid(MeisoRuntimeFacet value)
{
    return value >= MEISO_RUNTIME_FACET_CAPABILITY &&
        value <= MEISO_RUNTIME_FACET_IMMUTABLE;
}
void meiso_runtime_private_encode_header(
    const MeisoRuntime *runtime,
    uint8_t *target,
    uint8_t type,
    uint32_t size
)
{
    target[0] = (uint8_t)'M';
    target[1] = (uint8_t)'R';
    target[2] = (uint8_t)'T';
    target[3] = (uint8_t)'0';
    target[4] = runtime->wire_version;
    target[5] = type;
    target[6] = 0U;
    target[7] = 0U;
    meiso_runtime_private_put_u32(target + 8U, size);
}

bool meiso_runtime_private_decode_header(
    const MeisoRuntime *runtime,
    MeisoByteView frame,
    uint8_t *out_type
)
{
    if (runtime == NULL || out_type == NULL || frame.data == NULL ||
        frame.size < MEISO_RUNTIME_WIRE_HEADER_SIZE ||
        frame.size > UINT32_MAX || frame.data[0] != (uint8_t)'M' ||
        frame.data[1] != (uint8_t)'R' || frame.data[2] != (uint8_t)'T' ||
        frame.data[3] != (uint8_t)'0' ||
        frame.data[4] != runtime->wire_version ||
        frame.data[6] != 0U || frame.data[7] != 0U ||
        meiso_runtime_private_get_u32(frame.data + 8U) != frame.size) {
        return false;
    }
    *out_type = frame.data[5];
    return true;
}
void meiso_runtime_private_encode_route_key(uint8_t *output, MeisoRuntimeRouteKey key)
{
    meiso_runtime_private_put_u64(output, key.catalog_generation);
    meiso_runtime_private_put_endpoint_id(output + 8U, key.endpoint_id);
    meiso_runtime_private_put_u64(output + 24U, key.endpoint_incarnation);
    meiso_runtime_private_put_resource_id(output + 32U, key.resource_id);
    output[48U] = key.facet;
    memset(output + 49U, 0, 7U);
}

bool meiso_runtime_private_decode_route_key(
    const uint8_t *input,
    MeisoRuntimeRouteKey *out_key
)
{
    if (input == NULL || out_key == NULL || !meiso_runtime_private_bytes_zero(input + 49U, 7U)) {
        return false;
    }
    out_key->catalog_generation = meiso_runtime_private_get_u64(input);
    out_key->endpoint_id = meiso_runtime_private_get_endpoint_id(input + 8U);
    out_key->endpoint_incarnation = meiso_runtime_private_get_u64(input + 24U);
    out_key->resource_id = meiso_runtime_private_get_resource_id(input + 32U);
    out_key->facet = input[48U];
    return out_key->catalog_generation != 0U &&
        meiso_endpoint_id_valid(out_key->endpoint_id) &&
        out_key->endpoint_incarnation != 0U &&
        meiso_resource_id_valid(out_key->resource_id) &&
        meiso_runtime_facet_valid(out_key->facet);
}

void meiso_runtime_private_canonicalize_content_id(
    const MeisoContentId *source,
    MeisoContentId *target
)
{
    memset(target, 0, sizeof(*target));
    target->algorithm = source->algorithm;
    target->digest_size = source->digest_size;
    memmove(target->digest, source->digest, source->digest_size);
}

void meiso_runtime_private_encode_content_id(uint8_t *output, const MeisoContentId *value)
{
    meiso_runtime_private_put_u16(output, value->algorithm);
    output[2] = value->digest_size;
    output[3] = 0U;
    memmove(output + 4U, value->digest, MEISO_SHA256_DIGEST_SIZE);
}

bool meiso_runtime_private_decode_content_id(
    const uint8_t *input,
    MeisoContentId *out_value
)
{
    MeisoContentId value;

    if (input == NULL || out_value == NULL || input[3] != 0U) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    value.algorithm = meiso_runtime_private_get_u16(input);
    value.digest_size = input[2];
    memmove(value.digest, input + 4U, MEISO_SHA256_DIGEST_SIZE);
    if (!meiso_content_id_valid(&value)) {
        return false;
    }
    *out_value = value;
    return true;
}
