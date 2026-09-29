#include <meiso/core/state.h>

#include "meiso_test_support.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

_Static_assert(sizeof(MeisoStateDeliveryMode) == sizeof(uint8_t), "fixed mode");
_Static_assert(sizeof(MeisoStatePublishStatus) == sizeof(uint8_t), "fixed status");

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #condition);                            \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

typedef union {
    MeisoMaxAlign alignment;
    uint8_t bytes[1024];
} TestArena;

static MeisoResourceDescriptor state_descriptor(size_t capacity)
{
    MeisoResourceDescriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = (MeisoResourceId){10U, 20U};
    descriptor.kind = MEISO_RESOURCE_KIND_STATE;
    descriptor.schema.id = (MeisoSchemaId){30U, 40U};
    descriptor.schema.version = 1U;
    descriptor.owner = MEISO_OWNER_ROLE_EDGE;
    descriptor.max_payload_size = capacity;
    return descriptor;
}

static MeisoStatePublisher *make_publisher(
    TestArena *arena,
    MeisoStateDeliveryMode mode,
    size_t capacity
)
{
    MeisoResourceDescriptor descriptor = state_descriptor(capacity);
    MeisoStateConfig config = {mode};
    MeisoStatePublisher *publisher = NULL;
    const size_t required = meiso_state_publisher_memory_required(&descriptor, &config);

    CHECK(required > 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_state_publisher_init(
        arena->bytes, sizeof(arena->bytes), &descriptor, &config, &publisher
    ) == MEISO_STATE_INIT_OK);
    return publisher;
}

static MeisoStateReplica *make_replica(TestArena *arena, size_t capacity)
{
    MeisoResourceDescriptor descriptor = state_descriptor(capacity);
    MeisoStateReplica *replica = NULL;
    const size_t required = meiso_state_replica_memory_required(&descriptor);

    CHECK(required > 0U);
    CHECK(required <= sizeof(arena->bytes));
    CHECK(meiso_state_replica_init(
        arena->bytes, sizeof(arena->bytes), &descriptor, &replica
    ) == MEISO_STATE_INIT_OK);
    return replica;
}

static bool view_equals(
    const MeisoStateView *view,
    MeisoOwnerScope owner,
    uint64_t version,
    const uint8_t *data,
    size_t size
)
{
    return meiso_owner_scope_equal(view->owner, owner) &&
        view->version == version && view->payload.size == size &&
        (size == 0U || memcmp(view->payload.data, data, size) == 0);
}

static void test_init_contract(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MEISO_TEST_ARENA_STORAGE TestArena before;
    MeisoResourceDescriptor descriptor = state_descriptor(8U);
    MeisoStateConfig config = {MEISO_STATE_DELIVERY_CONVERGENT};
    MeisoStatePublisher *publisher = (MeisoStatePublisher *)(uintptr_t)1U;
    MeisoStateReplica *replica = (MeisoStateReplica *)(uintptr_t)1U;
    size_t required = meiso_state_publisher_memory_required(&descriptor, &config);

    CHECK(meiso_state_alignment_required() >= _Alignof(MeisoMaxAlign));
    CHECK(required > 0U);
    CHECK(meiso_state_publisher_init(
        arena.bytes + 1U, sizeof(arena.bytes) - 1U, &descriptor, &config, &publisher
    ) == MEISO_STATE_INIT_MISALIGNED_ARENA);
    CHECK(publisher == NULL);
    CHECK(meiso_state_publisher_init(
        arena.bytes, required - 1U, &descriptor, &config, &publisher
    ) == MEISO_STATE_INIT_ARENA_TOO_SMALL);
    CHECK(publisher == NULL);
    memset(&arena, 0x5a, sizeof(arena));
    before = arena;
    CHECK(meiso_state_publisher_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoStatePublisher **)arena.bytes
    ) == MEISO_STATE_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);

    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_state_publisher_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config,
        (MeisoStatePublisher **)arena.bytes
    ) == MEISO_STATE_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    CHECK(meiso_state_replica_init(
        arena.bytes, sizeof(arena.bytes), &descriptor,
        (MeisoStateReplica **)arena.bytes
    ) == MEISO_STATE_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&arena, &before, sizeof(arena)) == 0);
    descriptor.kind = MEISO_RESOURCE_KIND_STATE;

    config.delivery_mode = (MeisoStateDeliveryMode)0;
    CHECK(meiso_state_publisher_memory_required(&descriptor, &config) == 0U);
    CHECK(meiso_state_publisher_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &config, &publisher
    ) == MEISO_STATE_INIT_INVALID_CONFIG);
    descriptor.kind = MEISO_RESOURCE_KIND_STREAM;
    CHECK(meiso_state_replica_memory_required(&descriptor) == 0U);
    CHECK(meiso_state_replica_init(
        arena.bytes, sizeof(arena.bytes), &descriptor, &replica
    ) == MEISO_STATE_INIT_INVALID_DESCRIPTOR);
    CHECK(replica == NULL);

    descriptor = state_descriptor(SIZE_MAX);
    config.delivery_mode = MEISO_STATE_DELIVERY_CONVERGENT;
    CHECK(meiso_state_publisher_memory_required(&descriptor, &config) == 0U);
    CHECK(meiso_state_replica_memory_required(&descriptor) == 0U);
}

