#ifndef MEISO_DESCRIPTOR_SET_H
#define MEISO_DESCRIPTOR_SET_H

#include "meiso_resource.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoDescriptorSet MeisoDescriptorSet;

typedef uint8_t MeisoDescriptorSetInitStatus;
enum {
    MEISO_DESCRIPTOR_SET_INIT_OK = 0,
    MEISO_DESCRIPTOR_SET_INIT_INVALID_ARGUMENT = 1,
    MEISO_DESCRIPTOR_SET_INIT_INVALID_CAPACITY = 2,
    MEISO_DESCRIPTOR_SET_INIT_MISALIGNED_ARENA = 3,
    MEISO_DESCRIPTOR_SET_INIT_ARENA_TOO_SMALL = 4
};

typedef uint8_t MeisoDescriptorSetRegisterStatus;
enum {
    MEISO_DESCRIPTOR_SET_REGISTERED = 0,
    MEISO_DESCRIPTOR_SET_REGISTER_DUPLICATE = 1,
    MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT = 2,
    MEISO_DESCRIPTOR_SET_REGISTER_CAPACITY = 3,
    MEISO_DESCRIPTOR_SET_REGISTER_SEALED = 4,
    MEISO_DESCRIPTOR_SET_REGISTER_INVALID_DESCRIPTOR = 5,
    MEISO_DESCRIPTOR_SET_REGISTER_INVALID_ARGUMENT = 6
};

typedef uint8_t MeisoDescriptorSetSealStatus;
enum {
    MEISO_DESCRIPTOR_SET_SEALED = 0,
    MEISO_DESCRIPTOR_SET_ALREADY_SEALED = 1,
    MEISO_DESCRIPTOR_SET_SEAL_INVALID = 2
};

typedef uint8_t MeisoDescriptorSetLookupStatus;
enum {
    MEISO_DESCRIPTOR_SET_LOOKUP_FOUND = 0,
    MEISO_DESCRIPTOR_SET_LOOKUP_NOT_FOUND = 1,
    MEISO_DESCRIPTOR_SET_LOOKUP_NOT_SEALED = 2,
    MEISO_DESCRIPTOR_SET_LOOKUP_INVALID = 3
};

/*
 * Zero capacity is a valid empty builder. It can be sealed and queried, but
 * every pre-seal registration returns REGISTER_CAPACITY.
 */
size_t meiso_descriptor_set_alignment_required(void);
size_t meiso_descriptor_set_memory_required(size_t capacity);
MeisoDescriptorSetInitStatus meiso_descriptor_set_init(
    void *arena,
    size_t arena_size,
    size_t capacity,
    MeisoDescriptorSet **out_set
);

/*
 * Registration copies the complete descriptor. The input may alias the
 * caller arena. Invalid descriptors are rejected before the sealed-state
 * check; every valid registration attempt after seal returns REGISTER_SEALED.
 */
MeisoDescriptorSetRegisterStatus meiso_descriptor_set_register(
    MeisoDescriptorSet *set,
    const MeisoResourceDescriptor *descriptor
);

/* Repeated seal is an explicit successful no-op: ALREADY_SEALED. */
MeisoDescriptorSetSealStatus meiso_descriptor_set_seal(
    MeisoDescriptorSet *set
);

/*
 * Lookup is available only after seal and returns a value copy. Failure leaves
 * out_descriptor unchanged. out_descriptor must not overlap the set arena.
 */
MeisoDescriptorSetLookupStatus meiso_descriptor_set_lookup(
    const MeisoDescriptorSet *set,
    MeisoResourceId resource_id,
    MeisoResourceDescriptor *out_descriptor
);

/* Count is available while building and after seal; an invalid set counts 0. */
size_t meiso_descriptor_set_count(const MeisoDescriptorSet *set);

#ifdef __cplusplus
}
#endif

#endif
