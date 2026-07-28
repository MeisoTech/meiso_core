#include "meiso_latest_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #condition);                            \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

static bool view_equals(
    const MeisoLatestView *view,
    uint64_t version,
    const uint8_t *data,
    size_t size
)
{
    return view->version == version && view->size == size &&
        memcmp(view->data, data, size) == 0;
}

static void test_lost_ack_and_idempotence(void)
{
    uint8_t publisher_storage[16];
    uint8_t replica_storage[16];
    const uint8_t value[] = {1U, 2U, 3U};
    MeisoLatestPublisher publisher;
    MeisoLatestReplica replica;
    MeisoLatestView send;
    MeisoLatestView received;

    CHECK(meiso_latest_publisher_init(
        &publisher, publisher_storage, sizeof(publisher_storage)
    ));
    CHECK(meiso_latest_replica_init(
        &replica, replica_storage, sizeof(replica_storage)
    ));
    CHECK(meiso_latest_publisher_publish(
        &publisher, 1U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_QUEUED);
    CHECK(meiso_latest_publisher_pending(&publisher, &send));
    CHECK(view_equals(&send, 1U, value, sizeof(value)));
    CHECK(meiso_latest_replica_receive(
        &replica, send.version, send.data, send.size
    ) == MEISO_LATEST_RECEIVE_NEW);

    CHECK(meiso_latest_publisher_pending(&publisher, &send));
    CHECK(meiso_latest_replica_receive(
        &replica, send.version, send.data, send.size
    ) == MEISO_LATEST_RECEIVE_DUPLICATE);
    CHECK(meiso_latest_replica_current(&replica, &received));
    CHECK(view_equals(&received, 1U, value, sizeof(value)));

    CHECK(meiso_latest_publisher_ack(
        &publisher, 1U
    ) == MEISO_LATEST_ACK_CONFIRMED);
    CHECK(!meiso_latest_publisher_pending(&publisher, &send));
    CHECK(meiso_latest_publisher_ack(
        &publisher, 1U
    ) == MEISO_LATEST_ACK_DUPLICATE);
}

static void test_newest_replaces_pending(void)
{
    uint8_t publisher_storage[16];
    uint8_t replica_storage[16];
    const uint8_t version_two[] = {2U};
    const uint8_t version_three[] = {3U, 3U};
    MeisoLatestPublisher publisher;
    MeisoLatestReplica replica;
    MeisoLatestView send;

    CHECK(meiso_latest_publisher_init(
        &publisher, publisher_storage, sizeof(publisher_storage)
    ));
    CHECK(meiso_latest_replica_init(
        &replica, replica_storage, sizeof(replica_storage)
    ));
    CHECK(meiso_latest_publisher_publish(
        &publisher, 2U, version_two, sizeof(version_two)
    ) == MEISO_LATEST_PUBLISH_QUEUED);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 3U, version_three, sizeof(version_three)
    ) == MEISO_LATEST_PUBLISH_REPLACED);
    CHECK(meiso_latest_publisher_ack(
        &publisher, 2U
    ) == MEISO_LATEST_ACK_STALE);
    CHECK(meiso_latest_publisher_ack(
        &publisher, 4U
    ) == MEISO_LATEST_ACK_FUTURE);
    CHECK(meiso_latest_publisher_pending(&publisher, &send));
    CHECK(view_equals(&send, 3U, version_three, sizeof(version_three)));
    CHECK(meiso_latest_replica_receive(
        &replica, send.version, send.data, send.size
    ) == MEISO_LATEST_RECEIVE_NEW);
    CHECK(meiso_latest_publisher_ack(
        &publisher, 3U
    ) == MEISO_LATEST_ACK_CONFIRMED);
}

