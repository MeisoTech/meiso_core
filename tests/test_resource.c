#include "meiso_resource.h"

#include "meiso_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

_Static_assert(sizeof(MeisoResourceKind) == sizeof(uint8_t), "fixed resource kind");
_Static_assert(sizeof(MeisoOwnerRole) == sizeof(uint8_t), "fixed owner role");
_Static_assert(sizeof(MeisoDescriptorStatus) == sizeof(uint8_t), "fixed status");
_Static_assert(sizeof(MeisoContentAlgorithm) == sizeof(uint16_t), "fixed algorithm");

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #condition);                            \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

static MeisoResourceDescriptor valid_descriptor(void)
{
    MeisoResourceDescriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = (MeisoResourceId){1U, 2U};
    descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    descriptor.schema.id = (MeisoSchemaId){3U, 4U};
    descriptor.schema.version = 1U;
    descriptor.owner = MEISO_OWNER_ROLE_EDGE;
    descriptor.max_payload_size = 0U;
    return descriptor;
}

static void test_distinct_identity_types(void)
{
    const MeisoResourceId resource = {0U, 7U};
    const MeisoSchemaId schema = {0U, 7U};
    const MeisoActionId action = {0U, 7U};

    CHECK(meiso_resource_id_valid(resource));
    CHECK(meiso_schema_id_valid(schema));
    CHECK(meiso_action_id_valid(action));
    CHECK(meiso_resource_id_equal(resource, (MeisoResourceId){0U, 7U}));
    CHECK(!meiso_resource_id_equal(resource, (MeisoResourceId){7U, 0U}));
    CHECK(!meiso_resource_id_valid((MeisoResourceId){0U, 0U}));
    CHECK(!meiso_schema_id_valid((MeisoSchemaId){0U, 0U}));
    CHECK(!meiso_action_id_valid((MeisoActionId){0U, 0U}));
}

static void test_borrowed_views_and_time(void)
{
    uint8_t byte = 9U;
    const MeisoByteView empty = {NULL, 0U};
    const MeisoMutableBuffer empty_mutable = {NULL, 0U};

    CHECK(meiso_byte_view_valid(empty));
    CHECK(meiso_mutable_buffer_valid(empty_mutable));
    CHECK(meiso_byte_view_valid((MeisoByteView){&byte, 1U}));
    CHECK(meiso_mutable_buffer_valid((MeisoMutableBuffer){&byte, 1U}));
    CHECK(!meiso_byte_view_valid((MeisoByteView){NULL, 1U}));
    CHECK(!meiso_mutable_buffer_valid((MeisoMutableBuffer){NULL, 1U}));
    CHECK(meiso_time_point_valid((MeisoTimePoint){1U, 0U}));
    CHECK(!meiso_time_point_valid((MeisoTimePoint){0U, 10U}));
    CHECK(meiso_deadline_valid((MeisoDeadline){false, {0U, 0U}}));
    CHECK(meiso_deadline_valid((MeisoDeadline){true, {2U, 10U}}));
    CHECK(!meiso_deadline_valid((MeisoDeadline){true, {0U, 10U}}));
    CHECK(meiso_delivery_generation_valid((MeisoDeliveryGeneration){1U}));
    CHECK(!meiso_delivery_generation_valid((MeisoDeliveryGeneration){0U}));
}

static void test_owner_scope(void)
{
    const MeisoOwnerScope first = {MEISO_OWNER_ROLE_HOST, 4U};
    MeisoOwnerScope padded_first;
    MeisoOwnerScope padded_second;

    CHECK(meiso_owner_scope_valid(first));
    CHECK(meiso_owner_scope_equal(first, (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 4U}));
    CHECK(!meiso_owner_scope_equal(first, (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 4U}));
    CHECK(!meiso_owner_scope_valid((MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 0U}));
    CHECK(!meiso_owner_scope_valid((MeisoOwnerScope){(MeisoOwnerRole)0, 4U}));

    memset(&padded_first, 0xaa, sizeof(padded_first));
    memset(&padded_second, 0x55, sizeof(padded_second));
    padded_first.role = MEISO_OWNER_ROLE_HOST;
    padded_first.incarnation = 4U;
    padded_second.role = MEISO_OWNER_ROLE_HOST;
    padded_second.incarnation = 4U;
    CHECK(meiso_owner_scope_equal(padded_first, padded_second));
}

