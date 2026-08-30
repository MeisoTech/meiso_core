#include <meiso/core/action.h>

#include "meiso_test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

_Static_assert(sizeof(MeisoActionRetryPolicy) == sizeof(uint8_t), "fixed policy");
_Static_assert(sizeof(MeisoActionState) == sizeof(uint8_t), "fixed state");
_Static_assert(sizeof(MeisoActionIngestStatus) == sizeof(uint8_t), "fixed status");

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                    \
                    __FILE__, __LINE__, #condition);                            \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

typedef union {
    max_align_t alignment;
    uint8_t bytes[32768];
} TestArena;

static MeisoResourceDescriptor action_descriptor(size_t payload_capacity)
{
    MeisoResourceDescriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = (MeisoResourceId){1001U, 1002U};
    descriptor.kind = MEISO_RESOURCE_KIND_ACTION;
    descriptor.schema.id = (MeisoSchemaId){1003U, 1004U};
    descriptor.schema.version = 1U;
    descriptor.owner = MEISO_OWNER_ROLE_EDGE;
    descriptor.max_payload_size = payload_capacity;
    return descriptor;
}

static MeisoActionConfig action_config(
    uint32_t slots,
    size_t arguments,
    size_t results
)
{
    MeisoActionConfig config;

    config.slot_count = slots;
    config.max_argument_size = arguments;
    config.max_result_size = results;
    return config;
}

static MeisoActionRequest action_request(
    uint64_t number,
    MeisoOwnerScope executor,
    const uint8_t *arguments,
    size_t argument_size
)
{
    MeisoActionRequest request;

    memset(&request, 0, sizeof(request));
    request.id = (MeisoActionId){2000U, number};
    request.expected_executor = executor;
    request.target = (MeisoResourceId){3000U, 3001U};
    request.operation.id = (MeisoSchemaId){4000U, 4001U};
    request.operation.version = 2U;
    request.arguments = (MeisoByteView){arguments, argument_size};
    request.retry_policy = MEISO_ACTION_RETRY_NONE;
    return request;
}

static MeisoActionResult action_result(
    MeisoActionId id,
    MeisoOwnerScope executor,
    MeisoActionState state,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoActionResult result;

    result.id = id;
    result.executor = executor;
    result.state = state;
    result.payload = (MeisoByteView){payload, payload_size};
    return result;
}

static MeisoActionCaller *make_caller(
    TestArena *arena,
    MeisoActionConfig config
)
{
    MeisoResourceDescriptor descriptor = action_descriptor(64U);
    MeisoActionCaller *caller = NULL;
    const size_t required = meiso_action_caller_memory_required(
        &descriptor, &config
    );

    CHECK(required > 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_action_caller_init(
        arena->bytes, sizeof(arena->bytes), &descriptor, &config, &caller
    ) == MEISO_ACTION_INIT_OK);
    return caller;
}

static MeisoActionExecutor *make_executor(
    TestArena *arena,
    MeisoActionConfig config,
    MeisoOwnerScope scope
)
{
    MeisoResourceDescriptor descriptor = action_descriptor(64U);
    MeisoActionExecutor *executor = NULL;
    const size_t required = meiso_action_executor_memory_required(
        &descriptor, &config
    );

    CHECK(required > 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_action_executor_init(
        arena->bytes, sizeof(arena->bytes), &descriptor, &config, scope, &executor
    ) == MEISO_ACTION_INIT_OK);
    return executor;
}

static bool bytes_are(MeisoByteView view, const uint8_t *data, size_t size)
{
    return view.size == size &&
        (size == 0U || memcmp(view.data, data, size) == 0);
}

