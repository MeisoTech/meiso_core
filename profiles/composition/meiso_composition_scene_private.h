#ifndef MEISO_COMPOSITION_SCENE_PRIVATE_H
#define MEISO_COMPOSITION_SCENE_PRIVATE_H

#include <meiso/profiles/composition/scene.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_COMPOSITION_SCENE_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_COMPOSITION_SCENE_PRIVATE_API
#endif

/*
 * Build-private codec seam; this header is not installed. Public values and
 * encode/decode buffers remain caller-owned. Catalog constants and the
 * canonical cube are immutable library-owned borrows, while decoded mesh
 * views borrow their input bytes for the lifetime documented by scene.h.
 */
extern MEISO_COMPOSITION_SCENE_PRIVATE_API const MeisoEndpointId
    meiso_composition_scene_private_scene_endpoint;

MEISO_COMPOSITION_SCENE_PRIVATE_API void
meiso_composition_scene_private_write_u16(uint8_t *destination, uint16_t value);
MEISO_COMPOSITION_SCENE_PRIVATE_API void
meiso_composition_scene_private_write_u32(uint8_t *destination, uint32_t value);
MEISO_COMPOSITION_SCENE_PRIVATE_API void
meiso_composition_scene_private_write_u64(uint8_t *destination, uint64_t value);
MEISO_COMPOSITION_SCENE_PRIVATE_API uint16_t
meiso_composition_scene_private_read_u16(const uint8_t *source);
MEISO_COMPOSITION_SCENE_PRIVATE_API uint32_t
meiso_composition_scene_private_read_u32(const uint8_t *source);
MEISO_COMPOSITION_SCENE_PRIVATE_API uint64_t
meiso_composition_scene_private_read_u64(const uint8_t *source);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_bytes_zero(const uint8_t *bytes, size_t size);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_size_multiply(
    size_t lhs,
    size_t rhs,
    size_t *out_value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_size_add(
    size_t lhs,
    size_t rhs,
    size_t *out_value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_endpoint_id_equal(
    MeisoEndpointId lhs,
    MeisoEndpointId rhs
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_schema_id_equal(
    MeisoSchemaId lhs,
    MeisoSchemaId rhs
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_resource_id_equal(
    MeisoResourceId lhs,
    MeisoResourceId rhs
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_float_to_bits(
    float value,
    uint32_t *out_bits
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_write_float(
    uint8_t *destination,
    float value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_read_float(
    const uint8_t *source,
    float *out_value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_canonical_float_array(
    const float *values,
    size_t count
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_canonical_quaternion(const float value[4]);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_object_id_valid(
    MeisoCompositionSceneObjectId value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_object_id_equal(
    MeisoCompositionSceneObjectId lhs,
    MeisoCompositionSceneObjectId rhs
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_content_id_equal(
    const MeisoContentId *lhs,
    const MeisoContentId *rhs
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_stable_reason_valid(
    MeisoCompositionSceneStableReason value
);
MEISO_COMPOSITION_SCENE_PRIVATE_API bool
meiso_composition_scene_private_bytes_nonzero(
    const uint8_t *bytes,
    size_t size
);
MEISO_COMPOSITION_SCENE_PRIVATE_API MeisoCompositionSceneCodecStatus
meiso_composition_scene_private_prepare_encode(
    const void *value,
    bool value_valid,
    MeisoMutableBuffer output,
    size_t required,
    size_t *out_size
);
MEISO_COMPOSITION_SCENE_PRIVATE_API MeisoCompositionSceneCodecStatus
meiso_composition_scene_private_prepare_fixed_decode(
    MeisoByteView input,
    size_t required,
    const void *out_value
);

#endif
