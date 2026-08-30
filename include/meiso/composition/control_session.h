#ifndef MEISO_COMPOSITION_CONTROL_SESSION_H
#define MEISO_COMPOSITION_CONTROL_SESSION_H

#include <meiso/profiles/composition/profile.h>
#include <meiso/adapters/posix/tcp.h>
#include <meiso/runtime/scheduler.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT 20U
#define MEISO_COMPOSITION_CONTROL_SESSION_NO_ROUTE UINT8_C(255)

typedef struct MeisoCompositionControlSession MeisoCompositionControlSession;

/*
 * A Scheduler source has one immutable work kind and, for RouteKey-carrying
 * kinds, one immutable Composition Profile route index. Exactly one source is
 * the control-session-owned catalog source: CATALOG_OFFER on Edge,
 * CATALOG_ACCEPT on Host, with NO_ROUTE. The other catalog kinds are not
 * accepted in this slice.
 */
typedef struct {
    MeisoRuntimeSchedulerWorkKind work_kind;
    uint8_t route_index;
} MeisoCompositionControlSessionSourceConfig;

typedef struct {
    /* Already-connected, nonblocking SOCK_STREAM; session never closes it. */
    int connected_fd;
    MeisoSessionRole local_role;
    MeisoPeerId local_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce local_nonce;
    MeisoPeerId expected_peer_id;
    MeisoProtocolVersion protocol;
    uint64_t catalog_generation;
    MeisoCompositionProfileCatalogConfig catalog;
    size_t max_io_bytes_per_step;

    /*
     * Portable bootstrap cannot ask Runtime/Scheduler for exact sizes until
     * the owned Session is READY. These caller-selected reservations live in
     * the one control-session arena; READY activation validates exact required
     * sizes and fails terminally rather than allocating or borrowing storage.
     */
    size_t adapter_reserve_bytes;
    size_t runtime_reserve_bytes;
    size_t scheduler_reserve_bytes;
    MeisoCompositionControlSessionSourceConfig sources[
        MEISO_COMPOSITION_CONTROL_SESSION_TX_SOURCE_COUNT
    ];
} MeisoCompositionControlSessionConfig;

typedef uint8_t MeisoCompositionControlSessionInitStatus;
enum {
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_OK = 0,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_INVALID_CONFIG = 2,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_FD_ERROR = 3,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_MISALIGNED_ARENA = 4,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_ARENA_TOO_SMALL = 5,
    MEISO_COMPOSITION_CONTROL_SESSION_INIT_INTERNAL_FAILURE = 6
};

typedef uint8_t MeisoCompositionControlSessionStatus;
enum {
    MEISO_COMPOSITION_CONTROL_SESSION_OK = 0,
    MEISO_COMPOSITION_CONTROL_SESSION_INVALID_ARGUMENT = 1,
    MEISO_COMPOSITION_CONTROL_SESSION_INVALID_STATE = 2,
    MEISO_COMPOSITION_CONTROL_SESSION_SOURCE_BUSY = 3,
    MEISO_COMPOSITION_CONTROL_SESSION_SOURCE_KIND_MISMATCH = 4,
    MEISO_COMPOSITION_CONTROL_SESSION_ROUTE_MISMATCH = 5,
    MEISO_COMPOSITION_CONTROL_SESSION_RX_OBLIGATION = 6,
    MEISO_COMPOSITION_CONTROL_SESSION_TERMINAL = 7,
    MEISO_COMPOSITION_CONTROL_SESSION_COUNTER_EXHAUSTED = 8
};

typedef uint8_t MeisoCompositionControlSessionPumpStatus;
enum {
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_PROGRESS = 0,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_IDLE = 1,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_EVENT = 2,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_RX_OBLIGATION = 3,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_WOULD_BLOCK = 4,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TX_BUSY = 5,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_REBIND_PROGRESS = 6,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_REJECTED = 7,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_SOURCE_INVALID = 8,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_TERMINAL = 9,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_COUNTER_EXHAUSTED = 10,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INVALID_ARGUMENT = 11,
    MEISO_COMPOSITION_CONTROL_SESSION_PUMP_INTERNAL_FAILURE = 12
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
} MeisoCompositionControlSessionPumpResult;

typedef uint8_t MeisoCompositionControlSessionPhase;
enum {
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_HANDSHAKE = 1,
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_CATALOG = 2,
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_REBIND = 3,
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY = 4,
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_RX_OBLIGATION = 5,
    MEISO_COMPOSITION_CONTROL_SESSION_PHASE_TERMINAL = 6
};

typedef struct {
    MeisoCompositionControlSessionPhase phase;
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
} MeisoCompositionControlSessionSnapshot;

