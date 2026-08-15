#ifndef MEISO_FOUR_KIND_LAB_PROTOCOL_H
#define MEISO_FOUR_KIND_LAB_PROTOCOL_H

#include "meiso_action.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_LAB_WIRE_VERSION 1U
#define MEISO_LAB_WIRE_HEADER_SIZE 12U
#define MEISO_LAB_MAX_MESSAGE_SIZE 512U
#define MEISO_LAB_REGISTRY_GENERATION 1U
#define MEISO_LAB_DESCRIPTOR_COUNT 4U
#define MEISO_LAB_EDGE_STATE_WIRE_SIZE 24U
#define MEISO_LAB_TELEMETRY_SAMPLE_WIRE_SIZE 80U

#define MEISO_LAB_PROFILE_ID_HIGH UINT64_C(0x464f55525f4b494e)
#define MEISO_LAB_PROFILE_ID_LOW UINT64_C(0x445f4c41425f5630)
#define MEISO_LAB_PROFILE_VERSION 1U
#define MEISO_LAB_PROTOCOL_MAJOR 1U
#define MEISO_LAB_PROTOCOL_MINOR 0U
#define MEISO_LAB_HOST_PEER_ID_HIGH UINT64_C(0x4d344c315f484f53)
#define MEISO_LAB_HOST_PEER_ID_LOW UINT64_C(0x545f4c41425f5630)
#define MEISO_LAB_EDGE_PEER_ID_HIGH UINT64_C(0x4d344c315f454447)
#define MEISO_LAB_EDGE_PEER_ID_LOW UINT64_C(0x455f4c41425f5630)
#define MEISO_LAB_TCP_PORT 27640U
#define MEISO_LAB_SESSION_MAX_FRAME_BYTES 640U
#define MEISO_LAB_SESSION_MAX_RUNTIME_PAYLOAD_BYTES 512U
#define MEISO_LAB_SESSION_MAX_REGISTRY_ENTRIES 4U

#define MEISO_LAB_SIGNAL_MIN_MBM (-20000)
#define MEISO_LAB_SIGNAL_MAX_MBM 0
#define MEISO_LAB_TEMPERATURE_MIN_MILLIC (-40000)
#define MEISO_LAB_TEMPERATURE_MAX_MILLIC 125000
#define MEISO_LAB_TEMPERATURE_UNCERTAINTY_MILLIC 3000U

typedef uint8_t MeisoLabMessageType;
enum {
    MEISO_LAB_MESSAGE_REGISTRY = 1,
    MEISO_LAB_MESSAGE_REGISTRY_ACCEPT = 2,
    MEISO_LAB_MESSAGE_STATE_ACTUAL = 3,
    MEISO_LAB_MESSAGE_STATE_ACCEPT = 4,
    MEISO_LAB_MESSAGE_STREAM_METRICS = 5,
    MEISO_LAB_MESSAGE_ACTION_RESET = 6,
    MEISO_LAB_MESSAGE_ACTION_DELIVERY_ACCEPT = 7,
    MEISO_LAB_MESSAGE_ACTION_RESULT = 8,
    MEISO_LAB_MESSAGE_BLOB_MANIFEST = 9,
    MEISO_LAB_MESSAGE_BLOB_ACCEPT = 10,
    MEISO_LAB_MESSAGE_CLOSE = 11
};

typedef uint8_t MeisoLabSender;
enum {
    MEISO_LAB_SENDER_HOST = 1,
    MEISO_LAB_SENDER_EDGE = 2
};

typedef uint8_t MeisoLabCloseStatus;
enum {
    MEISO_LAB_CLOSE_PASS = 1,
    MEISO_LAB_CLOSE_FAIL = 2
};

typedef struct {
    uint64_t window_id;
    bool interface_present;
    bool carrier_present;
    bool temperature_present;
    int32_t corrected_millic;
    uint32_t uncertainty_millic;
} MeisoLabEdgeState;

typedef struct {
    uint64_t window_id;
    uint64_t elapsed_ns;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    int32_t signal_mbm;
    bool signal_present;
    int32_t corrected_millic;
    uint32_t uncertainty_millic;
    bool temperature_present;
} MeisoLabTelemetrySample;

typedef struct {
    uint32_t descriptor_count;
} MeisoLabRegistry;

typedef struct {
    MeisoOwnerScope owner;
    uint64_t version;
    MeisoLabEdgeState value;
} MeisoLabStateActual;

