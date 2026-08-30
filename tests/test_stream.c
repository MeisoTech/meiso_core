#include <meiso/core/stream.h>

#include "meiso_test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

_Static_assert(sizeof(MeisoStreamOrdering) == sizeof(uint8_t), "fixed ordering");
_Static_assert(sizeof(MeisoStreamReceiveCode) == sizeof(uint8_t), "fixed status");

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #condition);                            \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

typedef union {
    max_align_t alignment;
    uint8_t bytes[8192];
} TestArena;

static MeisoResourceDescriptor stream_descriptor(size_t payload_capacity)
{
    MeisoResourceDescriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = (MeisoResourceId){101U, 102U};
    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;
    descriptor.schema.id = (MeisoSchemaId){103U, 104U};
    descriptor.schema.version = 1U;
    descriptor.owner = MEISO_OWNER_ROLE_EDGE;
    descriptor.max_payload_size = payload_capacity;
    return descriptor;
}

static MeisoStreamConfig stream_config(
    MeisoStreamOverflowPolicy overflow,
    MeisoStreamDeadlinePolicy deadline_policy,
    uint32_t queue_slots,
    uint32_t history_slots,
    size_t dependency_capacity
)
{
    MeisoStreamConfig config;

    memset(&config, 0, sizeof(config));
    config.ordering = MEISO_STREAM_ORDER_STRICT_MONOTONIC;
    config.overflow = overflow;
    config.deadline_policy = deadline_policy;
    config.queue_slots = queue_slots;
    config.history_slots = history_slots;
    config.max_dependency_size = dependency_capacity;
    return config;
}

static MeisoStream *make_stream(
    TestArena *arena,
    size_t payload_capacity,
    MeisoStreamConfig config
)
{
    MeisoResourceDescriptor descriptor = stream_descriptor(payload_capacity);
    MeisoStream *stream = NULL;
    const size_t required = meiso_stream_memory_required(&descriptor, &config);

    CHECK(required > 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_stream_init(
        arena->bytes, sizeof(arena->bytes), &descriptor, &config, &stream
    ) == MEISO_STREAM_INIT_OK);
    return stream;
}

static MeisoStreamSample sample(
    MeisoOwnerScope producer,
    uint64_t epoch,
    uint64_t sequence,
    uint64_t source_ns,
    const uint8_t *dependency,
    size_t dependency_size,
    const uint8_t *payload,
    size_t payload_size
)
{
    MeisoStreamSample value;

    memset(&value, 0, sizeof(value));
    value.key.producer = producer;
    value.key.epoch = epoch;
    value.key.sequence = sequence;
    value.source_time = (MeisoTimePoint){9U, source_ns};
    value.dependency = (MeisoByteView){dependency, dependency_size};
    value.payload = (MeisoByteView){payload, payload_size};
    return value;
}

static bool sample_view_equals(
    const MeisoStreamSample *value,
    uint64_t sequence,
    const uint8_t *payload,
    size_t payload_size
)
{
    return value->key.sequence == sequence &&
        value->payload.size == payload_size &&
        (payload_size == 0U ||
         memcmp(value->payload.data, payload, payload_size) == 0);
}

static void test_init_contract(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MEISO_TEST_ARENA_STORAGE TestArena before;
    MeisoResourceDescriptor descriptor = stream_descriptor(8U);
    MeisoStreamConfig config = stream_config(
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        2U,
        2U,
        4U
    );
    MeisoStream *stream = (MeisoStream *)(uintptr_t)1U;
    size_t required = meiso_stream_memory_required(&descriptor, &config);

    CHECK(required > 0U);
    CHECK(meiso_stream_alignment_required() >= _Alignof(max_align_t));
    CHECK(meiso_stream_init(
        arena.bytes + 1U, sizeof(arena.bytes) - 1U,
        &descriptor, &config, &stream
    ) == MEISO_STREAM_INIT_MISALIGNED_ARENA);
    CHECK(stream == NULL);
    CHECK(meiso_stream_init(
        arena.bytes, required - 1U, &descriptor, &config, &stream
    ) == MEISO_STREAM_INIT_ARENA_TOO_SMALL);
    CHECK(stream == NULL);
    memset(&arena, 0x5a, sizeof(arena));
    before = arena;
    CHECK(meiso_stream_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoStream **)arena.bytes
    ) == MEISO_STREAM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);

    descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    CHECK(meiso_stream_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoStream **)arena.bytes
    ) == MEISO_STREAM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;

    config.queue_slots = 0U;
    CHECK(meiso_stream_memory_required(&descriptor, &config) == 0U);
    config = stream_config(
        MEISO_STREAM_OVERFLOW_DROP_OLDEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        1U,
        UINT32_MAX,
        SIZE_MAX
    );
    descriptor.max_payload_size = SIZE_MAX;
    CHECK(meiso_stream_memory_required(&descriptor, &config) == 0U);
    descriptor = stream_descriptor(8U);
    descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    CHECK(meiso_stream_memory_required(&descriptor, &config) == 0U);
}

