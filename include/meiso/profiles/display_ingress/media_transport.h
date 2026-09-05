#ifndef MEISO_PROFILES_DISPLAY_INGRESS_MEDIA_TRANSPORT_H
#define MEISO_PROFILES_DISPLAY_INGRESS_MEDIA_TRANSPORT_H

#include <meiso/profiles/display_ingress/profile.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Profile-local Host-to-Edge full-frame transport for
 * DISPLAY_/INGRESS_:v1. This object owns no socket, thread, heap allocation,
 * Runtime or display API. Both peers initialize it from the same currently
 * active MRT0 v4 external-media binding; there is no media OPEN/BIND exchange.
 * Every call on one object must be serialized by one owner.
 */

#define MEISO_DISPLAY_INGRESS_MEDIA_PACKET_SCHEMA_VERSION 1U
#define MEISO_DISPLAY_INGRESS_MEDIA_PACKET_HEADER_WIRE_SIZE 224U
#define MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_METADATA_BYTES 2048U

typedef struct MeisoDisplayIngressMediaTransport
    MeisoDisplayIngressMediaTransport;

typedef uint16_t MeisoDisplayIngressMediaPacketFlags;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_PACKET_BEGIN = UINT16_C(1) << 0U,
    MEISO_DISPLAY_INGRESS_MEDIA_PACKET_END = UINT16_C(1) << 1U,
    MEISO_DISPLAY_INGRESS_MEDIA_PACKET_RECOVERY = UINT16_C(1) << 2U
};

typedef uint8_t MeisoDisplayIngressMediaTransportStatus;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK = 0,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_ARGUMENT = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_CONFIG = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CAPACITY = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_INVALID_STATE = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_PACKET = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NO_FRAME = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_LATEST_REPLACED = 7,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED = 8,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_FRAME_PUBLISHED = 9,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_REASSEMBLY_REPLACED = 10,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DROPPED_EXPIRED = 11,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNSUPPORTED = 12,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DEADLINE_UNCOMPARABLE = 13,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TRUNCATED = 14,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_MALFORMED = 15,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_BINDING_MISMATCH = 16,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_EPOCH_MISMATCH = 17,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_NONCE_MISMATCH = 18,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OWNER_MISMATCH = 19,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_SEQUENCE_REPLAY = 20,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_DISCONTINUITY = 21,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_CONFLICT = 22,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_COUNTER_EXHAUSTED = 23,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_TERMINAL = 24
};

typedef uint8_t MeisoDisplayIngressMediaTransportPhase;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL = 2
};

typedef uint8_t MeisoDisplayIngressMediaTerminalReason;
enum {
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE = 1,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST = 2,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CONTROL_SESSION = 3,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CATALOG_CHANGED = 4,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_ENDPOINT_CHANGED = 5,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PRODUCTION_CHANGED = 6,
    MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING = 7
};

typedef struct {
    MeisoSessionRole local_role;
    MeisoDisplayIngressMediaBinding binding;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t max_fragment_payload_bytes;
    MeisoStreamDeadlinePolicy deadline_policy;
} MeisoDisplayIngressMediaTransportConfig;

/* One complete, independent, row-major R,G,B,A byte image. */
typedef struct {
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoDeadline sender_deadline;
    /* Upstream producer drops; transport latest replacements are added. */
    uint64_t producer_dropped_frames;
    MeisoByteView rgba_frame;
} MeisoDisplayIngressMediaFrame;

typedef struct {
    MeisoDisplayIngressMediaBinding binding;
    uint64_t serial;
    uint64_t frame_sequence;
    uint32_t fragment_offset;
    uint32_t packet_bytes;
} MeisoDisplayIngressMediaPacketRef;

typedef struct {
    MeisoDisplayIngressMediaPacketRef ref;
    MeisoByteView bytes;
} MeisoDisplayIngressMediaPacketView;

typedef struct {
    MeisoDisplayIngressMediaBinding binding;
    uint64_t frame_sequence;
} MeisoDisplayIngressMediaFrameRef;

typedef struct {
    MeisoDisplayIngressMediaFrameRef ref;
    MeisoDisplayIngressMediaFrame frame;
} MeisoDisplayIngressMediaLatestView;

