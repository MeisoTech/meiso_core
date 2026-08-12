#include "meiso_resource.h"

#include <string.h>

static bool resource_kind_valid(MeisoResourceKind value)
{
    return value == MEISO_RESOURCE_KIND_STATE ||
        value == MEISO_RESOURCE_KIND_STREAM ||
        value == MEISO_RESOURCE_KIND_ACTION ||
        value == MEISO_RESOURCE_KIND_BLOB;
}

bool meiso_resource_id_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_resource_id_valid(MeisoResourceId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_schema_id_equal(MeisoSchemaId lhs, MeisoSchemaId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_schema_id_valid(MeisoSchemaId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_action_id_equal(MeisoActionId lhs, MeisoActionId rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool meiso_action_id_valid(MeisoActionId value)
{
    return value.high != 0U || value.low != 0U;
}

bool meiso_owner_role_valid(MeisoOwnerRole value)
{
    return value == MEISO_OWNER_ROLE_HOST || value == MEISO_OWNER_ROLE_EDGE;
}

bool meiso_owner_scope_valid(MeisoOwnerScope value)
{
    return meiso_owner_role_valid(value.role) && value.incarnation != 0U;
}

bool meiso_owner_scope_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs)
{
    return lhs.role == rhs.role && lhs.incarnation == rhs.incarnation;
}

bool meiso_byte_view_valid(MeisoByteView value)
{
    return value.size == 0U || value.data != NULL;
}

bool meiso_mutable_buffer_valid(MeisoMutableBuffer value)
{
    return value.capacity == 0U || value.data != NULL;
}

bool meiso_time_point_valid(MeisoTimePoint value)
{
    return value.clock_epoch != 0U;
}

bool meiso_deadline_valid(MeisoDeadline value)
{
    return !value.present || meiso_time_point_valid(value.value);
}

bool meiso_delivery_generation_valid(MeisoDeliveryGeneration value)
{
    return value.value != 0U;
}

bool meiso_content_id_valid(const MeisoContentId *value)
{
    if (value == NULL) {
        return false;
    }
    switch (value->algorithm) {
    case MEISO_CONTENT_ALGORITHM_SHA256:
        return value->digest_size == MEISO_SHA256_DIGEST_SIZE;
    default:
        return false;
    }
}

bool meiso_content_id_equal(const MeisoContentId *lhs, const MeisoContentId *rhs)
{
    if (!meiso_content_id_valid(lhs) || !meiso_content_id_valid(rhs)) {
        return false;
    }
    return lhs->algorithm == rhs->algorithm &&
        lhs->digest_size == rhs->digest_size &&
        memcmp(lhs->digest, rhs->digest, lhs->digest_size) == 0;
}

bool meiso_schema_ref_equal(MeisoSchemaRef lhs, MeisoSchemaRef rhs)
{
    return lhs.version == rhs.version && meiso_schema_id_equal(lhs.id, rhs.id);
}

MeisoDescriptorStatus meiso_resource_descriptor_validate(
    const MeisoResourceDescriptor *descriptor
)
{
    if (descriptor == NULL) {
        return MEISO_DESCRIPTOR_INVALID_ARGUMENT;
    }
    if (!meiso_resource_id_valid(descriptor->id)) {
        return MEISO_DESCRIPTOR_INVALID_RESOURCE_ID;
    }
    if (!resource_kind_valid(descriptor->kind)) {
        return MEISO_DESCRIPTOR_INVALID_KIND;
    }
    if (!meiso_schema_id_valid(descriptor->schema.id) ||
        descriptor->schema.version == 0U) {
        return MEISO_DESCRIPTOR_INVALID_SCHEMA;
    }
    if (!meiso_owner_role_valid(descriptor->owner)) {
        return MEISO_DESCRIPTOR_INVALID_OWNER;
    }
    return MEISO_DESCRIPTOR_VALID;
}

bool meiso_resource_descriptor_equal(
    const MeisoResourceDescriptor *lhs,
    const MeisoResourceDescriptor *rhs
)
{
    if (meiso_resource_descriptor_validate(lhs) != MEISO_DESCRIPTOR_VALID ||
        meiso_resource_descriptor_validate(rhs) != MEISO_DESCRIPTOR_VALID) {
        return false;
    }
    return meiso_resource_id_equal(lhs->id, rhs->id) &&
        lhs->kind == rhs->kind &&
        meiso_schema_ref_equal(lhs->schema, rhs->schema) &&
        lhs->owner == rhs->owner &&
        lhs->max_payload_size == rhs->max_payload_size;
}