static void test_convergent_lost_ack_and_actual_separation(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena publisher_arena;
    MEISO_TEST_ARENA_STORAGE TestArena replica_arena;
    MeisoStatePublisher *publisher = make_publisher(
        &publisher_arena, MEISO_STATE_DELIVERY_CONVERGENT, 16U
    );
    MeisoStateReplica *replica = make_replica(&replica_arena, 16U);
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 7U};
    const MeisoDeliveryGeneration delivery = {11U};
    const uint8_t payload[] = {1U, 2U, 3U};
    MeisoStateOffer offer;
    MeisoStateView actual;

    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){payload, sizeof(payload)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(!meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(meiso_state_publisher_begin_delivery(publisher, delivery) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(offer.generation.value == delivery.value);
    CHECK(view_equals(&offer.state, owner, 1U, payload, sizeof(payload)));

    CHECK(!meiso_state_replica_current(replica, &actual));
    CHECK(meiso_state_replica_begin_owner(replica, owner) == MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_replica_receive(
        replica, owner, offer.state.version, offer.state.payload
    ) == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_receive(
        replica, owner, offer.state.version, offer.state.payload
    ) == MEISO_STATE_RECEIVE_DUPLICATE);
    CHECK(meiso_state_replica_current(replica, &actual));
    CHECK(view_equals(&actual, owner, 1U, payload, sizeof(payload)));

    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher, delivery, owner, 1U
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(!meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher, delivery, owner, 1U
    ) == MEISO_STATE_ACCEPT_DUPLICATE);
    CHECK(meiso_state_publisher_end_delivery(publisher, delivery) ==
          MEISO_STATE_DELIVERY_ENDED);
}

static void test_newest_replaces_pending_and_failures_do_not_mutate(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStatePublisher *publisher = make_publisher(
        &arena, MEISO_STATE_DELIVERY_CONVERGENT, 4U
    );
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 9U};
    const MeisoDeliveryGeneration delivery = {2U};
    const uint8_t one[] = {1U};
    const uint8_t two[] = {2U, 2U};
    const uint8_t conflict[] = {8U};
    const uint8_t oversized[] = {0U, 1U, 2U, 3U, 4U};
    MeisoStateOffer offer;
    MeisoStateView current;

    CHECK(meiso_state_publisher_begin_delivery(publisher, delivery) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){one, sizeof(one)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 2U, (MeisoByteView){two, sizeof(two)}
    ) == MEISO_STATE_PUBLISH_REPLACED_PENDING);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher, delivery, owner, 1U
    ) == MEISO_STATE_ACCEPT_STALE);
    CHECK(meiso_state_publisher_next_offer(publisher, &offer));
    CHECK(view_equals(&offer.state, owner, 2U, two, sizeof(two)));

    CHECK(meiso_state_publisher_publish(
        publisher, owner, 2U, (MeisoByteView){two, sizeof(two)}
    ) == MEISO_STATE_PUBLISH_DUPLICATE);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 2U, (MeisoByteView){conflict, sizeof(conflict)}
    ) == MEISO_STATE_PUBLISH_CONFLICT);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){one, sizeof(one)}
    ) == MEISO_STATE_PUBLISH_STALE);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 3U, (MeisoByteView){oversized, sizeof(oversized)}
    ) == MEISO_STATE_PUBLISH_CAPACITY);
    CHECK(meiso_state_publisher_publish(
        publisher, (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 10U}, 3U,
        (MeisoByteView){one, sizeof(one)}
    ) == MEISO_STATE_PUBLISH_SCOPE_MISMATCH);
    CHECK(meiso_state_publisher_current(publisher, &current));
    CHECK(view_equals(&current, owner, 2U, two, sizeof(two)));
}

