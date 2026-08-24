#ifndef MEISO_COMPOSITION_DEBUG_PEER_H
#define MEISO_COMPOSITION_DEBUG_PEER_H

#include "composition_session_v3.h"
#include "meiso_posix_tcp.h"
#include "meiso_runtime_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT 20U
#define MEISO_COMPOSITION_DEBUG_PEER_NO_ROUTE UINT8_C(255)

typedef struct MeisoCompositionDebugPeer MeisoCompositionDebugPeer;

/*
 * A Scheduler source has one immutable work kind and, for RouteKey-carrying
 * kinds, one immutable Profile-v3 route index. Exactly one source is the
 * peer-owned catalog source: CATALOG_OFFER on Edge, CATALOG_ACCEPT on Host,
 * with NO_ROUTE. The other catalog kinds are not accepted in this slice.
 */
typedef struct {
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t route_index;
} MeisoCompositionDebugPeerSourceConfig;

typedef struct {
    /* Already-connected, nonblocking SOCK_STREAM; peer never closes it. */
    int connected_fd;
    MeisoSessionRole local_role;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_peer_id;
    MeisoProtocolVersion protocol;
    uint64_t catalog_generation;
    MeisoCompositionSessionV3CatalogConfig catalog;
    size_t max_io_bytes_per_step;

    /*
     * Portable bootstrap cannot ask Runtime/Scheduler for exact sizes until
     * the owned Session is READY. These caller-selected reservations live in
     * the one peer arena; READY activation validates exact required sizes and
     * fails terminally rather than allocating or borrowing more storage.
     */
    size_t adapter_reserve_bytes;
    size_t runtime_reserve_bytes;
    size_t scheduler_reserve_bytes;
    MeisoCompositionDebugPeerSourceConfig sources[
        MEISO_COMPOSITION_DEBUG_PEER_TX_SOURCE_COUNT
    ];
} MeisoCompositionDebugPeerConfig;

typedef uint8_t MeisoCompositionDebugPeerInitStatus;
enum {
    MEISO_COMPOSITION_DEBUG_PEER_INIT_OK = 0,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_FD_ERROR = 3,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_MISALIGNED_ARENA = 4,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_ARENA_TOO_SMALL = 5,
    MEISO_COMPOSITION_DEBUG_PEER_INIT_INTERNAL_FAILURE = 6
};

typedef uint8_t MeisoCompositionDebugPeerStatus;
enum {
    MEISO_COMPOSITION_DEBUG_PEER_OK = 0,
    MEISO_COMPOSITION_DEBUG_PEER_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_DEBUG_PEER_INVALID_STATE = 2,
    MEISO_COMPOSITION_DEBUG_PEER_SOURCE_BUSY = 3,
    MEISO_COMPOSITION_DEBUG_PEER_SOURCE_KIND_MISMATCH = 4,
    MEISO_COMPOSITION_DEBUG_PEER_ROUTE_MISMATCH = 5,
    MEISO_COMPOSITION_DEBUG_PEER_RX_OBLIGATION = 6,
    MEISO_COMPOSITION_DEBUG_PEER_TERMINAL = 7,
    MEISO_COMPOSITION_DEBUG_PEER_COUNTER_EXHAUSTED = 8
};

typedef uint8_t MeisoCompositionDebugPeerPumpStatus;
enum {
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_PROGRESS = 0,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_IDLE = 1,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_EVENT = 2,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_RX_OBLIGATION = 3,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_WOULD_BLOCK = 4,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_TX_BUSY = 5,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_REBIND_PROGRESS = 6,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_REJECTED = 7,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_SOURCE_INVALID = 8,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_TERMINAL = 9,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_COUNTER_EXHAUSTED = 10,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_INVALID_ARGUMENT = 11,
    MEISO_COMPOSITION_DEBUG_PEER_PUMP_INTERNAL_FAILURE = 12
};

typedef struct {
    MeisoRuntimeEvent event;
    MeisoRuntimeSchedulerStepResult scheduler_step;
    MeisoPosixTcpReadStatus read_status;
    MeisoPosixTcpWriteStatus write_status;
    MeisoRuntimeSchedulerRxObligation rx_obligation;
    bool event_present;
    bool scheduler_step_present;
    bool read_attempted;
    bool write_attempted;
    uint8_t completed_source_index;
    bool completed_source_present;
} MeisoCompositionDebugPeerPumpResult;

typedef uint8_t MeisoCompositionDebugPeerPhase;
enum {
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_HANDSHAKE = 1,
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_CATALOG = 2,
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_REBIND = 3,
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_READY = 4,
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_RX_OBLIGATION = 5,
    MEISO_COMPOSITION_DEBUG_PEER_PHASE_TERMINAL = 6
};

typedef struct {
    MeisoCompositionDebugPeerPhase phase;
    MeisoSessionSnapshot session;
    MeisoRuntimeSnapshot runtime;
    MeisoRuntimeSchedulerSnapshot scheduler;
    uint64_t catalog_generation;
    uint64_t next_ticket;
    uint64_t boundary_serial;
    uint64_t last_boundary_generation;
    MeisoRuntimeSchedulerBoundary last_boundary;
    size_t arena_bytes;
    size_t session_bytes;
    size_t adapter_bytes;
    size_t adapter_reserve_bytes;
    size_t runtime_reserve_bytes;
    size_t runtime_bytes;
    size_t scheduler_reserve_bytes;
    size_t scheduler_bytes;
    uint8_t pending_source_count;
    bool runtime_present;
    bool scheduler_present;
    bool terminal;
} MeisoCompositionDebugPeerSnapshot;

