#ifndef MEISO_COMPOSITION_PROFILE_PRIVATE_H
#define MEISO_COMPOSITION_PROFILE_PRIVATE_H

#include <meiso/profiles/composition/profile.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_COMPOSITION_PROFILE_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_COMPOSITION_PROFILE_PRIVATE_API
#endif

enum {
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CAPABILITY_MAGIC = 0x4d434150U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_DESIRED_MAGIC = 0x4d444553U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_ACTUAL_MAGIC = 0x4d414354U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_OPEN_MAGIC = 0x4d4f504eU,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_BIND_MAGIC = 0x4d424e44U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_FRAME_MAGIC = 0x4d46524dU,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_CODEC_VERSION = 1U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MRT0_CATALOG_TYPE = 1U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_ENDPOINT_RECORD_WIRE_SIZE = 48U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_CATALOG_PREFIX_WIRE_SIZE = 16U,
    MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT = 3U
};

/*
 * Build-private Profile seam; this header is not installed. Constants are
 * immutable library-owned borrows. Catalog storage and codec buffers remain
 * caller-owned; frame fragments and endpoint capability views retain the
 * borrow lifetimes documented by the public Profile header.
 */
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoProfileRef
    meiso_composition_profile_private_profile;
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoEndpointId
    meiso_composition_profile_private_render_endpoint;
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoEndpointId
    meiso_composition_profile_private_camera_endpoint;
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoSchemaRef
    meiso_composition_profile_private_media_capability_schema;
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoResourceDescriptor
    meiso_composition_profile_private_render_resources[
        MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT
    ];
extern MEISO_COMPOSITION_PROFILE_PRIVATE_API const MeisoResourceDescriptor
    meiso_composition_profile_private_camera_resources[
        MEISO_COMPOSITION_PROFILE_PRIVATE_MEDIA_RESOURCE_COUNT
    ];

MEISO_COMPOSITION_PROFILE_PRIVATE_API void
meiso_composition_profile_private_put_u16(uint8_t *out, uint16_t value);
MEISO_COMPOSITION_PROFILE_PRIVATE_API void
meiso_composition_profile_private_put_u32(uint8_t *out, uint32_t value);
MEISO_COMPOSITION_PROFILE_PRIVATE_API void
meiso_composition_profile_private_put_u64(uint8_t *out, uint64_t value);
MEISO_COMPOSITION_PROFILE_PRIVATE_API uint16_t
meiso_composition_profile_private_get_u16(const uint8_t *in);
MEISO_COMPOSITION_PROFILE_PRIVATE_API uint32_t
meiso_composition_profile_private_get_u32(const uint8_t *in);
MEISO_COMPOSITION_PROFILE_PRIVATE_API uint64_t
meiso_composition_profile_private_get_u64(const uint8_t *in);
MEISO_COMPOSITION_PROFILE_PRIVATE_API void
meiso_composition_profile_private_put_id(
    uint8_t *out,
    uint64_t high,
    uint64_t low
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_bytes_zero(
    const uint8_t *bytes,
    size_t size
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_checked_multiply(
    size_t lhs,
    size_t rhs,
    size_t *out_value
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_endpoint_equal(
    MeisoEndpointId lhs,
    MeisoEndpointId rhs
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_resource_equal(
    MeisoResourceId lhs,
    MeisoResourceId rhs
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_schema_equal(
    MeisoSchemaRef lhs,
    MeisoSchemaRef rhs
);
MEISO_COMPOSITION_PROFILE_PRIVATE_API bool
meiso_composition_profile_private_media_source_valid(
    MeisoCompositionMediaSource source
);

static inline MeisoCompositionProfileStatus
meiso_composition_profile_private_prepare_encode(
    const void *value,
    size_t value_size,
    bool valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
)
{
    if (out_size == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (value != NULL &&
        meiso_composition_profile_private_ranges_overlap(
            value, value_size, out_size, sizeof(*out_size)
        )) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (value == NULL || output.data == NULL) {
        *out_size = 0U;
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_composition_profile_private_ranges_overlap(
            value, value_size, output.data, required
        ) ||
        meiso_composition_profile_private_ranges_overlap(
            output.data, required, out_size, sizeof(*out_size)
        )) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    *out_size = 0U;
    if (!valid) {
        return MEISO_COMPOSITION_PROFILE_INVALID_CONFIG;
    }
    return output.capacity < required ? MEISO_COMPOSITION_PROFILE_CAPACITY :
        MEISO_COMPOSITION_PROFILE_OK;
}

static inline MeisoCompositionProfileStatus
meiso_composition_profile_private_prepare_decode(
    MeisoByteView input,
    size_t required,
    void *output,
    size_t output_size
)
{
    if (input.data == NULL || output == NULL) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (meiso_composition_profile_private_ranges_overlap(
            input.data, input.size, output, output_size
        )) {
        return MEISO_COMPOSITION_PROFILE_INVALID_ARGUMENT;
    }
    if (input.size < required) {
        return MEISO_COMPOSITION_PROFILE_TRUNCATED;
    }
    return input.size > required ? MEISO_COMPOSITION_PROFILE_MALFORMED :
        MEISO_COMPOSITION_PROFILE_OK;
}

#endif
