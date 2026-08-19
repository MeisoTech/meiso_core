#include "composition_session_v2.h"
#include "meiso_runtime_scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    TEST_SESSION_ARENA_BYTES = 65536,
    TEST_RUNTIME_ARENA_BYTES = 400000,
    TEST_SCHEDULER_ARENA_BYTES = 16384,
    TEST_FRAME_BYTES = 16384,
    TEST_MAX_SOURCES = 12,
    TEST_MAX_LOG = 512
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_SESSION_ARENA_BYTES];
} SessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_RUNTIME_ARENA_BYTES];
} RuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[TEST_SCHEDULER_ARENA_BYTES];
} SchedulerArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[2097152];
} BlobArena;

typedef struct {
    SessionArena host_session_arena;
    SessionArena edge_session_arena;
    RuntimeArena host_runtime_arena;
    RuntimeArena edge_runtime_arena;
    uint8_t frame[TEST_FRAME_BYTES];
    MeisoSession *host_session;
    MeisoSession *edge_session;
    MeisoRuntime *host_runtime;
    MeisoRuntime *edge_runtime;
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    uint8_t wifi_capability[
        MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
    ];
    uint64_t catalog_generation;
} RuntimePair;

typedef struct {
    MeisoRuntimeSchedulerSourceId source_id;
    uint64_t ticket;
    MeisoRuntimeSchedulerWorkKind kind;
    MeisoRuntimeSchedulerCompletionCode code;
    MeisoRuntimeStatus runtime_status;
} CompletionRecord;

typedef struct {
    CompletionRecord entries[TEST_MAX_LOG];
    size_t count;
} CompletionLog;

typedef struct {
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerWorkKind configured_kind;
    MeisoRuntimeSchedulerSourceId source_id;
    CompletionLog *log;
    uint64_t polls;
    uint64_t completions;
    uint64_t boundary_calls;
    uint64_t last_boundary_catalog;
    MeisoRuntimeSchedulerBoundary last_boundary;
    MeisoRuntimeSchedulerPollStatus poll_status;
    bool clear_after_complete;
    bool reenter_poll;
    bool reenter_complete;
    MeisoRuntimeScheduler *reentrant_scheduler;
    MeisoRuntimeSchedulerStepStatus poll_reentrant_step;
    MeisoRuntimeSchedulerStepStatus complete_reentrant_step;
    bool poll_reentrant_snapshot;
    bool complete_reentrant_snapshot;
} TestSource;

typedef struct {
    SchedulerArena arena;
    MeisoRuntimeScheduler *scheduler;
    TestSource sources[TEST_MAX_SOURCES];
    size_t source_count;
    CompletionLog log;
} SchedulerFixture;

static const MeisoOwnerScope HOST_OWNER = {
    MEISO_OWNER_ROLE_HOST, UINT64_C(0x101)
};
static const MeisoOwnerScope EDGE_OWNER = {
    MEISO_OWNER_ROLE_EDGE, UINT64_C(0x201)
};
static const uint8_t BLOB_BYTES[] = {0xe1U, 0xe2U, 0xe3U, 0xe4U};
static const uint8_t BLOB_SHA256[MEISO_SHA256_DIGEST_SIZE] = {
    0xb0U, 0x06U, 0x7bU, 0x7aU, 0xebU, 0x07U, 0x9cU, 0x61U,
    0x78U, 0x80U, 0x6bU, 0x87U, 0x8dU, 0x06U, 0x0cU, 0x75U,
    0x23U, 0x6cU, 0xf2U, 0xa1U, 0xcfU, 0x65U, 0x8cU, 0x14U,
    0x93U, 0x23U, 0x50U, 0x4aU, 0x31U, 0x35U, 0xf1U, 0x5dU
};

static MeisoSessionConfig base_session_config(
    MeisoSessionRole role,
    uint64_t nonce_salt
)
{
    MeisoSessionConfig config;
    const MeisoPeerId host = {
        UINT64_C(0x1111111111111111), UINT64_C(0x1212121212121212)
    };
    const MeisoPeerId edge = {
        UINT64_C(0x2121212121212121), UINT64_C(0x2222222222222222)
    };

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id = role == MEISO_SESSION_ROLE_HOST ? host : edge;
    config.expected_peer_id = role == MEISO_SESSION_ROLE_HOST ? edge : host;
    config.local_incarnation = role == MEISO_SESSION_ROLE_HOST ?
        HOST_OWNER.incarnation : EDGE_OWNER.incarnation;
    config.local_nonce = role == MEISO_SESSION_ROLE_HOST ?
        (MeisoSessionNonce){UINT64_C(0x3131313131313131), nonce_salt + 1U} :
        (MeisoSessionNonce){UINT64_C(0x4141414141414141), nonce_salt + 2U};
    config.protocol = (MeisoProtocolVersion){1U, 0U};
    config.profile = *meiso_composition_scene_profile_v2();
    config.limits = (MeisoSessionLimits){16384U, 8192U, 32U};
    return config;
}

static bool init_session(
    SessionArena *arena,
    const MeisoSessionConfig *config,
    MeisoSession **out_session
)
{
    const size_t required = meiso_session_memory_required(config);

    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_session_init(
        arena->bytes, required, config, out_session
    ) == MEISO_SESSION_INIT_OK);
    return true;
}

static bool handshake_one(MeisoSession *sender, MeisoSession *receiver)
{
    uint8_t frame[TEST_FRAME_BYTES];
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage ignored = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){frame, outbound.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS);
    return true;
}