static void test_ordering_history_duplicate_and_conflict(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStream *stream = make_stream(
        &arena,
        4U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            3U,
            3U,
            2U
        )
    );
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t dep[] = {7U};
    const uint8_t changed_dep[] = {8U, 9U};
    const uint8_t one[] = {1U};
    const uint8_t changed[] = {9U};
    const uint8_t oversized[] = {1U, 2U, 3U, 4U, 5U};
    MeisoStreamSample first = sample(
        producer, 2U, 1U, 100U, dep, sizeof(dep), one, sizeof(one)
    );
    MeisoStreamSample third = sample(
        producer, 2U, 3U, 300U, dep, sizeof(dep), one, sizeof(one)
    );
    MeisoStreamSample view;
    MeisoStreamReceiveResult result;

    CHECK(meiso_stream_begin_producer(stream, producer, 2U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    result = meiso_stream_receive(stream, &first, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_NONE);
    CHECK(result.missing_before == 0U);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    first.payload = (MeisoByteView){changed, sizeof(changed)};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.payload = (MeisoByteView){one, sizeof(one)};
    first.source_time.monotonic_ns += 1U;
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.source_time.monotonic_ns = 100U;
    first.source_time.clock_epoch += 1U;
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.source_time.clock_epoch = 9U;
    first.dependency = (MeisoByteView){changed_dep, 1U};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.dependency = (MeisoByteView){changed_dep, sizeof(changed_dep)};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.dependency = (MeisoByteView){dep, sizeof(dep)};
    first.payload = (MeisoByteView){changed_dep, sizeof(changed_dep)};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.payload = (MeisoByteView){one, sizeof(one)};
    first.sender_deadline.value = (MeisoTimePoint){UINT64_MAX, UINT64_MAX};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);

    result = meiso_stream_receive(stream, &third, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_GAP);
    CHECK(result.missing_before == 1U);
    CHECK(meiso_stream_peek(stream, &view));
    CHECK(sample_view_equals(&view, 1U, one, sizeof(one)));
    CHECK(meiso_stream_pop(stream));
    first.source_time.monotonic_ns = 100U;
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    CHECK(meiso_stream_pop(stream));

    first.key.sequence = 2U;
    first.source_time.monotonic_ns = 200U;
    first.payload = (MeisoByteView){oversized, sizeof(oversized)};
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
    CHECK(meiso_stream_count(stream) == 0U);
}

