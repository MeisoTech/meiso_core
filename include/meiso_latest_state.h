#ifndef MEISO_LATEST_STATE_H
#define MEISO_LATEST_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t version;
    const uint8_t *data;
    size_t size;
} MeisoLatestView;

typedef enum {
    MEISO_LATEST_PUBLISH_QUEUED = 0,
    MEISO_LATEST_PUBLISH_REPLACED,
    MEISO_LATEST_PUBLISH_DUPLICATE,
    MEISO_LATEST_PUBLISH_STALE,
    MEISO_LATEST_PUBLISH_CONFLICT,
    MEISO_LATEST_PUBLISH_VERSION_WRAP,
    MEISO_LATEST_PUBLISH_CAPACITY,
    MEISO_LATEST_PUBLISH_INVALID
} MeisoLatestPublishStatus;

typedef enum {
    MEISO_LATEST_ACK_CONFIRMED = 0,
    MEISO_LATEST_ACK_DUPLICATE,
    MEISO_LATEST_ACK_STALE,
    MEISO_LATEST_ACK_FUTURE,
    MEISO_LATEST_ACK_INVALID
} MeisoLatestAckStatus;

typedef enum {
    MEISO_LATEST_RECEIVE_NEW = 0,
    MEISO_LATEST_RECEIVE_DUPLICATE,
    MEISO_LATEST_RECEIVE_STALE,
    MEISO_LATEST_RECEIVE_CONFLICT,
    MEISO_LATEST_RECEIVE_VERSION_WRAP,
    MEISO_LATEST_RECEIVE_CAPACITY,
    MEISO_LATEST_RECEIVE_INVALID
} MeisoLatestReceiveStatus;

typedef struct {
    uint8_t *storage;
    size_t capacity;
    size_t size;
    uint64_t version;
    bool has_value;
    bool pending;
} MeisoLatestPublisher;

typedef struct {
    uint8_t *storage;
    size_t capacity;
    size_t size;
    uint64_t version;
    bool has_value;
} MeisoLatestReplica;

bool meiso_latest_publisher_init(
    MeisoLatestPublisher *publisher,
    uint8_t *storage,
    size_t capacity
);
MeisoLatestPublishStatus meiso_latest_publisher_publish(
    MeisoLatestPublisher *publisher,
    uint64_t version,
    const void *data,
    size_t size
);
bool meiso_latest_publisher_pending(
    const MeisoLatestPublisher *publisher,
    MeisoLatestView *view
);
MeisoLatestAckStatus meiso_latest_publisher_ack(
    MeisoLatestPublisher *publisher,
    uint64_t version
);
void meiso_latest_publisher_start_session(MeisoLatestPublisher *publisher);

bool meiso_latest_replica_init(
    MeisoLatestReplica *replica,
    uint8_t *storage,
    size_t capacity
);
MeisoLatestReceiveStatus meiso_latest_replica_receive(
    MeisoLatestReplica *replica,
    uint64_t version,
    const void *data,
    size_t size
);
bool meiso_latest_replica_current(
    const MeisoLatestReplica *replica,
    MeisoLatestView *view
);
void meiso_latest_replica_start_session(MeisoLatestReplica *replica);

#ifdef __cplusplus
}
#endif

#endif