static bool initialize_runtime(
    RuntimeArena *arena,
    MeisoSession *session,
    MeisoRuntime **out_runtime
)
{
    MeisoRuntimeV3Config config;
    size_t required;

    CHECK(meiso_composition_session_v2_runtime_config(
        session, &config
    ) == MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(config.base.max_endpoints ==
              MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINTS &&
          config.base.max_endpoint_history ==
              MEISO_COMPOSITION_SESSION_V2_MAX_ENDPOINT_HISTORY &&
          config.base.max_routes ==
              MEISO_COMPOSITION_SESSION_V2_MAX_ROUTES &&
          config.base.max_capability_bytes ==
              MEISO_COMPOSITION_SESSION_V2_MAX_CAPABILITY_BYTES &&
          config.base.max_desired_profile_bytes_per_endpoint ==
              MEISO_COMPOSITION_SESSION_V2_MAX_DESIRED_PROFILE_BYTES &&
          config.base.max_actual_profile_bytes_per_endpoint ==
              MEISO_COMPOSITION_SESSION_V2_MAX_ACTUAL_PROFILE_BYTES &&
          config.max_blob_transfer_bytes ==
              MEISO_COMPOSITION_SESSION_V2_MAX_BLOB_TRANSFER_BYTES);
    required = meiso_runtime_v3_memory_required(&config);
    CHECK(required != 0U && required <= sizeof(arena->bytes));
    CHECK(meiso_runtime_v3_init(
        arena->bytes, required, &config, out_runtime
    ) == MEISO_RUNTIME_INIT_OK);
    return true;
}

static bool build_catalog(RuntimePair *pair)
{
    const MeisoCompositionSessionV2CatalogConfig config = {
        UINT64_C(0x501), UINT64_C(0x601), UINT64_C(0x701),
        {
            MEISO_WIFI_TELEMETRY_FIELD_ALL,
            MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
        }
    };

    CHECK(meiso_composition_session_v2_build_catalog(
        &config,
        (MeisoMutableBuffer){
            pair->wifi_capability, sizeof(pair->wifi_capability)
        },
        pair->endpoints,
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT,
        pair->routes,
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ) == MEISO_COMPOSITION_SESSION_V2_OK);
    return true;
}

static bool initialize_pair(RuntimePair *pair, uint64_t nonce_salt)
{
    MeisoSessionConfig host = base_session_config(
        MEISO_SESSION_ROLE_HOST, nonce_salt
    );
    MeisoSessionConfig edge = base_session_config(
        MEISO_SESSION_ROLE_EDGE, nonce_salt
    );

    memset(pair, 0, sizeof(*pair));
    CHECK(meiso_composition_session_v2_configure_session(&host) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_composition_session_v2_configure_session(&edge) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(init_session(
        &pair->host_session_arena, &host, &pair->host_session
    ));
    CHECK(init_session(
        &pair->edge_session_arena, &edge, &pair->edge_session
    ));
    CHECK(meiso_session_start(pair->edge_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(pair->host_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(handshake_one(pair->host_session, pair->edge_session));
    CHECK(handshake_one(pair->edge_session, pair->host_session));
    CHECK(handshake_one(pair->host_session, pair->edge_session));
    CHECK(handshake_one(pair->edge_session, pair->host_session));
    CHECK(initialize_runtime(
        &pair->host_runtime_arena, pair->host_session, &pair->host_runtime
    ));
    CHECK(initialize_runtime(
        &pair->edge_runtime_arena, pair->edge_session, &pair->edge_runtime
    ));
    CHECK(build_catalog(pair));
    return true;
}

static bool transfer_runtime(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntime *receiver_runtime,
    MeisoRuntimeEvent *out_event,
    size_t *out_payload_size
)
{
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage message = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(pair->frame));
    memmove(pair->frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){pair->frame, outbound.size}, &message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    if (out_payload_size != NULL) {
        *out_payload_size = message.payload.size;
    }
    CHECK(meiso_runtime_receive(receiver_runtime, &message, out_event) ==
          MEISO_RUNTIME_OK);
    return true;
}

static bool transfer_scheduler(
    RuntimePair *pair,
    MeisoSession *sender,
    MeisoSession *receiver,
    MeisoRuntimeScheduler *receiver_scheduler,
    MeisoRuntimeEvent *out_event
)
{
    MeisoByteView outbound;
    MeisoSessionRuntimeMessage message = {0};

    CHECK(meiso_session_next_outbound(sender, &outbound));
    CHECK(outbound.size <= sizeof(pair->frame));
    memmove(pair->frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(sender) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        receiver, (MeisoByteView){pair->frame, outbound.size}, &message
    ) == MEISO_SESSION_RECEIVE_RUNTIME);
    CHECK(meiso_runtime_scheduler_receive(
        receiver_scheduler, &message, out_event
    ) == MEISO_RUNTIME_OK);
    return true;
}

static bool drain_outbound(MeisoSession *session)
{
    MeisoByteView outbound;

    CHECK(meiso_session_next_outbound(session, &outbound));
    CHECK(outbound.data != NULL && outbound.size != 0U);
    CHECK(meiso_session_mark_outbound_sent(session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    return true;
}

static bool activate_catalog(RuntimePair *pair, uint64_t generation)
{
    MeisoRuntimeEvent event = {0};
    size_t payload_size = 0U;

    CHECK(meiso_runtime_offer_catalog(
        pair->edge_runtime,
        generation,
        pair->endpoints,
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT,
        pair->routes,
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->edge_session,
        pair->host_session,
        pair->host_runtime,
        &event,
        &payload_size
    ));
    CHECK(payload_size == MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE);
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == generation);
    CHECK(meiso_runtime_accept_catalog(pair->host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->host_session,
        pair->edge_session,
        pair->edge_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == generation);
    pair->catalog_generation = generation;
    return true;
}

static MeisoRuntimeRouteKey route_key(
    const RuntimePair *pair,
    size_t index
)
{
    const MeisoRuntimeRouteDescriptor *route = &pair->routes[index];
    const MeisoRuntimeRouteKey key = {
        pair->catalog_generation,
        route->endpoint_id,
        route->endpoint_incarnation,
        route->resource.id,
        route->facet
    };

    return key;
}

static bool activate_stream_endpoint(
    RuntimePair *pair,
    size_t desired_index,
    size_t actual_index,
    uint64_t production_epoch
)
{
    uint8_t desired_bytes[MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE];
    uint8_t actual_bytes[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE];
    MeisoByteView payload;
    MeisoRuntimeDesiredState desired = {true, {NULL, 0U}};
    MeisoRuntimeDesiredReference desired_ref = {
        true, HOST_OWNER, production_epoch
    };
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoRuntimeEvent event = {0};

    CHECK(meiso_runtime_desired_state_encode(
        &desired,
        (MeisoMutableBuffer){desired_bytes, sizeof(desired_bytes)},
        &payload
    ));
    offer.generation.value = production_epoch;
    offer.state.owner = HOST_OWNER;
    offer.state.version = production_epoch;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair->host_runtime, route_key(pair, desired_index), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->host_session,
        pair->edge_session,
        pair->edge_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(
        pair->edge_runtime,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);

    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = desired_ref;
    actual.applied_desired = desired_ref;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = production_epoch;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &payload
    ));
    offer.generation.value = production_epoch;
    offer.state.owner = EDGE_OWNER;
    offer.state.version = production_epoch;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair->edge_runtime, route_key(pair, actual_index), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->edge_session,
        pair->host_session,
        pair->host_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_commit_state_receive(
        pair->host_runtime,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    return true;
}

static MeisoContentId blob_content_id(void)
{
    MeisoContentId value;

    memset(&value, 0, sizeof(value));
    value.algorithm = MEISO_CONTENT_ALGORITHM_SHA256;
    value.digest_size = MEISO_SHA256_DIGEST_SIZE;
    memmove(value.digest, BLOB_SHA256, sizeof(BLOB_SHA256));
    return value;
}

static bool prepare_blob_transfer(
    RuntimePair *pair,
    MeisoRuntimeBlobTransferRef *out_transfer
)
{
    const MeisoContentId content_id = blob_content_id();
    MeisoRuntimeEvent event = {0};

    CHECK(meiso_runtime_send_blob_begin(
        pair->host_runtime,
        route_key(pair, 7U),
        &content_id,
        sizeof(BLOB_BYTES),
        out_transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->host_session,
        pair->edge_session,
        pair->edge_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_BEGIN);
    CHECK(meiso_runtime_send_blob_result(
        pair->edge_runtime, &event.blob_transfer, MEISO_RUNTIME_BLOB_BEGIN
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_runtime(
        pair,
        pair->edge_session,
        pair->host_session,
        pair->host_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_READY);
    return true;
}

static MeisoRuntimeSchedulerPollStatus source_poll(
    void *context,
    MeisoRuntimeSchedulerWork *out_work
)
{
    TestSource *source = (TestSource *)context;

    if (source == NULL || out_work == NULL) {
        return MEISO_RUNTIME_SCHEDULER_POLL_INVALID;
    }
    source->polls++;
    if (source->reenter_poll) {
        MeisoRuntimeSchedulerStepResult result;
        MeisoRuntimeSchedulerSnapshot snapshot;

        memset(&result, 0xa5, sizeof(result));
        source->poll_reentrant_step = meiso_runtime_scheduler_step(
            source->reentrant_scheduler, &result
        );
        source->poll_reentrant_snapshot = meiso_runtime_scheduler_snapshot(
            source->reentrant_scheduler, &snapshot
        );
    }
    if (source->poll_status != MEISO_RUNTIME_SCHEDULER_POLL_READY) {
        return source->poll_status;
    }
    *out_work = source->work;
    return MEISO_RUNTIME_SCHEDULER_POLL_READY;
}

static void source_complete(
    void *context,
    const MeisoRuntimeSchedulerCompletion *completion
)
{
    TestSource *source = (TestSource *)context;

    if (source == NULL || completion == NULL) {
        return;
    }
    if (source->log != NULL && source->log->count < TEST_MAX_LOG) {
        CompletionRecord *record = &source->log->entries[
            source->log->count++
        ];

        record->source_id = completion->source_id;
        record->ticket = completion->ticket;
        record->kind = completion->kind;
        record->code = completion->code;
        record->runtime_status = completion->runtime_status;
    }
    source->completions++;
    if (source->reenter_complete) {
        MeisoRuntimeSchedulerStepResult result;
        MeisoRuntimeSchedulerSnapshot snapshot;

        memset(&result, 0xa5, sizeof(result));
        source->complete_reentrant_step = meiso_runtime_scheduler_step(
            source->reentrant_scheduler, &result
        );
        source->complete_reentrant_snapshot =
            meiso_runtime_scheduler_snapshot(
                source->reentrant_scheduler, &snapshot
            );
    }
    if (source->clear_after_complete) {
        source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
        return;
    }
    source->work.ticket++;
    if (source->work.ticket == 0U) {
        source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    }
    switch (source->work.kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        source->work.value.state_accept.generation.value++;
        source->work.value.state_accept.version++;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        source->work.value.stream_sample.sample.key.sequence++;
        source->work.value.stream_sample.sample.source_time.monotonic_ns++;
        break;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        source->work.value.blob_chunk.offset +=
            source->work.value.blob_chunk.payload.size;
        break;
    default:
        break;
    }
}

static void source_boundary(
    void *context,
    MeisoRuntimeSchedulerBoundary boundary,
    uint64_t catalog_generation
)
{
    TestSource *source = (TestSource *)context;

    if (source == NULL) {
        return;
    }
    source->boundary_calls++;
    source->last_boundary = boundary;
    source->last_boundary_catalog = catalog_generation;
}

static bool scheduler_init_fixture(
    SchedulerFixture *fixture,
    MeisoRuntime *runtime,
    MeisoSession *session,
    uint16_t max_control_sources,
    uint16_t max_data_sources
)
{
    const MeisoRuntimeSchedulerConfig config = {
        runtime, session, max_control_sources, max_data_sources
    };
    const size_t required = meiso_runtime_scheduler_memory_required(&config);

    memset(fixture, 0, sizeof(*fixture));
    CHECK(required != 0U && required <= sizeof(fixture->arena.bytes));
    CHECK(meiso_runtime_scheduler_init(
        fixture->arena.bytes,
        required,
        &config,
        &fixture->scheduler
    ) == MEISO_RUNTIME_SCHEDULER_INIT_OK);
    return true;
}

static bool scheduler_add_source(
    SchedulerFixture *fixture,
    MeisoRuntimeSchedulerWorkKind configured_kind,
    const MeisoRuntimeSchedulerWork *work,
    TestSource **out_source
)
{
    TestSource *source;
    MeisoRuntimeSchedulerSourceConfig config;

    CHECK(fixture->source_count < TEST_MAX_SOURCES);
    source = &fixture->sources[fixture->source_count++];
    memset(source, 0, sizeof(*source));
    source->configured_kind = configured_kind;
    source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    source->log = &fixture->log;
    if (work != NULL) {
        source->work = *work;
    }
    config.work_kind = configured_kind;
    config.poll = source_poll;
    config.complete = source_complete;
    config.boundary = source_boundary;
    config.context = source;
    CHECK(meiso_runtime_scheduler_register_source(
        fixture->scheduler, &config, &source->source_id
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    if (out_source != NULL) {
        *out_source = source;
    }
    return true;
}

static bool work_route(
    const MeisoRuntimeSchedulerWork *work,
    MeisoRuntimeRouteKey *out_route
)
{
    switch (work->kind) {
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_OFFER:
        *out_route = work->value.state_offer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT:
        *out_route = work->value.state_accept.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE:
        *out_route = work->value.stream_sample.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_BEGIN:
        *out_route = work->value.blob_begin.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK:
        *out_route = work->value.blob_chunk.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_COMMIT:
        *out_route = work->value.blob_commit.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_ABORT:
        *out_route = work->value.blob_abort.transfer.route;
        return true;
    case MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT:
        *out_route = work->value.blob_result.transfer.route;
        return true;
    default:
        memset(out_route, 0, sizeof(*out_route));
        return false;
    }
}

static bool scheduler_seal_and_bind(SchedulerFixture *fixture)
{
    size_t index;

    CHECK(meiso_runtime_scheduler_seal(fixture->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    for (index = 0U; index < fixture->source_count; ++index) {
        MeisoRuntimeRouteKey route;
        TestSource *source = &fixture->sources[index];

        if (work_route(&source->work, &route)) {
            CHECK(meiso_runtime_scheduler_bind_source_route(
                fixture->scheduler, source->source_id, route
            ) == MEISO_RUNTIME_SCHEDULER_OK);
        }
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture->scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    return true;
}

static MeisoRuntimeSchedulerWork state_accept_work(
    MeisoRuntimeRouteKey route,
    MeisoOwnerScope owner,
    uint64_t ticket
)
{
    MeisoRuntimeSchedulerWork work;

    memset(&work, 0, sizeof(work));
    work.ticket = ticket;
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    work.value.state_accept.route = route;
    work.value.state_accept.generation.value = ticket;
    work.value.state_accept.owner = owner;
    work.value.state_accept.version = ticket;
    return work;
}

static MeisoRuntimeSchedulerWork stream_work(
    MeisoRuntimeRouteKey route,
    uint64_t production_epoch,
    uint64_t ticket,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoRuntimeSchedulerWork work;

    memset(&work, 0, sizeof(work));
    work.ticket = ticket;
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    work.value.stream_sample.route = route;
    work.value.stream_sample.production_epoch = production_epoch;
    work.value.stream_sample.sample.key.producer = EDGE_OWNER;
    work.value.stream_sample.sample.key.epoch = 1U;
    work.value.stream_sample.sample.key.sequence = ticket;
    work.value.stream_sample.sample.source_time = (MeisoTimePoint){
        1U, ticket
    };
    work.value.stream_sample.sample.payload = (MeisoByteView){
        payload, payload_size
    };
    return work;
}

static MeisoRuntimeSchedulerWork blob_chunk_work(
    const MeisoRuntimeBlobTransferRef *transfer,
    uint64_t ticket
)
{
    MeisoRuntimeSchedulerWork work;

    memset(&work, 0, sizeof(work));
    work.ticket = ticket;
    work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK;
    work.value.blob_chunk.transfer = *transfer;
    work.value.blob_chunk.payload = (MeisoByteView){
        BLOB_BYTES, sizeof(BLOB_BYTES)
    };
    return work;
}

static bool step_sent(
    MeisoRuntimeScheduler *scheduler,
    MeisoSession *session,
    MeisoRuntimeSchedulerSourceId expected_source,
    MeisoRuntimeSchedulerStepResult *out_result
)
{
    CHECK(meiso_runtime_scheduler_step(scheduler, out_result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(out_result->completion.source_id == expected_source &&
          out_result->completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT &&
          out_result->completion.runtime_status == MEISO_RUNTIME_OK);
    CHECK(drain_outbound(session));
    return true;
}

static bool endpoint_equal(MeisoEndpointId lhs, MeisoEndpointId rhs)
{
    return meiso_endpoint_id_equal(lhs, rhs);
}

static bool resource_equal(MeisoResourceId lhs, MeisoResourceId rhs)
{
    return meiso_resource_id_equal(lhs, rhs);
}

static bool expect_profile_mismatch(MeisoProfileRef edge_profile)
{
    SessionArena host_arena;
    SessionArena edge_arena;
    MeisoSession *host_session = NULL;
    MeisoSession *edge_session = NULL;
    MeisoSessionConfig host = base_session_config(
        MEISO_SESSION_ROLE_HOST, 90U
    );
    MeisoSessionConfig edge = base_session_config(
        MEISO_SESSION_ROLE_EDGE, 90U
    );
    MeisoSessionRuntimeMessage ignored = {0};
    MeisoSessionSnapshot snapshot;
    MeisoByteView outbound;
    uint8_t frame[TEST_FRAME_BYTES];

    CHECK(meiso_composition_session_v2_configure_session(&host) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_composition_session_v2_configure_session(&edge) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    edge.profile = edge_profile;
    CHECK(init_session(&host_arena, &host, &host_session));
    CHECK(init_session(&edge_arena, &edge, &edge_session));
    CHECK(meiso_session_start(edge_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_start(host_session) ==
          MEISO_SESSION_START_STARTED);
    CHECK(meiso_session_next_outbound(host_session, &outbound));
    CHECK(outbound.size <= sizeof(frame));
    memmove(frame, outbound.data, outbound.size);
    CHECK(meiso_session_mark_outbound_sent(host_session) ==
          MEISO_SESSION_OUTBOUND_SENT);
    CHECK(meiso_session_receive(
        edge_session, (MeisoByteView){frame, outbound.size}, &ignored
    ) == MEISO_SESSION_RECEIVE_FAILED);
    CHECK(meiso_session_snapshot(edge_session, &snapshot));
    CHECK(snapshot.state == MEISO_SESSION_STATE_FAILED &&
          snapshot.failure == MEISO_SESSION_FAILURE_PROFILE_MISMATCH);
    return true;
}

typedef union {
    max_align_t alignment;
    MeisoCompositionSessionV2CatalogConfig config;
    uint8_t storage[MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    uint8_t bytes[2048];
} BuilderAlias;

static MeisoCompositionSessionV2CatalogConfig catalog_config(void)
{
    const MeisoCompositionSessionV2CatalogConfig config = {
        UINT64_C(0x501), UINT64_C(0x601), UINT64_C(0x701),
        {
            MEISO_WIFI_TELEMETRY_FIELD_ALL,
            MEISO_WIFI_TELEMETRY_MIN_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_MAX_PERIOD_MS,
            MEISO_WIFI_TELEMETRY_PERIOD_STEP_MS
        }
    };

    return config;
}

static bool test_builder_alias_pair(unsigned pair)
{
    BuilderAlias alias;
    BuilderAlias before;
    MeisoCompositionSessionV2CatalogConfig separate_config =
        catalog_config();
    uint8_t separate_storage[
        MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES
    ];
    MeisoRuntimeEndpointDescriptor separate_endpoints[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor separate_routes[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    const MeisoCompositionSessionV2CatalogConfig *config = &separate_config;
    MeisoMutableBuffer storage = {
        separate_storage, sizeof(separate_storage)
    };
    MeisoRuntimeEndpointDescriptor *endpoints = separate_endpoints;
    MeisoRuntimeRouteDescriptor *routes = separate_routes;

    memset(&alias, 0xa5, sizeof(alias));
    memset(separate_storage, 0xb6, sizeof(separate_storage));
    memset(separate_endpoints, 0xc7, sizeof(separate_endpoints));
    memset(separate_routes, 0xd8, sizeof(separate_routes));
    switch (pair) {
    case 0U:
        alias.config = separate_config;
        config = &alias.config;
        storage = (MeisoMutableBuffer){
            alias.storage, sizeof(alias.storage)
        };
        break;
    case 1U:
        alias.config = separate_config;
        config = &alias.config;
        endpoints = alias.endpoints;
        break;
    case 2U:
        alias.config = separate_config;
        config = &alias.config;
        routes = alias.routes;
        break;
    case 3U:
        storage = (MeisoMutableBuffer){
            alias.storage, sizeof(alias.storage)
        };
        endpoints = alias.endpoints;
        break;
    case 4U:
        storage = (MeisoMutableBuffer){
            alias.storage, sizeof(alias.storage)
        };
        routes = alias.routes;
        break;
    case 5U:
        endpoints = alias.endpoints;
        routes = alias.routes;
        break;
    default:
        CHECK(false);
    }
    before = alias;
    CHECK(meiso_composition_session_v2_build_catalog(
        config,
        storage,
        endpoints,
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT,
        routes,
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ) == MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT);
    CHECK(memcmp(&alias, &before, sizeof(alias)) == 0);
    return true;
}

static bool test_composite_profile_catalog_builder(void)
{
    RuntimePair pair;
    MeisoSessionConfig configured = base_session_config(
        MEISO_SESSION_ROLE_HOST, 0U
    );
    MeisoSessionConfig before_config;
    MeisoRuntimeV3Config runtime_output;
    MeisoRuntimeV3Config runtime_before;
    const MeisoResourceDescriptor *wifi_resources;
    const MeisoResourceDescriptor *composition_resources;
    size_t wifi_count = 0U;
    size_t composition_count = 0U;
    size_t index;
    unsigned pair_index;

    CHECK(MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT == 3U &&
          MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT == 9U &&
          MEISO_COMPOSITION_SESSION_V2_CATALOG_WIRE_SIZE == 920U);
    CHECK(meiso_composition_session_v2_configure_session(&configured) ==
          MEISO_COMPOSITION_SESSION_V2_OK);
    CHECK(meiso_profile_ref_equal(
        configured.profile, *meiso_composition_scene_profile_v2()
    ));
    CHECK(configured.limits.max_frame_bytes ==
              MEISO_COMPOSITION_SESSION_V2_MAX_FRAME_BYTES &&
          configured.limits.max_runtime_payload_bytes ==
              MEISO_COMPOSITION_SESSION_V2_MAX_RUNTIME_PAYLOAD_BYTES &&
          configured.limits.max_registry_entries ==
              MEISO_COMPOSITION_SESSION_V2_MAX_REGISTRY_ENTRIES);
    before_config = configured;
    configured.local_nonce = (MeisoSessionNonce){0U, 0U};
    before_config = configured;
    CHECK(meiso_composition_session_v2_configure_session(&configured) ==
          MEISO_COMPOSITION_SESSION_V2_INVALID_CONFIG);
    CHECK(memcmp(&configured, &before_config, sizeof(configured)) == 0);
    memset(&runtime_output, 0xa5, sizeof(runtime_output));
    runtime_before = runtime_output;
    CHECK(meiso_composition_session_v2_runtime_config(
        NULL, &runtime_output
    ) == MEISO_COMPOSITION_SESSION_V2_INVALID_ARGUMENT);
    CHECK(memcmp(
        &runtime_output, &runtime_before, sizeof(runtime_output)
    ) == 0);

    CHECK(initialize_pair(&pair, 0U));
    CHECK(endpoint_equal(
        pair.endpoints[0].id, *meiso_wifi_telemetry_endpoint()
    ));
    CHECK(endpoint_equal(
        pair.endpoints[1].id,
        *meiso_composition_scene_presentation_endpoint()
    ));
    CHECK(endpoint_equal(
        pair.endpoints[2].id, *meiso_composition_scene_scene_endpoint()
    ));
    CHECK(pair.endpoints[0].capability.size ==
              MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE &&
          pair.endpoints[1].capability.size ==
              MEISO_COMPOSITION_SCENE_PRESENTATION_CAPABILITY_WIRE_SIZE &&
          pair.endpoints[2].capability.size ==
              MEISO_COMPOSITION_SCENE_CAPABILITY_WIRE_SIZE);
    wifi_resources = meiso_wifi_telemetry_resources(&wifi_count);
    composition_resources = meiso_composition_scene_resources(
        &composition_count
    );
    CHECK(wifi_resources != NULL && wifi_count == 3U &&
          composition_resources != NULL && composition_count == 6U);
    for (index = 0U; index < wifi_count; ++index) {
        CHECK(resource_equal(
            pair.routes[index].resource.id, wifi_resources[index].id
        ));
    }
    for (index = 0U; index < composition_count; ++index) {
        CHECK(resource_equal(
            pair.routes[wifi_count + index].resource.id,
            composition_resources[index].id
        ));
    }
    CHECK(activate_catalog(&pair, 1U));

    CHECK(expect_profile_mismatch(*meiso_composition_scene_profile()));
    CHECK(expect_profile_mismatch(*meiso_wifi_telemetry_profile()));
    for (pair_index = 0U; pair_index < 6U; ++pair_index) {
        CHECK(test_builder_alias_pair(pair_index));
    }
    return true;
}

static bool test_builder_failure_no_partial(void)
{
    MeisoCompositionSessionV2CatalogConfig config = catalog_config();
    uint8_t storage[MEISO_COMPOSITION_SESSION_V2_WIFI_CAPABILITY_BYTES];
    uint8_t storage_before[sizeof(storage)];
    MeisoRuntimeEndpointDescriptor endpoints[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeEndpointDescriptor endpoints_before[
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT
    ];
    MeisoRuntimeRouteDescriptor routes[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    MeisoRuntimeRouteDescriptor routes_before[
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT
    ];
    unsigned variant;

    for (variant = 0U; variant < 5U; ++variant) {
        MeisoCompositionSessionV2CatalogConfig value = config;
        size_t storage_size = sizeof(storage);
        size_t endpoint_capacity =
            MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT;
        size_t route_capacity = MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT;

        memset(storage, 0xa1, sizeof(storage));
        memset(endpoints, 0xb2, sizeof(endpoints));
        memset(routes, 0xc3, sizeof(routes));
        memmove(storage_before, storage, sizeof(storage));
        memmove(endpoints_before, endpoints, sizeof(endpoints));
        memmove(routes_before, routes, sizeof(routes));
        if (variant == 0U) {
            value.wifi_incarnation = 0U;
        } else if (variant == 1U) {
            value.wifi_capability.period_step_ms = 0U;
        } else if (variant == 2U) {
            storage_size--;
        } else if (variant == 3U) {
            endpoint_capacity--;
        } else {
            route_capacity--;
        }
        CHECK(meiso_composition_session_v2_build_catalog(
            &value,
            (MeisoMutableBuffer){storage, storage_size},
            endpoints,
            endpoint_capacity,
            routes,
            route_capacity
        ) != MEISO_COMPOSITION_SESSION_V2_OK);
        CHECK(memcmp(storage, storage_before, sizeof(storage)) == 0 &&
              memcmp(endpoints, endpoints_before, sizeof(endpoints)) == 0 &&
              memcmp(routes, routes_before, sizeof(routes)) == 0);
    }
    return true;
}

static bool test_scheduler_arena_registration_and_binding(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    SchedulerArena exact_arena;
    SchedulerArena exact_before;
    SchedulerArena alias_arena;
    SchedulerArena alias_before;
    SchedulerArena scheduler_before;
    MeisoRuntimeScheduler *scheduler = NULL;
    MeisoRuntimeScheduler *sentinel = (MeisoRuntimeScheduler *)(uintptr_t)1U;
    MeisoRuntimeSchedulerConfig config;
    MeisoRuntimeSchedulerConfig aliased_config;
    MeisoRuntimeSchedulerSnapshot snapshot;
    MeisoRuntimeSchedulerWork control_work;
    MeisoRuntimeSchedulerWork data_work;
    TestSource *control;
    TestSource *data;
    MeisoRuntimeSchedulerSourceConfig source_config;
    union {
        max_align_t alignment;
        MeisoRuntimeSchedulerSourceConfig config;
        MeisoRuntimeSchedulerSourceId source_id;
    } register_alias, register_alias_before;
    MeisoRuntimeSchedulerSourceId source_id = UINT16_MAX;
    MeisoRuntimeRouteKey wrong_route;
    static const uint8_t payload[] = {0x41U};
    size_t required;

    CHECK(initialize_pair(&pair, 10U));
    CHECK(activate_catalog(&pair, 1U));
    config = (MeisoRuntimeSchedulerConfig){
        pair.edge_runtime, pair.edge_session, 1U, 1U
    };
    required = meiso_runtime_scheduler_memory_required(&config);
    CHECK(required != 0U && required <= sizeof(exact_arena.bytes));
    CHECK(meiso_runtime_scheduler_alignment_required() ==
          _Alignof(max_align_t));
    scheduler = sentinel;
    CHECK(meiso_runtime_scheduler_init(
        exact_arena.bytes, required - 1U, &config, &scheduler
    ) == MEISO_RUNTIME_SCHEDULER_INIT_ARENA_TOO_SMALL);
    CHECK(scheduler == NULL);
    scheduler = sentinel;
    CHECK(meiso_runtime_scheduler_init(
        exact_arena.bytes + 1U, required, &config, &scheduler
    ) == MEISO_RUNTIME_SCHEDULER_INIT_MISALIGNED_ARENA);
    CHECK(scheduler == NULL);
    memset(&exact_arena, 0x5a, sizeof(exact_arena));
    exact_before = exact_arena;
    CHECK(meiso_runtime_scheduler_init(
        exact_arena.bytes,
        required,
        &config,
        (MeisoRuntimeScheduler **)(void *)exact_arena.bytes
    ) == MEISO_RUNTIME_SCHEDULER_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&exact_arena, &exact_before, sizeof(exact_arena)) == 0);
    memset(&alias_arena, 0, sizeof(alias_arena));
    aliased_config = config;
    memmove(alias_arena.bytes, &aliased_config, sizeof(aliased_config));
    alias_before = alias_arena;
    scheduler = sentinel;
    CHECK(meiso_runtime_scheduler_init(
        alias_arena.bytes,
        required,
        (const MeisoRuntimeSchedulerConfig *)(const void *)alias_arena.bytes,
        &scheduler
    ) == MEISO_RUNTIME_SCHEDULER_INIT_INVALID_ARGUMENT);
    CHECK(scheduler == sentinel &&
          memcmp(&alias_arena, &alias_before, sizeof(alias_arena)) == 0);

    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 1U
    ));
    control_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    data_work = stream_work(
        route_key(&pair, 2U), 1U, 1U, payload, sizeof(payload)
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &control_work,
        &control
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &data_work,
        &data
    ));
    source_config = (MeisoRuntimeSchedulerSourceConfig){
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        source_poll,
        source_complete,
        source_boundary,
        control
    };
    register_alias.config = source_config;
    register_alias_before = register_alias;
    CHECK(meiso_runtime_scheduler_register_source(
        fixture.scheduler,
        &register_alias.config,
        &register_alias.source_id
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT);
    CHECK(memcmp(
        &register_alias, &register_alias_before, sizeof(register_alias)
    ) == 0);
    CHECK(meiso_runtime_scheduler_register_source(
        fixture.scheduler, &source_config, &source_id
    ) == MEISO_RUNTIME_SCHEDULER_CAPACITY);
    CHECK(source_id == UINT16_MAX);
    scheduler_before = fixture.arena;
    CHECK(meiso_runtime_scheduler_register_source(
        fixture.scheduler,
        &source_config,
        (MeisoRuntimeSchedulerSourceId *)(void *)fixture.arena.bytes
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT);
    CHECK(memcmp(
        &fixture.arena, &scheduler_before, sizeof(fixture.arena)
    ) == 0);
    source_config.work_kind = MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE;
    source_config.context = data;
    CHECK(meiso_runtime_scheduler_register_source(
        fixture.scheduler, &source_config, &source_id
    ) == MEISO_RUNTIME_SCHEDULER_CAPACITY);
    CHECK(source_id == UINT16_MAX);
    CHECK(meiso_runtime_scheduler_seal(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    wrong_route = route_key(&pair, 0U);
    wrong_route.endpoint_id.high ^= UINT64_C(0x1);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler, control->source_id, wrong_route
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT);
    wrong_route = route_key(&pair, 0U);
    wrong_route.resource_id.high ^= UINT64_C(0x1);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler, control->source_id, wrong_route
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT);
    wrong_route = route_key(&pair, 0U);
    wrong_route.facet = MEISO_RUNTIME_FACET_ACTUAL;
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler, control->source_id, wrong_route
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_ARGUMENT);
    wrong_route = route_key(&pair, 0U);
    wrong_route.catalog_generation++;
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler, control->source_id, wrong_route
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_STATE);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        control->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        data->source_id,
        route_key(&pair, 2U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_seal(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_DUPLICATE);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        control->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_INVALID_STATE);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          snapshot.control_source_count == 1U &&
          snapshot.data_source_count == 1U && snapshot.sealed);
    scheduler_before = fixture.arena;
    CHECK(!meiso_runtime_scheduler_snapshot(
        fixture.scheduler,
        (MeisoRuntimeSchedulerSnapshot *)(void *)fixture.arena.bytes
    ));
    CHECK(memcmp(
        &fixture.arena, &scheduler_before, sizeof(fixture.arena)
    ) == 0);
    CHECK(meiso_runtime_scheduler_step(
        fixture.scheduler,
        (MeisoRuntimeSchedulerStepResult *)(void *)fixture.arena.bytes
    ) == MEISO_RUNTIME_SCHEDULER_STEP_INVALID_ARGUMENT);
    CHECK(memcmp(
        &fixture.arena, &scheduler_before, sizeof(fixture.arena)
    ) == 0 && control->polls == 0U && data->polls == 0U);
    return true;
}

static bool test_host_control_flood_blob_bound(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeSchedulerWork control_work;
    MeisoRuntimeSchedulerWork data_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    TestSource *control;
    TestSource *data;
    unsigned slot;

    CHECK(initialize_pair(&pair, 20U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(prepare_blob_transfer(&pair, &transfer));
    CHECK(scheduler_init_fixture(
        &fixture, pair.host_runtime, pair.host_session, 1U, 1U
    ));
    control_work = state_accept_work(
        route_key(&pair, 1U), EDGE_OWNER, 1U
    );
    data_work = blob_chunk_work(&transfer, 1U);
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &control_work,
        &control
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK,
        &data_work,
        &data
    ));
    data->clear_after_complete = true;
    CHECK(scheduler_seal_and_bind(&fixture));

    for (slot = 1U; slot <= 4U; ++slot) {
        CHECK(step_sent(
            fixture.scheduler, pair.host_session, control->source_id, &result
        ));
        CHECK(result.successful_slot == slot && result.data_round == 0U);
    }
    CHECK(step_sent(
        fixture.scheduler, pair.host_session, data->source_id, &result
    ));
    CHECK(result.successful_slot == 5U && result.data_round == 1U &&
          result.completion.kind ==
              MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.successful_slots == 5U &&
          snapshot.successful_control_slots == 4U &&
          snapshot.successful_data_slots == 1U &&
          snapshot.data_rounds == 1U &&
          snapshot.successful_control_burst == 0U);
    return true;
}

static bool test_edge_fairness_rr_and_two_streams(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork control_a_work;
    MeisoRuntimeSchedulerWork control_b_work;
    MeisoRuntimeSchedulerWork wifi_work;
    MeisoRuntimeSchedulerWork render_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    TestSource *control_a;
    TestSource *control_b;
    TestSource *wifi;
    TestSource *render;
    static const uint8_t wifi_payload[] = {0x51U};
    static const uint8_t render_payload[] = {0x61U, 0x62U};
    const MeisoRuntimeSchedulerSourceId expected_controls[4] = {
        1U, 2U, 1U, 2U
    };
    unsigned slot;

    CHECK(initialize_pair(&pair, 30U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 11U));
    CHECK(activate_stream_endpoint(&pair, 5U, 6U, 12U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 2U, 2U
    ));
    control_a_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    control_b_work = state_accept_work(
        route_key(&pair, 5U), HOST_OWNER, 1U
    );
    wifi_work = stream_work(
        route_key(&pair, 2U), 11U, 1U,
        wifi_payload, sizeof(wifi_payload)
    );
    render_work = stream_work(
        route_key(&pair, 8U), 12U, 1U,
        render_payload, sizeof(render_payload)
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &control_a_work,
        &control_a
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &control_b_work,
        &control_b
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &wifi_work,
        &wifi
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &render_work,
        &render
    ));
    CHECK(scheduler_seal_and_bind(&fixture));

    for (slot = 0U; slot < 4U; ++slot) {
        CHECK(step_sent(
            fixture.scheduler,
            pair.edge_session,
            expected_controls[slot],
            &result
        ));
        CHECK(result.successful_slot == slot + 1U);
    }
    CHECK(fixture.log.entries[0].source_id == control_a->source_id &&
          fixture.log.entries[1].source_id == control_b->source_id &&
          fixture.log.entries[2].source_id == control_a->source_id &&
          fixture.log.entries[3].source_id == control_b->source_id);
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, wifi->source_id, &result
    ));
    CHECK(result.successful_slot == 5U && result.data_round == 1U);
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, render->source_id, &result
    ));
    CHECK(result.successful_slot == 6U && result.data_round == 1U);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.successful_slots == 6U &&
          snapshot.successful_control_slots == 4U &&
          snapshot.successful_data_slots == 2U &&
          snapshot.data_rounds == 1U &&
          snapshot.data_round_member_count == 0U);
    return true;
}

static bool test_data_round_membership_and_cursor_wrap(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork wifi_work;
    MeisoRuntimeSchedulerWork render_work;
    MeisoRuntimeSchedulerWork late_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    MeisoRuntimeBlobTransferRef fake_transfer;
    TestSource *wifi;
    TestSource *render;
    TestSource *late;
    static const uint8_t wifi_payload[] = {0x71U};
    static const uint8_t render_payload[] = {0x72U};

    CHECK(initialize_pair(&pair, 40U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 21U));
    CHECK(activate_stream_endpoint(&pair, 5U, 6U, 22U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 3U
    ));
    wifi_work = stream_work(
        route_key(&pair, 2U), 21U, 1U,
        wifi_payload, sizeof(wifi_payload)
    );
    render_work = stream_work(
        route_key(&pair, 8U), 22U, 1U,
        render_payload, sizeof(render_payload)
    );
    memset(&fake_transfer, 0, sizeof(fake_transfer));
    fake_transfer.route = route_key(&pair, 7U);
    fake_transfer.transfer_id = 1U;
    fake_transfer.content_id = blob_content_id();
    fake_transfer.total_size = sizeof(BLOB_BYTES);
    late_work = blob_chunk_work(&fake_transfer, 1U);
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &wifi_work,
        &wifi
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &render_work,
        &render
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK,
        &late_work,
        &late
    ));
    late->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_INVALID;
    CHECK(scheduler_seal_and_bind(&fixture));

    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, wifi->source_id, &result
    ));
    CHECK(result.data_round == 1U);
    CHECK(late->polls == 1U && late->completions == 0U);
    late->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, render->source_id, &result
    ));
    CHECK(result.data_round == 1U);
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REJECTED);
    CHECK(result.completion.source_id == late->source_id &&
          result.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          result.completion.runtime_status != MEISO_RUNTIME_TX_BUSY &&
          result.data_round == 2U);
    CHECK(late->completions == 1U);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.data_rounds == 2U &&
          snapshot.data_round_member_count == 2U &&
          snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_DATA_ROUND);
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, wifi->source_id, &result
    ));
    CHECK(result.data_round == 2U);
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, render->source_id, &result
    ));
    CHECK(result.data_round == 2U);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.data_round_member_count == 0U &&
          snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY);
    return true;
}

