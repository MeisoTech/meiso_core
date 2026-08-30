#include "meiso_four_kind_lab_protocol_private.h"

/* Message codec owns exact M4L1 framing, validation and wire dispatch. */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool edge_scope_valid(MeisoOwnerScope value)
{
    return meiso_owner_scope_valid(value) &&
           value.role == MEISO_OWNER_ROLE_EDGE;
}

static bool action_result_valid(const MeisoLabActionResult *value)
{
    if (value == NULL || !meiso_action_id_valid(value->id) ||
        !edge_scope_valid(value->executor)) {
        return false;
    }
    if (value->state == MEISO_ACTION_STATE_SUCCEEDED) {
        return value->new_window_id != 0U;
    }
    if (value->new_window_id != 0U) {
        return false;
    }
    return value->state == MEISO_ACTION_STATE_ACCEPTED ||
           value->state == MEISO_ACTION_STATE_FAILED ||
           value->state == MEISO_ACTION_STATE_REJECTED ||
           value->state == MEISO_ACTION_STATE_EXPIRED ||
           value->state == MEISO_ACTION_STATE_INDETERMINATE;
}

static bool manifest_exact(MeisoLabBlobManifest value)
{
    return meiso_content_id_equal(
               &value.content_id,
               &meiso_four_kind_lab_protocol_private_manifest_content_id) &&
           value.content.size ==
               sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U &&
           value.content.data != NULL &&
           memcmp(value.content.data,
                  meiso_four_kind_lab_protocol_private_manifest,
                  sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U) ==
               0;
}

