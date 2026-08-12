#include "meiso_descriptor_set.h"

#include "meiso_test_support.h"

#include <stdio.h>
#include <string.h>

typedef union {
    max_align_t alignment;
    uint8_t bytes[4096];
} TestArena;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                         \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

_Static_assert(sizeof(MeisoDescriptorSetInitStatus) == sizeof(uint8_t),
    "init status must have fixed width");
_Static_assert(sizeof(MeisoDescriptorSetRegisterStatus) == sizeof(uint8_t),
    "register status must have fixed width");
_Static_assert(sizeof(MeisoDescriptorSetSealStatus) == sizeof(uint8_t),
    "seal status must have fixed width");
_Static_assert(sizeof(MeisoDescriptorSetLookupStatus) == sizeof(uint8_t),
    "lookup status must have fixed width");

static MeisoResourceDescriptor descriptor(uint64_t id, MeisoResourceKind kind)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id.high = 0xD35CU;
    value.id.low = id;
    value.kind = kind;
    value.schema.id.high = 0x5C4EU;
    value.schema.id.low = id + 100U;
    value.schema.version = 1U;
    value.owner = MEISO_OWNER_ROLE_HOST;
    value.max_payload_size = 256U + (size_t)id;
    return value;
}

static bool init_set(
    TestArena *arena,
    size_t capacity,
    MeisoDescriptorSet **out_set
)
{
    const size_t required = meiso_descriptor_set_memory_required(capacity);

    return required != 0U && required <= sizeof(arena->bytes) &&
        meiso_descriptor_set_init(
            arena->bytes, required, capacity, out_set
        ) == MEISO_DESCRIPTOR_SET_INIT_OK;
}