static bool test_invalid_data_member_preserves_round_peers(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork wifi_work;
    MeisoRuntimeSchedulerWork render_work;
    MeisoRuntimeSchedulerWork blob_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    MeisoRuntimeBlobTransferRef fake_transfer;
    TestSource *wifi;
    TestSource *render;
    TestSource *blob;
    static const uint8_t wifi_payload[] = {0x73U};
    static const uint8_t render_payload[] = {0x74U};

    CHECK(initialize_pair(&pair, 45U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 23U));
    CHECK(activate_stream_endpoint(&pair, 5U, 6U, 24U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 3U
    ));
    wifi_work = stream_work(
        route_key(&pair, 2U), 23U, 1U,
        wifi_payload, sizeof(wifi_payload)
    );
    render_work = stream_work(
        route_key(&pair, 8U), 24U, 1U,
        render_payload, sizeof(render_payload)
    );
    memset(&fake_transfer, 0, sizeof(fake_transfer));
    fake_transfer.route = route_key(&pair, 7U);
    fake_transfer.transfer_id = 1U;
    fake_transfer.content_id = blob_content_id();
    fake_transfer.total_size = sizeof(BLOB_BYTES);
    blob_work = blob_chunk_work(&fake_transfer, 1U);
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &wifi_work,
        &wifi
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &render_work,
        &render
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_CHUNK,
        &blob_work,
        &blob
    ));
    CHECK(scheduler_seal_and_bind(&fixture));
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, wifi->source_id, &result
    ));
    CHECK(result.data_round == 1U);
    render->work.kind = MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT;
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REJECTED);
    CHECK(render->completions == 1U && blob->completions == 1U &&
          result.completion.source_id == blob->source_id &&
          result.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          result.data_round == 1U && fixture.log.count == 3U);
    CHECK(fixture.log.entries[1].source_id == render->source_id &&
          fixture.log.entries[1].runtime_status ==
              MEISO_RUNTIME_INVALID_ARGUMENT &&
          fixture.log.entries[2].source_id == blob->source_id);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.data_rounds == 1U &&
          snapshot.data_round_member_count == 0U &&
          snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY);
    return true;
}

