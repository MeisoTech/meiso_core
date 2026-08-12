#ifndef MEISO_RESOURCE_H
#define MEISO_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_CONTENT_DIGEST_MAX_SIZE 64U
#define MEISO_SHA256_DIGEST_SIZE 32U

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoResourceId;

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoSchemaId;

typedef struct {
    uint64_t high;
    uint64_t low;
} MeisoActionId;

typedef enum {
    MEISO_RESOURCE_KIND_STATE = 1,
    MEISO_RESOURCE_KIND_STREAM = 2,
    MEISO_RESOURCE_KIND_ACTION = 3,
    MEISO_RESOURCE_KIND_BLOB = 4
} MeisoResourceKind;

typedef enum {
    MEISO_OWNER_ROLE_HOST = 1,
    MEISO_OWNER_ROLE_EDGE = 2
} MeisoOwnerRole;

typedef struct {
    MeisoOwnerRole role;
    uint64_t incarnation;
} MeisoOwnerScope;

typedef struct {
    MeisoSchemaId id;
    uint32_t version;
} MeisoSchemaRef;

typedef struct {
    const uint8_t *data;
    size_t size;
} MeisoByteView;

typedef struct {
    uint8_t *data;
    size_t capacity;
} MeisoMutableBuffer;

typedef struct {
    uint64_t clock_epoch;
    uint64_t monotonic_ns;
} MeisoTimePoint;

typedef struct {
    bool present;
    MeisoTimePoint value;
} MeisoDeadline;

typedef struct {
    uint64_t value;
} MeisoDeliveryScope;

typedef enum {
    MEISO_CONTENT_ALGORITHM_SHA256 = 1
} MeisoContentAlgorithm;

typedef struct {
    uint16_t algorithm;
    uint8_t digest_size;
    uint8_t digest[MEISO_CONTENT_DIGEST_MAX_SIZE];
} MeisoContentId;

typedef struct {
    MeisoResourceId id;
    MeisoResourceKind kind;
    MeisoSchemaRef schema;
    MeisoOwnerRole owner;
    size_t max_payload_size;
} MeisoResourceDescriptor;

typedef enum {
    MEISO_DESCRIPTOR_VALID = 0,
    MEISO_DESCRIPTOR_INVALID_ARGUMENT = 1,
    MEISO_DESCRIPTOR_INVALID_RESOURCE_ID = 2,
    MEISO_DESCRIPTOR_INVALID_KIND = 3,
    MEISO_DESCRIPTOR_INVALID_SCHEMA = 4,
    MEISO_DESCRIPTOR_INVALID_OWNER = 5
} MeisoDescriptorStatus;

bool meiso_resource_id_equal(MeisoResourceId lhs, MeisoResourceId rhs);
bool meiso_resource_id_valid(MeisoResourceId value);
bool meiso_schema_id_equal(MeisoSchemaId lhs, MeisoSchemaId rhs);
bool meiso_schema_id_valid(MeisoSchemaId value);
bool meiso_action_id_equal(MeisoActionId lhs, MeisoActionId rhs);
bool meiso_action_id_valid(MeisoActionId value);
bool meiso_owner_role_valid(MeisoOwnerRole value);
bool meiso_owner_scope_valid(MeisoOwnerScope value);
bool meiso_owner_scope_equal(MeisoOwnerScope lhs, MeisoOwnerScope rhs);
bool meiso_byte_view_valid(MeisoByteView value);
bool meiso_mutable_buffer_valid(MeisoMutableBuffer value);
bool meiso_time_point_valid(MeisoTimePoint value);
bool meiso_deadline_valid(MeisoDeadline value);
bool meiso_delivery_scope_valid(MeisoDeliveryScope value);
bool meiso_content_id_valid(const MeisoContentId *value);
bool meiso_content_id_equal(const MeisoContentId *lhs, const MeisoContentId *rhs);
bool meiso_schema_ref_equal(MeisoSchemaRef lhs, MeisoSchemaRef rhs);
MeisoDescriptorStatus meiso_resource_descriptor_validate(
    const MeisoResourceDescriptor *descriptor
);
bool meiso_resource_descriptor_equal(
    const MeisoResourceDescriptor *lhs,
    const MeisoResourceDescriptor *rhs
);

#ifdef __cplusplus
}
#endif

#endif