static void test_init_contract(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MEISO_TEST_ARENA_STORAGE TestArena before;
    MeisoResourceDescriptor descriptor = action_descriptor(16U);
    MeisoActionConfig config = action_config(2U, 8U, 8U);
    MeisoActionCaller *caller = (MeisoActionCaller *)(uintptr_t)1U;
    MeisoActionExecutor *executor = (MeisoActionExecutor *)(uintptr_t)1U;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 7U};
    size_t required = meiso_action_caller_memory_required(&descriptor, &config);

    CHECK(meiso_action_alignment_required() >= _Alignof(max_align_t));
    CHECK(required > 0U);
    CHECK(meiso_action_caller_init(
        arena.bytes + 1U, sizeof(arena.bytes) - 1U,
        &descriptor, &config, &caller
    ) == MEISO_ACTION_INIT_MISALIGNED_ARENA);
    CHECK(caller == NULL);
    CHECK(meiso_action_caller_init(
        arena.bytes, required - 1U, &descriptor, &config, &caller
    ) == MEISO_ACTION_INIT_ARENA_TOO_SMALL);
    CHECK(caller == NULL);

    memset(&arena, 0x5a, sizeof(arena));
    before = arena;
    CHECK(meiso_action_caller_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoActionCaller **)arena.bytes
    ) == MEISO_ACTION_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);

    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_action_caller_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoActionCaller **)arena.bytes
    ) == MEISO_ACTION_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    CHECK(meiso_action_executor_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config, scope,
        (MeisoActionExecutor **)arena.bytes
    ) == MEISO_ACTION_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    descriptor = action_descriptor(16U);

    config.slot_count = 0U;
    CHECK(meiso_action_caller_memory_required(&descriptor, &config) == 0U);
    CHECK(meiso_action_caller_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config, &caller
    ) == MEISO_ACTION_INIT_INVALID_CONFIG);
    config = action_config(1U, 17U, 0U);
    CHECK(meiso_action_executor_memory_required(&descriptor, &config) == 0U);
    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;
    config = action_config(1U, 1U, 1U);
    CHECK(meiso_action_caller_memory_required(&descriptor, &config) == 0U);
    CHECK(meiso_action_caller_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config, &caller
    ) == MEISO_ACTION_INIT_INVALID_DESCRIPTOR);

    descriptor = action_descriptor(16U);
    CHECK(meiso_action_executor_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 1U}, &executor
    ) == MEISO_ACTION_INIT_INVALID_EXECUTOR);
    CHECK(executor == NULL);

    descriptor = action_descriptor(SIZE_MAX);
    config = action_config(UINT32_MAX, SIZE_MAX, SIZE_MAX);
    CHECK(meiso_action_caller_memory_required(&descriptor, &config) == 0U);
    CHECK(meiso_action_executor_memory_required(&descriptor, &config) == 0U);

    descriptor = action_descriptor(16U);
    config = action_config(1U, 4U, 4U);
    memset(&arena, 0, sizeof(arena));
    memcpy(arena.bytes + 128U, &descriptor, sizeof(descriptor));
    memcpy(arena.bytes + 256U, &config, sizeof(config));
    CHECK(meiso_action_executor_init(
        arena.bytes, sizeof(arena.bytes),
        (const MeisoResourceDescriptor *)(const void *)(arena.bytes + 128U),
        (const MeisoActionConfig *)(const void *)(arena.bytes + 256U),
        scope, &executor
    ) == MEISO_ACTION_INIT_OK);
    CHECK(executor != NULL);
}