static bool test_tx_busy_is_total_zero_progress(void)
{
    static RuntimeArena runtime_arena_before;
    static SessionArena session_arena_before;
    static SchedulerArena scheduler_arena_before;
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork first_work;
    MeisoRuntimeSchedulerWork second_work;
    MeisoRuntimeSchedulerWork data_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerStepResult before_result;
    MeisoRuntimeSchedulerSnapshot scheduler_before;
    MeisoRuntimeSchedulerSnapshot scheduler_after;
    MeisoRuntimeSnapshot runtime_before;
    MeisoRuntimeSnapshot runtime_after;
    MeisoSessionSnapshot session_before;
    MeisoSessionSnapshot session_after;
    MeisoByteView outbound;
    uint8_t frame_before[TEST_FRAME_BYTES];
    TestSource *first;
    TestSource *second;
    TestSource *data;
    TestSource first_before;
    TestSource second_before;
    TestSource data_before;
    static const uint8_t payload[] = {0x79U};

    CHECK(initialize_pair(&pair, 50U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 29U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 2U, 1U
    ));
    first_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    second_work = state_accept_work(
        route_key(&pair, 5U), HOST_OWNER, 1U
    );
    data_work = stream_work(
        route_key(&pair, 2U), 29U, 1U, payload, sizeof(payload)
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &first_work,
        &first
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &second_work,
        &second
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &data_work,
        &data
    ));
    CHECK(scheduler_seal_and_bind(&fixture));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(result.completion.source_id == first->source_id);
    CHECK(meiso_session_next_outbound(pair.edge_session, &outbound));
    CHECK(outbound.size <= sizeof(frame_before));
    memmove(frame_before, outbound.data, outbound.size);
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.scheduler, &scheduler_before
    ));
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &runtime_before));
    CHECK(meiso_session_snapshot(pair.edge_session, &session_before));
    runtime_arena_before = pair.edge_runtime_arena;
    session_arena_before = pair.edge_session_arena;
    scheduler_arena_before = fixture.arena;
    first_before = *first;
    second_before = *second;
    data_before = *data;
    memset(&result, 0xa5, sizeof(result));
    before_result = result;
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_TX_BUSY);
    CHECK(memcmp(&result, &before_result, sizeof(result)) == 0);
    CHECK(meiso_runtime_scheduler_snapshot(
        fixture.scheduler, &scheduler_after
    ));
    CHECK(meiso_runtime_snapshot(pair.edge_runtime, &runtime_after));
    CHECK(meiso_session_snapshot(pair.edge_session, &session_after));
    CHECK(memcmp(
        &scheduler_after, &scheduler_before, sizeof(scheduler_after)
    ) == 0);
    CHECK(memcmp(&runtime_after, &runtime_before, sizeof(runtime_after)) == 0);
    CHECK(memcmp(&session_after, &session_before, sizeof(session_after)) == 0);
    CHECK(memcmp(
        &pair.edge_runtime_arena,
        &runtime_arena_before,
        sizeof(pair.edge_runtime_arena)
    ) == 0);
    CHECK(memcmp(
        &pair.edge_session_arena,
        &session_arena_before,
        sizeof(pair.edge_session_arena)
    ) == 0);
    CHECK(memcmp(
        &fixture.arena,
        &scheduler_arena_before,
        sizeof(fixture.arena)
    ) == 0);
    CHECK(memcmp(first, &first_before, sizeof(*first)) == 0 &&
          memcmp(second, &second_before, sizeof(*second)) == 0 &&
          memcmp(data, &data_before, sizeof(*data)) == 0);
    CHECK(meiso_session_next_outbound(pair.edge_session, &outbound));
    CHECK(outbound.size <= sizeof(frame_before) &&
          memcmp(outbound.data, frame_before, outbound.size) == 0);
    CHECK(drain_outbound(pair.edge_session));
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, second->source_id, &result
    ));
    CHECK(data->polls == 0U);
    return true;
}