static void test_delivery_discontinuity_modes(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena convergent_arena;
    MEISO_TEST_ARENA_STORAGE TestArena best_effort_arena;
    MeisoStatePublisher *convergent = make_publisher(
        &convergent_arena, MEISO_STATE_DELIVERY_CONVERGENT, 8U
    );
    MeisoStatePublisher *best_effort = make_publisher(
        &best_effort_arena, MEISO_STATE_DELIVERY_BEST_EFFORT, 8U
    );
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t value[] = {5U};
    MeisoStateOffer offer;
    MeisoStatePublisherStats stats;

    CHECK(meiso_state_publisher_begin_delivery(
        convergent, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_publish(
        convergent, owner, 1U, (MeisoByteView){value, sizeof(value)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        convergent, (MeisoDeliveryGeneration){2U}, owner, 1U
    ) == MEISO_STATE_ACCEPT_DELIVERY_MISMATCH);
    CHECK(meiso_state_publisher_mark_delivery_failed(
        convergent, (MeisoDeliveryGeneration){1U}, owner, 1U
    ) == MEISO_STATE_FAILURE_RETAINED);
    CHECK(meiso_state_publisher_next_offer(convergent, &offer));
    CHECK(meiso_state_publisher_end_delivery(
        convergent, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_CONVERGENCE_FAILED);
    CHECK(meiso_state_publisher_stats(convergent, &stats));
    CHECK(stats.delivery_failures == 1U);
    CHECK(stats.convergence_failures == 1U);
    CHECK(!meiso_state_publisher_next_offer(convergent, &offer));
    CHECK(meiso_state_publisher_publish(
        convergent, owner, 2U, (MeisoByteView){value, sizeof(value)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_begin_delivery(
        convergent, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_STALE_GENERATION);
    CHECK(meiso_state_publisher_begin_delivery(
        convergent, (MeisoDeliveryGeneration){2U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_next_offer(convergent, &offer));
    CHECK(offer.state.version == 2U);

    CHECK(meiso_state_publisher_publish(
        best_effort, owner, 1U, (MeisoByteView){value, sizeof(value)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_begin_delivery(
        best_effort, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_next_offer(best_effort, &offer));
    CHECK(offer.state.version == 1U);
    CHECK(meiso_state_publisher_mark_offer_complete(
        best_effort, (MeisoDeliveryGeneration){1U}, owner, 1U
    ) == MEISO_STATE_OFFER_COMPLETED);
    CHECK(!meiso_state_publisher_next_offer(best_effort, &offer));
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        best_effort, (MeisoDeliveryGeneration){1U}, owner, 1U
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        best_effort, (MeisoDeliveryGeneration){1U}, owner, 1U
    ) == MEISO_STATE_ACCEPT_DUPLICATE);
    CHECK(meiso_state_publisher_publish(
        best_effort, owner, 2U, (MeisoByteView){value, sizeof(value)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_next_offer(best_effort, &offer));
    CHECK(meiso_state_publisher_mark_delivery_failed(
        best_effort, (MeisoDeliveryGeneration){1U}, owner, 2U
    ) == MEISO_STATE_FAILURE_DROPPED);
    CHECK(!meiso_state_publisher_next_offer(best_effort, &offer));
    CHECK(meiso_state_publisher_end_delivery(
        best_effort, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_ENDED);
    CHECK(meiso_state_publisher_begin_delivery(
        best_effort, (MeisoDeliveryGeneration){2U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_next_offer(best_effort, &offer));
    CHECK(offer.state.version == 2U);
}

static void test_empty_delivery_has_no_scope_mismatch(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStatePublisher *publisher = make_publisher(
        &arena, MEISO_STATE_DELIVERY_BEST_EFFORT, 1U
    );
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 1U};
    const MeisoDeliveryGeneration generation = {1U};
    MeisoStatePublisherStats stats;

    CHECK(meiso_state_publisher_begin_delivery(publisher, generation) ==
          MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher, generation, owner, 1U
    ) == MEISO_STATE_ACCEPT_NOT_PENDING);
    CHECK(meiso_state_publisher_mark_delivery_failed(
        publisher, generation, owner, 1U
    ) == MEISO_STATE_FAILURE_NOT_PENDING);
    CHECK(meiso_state_publisher_mark_offer_complete(
        publisher, generation, owner, 1U
    ) == MEISO_STATE_OFFER_COMPLETE_NOT_PENDING);
    CHECK(meiso_state_publisher_stats(publisher, &stats));
    CHECK(stats.scope_mismatches == 0U);
}

static void test_replica_owner_reset_wrap_and_zero_payload(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStateReplica *replica = make_replica(&arena, 2U);
    const MeisoOwnerScope first = {MEISO_OWNER_ROLE_EDGE, 1U};
    const MeisoOwnerScope second = {MEISO_OWNER_ROLE_EDGE, 2U};
    const uint8_t byte = 9U;
    MeisoStateView view;

    CHECK(meiso_state_replica_receive(
        replica, first, 1U, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_RECEIVE_NO_OWNER);
    CHECK(meiso_state_replica_begin_owner(replica, first) == MEISO_STATE_OWNER_STARTED);
    CHECK(meiso_state_replica_receive(
        replica, first, UINT64_MAX, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_receive(
        replica, first, 1U, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_RECEIVE_VERSION_WRAP);
    CHECK(meiso_state_replica_begin_owner(replica, second) == MEISO_STATE_OWNER_STARTED);
    CHECK(!meiso_state_replica_current(replica, &view));
    CHECK(meiso_state_replica_receive(
        replica, first, 1U, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_RECEIVE_SCOPE_MISMATCH);
    CHECK(meiso_state_replica_receive(
        replica, second, 1U, (MeisoByteView){NULL, 0U}
    ) == MEISO_STATE_RECEIVE_NEW);
    CHECK(meiso_state_replica_current(replica, &view));
    CHECK(view_equals(&view, second, 1U, NULL, 0U));
}

static void test_overlap_and_role_scope(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStatePublisher *publisher = make_publisher(
        &arena, MEISO_STATE_DELIVERY_CONVERGENT, 4U
    );
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 1U};
    const uint8_t bytes[] = {1U, 2U};
    MeisoStateView current;

    CHECK(meiso_state_publisher_publish(
        publisher, (MeisoOwnerScope){MEISO_OWNER_ROLE_HOST, 1U}, 1U,
        (MeisoByteView){bytes, sizeof(bytes)}
    ) == MEISO_STATE_PUBLISH_SCOPE_MISMATCH);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){bytes, sizeof(bytes)}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_current(publisher, &current));
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 2U, current.payload
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_current(publisher, &current));
    CHECK(view_equals(&current, owner, 2U, bytes, sizeof(bytes)));
}

static void test_stats(void)
{
    MEISO_TEST_ARENA_STORAGE TestArena arena;
    MeisoStatePublisher *publisher = make_publisher(
        &arena, MEISO_STATE_DELIVERY_CONVERGENT, 1U
    );
    const MeisoOwnerScope owner = {MEISO_OWNER_ROLE_EDGE, 3U};
    const uint8_t byte = 1U;
    MeisoStatePublisherStats stats;

    CHECK(meiso_state_publisher_begin_delivery(
        publisher, (MeisoDeliveryGeneration){1U}
    ) == MEISO_STATE_DELIVERY_STARTED);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_PUBLISH_NEW);
    CHECK(meiso_state_publisher_publish(
        publisher, owner, 1U, (MeisoByteView){&byte, 1U}
    ) == MEISO_STATE_PUBLISH_DUPLICATE);
    CHECK(meiso_state_publisher_mark_delivery_accepted(
        publisher, (MeisoDeliveryGeneration){1U}, owner, 1U
    ) == MEISO_STATE_ACCEPT_CONFIRMED);
    CHECK(meiso_state_publisher_stats(publisher, &stats));
    CHECK(stats.publishes == 1U);
    CHECK(stats.duplicates == 1U);
    CHECK(stats.delivery_accepts == 1U);
}

int main(void)
{
    test_init_contract();
    test_convergent_lost_ack_and_actual_separation();
    test_newest_replaces_pending_and_failures_do_not_mutate();
    test_delivery_discontinuity_modes();
    test_empty_delivery_has_no_scope_mismatch();
    test_replica_owner_reset_wrap_and_zero_payload();
    test_overlap_and_role_scope();
    test_stats();

    if (failures != 0) {
        fprintf(stderr, "%d state test(s) failed\n", failures);
        return 1;
    }
    puts("state tests passed");
    return 0;
}