size_t meiso_composition_debug_peer_alignment_required(void);

/*
 * Returns the exact one-arena size for the fixed bootstrap storage plus the
 * three caller reservations. It does not inspect or mutate the fd. Zero means
 * invalid configuration or size overflow. init starts the owned Session and
 * initializes the POSIX adapter, but Runtime/Scheduler activation is deferred
 * until that Session becomes READY. All config/output/arena ranges must be
 * mutually disjoint. No function allocates, creates a socket, changes fd
 * flags, or closes the fd.
 *
 * Edge offers exactly config.catalog_generation and the locally built v3
 * 6-endpoint/18-route MRT0-v4 catalog. Host accepts only after the staged
 * generation, endpoint identity/incarnation/schema/capability bytes, route
 * identity/facet/schema/owner/max-payload and delivery classes all equal its
 * independently built catalog. Any other offer is terminal in this minimal
 * debug peer (there is no policy negotiation/reject fallback). A staged offer
 * does not clear queued work; only active-catalog change triggers the existing
 * Scheduler boundary. Every route source is rebound to its configured exact
 * route index before READY is exposed.
 */
size_t meiso_composition_debug_peer_memory_required(
    const MeisoCompositionDebugPeerConfig *config
);
MeisoCompositionDebugPeerInitStatus meiso_composition_debug_peer_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionDebugPeerConfig *config,
    MeisoCompositionDebugPeer **out_peer
);

/*
 * Queues exactly one value-copy work description in the selected source.
 * source_index is 0..19 and cannot name the peer-owned catalog source. The
 * kind and exact active RouteKey must match its immutable source config;
 * input ticket must be zero and the peer assigns the nonzero ticket.
 * Byte views inside work must be disjoint from the complete peer arena and
 * remain caller-owned until the matching source completion, a boundary_serial
 * change, or terminal. Pump identifies a completion by public source_index;
 * Scheduler SourceId is diagnostic only. Failure is atomic
 * and leaves the work, peer, Runtime, Session, Scheduler and outputs unchanged.
 */
MeisoCompositionDebugPeerStatus meiso_composition_debug_peer_queue_work(
    MeisoCompositionDebugPeer *peer,
    uint8_t source_index,
    const MeisoRuntimeSchedulerWork *work
);

/*
 * Completion is retained per source and must be taken before that source can
 * accept another work item. The internal catalog source has no public
 * completion. NO completion leaves out_completion byte-exact unchanged.
 */
MeisoCompositionDebugPeerStatus meiso_composition_debug_peer_take_completion(
    MeisoCompositionDebugPeer *peer,
    uint8_t source_index,
    MeisoRuntimeSchedulerCompletion *out_completion
);

/*
 * One call is bounded. It performs at most one of: recv when readable is true,
 * send when writable is true, internal activation, catalog/rebind transition,
 * or Scheduler step. The caller supplies readiness
 * from its event loop; no poll/select or socket creation is hidden here.
 * A persistent three-way cursor gives readable I/O, writable I/O, and the
 * internal activation/Scheduler path each an attempt within three eligible
 * pump calls; a continuously writable/readable socket cannot starve work in
 * the other classes. WOULD_BLOCK/INTERRUPTED and every invalid call leave
 * out_result unchanged (the fairness cursor may advance after an attempted
 * syscall).
 *
 * A returned Runtime event borrows the owned Session/Runtime until the next
 * mutable peer call. State offers and publishable Blob COMMITs establish an
 * explicit RX obligation: further pump/queue calls fail closed until the
 * matching commit/publish wrapper below resolves it. This is the only path
 * by which the peer drives Runtime receive; manual wire/Session receive is
 * outside the contract.
 */
MeisoCompositionDebugPeerPumpStatus meiso_composition_debug_peer_pump(
    MeisoCompositionDebugPeer *peer,
    bool readable,
    bool writable,
    MeisoCompositionDebugPeerPumpResult *out_result
);

MeisoCompositionDebugPeerStatus
meiso_composition_debug_peer_commit_state_receive(
    MeisoCompositionDebugPeer *peer,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
);
MeisoCompositionDebugPeerStatus
meiso_composition_debug_peer_publish_blob_receive(
    MeisoCompositionDebugPeer *peer,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
);

/*
 * store's complete opaque arena and the verifier context are caller-proven
 * disjoint from the complete peer arena. The implementation additionally
 * rejects detectable handle-start overlap. The verifier is synchronous and
 * may not re-enter any peer/Session/Runtime/Scheduler operation; peer APIs
 * fail closed while it runs.
 */

bool meiso_composition_debug_peer_wants_read(
    const MeisoCompositionDebugPeer *peer
);
bool meiso_composition_debug_peer_wants_write(
    const MeisoCompositionDebugPeer *peer
);
/* Returns the exact active Profile-v3 RouteKey for index 0..17. */
bool meiso_composition_debug_peer_route_key(
    const MeisoCompositionDebugPeer *peer,
    uint8_t route_index,
    MeisoRuntimeRouteKey *out_route
);
bool meiso_composition_debug_peer_snapshot(
    const MeisoCompositionDebugPeer *peer,
    MeisoCompositionDebugPeerSnapshot *out_snapshot
);

/* Records transport loss, clears all pending sources and makes terminal sticky. */
void meiso_composition_debug_peer_abort(
    MeisoCompositionDebugPeer *peer,
    int error_number
);

#ifdef __cplusplus
}
#endif

#endif