static bool test_invalid_control_heads_do_not_starve(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork invalid_poll_work;
    MeisoRuntimeSchedulerWork wrong_kind_work;
    MeisoRuntimeSchedulerWork wrong_blob_work;
    MeisoRuntimeSchedulerWork valid_work;
    MeisoRuntimeSchedulerWork data_work;
    MeisoRuntimeSchedulerStepResult result;
    TestSource *invalid_poll;
    TestSource *wrong_kind;
    TestSource *wrong_blob;
    TestSource *valid;
    TestSource *data;
    static const uint8_t payload[] = {0x81U};

    CHECK(initialize_pair(&pair, 60U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 31U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 4U, 1U
    ));
    invalid_poll_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    wrong_kind_work = stream_work(
        route_key(&pair, 2U), 31U, 1U, payload, sizeof(payload)
    );
    wrong_blob_work = blob_chunk_work(
        &(MeisoRuntimeBlobTransferRef){
            route_key(&pair, 7U), 1U, {0}, sizeof(BLOB_BYTES)
        },
        1U
    );
    valid_work = state_accept_work(
        route_key(&pair, 5U), HOST_OWNER, 1U
    );
    data_work = stream_work(
        route_key(&pair, 2U), 31U, 100U, payload, sizeof(payload)
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &invalid_poll_work,
        &invalid_poll
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &wrong_kind_work,
        &wrong_kind
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &wrong_blob_work,
        &wrong_blob
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &valid_work,
        &valid
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &data_work,
        &data
    ));
    invalid_poll->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_INVALID;
    wrong_kind->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    wrong_blob->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    valid->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    data->clear_after_complete = true;
    CHECK(meiso_runtime_scheduler_seal(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        invalid_poll->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        wrong_kind->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        wrong_blob->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        valid->source_id,
        route_key(&pair, 5U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        data->source_id,
        route_key(&pair, 2U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);

    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, data->source_id, &result
    ));
    CHECK(invalid_poll->polls == 1U && invalid_poll->completions == 0U);
    invalid_poll->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    wrong_kind->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    wrong_blob->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    valid->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, valid->source_id, &result
    ));
    CHECK(valid->completions == 1U && wrong_kind->completions == 1U &&
          wrong_blob->completions == 1U && fixture.log.count >= 4U);
    CHECK(fixture.log.entries[fixture.log.count - 3U].source_id ==
              wrong_kind->source_id &&
          fixture.log.entries[fixture.log.count - 3U].ticket ==
              wrong_kind_work.ticket &&
          fixture.log.entries[fixture.log.count - 3U].code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          fixture.log.entries[fixture.log.count - 3U].runtime_status ==
              MEISO_RUNTIME_INVALID_ARGUMENT &&
          fixture.log.entries[fixture.log.count - 2U].source_id ==
              wrong_blob->source_id &&
          fixture.log.entries[fixture.log.count - 2U].code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          fixture.log.entries[fixture.log.count - 2U].runtime_status ==
              MEISO_RUNTIME_INVALID_ARGUMENT &&
          fixture.log.entries[fixture.log.count - 1U].source_id ==
              valid->source_id);
    return true;
}