static void test_single_execution_and_orthogonal_ack(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 11U};
    const MeisoDeliveryGeneration delivery = {5U};
    const uint8_t arguments[] = {1U, 2U, 3U};
    const uint8_t response[] = {8U, 9U};
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(4U, 8U, 8U)
    );
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(4U, 8U, 8U), scope
    );
    MeisoActionRequest request = action_request(
        1U, scope, arguments, sizeof(arguments)
    );
    MeisoActionResult result = action_result(
        request.id, scope, MEISO_ACTION_STATE_SUCCEEDED,
        response, sizeof(response)
    );
    MeisoActionResult accepted = action_result(
        request.id, scope, MEISO_ACTION_STATE_ACCEPTED, NULL, 0U
    );
    MeisoActionEntryView view;
    MeisoActionOffer offer;
    MeisoActionRequest application_request;
    size_t cursor = 0U;
    size_t second_cursor = 0U;

    CHECK(meiso_action_caller_submit(caller, &request) ==
          MEISO_ACTION_SUBMIT_NO_ACTIVE_DELIVERY);
    CHECK(meiso_action_caller_begin_delivery(caller, delivery) ==
          MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &request) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_next_offer(
        caller, delivery, &cursor, &offer
    ));
    CHECK(meiso_action_id_equal(offer.request.id, request.id));
    CHECK(bytes_are(offer.request.arguments, arguments, sizeof(arguments)));

    CHECK(meiso_action_executor_ingest(executor, &offer.request, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    cursor = 0U;
    CHECK(meiso_action_executor_next_application_request(
        executor, &cursor, &application_request
    ));
    CHECK(meiso_action_executor_next_application_request(
        executor, &second_cursor, &application_request
    ));
    CHECK(meiso_action_id_equal(application_request.id, request.id));
    CHECK(meiso_action_executor_accept(
        executor, scope, request.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPTED);
    CHECK(meiso_action_executor_accept(
        executor, scope, request.id, (MeisoTimePoint){0U, 0U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPT_DUPLICATE);

    CHECK(meiso_action_caller_observe_result(caller, &accepted) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_observe_result(caller, &accepted) ==
          MEISO_ACTION_RESULT_DUPLICATE);
    CHECK(meiso_action_caller_query(caller, request.id, &view));
    CHECK(view.delivery_accepted == 0U);
    CHECK(view.state == MEISO_ACTION_STATE_ACCEPTED);

    CHECK(meiso_action_caller_mark_delivery_accepted(
        caller, delivery, request.id, scope
    ) == MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED);
    CHECK(meiso_action_caller_query(caller, request.id, &view));
    CHECK(view.delivery_accepted == 1U);
    CHECK(view.state == MEISO_ACTION_STATE_ACCEPTED);
    CHECK(view.result.size == 0U);

    CHECK(meiso_action_executor_record_result(executor, &result) ==
          MEISO_ACTION_RECORD_RECORDED);
    CHECK(meiso_action_executor_query(executor, request.id, &view));
    CHECK(view.state == MEISO_ACTION_STATE_SUCCEEDED);
    CHECK(bytes_are(view.result, response, sizeof(response)));
    CHECK(meiso_action_caller_observe_result(caller, &result) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_query(caller, request.id, &view));
    CHECK(view.delivery_accepted == 1U);
    CHECK(view.state == MEISO_ACTION_STATE_SUCCEEDED);

    CHECK(meiso_action_executor_ingest(executor, &request, NULL) ==
          MEISO_ACTION_INGEST_REPLAY);
    cursor = 0U;
    CHECK(!meiso_action_executor_next_application_request(
        executor, &cursor, &application_request
    ));
    CHECK(meiso_action_executor_record_result(executor, &result) ==
          MEISO_ACTION_RECORD_DUPLICATE);
    CHECK(meiso_action_caller_observe_result(caller, &result) ==
          MEISO_ACTION_RESULT_DUPLICATE);
    CHECK(meiso_action_caller_observe_result(caller, &accepted) ==
          MEISO_ACTION_RESULT_TERMINAL_ALREADY_SET);
}

static void test_identity_conflict_scope_and_mapped_deadline(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 21U};
    const uint8_t arguments[] = {4U, 5U};
    const uint8_t changed[] = {4U, 6U};
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(4U, 4U, 4U)
    );
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(4U, 4U, 4U), scope
    );
    MeisoActionRequest request = action_request(
        10U, scope, arguments, sizeof(arguments)
    );
    MeisoActionRequest candidate;
    MeisoActionAdmission admission = {
        1U, 1U, {7U, 100U}, {7U, 10U}
    };

    request.sender_deadline.present = true;
    request.sender_deadline.value = (MeisoTimePoint){90U, 1000U};
    CHECK(meiso_action_caller_begin_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &request) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_submit(caller, &request) ==
          MEISO_ACTION_SUBMIT_DUPLICATE);

    candidate = request;
    candidate.arguments = (MeisoByteView){changed, sizeof(changed)};
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);
    candidate = request;
    candidate.target.low += 1U;
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);
    candidate = request;
    candidate.operation.version += 1U;
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);
    candidate = request;
    candidate.sender_deadline.value.monotonic_ns += 1U;
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);
    candidate = request;
    candidate.retry_policy = MEISO_ACTION_RETRY_IDEMPOTENT;
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);
    candidate = request;
    candidate.expected_executor.incarnation += 1U;
    CHECK(meiso_action_caller_submit(caller, &candidate) ==
          MEISO_ACTION_SUBMIT_CONFLICT);

    CHECK(meiso_action_executor_ingest(executor, &request, &admission) ==
          MEISO_ACTION_INGEST_NEW);
    admission.comparable = 2U;
    CHECK(meiso_action_executor_ingest(executor, &request, &admission) ==
          MEISO_ACTION_INGEST_REPLAY);
    admission.comparable = 1U;
    admission.local_deadline = (MeisoTimePoint){8U, 1U};
    admission.local_now = (MeisoTimePoint){8U, 999U};
    CHECK(meiso_action_executor_ingest(executor, &request, &admission) ==
          MEISO_ACTION_INGEST_REPLAY);
    candidate = request;
    candidate.arguments = (MeisoByteView){changed, sizeof(changed)};
    CHECK(meiso_action_executor_ingest(executor, &candidate, &admission) ==
          MEISO_ACTION_INGEST_CONFLICT);
    candidate = request;
    candidate.expected_executor.incarnation -= 1U;
    CHECK(meiso_action_executor_ingest(executor, &candidate, &admission) ==
          MEISO_ACTION_INGEST_SCOPE_MISMATCH);
}

