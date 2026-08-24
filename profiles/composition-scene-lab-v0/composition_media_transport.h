#ifndef MEISO_COMPOSITION_MEDIA_TRANSPORT_H
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_H

#include "composition_session_v3.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Profile-local, single-threaded external-media state machine. It owns no
 * socket, thread, heap allocation, Core object, Session or Runtime. The
 * caller must terminate this object before exposing Session terminal,
 * catalog replacement, endpoint re-incarnation or production-epoch change.
 * A terminal object is never reset; a new control scope uses a new arena.
 */

#define MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT 2U
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_RENDER_MASK UINT8_C(0x01)
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_CAMERA_MASK UINT8_C(0x02)
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_ALL_MASK UINT8_C(0x03)
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_METADATA_BYTES 4096U
#define MEISO_COMPOSITION_MEDIA_TRANSPORT_MAX_PACKET_BYTES \
    (MEISO_COMPOSITION_MEDIA_FRAME_HEADER_WIRE_SIZE + \
     MEISO_COMPOSITION_MEDIA_MAX_FRAGMENT_PAYLOAD_SIZE)

typedef struct MeisoCompositionMediaTransport MeisoCompositionMediaTransport;

typedef uint8_t MeisoCompositionMediaTransportStatus;
enum {
    MEISO_COMPOSITION_MEDIA_TRANSPORT_OK = 0,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_CAPACITY = 3,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_INVALID_STATE = 4,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_PACKET = 5,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME = 6,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED = 7,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAGMENT_ACCEPTED = 8,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_FRAME_PUBLISHED = 9,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_REASSEMBLY_REPLACED = 10,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_TRUNCATED = 11,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_MALFORMED = 12,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_BINDING_MISMATCH = 13,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_EPOCH_MISMATCH = 14,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_NONCE_MISMATCH = 15,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_SEQUENCE_REPLAY = 16,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_DISCONTINUITY = 17,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_CONFLICT = 18,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_COUNTER_EXHAUSTED = 19,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_TERMINAL = 20
};

typedef uint8_t MeisoCompositionMediaTransportPhase;
enum {
    MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_OPEN_PENDING = 1,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_WAIT_BIND = 2,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE = 3,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_WAIT_OPEN = 4,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_BINDING = 5,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE = 6,
    MEISO_COMPOSITION_MEDIA_TRANSPORT_PHASE_TERMINAL = 7
};

typedef uint8_t MeisoCompositionMediaPacketKind;
enum {
    MEISO_COMPOSITION_MEDIA_PACKET_OPEN = 1,
    MEISO_COMPOSITION_MEDIA_PACKET_BIND = 2,
    MEISO_COMPOSITION_MEDIA_PACKET_FRAME = 3
};

typedef uint8_t MeisoCompositionMediaTerminalReason;
enum {
    MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE = 1,
    MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST = 2,
    MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION = 3,
    MEISO_COMPOSITION_MEDIA_TERMINAL_CATALOG_CHANGED = 4,
    MEISO_COMPOSITION_MEDIA_TERMINAL_ENDPOINT_CHANGED = 5,
    MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED = 6
};

typedef struct {
    MeisoCompositionMediaBinding binding;
    MeisoCompositionMediaCapability capability;
    MeisoCompositionMediaDesired effective;
} MeisoCompositionMediaRouteConfig;

typedef struct {
    MeisoSessionRole local_role;
    MeisoCompositionMediaOpen scope;
    /*
     * Explicit subset of ROUTE_ALL_MASK. New callers must pass render, camera
     * or both. Zero is a deprecated source-compatibility alias for both and is
     * normalized to ROUTE_ALL_MASK in the transport/snapshot. The active set
     * is immutable for this lifetime; changing it requires terminate + init in
     * a fresh arena. OPEN does not carry this mask: both peers must derive the
     * same mask from the same current accepted control-plane actual truth.
     */
    uint8_t active_route_mask;
    /* Canonical order is render preview, then camera. */
    MeisoCompositionMediaRouteConfig
        routes[MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT];
} MeisoCompositionMediaTransportConfig;

/*
 * Full access unit submitted by the Edge producer or returned by the Host
 * latest-frame query. BEGIN/END are transport-owned and forbidden on submit;
 * KEY is required by the Profile v3 RAW codec and RECOVERY is optional.
 */
