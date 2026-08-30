#ifndef MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE_H
#define MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE_H

/*
 * Build-private and non-installed. The protocol owns immutable process-static
 * catalog/manifest storage; returned public views borrow it for process life.
 * Callers own all codec buffers. Helpers allocate nothing and are reentrant.
 */

#include <meiso/internal/lab/four_kind_protocol.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE                                   \
    __attribute__((visibility("hidden")))
#else
#define MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE
#endif

enum {
    MEISO_FOUR_KIND_LAB_DESCRIPTOR_WIRE_SIZE = 44U,
    MEISO_FOUR_KIND_LAB_CONTENT_ID_WIRE_SIZE = 36U,
    MEISO_FOUR_KIND_LAB_SCOPE_WIRE_SIZE = 16U,
    MEISO_FOUR_KIND_LAB_RESOURCE_ID_WIRE_SIZE = 16U,
    MEISO_FOUR_KIND_LAB_ACTION_ID_WIRE_SIZE = 16U,
    MEISO_FOUR_KIND_LAB_SCHEMA_REF_WIRE_SIZE = 20U,
    MEISO_FOUR_KIND_LAB_REGISTRY_TOTAL_SIZE = 196U,
    MEISO_FOUR_KIND_LAB_REGISTRY_ACCEPT_TOTAL_SIZE = 20U,
    MEISO_FOUR_KIND_LAB_STATE_ACTUAL_TOTAL_SIZE = 80U,
    MEISO_FOUR_KIND_LAB_STATE_ACCEPT_TOTAL_SIZE = 56U,
    MEISO_FOUR_KIND_LAB_STREAM_METRICS_TOTAL_SIZE = 160U,
    MEISO_FOUR_KIND_LAB_ACTION_RESET_TOTAL_SIZE = 104U,
    MEISO_FOUR_KIND_LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE = 64U,
    MEISO_FOUR_KIND_LAB_ACTION_RESULT_TOTAL_SIZE = 80U,
    MEISO_FOUR_KIND_LAB_BLOB_ACCEPT_TOTAL_SIZE = 68U,
    MEISO_FOUR_KIND_LAB_CLOSE_TOTAL_SIZE = 20U
};

enum {
    MEISO_FOUR_KIND_LAB_EDGE_STATE_INTERFACE_PRESENT = 1U,
    MEISO_FOUR_KIND_LAB_EDGE_STATE_CARRIER_PRESENT = 2U,
    MEISO_FOUR_KIND_LAB_EDGE_STATE_TEMPERATURE_PRESENT = 8U,
    MEISO_FOUR_KIND_LAB_EDGE_STATE_KNOWN_FLAGS = 11U,
    MEISO_FOUR_KIND_LAB_TELEMETRY_SIGNAL_PRESENT = 1U,
    MEISO_FOUR_KIND_LAB_TELEMETRY_TEMPERATURE_PRESENT = 2U,
    MEISO_FOUR_KIND_LAB_TELEMETRY_KNOWN_FLAGS = 3U
};

MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE extern const uint8_t
    meiso_four_kind_lab_protocol_private_manifest[402U];
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE extern const MeisoContentId
    meiso_four_kind_lab_protocol_private_manifest_content_id;
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE extern const MeisoResourceDescriptor
    meiso_four_kind_lab_protocol_private_descriptors
        [MEISO_LAB_DESCRIPTOR_COUNT];

MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_ranges_overlap(const void *lhs,
                                                    size_t lhs_size,
                                                    const void *rhs,
                                                    size_t rhs_size);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_u16(uint8_t *destination,
                                               uint16_t value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_u32(uint8_t *destination,
                                               uint32_t value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_u64(uint8_t *destination,
                                               uint64_t value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE uint16_t
meiso_four_kind_lab_protocol_private_read_u16(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE uint32_t
meiso_four_kind_lab_protocol_private_read_u32(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE uint64_t
meiso_four_kind_lab_protocol_private_read_u64(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_resource_id(uint8_t *destination,
                                                       MeisoResourceId value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE MeisoResourceId
meiso_four_kind_lab_protocol_private_read_resource_id(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_schema_ref(uint8_t *destination,
                                                      MeisoSchemaRef value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE MeisoSchemaRef
meiso_four_kind_lab_protocol_private_read_schema_ref(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_action_id(uint8_t *destination,
                                                     MeisoActionId value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE MeisoActionId
meiso_four_kind_lab_protocol_private_read_action_id(const uint8_t *source);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_scope(uint8_t *destination,
                                                 MeisoOwnerScope value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_read_scope(const uint8_t *source,
                                                MeisoOwnerScope *out_scope);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_write_content_id(
    uint8_t *destination, const MeisoContentId *value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_read_content_id(const uint8_t *source,
                                                     MeisoContentId *out_value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_bytes_zero(const uint8_t *bytes,
                                                size_t size);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_edge_state_valid(
    const MeisoLabEdgeState *value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_telemetry_sample_valid(
    const MeisoLabTelemetrySample *value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_encode_edge_state(
    const MeisoLabEdgeState *value, uint8_t *output);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_decode_edge_state(
    const uint8_t *input, MeisoLabEdgeState *out_value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE void
meiso_four_kind_lab_protocol_private_encode_telemetry(
    const MeisoLabTelemetrySample *value, uint8_t *output);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_decode_telemetry(
    const uint8_t *input, MeisoLabTelemetrySample *out_value);
MEISO_FOUR_KIND_LAB_PROTOCOL_PRIVATE bool
meiso_four_kind_lab_protocol_private_message_size_and_valid(
    const MeisoLabMessage *message, size_t *out_size);

#endif
