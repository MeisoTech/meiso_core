#ifndef MEISO_RUNTIME_SCHEDULER_H
#define MEISO_RUNTIME_SCHEDULER_H

#include "meiso_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEISO_RUNTIME_SCHEDULER_CONTROL_BURST 4U

typedef struct MeisoRuntimeScheduler MeisoRuntimeScheduler;
typedef uint16_t MeisoRuntimeSchedulerSourceId;

typedef uint8_t MeisoRuntimeSchedulerWorkKind;
enum {
    MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER = 1,
    MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_ACCEPT = 2,
    MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_REJECT = 3,
    MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER = 4,
    MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT = 5,
    MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE = 6,
    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN = 7,
    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK = 8,
    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT = 9,
    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT = 10,
    MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT = 11
};

typedef struct {
    uint64_t generation;
    const MeisoRuntimeEndpointDescriptor *endpoints;
    size_t endpoint_count;
    const MeisoRuntimeRouteDescriptor *routes;
    size_t route_count;
} MeisoRuntimeSchedulerCatalogOffer;

typedef struct {
    MeisoRuntimeCatalogRejectReason reason;
} MeisoRuntimeSchedulerCatalogReject;

typedef struct {
    MeisoRuntimeRouteKey route;
    MeisoStateOffer offer;
} MeisoRuntimeSchedulerStateOffer;

typedef struct {
    MeisoRuntimeRouteKey route;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;
} MeisoRuntimeSchedulerStateAccept;

typedef struct {
    MeisoRuntimeRouteKey route;
    uint64_t production_epoch;
    MeisoStreamSample sample;
} MeisoRuntimeSchedulerStreamSample;

typedef struct {
    MeisoRuntimeRouteKey route;
    MeisoContentId content_id;
    uint64_t total_size;
} MeisoRuntimeSchedulerBlobBegin;

typedef struct {
    MeisoRuntimeBlobTransferRef transfer;
    uint64_t offset;
    MeisoByteView payload;
} MeisoRuntimeSchedulerBlobChunk;

typedef struct {
    MeisoRuntimeBlobTransferRef transfer;
} MeisoRuntimeSchedulerBlobCommit;

typedef struct {
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobAbortReason reason;
} MeisoRuntimeSchedulerBlobAbort;

typedef struct {
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeBlobOperation operation;
} MeisoRuntimeSchedulerBlobResult;

/*
 * poll is a pure, idempotent observation: it never dequeues or otherwise
 * advances its source. It returns a value-copy description whose byte views
 * and catalog arrays remain valid only through the immediately following
 * synchronous Runtime dispatch. Scheduler never retains those pointers.
 * ticket is nonzero and the complete work stays exact until complete is
 * called. complete is the only operation allowed to advance/drop that head.
 */
typedef struct {
    uint64_t ticket;
    MeisoRuntimeSchedulerWorkKind kind;
    union {
        MeisoRuntimeSchedulerCatalogOffer catalog_offer;
        MeisoRuntimeSchedulerCatalogReject catalog_reject;
        MeisoRuntimeSchedulerStateOffer state_offer;
        MeisoRuntimeSchedulerStateAccept state_accept;
        MeisoRuntimeSchedulerStreamSample stream_sample;
        MeisoRuntimeSchedulerBlobBegin blob_begin;
        MeisoRuntimeSchedulerBlobChunk blob_chunk;
        MeisoRuntimeSchedulerBlobCommit blob_commit;
        MeisoRuntimeSchedulerBlobAbort blob_abort;
        MeisoRuntimeSchedulerBlobResult blob_result;
    } value;
} MeisoRuntimeSchedulerWork;

typedef uint8_t MeisoRuntimeSchedulerPollStatus;
enum {
    MEISO_RUNTIME_SCHEDULER_POLL_EMPTY = 0,
    MEISO_RUNTIME_SCHEDULER_POLL_READY = 1,
    MEISO_RUNTIME_SCHEDULER_POLL_INVALID = 2
};

typedef uint8_t MeisoRuntimeSchedulerCompletionCode;
enum {
    MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT = 1,
    MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED = 2
};

typedef struct {
    MeisoRuntimeSchedulerSourceId source_id;
    uint64_t ticket;
    MeisoRuntimeSchedulerWorkKind kind;
    MeisoRuntimeSchedulerCompletionCode code;
    MeisoRuntimeStatus runtime_status;
    bool generated_blob_transfer_present;
    MeisoRuntimeBlobTransferRef generated_blob_transfer;
} MeisoRuntimeSchedulerCompletion;

typedef uint8_t MeisoRuntimeSchedulerBoundary;
enum {
    MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED = 1,
    MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL = 2
};