typedef struct {
    MeisoOwnerScope owner;
    uint64_t version;
} MeisoLabStateAccept;

typedef struct {
    MeisoOwnerScope producer;
    uint64_t epoch;
    uint64_t sequence;
    MeisoTimePoint source_time;
    MeisoLabTelemetrySample value;
} MeisoLabStreamMetrics;

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope executor;
} MeisoLabActionReset;

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope executor;
} MeisoLabActionDeliveryAccept;

typedef struct {
    MeisoActionId id;
    MeisoOwnerScope executor;
    MeisoActionState state;
    uint64_t new_window_id;
} MeisoLabActionResult;

typedef struct {
    MeisoContentId content_id;
    MeisoByteView content;
} MeisoLabBlobManifest;

typedef struct {
    MeisoContentId content_id;
} MeisoLabBlobAccept;

typedef struct {
    MeisoLabCloseStatus status;
} MeisoLabClose;

typedef struct {
    MeisoLabMessageType type;
    uint32_t registry_generation;
    union {
        MeisoLabRegistry registry;
        MeisoLabRegistry registry_accept;
        MeisoLabStateActual state_actual;
        MeisoLabStateAccept state_accept;
        MeisoLabStreamMetrics stream_metrics;
        MeisoLabActionReset action_reset;
        MeisoLabActionDeliveryAccept action_delivery_accept;
        MeisoLabActionResult action_result;
        MeisoLabBlobManifest blob_manifest;
        MeisoLabBlobAccept blob_accept;
        MeisoLabClose close;
    } body;
} MeisoLabMessage;

typedef uint8_t MeisoLabCodecStatus;
enum {
    MEISO_LAB_CODEC_OK = 0,
    MEISO_LAB_CODEC_INVALID_ARGUMENT = 1,
    MEISO_LAB_CODEC_INVALID_VALUE = 2,
    MEISO_LAB_CODEC_CAPACITY = 3,
    MEISO_LAB_CODEC_TRUNCATED = 4,
    MEISO_LAB_CODEC_MALFORMED = 5,
    MEISO_LAB_CODEC_UNSUPPORTED = 6
};

typedef struct {
    uint8_t step;
    uint8_t terminal;
    MeisoOwnerScope state_owner;
    uint64_t state_version;
    uint64_t window_id;
    MeisoActionId action_id;
    MeisoOwnerScope action_executor;
} MeisoLabOrder;

typedef uint8_t MeisoLabOrderStatus;
enum {
    MEISO_LAB_ORDER_ADVANCED = 0,
    MEISO_LAB_ORDER_COMPLETE = 1,
    MEISO_LAB_ORDER_UNEXPECTED = 2,
    MEISO_LAB_ORDER_ALREADY_TERMINAL = 3,
    MEISO_LAB_ORDER_INVALID_ARGUMENT = 4
};

const MeisoResourceDescriptor *meiso_lab_descriptors(size_t *out_count);
const MeisoResourceDescriptor *meiso_lab_descriptor(MeisoResourceKind kind);
const uint8_t *meiso_lab_capability_manifest(size_t *out_size);
const MeisoContentId *meiso_lab_capability_manifest_content_id(void);

MeisoLabCodecStatus meiso_lab_edge_state_encode(
    const MeisoLabEdgeState *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoLabCodecStatus meiso_lab_edge_state_decode(
    MeisoByteView input,
    MeisoLabEdgeState *out_value
);
MeisoLabCodecStatus meiso_lab_telemetry_sample_encode(
    const MeisoLabTelemetrySample *value,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoLabCodecStatus meiso_lab_telemetry_sample_decode(
    MeisoByteView input,
    MeisoLabTelemetrySample *out_value
);

MeisoLabCodecStatus meiso_lab_message_encode(
    const MeisoLabMessage *message,
    MeisoMutableBuffer output,
    size_t *out_size
);
MeisoLabCodecStatus meiso_lab_message_decode(
    MeisoByteView input,
    MeisoLabMessage *out_message
);

void meiso_lab_order_init(MeisoLabOrder *order);
MeisoLabOrderStatus meiso_lab_order_advance(
    MeisoLabOrder *order,
    MeisoLabSender sender,
    const MeisoLabMessage *message
);

#ifdef __cplusplus
}
#endif

#endif