bool meiso_four_kind_lab_protocol_private_message_size_and_valid(
    const MeisoLabMessage *message, size_t *out_size)
{
    size_t size;

    if (message == NULL || out_size == NULL ||
        message->registry_generation != MEISO_LAB_REGISTRY_GENERATION) {
        return false;
    }
    switch (message->type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        size = MEISO_FOUR_KIND_LAB_REGISTRY_TOTAL_SIZE;
        if (message->body.registry.descriptor_count !=
            MEISO_LAB_DESCRIPTOR_COUNT) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        size = MEISO_FOUR_KIND_LAB_REGISTRY_ACCEPT_TOTAL_SIZE;
        if (message->body.registry_accept.descriptor_count !=
            MEISO_LAB_DESCRIPTOR_COUNT) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        size = MEISO_FOUR_KIND_LAB_STATE_ACTUAL_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.state_actual.owner) ||
            message->body.state_actual.version == 0U ||
            !meiso_four_kind_lab_protocol_private_edge_state_valid(
                &message->body.state_actual.value)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        size = MEISO_FOUR_KIND_LAB_STATE_ACCEPT_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.state_accept.owner) ||
            message->body.state_accept.version == 0U) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        size = MEISO_FOUR_KIND_LAB_STREAM_METRICS_TOTAL_SIZE;
        if (!edge_scope_valid(message->body.stream_metrics.producer) ||
            message->body.stream_metrics.epoch == 0U ||
            message->body.stream_metrics.sequence == 0U ||
            !meiso_time_point_valid(message->body.stream_metrics.source_time) ||
            !meiso_four_kind_lab_protocol_private_telemetry_sample_valid(
                &message->body.stream_metrics.value)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        size = MEISO_FOUR_KIND_LAB_ACTION_RESET_TOTAL_SIZE;
        if (!meiso_action_id_valid(message->body.action_reset.id) ||
            !edge_scope_valid(message->body.action_reset.executor)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        size = MEISO_FOUR_KIND_LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE;
        if (!meiso_action_id_valid(message->body.action_delivery_accept.id) ||
            !edge_scope_valid(message->body.action_delivery_accept.executor)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        size = MEISO_FOUR_KIND_LAB_ACTION_RESULT_TOTAL_SIZE;
        if (!action_result_valid(&message->body.action_result)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        size = MEISO_LAB_WIRE_HEADER_SIZE + 4U +
               MEISO_FOUR_KIND_LAB_RESOURCE_ID_WIRE_SIZE +
               MEISO_FOUR_KIND_LAB_CONTENT_ID_WIRE_SIZE + 4U +
               (sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U);
        if (!manifest_exact(message->body.blob_manifest)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        size = MEISO_FOUR_KIND_LAB_BLOB_ACCEPT_TOTAL_SIZE;
        if (!meiso_content_id_equal(
                &message->body.blob_accept.content_id,
                &meiso_four_kind_lab_protocol_private_manifest_content_id)) {
            return false;
        }
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        size = MEISO_FOUR_KIND_LAB_CLOSE_TOTAL_SIZE;
        if (message->body.close.status != MEISO_LAB_CLOSE_PASS &&
            message->body.close.status != MEISO_LAB_CLOSE_FAIL) {
            return false;
        }
        break;
    default:
        return false;
    }
    if (size > MEISO_LAB_MAX_MESSAGE_SIZE || size > UINT32_MAX) {
        return false;
    }
    *out_size = size;
    return true;
}

static void write_descriptor(uint8_t *destination,
                             const MeisoResourceDescriptor *descriptor)
{
    meiso_four_kind_lab_protocol_private_write_resource_id(destination,
                                                           descriptor->id);
    destination[16] = descriptor->kind;
    destination[17] = descriptor->owner;
    destination[18] = 0U;
    destination[19] = 0U;
    meiso_four_kind_lab_protocol_private_write_schema_ref(destination + 20U,
                                                          descriptor->schema);
    meiso_four_kind_lab_protocol_private_write_u32(
        destination + 40U, (uint32_t)descriptor->max_payload_size);
}

static bool read_descriptor(const uint8_t *source,
                            MeisoResourceDescriptor *out_descriptor)
{
    MeisoResourceDescriptor value;

    memset(&value, 0, sizeof(value));
    value.id = meiso_four_kind_lab_protocol_private_read_resource_id(source);
    value.kind = source[16];
    value.owner = source[17];
    if (source[18] != 0U || source[19] != 0U) {
        return false;
    }
    value.schema =
        meiso_four_kind_lab_protocol_private_read_schema_ref(source + 20U);
    value.max_payload_size =
        (size_t)meiso_four_kind_lab_protocol_private_read_u32(source + 40U);
    if (meiso_resource_descriptor_validate(&value) != MEISO_DESCRIPTOR_VALID) {
        return false;
    }
    *out_descriptor = value;
    return true;
}

static void write_message_header(uint8_t *output, MeisoLabMessageType type,
                                 size_t total_size)
{
    output[0] = (uint8_t)'M';
    output[1] = (uint8_t)'4';
    output[2] = (uint8_t)'L';
    output[3] = (uint8_t)'1';
    output[4] = MEISO_LAB_WIRE_VERSION;
    output[5] = type;
    output[6] = 0U;
    output[7] = 0U;
    meiso_four_kind_lab_protocol_private_write_u32(output + 8U,
                                                   (uint32_t)total_size);
    meiso_four_kind_lab_protocol_private_write_u32(
        output + MEISO_LAB_WIRE_HEADER_SIZE, MEISO_LAB_REGISTRY_GENERATION);
}

MeisoLabCodecStatus meiso_lab_message_encode(const MeisoLabMessage *message,
                                             MeisoMutableBuffer output,
                                             size_t *out_size)
{
    const MeisoResourceDescriptor *state =
        &meiso_four_kind_lab_protocol_private_descriptors[0];
    const MeisoResourceDescriptor *stream =
        &meiso_four_kind_lab_protocol_private_descriptors[1];
    const MeisoResourceDescriptor *action =
        &meiso_four_kind_lab_protocol_private_descriptors[2];
    const MeisoResourceDescriptor *blob =
        &meiso_four_kind_lab_protocol_private_descriptors[3];
    size_t total_size;
    uint8_t *body;
    size_t index;

    if (message == NULL || out_size == NULL ||
        !meiso_mutable_buffer_valid(output) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            message, sizeof(*message), output.data, output.capacity) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            out_size, sizeof(*out_size), output.data, output.capacity)) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    if (!meiso_four_kind_lab_protocol_private_message_size_and_valid(
            message, &total_size)) {
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    if (output.capacity < total_size) {
        return MEISO_LAB_CODEC_CAPACITY;
    }
    write_message_header(output.data, message->type, total_size);
    body = output.data + MEISO_LAB_WIRE_HEADER_SIZE + 4U;

    switch (message->type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        meiso_four_kind_lab_protocol_private_write_u32(
            body, MEISO_LAB_DESCRIPTOR_COUNT);
        body += 4U;
        for (index = 0U; index < MEISO_LAB_DESCRIPTOR_COUNT; ++index) {
            write_descriptor(
                body, &meiso_four_kind_lab_protocol_private_descriptors[index]);
            body += MEISO_FOUR_KIND_LAB_DESCRIPTOR_WIRE_SIZE;
        }
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        meiso_four_kind_lab_protocol_private_write_u32(
            body, MEISO_LAB_DESCRIPTOR_COUNT);
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        meiso_four_kind_lab_protocol_private_write_resource_id(body, state->id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 16U, message->body.state_actual.owner);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 32U, message->body.state_actual.version);
        meiso_four_kind_lab_protocol_private_encode_edge_state(
            &message->body.state_actual.value, body + 40U);
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        meiso_four_kind_lab_protocol_private_write_resource_id(body, state->id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 16U, message->body.state_accept.owner);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 32U, message->body.state_accept.version);
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        meiso_four_kind_lab_protocol_private_write_resource_id(body,
                                                               stream->id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 16U, message->body.stream_metrics.producer);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 32U, message->body.stream_metrics.epoch);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 40U, message->body.stream_metrics.sequence);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 48U, message->body.stream_metrics.source_time.clock_epoch);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 56U, message->body.stream_metrics.source_time.monotonic_ns);
        meiso_four_kind_lab_protocol_private_encode_telemetry(
            &message->body.stream_metrics.value, body + 64U);
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        meiso_four_kind_lab_protocol_private_write_resource_id(body,
                                                               action->id);
        meiso_four_kind_lab_protocol_private_write_action_id(
            body + 16U, message->body.action_reset.id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 32U, message->body.action_reset.executor);
        meiso_four_kind_lab_protocol_private_write_resource_id(body + 48U,
                                                               stream->id);
        meiso_four_kind_lab_protocol_private_write_schema_ref(body + 64U,
                                                              action->schema);
        body[84] = MEISO_ACTION_RETRY_NONE;
        memset(body + 85U, 0, 3U);
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        meiso_four_kind_lab_protocol_private_write_resource_id(body,
                                                               action->id);
        meiso_four_kind_lab_protocol_private_write_action_id(
            body + 16U, message->body.action_delivery_accept.id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 32U, message->body.action_delivery_accept.executor);
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        meiso_four_kind_lab_protocol_private_write_resource_id(body,
                                                               action->id);
        meiso_four_kind_lab_protocol_private_write_action_id(
            body + 16U, message->body.action_result.id);
        meiso_four_kind_lab_protocol_private_write_scope(
            body + 32U, message->body.action_result.executor);
        body[48] = message->body.action_result.state;
        memset(body + 49U, 0, 7U);
        meiso_four_kind_lab_protocol_private_write_u64(
            body + 56U, message->body.action_result.new_window_id);
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        meiso_four_kind_lab_protocol_private_write_resource_id(body, blob->id);
        meiso_four_kind_lab_protocol_private_write_content_id(
            body + 16U,
            &meiso_four_kind_lab_protocol_private_manifest_content_id);
        meiso_four_kind_lab_protocol_private_write_u32(
            body + 52U,
            (uint32_t)(sizeof(meiso_four_kind_lab_protocol_private_manifest) -
                       1U));
        memmove(body + 56U, meiso_four_kind_lab_protocol_private_manifest,
                sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U);
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        meiso_four_kind_lab_protocol_private_write_resource_id(body, blob->id);
        meiso_four_kind_lab_protocol_private_write_content_id(
            body + 16U,
            &meiso_four_kind_lab_protocol_private_manifest_content_id);
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        body[0] = message->body.close.status;
        memset(body + 1U, 0, 3U);
        break;
    default:
        return MEISO_LAB_CODEC_INVALID_VALUE;
    }
    *out_size = total_size;
    return MEISO_LAB_CODEC_OK;
}