static void test_content_identity(void)
{
    MeisoContentId first;
    MeisoContentId second;

    memset(&first, 0, sizeof(first));
    first.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    first.digest_size = MEISO_SHA256_DIGEST_SIZE;
    first.digest[0] = 0x12U;
    first.digest[1] = 0x34U;
    second = first;

    CHECK(meiso_content_id_valid(&first));
    CHECK(meiso_content_id_equal(&first, &second));
    second.digest[MEISO_SHA256_DIGEST_SIZE] = 0xffU;
    CHECK(meiso_content_id_equal(&first, &second));
    second.digest[1] = 0x35U;
    CHECK(!meiso_content_id_equal(&first, &second));
    first.digest_size = MEISO_SHA256_DIGEST_SIZE - 1U;
    CHECK(!meiso_content_id_valid(&first));
    first.digest_size = MEISO_SHA256_DIGEST_SIZE + 1U;
    CHECK(!meiso_content_id_valid(&first));
    first.digest_size = MEISO_CONTENT_DIGEST_MAX_SIZE;
    CHECK(!meiso_content_id_valid(&first));
    first.digest_size = MEISO_SHA256_DIGEST_SIZE;
    first.algorithm = 0xffffU;
    CHECK(!meiso_content_id_valid(&first));
    CHECK(!meiso_content_id_valid(NULL));
    CHECK(!meiso_content_id_equal(NULL, &second));
}

static void test_descriptor_validation(void)
{
    MeisoResourceDescriptor descriptor = valid_descriptor();
    MeisoResourceDescriptor same = descriptor;
    MeisoResourceDescriptor padded;

    CHECK(meiso_resource_descriptor_validate(NULL) ==
          MEISO_DESCRIPTOR_INVALID_ARGUMENT);
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_VALID);
    CHECK(meiso_resource_descriptor_equal(&descriptor, &same));
    CHECK(!meiso_resource_descriptor_equal(NULL, &same));

    memset(&padded, 0xa5, sizeof(padded));
    padded.id = descriptor.id;
    padded.kind = descriptor.kind;
    padded.schema = descriptor.schema;
    padded.owner = descriptor.owner;
    padded.max_payload_size = descriptor.max_payload_size;
    CHECK(meiso_resource_descriptor_equal(&descriptor, &padded));

    descriptor.id = (MeisoResourceId){0U, 0U};
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_INVALID_RESOURCE_ID);
    descriptor = valid_descriptor();
    descriptor.kind = (MeisoResourceKind)0;
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_INVALID_KIND);
    descriptor = valid_descriptor();
    descriptor.schema.id = (MeisoSchemaId){0U, 0U};
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_INVALID_SCHEMA);
    descriptor = valid_descriptor();
    descriptor.schema.version = 0U;
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_INVALID_SCHEMA);
    descriptor = valid_descriptor();
    descriptor.owner = (MeisoOwnerRole)0;
    CHECK(meiso_resource_descriptor_validate(&descriptor) ==
          MEISO_DESCRIPTOR_INVALID_OWNER);

    descriptor = valid_descriptor();
    same = descriptor;
    same.max_payload_size = 1U;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
    same = descriptor;
    same.id.low += 1U;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
    same = descriptor;
    same.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
    same = descriptor;
    same.schema.id.low += 1U;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
    same = descriptor;
    same.schema.version += 1U;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
    same = descriptor;
    same.owner = MEISO_OWNER_ROLE_HOST;
    CHECK(!meiso_resource_descriptor_equal(&descriptor, &same));
}

static void test_saturating_counter(void)
{
    uint64_t value = UINT64_MAX - 1U;

    meiso_increment_saturated(&value);
    CHECK(value == UINT64_MAX);
    meiso_increment_saturated(&value);
    CHECK(value == UINT64_MAX);
}

int main(void)
{
    test_distinct_identity_types();
    test_borrowed_views_and_time();
    test_owner_scope();
    test_content_identity();
    test_descriptor_validation();
    test_saturating_counter();

    if (failures != 0) {
        fprintf(stderr, "%d resource test(s) failed\n", failures);
        return 1;
    }
    puts("resource tests passed");
    return 0;
}