size_t meiso_composition_control_session_alignment_required(void);

/*
 * Returns the exact one-arena size for the fixed bootstrap storage plus the
 * three caller reservations. It does not inspect or mutate the fd. Zero means
 * invalid configuration or size overflow. init starts the owned Session and
 * initializes the POSIX adapter, but Runtime/Scheduler activation is deferred
 * until that Session becomes READY. All config/output/arena ranges must be
 * mutually disjoint. No function allocates, creates a socket, changes fd
 * flags, or closes the fd.
 *
 * Edge offers exactly config.catalog_generation and the locally built
 * 6-endpoint/18-route MRT0-v4 catalog. Host accepts only after the staged
 * generation, endpoint identity/incarnation/schema/capability bytes, route
 * identity/facet/schema/owner/max-payload and delivery classes all equal its
 * independently built catalog. Any other offer is terminal in this minimal
 * control session (there is no policy negotiation/reject fallback). A staged
 * offer does not clear queued work; only active-catalog change triggers the
 * existing Scheduler boundary. Every route source is rebound to its configured
 * exact route index before READY is exposed.
 */
size_t meiso_composition_control_session_memory_required(
    const MeisoCompositionControlSessionConfig *config
);
MeisoCompositionControlSessionInitStatus meiso_composition_control_session_init(
    void *arena,
    size_t arena_size,
    const MeisoCompositionControlSessionConfig *config,
    MeisoCompositionControlSession **out_session
);

/*
 * Queues exactly one value-copy work description in the selected source.
 * source_index is 0..19 and cannot name the control-session-owned catalog
 * source. The kind and exact active RouteKey must match its immutable source
 * config; input ticket must be zero and the control session assigns the
 * nonzero ticket. Byte views inside work must be disjoint from the complete
 * control-session arena and remain caller-owned until the matching source
 * completion, a boundary_serial change, or terminal. Pump identifies a
 * completion by public source_index; Scheduler SourceId is diagnostic only.
 * Failure is atomic and leaves the work, ControlSession, Runtime, Session,
 * Scheduler and outputs unchanged.
 */
MeisoCompositionControlSessionStatus meiso_composition_control_session_queue_work(
    MeisoCompositionControlSession *control_session,
    uint8_t source_index,
    const MeisoRuntimeSchedulerWork *work
);

/*
 * Completion is retained per source and must be taken before that source can
 * accept another work item. The internal catalog source has no public
 * completion. NO completion leaves out_completion byte-exact unchanged.
 */
MeisoCompositionControlSessionStatus meiso_composition_control_session_take_completion(
    MeisoCompositionControlSession *control_session,
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
 * mutable ControlSession call. State offers and publishable Blob COMMITs
 * establish an explicit RX obligation: further pump/queue calls fail closed
 * until the matching commit/publish wrapper below resolves it. This is the
 * only path by which ControlSession drives Runtime receive; manual
 * wire/Session receive is outside the contract.
 */
MeisoCompositionControlSessionPumpStatus meiso_composition_control_session_pump(
    MeisoCompositionControlSession *control_session,
    bool readable,
    bool writable,
    MeisoCompositionControlSessionPumpResult *out_result
);

MeisoCompositionControlSessionStatus
meiso_composition_control_session_commit_state_receive(
    MeisoCompositionControlSession *control_session,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
);
MeisoCompositionControlSessionStatus
meiso_composition_control_session_publish_blob_receive(
    MeisoCompositionControlSession *control_session,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
);

/*
 * store's complete opaque arena and the verifier context are caller-proven
 * disjoint from the complete ControlSession arena. The implementation also
 * rejects detectable handle-start overlap. The verifier is synchronous and
 * may not re-enter any ControlSession/Session/Runtime/Scheduler operation;
 * ControlSession APIs fail closed while it runs.
 */

bool meiso_composition_control_session_wants_read(
    const MeisoCompositionControlSession *control_session
);
bool meiso_composition_control_session_wants_write(
    const MeisoCompositionControlSession *control_session
);
/* Returns the exact active Composition Profile RouteKey for index 0..17. */
bool meiso_composition_control_session_route_key(
    const MeisoCompositionControlSession *control_session,
    uint8_t route_index,
    MeisoRuntimeRouteKey *out_route
);
bool meiso_composition_control_session_snapshot(
    const MeisoCompositionControlSession *control_session,
    MeisoCompositionControlSessionSnapshot *out_snapshot
);

/* Records transport loss, clears all pending sources and makes terminal sticky. */
void meiso_composition_control_session_abort(
    MeisoCompositionControlSession *control_session,
    int error_number
);

#ifdef __cplusplus
}
#endif

#endif