/*
 * Callbacks are serialized and may not re-enter Scheduler/Runtime/Session.
 * Their contexts and every borrowed work byte must be caller-proven disjoint
 * from the complete Scheduler, Runtime, and Session arenas. POLL_INVALID is a
 * visited bad source (it cannot block RR) but has no trusted ticket to
 * complete. READY with invalid work is completed as REJECTED/INVALID_ARGUMENT.
 */
typedef MeisoRuntimeSchedulerPollStatus (*MeisoRuntimeSchedulerPoll)(
    void *context,
    MeisoRuntimeSchedulerWork *out_work
);
typedef void (*MeisoRuntimeSchedulerComplete)(
    void *context,
    const MeisoRuntimeSchedulerCompletion *completion
);
typedef void (*MeisoRuntimeSchedulerBoundaryNotify)(
    void *context,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t catalog_generation
);

typedef struct {
    /*
     * Fixed for the source lifetime. STREAM_SAMPLE and BLOB_CHUNK are always
     * DATA; every other kind is CONTROL. Every kind carrying a RouteKey must
     * be rebound after each catalog generation change.
     */
    MeisoRuntimeSchedulerWorkKind work_kind;
    MeisoRuntimeSchedulerPoll poll;
    MeisoRuntimeSchedulerComplete complete;
    MeisoRuntimeSchedulerBoundaryNotify boundary;
    void *context;
} MeisoRuntimeSchedulerSourceConfig;

typedef struct {
    /* runtime and session must be the exact bound pair. Their complete opaque
     * arenas, the Scheduler arena, callbacks, work and outputs are disjoint. */
    MeisoRuntime *runtime;
    MeisoSession *session;
    uint16_t max_control_sources;
    uint16_t max_data_sources;
} MeisoRuntimeSchedulerConfig;

typedef uint8_t MeisoRuntimeSchedulerInitStatus;
enum {
    MEISO_RUNTIME_SCHEDULER_INIT_OK = 0,
    MEISO_RUNTIME_SCHEDULER_INIT_INVALID_ARGUMENT = 1,
    MEISO_RUNTIME_SCHEDULER_INIT_CORE_NOT_READY = 2,
    MEISO_RUNTIME_SCHEDULER_INIT_INVALID_CONFIG = 3,
    MEISO_RUNTIME_SCHEDULER_INIT_MISALIGNED_ARENA = 4,
    MEISO_RUNTIME_SCHEDULER_INIT_ARENA_TOO_SMALL = 5
};

typedef uint8_t MeisoRuntimeSchedulerStatus;
enum {
    MEISO_RUNTIME_SCHEDULER_OK = 0,
    MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT = 1,
    MEISO_RUNTIME_SCHEDULER_INVALID_STATE = 2,
    MEISO_RUNTIME_SCHEDULER_CAPACITY = 3,
    MEISO_RUNTIME_SCHEDULER_DUPLICATE = 4,
    MEISO_RUNTIME_SCHEDULER_REENTRANT = 5,
    MEISO_RUNTIME_SCHEDULER_STALE_SESSION = 6,
    MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED = 7
};

typedef uint8_t MeisoRuntimeSchedulerStepStatus;
enum {
    MEISO_RUNTIME_SCHEDULER_STEP_SENT = 0,
    MEISO_RUNTIME_SCHEDULER_STEP_IDLE = 1,
    MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY = 2,
    MEISO_RUNTIME_SCHEDULER_STEP_REJECTED = 3,
    MEISO_RUNTIME_SCHEDULER_STEP_SOURCE_INVALID = 4,
    MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED = 5,
    MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION = 6,
    MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL = 7,
    MEISO_RUNTIME_SCHEDULER_STEP_COUNTER_EXHAUSTED = 8,
    MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT = 9,
    MEISO_RUNTIME_SCHEDULER_STEP_INVALID_STATE = 10,
    MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT = 11
};

typedef struct {
    MeisoRuntimeSchedulerCompletion completion;
    uint64_t successful_slot;
    uint64_t data_round;
} MeisoRuntimeSchedulerStepResult;

typedef uint8_t MeisoRuntimeSchedulerPhase;
enum {
    MEISO_RUNTIME_SCHEDULER_PHASE_CONFIGURING = 1,
    MEISO_RUNTIME_SCHEDULER_PHASE_REBIND_REQUIRED = 2,
    MEISO_RUNTIME_SCHEDULER_PHASE_READY = 3,
    MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND = 4,
    MEISO_RUNTIME_SCHEDULER_PHASE_SESSION_TERMINAL = 5
};

typedef uint8_t MeisoRuntimeSchedulerRxObligation;
enum {
    MEISO_RUNTIME_SCHEDULER_RX_NONE = 0,
    MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT = 1,
    MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH = 2
};