static bool test_memory_and_init_contract(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoDescriptorSet *set = (MeisoDescriptorSet *)(uintptr_t)1U;
    const size_t empty_required = meiso_descriptor_set_memory_required(0U);
    const size_t required = meiso_descriptor_set_memory_required(2U);
    size_t index;

    CHECK(meiso_descriptor_set_alignment_required() == _Alignof(max_align_t));
    CHECK(empty_required != 0U);
    CHECK(required > empty_required && required <= sizeof(arena.bytes));
    CHECK(meiso_descriptor_set_memory_required(SIZE_MAX) == 0U);

    CHECK(meiso_descriptor_set_init(
        arena.bytes, required, 2U, NULL
    ) == MEISO_DESCRIPTOR_SET_INIT_INVALID_ARGUMENT);
    CHECK(meiso_descriptor_set_init(
        NULL, required, 2U, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_MISALIGNED_ARENA);
    CHECK(set == NULL);
    CHECK(meiso_descriptor_set_init(
        arena.bytes + 1U, required, 2U, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_MISALIGNED_ARENA);
    CHECK(set == NULL);
    CHECK(meiso_descriptor_set_init(
        arena.bytes, required - 1U, 2U, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_ARENA_TOO_SMALL);
    CHECK(set == NULL);
    CHECK(meiso_descriptor_set_init(
        arena.bytes, required, SIZE_MAX, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_INVALID_CAPACITY);
    CHECK(set == NULL);

    memset(arena.bytes, 0xA5, sizeof(arena.bytes));
    CHECK(meiso_descriptor_set_init(
        arena.bytes,
        required,
        2U,
        (MeisoDescriptorSet **)(void *)arena.bytes
    ) == MEISO_DESCRIPTOR_SET_INIT_INVALID_ARGUMENT);
    for (index = 0U; index < sizeof(MeisoDescriptorSet *); ++index) {
        CHECK(arena.bytes[index] == 0xA5U);
    }

    CHECK(meiso_descriptor_set_init(
        arena.bytes, required, 2U, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_OK);
    CHECK(set != NULL);
    CHECK(meiso_descriptor_set_count(set) == 0U);
    CHECK(meiso_descriptor_set_count(NULL) == 0U);
    return true;
}

static bool test_zero_capacity_empty_snapshot(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoDescriptorSet *set;
    MeisoResourceDescriptor candidate = descriptor(
        1U, MEISO_RESOURCE_KIND_STATE
    );
    MeisoResourceDescriptor output = descriptor(
        99U, MEISO_RESOURCE_KIND_BLOB
    );
    MeisoResourceDescriptor unchanged = output;

    CHECK(init_set(&arena, 0U, &set));
    CHECK(meiso_descriptor_set_register(set, &candidate) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CAPACITY);
    CHECK(meiso_descriptor_set_count(set) == 0U);
    CHECK(meiso_descriptor_set_lookup(set, candidate.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_NOT_SEALED);
    CHECK(meiso_resource_descriptor_equal(&output, &unchanged));
    CHECK(meiso_descriptor_set_seal(set) == MEISO_DESCRIPTOR_SET_SEALED);
    CHECK(meiso_descriptor_set_seal(set) ==
        MEISO_DESCRIPTOR_SET_ALREADY_SEALED);
    CHECK(meiso_descriptor_set_lookup(set, candidate.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_NOT_FOUND);
    CHECK(meiso_resource_descriptor_equal(&output, &unchanged));
    CHECK(meiso_descriptor_set_register(set, &candidate) ==
        MEISO_DESCRIPTOR_SET_REGISTER_SEALED);
    return true;
}

static bool test_registration_identity_and_capacity(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoDescriptorSet *set;
    MeisoResourceDescriptor first = descriptor(
        1U, MEISO_RESOURCE_KIND_STATE
    );
    MeisoResourceDescriptor second = descriptor(
        2U, MEISO_RESOURCE_KIND_STREAM
    );
    MeisoResourceDescriptor third = descriptor(
        3U, MEISO_RESOURCE_KIND_BLOB
    );
    MeisoResourceDescriptor conflict;
    MeisoResourceDescriptor invalid = first;

    CHECK(init_set(&arena, 2U, &set));
    CHECK(meiso_descriptor_set_register(NULL, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTER_INVALID_ARGUMENT);
    CHECK(meiso_descriptor_set_register(set, NULL) ==
        MEISO_DESCRIPTOR_SET_REGISTER_INVALID_ARGUMENT);
    invalid.schema.version = 0U;
    CHECK(meiso_descriptor_set_register(set, &invalid) ==
        MEISO_DESCRIPTOR_SET_REGISTER_INVALID_DESCRIPTOR);
    CHECK(meiso_descriptor_set_count(set) == 0U);

    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTERED);
    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTER_DUPLICATE);
    CHECK(meiso_descriptor_set_count(set) == 1U);

    conflict = first;
    conflict.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_descriptor_set_register(set, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    conflict = first;
    conflict.schema.id.low += 1U;
    CHECK(meiso_descriptor_set_register(set, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    conflict = first;
    conflict.schema.version += 1U;
    CHECK(meiso_descriptor_set_register(set, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    conflict = first;
    conflict.owner = MEISO_OWNER_ROLE_EDGE;
    CHECK(meiso_descriptor_set_register(set, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    conflict = first;
    conflict.max_payload_size += 1U;
    CHECK(meiso_descriptor_set_register(set, &conflict) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CONFLICT);
    CHECK(meiso_descriptor_set_count(set) == 1U);

    CHECK(meiso_descriptor_set_register(set, &second) ==
        MEISO_DESCRIPTOR_SET_REGISTERED);
    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTER_DUPLICATE);
    CHECK(meiso_descriptor_set_register(set, &third) ==
        MEISO_DESCRIPTOR_SET_REGISTER_CAPACITY);
    CHECK(meiso_descriptor_set_count(set) == 2U);
    return true;
}

static bool test_sealed_lookup_value_copy(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoDescriptorSet *set;
    MeisoResourceDescriptor first = descriptor(
        1U, MEISO_RESOURCE_KIND_ACTION
    );
    MeisoResourceDescriptor second = descriptor(
        2U, MEISO_RESOURCE_KIND_BLOB
    );
    MeisoResourceDescriptor output = descriptor(
        99U, MEISO_RESOURCE_KIND_STATE
    );
    MeisoResourceDescriptor unchanged = output;
    MeisoResourceId invalid_id = {0U, 0U};
    MeisoResourceId missing_id = {0xD35CU, 900U};

    CHECK(init_set(&arena, 2U, &set));
    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTERED);
    CHECK(meiso_descriptor_set_register(set, &second) ==
        MEISO_DESCRIPTOR_SET_REGISTERED);
    CHECK(meiso_descriptor_set_lookup(set, first.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_NOT_SEALED);
    CHECK(meiso_resource_descriptor_equal(&output, &unchanged));
    CHECK(meiso_descriptor_set_seal(set) == MEISO_DESCRIPTOR_SET_SEALED);

    CHECK(meiso_descriptor_set_lookup(set, first.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_FOUND);
    CHECK(meiso_resource_descriptor_equal(&output, &first));
    output.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_descriptor_set_lookup(set, first.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_FOUND);
    CHECK(meiso_resource_descriptor_equal(&output, &first));

    output = unchanged;
    CHECK(meiso_descriptor_set_lookup(set, missing_id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_NOT_FOUND);
    CHECK(meiso_resource_descriptor_equal(&output, &unchanged));
    CHECK(meiso_descriptor_set_lookup(set, invalid_id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_INVALID);
    CHECK(meiso_resource_descriptor_equal(&output, &unchanged));
    CHECK(meiso_descriptor_set_lookup(NULL, first.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_INVALID);
    CHECK(meiso_descriptor_set_lookup(set, first.id, NULL) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_INVALID);

    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTER_SEALED);
    first.schema.version = 0U;
    CHECK(meiso_descriptor_set_register(set, &first) ==
        MEISO_DESCRIPTOR_SET_REGISTER_INVALID_DESCRIPTOR);
    CHECK(meiso_descriptor_set_count(set) == 2U);
    return true;
}

static bool test_registration_input_alias(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoDescriptorSet *set;
    const size_t slot_offset = meiso_descriptor_set_memory_required(0U);
    const size_t required = meiso_descriptor_set_memory_required(2U);
    MeisoResourceDescriptor *inside =
        (MeisoResourceDescriptor *)(void *)(arena.bytes + slot_offset);
    MeisoResourceDescriptor expected = descriptor(
        7U, MEISO_RESOURCE_KIND_STREAM
    );
    MeisoResourceDescriptor output;

    CHECK(required <= sizeof(arena.bytes));
    CHECK(meiso_descriptor_set_init(
        arena.bytes, required, 2U, &set
    ) == MEISO_DESCRIPTOR_SET_INIT_OK);
    *inside = expected;
    CHECK(meiso_descriptor_set_register(set, inside) ==
        MEISO_DESCRIPTOR_SET_REGISTERED);
    CHECK(meiso_descriptor_set_seal(set) == MEISO_DESCRIPTOR_SET_SEALED);
    CHECK(meiso_descriptor_set_lookup(set, expected.id, &output) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_FOUND);
    CHECK(meiso_resource_descriptor_equal(&output, &expected));
    CHECK(meiso_descriptor_set_lookup(set, expected.id, inside) ==
        MEISO_DESCRIPTOR_SET_LOOKUP_INVALID);
    CHECK(meiso_descriptor_set_count(set) == 1U);
    return true;
}

int main(void)
{
    if (!test_memory_and_init_contract() ||
        !test_zero_capacity_empty_snapshot() ||
        !test_registration_identity_and_capacity() ||
        !test_sealed_lookup_value_copy() ||
        !test_registration_input_alias()) {
        return 1;
    }
    (void)puts("descriptor set tests passed");
    return 0;
}