typedef struct {
    MeisoSessionRole local_role;
    MeisoDisplayIngressMediaBinding binding;
    MeisoDisplayIngressMediaTransportPhase phase;
    MeisoDisplayIngressMediaTerminalReason terminal_reason;
    MeisoStreamDeadlinePolicy deadline_policy;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t frame_bytes;
    uint32_t max_fragment_payload_bytes;
    size_t arena_bytes;
    bool outbound_packet_pending;
    bool outbound_preemption_pending;
    uint64_t outbound_packet_serial_high_water;
    bool send_in_flight;
    bool send_latest;
    uint64_t send_frame_sequence;
    uint32_t send_fragment_offset;
    uint64_t submitted_sequence_high_water;
    uint64_t submitted_producer_drop_high_water;
    uint64_t transport_whole_frames_dropped;
    bool receive_reassembly;
    bool receive_latest;
    uint64_t receive_frame_sequence;
    uint32_t receive_next_offset;
    uint64_t observed_sequence_high_water;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
    uint64_t expired_frames_dropped;
} MeisoDisplayIngressMediaTransportSnapshot;

/*
 * Config must name exact DISPLAY_/INGR_END + DISPLAY_/DATA_MED,
 * Stream/DATA, Host owner, equal nonzero production/media epochs, a nonzero
 * nonce and Host producer incarnation. The frame envelope is exactly
 * width*height*4 and may not exceed the Runtime Resource maximum unit size.
 *
 * For an aligned arena, the exact requirement is:
 *   2048 + 2 * frame_bytes + 224 + max_fragment_payload_bytes.
 * Failure leaves disjoint outputs byte-exact unchanged.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_memory_required(
    const MeisoDisplayIngressMediaTransportConfig *config,
    size_t *out_size,
    size_t *out_alignment
);

/*
 * config, the complete supplied arena and out_transport must be mutually
 * disjoint. Init copies config and allocates no memory after construction.
 * Failure writes no arena byte and leaves out_transport unchanged.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_init(
    const MeisoDisplayIngressMediaTransportConfig *config,
    MeisoMutableBuffer arena,
    MeisoDisplayIngressMediaTransport **out_transport
);

/*
 * Host-only. submit copies the complete caller frame before returning. A
 * third queued value atomically replaces the unsent latest value. submit
 * never changes a packet already borrowed by next_outbound. A present
 * deadline uses the configured policy and the supplied Core-style admission
 * assertion; expiration is local_now > local_deadline, so equality remains
 * live. Deadline failure and all other failure statuses leave transport and
 * caller bytes unchanged.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_submit(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrame *frame,
    const MeisoStreamAdmission *admission
);

/*
 * next_outbound returns one canonical DIFR packet borrowed from the arena.
 * Repeated calls are idempotent until exact successful completion. Carrier
 * WOULD_BLOCK is represented by omitting complete_outbound. At a fragment
 * boundary with no borrowed packet, an available latest frame discards the
 * unsent primary remainder, increments the whole-frame drop high-water and
 * starts latest with RECOVERY|BEGIN. Only successful completion advances a
 * borrowed packet. Failure does not write output.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_next_outbound(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaPacketView *out_packet
);
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_complete_outbound(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaPacketRef *expected_packet
);

/*
 * Edge-only. A complete, canonical packet's routing/production scope is
 * admitted against the init binding before any pixel publication. Resource
 * descriptor truth is pinned by catalog identity and validated at init.
 * Scope/epoch/nonce/owner mismatch fail-closes this object with a
 * protocol-binding terminal reason. Fragment gaps/overlaps drop only the
 * current partial frame. A present deadline has the same three-way
 * unsupported/uncomparable/expired result as submit.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_receive_packet(
    MeisoDisplayIngressMediaTransport *transport,
    MeisoByteView packet,
    const MeisoStreamAdmission *admission
);

/*
 * Edge-only latest query. rgba_frame borrows the arena until exact release,
 * the next successful publication, or termination. release_latest is local
 * consumption, not a network ACK and not source-release authority.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_latest(
    const MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaLatestView *out_latest
);
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_release_latest(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaFrameRef *expected_frame
);

/*
 * Exact first termination clears both payload slots and packet scratch and is
 * sticky. A new control scope requires a new arena and init.
 */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_terminate(
    MeisoDisplayIngressMediaTransport *transport,
    const MeisoDisplayIngressMediaBinding *expected_binding,
    MeisoDisplayIngressMediaTerminalReason reason
);

/* Pure query. Failure leaves out_snapshot byte-exact unchanged. */
MeisoDisplayIngressMediaTransportStatus
meiso_display_ingress_media_transport_snapshot(
    const MeisoDisplayIngressMediaTransport *transport,
    MeisoDisplayIngressMediaTransportSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