static bool test_runtime_rejected_control_forces_data_probe(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork rejected_work;
    MeisoRuntimeSchedulerWork data_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    TestSource *rejected;
    TestSource *data;
    static const uint8_t payload[] = {0x91U};

    CHECK(initialize_pair(&pair, 65U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(activate_stream_endpoint(&pair, 0U, 1U, 41U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 1U
    ));
    rejected_work = state_accept_work(
        route_key(&pair, 1U), EDGE_OWNER, 1U
    );
    data_work = stream_work(
        route_key(&pair, 2U), 41U, 1U, payload, sizeof(payload)
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &rejected_work,
        &rejected
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STREAM_SAMPLE,
        &data_work,
        &data
    ));
    data->clear_after_complete = true;
    CHECK(scheduler_seal_and_bind(&fixture));
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REJECTED);
    CHECK(result.completion.source_id == rejected->source_id &&
          result.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_REJECTED &&
          result.completion.runtime_status == MEISO_RUNTIME_WRONG_ROLE &&
          rejected->completions == 1U && data->polls == 0U);
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, data->source_id, &result
    ));
    CHECK(result.successful_slot == 1U && result.data_round == 1U);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.successful_control_slots == 0U &&
          snapshot.successful_data_slots == 1U &&
          rejected->completions == 1U);
    return true;
}