static void test_deadline_expiry_is_retained(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 31U};
    const uint8_t value[] = {1U};
    MeisoActionExecutor *executor = make_executor(
        &arena, action_config(4U, 2U, 2U), scope
    );
    MeisoActionRequest expired = action_request(1U, scope, value, sizeof(value));
    MeisoActionRequest retry = action_request(2U, scope, value, sizeof(value));
    MeisoActionRequest no_deadline = action_request(
        3U, scope, value, sizeof(value)
    );
    MeisoActionRequest delayed = action_request(4U, scope, value, sizeof(value));
    MeisoActionAdmission admission = {
        1U, 1U, {5U, 50U}, {5U, 51U}
    };
    MeisoActionEntryView view;
    MeisoActionRequest application_request;
    size_t cursor = 0U;

    expired.sender_deadline.present = true;
    expired.sender_deadline.value = (MeisoTimePoint){99U, 100U};
    CHECK(meiso_action_executor_ingest(executor, &expired, &admission) ==
          MEISO_ACTION_INGEST_EXPIRED);
    CHECK(meiso_action_executor_query(executor, expired.id, &view));
    CHECK(view.state == MEISO_ACTION_STATE_EXPIRED);
    admission.local_deadline.monotonic_ns = 500U;
    admission.local_now.monotonic_ns = 1U;
    CHECK(meiso_action_executor_ingest(executor, &expired, &admission) ==
          MEISO_ACTION_INGEST_REPLAY);
    CHECK(!meiso_action_executor_next_application_request(
        executor, &cursor, &application_request
    ));

    retry.sender_deadline = expired.sender_deadline;
    admission.comparable = 0U;
    CHECK(meiso_action_executor_ingest(executor, &retry, &admission) ==
          MEISO_ACTION_INGEST_CLOCK_SCOPE_MISMATCH);
    CHECK(!meiso_action_executor_query(executor, retry.id, &view));
    admission.comparable = 1U;
    admission.local_now.clock_epoch = 6U;
    CHECK(meiso_action_executor_ingest(executor, &retry, &admission) ==
          MEISO_ACTION_INGEST_CLOCK_SCOPE_MISMATCH);
    admission.local_now = (MeisoTimePoint){5U, 1U};
    CHECK(meiso_action_executor_ingest(executor, &retry, &admission) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_accept(
        executor, scope, retry.id, (MeisoTimePoint){5U, 500U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPTED);

    delayed.sender_deadline = expired.sender_deadline;
    admission.local_deadline = (MeisoTimePoint){5U, 1000U};
    admission.local_now = (MeisoTimePoint){5U, 10U};
    CHECK(meiso_action_executor_ingest(executor, &delayed, &admission) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_accept(
        executor, scope, delayed.id, (MeisoTimePoint){6U, 11U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPT_CLOCK_SCOPE_MISMATCH);
    CHECK(meiso_action_executor_accept(
        executor, scope, delayed.id, (MeisoTimePoint){5U, 1001U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPT_EXPIRED);
    CHECK(meiso_action_executor_query(executor, delayed.id, &view));
    CHECK(view.state == MEISO_ACTION_STATE_EXPIRED);

    admission.comparable = 0U;
    admission.uncertainty_acceptable = 0U;
    CHECK(meiso_action_executor_ingest(
        executor, &no_deadline, &admission
    ) == MEISO_ACTION_INGEST_NEW);
}

static void test_capacity_no_eviction_and_replay_when_full(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 41U};
    const uint8_t one[] = {1U};
    const uint8_t too_large[] = {1U, 2U, 3U};
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(1U, 2U, 2U)
    );
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(1U, 2U, 2U), scope
    );
    MeisoActionRequest first = action_request(1U, scope, one, sizeof(one));
    MeisoActionRequest second = action_request(2U, scope, one, sizeof(one));
    MeisoActionRequest oversized = action_request(
        3U, scope, too_large, sizeof(too_large)
    );
    MeisoActionEntryView view;

    CHECK(meiso_action_caller_begin_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &oversized) ==
          MEISO_ACTION_SUBMIT_ARGUMENT_CAPACITY);
    CHECK(meiso_action_caller_submit(caller, &first) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_submit(caller, &second) ==
          MEISO_ACTION_SUBMIT_TABLE_FULL);
    CHECK(meiso_action_caller_submit(caller, &first) ==
          MEISO_ACTION_SUBMIT_DUPLICATE);
    CHECK(meiso_action_caller_query(caller, first.id, &view));
    CHECK(!meiso_action_caller_query(caller, second.id, &view));

    CHECK(meiso_action_executor_ingest(executor, &oversized, NULL) ==
          MEISO_ACTION_INGEST_ARGUMENT_CAPACITY);
    CHECK(meiso_action_executor_ingest(executor, &first, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_ingest(executor, &second, NULL) ==
          MEISO_ACTION_INGEST_TABLE_FULL);
    CHECK(meiso_action_executor_ingest(executor, &first, NULL) ==
          MEISO_ACTION_INGEST_REPLAY);
    CHECK(meiso_action_executor_query(executor, first.id, &view));
    CHECK(!meiso_action_executor_query(executor, second.id, &view));
}

static void test_terminal_transitions_and_indeterminate(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 51U};
    const uint8_t value[] = {7U};
    const uint8_t other[] = {8U};
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(5U, 2U, 2U), scope
    );
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(2U, 2U, 2U)
    );
    MeisoActionRequest success = action_request(1U, scope, value, sizeof(value));
    MeisoActionRequest rejected = action_request(2U, scope, value, sizeof(value));
    MeisoActionRequest uncertain = action_request(3U, scope, value, sizeof(value));
    MeisoActionResult succeeded = action_result(
        success.id, scope, MEISO_ACTION_STATE_SUCCEEDED, value, sizeof(value)
    );
    MeisoActionResult failed = action_result(
        success.id, scope, MEISO_ACTION_STATE_FAILED, other, sizeof(other)
    );
    MeisoActionResult rejection = action_result(
        rejected.id, scope, MEISO_ACTION_STATE_REJECTED, NULL, 0U
    );
    MeisoActionResult late_rejection = action_result(
        success.id, scope, MEISO_ACTION_STATE_REJECTED, NULL, 0U
    );
    MeisoActionResult late_expiry = action_result(
        success.id, scope, MEISO_ACTION_STATE_EXPIRED, NULL, 0U
    );
    MeisoActionEntryView view;

    CHECK(meiso_action_executor_ingest(executor, &success, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_record_result(executor, &succeeded) ==
          MEISO_ACTION_RECORD_NOT_ACCEPTED);
    CHECK(meiso_action_executor_accept(
        executor, (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 52U}, success.id,
        (MeisoTimePoint){0U, 0U}
    ) == MEISO_ACTION_EXECUTOR_ACCEPT_SCOPE_MISMATCH);
    CHECK(meiso_action_executor_accept(
        executor, scope, success.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPTED);
    CHECK(meiso_action_executor_record_result(executor, &succeeded) ==
          MEISO_ACTION_RECORD_RECORDED);
    CHECK(meiso_action_executor_record_result(executor, &failed) ==
          MEISO_ACTION_RECORD_TERMINAL_ALREADY_SET);
    CHECK(meiso_action_executor_accept(
        executor, scope, success.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPT_TERMINAL_ALREADY_SET);

    CHECK(meiso_action_executor_ingest(executor, &rejected, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_record_result(executor, &rejection) ==
          MEISO_ACTION_RECORD_RECORDED);
    CHECK(meiso_action_executor_accept(
        executor, scope, rejected.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPT_TERMINAL_ALREADY_SET);

    CHECK(meiso_action_executor_ingest(executor, &uncertain, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_mark_indeterminate(
        executor, scope, uncertain.id
    ) ==
          MEISO_ACTION_INDETERMINATE_NOT_ACCEPTED);
    CHECK(meiso_action_executor_accept(
        executor, scope, uncertain.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPTED);
    CHECK(meiso_action_executor_mark_indeterminate(
        executor, scope, uncertain.id
    ) ==
          MEISO_ACTION_INDETERMINATE_MARKED);
    CHECK(meiso_action_executor_mark_indeterminate(
        executor, scope, uncertain.id
    ) ==
          MEISO_ACTION_INDETERMINATE_DUPLICATE);
    failed.id = uncertain.id;
    CHECK(meiso_action_executor_record_result(executor, &failed) ==
          MEISO_ACTION_RECORD_TERMINAL_ALREADY_SET);

    CHECK(meiso_action_caller_begin_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &success) == MEISO_ACTION_SUBMIT_NEW);
    {
        MeisoActionResult accepted = action_result(
            success.id, scope, MEISO_ACTION_STATE_ACCEPTED, NULL, 0U
        );
        CHECK(meiso_action_caller_observe_result(caller, &accepted) ==
              MEISO_ACTION_RESULT_RECORDED);
        CHECK(meiso_action_caller_observe_result(caller, &late_rejection) ==
              MEISO_ACTION_RESULT_WRONG_STATE);
        CHECK(meiso_action_caller_observe_result(caller, &late_expiry) ==
              MEISO_ACTION_RESULT_WRONG_STATE);
        CHECK(meiso_action_caller_query(caller, success.id, &view));
        CHECK(view.state == MEISO_ACTION_STATE_ACCEPTED);
    }
    CHECK(meiso_action_caller_mark_indeterminate(
        caller, success.id, (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 52U}
    ) == MEISO_ACTION_INDETERMINATE_SCOPE_MISMATCH);
    CHECK(meiso_action_caller_mark_indeterminate(caller, success.id, scope) ==
          MEISO_ACTION_INDETERMINATE_MARKED);
    CHECK(meiso_action_caller_query(caller, success.id, &view));
    CHECK(view.state == MEISO_ACTION_STATE_INDETERMINATE);
    CHECK(meiso_action_caller_observe_result(caller, &succeeded) ==
          MEISO_ACTION_RESULT_TERMINAL_ALREADY_SET);
}

static void test_generation_cursor_and_late_ack(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 61U};
    const uint8_t value[] = {1U};
    const MeisoDeliveryGeneration first_generation = {1U};
    const MeisoDeliveryGeneration second_generation = {2U};
    MeisoActionCaller *caller = make_caller(
        &arena, action_config(3U, 1U, 1U)
    );
    MeisoActionRequest first = action_request(1U, scope, value, sizeof(value));
    MeisoActionRequest second = action_request(2U, scope, value, sizeof(value));
    MeisoActionRequest third = action_request(3U, scope, value, sizeof(value));
    MeisoActionOffer offer_a;
    MeisoActionOffer offer_b;
    MeisoActionResult accepted_first = action_result(
        first.id, scope, MEISO_ACTION_STATE_ACCEPTED, NULL, 0U
    );
    MeisoActionResult accepted_second = action_result(
        second.id, scope, MEISO_ACTION_STATE_ACCEPTED, NULL, 0U
    );
    MeisoActionResult succeeded_first = action_result(
        first.id, scope, MEISO_ACTION_STATE_SUCCEEDED, NULL, 0U
    );
    MeisoActionResult succeeded_third = action_result(
        third.id, scope, MEISO_ACTION_STATE_SUCCEEDED, NULL, 0U
    );
    size_t cursor_a = 0U;
    size_t cursor_b = 0U;

    CHECK(meiso_action_caller_begin_delivery(caller, first_generation) ==
          MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &first) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_submit(caller, &second) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_next_offer(
        caller, first_generation, &cursor_a, &offer_a
    ));
    CHECK(meiso_action_caller_next_offer(
        caller, first_generation, &cursor_b, &offer_b
    ));
    CHECK(meiso_action_id_equal(offer_a.request.id, first.id));
    CHECK(meiso_action_id_equal(offer_b.request.id, first.id));
    CHECK(meiso_action_caller_next_offer(
        caller, first_generation, &cursor_a, &offer_a
    ));
    CHECK(meiso_action_id_equal(offer_a.request.id, second.id));
    CHECK(meiso_action_caller_observe_result(caller, &accepted_first) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_observe_result(caller, &accepted_second) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_end_delivery(caller, first_generation) ==
          MEISO_ACTION_DELIVERY_ENDED_WITH_UNRESOLVED);
    CHECK(meiso_action_caller_mark_delivery_accepted(
        caller, first_generation, second.id,
        (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 62U}
    ) == MEISO_ACTION_DELIVERY_ACCEPT_SCOPE_MISMATCH);
    CHECK(meiso_action_caller_mark_delivery_accepted(
        caller, first_generation, second.id, scope
    ) == MEISO_ACTION_DELIVERY_ACCEPT_CONFIRMED);
    CHECK(meiso_action_caller_observe_result(caller, &succeeded_first) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_begin_delivery(caller, first_generation) ==
          MEISO_ACTION_DELIVERY_STALE_GENERATION);
    CHECK(meiso_action_caller_begin_delivery(caller, second_generation) ==
          MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &first) ==
          MEISO_ACTION_SUBMIT_DUPLICATE);
    CHECK(meiso_action_caller_submit(caller, &third) == MEISO_ACTION_SUBMIT_NEW);
    cursor_a = 0U;
    CHECK(meiso_action_caller_next_offer(
        caller, second_generation, &cursor_a, &offer_a
    ));
    CHECK(meiso_action_id_equal(offer_a.request.id, third.id));
    CHECK(!meiso_action_caller_next_offer(
        caller, second_generation, &cursor_a, &offer_a
    ));
    CHECK(meiso_action_caller_observe_result(caller, &succeeded_third) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_end_delivery(caller, second_generation) ==
          MEISO_ACTION_DELIVERY_ENDED);
}

static void test_alias_zero_length_and_failure_rollback(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    MEISO_TEST_ARENA_STORAGE TestArena zero_caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena zero_executor_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 71U};
    const uint8_t bytes[] = {1U, 2U, 3U, 4U};
    const uint8_t oversized[] = {9U, 9U, 9U, 9U, 9U};
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(3U, 4U, 4U)
    );
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(3U, 4U, 4U), scope
    );
    MeisoActionRequest first = action_request(1U, scope, bytes, sizeof(bytes));
    MeisoActionRequest aliased;
    MeisoActionEntryView first_view;
    MeisoActionEntryView second_view;
    MeisoActionResult result;

    CHECK(meiso_action_caller_begin_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &first) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_query(caller, first.id, &first_view));
    aliased = first_view.request;
    aliased.id.low = 2U;
    CHECK(meiso_action_caller_submit(caller, &aliased) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_query(caller, aliased.id, &second_view));
    CHECK(bytes_are(second_view.request.arguments, bytes, sizeof(bytes)));

    result = action_result(
        first.id, scope, MEISO_ACTION_STATE_SUCCEEDED,
        first_view.request.arguments.data, first_view.request.arguments.size
    );
    CHECK(meiso_action_caller_observe_result(caller, &result) ==
          MEISO_ACTION_RESULT_RECORDED);
    CHECK(meiso_action_caller_query(caller, first.id, &first_view));
    CHECK(bytes_are(first_view.result, bytes, sizeof(bytes)));

    CHECK(meiso_action_executor_ingest(executor, &first_view.request, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_query(executor, first.id, &first_view));
    aliased = first_view.request;
    aliased.id.low = 3U;
    CHECK(meiso_action_executor_ingest(executor, &aliased, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_accept(
        executor, scope, first.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPTED);
    result = action_result(
        first.id, scope, MEISO_ACTION_STATE_SUCCEEDED,
        first_view.request.arguments.data, first_view.request.arguments.size
    );
    CHECK(meiso_action_executor_record_result(executor, &result) ==
          MEISO_ACTION_RECORD_RECORDED);
    CHECK(meiso_action_executor_query(executor, first.id, &first_view));
    CHECK(bytes_are(first_view.result, bytes, sizeof(bytes)));

    result = action_result(
        aliased.id, scope, MEISO_ACTION_STATE_SUCCEEDED,
        oversized, sizeof(oversized)
    );
    CHECK(meiso_action_executor_record_result(executor, &result) ==
          MEISO_ACTION_RECORD_NOT_ACCEPTED);
    CHECK(meiso_action_executor_accept(
        executor, scope, aliased.id, (MeisoTimePoint){0U, 0U}
    ) ==
          MEISO_ACTION_EXECUTOR_ACCEPTED);
    CHECK(meiso_action_executor_record_result(executor, &result) ==
          MEISO_ACTION_RECORD_CAPACITY);
    CHECK(meiso_action_executor_query(executor, aliased.id, &second_view));
    CHECK(second_view.state == MEISO_ACTION_STATE_ACCEPTED);
    CHECK(second_view.result.size == 0U);

    {
        MeisoActionCaller *zero_caller = make_caller(
            &zero_caller_arena, action_config(1U, 0U, 0U)
        );
        MeisoActionExecutor *zero_executor = make_executor(
            &zero_executor_arena, action_config(1U, 0U, 0U), scope
        );
        MeisoActionRequest empty = action_request(9U, scope, NULL, 0U);
        MeisoActionResult empty_result = action_result(
            empty.id, scope, MEISO_ACTION_STATE_SUCCEEDED, NULL, 0U
        );

        CHECK(meiso_action_caller_begin_delivery(
            zero_caller, (MeisoDeliveryGeneration){1U}
        ) == MEISO_ACTION_DELIVERY_STARTED);
        CHECK(meiso_action_caller_submit(zero_caller, &empty) ==
              MEISO_ACTION_SUBMIT_NEW);
        CHECK(meiso_action_executor_ingest(zero_executor, &empty, NULL) ==
              MEISO_ACTION_INGEST_NEW);
        CHECK(meiso_action_executor_accept(
            zero_executor, scope, empty.id, (MeisoTimePoint){0U, 0U}
        ) ==
              MEISO_ACTION_EXECUTOR_ACCEPTED);
        CHECK(meiso_action_executor_record_result(
            zero_executor, &empty_result
        ) == MEISO_ACTION_RECORD_RECORDED);
        CHECK(meiso_action_caller_observe_result(
            zero_caller, &empty_result
        ) == MEISO_ACTION_RESULT_RECORDED);
    }
}

static void test_stats(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena caller_arena;
    MEISO_TEST_ARENA_STORAGE TestArena executor_arena;
    const MeisoOwnerScope scope = {MEISO_OWNER_ROLE_EDGE, 81U};
    const uint8_t value[] = {1U};
    MeisoActionCaller *caller = make_caller(
        &caller_arena, action_config(1U, 1U, 1U)
    );
    MeisoActionExecutor *executor = make_executor(
        &executor_arena, action_config(1U, 1U, 1U), scope
    );
    MeisoActionRequest request = action_request(1U, scope, value, sizeof(value));
    MeisoActionCallerStats caller_stats;
    MeisoActionExecutorStats executor_stats;

    CHECK(meiso_action_caller_begin_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_STARTED);
    CHECK(meiso_action_caller_submit(caller, &request) == MEISO_ACTION_SUBMIT_NEW);
    CHECK(meiso_action_caller_submit(caller, &request) ==
          MEISO_ACTION_SUBMIT_DUPLICATE);
    CHECK(meiso_action_caller_end_delivery(
        caller, (MeisoDeliveryGeneration){1U}
    ) == MEISO_ACTION_DELIVERY_ENDED_WITH_UNRESOLVED);
    CHECK(meiso_action_caller_stats(caller, &caller_stats));
    CHECK(caller_stats.delivery_starts == 1U);
    CHECK(caller_stats.delivery_ends == 1U);
    CHECK(caller_stats.submissions == 1U);
    CHECK(caller_stats.duplicates == 1U);

    CHECK(meiso_action_executor_ingest(executor, &request, NULL) ==
          MEISO_ACTION_INGEST_NEW);
    CHECK(meiso_action_executor_ingest(executor, &request, NULL) ==
          MEISO_ACTION_INGEST_REPLAY);
    CHECK(meiso_action_executor_stats(executor, &executor_stats));
    CHECK(executor_stats.ingests == 1U);
    CHECK(executor_stats.replays == 1U);
}

int main(void)
{
    test_init_contract();
    test_single_execution_and_orthogonal_ack();
    test_identity_conflict_scope_and_mapped_deadline();
    test_deadline_expiry_is_retained();
    test_capacity_no_eviction_and_replay_when_full();
    test_terminal_transitions_and_indeterminate();
    test_generation_cursor_and_late_ack();
    test_alias_zero_length_and_failure_rollback();
    test_stats();

    if (failures != 0) {
        fprintf(stderr, "%d action test(s) failed\n", failures);
        return 1;
    }
    puts("action tests passed");
    return 0;
}
