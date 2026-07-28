#include "meiso_latest_state.h"

#include <limits.h>
#include <string.h>

static bool publisher_valid(const MeisoLatestPublisher *publisher)
{
    return publisher != NULL && publisher->storage != NULL &&
        publisher->capacity > 0U;
}

static bool replica_valid(const MeisoLatestReplica *replica)
{
    return replica != NULL && replica->storage != NULL && replica->capacity > 0U;
}

static bool content_equal(
    const uint8_t *stored,
    size_t stored_size,
    const void *candidate,
    size_t candidate_size
)
{
    return stored_size == candidate_size &&
        memcmp(stored, candidate, stored_size) == 0;
}

bool meiso_latest_publisher_init(
    MeisoLatestPublisher *publisher,
    uint8_t *storage,
    size_t capacity
)
{
    if (publisher == NULL) {
        return false;
    }
    memset(publisher, 0, sizeof(*publisher));
    if (storage == NULL || capacity == 0U) {
        return false;
    }
    publisher->storage = storage;
    publisher->capacity = capacity;
    return true;
}

MeisoLatestPublishStatus meiso_latest_publisher_publish(
    MeisoLatestPublisher *publisher,
    uint64_t version,
    const void *data,
    size_t size
)
{
    MeisoLatestPublishStatus result = MEISO_LATEST_PUBLISH_QUEUED;

    if (!publisher_valid(publisher) || data == NULL || size == 0U) {
        return MEISO_LATEST_PUBLISH_INVALID;
    }
    if (publisher->has_value && publisher->version == UINT64_MAX &&
        version < publisher->version) {
        return MEISO_LATEST_PUBLISH_VERSION_WRAP;
    }
    if (version == 0U) {
        return MEISO_LATEST_PUBLISH_INVALID;
    }
    if (publisher->has_value) {
        if (version == publisher->version) {
            if (content_equal(publisher->storage, publisher->size, data, size)) {
                return MEISO_LATEST_PUBLISH_DUPLICATE;
            }
            return MEISO_LATEST_PUBLISH_CONFLICT;
        }
        if (version < publisher->version) {
            return MEISO_LATEST_PUBLISH_STALE;
        }
    }
    if (size > publisher->capacity) {
        return MEISO_LATEST_PUBLISH_CAPACITY;
    }
    if (publisher->pending) {
        result = MEISO_LATEST_PUBLISH_REPLACED;
    }
    memmove(publisher->storage, data, size);
    publisher->size = size;
    publisher->version = version;
    publisher->has_value = true;
    publisher->pending = true;
    return result;
}

bool meiso_latest_publisher_pending(
    const MeisoLatestPublisher *publisher,
    MeisoLatestView *view
)
{
    if (!publisher_valid(publisher) || view == NULL || !publisher->has_value ||
        !publisher->pending) {
        return false;
    }
    view->version = publisher->version;
    view->data = publisher->storage;
    view->size = publisher->size;
    return true;
}

MeisoLatestAckStatus meiso_latest_publisher_ack(
    MeisoLatestPublisher *publisher,
    uint64_t version
)
{
    if (!publisher_valid(publisher) || !publisher->has_value || version == 0U) {
        return MEISO_LATEST_ACK_INVALID;
    }
    if (version < publisher->version) {
        return MEISO_LATEST_ACK_STALE;
    }
    if (version > publisher->version) {
        return MEISO_LATEST_ACK_FUTURE;
    }
    if (!publisher->pending) {
        return MEISO_LATEST_ACK_DUPLICATE;
    }
    publisher->pending = false;
    return MEISO_LATEST_ACK_CONFIRMED;
}

void meiso_latest_publisher_start_session(MeisoLatestPublisher *publisher)
{
    if (publisher_valid(publisher)) {
        publisher->pending = publisher->has_value;
    }
}

bool meiso_latest_replica_init(
    MeisoLatestReplica *replica,
    uint8_t *storage,
    size_t capacity
)
{
    if (replica == NULL) {
        return false;
    }
    memset(replica, 0, sizeof(*replica));
    if (storage == NULL || capacity == 0U) {
        return false;
    }
    replica->storage = storage;
    replica->capacity = capacity;
    return true;
}

MeisoLatestReceiveStatus meiso_latest_replica_receive(
    MeisoLatestReplica *replica,
    uint64_t version,
    const void *data,
    size_t size
)
{
    if (!replica_valid(replica) || data == NULL || size == 0U) {
        return MEISO_LATEST_RECEIVE_INVALID;
    }
    if (replica->has_value && replica->version == UINT64_MAX &&
        version < replica->version) {
        return MEISO_LATEST_RECEIVE_VERSION_WRAP;
    }
    if (version == 0U) {
        return MEISO_LATEST_RECEIVE_INVALID;
    }
    if (replica->has_value) {
        if (version == replica->version) {
            if (content_equal(replica->storage, replica->size, data, size)) {
                return MEISO_LATEST_RECEIVE_DUPLICATE;
            }
            return MEISO_LATEST_RECEIVE_CONFLICT;
        }
        if (version < replica->version) {
            return MEISO_LATEST_RECEIVE_STALE;
        }
    }
    if (size > replica->capacity) {
        return MEISO_LATEST_RECEIVE_CAPACITY;
    }
    memmove(replica->storage, data, size);
    replica->size = size;
    replica->version = version;
    replica->has_value = true;
    return MEISO_LATEST_RECEIVE_NEW;
}

bool meiso_latest_replica_current(
    const MeisoLatestReplica *replica,
    MeisoLatestView *view
)
{
    if (!replica_valid(replica) || view == NULL || !replica->has_value) {
        return false;
    }
    view->version = replica->version;
    view->data = replica->storage;
    view->size = replica->size;
    return true;
}

void meiso_latest_replica_start_session(MeisoLatestReplica *replica)
{
    if (!replica_valid(replica)) {
        return;
    }
    replica->size = 0U;
    replica->version = 0U;
    replica->has_value = false;
}