static bool test_callback_reentrancy_fails_closed(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    TestSource *source;

    CHECK(initialize_pair(&pair, 68U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 1U
    ));
    work = state_accept_work(route_key(&pair, 0U), HOST_OWNER, 1U);
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &work,
        &source
    ));
    source->reentrant_scheduler = fixture.scheduler;
    source->reenter_poll = true;
    source->reenter_complete = true;
    CHECK(scheduler_seal_and_bind(&fixture));
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, source->source_id, &result
    ));
    CHECK(source->polls == 1U && source->completions == 1U &&
          source->poll_reentrant_step ==
              MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT &&
          source->complete_reentrant_step ==
              MEISO_RUNTIME_SCHEDULER_STEP_REENTRANT &&
          !source->poll_reentrant_snapshot &&
          !source->complete_reentrant_snapshot);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.successful_slots == 1U &&
          snapshot.successful_control_slots == 1U &&
          snapshot.successful_data_slots == 0U);
    return true;
}

static bool test_rx_obligation_blocks_until_committed(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork control_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    TestSource *control;
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoByteView payload;
    MeisoRuntimeEvent event = {0};
    uint8_t actual_bytes[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE];

    CHECK(initialize_pair(&pair, 70U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.host_runtime, pair.host_session, 1U, 1U
    ));
    control_work = state_accept_work(
        route_key(&pair, 1U), EDGE_OWNER, 1U
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &control_work,
        &control
    ));
    CHECK(scheduler_seal_and_bind(&fixture));

    memset(&actual, 0, sizeof(actual));
    actual.apply_result = MEISO_RUNTIME_APPLY_NONE;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = false;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_STOPPED;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_DISABLED;
    CHECK(meiso_runtime_actual_state_encode(
        &actual,
        (MeisoMutableBuffer){actual_bytes, sizeof(actual_bytes)},
        &payload
    ));
    offer.generation.value = 1U;
    offer.state.owner = EDGE_OWNER;
    offer.state.version = 1U;
    offer.state.payload = payload;
    CHECK(meiso_runtime_send_state_offer(
        pair.edge_runtime, route_key(&pair, 1U), &offer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_scheduler(
        &pair,
        pair.edge_session,
        pair.host_session,
        fixture.scheduler,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_STATE_OFFER);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.rx_obligation ==
          MEISO_RUNTIME_SCHEDULER_RX_STATE_COMMIT);
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION);
    CHECK(control->polls == 0U && control->completions == 0U);
    CHECK(meiso_runtime_scheduler_commit_state_receive(
        fixture.scheduler,
        event.route,
        &event.state_offer,
        MEISO_STATE_RECEIVE_NEW
    ) == MEISO_RUNTIME_OK);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.rx_obligation == MEISO_RUNTIME_SCHEDULER_RX_NONE);
    CHECK(step_sent(
        fixture.scheduler, pair.host_session, control->source_id, &result
    ));
    return true;
}