static bool fixed_resource_id(const uint8_t *source, MeisoResourceKind kind)
{
    const MeisoResourceDescriptor *descriptor = meiso_lab_descriptor(kind);
    return descriptor != NULL &&
           meiso_resource_id_equal(
               meiso_four_kind_lab_protocol_private_read_resource_id(source),
               descriptor->id);
}

static MeisoLabCodecStatus decode_header(MeisoByteView input,
                                         MeisoLabMessageType *out_type)
{
    uint32_t declared_size;

    if (input.size < MEISO_LAB_WIRE_HEADER_SIZE) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (input.data[0] != (uint8_t)'M' || input.data[1] != (uint8_t)'4' ||
        input.data[2] != (uint8_t)'L' || input.data[3] != (uint8_t)'1' ||
        input.data[6] != 0U || input.data[7] != 0U) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    if (input.data[4] != MEISO_LAB_WIRE_VERSION) {
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    if (input.data[5] < MEISO_LAB_MESSAGE_REGISTRY ||
        input.data[5] > MEISO_LAB_MESSAGE_CLOSE) {
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    declared_size =
        meiso_four_kind_lab_protocol_private_read_u32(input.data + 8U);
    if (declared_size > MEISO_LAB_MAX_MESSAGE_SIZE ||
        declared_size < MEISO_LAB_WIRE_HEADER_SIZE) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    if ((size_t)declared_size > input.size) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if ((size_t)declared_size != input.size) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    *out_type = input.data[5];
    return MEISO_LAB_CODEC_OK;
}

MeisoLabCodecStatus meiso_lab_message_decode(MeisoByteView input,
                                             MeisoLabMessage *out_message)
{
    MeisoLabMessage message;
    MeisoLabMessageType type;
    MeisoLabCodecStatus header_status;
    const uint8_t *body;
    MeisoResourceDescriptor descriptor;
    MeisoContentId content_id;
    size_t expected_size;
    size_t index;

    if (out_message == NULL || !meiso_byte_view_valid(input) ||
        meiso_four_kind_lab_protocol_private_ranges_overlap(
            input.data, input.size, out_message, sizeof(*out_message))) {
        return MEISO_LAB_CODEC_INVALID_ARGUMENT;
    }
    header_status = decode_header(input, &type);
    if (header_status != MEISO_LAB_CODEC_OK) {
        return header_status;
    }
    if (input.size < MEISO_LAB_WIRE_HEADER_SIZE + 4U) {
        return MEISO_LAB_CODEC_TRUNCATED;
    }
    if (meiso_four_kind_lab_protocol_private_read_u32(
            input.data + MEISO_LAB_WIRE_HEADER_SIZE) !=
        MEISO_LAB_REGISTRY_GENERATION) {
        return MEISO_LAB_CODEC_MALFORMED;
    }
    memset(&message, 0, sizeof(message));
    message.type = type;
    message.registry_generation = MEISO_LAB_REGISTRY_GENERATION;
    body = input.data + MEISO_LAB_WIRE_HEADER_SIZE + 4U;

    switch (type) {
    case MEISO_LAB_MESSAGE_REGISTRY:
        expected_size = MEISO_FOUR_KIND_LAB_REGISTRY_TOTAL_SIZE;
        if (input.size != expected_size ||
            meiso_four_kind_lab_protocol_private_read_u32(body) !=
                MEISO_LAB_DESCRIPTOR_COUNT) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        body += 4U;
        for (index = 0U; index < MEISO_LAB_DESCRIPTOR_COUNT; ++index) {
            if (!read_descriptor(body, &descriptor) ||
                !meiso_resource_descriptor_equal(
                    &descriptor,
                    &meiso_four_kind_lab_protocol_private_descriptors[index])) {
                return MEISO_LAB_CODEC_MALFORMED;
            }
            body += MEISO_FOUR_KIND_LAB_DESCRIPTOR_WIRE_SIZE;
        }
        message.body.registry.descriptor_count = MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_REGISTRY_ACCEPT:
        if (input.size != MEISO_FOUR_KIND_LAB_REGISTRY_ACCEPT_TOTAL_SIZE ||
            meiso_four_kind_lab_protocol_private_read_u32(body) !=
                MEISO_LAB_DESCRIPTOR_COUNT) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.registry_accept.descriptor_count =
            MEISO_LAB_DESCRIPTOR_COUNT;
        break;
    case MEISO_LAB_MESSAGE_STATE_ACTUAL:
        if (input.size != MEISO_FOUR_KIND_LAB_STATE_ACTUAL_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STATE) ||
            !meiso_four_kind_lab_protocol_private_read_scope(
                body + 16U, &message.body.state_actual.owner) ||
            !edge_scope_valid(message.body.state_actual.owner)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.state_actual.version =
            meiso_four_kind_lab_protocol_private_read_u64(body + 32U);
        if (message.body.state_actual.version == 0U ||
            !meiso_four_kind_lab_protocol_private_decode_edge_state(
                body + 40U, &message.body.state_actual.value)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_STATE_ACCEPT:
        if (input.size != MEISO_FOUR_KIND_LAB_STATE_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STATE) ||
            !meiso_four_kind_lab_protocol_private_read_scope(
                body + 16U, &message.body.state_accept.owner) ||
            !edge_scope_valid(message.body.state_accept.owner)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.state_accept.version =
            meiso_four_kind_lab_protocol_private_read_u64(body + 32U);
        if (message.body.state_accept.version == 0U) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_STREAM_METRICS:
        if (input.size != MEISO_FOUR_KIND_LAB_STREAM_METRICS_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_STREAM) ||
            !meiso_four_kind_lab_protocol_private_read_scope(
                body + 16U, &message.body.stream_metrics.producer) ||
            !edge_scope_valid(message.body.stream_metrics.producer)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.stream_metrics.epoch =
            meiso_four_kind_lab_protocol_private_read_u64(body + 32U);
        message.body.stream_metrics.sequence =
            meiso_four_kind_lab_protocol_private_read_u64(body + 40U);
        message.body.stream_metrics.source_time.clock_epoch =
            meiso_four_kind_lab_protocol_private_read_u64(body + 48U);
        message.body.stream_metrics.source_time.monotonic_ns =
            meiso_four_kind_lab_protocol_private_read_u64(body + 56U);
        if (message.body.stream_metrics.epoch == 0U ||
            message.body.stream_metrics.sequence == 0U ||
            !meiso_time_point_valid(message.body.stream_metrics.source_time) ||
            !meiso_four_kind_lab_protocol_private_decode_telemetry(
                body + 64U, &message.body.stream_metrics.value)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESET:
        if (input.size != MEISO_FOUR_KIND_LAB_ACTION_RESET_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION) ||
            !fixed_resource_id(body + 48U, MEISO_RESOURCE_KIND_STREAM) ||
            !meiso_schema_ref_equal(
                meiso_four_kind_lab_protocol_private_read_schema_ref(body +
                                                                     64U),
                meiso_four_kind_lab_protocol_private_descriptors[2].schema) ||
            body[84] != MEISO_ACTION_RETRY_NONE ||
            !meiso_four_kind_lab_protocol_private_bytes_zero(body + 85U, 3U)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_reset.id =
            meiso_four_kind_lab_protocol_private_read_action_id(body + 16U);
        if (!meiso_four_kind_lab_protocol_private_read_scope(
                body + 32U, &message.body.action_reset.executor) ||
            !meiso_action_id_valid(message.body.action_reset.id) ||
            !edge_scope_valid(message.body.action_reset.executor)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT:
        if (input.size !=
                MEISO_FOUR_KIND_LAB_ACTION_DELIVERY_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_delivery_accept.id =
            meiso_four_kind_lab_protocol_private_read_action_id(body + 16U);
        if (!meiso_four_kind_lab_protocol_private_read_scope(
                body + 32U, &message.body.action_delivery_accept.executor) ||
            !meiso_action_id_valid(message.body.action_delivery_accept.id) ||
            !edge_scope_valid(message.body.action_delivery_accept.executor)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_ACTION_RESULT:
        if (input.size != MEISO_FOUR_KIND_LAB_ACTION_RESULT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_ACTION) ||
            !meiso_four_kind_lab_protocol_private_bytes_zero(body + 49U, 7U)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_result.id =
            meiso_four_kind_lab_protocol_private_read_action_id(body + 16U);
        if (!meiso_four_kind_lab_protocol_private_read_scope(
                body + 32U, &message.body.action_result.executor)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.action_result.state = body[48];
        message.body.action_result.new_window_id =
            meiso_four_kind_lab_protocol_private_read_u64(body + 56U);
        if (!action_result_valid(&message.body.action_result)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        break;
    case MEISO_LAB_MESSAGE_BLOB_MANIFEST:
        expected_size =
            MEISO_LAB_WIRE_HEADER_SIZE + 4U +
            MEISO_FOUR_KIND_LAB_RESOURCE_ID_WIRE_SIZE +
            MEISO_FOUR_KIND_LAB_CONTENT_ID_WIRE_SIZE + 4U +
            (sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U);
        if (input.size != expected_size ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_BLOB) ||
            !meiso_four_kind_lab_protocol_private_read_content_id(
                body + 16U, &content_id) ||
            !meiso_content_id_equal(
                &content_id,
                &meiso_four_kind_lab_protocol_private_manifest_content_id) ||
            meiso_four_kind_lab_protocol_private_read_u32(body + 52U) !=
                sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U ||
            memcmp(body + 56U, meiso_four_kind_lab_protocol_private_manifest,
                   sizeof(meiso_four_kind_lab_protocol_private_manifest) -
                       1U) != 0) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.blob_manifest.content_id = content_id;
        message.body.blob_manifest.content = (MeisoByteView){
            body + 56U,
            sizeof(meiso_four_kind_lab_protocol_private_manifest) - 1U};
        break;
    case MEISO_LAB_MESSAGE_BLOB_ACCEPT:
        if (input.size != MEISO_FOUR_KIND_LAB_BLOB_ACCEPT_TOTAL_SIZE ||
            !fixed_resource_id(body, MEISO_RESOURCE_KIND_BLOB) ||
            !meiso_four_kind_lab_protocol_private_read_content_id(
                body + 16U, &content_id) ||
            !meiso_content_id_equal(
                &content_id,
                &meiso_four_kind_lab_protocol_private_manifest_content_id)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.blob_accept.content_id = content_id;
        break;
    case MEISO_LAB_MESSAGE_CLOSE:
        if (input.size != MEISO_FOUR_KIND_LAB_CLOSE_TOTAL_SIZE ||
            !meiso_four_kind_lab_protocol_private_bytes_zero(body + 1U, 3U) ||
            (body[0] != MEISO_LAB_CLOSE_PASS &&
             body[0] != MEISO_LAB_CLOSE_FAIL)) {
            return MEISO_LAB_CODEC_MALFORMED;
        }
        message.body.close.status = body[0];
        break;
    default:
        return MEISO_LAB_CODEC_UNSUPPORTED;
    }
    *out_message = message;
    return MEISO_LAB_CODEC_OK;
}