typedef struct {
    MeisoCompositionMediaBinding binding;
    uint64_t frame_sequence;
    MeisoTimePoint source_time;
    MeisoOwnerScope applied_scene_owner;
    uint64_t applied_scene_state_version;
    uint64_t active_plan_generation;
    uint64_t world_generation;
    uint64_t dependency_sequence;
    uint64_t producer_dropped_frames;
    MeisoCompositionMediaPixelFormat pixel_format;
    MeisoCompositionMediaCodec codec;
    uint16_t width;
    uint16_t height;
    MeisoCompositionMediaFrameFlags flags;
    MeisoByteView access_unit;
} MeisoCompositionMediaUnit;

typedef struct {
    MeisoSessionId session_id;
    uint64_t catalog_generation;
    uint64_t serial;
    MeisoCompositionMediaPacketKind kind;
    MeisoCompositionMediaSource source;
    uint64_t frame_sequence;
    uint32_t fragment_offset;
    uint32_t packet_bytes;
} MeisoCompositionMediaPacketRef;

typedef struct {
    MeisoCompositionMediaPacketRef ref;
    MeisoByteView bytes;
} MeisoCompositionMediaPacketView;

typedef struct {
    MeisoCompositionMediaBinding binding;
    uint64_t frame_sequence;
} MeisoCompositionMediaFrameRef;

typedef struct {
    MeisoCompositionMediaFrameRef ref;
    MeisoCompositionMediaUnit unit;
} MeisoCompositionMediaLatestView;

typedef struct {
    MeisoCompositionMediaSource source;
    bool send_in_flight;
    bool send_latest;
    uint64_t send_frame_sequence;
    uint32_t send_fragment_offset;
    uint64_t submitted_sequence_high_water;
    uint64_t transport_whole_frames_dropped;
    bool receive_reassembly;
    bool receive_latest;
    uint64_t receive_frame_sequence;
    uint32_t receive_next_offset;
    uint64_t published_sequence_high_water;
    uint64_t sequence_gap_frames;
    uint64_t producer_drop_high_water;
    uint64_t reassembly_frames_dropped;
    uint64_t unread_latest_frames_dropped;
} MeisoCompositionMediaRouteSnapshot;

typedef struct {
    MeisoSessionRole local_role;
    MeisoCompositionMediaOpen scope;
    MeisoCompositionMediaTransportPhase phase;
    MeisoCompositionMediaTerminalReason terminal_reason;
    uint8_t configured_active_route_mask;
    uint8_t bound_route_mask;
    uint8_t fragment_rr_cursor;
    bool outbound_packet_pending;
    MeisoCompositionMediaPacketKind outbound_packet_kind;
    uint64_t outbound_packet_serial_high_water;
    size_t arena_bytes;
    MeisoCompositionMediaRouteSnapshot
        routes[MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_COUNT];
} MeisoCompositionMediaTransportSnapshot;

/*
 * active_route_mask must be render, camera or both. Deprecated zero is
 * normalized to both for existing source callers; unknown bits are
 * INVALID_CONFIG. Each active route must be the exact Profile v3 external-data
 * route with an exact accepted effective format. Its binding SessionId and
 * catalog generation equal scope; production/media epochs and nonce are
 * nonzero. An inactive route's binding and effective fields are ignored and
 * may be all zero. Its capability still has the canonical source and valid
 * nonzero bounded max-access-unit/max-fragment sizes because the fixed arena
 * reserves both route slot pairs. Other inactive capability fields are
 * ignored. No API changes active_route_mask in place.
 *
 * For an aligned arena the exact required bytes are:
 *   4096 metadata
 *   + 2 * (render max access unit + camera max access unit)
 *   + 240 + max(configured route capability fragment payload).
 * The two payload slots per route are in-flight+latest on Edge and
 * reassembly+latest on Host. No allocation occurs after init.
 * Failure leaves disjoint outputs unchanged.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_memory_required(
    const MeisoCompositionMediaTransportConfig *config,
    size_t *out_size,
    size_t *out_alignment
);

/*
 * config, arena and out_transport storage are mutually disjoint. arena.data
 * must meet the returned alignment and arena.size must be at least the exact
 * required size. Blob/packet inputs and public output structs must never
 * overlap the arena. Failure writes no arena byte and leaves out_transport
 * unchanged.
 */