static void test_duplicate_conflict_and_stale(void)
{
    uint8_t publisher_storage[16];
    uint8_t replica_storage[16];
    const uint8_t value[] = {4U, 5U};
    const uint8_t conflict[] = {4U, 6U};
    MeisoLatestPublisher publisher;
    MeisoLatestReplica replica;

    CHECK(meiso_latest_publisher_init(
        &publisher, publisher_storage, sizeof(publisher_storage)
    ));
    CHECK(meiso_latest_replica_init(
        &replica, replica_storage, sizeof(replica_storage)
    ));
    CHECK(meiso_latest_publisher_publish(
        &publisher, 8U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_QUEUED);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 8U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_DUPLICATE);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 8U, conflict, sizeof(conflict)
    ) == MEISO_LATEST_PUBLISH_CONFLICT);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 7U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_STALE);

    CHECK(meiso_latest_replica_receive(
        &replica, 8U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_NEW);
    CHECK(meiso_latest_replica_receive(
        &replica, 8U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_DUPLICATE);
    CHECK(meiso_latest_replica_receive(
        &replica, 8U, conflict, sizeof(conflict)
    ) == MEISO_LATEST_RECEIVE_CONFLICT);
    CHECK(meiso_latest_replica_receive(
        &replica, 7U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_STALE);
}

static void test_session_republication(void)
{
    uint8_t publisher_storage[16];
    uint8_t replica_storage[16];
    const uint8_t value[] = {9U, 8U, 7U};
    MeisoLatestPublisher publisher;
    MeisoLatestReplica replica;
    MeisoLatestView view;

    CHECK(meiso_latest_publisher_init(
        &publisher, publisher_storage, sizeof(publisher_storage)
    ));
    CHECK(meiso_latest_replica_init(
        &replica, replica_storage, sizeof(replica_storage)
    ));
    CHECK(meiso_latest_publisher_publish(
        &publisher, 12U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_QUEUED);
    CHECK(meiso_latest_publisher_ack(
        &publisher, 12U
    ) == MEISO_LATEST_ACK_CONFIRMED);
    meiso_latest_publisher_start_session(&publisher);
    CHECK(meiso_latest_publisher_pending(&publisher, &view));
    CHECK(view_equals(&view, 12U, value, sizeof(value)));

    CHECK(meiso_latest_replica_receive(
        &replica, 12U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_NEW);
    meiso_latest_replica_start_session(&replica);
    CHECK(!meiso_latest_replica_current(&replica, &view));
    CHECK(meiso_latest_replica_receive(
        &replica, 12U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_NEW);
}

static void test_capacity_invalid_and_wrap(void)
{
    uint8_t publisher_storage[4];
    uint8_t replica_storage[4];
    const uint8_t value[] = {1U, 2U, 3U, 4U};
    const uint8_t too_large[] = {1U, 2U, 3U, 4U, 5U};
    MeisoLatestPublisher publisher;
    MeisoLatestReplica replica;
    MeisoLatestView view;

    CHECK(!meiso_latest_publisher_init(NULL, publisher_storage, sizeof(publisher_storage)));
    CHECK(!meiso_latest_replica_init(NULL, replica_storage, sizeof(replica_storage)));
    CHECK(!meiso_latest_publisher_init(&publisher, NULL, 0U));
    CHECK(!meiso_latest_replica_init(&replica, NULL, 0U));
    CHECK(meiso_latest_publisher_init(
        &publisher, publisher_storage, sizeof(publisher_storage)
    ));
    CHECK(meiso_latest_replica_init(
        &replica, replica_storage, sizeof(replica_storage)
    ));
    CHECK(meiso_latest_publisher_publish(
        &publisher, 0U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_INVALID);
    CHECK(meiso_latest_replica_receive(
        &replica, 0U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_INVALID);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 1U, too_large, sizeof(too_large)
    ) == MEISO_LATEST_PUBLISH_CAPACITY);
    CHECK(meiso_latest_replica_receive(
        &replica, 1U, too_large, sizeof(too_large)
    ) == MEISO_LATEST_RECEIVE_CAPACITY);
    CHECK(!meiso_latest_publisher_pending(&publisher, &view));
    CHECK(!meiso_latest_replica_current(&replica, &view));

    CHECK(meiso_latest_publisher_publish(
        &publisher, UINT64_MAX, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_QUEUED);
    CHECK(meiso_latest_replica_receive(
        &replica, UINT64_MAX, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_NEW);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 1U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_VERSION_WRAP);
    CHECK(meiso_latest_publisher_publish(
        &publisher, 0U, value, sizeof(value)
    ) == MEISO_LATEST_PUBLISH_VERSION_WRAP);
    CHECK(meiso_latest_replica_receive(
        &replica, 1U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_VERSION_WRAP);
    CHECK(meiso_latest_replica_receive(
        &replica, 0U, value, sizeof(value)
    ) == MEISO_LATEST_RECEIVE_VERSION_WRAP);
    CHECK(meiso_latest_publisher_pending(&publisher, &view));
    CHECK(view.version == UINT64_MAX);
    CHECK(meiso_latest_replica_current(&replica, &view));
    CHECK(view.version == UINT64_MAX);
}

int main(void)
{
    test_lost_ack_and_idempotence();
    test_newest_replaces_pending();
    test_duplicate_conflict_and_stale();
    test_session_republication();
    test_capacity_invalid_and_wrap();
    if (failures != 0) {
        fprintf(stderr, "%d test checks failed\n", failures);
        return 1;
    }
    printf("meiso_latest_state_test: passed\n");
    return 0;
}