static bool test_blob_publish_obligation_and_result(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    BlobArena blob_arena;
    MeisoBlobStore *store = NULL;
    const MeisoBlobConfig blob_config = {2U};
    MeisoRuntimeBlobTransferRef transfer;
    MeisoRuntimeSchedulerWork result_work;
    MeisoRuntimeSchedulerStepResult step_result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    MeisoRuntimeBlobPublishReport report;
    MeisoRuntimeEvent event = {0};
    TestSource *result_source;
    size_t required;

    CHECK(initialize_pair(&pair, 75U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(prepare_blob_transfer(&pair, &transfer));
    required = meiso_blob_store_memory_required(
        &pair.routes[7].resource, &blob_config
    );
    CHECK(required != 0U && required <= sizeof(blob_arena.bytes));
    CHECK(meiso_blob_store_init(
        blob_arena.bytes,
        required,
        &pair.routes[7].resource,
        &blob_config,
        &store
    ) == MEISO_BLOB_INIT_OK);
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 1U, 1U
    ));
    memset(&result_work, 0, sizeof(result_work));
    result_work.ticket = 1U;
    result_work.kind = MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT;
    result_work.value.blob_result.transfer = transfer;
    result_work.value.blob_result.operation = MEISO_RUNTIME_BLOB_COMMIT;
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT,
        &result_work,
        &result_source
    ));
    result_source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    result_source->clear_after_complete = true;
    CHECK(scheduler_seal_and_bind(&fixture));

    CHECK(meiso_runtime_send_blob_chunk(
        pair.host_runtime,
        &transfer,
        0U,
        (MeisoByteView){BLOB_BYTES, sizeof(BLOB_BYTES)}
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_scheduler(
        &pair,
        pair.host_session,
        pair.edge_session,
        fixture.scheduler,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_CHUNK &&
          event.blob_payload.size == sizeof(BLOB_BYTES));
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.rx_obligation == MEISO_RUNTIME_SCHEDULER_RX_NONE);

    CHECK(meiso_runtime_send_blob_commit(
        pair.host_runtime, &transfer
    ) == MEISO_RUNTIME_OK);
    CHECK(transfer_scheduler(
        &pair,
        pair.host_session,
        pair.edge_session,
        fixture.scheduler,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_REQUEST &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_payload.size == sizeof(BLOB_BYTES));
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.rx_obligation ==
          MEISO_RUNTIME_SCHEDULER_RX_BLOB_PUBLISH);
    memset(&step_result, 0xa5, sizeof(step_result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &step_result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_RX_OBLIGATION);
    CHECK(result_source->polls == 0U && result_source->completions == 0U);
    memset(&report, 0, sizeof(report));
    CHECK(meiso_runtime_scheduler_publish_blob_receive(
        fixture.scheduler,
        &event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_OK);
    CHECK(report.result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED &&
          report.stage_attempted && report.commit_attempted &&
          meiso_blob_store_count(store) == 1U);
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.rx_obligation == MEISO_RUNTIME_SCHEDULER_RX_NONE);
    CHECK(meiso_runtime_scheduler_publish_blob_receive(
        fixture.scheduler,
        &event.blob_transfer,
        store,
        meiso_runtime_blob_sha256_verify,
        NULL,
        &report
    ) == MEISO_RUNTIME_CONFLICT);

    result_source->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &step_result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(step_result.completion.source_id == result_source->source_id &&
          step_result.completion.kind ==
              MEISO_RUNTIME_SCHEDULER_WORK_BLOB_RESULT);
    CHECK(transfer_runtime(
        &pair,
        pair.edge_session,
        pair.host_session,
        pair.host_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_BLOB_RESULT &&
          event.blob_operation == MEISO_RUNTIME_BLOB_COMMIT &&
          event.blob_result == MEISO_RUNTIME_BLOB_RESULT_PUBLISHED);
    return true;
}

static bool test_catalog_rebind_and_terminal_notify_once(void)
{
    RuntimePair pair;
    SchedulerFixture fixture;
    MeisoRuntimeSchedulerWork catalog_work;
    MeisoRuntimeSchedulerWork state_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot snapshot;
    MeisoRuntimeEvent event = {0};
    TestSource *catalog;
    TestSource *state;
    size_t index;

    CHECK(initialize_pair(&pair, 80U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(scheduler_init_fixture(
        &fixture, pair.edge_runtime, pair.edge_session, 2U, 1U
    ));
    memset(&catalog_work, 0, sizeof(catalog_work));
    catalog_work.ticket = 1U;
    catalog_work.kind = MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER;
    catalog_work.value.catalog_offer.generation = 2U;
    catalog_work.value.catalog_offer.endpoints = pair.endpoints;
    catalog_work.value.catalog_offer.endpoint_count =
        MEISO_COMPOSITION_SESSION_V2_ENDPOINT_COUNT;
    catalog_work.value.catalog_offer.routes = pair.routes;
    catalog_work.value.catalog_offer.route_count =
        MEISO_COMPOSITION_SESSION_V2_ROUTE_COUNT;
    state_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_CATALOG_OFFER,
        &catalog_work,
        &catalog
    ));
    CHECK(scheduler_add_source(
        &fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &state_work,
        &state
    ));
    catalog->clear_after_complete = true;
    state->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_EMPTY;
    CHECK(scheduler_seal_and_bind(&fixture));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SENT);
    CHECK(result.completion.source_id == catalog->source_id &&
          result.completion.code ==
              MEISO_RUNTIME_SCHEDULER_COMPLETION_SENT);
    CHECK(transfer_runtime(
        &pair,
        pair.edge_session,
        pair.host_session,
        pair.host_runtime,
        &event,
        NULL
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_OFFERED &&
          event.catalog_generation == 2U);
    CHECK(meiso_runtime_accept_catalog(pair.host_runtime) ==
          MEISO_RUNTIME_OK);
    CHECK(transfer_scheduler(
        &pair,
        pair.host_session,
        pair.edge_session,
        fixture.scheduler,
        &event
    ));
    CHECK(event.type == MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED &&
          event.catalog_generation == 2U);
    pair.catalog_generation = 2U;
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED);
    for (index = 0U; index < fixture.source_count; ++index) {
        CHECK(fixture.sources[index].boundary_calls == 1U &&
              fixture.sources[index].last_boundary ==
                  MEISO_RUNTIME_SCHEDULER_BOUNDARY_CATALOG_CHANGED &&
              fixture.sources[index].last_boundary_catalog == 2U);
    }
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_REBIND_REQUIRED);
    for (index = 0U; index < fixture.source_count; ++index) {
        CHECK(fixture.sources[index].boundary_calls == 1U);
    }
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_REBIND_REQUIRED);
    state->work.value.state_accept.route.catalog_generation = 2U;
    CHECK(meiso_runtime_scheduler_bind_source_route(
        fixture.scheduler,
        state->source_id,
        route_key(&pair, 0U)
    ) == MEISO_RUNTIME_SCHEDULER_OK);
    CHECK(meiso_runtime_scheduler_finish_rebind(fixture.scheduler) ==
          MEISO_RUNTIME_SCHEDULER_OK);
    state->poll_status = MEISO_RUNTIME_SCHEDULER_POLL_READY;
    CHECK(step_sent(
        fixture.scheduler, pair.edge_session, state->source_id, &result
    ));
    CHECK(meiso_runtime_scheduler_snapshot(fixture.scheduler, &snapshot));
    CHECK(snapshot.catalog_generation == 2U &&
          snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY);

    meiso_session_transport_lost(pair.edge_session);
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    for (index = 0U; index < fixture.source_count; ++index) {
        CHECK(fixture.sources[index].boundary_calls == 2U &&
              fixture.sources[index].last_boundary ==
                  MEISO_RUNTIME_SCHEDULER_BOUNDARY_SESSION_TERMINAL &&
              fixture.sources[index].last_boundary_catalog == 0U);
    }
    CHECK(meiso_runtime_scheduler_step(fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    for (index = 0U; index < fixture.source_count; ++index) {
        CHECK(fixture.sources[index].boundary_calls == 2U);
    }
    return true;
}

static bool test_per_peer_independence_and_ticket_boundary(void)
{
    RuntimePair pair;
    SchedulerFixture host_fixture;
    SchedulerFixture edge_fixture;
    MeisoRuntimeSchedulerWork host_work;
    MeisoRuntimeSchedulerWork edge_work;
    MeisoRuntimeSchedulerStepResult result;
    MeisoRuntimeSchedulerSnapshot host_snapshot;
    MeisoRuntimeSchedulerSnapshot edge_snapshot;
    TestSource *host_source;
    TestSource *edge_source;

    CHECK(initialize_pair(&pair, 100U));
    CHECK(activate_catalog(&pair, 1U));
    CHECK(scheduler_init_fixture(
        &host_fixture, pair.host_runtime, pair.host_session, 1U, 1U
    ));
    CHECK(scheduler_init_fixture(
        &edge_fixture, pair.edge_runtime, pair.edge_session, 1U, 1U
    ));
    host_work = state_accept_work(
        route_key(&pair, 1U), EDGE_OWNER, UINT64_MAX
    );
    edge_work = state_accept_work(
        route_key(&pair, 0U), HOST_OWNER, 1U
    );
    CHECK(scheduler_add_source(
        &host_fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &host_work,
        &host_source
    ));
    CHECK(scheduler_add_source(
        &edge_fixture,
        MEISO_RUNTIME_SCHEDULER_WORK_STATE_ACCEPT,
        &edge_work,
        &edge_source
    ));
    CHECK(scheduler_seal_and_bind(&host_fixture));
    CHECK(scheduler_seal_and_bind(&edge_fixture));
    CHECK(step_sent(
        host_fixture.scheduler,
        pair.host_session,
        host_source->source_id,
        &result
    ));
    CHECK(host_source->poll_status == MEISO_RUNTIME_SCHEDULER_POLL_EMPTY);
    CHECK(meiso_runtime_scheduler_snapshot(
        host_fixture.scheduler, &host_snapshot
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        edge_fixture.scheduler, &edge_snapshot
    ));
    CHECK(host_snapshot.successful_slots == 1U &&
          edge_snapshot.successful_slots == 0U);
    CHECK(step_sent(
        edge_fixture.scheduler,
        pair.edge_session,
        edge_source->source_id,
        &result
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        host_fixture.scheduler, &host_snapshot
    ));
    CHECK(meiso_runtime_scheduler_snapshot(
        edge_fixture.scheduler, &edge_snapshot
    ));
    CHECK(host_snapshot.successful_slots == 1U &&
          edge_snapshot.successful_slots == 1U &&
          host_snapshot.successful_control_slots == 1U &&
          edge_snapshot.successful_control_slots == 1U);
    meiso_session_transport_lost(pair.host_session);
    CHECK(meiso_runtime_scheduler_step(host_fixture.scheduler, &result) ==
          MEISO_RUNTIME_SCHEDULER_STEP_SESSION_TERMINAL);
    CHECK(meiso_runtime_scheduler_snapshot(
        edge_fixture.scheduler, &edge_snapshot
    ));
    CHECK(edge_snapshot.phase == MEISO_RUNTIME_SCHEDULER_PHASE_READY &&
          edge_source->boundary_calls == 0U);
    return true;
}

int main(void)
{
    if (!test_composite_profile_catalog_builder() ||
        !test_builder_failure_no_partial() ||
        !test_scheduler_arena_registration_and_binding() ||
        !test_host_control_flood_blob_bound() ||
        !test_edge_fairness_rr_and_two_streams() ||
        !test_data_round_membership_and_cursor_wrap() ||
        !test_invalid_data_member_preserves_round_peers() ||
        !test_tx_busy_is_total_zero_progress() ||
        !test_invalid_control_heads_do_not_starve() ||
        !test_runtime_rejected_control_forces_data_probe() ||
        !test_callback_reentrancy_fails_closed() ||
        !test_rx_obligation_blocks_until_committed() ||
        !test_blob_publish_obligation_and_result() ||
        !test_catalog_rebind_and_terminal_notify_once() ||
        !test_per_peer_independence_and_ticket_boundary()) {
        return 1;
    }
    (void)printf("composition TX scheduler tests passed\n");
    return 0;
}