static void test_drop_oldest_and_reject_newest(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena drop_arena;
    MEISO_TEST_ARENA_STORAGE TestArena reject_arena;
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 4U};
    const uint8_t values[] = {1U, 2U, 3U};
    MeisoStream *drop = make_stream(
        &drop_arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            2U,
            1U,
            0U
        )
    );
    MeisoStream *reject = make_stream(
        &reject_arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            1U,
            1U,
            0U
        )
    );
    MeisoStreamSample first = sample(
        producer, 1U, 1U, 1U, NULL, 0U, &values[0], 1U
    );
    MeisoStreamSample second = sample(
        producer, 1U, 2U, 2U, NULL, 0U, &values[1], 1U
    );
    MeisoStreamSample third = sample(
        producer, 1U, 3U, 3U, NULL, 0U, &values[2], 1U
    );
    MeisoStreamSample view;
    MeisoStreamReceiveResult result;

    CHECK(meiso_stream_begin_producer(drop, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(drop, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_receive(drop, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    result = meiso_stream_receive(drop, &third, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST);
    CHECK(meiso_stream_peek(drop, &view));
    CHECK(sample_view_equals(&view, 2U, &values[1], 1U));
    CHECK(meiso_stream_receive(drop, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
    CHECK(meiso_stream_receive(drop, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);

    CHECK(meiso_stream_begin_producer(reject, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(reject, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_receive(reject, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_OVERFLOW_REJECTED);
    CHECK(meiso_stream_pop(reject));
    CHECK(meiso_stream_receive(reject, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
}

static void test_retention_is_receive_ordered(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStream *stream = make_stream(
        &arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            2U,
            1U,
            0U
        )
    );
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t byte = 1U;
    MeisoStreamSample first = sample(
        producer, 1U, 1U, 1U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample second = sample(
        producer, 1U, 2U, 2U, NULL, 0U, &byte, 1U
    );

    CHECK(meiso_stream_begin_producer(stream, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_receive(stream, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_pop(stream));
    CHECK(meiso_stream_receive(stream, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
}

static void test_queued_identity_survives_history_rotation(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStream *stream = make_stream(
        &arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE,
            2U,
            1U,
            0U
        )
    );
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t byte = 1U;
    MeisoStreamSample first = sample(
        producer, 1U, 1U, 1U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample second = sample(
        producer, 1U, 2U, 2U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample third = sample(
        producer, 1U, 3U, 3U, NULL, 0U, &byte, 1U
    );
    const MeisoStreamAdmission expired = {
        1U, 1U, {7U, 10U}, {7U, 11U}
    };

    second.sender_deadline = (MeisoDeadline){true, {9U, 20U}};
    third.sender_deadline = (MeisoDeadline){true, {9U, 30U}};
    CHECK(meiso_stream_begin_producer(stream, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_receive(stream, &second, &expired).code ==
          MEISO_STREAM_RECEIVE_DROPPED_EXPIRED);
    CHECK(meiso_stream_receive(stream, &third, &expired).code ==
          MEISO_STREAM_RECEIVE_DROPPED_EXPIRED);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    CHECK(meiso_stream_receive(stream, &third, NULL).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    CHECK(meiso_stream_receive(stream, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
    CHECK(meiso_stream_pop(stream));
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_STALE);
}

static void test_deadline_is_admission_not_identity(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStream *stream = make_stream(
        &arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
            MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE,
            2U,
            2U,
            0U
        )
    );
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 3U};
    const uint8_t byte = 1U;
    MeisoStreamSample first = sample(
        producer, 8U, 1U, 10U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample second = sample(
        producer, 8U, 2U, 20U, NULL, 0U, &byte, 1U
    );
    MeisoStreamAdmission expired = {1U, 1U, {5U, 100U}, {5U, 101U}};
    MeisoStreamAdmission boundary = {1U, 1U, {6U, 200U}, {6U, 200U}};
    MeisoStreamAdmission changed_projection = {1U, 1U, {7U, 400U}, {7U, 0U}};
    MeisoStreamReceiveResult result;

    first.sender_deadline = (MeisoDeadline){true, {9U, 50U}};
    second.sender_deadline = (MeisoDeadline){true, {9U, 60U}};
    CHECK(meiso_stream_begin_producer(stream, producer, 8U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_CLOCK_SCOPE_MISMATCH);
    result = meiso_stream_receive(stream, &first, &expired);
    CHECK(result.code == MEISO_STREAM_RECEIVE_DROPPED_EXPIRED);
    CHECK(result.missing_before == 0U);
    CHECK(meiso_stream_count(stream) == 0U);
    CHECK(meiso_stream_receive(stream, &first, &changed_projection).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    first.sender_deadline.value.monotonic_ns += 1U;
    CHECK(meiso_stream_receive(stream, &first, &changed_projection).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.sender_deadline.value.monotonic_ns -= 1U;
    first.sender_deadline.present = false;
    CHECK(meiso_stream_receive(stream, &first, &changed_projection).code ==
          MEISO_STREAM_RECEIVE_CONFLICT);
    first.sender_deadline = (MeisoDeadline){true, {8U, 50U}};
    CHECK(meiso_stream_receive(stream, &first, &changed_projection).code ==
          MEISO_STREAM_RECEIVE_INVALID);
    first.sender_deadline = (MeisoDeadline){true, {9U, 50U}};
    result = meiso_stream_receive(stream, &second, &boundary);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(result.missing_before == 0U);
}

static void test_expiry_precedes_overflow_and_unsupported_is_explicit(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena deadline_arena;
    MEISO_TEST_ARENA_STORAGE TestArena unsupported_arena;
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t byte = 1U;
    MeisoStream *deadline_stream = make_stream(
        &deadline_arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
            MEISO_STREAM_DEADLINE_REQUIRE_COMPARABLE,
            1U,
            2U,
            0U
        )
    );
    MeisoStream *unsupported_stream = make_stream(
        &unsupported_arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            1U,
            1U,
            0U
        )
    );
    MeisoStreamSample first = sample(
        producer, 1U, 1U, 1U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample second = sample(
        producer, 1U, 2U, 2U, NULL, 0U, &byte, 1U
    );
    MeisoStreamSample third = sample(
        producer, 1U, 3U, 3U, NULL, 0U, &byte, 1U
    );
    MeisoStreamAdmission expired = {1U, 1U, {7U, 10U}, {7U, 11U}};
    MeisoStreamAdmission live = {1U, 1U, {7U, 20U}, {7U, 19U}};
    MeisoStreamSample view;
    MeisoStreamStats stats;

    first.sender_deadline = (MeisoDeadline){false, {UINT64_MAX, UINT64_MAX}};
    second.sender_deadline = (MeisoDeadline){true, {9U, 20U}};
    third.sender_deadline = (MeisoDeadline){true, {9U, 30U}};
    CHECK(meiso_stream_begin_producer(deadline_stream, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(deadline_stream, &first, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_receive(deadline_stream, &second, &expired).code ==
          MEISO_STREAM_RECEIVE_DROPPED_EXPIRED);
    CHECK(meiso_stream_count(deadline_stream) == 1U);
    CHECK(meiso_stream_peek(deadline_stream, &view));
    CHECK(view.key.sequence == 1U);
    CHECK(meiso_stream_receive(deadline_stream, &second, &live).code ==
          MEISO_STREAM_RECEIVE_DUPLICATE);
    CHECK(meiso_stream_receive(deadline_stream, &third, &live).code ==
          MEISO_STREAM_RECEIVE_OVERFLOW_REJECTED);
    CHECK(meiso_stream_pop(deadline_stream));
    CHECK(meiso_stream_receive(deadline_stream, &third, &live).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);

    CHECK(meiso_stream_begin_producer(unsupported_stream, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(unsupported_stream, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_DEADLINE_UNSUPPORTED);
    CHECK(meiso_stream_stats(unsupported_stream, &stats));
    CHECK(stats.deadline_unsupported == 1U);
    second.sender_deadline.present = false;
    CHECK(meiso_stream_receive(unsupported_stream, &second, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
}

static void test_epoch_scope_wrap_capacity_zero_and_alias(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MEISO_TEST_ARENA_STORAGE TestArena zero_arena;
    MeisoStream *stream = make_stream(
        &arena,
        2U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            1U,
            1U,
            1U
        )
    );
    MeisoStream *zero = make_stream(
        &zero_arena,
        0U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            1U,
            0U,
            0U
        )
    );
    const MeisoOwnerScope first_producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const MeisoOwnerScope second_producer = {MEISO_OWNER_ROLE_EDGE, 2U};
    const uint8_t bytes[] = {1U, 2U, 3U};
    MeisoStreamSample value = sample(
        first_producer, 1U, 1U, 1U, NULL, 0U, bytes, 2U
    );
    MeisoStreamSample borrowed;

    CHECK(meiso_stream_begin_producer(
        stream, (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 1U}, 1U
    ) == MEISO_STREAM_PRODUCER_SCOPE_MISMATCH);
    CHECK(meiso_stream_begin_producer(stream, first_producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    CHECK(meiso_stream_receive(stream, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(meiso_stream_peek(stream, &borrowed));
    CHECK(sample_view_equals(&borrowed, 1U, bytes, 2U));
    value.key.sequence = 2U;
    value.source_time.monotonic_ns = 2U;
    value.payload = borrowed.payload;
    {
        MeisoStreamReceiveResult result = meiso_stream_receive(stream, &value, NULL);
        CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
        CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_DROPPED_OLDEST);
        CHECK(result.missing_before == 0U);
    }
    CHECK(meiso_stream_peek(stream, &borrowed));
    CHECK(sample_view_equals(&borrowed, 2U, bytes, 2U));
    CHECK(meiso_stream_begin_producer(stream, first_producer, 2U) ==
          MEISO_STREAM_PRODUCER_SWITCHED);
    value.key.epoch = 1U;
    CHECK(meiso_stream_receive(stream, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_EPOCH_MISMATCH);
    CHECK(meiso_stream_begin_producer(stream, second_producer, 1U) ==
          MEISO_STREAM_PRODUCER_SWITCHED);
    value.key.producer = first_producer;
    CHECK(meiso_stream_receive(stream, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_SCOPE_MISMATCH);

    CHECK(meiso_stream_begin_producer(stream, second_producer, UINT64_MAX) ==
          MEISO_STREAM_PRODUCER_SWITCHED);
    value = sample(
        second_producer, UINT64_MAX, UINT64_MAX, 3U, NULL, 0U, bytes, 1U
    );
    {
        MeisoStreamReceiveResult result = meiso_stream_receive(stream, &value, NULL);
        CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
        CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_GAP);
        CHECK(result.missing_before == UINT64_MAX - 1U);
    }
    value.key.sequence = 1U;
    CHECK(meiso_stream_receive(stream, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_SEQUENCE_WRAP);
    CHECK(meiso_stream_begin_producer(stream, second_producer, 1U) ==
          MEISO_STREAM_PRODUCER_SWITCHED);

    CHECK(meiso_stream_begin_producer(zero, first_producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    value = sample(first_producer, 1U, 1U, 1U, NULL, 0U, NULL, 0U);
    CHECK(meiso_stream_receive(zero, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_ACCEPTED);
    value.key.sequence = 2U;
    value.payload = (MeisoByteView){bytes, 1U};
    CHECK(meiso_stream_receive(zero, &value, NULL).code ==
          MEISO_STREAM_RECEIVE_CAPACITY);
}

static void test_stats(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStream *stream = make_stream(
        &arena,
        1U,
        stream_config(
            MEISO_STREAM_OVERFLOW_DROP_OLDEST,
            MEISO_STREAM_DEADLINE_UNSUPPORTED,
            1U,
            0U,
            0U
        )
    );
    const MeisoOwnerScope producer = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t byte = 1U;
    MeisoStreamSample value = sample(
        producer, 1U, 3U, 1U, NULL, 0U, &byte, 1U
    );
    MeisoStreamStats stats;
    MeisoStreamReceiveResult result;

    CHECK(meiso_stream_begin_producer(stream, producer, 1U) ==
          MEISO_STREAM_PRODUCER_STARTED);
    result = meiso_stream_receive(stream, &value, NULL);
    CHECK(result.code == MEISO_STREAM_RECEIVE_ACCEPTED);
    CHECK(result.flags == MEISO_STREAM_RECEIVE_FLAG_GAP);
    CHECK(meiso_stream_stats(stream, &stats));
    CHECK(stats.accepted == 1U);
    CHECK(stats.gaps == 1U);
    CHECK(stats.missing_samples == 2U);
}

int main(void)
{
    test_init_contract();
    test_ordering_history_duplicate_and_conflict();
    test_drop_oldest_and_reject_newest();
    test_retention_is_receive_ordered();
    test_queued_identity_survives_history_rotation();
    test_deadline_is_admission_not_identity();
    test_expiry_precedes_overflow_and_unsupported_is_explicit();
    test_epoch_scope_wrap_capacity_zero_and_alias();
    test_stats();

    if (failures != 0) {
        fprintf(stderr, "%d stream test(s) failed\n", failures);
        return 1;
    }
    puts("stream tests passed");
    return 0;
}