typedef struct {
    MeisoSessionId session_id;
    uint64_t catalog_generation;
    uint64_t successful_slots;
    uint64_t successful_control_slots;
    uint64_t successful_data_slots;
    uint64_t data_rounds;
    uint16_t control_source_count;
    uint16_t data_source_count;
    uint16_t data_round_member_count;
    uint8_t successful_control_burst;
    MeisoRuntimeSchedulerPhase phase;
    MeisoRuntimeSchedulerRxObligation rx_obligation;
    bool data_probe_due;
    bool sealed;
} MeisoRuntimeSchedulerSnapshot;

size_t meiso_runtime_scheduler_alignment_required(void);
size_t meiso_runtime_scheduler_memory_required(
    const MeisoRuntimeSchedulerConfig *config
);
MeisoRuntimeSchedulerInitStatus meiso_runtime_scheduler_init(
    void *arena,
    size_t arena_size,
    const MeisoRuntimeSchedulerConfig *config,
    MeisoRuntimeScheduler **out_scheduler
);

/*
 * Sources are registered before seal; source id zero is never issued. One
 * Scheduler owns only this peer's local outbound direction. Other peers use
 * independent Scheduler/Runtime/Session arenas and fairness state.
 */
MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_register_source(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeSchedulerSourceConfig *config,
    MeisoRuntimeSchedulerSourceId *out_source_id
);
MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_seal(
    MeisoRuntimeScheduler *scheduler
);

/*
 * A catalog boundary clears every route binding and the current data round.
 * Each RouteKey-carrying source must be rebound to an exact active RouteKey,
 * then finish_rebind makes TX eligible. Control work carrying a RouteKey is
 * also checked against the current catalog generation.
 * Only an active-catalog generation change triggers CATALOG_CHANGED; a staged
 * offer alone does not. Each source is notified exactly once per observed
 * active generation. If init observes an already-active catalog, seal enters
 * REBIND_REQUIRED without synthesizing a boundary callback; the caller binds
 * that initial generation before finish_rebind.
 */
MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_bind_source_route(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSourceId source_id,
    MeisoRuntimeRouteKey route
);
MeisoRuntimeSchedulerStatus meiso_runtime_scheduler_finish_rebind(
    MeisoRuntimeScheduler *scheduler
);

/*
 * For a scheduler-owned Runtime, all receive calls use this wrapper. A State
 * offer and only a Blob COMMIT whose Runtime snapshot has publish_pending
 * establish an RX obligation; length/SHA precheck failure already has a
 * pending RESULT and does not establish a publication obligation.
 * step then fails closed until the matching wrapper below resolves it.
 */
MeisoRuntimeStatus meiso_runtime_scheduler_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoSessionRuntimeMessage *message,
    MeisoRuntimeEvent *out_event
);
MeisoRuntimeStatus meiso_runtime_scheduler_commit_state_receive(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeRouteKey route,
    const MeisoStateOffer *offer,
    MeisoStateReceiveStatus receive_status
);
MeisoRuntimeStatus meiso_runtime_scheduler_publish_blob_receive(
    MeisoRuntimeScheduler *scheduler,
    const MeisoRuntimeBlobTransferRef *expected_transfer,
    MeisoBlobStore *store,
    MeisoBlobVerifier verifier,
    void *verifier_context,
    MeisoRuntimeBlobPublishReport *out_report
);

/*
 * A bound scheduler is the sole TX owner of its Runtime. Data-ready membership
 * is snapshotted per round; each member gets at most one frame in that round.
 * At most four successful control frames precede a pending data round.
 * TX_BUSY is preflighted before poll: no poll/complete callback runs and the
 * Scheduler, source, Runtime, round/cursors/counters and out_result remain
 * byte-for-byte unchanged. A nonbusy Runtime rejection calls complete with
 * REJECTED and visits that source. A data member receives at most one dispatch
 * attempt per round, including invalid/rejected attempts. Session terminal is
 * permanent and notifies every source exactly once with generation zero.
 * CONTROL uses a persistent round-robin cursor. POLL_INVALID, READY-invalid,
 * and every nonbusy Runtime rejection are visited and advance that cursor;
 * if a complete scan has no dispatchable control, selection continues into
 * DATA rather than allowing a bad/empty control head to block it. A nonbusy
 * CONTROL Runtime rejection sets data_probe_due: the next step probes DATA
 * before CONTROL, without pretending that the rejected frame was successful.
 * If no DATA is ready, CONTROL remains eligible in that same step. TX_BUSY
 * neither sets nor clears data_probe_due.
 */
MeisoRuntimeSchedulerStepStatus meiso_runtime_scheduler_step(
    MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerStepResult *out_result
);
bool meiso_runtime_scheduler_snapshot(
    const MeisoRuntimeScheduler *scheduler,
    MeisoRuntimeSchedulerSnapshot *out_snapshot
);

#ifdef __cplusplus
}
#endif

#endif