MeisoCompositionMediaTransportStatus meiso_composition_media_transport_init(
    const MeisoCompositionMediaTransportConfig *config,
    MeisoMutableBuffer arena,
    MeisoCompositionMediaTransport **out_transport
);

/*
 * Host emits one unchanged Profile v3 OPEN. Edge accepts exact OPEN and emits
 * one exact BIND for each selected route in canonical order, skipping inactive
 * routes. Host waits only for the configured selected mask; both peers become
 * ACTIVE when exactly that mask is bound. An inactive-route BIND is rejected
 * with BINDING_MISMATCH and no state change. Since OPEN does not negotiate the
 * mask, mismatched peer configs only fail closed and do not guarantee progress;
 * the control-plane owner must terminate/rebuild them. next_outbound is
 * idempotent while a packet is pending and returns an arena borrow stable until
 * exact successful completion or termination. A socket WOULD_BLOCK is
 * represented by doing nothing: no complete call, no cursor/offset/sequence/
 * counter advance. complete_outbound is the sole send-progress operation.
 * NO_PACKET and every other non-success status leave out_packet byte-exact
 * unchanged.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_next_outbound(
    MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaPacketView *out_packet
);
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_complete_outbound(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaPacketRef *expected_packet
);

/*
 * Edge-only full-unit submission copies bytes into its bounded slots. The
 * source must be selected by active_route_mask; an inactive source returns
 * INVALID_STATE with no mutation. Exact binding/epoch/nonce and strictly
 * increasing per-route frame sequence are required. If both slots are
 * occupied, the old latest frame is atomically
 * replaced, LATEST_REPLACED is returned and the whole-frame drop counter is
 * advanced without changing the in-flight frame. Two ready routes are chosen
 * by persistent round-robin at each successfully completed fragment.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_submit(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaUnit *unit
);

/*
 * Receives one complete MOPN/MBND/MFRM codec packet. An MFRM for an inactive
 * source returns INVALID_STATE; it publishes no bytes and preserves all route
 * state. Truncated/malformed or wrong binding/epoch/nonce packets publish no
 * bytes and preserve an existing valid reassembly. Same-frame gap/overlap
 * drops that whole reassembly and
 * returns DISCONTINUITY. A newer valid BEGIN may replace an incomplete older
 * frame and returns REASSEMBLY_REPLACED while it remains partial; if that
 * replacement is already a complete BEGIN|END unit, FRAME_PUBLISHED takes
 * priority. A replacement BEGIN must not decrease the current production-
 * epoch cumulative producer drop value. Only exact contiguous END atomically
 * replaces the Host latest. Sequence gaps, producer cumulative drops,
 * incomplete-frame drops and unread-latest replacement are maintained as
 * separate bounded counters.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_receive_packet(
    MeisoCompositionMediaTransport *transport,
    MeisoByteView packet
);

/*
 * Host-only latest query. An inactive source (or inactive expected binding for
 * release) returns INVALID_STATE with no mutation/output change. On OK,
 * unit.access_unit borrows the arena. The borrow ends on exact release, a
 * successful receive that publishes the same route, or termination.
 * release_latest is exact binding+sequence and is the only explicit consumer
 * acknowledgement. NO_FRAME and every other non-success status leave
 * out_latest byte-exact unchanged.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_latest(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
);
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_release_latest(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaFrameRef *expected_frame
);

/*
 * scope must exactly match the initialized SessionId/catalog. First success
 * atomically clears all payload slots and outbound scratch, invalidates every
 * borrow and enters sticky terminal. Later non-query calls return TERMINAL;
 * snapshot remains available to report the terminal reason. A new scope must
 * use a newly initialized arena; no bind, sequence or frame resurrects.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_terminate(
    MeisoCompositionMediaTransport *transport,
    const MeisoCompositionMediaOpen *expected_scope,
    MeisoCompositionMediaTerminalReason reason
);

/*
 * Query is pure; failure leaves out_snapshot byte-exact unchanged. Across the
 * API, any input/expected/output range overlapping the transport arena is
 * INVALID_ARGUMENT with byte-exact zero state/output change. Every bounded
 * counter increment is checked before any payload, slot, cursor, sequence,
 * counter or output mutation; COUNTER_EXHAUSTED is therefore fully atomic.
 */
MeisoCompositionMediaTransportStatus
meiso_composition_media_transport_snapshot(
    const MeisoCompositionMediaTransport *transport,
    MeisoCompositionMediaTransportSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
