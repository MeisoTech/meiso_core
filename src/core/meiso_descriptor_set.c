#include <meiso/core/descriptor_set.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MEISO_DESCRIPTOR_SET_MAGIC UINT64_C(0x4D4549534F445345)

struct MeisoDescriptorSet {
    uint64_t magic;
    size_t capacity;
    size_t count;
    size_t required_size;
    size_t slots_offset;
    bool sealed;
};

static bool checked_add(size_t lhs, size_t rhs, size_t *out)
{
    if (rhs > SIZE_MAX - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static bool checked_multiply(size_t lhs, size_t rhs, size_t *out)
{
    if (lhs != 0U && rhs > SIZE_MAX / lhs) {
        return false;
    }
    *out = lhs * rhs;
    return true;
}

static bool align_up(size_t value, size_t alignment, size_t *out)
{
    size_t remainder;

    if (alignment == 0U) {
        return false;
    }
    remainder = value % alignment;
    if (remainder == 0U) {
        *out = value;
        return true;
    }
    return checked_add(value, alignment - remainder, out);
}

static bool calculate_layout(
    size_t capacity,
    size_t *out_slots_offset,
    size_t *out_required
)
{
    size_t slots_size;

    return align_up(
            sizeof(MeisoDescriptorSet),
            _Alignof(MeisoResourceDescriptor),
            out_slots_offset
        ) &&
        checked_multiply(
            capacity, sizeof(MeisoResourceDescriptor), &slots_size
        ) &&
        checked_add(*out_slots_offset, slots_size, out_required);
}

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_descriptor_set_alignment_required() == 0U;
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

    if (lhs_size > UINTPTR_MAX - lhs_start ||
        rhs_size > UINTPTR_MAX - rhs_start) {
        return true;
    }
    lhs_end = lhs_start + lhs_size;
    rhs_end = rhs_start + rhs_size;
    return lhs_start < rhs_end && rhs_start < lhs_end;
}

static MeisoResourceDescriptor *slots(MeisoDescriptorSet *set)
{
    return (MeisoResourceDescriptor *)(void *)(
        (uint8_t *)(void *)set + set->slots_offset
    );
}

static const MeisoResourceDescriptor *const_slots(
    const MeisoDescriptorSet *set
)
{
    return (const MeisoResourceDescriptor *)(const void *)(
        (const uint8_t *)(const void *)set + set->slots_offset
    );
}

static bool set_valid(const MeisoDescriptorSet *set)
{
    size_t slots_offset;
    size_t required;
    size_t index;

    if (set == NULL || set->magic != MEISO_DESCRIPTOR_SET_MAGIC ||
        !calculate_layout(set->capacity, &slots_offset, &required) ||
        set->slots_offset != slots_offset || set->required_size != required ||
        set->count > set->capacity) {
        return false;
    }
    for (index = 0U; index < set->count; ++index) {
        if (meiso_resource_descriptor_validate(&const_slots(set)[index]) !=
            MEISO_DESCRIPTOR_VALID) {
            return false;
        }
    }
    return true;
}

size_t meiso_descriptor_set_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_descriptor_set_memory_required(size_t capacity)
{
    size_t slots_offset;
    size_t required;

    if (!calculate_layout(capacity, &slots_offset, &required)) {
        return 0U;
    }
    return required;
}

MeisoDescriptorSetInitStatus meiso_descriptor_set_init(
    void *arena,
    size_t arena_size,
    size_t capacity,
    MeisoDescriptorSet **out_set
)
{
    size_t slots_offset;
    size_t required;
    MeisoDescriptorSet *set;

    if (out_set == NULL) {
        return MEISO_DESCRIPTOR_SET_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && ranges_overlap(
        arena, arena_size, out_set, sizeof(*out_set)
    )) {
        return MEISO_DESCRIPTOR_SET_INIT_INVALID_ARGUMENT;
    }
    *out_set = NULL;
    if (!calculate_layout(capacity, &slots_offset, &required)) {
        return MEISO_DESCRIPTOR_SET_INIT_INVALID_CAPACITY;
    }
    if (!arena_aligned(arena)) {
        return MEISO_DESCRIPTOR_SET_INIT_MISALIGNED_ARENA;
    }
    if (arena_size < required) {
        return MEISO_DESCRIPTOR_SET_INIT_ARENA_TOO_SMALL;
    }

    memset(arena, 0, required);
    set = arena;
    set->magic = MEISO_DESCRIPTOR_SET_MAGIC;
    set->capacity = capacity;
    set->required_size = required;
    set->slots_offset = slots_offset;
    *out_set = set;
    return MEISO_DESCRIPTOR_SET_INIT_OK;
}

MeisoDescriptorSetRegisterStatus meiso_descriptor_set_register(
    MeisoDescriptorSet *set,
    const MeisoResourceDescriptor *descriptor
)
{
    MeisoResourceDescriptor descriptor_copy;
    size_t index;

    if (set == NULL || !set_valid(set) || descriptor == NULL) {
        return MEISO_DESCRIPTOR_SET_REGISTER_INVALID_ARGUMENT;
    }
    descriptor_copy = *descriptor;
    if (meiso_resource_descriptor_validate(&descriptor_copy) !=
        MEISO_DESCRIPTOR_VALID) {
        return MEISO_DESCRIPTOR_SET_REGISTER_INVALID_DESCRIPTOR;
    }
    if (set->sealed) {
        return MEISO_DESCRIPTOR_SET_REGISTER_SEALED;
    }

    for (index = 0U; index < set->count; ++index) {
        if (meiso_resource_id_equal(
            const_slots(set)[index].id, descriptor_copy.id
        )) {
            if (meiso_resource_descriptor_equal(
                &const_slots(set)[index], &descriptor_copy
            )) {
                return MEISO_DESCRIPTOR_SET_REGISTER_DUPLICATE;
            }
            return MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT;
        }
    }
    if (set->count == set->capacity) {
        return MEISO_DESCRIPTOR_SET_REGISTER_CAPACITY;
    }

    slots(set)[set->count] = descriptor_copy;
    set->count += 1U;
    return MEISO_DESCRIPTOR_SET_REGISTERED;
}

MeisoDescriptorSetSealStatus meiso_descriptor_set_seal(MeisoDescriptorSet *set)
{
    if (set == NULL || !set_valid(set)) {
        return MEISO_DESCRIPTOR_SET_SEAL_INVALID;
    }
    if (set->sealed) {
        return MEISO_DESCRIPTOR_SET_ALREADY_SEALED;
    }
    set->sealed = true;
    return MEISO_DESCRIPTOR_SET_SEALED;
}

MeisoDescriptorSetLookupStatus meiso_descriptor_set_lookup(
    const MeisoDescriptorSet *set,
    MeisoResourceId resource_id,
    MeisoResourceDescriptor *out_descriptor
)
{
    size_t index;

    if (set == NULL || !set_valid(set) ||
        !meiso_resource_id_valid(resource_id) || out_descriptor == NULL ||
        ranges_overlap(
            set, set->required_size, out_descriptor, sizeof(*out_descriptor)
        )) {
        return MEISO_DESCRIPTOR_SET_LOOKUP_INVALID;
    }
    if (!set->sealed) {
        return MEISO_DESCRIPTOR_SET_LOOKUP_NOT_SEALED;
    }
    for (index = 0U; index < set->count; ++index) {
        if (meiso_resource_id_equal(
            const_slots(set)[index].id, resource_id
        )) {
            *out_descriptor = const_slots(set)[index];
            return MEISO_DESCRIPTOR_SET_LOOKUP_FOUND;
        }
    }
    return MEISO_DESCRIPTOR_SET_LOOKUP_NOT_FOUND;
}

size_t meiso_descriptor_set_count(const MeisoDescriptorSet *set)
{
    return set_valid(set) ? set->count : 0U;
}
